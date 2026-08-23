// SPDX-License-Identifier: GPL-3.0-only
#include "input_hook.hpp"

#include "controller_state.hpp"
#include "input_mapping.hpp"
#include "present_hook.hpp"
#include "present_hook_logic.hpp"
#include "stereo_frame_broker.hpp"
#include "t4_layout_selector.hpp"
#include "tracking_anchor_sync.hpp"
#include "weapon_hook.hpp"

#include "t4/hook_api.hpp"
#include "t4/profile.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace wawvr::mod {

extern "C" void __cdecl wawvr_apply_controller_usercmd_from_bridge(
    wawvr::t4::UsercmdSp* command) noexcept;
extern "C" void __cdecl wawvr_apply_controller_usercmd_mp_from_bridge(
    wawvr::t4::UsercmdMp* command) noexcept;

namespace {

constexpr std::size_t kInlinePatchSize =
    kPostBuildUsercmdDisplacedInstruction.size();
constexpr std::size_t kTrampolineSize = kInlinePatchSize + 5;

std::atomic<bool> g_controller_input_installed{false};
std::atomic<bool> g_controller_input_enabled{false};
std::atomic<bool> g_logged_first_movement{false};
std::atomic<bool> g_logged_first_aim{false};
std::atomic<bool> g_logged_first_sprint_latch{false};
std::atomic<bool> g_logged_first_body_yaw_sync{false};
std::atomic<bool> g_logged_first_physical_melee{false};
std::atomic<bool> g_logged_first_right_stick{false};
std::uintptr_t g_gameplay_refdef_axis_address = 0;
std::uintptr_t g_client_view_yaw_address = 0;
std::uintptr_t g_key_catchers_address = 0;
std::uintptr_t g_connection_state_address = 0;
std::uintptr_t g_cgame_gun_pitch_address = 0;
std::uintptr_t g_cgame_gun_yaw_address = 0;
std::int32_t g_active_connection_state = kT4SpActiveConnectionState;
SnapTurnState g_snap_turn_state{};
SprintLatchState g_sprint_latch_state{};
PhysicalMeleeState g_physical_melee_state{};
bool g_snap_gate_diagnostic_latched = false;

// Read directly by the naked bridge. It is initialized before the target JMP
// is made visible while every pre-existing process thread is suspended.
void* g_input_trampoline = nullptr;
void* g_mp_original_create_cmd = nullptr;

struct ThreadRecord final {
    HANDLE handle{};
    DWORD id{};
    bool suspended{};
};

class SuspendedProcessThreads final {
public:
    SuspendedProcessThreads() = default;
    SuspendedProcessThreads(const SuspendedProcessThreads&) = delete;
    SuspendedProcessThreads& operator=(const SuspendedProcessThreads&) = delete;

    ~SuspendedProcessThreads() {
        for (std::size_t index = 0; index < thread_count_; ++index) {
            auto& thread = threads_[index];
            if (thread.suspended) {
                ResumeThread(thread.handle);
            }
            if (thread.handle != nullptr) {
                CloseHandle(thread.handle);
            }
        }
    }

    [[nodiscard]] bool open_all(
        InputHookInstallResult* const result) noexcept {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            fail(result, InputHookStatus::thread_snapshot_failed, GetLastError());
            return false;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (!Thread32First(snapshot, &entry)) {
            const DWORD error = GetLastError();
            CloseHandle(snapshot);
            fail(result, InputHookStatus::thread_snapshot_failed, error);
            return false;
        }

        const DWORD process_id = GetCurrentProcessId();
        const DWORD current_thread_id = GetCurrentThreadId();
        do {
            if (entry.th32OwnerProcessID == process_id &&
                entry.th32ThreadID != current_thread_id) {
                if (thread_count_ == threads_.size()) {
                    CloseHandle(snapshot);
                    fail(result, InputHookStatus::thread_snapshot_failed,
                         ERROR_INSUFFICIENT_BUFFER);
                    return false;
                }
                const HANDLE handle = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                    THREAD_QUERY_INFORMATION,
                    FALSE, entry.th32ThreadID);
                if (handle == nullptr) {
                    const DWORD error = GetLastError();
                    // Threads can disappear between the snapshot and
                    // OpenThread. A vanished thread cannot execute the site.
                    if (error != ERROR_INVALID_PARAMETER) {
                        CloseHandle(snapshot);
                        fail(result, InputHookStatus::thread_open_failed,
                             error);
                        return false;
                    }
                } else {
                    threads_[thread_count_++] = {
                        handle, entry.th32ThreadID, false};
                }
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
        return true;
    }

    [[nodiscard]] bool suspend_and_validate(
        const std::uintptr_t target,
        const std::size_t patch_size,
        InputHookInstallResult* const result) noexcept {
        for (std::size_t index = 0; index < thread_count_; ++index) {
            auto& thread = threads_[index];
            if (SuspendThread(thread.handle) == static_cast<DWORD>(-1)) {
                fail(result, InputHookStatus::thread_suspend_failed,
                     GetLastError());
                return false;
            }
            thread.suspended = true;

            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread.handle, &context)) {
                fail(result, InputHookStatus::thread_context_failed,
                     GetLastError());
                return false;
            }
#if defined(_M_IX86)
            const std::uintptr_t instruction = context.Eip;
#else
            const std::uintptr_t instruction = 0;
#endif
            if (instruction >= target && instruction < target + patch_size) {
                fail(result, InputHookStatus::target_thread_inside_patch, 0);
                return false;
            }
        }
        return true;
    }

private:
    static void fail(
        InputHookInstallResult* const result,
        const InputHookStatus status,
        const DWORD error) noexcept {
        if (result != nullptr) {
            result->status = status;
            result->system_error = error;
        }
    }

    std::array<ThreadRecord, 256> threads_{};
    std::size_t thread_count_{};
};

[[nodiscard]] bool input_disabled_by_environment() noexcept {
    std::array<wchar_t, 8> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"WAWVR_DISABLE_INPUT", value.data(),
        static_cast<DWORD>(value.size()));
    return length == 1 && value[0] == L'1';
}

[[nodiscard]] bool protection_is_writable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffU;
    return access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] bool protection_is_readable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffU;
    return access == PAGE_READONLY || access == PAGE_READWRITE ||
           access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] bool writable_range(
    const void* const address,
    const std::size_t size) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        !protection_is_writable(memory.Protect)) {
        return false;
    }
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t region_begin =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t region_end = region_begin + memory.RegionSize;
    return begin >= region_begin && begin <= region_end &&
           size <= region_end - begin;
}

[[nodiscard]] bool readable_range(
    const void* const address,
    const std::size_t size) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        !protection_is_readable(memory.Protect)) {
        return false;
    }
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t region_begin =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t region_end = region_begin + memory.RegionSize;
    return begin >= region_begin && begin <= region_end &&
           size <= region_end - begin;
}

[[nodiscard]] bool bytes_match(
    const std::uint8_t* const actual,
    const std::span<const std::uint8_t> expected) noexcept {
    if (actual == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool make_relative_instruction(
    const std::uintptr_t source,
    const std::uintptr_t destination,
    const std::uint8_t opcode,
    std::array<std::uint8_t, 5>* const jump) noexcept {
    if (jump == nullptr || source > std::numeric_limits<std::uint32_t>::max() ||
        destination > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const std::int64_t displacement =
        static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(source + 5);
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    (*jump)[0] = opcode;
    const auto encoded = static_cast<std::int32_t>(displacement);
    std::memcpy(jump->data() + 1, &encoded, sizeof(encoded));
    return true;
}

[[nodiscard]] bool rollback_original(
    std::uint8_t* const target,
    const DWORD old_protection,
    const std::span<const std::uint8_t> expected,
    InputHookInstallResult* const result) noexcept {
    if (expected.size() < kInlinePatchSize) {
        if (result != nullptr) {
            result->status = InputHookStatus::rollback_failed;
            result->system_error = ERROR_INVALID_DATA;
        }
        return false;
    }
    const auto original = expected.first(kInlinePatchSize);
    std::memcpy(target, original.data(), kInlinePatchSize);
    const bool bytes_restored = bytes_match(target, original);
    const bool cache_flushed =
        FlushInstructionCache(GetCurrentProcess(), target, kInlinePatchSize) !=
        FALSE;
    DWORD ignored = 0;
    const bool protection_restored =
        VirtualProtect(target, kInlinePatchSize, old_protection, &ignored) !=
        FALSE;
    if (!bytes_restored || !cache_flushed || !protection_restored) {
        if (result != nullptr) {
            result->status = InputHookStatus::rollback_failed;
            result->system_error = GetLastError();
        }
        return false;
    }
    return true;
}

[[nodiscard]] InputHookInstallResult patch_target(
    const wawvr::t4::PreparedInlineHook& prepared,
    const std::array<std::uint8_t, 5>& target_jump,
    const std::uintptr_t trampoline) noexcept {
    InputHookInstallResult result{};
    result.status = InputHookStatus::preparation_failed;
    result.target = prepared.target;
    result.trampoline = trampoline;

    SuspendedProcessThreads suspended;
    if (!suspended.open_all(&result) ||
        !suspended.suspend_and_validate(
            prepared.target, kInlinePatchSize, &result)) {
        return result;
    }

    auto* const target = reinterpret_cast<std::uint8_t*>(prepared.target);
    if (!bytes_match(target, prepared.expected_bytes())) {
        result.status = InputHookStatus::expected_bytes_changed;
        return result;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(
            target, kInlinePatchSize, PAGE_EXECUTE_READWRITE,
            &old_protection)) {
        result.status = InputHookStatus::target_protection_failed;
        result.system_error = GetLastError();
        return result;
    }

    if (!bytes_match(target, prepared.expected_bytes())) {
        DWORD ignored = 0;
        if (!VirtualProtect(
                target, kInlinePatchSize, old_protection, &ignored)) {
            result.status = InputHookStatus::protection_restore_failed;
            result.system_error = GetLastError();
        } else {
            result.status = InputHookStatus::expected_bytes_changed;
        }
        return result;
    }

    std::memcpy(target, target_jump.data(), target_jump.size());
    if (!bytes_match(target, target_jump)) {
        result.status = InputHookStatus::patch_write_failed;
        static_cast<void>(
            rollback_original(
                target, old_protection, prepared.expected_bytes(), &result));
        return result;
    }
    if (!FlushInstructionCache(
            GetCurrentProcess(), target, kInlinePatchSize)) {
        result.status = InputHookStatus::patch_cache_flush_failed;
        result.system_error = GetLastError();
        static_cast<void>(
            rollback_original(
                target, old_protection, prepared.expected_bytes(), &result));
        return result;
    }

    DWORD ignored = 0;
    if (!VirtualProtect(
            target, kInlinePatchSize, old_protection, &ignored)) {
        result.status = InputHookStatus::protection_restore_failed;
        result.system_error = GetLastError();
        static_cast<void>(
            rollback_original(
                target, old_protection, prepared.expected_bytes(), &result));
        return result;
    }

    result.status = InputHookStatus::installed;
    result.system_error = 0;
    return result;
}

#if defined(_MSC_VER) && defined(_M_IX86)
__declspec(naked) void input_detour_bridge() {
    __asm {
        // This is a mid-function contract, so preserve flags, every x86 GPR,
        // and the complete x87/MMX/SSE state around the ordinary C++ thunk.
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push eax
        call wawvr_apply_controller_usercmd_from_bridge
        add esp, 4
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [g_input_trampoline]
    }
}

__declspec(naked) void mp_create_cmd_detour_bridge() {
    __asm {
        // The replaced instruction is `call CL_CreateCmd`. Preserve its
        // usercall contract (destination in EAX, local client in ESI), then
        // service the completed 0x2C command returned in EAX before native MP
        // copies it into the command ring.
        call dword ptr [g_mp_original_create_cmd]
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push eax
        call wawvr_apply_controller_usercmd_mp_from_bridge
        add esp, 4
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    }
}
#endif

}  // namespace

template <typename Command>
void apply_controller_usercmd_from_bridge(Command* const command) noexcept {
    if (!g_controller_input_enabled.load(std::memory_order_acquire) ||
        !writable_range(command, sizeof(*command))) {
        return;
    }

    if constexpr (std::is_same_v<Command, wawvr::t4::UsercmdSp>) {
        // This runs after the native command has been completely serialized.
        // Clear target-assisted knife rotation/lunge independently of OpenXR
        // frame freshness so a stale/disconnected controller cannot leak one
        // legacy melee-charge command into the HMD camera. MP has no verified
        // equivalent fields and intentionally skips this write.
        suppress_t4_melee_charge(*command);
    }
    if (g_gameplay_refdef_axis_address == 0) {
        return;
    }

    ControllerFrameSnapshot snapshot{};
    if (!read_controller_frame(&snapshot)) {
        reset_sprint_latch(&g_sprint_latch_state);
        reset_physical_melee_gesture(&g_physical_melee_state);
        return;
    }
    const std::uint64_t now_milliseconds = GetTickCount64();
    const bool controller_frame_current =
        controller_frame_is_current(snapshot, now_milliseconds);

    wawvr::xr::Basis3f camera_axis{};
    std::memcpy(
        &camera_axis,
        reinterpret_cast<const void*>(g_gameplay_refdef_axis_address),
        sizeof(camera_axis));
    float snap_degrees = 0.0F;
    bool gameplay_controller_allowed = false;
    if (g_client_view_yaw_address != 0 && g_key_catchers_address != 0 &&
        g_connection_state_address != 0 &&
        writable_range(
            reinterpret_cast<void*>(g_client_view_yaw_address),
            sizeof(float)) &&
        readable_range(
            reinterpret_cast<const void*>(g_key_catchers_address),
            sizeof(std::uint32_t)) &&
        readable_range(
            reinterpret_cast<const void*>(g_connection_state_address),
            sizeof(std::int32_t))) {
        std::uint32_t key_catchers = 0;
        std::int32_t connection_state = 0;
        std::memcpy(
            &key_catchers,
            reinterpret_cast<const void*>(g_key_catchers_address),
            sizeof(key_catchers));
        std::memcpy(
            &connection_state,
            reinterpret_cast<const void*>(g_connection_state_address),
            sizeof(connection_state));
        gameplay_controller_allowed = controller_gameplay_input_allowed(
            key_catchers, connection_state, g_active_connection_state);

        if (gameplay_controller_allowed && controller_frame_current) {
            float body_yaw_delta = 0.0F;
            if (controller_body_yaw_delta_degrees(
                    snapshot, &body_yaw_delta) &&
                std::abs(body_yaw_delta) >=
                    kBodyYawSyncMinimumDegrees) {
                auto desired_anchor = leveled_tracking_anchor(
                    snapshot.frame.head_center,
                    &snapshot.tracking_anchor.orientation);
                // Physical body catch-up must not redefine the room-scale
                // origin. Transfer yaw only; manual recenter remains the sole
                // owner of tracking-anchor position.
                desired_anchor.position = snapshot.tracking_anchor.position;

                Command prospective_command = *command;
                wawvr::xr::Basis3f prospective_axis = camera_axis;
                auto* const live_yaw = reinterpret_cast<float*>(
                    g_client_view_yaw_address);
                float prospective_yaw = *live_yaw;
                TrackingAnchorSyncLock anchor_transaction(
                    tracking_anchor_sync_mutex());
                if (apply_snap_turn_to_t4_command(
                        prospective_command, body_yaw_delta,
                        &prospective_yaw, &prospective_axis) &&
                    try_rebase_pending_stereo_tracking_anchor(
                        snapshot.frame.frame_id,
                        snapshot.tracking_anchor.orientation,
                        desired_anchor.orientation)) {
                    if (rebase_controller_frame_tracking_anchor(
                            snapshot.generation,
                            snapshot.frame.frame_id,
                            snapshot.tracking_anchor.orientation,
                            desired_anchor.orientation)) {
                        const float yaw_before = *live_yaw;
                        *command = prospective_command;
                        *live_yaw = prospective_yaw;
                        camera_axis = prospective_axis;
                        snapshot.tracking_anchor = desired_anchor;
                        if (!g_logged_first_body_yaw_sync.exchange(
                                true, std::memory_order_acq_rel)) {
                            input_diagnostic_log(
                                "Physical HMD yaw transferred into T4 body without moving the visible view: delta=%.2f yaw=%.2f->%.2f",
                                body_yaw_delta, yaw_before,
                                prospective_yaw);
                        }
                    } else {
                        // The controller publication changed after it was
                        // sampled. Restore the exact stereo publication and
                        // leave native body/view state untouched.
                        static_cast<void>(
                            try_rebase_pending_stereo_tracking_anchor(
                                snapshot.frame.frame_id,
                                desired_anchor.orientation,
                                snapshot.tracking_anchor.orientation));
                    }
                }
            }
        }

        const auto& right_stick = snapshot.frame.actions.hands[
            static_cast<std::uint32_t>(wawvr::xr::Hand::Right)].stick;
        const bool right_stick_valid =
            controller_frame_current && right_stick.active &&
            std::isfinite(right_stick.current.x) &&
            std::isfinite(right_stick.current.y);
        const float right_x = right_stick_valid
                                  ? std::clamp(right_stick.current.x, -1.0F, 1.0F)
                                  : 0.0F;
        const float right_y = right_stick_valid
                                  ? std::clamp(right_stick.current.y, -1.0F, 1.0F)
                                  : 0.0F;
        const bool right_stick_deflected =
            right_stick_valid &&
            (std::abs(right_x) >= kControllerStickDeadzone ||
             std::abs(right_y) >= kControllerStickDeadzone);
        const bool horizontal_snap_intent =
            right_stick_valid &&
            std::abs(right_x) >= kSnapTurnEngageThreshold &&
            std::abs(right_x) >=
                std::abs(right_y) + kSnapTurnVerticalDominanceMargin;

        if (right_stick_deflected &&
            !g_logged_first_right_stick.exchange(
                true, std::memory_order_acq_rel)) {
            input_diagnostic_log(
                "Right-stick action live: x=%.2f y=%.2f connectionState=%d keyCatchers=0x%08X",
                right_x, right_y, connection_state, key_catchers);
        }
        if (!right_stick_valid ||
            std::abs(right_x) < kSnapTurnReleaseThreshold) {
            g_snap_gate_diagnostic_latched = false;
        }

        if (controller_frame_current &&
            snap_turn_gameplay_allowed(
                key_catchers, connection_state,
                g_active_connection_state)) {
            snap_degrees = consume_snap_turn_degrees(
                right_stick, &g_snap_turn_state);
            if (snap_degrees != 0.0F) {
                auto* const live_yaw = reinterpret_cast<float*>(
                    g_client_view_yaw_address);
                const float yaw_before = *live_yaw;
                if (!apply_snap_turn_to_t4_command(
                        *command, snap_degrees, live_yaw, &camera_axis)) {
                    input_diagnostic_log(
                        "Snap turn rejected by T4 yaw/camera validation: stick=(%.2f, %.2f) delta=%.1f",
                        right_x, right_y, snap_degrees);
                    snap_degrees = 0.0F;
                    g_snap_turn_state.armed = false;
                } else {
                    input_diagnostic_log(
                        "Snap turn applied: stick=(%.2f, %.2f) delta=%.1f yaw=%.1f->%.1f commandYaw=0x%04X",
                        right_x, right_y, snap_degrees, yaw_before, *live_yaw,
                        static_cast<unsigned int>(command->view_angles[1]) &
                            0xFFFFU);
                }
            }
        } else {
            // UI/focus/staleness suppression consumes the held direction.
            // A neutral stick must be observed after gameplay resumes before
            // another snap can fire.
            g_snap_turn_state.armed = false;
            if (horizontal_snap_intent &&
                !g_snap_gate_diagnostic_latched) {
                input_diagnostic_log(
                    "Snap turn gated: connectionState=%d (requires %d) keyCatchers=0x%08X blockedMask=0x%02X",
                    connection_state, g_active_connection_state, key_catchers,
                    kSnapTurnBlockedKeyCatcherMask);
                g_snap_gate_diagnostic_latched = true;
            }
        }
    } else {
        g_snap_turn_state.armed = false;
    }
    const auto& left_hand = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Left)];
    const bool sprint_latched = update_sprint_latch(
        gameplay_controller_allowed && controller_frame_current,
        snapshot.frame.actions.sequence,
        left_hand.stick,
        left_hand.stick_click,
        &g_sprint_latch_state);
    const bool physical_melee = update_physical_melee_gesture(
        gameplay_controller_allowed && controller_frame_current,
        snapshot, now_milliseconds, &g_physical_melee_state);
    ControllerInputResult applied{};
    if (gameplay_controller_allowed) {
        applied = apply_controller_input(
            *command, snapshot, camera_axis, now_milliseconds);
    }
    if (sprint_latched) {
        const bool native_or_physical_sprint_already_set =
            wawvr::t4::has_button(
                *command, wawvr::t4::UsercmdButton::sprint);
        wawvr::t4::add_button(
            *command, wawvr::t4::UsercmdButton::sprint);
        applied.gameplay_buttons_applied =
            applied.gameplay_buttons_applied ||
            !native_or_physical_sprint_already_set;
        if (!g_logged_first_sprint_latch.exchange(
                true, std::memory_order_acq_rel)) {
            OutputDebugStringA(
                "WorldAtWarVR: L3 click-to-sprint latch active until locomotion stick returns to neutral\n");
        }
    }
    if (physical_melee) {
        const bool melee_already_set = wawvr::t4::has_button(
            *command, wawvr::t4::UsercmdButton::melee);
        wawvr::t4::add_button(
            *command, wawvr::t4::UsercmdButton::melee);
        if constexpr (std::is_same_v<Command, wawvr::t4::UsercmdSp>) {
            suppress_t4_melee_charge(*command);
            applied.melee_comfort_applied = true;
        }
        applied.gameplay_buttons_applied =
            applied.gameplay_buttons_applied || !melee_already_set;
        if (!g_logged_first_physical_melee.exchange(
                true, std::memory_order_acq_rel)) {
            input_diagnostic_log(
                "Physical right-controller knife swing triggered native T4 melee");
        }
    }
    if (applied.weapon_aim_applied) {
        float final_pitch = 0.0F;
        float final_yaw = 0.0F;
        if (read_final_visible_weapon_aim(
                snapshot.generation, now_milliseconds,
                &final_pitch, &final_yaw)) {
            final_yaw += snap_degrees;
            applied.weapon_aim_applied = wawvr::t4::apply_vr_weapon_aim(
                *command, final_pitch, final_yaw,
                applied.weapon_trigger_applied);
            if (applied.weapon_aim_applied) {
                applied.weapon_pitch_degrees = final_pitch;
                applied.weapon_yaw_degrees = final_yaw;
            }
        }
    }

    if constexpr (std::is_same_v<Command, wawvr::t4::UsercmdMp>) {
        if (applied.weapon_aim_applied &&
            g_cgame_gun_pitch_address != 0 &&
            g_cgame_gun_yaw_address != 0 &&
            writable_range(
                reinterpret_cast<void*>(g_cgame_gun_pitch_address),
                sizeof(float)) &&
            writable_range(
                reinterpret_cast<void*>(g_cgame_gun_yaw_address),
                sizeof(float))) {
            std::memcpy(
                reinterpret_cast<void*>(g_cgame_gun_pitch_address),
                &applied.weapon_pitch_degrees, sizeof(float));
            std::memcpy(
                reinterpret_cast<void*>(g_cgame_gun_yaw_address),
                &applied.weapon_yaw_degrees, sizeof(float));
        }
    }

    if (applied.movement_applied &&
        !g_logged_first_movement.exchange(true, std::memory_order_acq_rel)) {
        OutputDebugStringA(
            "WorldAtWarVR: first focused HMD-oriented controller movement applied\n");
    }
    if (applied.weapon_aim_applied &&
        !g_logged_first_aim.exchange(true, std::memory_order_acq_rel)) {
        OutputDebugStringA(
            "WorldAtWarVR: first tracked right-controller gun aim applied to T4 usercmd\n");
    }
}

extern "C" void __cdecl wawvr_apply_controller_usercmd_from_bridge(
    wawvr::t4::UsercmdSp* const command) noexcept {
    apply_controller_usercmd_from_bridge(command);
}

extern "C" void __cdecl wawvr_apply_controller_usercmd_mp_from_bridge(
    wawvr::t4::UsercmdMp* const command) noexcept {
    apply_controller_usercmd_from_bridge(command);
}

InputHookInstallResult install_controller_input_hook(
    const wawvr::t4::ValidatedBindings& bindings) noexcept {
    InputHookInstallResult result{};
    if (g_controller_input_installed.load(std::memory_order_acquire)) {
        result.status = InputHookStatus::already_installed;
        return result;
    }
    if (input_disabled_by_environment()) {
        result.status = InputHookStatus::disabled_by_environment;
        return result;
    }

#if !defined(_MSC_VER) || !defined(_M_IX86)
    static_cast<void>(bindings);
    result.status = InputHookStatus::unsupported_compiler_or_architecture;
    return result;
#else
    const auto& bound = bindings.profile();
    const auto layout = select_t4_layout_family(bound);
    if (layout == T4LayoutFamily::unsupported) {
        result.status = InputHookStatus::rejected_wrong_profile;
        return result;
    }
    const bool multiplayer =
        layout == T4LayoutFamily::multiplayer_1_7_1263;

    const auto* const refdef = bindings.data_symbol(
        wawvr::t4::DataSymbolId::gameplay_refdef_viewport);
    const auto camera_axis = refdef == nullptr
        ? std::optional<std::uintptr_t>{}
        : bindings.module().address(
              static_cast<wawvr::t4::Rva>(
                  refdef->rva + kGameplayRefdefAxisOffset),
              sizeof(wawvr::xr::Basis3f));
    if (!camera_axis.has_value()) {
        result.status = InputHookStatus::camera_axis_out_of_range;
        return result;
    }
    const auto client_view_yaw = bindings.data_address(
        wawvr::t4::DataSymbolId::client_view_yaw_degrees, sizeof(float));
    const auto key_catchers = bindings.data_address(
        wawvr::t4::DataSymbolId::key_catchers, sizeof(std::uint32_t));
    const auto connection_state = bindings.data_address(
        wawvr::t4::DataSymbolId::connection_state, sizeof(std::int32_t));
    if (!client_view_yaw.has_value() || !key_catchers.has_value() ||
        !connection_state.has_value()) {
        result.status = InputHookStatus::snap_turn_data_out_of_range;
        return result;
    }

    std::optional<std::uintptr_t> cgame_gun_pitch{};
    std::optional<std::uintptr_t> cgame_gun_yaw{};
    if (multiplayer) {
        cgame_gun_pitch = bindings.data_address(
            wawvr::t4::DataSymbolId::cgame_gun_pitch_degrees,
            sizeof(float));
        cgame_gun_yaw = bindings.data_address(
            wawvr::t4::DataSymbolId::cgame_gun_yaw_degrees,
            sizeof(float));
        if (!cgame_gun_pitch.has_value() || !cgame_gun_yaw.has_value()) {
            result.status = InputHookStatus::snap_turn_data_out_of_range;
            return result;
        }
    }

    const auto prepared = wawvr::t4::prepare_inline_hook(
        bindings, wawvr::t4::HookSiteId::post_build_usercmd,
        multiplayer
            ? reinterpret_cast<std::uintptr_t>(
                  &mp_create_cmd_detour_bridge)
            : reinterpret_cast<std::uintptr_t>(&input_detour_bridge));
    if (!prepared.ok()) {
        result.status = InputHookStatus::preparation_failed;
        return result;
    }
    result.target = prepared.hook->target;
    const bool sp_boundary =
        !multiplayer &&
        std::equal(
            kPostBuildUsercmdDisplacedInstruction.begin(),
            kPostBuildUsercmdDisplacedInstruction.end(),
            prepared.hook->expected.begin());
    const bool mp_boundary =
        multiplayer && prepared.hook->expected[0] == 0xE8;
    if (prepared.hook->minimum_patch_bytes != kInlinePatchSize ||
        prepared.hook->expected_size < kInlinePatchSize ||
        (!sp_boundary && !mp_boundary)) {
        result.status = InputHookStatus::unexpected_instruction_boundary;
        return result;
    }

    g_gameplay_refdef_axis_address = *camera_axis;
    g_client_view_yaw_address = *client_view_yaw;
    g_key_catchers_address = *key_catchers;
    g_connection_state_address = *connection_state;
    g_cgame_gun_pitch_address = cgame_gun_pitch.value_or(0);
    g_cgame_gun_yaw_address = cgame_gun_yaw.value_or(0);
    g_active_connection_state = multiplayer
        ? kT4MpActiveConnectionState
        : kT4SpActiveConnectionState;
    reset_sprint_latch(&g_sprint_latch_state);
    reset_physical_melee_gesture(&g_physical_melee_state);
    g_logged_first_sprint_latch.store(false, std::memory_order_release);
    g_logged_first_body_yaw_sync.store(false, std::memory_order_release);
    g_logged_first_physical_melee.store(false, std::memory_order_release);

    const auto clear_bound_state = []() noexcept {
        g_input_trampoline = nullptr;
        g_mp_original_create_cmd = nullptr;
        g_gameplay_refdef_axis_address = 0;
        g_client_view_yaw_address = 0;
        g_key_catchers_address = 0;
        g_connection_state_address = 0;
        g_cgame_gun_pitch_address = 0;
        g_cgame_gun_yaw_address = 0;
        g_active_connection_state = kT4SpActiveConnectionState;
        reset_sprint_latch(&g_sprint_latch_state);
        reset_physical_melee_gesture(&g_physical_melee_state);
    };

    if (multiplayer) {
        std::int32_t original_displacement = 0;
        std::memcpy(
            &original_displacement, prepared.hook->expected.data() + 1,
            sizeof(original_displacement));
        const std::int64_t original_value =
            static_cast<std::int64_t>(prepared.hook->target + 5) +
            original_displacement;
        const auto module_begin = reinterpret_cast<std::uintptr_t>(
            bindings.module().base);
        const auto module_size = bindings.module().size;
        if (original_value < 0 ||
            static_cast<std::uint64_t>(original_value) < module_begin ||
            static_cast<std::uint64_t>(original_value) - module_begin >=
                module_size) {
            clear_bound_state();
            result.status = InputHookStatus::unexpected_instruction_boundary;
            return result;
        }
        const auto original = static_cast<std::uintptr_t>(original_value);
        std::array<std::uint8_t, 5> target_call{};
        if (!make_relative_instruction(
                prepared.hook->target,
                reinterpret_cast<std::uintptr_t>(
                    &mp_create_cmd_detour_bridge),
                0xE8, &target_call)) {
            clear_bound_state();
            result.status = InputHookStatus::jump_out_of_range;
            return result;
        }
        g_mp_original_create_cmd = reinterpret_cast<void*>(original);
        result = patch_target(*prepared.hook, target_call, original);
        if (!result.ok()) {
            clear_bound_state();
            return result;
        }
    } else {
        auto* const trampoline = static_cast<std::uint8_t*>(VirtualAlloc(
            nullptr, kTrampolineSize, MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE));
        if (trampoline == nullptr) {
            clear_bound_state();
            result.status = InputHookStatus::trampoline_allocation_failed;
            result.system_error = GetLastError();
            return result;
        }
        std::memcpy(
            trampoline, kPostBuildUsercmdDisplacedInstruction.data(),
            kInlinePatchSize);

        std::array<std::uint8_t, 5> return_jump{};
        std::array<std::uint8_t, 5> target_jump{};
        if (!make_relative_instruction(
                reinterpret_cast<std::uintptr_t>(trampoline) +
                    kInlinePatchSize,
                prepared.hook->target + kInlinePatchSize, 0xE9,
                &return_jump) ||
            !make_relative_instruction(
                prepared.hook->target,
                reinterpret_cast<std::uintptr_t>(&input_detour_bridge),
                0xE9, &target_jump)) {
            clear_bound_state();
            VirtualFree(trampoline, 0, MEM_RELEASE);
            result.status = InputHookStatus::jump_out_of_range;
            return result;
        }
        std::memcpy(
            trampoline + kInlinePatchSize, return_jump.data(),
            return_jump.size());

        DWORD old_trampoline_protection = 0;
        if (!VirtualProtect(
                trampoline, kTrampolineSize, PAGE_EXECUTE_READ,
                &old_trampoline_protection)) {
            clear_bound_state();
            result.status = InputHookStatus::trampoline_protection_failed;
            result.system_error = GetLastError();
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return result;
        }
        if (!FlushInstructionCache(
                GetCurrentProcess(), trampoline, kTrampolineSize)) {
            clear_bound_state();
            result.status = InputHookStatus::trampoline_cache_flush_failed;
            result.system_error = GetLastError();
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return result;
        }

        g_input_trampoline = trampoline;
        result = patch_target(
            *prepared.hook, target_jump,
            reinterpret_cast<std::uintptr_t>(trampoline));
        if (!result.ok()) {
            clear_bound_state();
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return result;
        }
    }

    if (!result.ok()) {
        clear_bound_state();
        return result;
    }

    g_controller_input_installed.store(true, std::memory_order_release);
    g_controller_input_enabled.store(true, std::memory_order_release);
    return result;
#endif
}

const char* input_hook_status_name(const InputHookStatus status) noexcept {
    switch (status) {
    case InputHookStatus::installed: return "installed";
    case InputHookStatus::already_installed: return "already-installed";
    case InputHookStatus::disabled_by_environment: return "disabled-by-environment";
    case InputHookStatus::rejected_wrong_profile: return "rejected-wrong-profile";
    case InputHookStatus::unsupported_compiler_or_architecture: return "unsupported-compiler-or-architecture";
    case InputHookStatus::camera_axis_out_of_range: return "camera-axis-out-of-range";
    case InputHookStatus::snap_turn_data_out_of_range: return "snap-turn-data-out-of-range";
    case InputHookStatus::preparation_failed: return "preparation-failed";
    case InputHookStatus::unexpected_instruction_boundary: return "unexpected-instruction-boundary";
    case InputHookStatus::trampoline_allocation_failed: return "trampoline-allocation-failed";
    case InputHookStatus::trampoline_protection_failed: return "trampoline-protection-failed";
    case InputHookStatus::trampoline_cache_flush_failed: return "trampoline-cache-flush-failed";
    case InputHookStatus::jump_out_of_range: return "jump-out-of-range";
    case InputHookStatus::thread_snapshot_failed: return "thread-snapshot-failed";
    case InputHookStatus::thread_open_failed: return "thread-open-failed";
    case InputHookStatus::thread_suspend_failed: return "thread-suspend-failed";
    case InputHookStatus::thread_context_failed: return "thread-context-failed";
    case InputHookStatus::target_thread_inside_patch: return "target-thread-inside-patch";
    case InputHookStatus::expected_bytes_changed: return "expected-bytes-changed";
    case InputHookStatus::target_protection_failed: return "target-protection-failed";
    case InputHookStatus::patch_write_failed: return "patch-write-failed";
    case InputHookStatus::patch_cache_flush_failed: return "patch-cache-flush-failed";
    case InputHookStatus::protection_restore_failed: return "protection-restore-failed";
    case InputHookStatus::rollback_failed: return "rollback-failed";
    }
    return "unknown";
}

void request_controller_input_shutdown() noexcept {
    g_controller_input_enabled.store(false, std::memory_order_release);
    reset_sprint_latch(&g_sprint_latch_state);
    reset_physical_melee_gesture(&g_physical_melee_state);
}

bool controller_input_hook_installed() noexcept {
    return g_controller_input_installed.load(std::memory_order_acquire);
}

bool controller_input_hook_enabled() noexcept {
    return g_controller_input_enabled.load(std::memory_order_acquire);
}

}  // namespace wawvr::mod
