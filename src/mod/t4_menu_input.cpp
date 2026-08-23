// SPDX-License-Identifier: GPL-3.0-only
#include "t4_menu_input.hpp"

#include "input_mapping.hpp"
#include "menu_panel_logic.hpp"
#include "menu_surface_logic.hpp"
#include "native_menu_mouse_gate_logic.hpp"
#include "peer_thread_quiescence.hpp"
#include "stereo_diagnostics.hpp"
#include "t4_presentation_state.hpp"

#include "t4/hook_api.hpp"
#include "t4/profile.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace wawvr::mod {

// Published before the native mouse call is patched and deliberately retained
// for the process lifetime. An in-flight bridge can therefore always finish
// its tail jump even while shutdown restores the original callsite.
extern "C" std::uintptr_t wawvr_t4_original_ui_mouse_event_address = 0;

#if defined(_MSC_VER) && defined(_M_IX86)
extern "C" void wawvr_t4_native_menu_mouse_gate_bridge() noexcept;
#endif

#if defined(_MSC_VER) && defined(_M_IX86)
// Exact T4 Cbuf_AddText contract: EAX=text, ECX=local command-buffer index.
// The target uses a plain ret and preserves the x86 callee-saved registers.
extern "C" __declspec(naked) void __cdecl wawvr_call_t4_cbuf_add_text(
    std::uintptr_t, const char*) noexcept {
    __asm {
        mov edx, dword ptr [esp + 4]
        mov eax, dword ptr [esp + 8]
        xor ecx, ecx
        call edx
        ret
    }
}
#endif

namespace {

using ClKeyEventFunction = void(__cdecl*)(
    std::int32_t local_client_num,
    std::int32_t key,
    std::int32_t down,
    std::uint32_t time);
using UiMouseEventFunction = void(__cdecl*)(
    std::int32_t x,
    std::int32_t y);

std::atomic<std::uintptr_t> g_cl_key_event_address{0};
std::atomic<std::uintptr_t> g_ui_mouse_event_address{0};
std::atomic<std::uintptr_t> g_cbuf_add_text_address{0};
std::atomic<bool> g_native_mouse_gate_installed{false};
std::uintptr_t g_native_mouse_gate_callsite = 0;
constexpr std::size_t kNativeMouseGateCallSize = 5;
// Release x86 disassembly is exactly 17 bytes through the terminal ret.
constexpr std::size_t kNativeMouseGateBridgeExecutionGuardSize = 17;
std::array<std::uint8_t, kNativeMouseGateCallSize>
    g_native_mouse_gate_original_call{};
std::array<std::uint8_t, kNativeMouseGateCallSize>
    g_native_mouse_gate_replacement_call{};
std::atomic<bool> g_single_player_profile{false};
std::atomic<bool> g_multiplayer_profile{false};
std::atomic<bool> g_pezbot_autofill_enabled{false};
PezBotAutofillState g_pezbot_autofill_state{};

constexpr char kWeaponNextCommand[] = "weapnext\n";
constexpr char kPezBotAutofillCommand[] = "set svr_pezbots 9\n";
constexpr wchar_t kPezBotAutofillLaunchMarker[] =
    L"+set wawvr_pezbot_autofill 9";

[[nodiscard]] bool executable_address(
    const std::uintptr_t address) noexcept {
    if (address == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address), &memory,
            sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD access = memory.Protect & 0xffU;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
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

[[nodiscard]] bool readable_range(
    const void* const address, const std::size_t size) noexcept {
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
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region_begin =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto region_end = region_begin + memory.RegionSize;
    return begin >= region_begin && begin <= region_end &&
           size <= region_end - begin;
}

[[nodiscard]] bool bytes_match(
    const void* const address,
    const std::span<const std::uint8_t> expected) noexcept {
    return readable_range(address, expected.size()) &&
           std::memcmp(address, expected.data(), expected.size()) == 0;
}

[[nodiscard]] bool make_relative_call(
    const std::uintptr_t source, const std::uintptr_t destination,
    std::array<std::uint8_t, kNativeMouseGateCallSize>* const output) noexcept {
    if (output == nullptr) {
        return false;
    }
    const std::int64_t displacement =
        static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(source + kNativeMouseGateCallSize);
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    (*output)[0] = 0xE8;
    const auto encoded = static_cast<std::int32_t>(displacement);
    std::memcpy(output->data() + 1, &encoded, sizeof(encoded));
    return true;
}

[[nodiscard]] std::uintptr_t decode_relative_call_target(
    const std::uintptr_t source,
    const std::array<std::uint8_t, kNativeMouseGateCallSize>& bytes) noexcept {
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    return static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(source + kNativeMouseGateCallSize) +
        displacement);
}

enum class NativeMouseGatePatchResult : std::uint8_t {
    ok,
    thread_suspend_failed,
    expected_bytes_changed,
    target_protection_failed,
    patch_write_failed,
    patch_cache_flush_failed,
    protection_restore_failed,
};

[[nodiscard]] NativeMouseGatePatchResult replace_native_mouse_gate_call(
    const std::uintptr_t target_address,
    const std::array<std::uint8_t, kNativeMouseGateCallSize>& expected,
    const std::array<std::uint8_t, kNativeMouseGateCallSize>& replacement,
    DWORD* const system_error) noexcept {
#if !defined(_MSC_VER) || !defined(_M_IX86)
    static_cast<void>(target_address);
    static_cast<void>(expected);
    static_cast<void>(replacement);
    static_cast<void>(system_error);
    return NativeMouseGatePatchResult::patch_write_failed;
#else
    const auto bridge_address = reinterpret_cast<std::uintptr_t>(
        &wawvr_t4_native_menu_mouse_gate_bridge);
    const std::array<PeerThreadPatchRange, 2> patch_ranges{{
        {target_address, kNativeMouseGateCallSize},
        {bridge_address, kNativeMouseGateBridgeExecutionGuardSize},
    }};
    SuspendedPeerThreads suspended;
    PeerThreadQuiesceResult quiesce{};
    if (!suspended.suspend(patch_ranges, &quiesce)) {
        if (system_error != nullptr) {
            *system_error = quiesce.system_error;
        }
        return NativeMouseGatePatchResult::thread_suspend_failed;
    }

    auto* const target = reinterpret_cast<std::uint8_t*>(target_address);
    if (!bytes_match(target, expected)) {
        return NativeMouseGatePatchResult::expected_bytes_changed;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(
            target, replacement.size(), PAGE_EXECUTE_READWRITE,
            &old_protection)) {
        if (system_error != nullptr) {
            *system_error = GetLastError();
        }
        return NativeMouseGatePatchResult::target_protection_failed;
    }
    if (std::memcmp(target, expected.data(), expected.size()) != 0) {
        DWORD ignored = 0;
        VirtualProtect(target, replacement.size(), old_protection, &ignored);
        return NativeMouseGatePatchResult::expected_bytes_changed;
    }

    std::memcpy(target, replacement.data(), replacement.size());
    if (std::memcmp(target, replacement.data(), replacement.size()) != 0) {
        std::memcpy(target, expected.data(), expected.size());
        FlushInstructionCache(GetCurrentProcess(), target, expected.size());
        DWORD ignored = 0;
        VirtualProtect(target, replacement.size(), old_protection, &ignored);
        return NativeMouseGatePatchResult::patch_write_failed;
    }
    if (!FlushInstructionCache(
            GetCurrentProcess(), target, replacement.size())) {
        if (system_error != nullptr) {
            *system_error = GetLastError();
        }
        std::memcpy(target, expected.data(), expected.size());
        FlushInstructionCache(GetCurrentProcess(), target, expected.size());
        DWORD ignored = 0;
        VirtualProtect(target, replacement.size(), old_protection, &ignored);
        return NativeMouseGatePatchResult::patch_cache_flush_failed;
    }

    DWORD ignored = 0;
    if (!VirtualProtect(
            target, replacement.size(), old_protection, &ignored)) {
        if (system_error != nullptr) {
            *system_error = GetLastError();
        }
        std::memcpy(target, expected.data(), expected.size());
        FlushInstructionCache(GetCurrentProcess(), target, expected.size());
        VirtualProtect(target, replacement.size(), old_protection, &ignored);
        return NativeMouseGatePatchResult::protection_restore_failed;
    }
    return NativeMouseGatePatchResult::ok;
#endif
}

[[nodiscard]] bool bool_held(
    const wawvr::xr::BoolActionState& action) noexcept {
    return action.active && action.current;
}

void send_key_tap(
    const ClKeyEventFunction key_event,
    const std::int32_t key,
    const std::uint64_t now_milliseconds) noexcept {
    if (key_event == nullptr) {
        return;
    }
    const auto event_time = static_cast<std::uint32_t>(now_milliseconds);
    key_event(0, key, 1, event_time);
    key_event(0, key, 0, event_time);
}

[[nodiscard]] bool queue_console_command(
    const std::uintptr_t cbuf_add_text,
    const char* const command) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    if (cbuf_add_text != 0 && command != nullptr) {
        wawvr_call_t4_cbuf_add_text(cbuf_add_text, command);
        return true;
    }
#else
    static_cast<void>(cbuf_add_text);
    static_cast<void>(command);
#endif
    return false;
}

[[nodiscard]] bool launch_requests_pezbot_autofill() noexcept {
    const wchar_t* const command_line = GetCommandLineW();
    return command_line != nullptr &&
        command_line_has_exact_marker(
            command_line, kPezBotAutofillLaunchMarker);
}

[[nodiscard]] MenuCursorRegion cursor_region(
    const std::uint32_t full_width,
    const std::uint32_t full_height,
    const bool active_gameplay_menu,
    const ActiveUiMonoSource source) noexcept {
    MenuCursorRegion region{
        .origin_x = 0,
        .width = full_width,
        .height = full_height,
    };
    if (!active_gameplay_menu || source == ActiveUiMonoSource::full_frame ||
        full_width < 2U) {
        return region;
    }
    region.width = full_width / 2U;
    if (source == ActiveUiMonoSource::right_eye) {
        region.origin_x = full_width / 2U;
    }
    return region;
}

[[nodiscard]] bool install_native_menu_mouse_gate(
    const wawvr::t4::ValidatedBindings& bindings,
    const std::uintptr_t expected_ui_mouse_event) noexcept {
#if !defined(_MSC_VER) || !defined(_M_IX86)
    static_cast<void>(bindings);
    static_cast<void>(expected_ui_mouse_event);
    return false;
#else
    if (g_native_mouse_gate_installed.load(std::memory_order_acquire)) {
        return true;
    }
    const auto prepared = wawvr::t4::prepare_inline_hook(
        bindings, wawvr::t4::HookSiteId::ui_mouse_event_native_call,
        reinterpret_cast<std::uintptr_t>(
            &wawvr_t4_native_menu_mouse_gate_bridge));
    if (!prepared.ok() ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::ui_mouse_event_call_context_sentinel) ||
        prepared.hook->expected_size != kNativeMouseGateCallSize ||
        prepared.hook->minimum_patch_bytes != kNativeMouseGateCallSize) {
        return false;
    }

    std::array<std::uint8_t, kNativeMouseGateCallSize> original_call{};
    std::copy_n(
        prepared.hook->expected.begin(), original_call.size(),
        original_call.begin());
    if (original_call[0] != 0xE8 ||
        decode_relative_call_target(prepared.hook->target, original_call) !=
            expected_ui_mouse_event) {
        return false;
    }

    std::array<std::uint8_t, kNativeMouseGateCallSize> replacement_call{};
    if (!make_relative_call(
            prepared.hook->target,
            reinterpret_cast<std::uintptr_t>(
                &wawvr_t4_native_menu_mouse_gate_bridge),
            &replacement_call)) {
        return false;
    }

    // This callsite may execute before the Present-hook monitor gets a chance
    // to pin the module. Reject installation unless the bridge and its stable
    // original target can be guaranteed to remain process-lifetime valid.
    HMODULE pinned_module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(
                &wawvr_t4_native_menu_mouse_gate_bridge),
            &pinned_module)) {
        return false;
    }
    if (wawvr_t4_original_ui_mouse_event_address != 0 &&
        wawvr_t4_original_ui_mouse_event_address != expected_ui_mouse_event) {
        return false;
    }
    wawvr_t4_original_ui_mouse_event_address = expected_ui_mouse_event;

    DWORD system_error = 0;
    const auto patch = replace_native_mouse_gate_call(
        prepared.hook->target, original_call, replacement_call,
        &system_error);
    if (patch != NativeMouseGatePatchResult::ok) {
        return false;
    }

    g_native_mouse_gate_callsite = prepared.hook->target;
    g_native_mouse_gate_original_call = original_call;
    g_native_mouse_gate_replacement_call = replacement_call;
    g_native_mouse_gate_installed.store(true, std::memory_order_release);
    return true;
#endif
}

void restore_native_menu_mouse_gate() noexcept {
    if (!g_native_mouse_gate_installed.load(std::memory_order_acquire)) {
        return;
    }
    DWORD system_error = 0;
    const auto patch = replace_native_mouse_gate_call(
        g_native_mouse_gate_callsite, g_native_mouse_gate_replacement_call,
        g_native_mouse_gate_original_call, &system_error);
    if (patch == NativeMouseGatePatchResult::ok ||
        patch == NativeMouseGatePatchResult::expected_bytes_changed) {
        // On an ownership mismatch, preserve the foreign bytes and stop
        // claiming the callsite. The immutable original target remains valid
        // for any bridge invocation that was already in flight.
        g_native_mouse_gate_installed.store(false, std::memory_order_release);
        if (patch == NativeMouseGatePatchResult::ok) {
            g_native_mouse_gate_callsite = 0;
            g_native_mouse_gate_original_call = {};
            g_native_mouse_gate_replacement_call = {};
        }
    }
}

}  // namespace

#if defined(_MSC_VER) && defined(_M_IX86)
extern "C" __declspec(naked) void
wawvr_t4_native_menu_mouse_gate_bridge() noexcept {
    __asm {
        // x/y are the bridge's two cdecl arguments. The caller's dx/dy remain
        // at +14h/+18h after x, y, saved ESI, and the CL caller return address.
        mov eax, dword ptr [esp + 14h]
        or eax, dword ptr [esp + 18h]
        jz stationary_mouse
        jmp dword ptr [wawvr_t4_original_ui_mouse_event_address]
    stationary_mouse:
        ret
    }
}
#endif

bool bind_t4_menu_input(
    const wawvr::t4::ValidatedBindings& bindings) noexcept {
    clear_t4_menu_input();
#if !defined(_MSC_VER) || !defined(_M_IX86)
    static_cast<void>(bindings);
    return false;
#else
    constexpr wawvr::t4::HookSiteId required_ui_sites[] = {
        wawvr::t4::HookSiteId::cl_key_event_entry_sentinel,
        wawvr::t4::HookSiteId::cl_key_event_dispatch_context_sentinel,
        wawvr::t4::HookSiteId::ui_mouse_event_entry_sentinel,
        wawvr::t4::HookSiteId::ui_mouse_event_native_call,
        wawvr::t4::HookSiteId::ui_mouse_event_call_context_sentinel,
    };
    for (const auto site : required_ui_sites) {
        if (!bindings.site_bytes_still_match(site)) {
            return false;
        }
    }
    const auto cl_key_event = bindings.site_address(
        wawvr::t4::HookSiteId::cl_key_event_entry_sentinel);
    const auto ui_mouse_event = bindings.site_address(
        wawvr::t4::HookSiteId::ui_mouse_event_entry_sentinel);
    if (!cl_key_event.has_value() || !ui_mouse_event.has_value() ||
        !executable_address(*cl_key_event) ||
        !executable_address(*ui_mouse_event)) {
        return false;
    }
    if (g_native_mouse_gate_installed.load(std::memory_order_acquire) ||
        !install_native_menu_mouse_gate(bindings, *ui_mouse_event)) {
        return false;
    }
    g_ui_mouse_event_address.store(
        *ui_mouse_event, std::memory_order_release);
    g_cl_key_event_address.store(*cl_key_event, std::memory_order_release);
    const auto executable_variant = bindings.profile().variant;
    g_single_player_profile.store(
        executable_variant == wawvr::t4::ExecutableVariant::single_player,
        std::memory_order_release);
    g_multiplayer_profile.store(
        executable_variant == wawvr::t4::ExecutableVariant::multiplayer,
        std::memory_order_release);

    // Mouse/key injection is the mandatory menu path. Weapon cycling is a
    // separate optional convenience built on Cbuf_AddText and its command
    // identity; an executable variant that lacks those extra sentinels must
    // not lose native menu interaction.
    constexpr wawvr::t4::HookSiteId optional_command_sites[] = {
        wawvr::t4::HookSiteId::cbuf_add_text_entry_sentinel,
        wawvr::t4::HookSiteId::winmain_cbuf_add_text_call_context_sentinel,
        wawvr::t4::HookSiteId::weapnext_command_identity_sentinel,
        wawvr::t4::HookSiteId::weapnext_command_registration_sentinel,
        wawvr::t4::HookSiteId::weapnext_handler_entry_sentinel,
    };
    bool command_sites_match = true;
    for (const auto site : optional_command_sites) {
        command_sites_match = command_sites_match &&
            bindings.site_bytes_still_match(site);
    }
    if (command_sites_match) {
        const auto cbuf_add_text = bindings.site_address(
            wawvr::t4::HookSiteId::cbuf_add_text_entry_sentinel);
        if (cbuf_add_text.has_value() &&
            executable_address(*cbuf_add_text)) {
            g_cbuf_add_text_address.store(
                *cbuf_add_text, std::memory_order_release);
            g_pezbot_autofill_enabled.store(
                bindings.profile().variant ==
                        wawvr::t4::ExecutableVariant::multiplayer &&
                    launch_requests_pezbot_autofill(),
                std::memory_order_release);
        }
    }
    return true;
#endif
}

void clear_t4_menu_input() noexcept {
    restore_native_menu_mouse_gate();
    g_cl_key_event_address.store(0, std::memory_order_release);
    g_ui_mouse_event_address.store(0, std::memory_order_release);
    g_cbuf_add_text_address.store(0, std::memory_order_release);
    g_single_player_profile.store(false, std::memory_order_release);
    g_multiplayer_profile.store(false, std::memory_order_release);
    g_pezbot_autofill_enabled.store(false, std::memory_order_release);
    g_pezbot_autofill_state = {};
}

T4MenuInputServiceResult service_t4_menu_input_after_com_frame(
    const wawvr::xr::FrameState& frame,
    std::uint32_t full_backbuffer_width,
    std::uint32_t full_backbuffer_height,
    const ActiveUiMonoSource active_ui_source,
    const MenuPointerSurface* const visible_menu_surface,
    const bool menu_escape_tap_requested,
    const std::uint64_t now_milliseconds,
    T4MenuInputState* const state) noexcept {
    T4MenuInputServiceResult result{};
    if (state == nullptr) {
        return result;
    }
    const auto key_event = reinterpret_cast<ClKeyEventFunction>(
        g_cl_key_event_address.load(std::memory_order_acquire));
    const auto mouse_event = reinterpret_cast<UiMouseEventFunction>(
        g_ui_mouse_event_address.load(std::memory_order_acquire));
    const std::uintptr_t cbuf_add_text =
        g_cbuf_add_text_address.load(std::memory_order_acquire);
    if (key_event == nullptr || mouse_event == nullptr) {
        *state = {};
        return result;
    }

    T4PresentationState presentation = read_t4_presentation_state();
    result.state_valid = presentation.valid;
    if (!presentation.valid) {
        *state = {};
        return result;
    }

    if (menu_escape_tap_requested) {
        send_key_tap(key_event, kT4KeyEscape, now_milliseconds);
        result.menu_button_tapped = true;
        // Escape may synchronously open or close a native menu. Re-read the
        // exact state before deciding whether cursor/A/B own this frame.
        presentation = read_t4_presentation_state();
        result.state_valid = presentation.valid;
        if (!presentation.valid) {
            *state = {};
            return result;
        }
    }

    result.ui_active =
        (presentation.key_catchers & kT4UiKeyCatcher) != 0;
    const bool active_gameplay_menu =
        presentation.connection_state ==
        presentation.active_connection_state;
    const bool multiplayer_profile =
        g_multiplayer_profile.load(std::memory_order_acquire);
    const bool pezbot_autofill_requested =
        should_queue_pezbot_autofill(
            {
                .enabled = g_pezbot_autofill_enabled.load(
                    std::memory_order_acquire),
                .multiplayer_profile = multiplayer_profile,
                .presentation_state_valid = presentation.valid,
                .connection_state = presentation.connection_state,
                .active_connection_state =
                    presentation.active_connection_state,
            },
            &g_pezbot_autofill_state);
    if (pezbot_autofill_requested) {
        if (queue_console_command(
                cbuf_add_text, kPezBotAutofillCommand)) {
            result.pezbot_autofill_queued = true;
        } else {
            // The exact x86 binding normally makes this impossible. If it is
            // ever unavailable, retry on the next frontend frame instead of
            // silently consuming the one-shot lifecycle edge.
            g_pezbot_autofill_state.disconnected_frontend_latched = false;
        }
    }

    // Use conservative virtual dimensions until the first captured T4
    // dimensions are available. Normal operation replaces these after one XR
    // frame.
    if (full_backbuffer_width == 0) {
        full_backbuffer_width = 640;
    }
    if (full_backbuffer_height == 0) {
        full_backbuffer_height = 480;
    }
    const auto& left = frame.actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Left)];
    const auto& right = frame.actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Right)];
    const bool focused = frame.actions.focused;
    // Mission-only bindings are authorized by the fully validated executable
    // profile. Connection state 10 is shared by SP and MP and is therefore not
    // an executable-identity discriminator.
    const bool single_player_profile =
        g_single_player_profile.load(std::memory_order_acquire);
    const NativeGameplayCommandUpdate native_commands =
        update_native_gameplay_commands(
            {
                .input_owned =
                    focused && controller_gameplay_input_allowed(
                                   presentation.key_catchers,
                                   presentation.connection_state,
                                   presentation.active_connection_state),
                .single_player_profile = single_player_profile,
                .weapon_next_held = bool_held(left.secondary),
                .mission_stick_valid = left.stick.active,
                .mission_stick_x = left.stick.current.x,
                .mission_stick_y = left.stick.current.y,
            },
            &state->native_commands);
    if (native_commands.weapon_next_tap &&
        queue_console_command(cbuf_add_text, kWeaponNextCommand)) {
        result.weapon_next_queued = true;
    }
    if (native_commands.mission_key_tap != 0) {
        send_key_tap(
            key_event, native_commands.mission_key_tap, now_milliseconds);
        result.mission_key_tapped = native_commands.mission_key_tap;
        if (native_commands.mission_key_tap == kMissionDpadLeftKey) {
            WAWVR_STEREO_DIAG_ONCE(
                "CampaignDiag rocket-barrage selection emitted: nativeKey=%d modifier=left-secondary exactSpProfile=1",
                native_commands.mission_key_tap);
        }
    }
    const MenuCursorRegion current_cursor_region = cursor_region(
        full_backbuffer_width, full_backbuffer_height,
        active_gameplay_menu, active_ui_source);
    MenuCursorRegion submitted_cursor_region = current_cursor_region;
    MenuPointerHit pointer_hit{};
    const bool submitted_ui_surface_is_current =
        result.ui_active && menu_pointer_surface_allows_interaction(
            visible_menu_surface, presentation.connection_state,
            presentation.key_catchers, focused,
            menu_escape_tap_requested);
    if (submitted_ui_surface_is_current) {
        pointer_hit = point_at_world_menu_panel(
            right.aim, visible_menu_surface->panel);
        if (pointer_hit.valid) {
            pointer_hit = remap_menu_pointer_to_content(
                pointer_hit, visible_menu_surface->content_viewport);
        }
        if (pointer_hit.valid) {
            submitted_cursor_region =
                visible_menu_surface->cursor_region;
        }
    }
    const bool pointer_trigger_held = update_menu_pointer_trigger(
        focused,
        right.trigger_click.active,
        right.trigger_click.current,
        right.trigger.active,
        right.trigger.current,
        &state->pointer_trigger);
    const MenuNavigationUpdate update = update_menu_navigation(
        {
            // Native stick/A/B fallback follows the freshly read UI catcher
            // even while a captured surface is temporarily unavailable. The
            // exact surface gate remains mandatory for ray position and
            // trigger confirmation, so a stale or invisible panel can never
            // receive a pointer click. Edge latches still prevent a held
            // button from repeating across a synchronous page change.
            .ui_active = result.ui_active,
            .input_focused = focused,
            .active_gameplay_menu = active_gameplay_menu,
            .stick_valid = focused && left.stick.active,
            .stick_x = left.stick.current.x,
            .stick_y = left.stick.current.y,
            .confirm_held = focused && bool_held(right.primary),
            .pointer_valid = pointer_hit.valid,
            .pointer_u = pointer_hit.u,
            .pointer_v = pointer_hit.v,
            .pointer_confirm_held = pointer_trigger_held,
            .back_held = focused && bool_held(right.secondary),
            .now_milliseconds = now_milliseconds,
            .region = submitted_cursor_region,
        },
        &state->navigation);

    if (update.cursor_position_valid) {
        mouse_event(update.cursor_x, update.cursor_y);
        result.cursor_submitted = true;
        result.pointer_submitted = update.pointer_position_used;
    }
    if (update.back_tap && !menu_escape_tap_requested) {
        send_key_tap(key_event, kT4KeyEscape, now_milliseconds);
        result.back_tapped = true;
    } else if (update.confirm_tap) {
        // Defense in depth: even if the pure navigation policy regresses,
        // never dispatch two synchronous native page mutations in one call.
        const NativeMenuConfirmKey confirm_key =
            choose_native_menu_confirm_key(
                g_multiplayer_profile.load(std::memory_order_acquire),
                active_gameplay_menu,
                result.ui_active);
        result.confirm_via_enter =
            confirm_key == NativeMenuConfirmKey::enter;
        send_key_tap(
            key_event,
            result.confirm_via_enter ? kT4KeyEnter : kT4KeyMouse1,
            now_milliseconds);
        result.confirm_tapped = true;
        result.pointer_confirm_tapped = update.pointer_confirm_tap;
    }
    return result;
}

}  // namespace wawvr::mod
