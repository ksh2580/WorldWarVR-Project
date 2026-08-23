#include "stereo_scene_hook.hpp"

#include "lod_fov_clamp.hpp"
#include "stereo_backend_hook.hpp"
#include "stereo_diagnostics.hpp"
#include "stereo_frame_broker.hpp"
#include "peer_thread_quiescence.hpp"
#include "present_hook_logic.hpp"
#include "stereo_scene_math.hpp"
#include "present_hook.hpp"
#include "t4_layout_selector.hpp"
#if defined(WAWVR_HAS_T4_BINDINGS)
#include "t4_presentation_state.hpp"
#endif

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace wawvr::mod {
namespace {

// R_RenderScene inlines R_DynamicShadowType into a stack local at
// [ebp-0x0C].  Same-frame stereo generates the scene twice while sharing the
// renderer's transient shadow lists, so the selector must be clamped before
// any shadow-dependent frontend work runs.  These two exact-build patches
// cover the SHADOW_MAP and SHADOW_COOKIE assignments respectively; changing
// only the generated GfxViewInfo field after R_RenderScene returns is too
// late because the invalid transient lists have already been emitted.
constexpr std::size_t kCallInstructionSize = 5;

struct StereoSceneExecutableProfile final {
    const char* name{};
    std::uintptr_t gameplay_refdef_address{};
    std::uintptr_t gameplay_scene_call_address{};
    std::uintptr_t render_scene_address{};
    std::uintptr_t shadow_map_selector_address{};
    std::uintptr_t shadow_cookie_selector_address{};
    std::uintptr_t front_end_data_out_pointer_address{};
    std::uintptr_t clear_client_cmd_list_2d_address{};
    std::array<std::uint8_t, kCallInstructionSize> expected_gameplay_call{};
    std::array<std::uint8_t, 7> expected_shadow_map_selector{};
    std::array<std::uint8_t, 3> expected_shadow_cookie_selector{};
    std::array<std::uint8_t, 13> expected_render_scene_entry{};
    std::array<std::uint8_t, 35> expected_clear_client_cmd_list_2d{};
};

constexpr StereoSceneExecutableProfile kSpSceneProfile{
    .name = "T4 SP 1.7.1263",
    .gameplay_refdef_address = 0x03520338u,
    .gameplay_scene_call_address = 0x00438C57u,
    .render_scene_address = 0x006DEC70u,
    .shadow_map_selector_address = 0x006DCF0Du,
    .shadow_cookie_selector_address = 0x006DCF21u,
    .front_end_data_out_pointer_address = 0x03DCB498u,
    .clear_client_cmd_list_2d_address = 0x006F56B0u,
    .expected_gameplay_call = {0xE8, 0x14, 0x60, 0x2A, 0x00},
    .expected_shadow_map_selector = {
        0xC7, 0x45, 0xF4, 0x02, 0x00, 0x00, 0x00,
    },
    .expected_shadow_cookie_selector = {0x0F, 0x95, 0xC2},
    .expected_render_scene_entry = {
        0x81, 0xEC, 0xE4, 0x02, 0x00, 0x00, 0x80,
        0x3D, 0x61, 0x69, 0xBF, 0x03, 0x00,
    },
    .expected_clear_client_cmd_list_2d = {
        0xA1, 0x98, 0xB4, 0xDC, 0x03,
        0x8B, 0x88, 0xD0, 0x4D, 0x14, 0x00,
        0x8B, 0x90, 0xD4, 0x4D, 0x14, 0x00,
        0x69, 0xC9, 0x80, 0x6D, 0x00, 0x00,
        0xC7, 0x84, 0x11, 0xCC, 0x58, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xC3,
    },
};

constexpr StereoSceneExecutableProfile kMpSceneProfile{
    .name = "T4 MP 1.7.1263",
    .gameplay_refdef_address = 0x009E676Cu,
    .gameplay_scene_call_address = 0x0043E73Eu,
    .render_scene_address = 0x006B7850u,
    .shadow_map_selector_address = 0x006B588Du,
    .shadow_cookie_selector_address = 0x006B58A1u,
    .front_end_data_out_pointer_address = 0x10882FB4u,
    .clear_client_cmd_list_2d_address = 0x006BD5D0u,
    .expected_gameplay_call = {0xE8, 0x0D, 0x91, 0x27, 0x00},
    .expected_shadow_map_selector = {
        0xC7, 0x45, 0xF4, 0x02, 0x00, 0x00, 0x00,
    },
    .expected_shadow_cookie_selector = {0x0F, 0x95, 0xC2},
    .expected_render_scene_entry = {
        0x81, 0xEC, 0xE4, 0x02, 0x00, 0x00, 0x80,
        0x3D, 0x91, 0x0B, 0x88, 0x10, 0x00,
    },
    .expected_clear_client_cmd_list_2d = {
        0xA1, 0xB4, 0x2F, 0x88, 0x10,
        0x8B, 0x88, 0xD0, 0x4D, 0x14, 0x00,
        0x8B, 0x90, 0xD4, 0x4D, 0x14, 0x00,
        0x69, 0xC9, 0x80, 0x6D, 0x00, 0x00,
        0xC7, 0x84, 0x11, 0xCC, 0x58, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xC3,
    },
};

[[nodiscard]] const StereoSceneExecutableProfile*
configured_scene_profile() noexcept {
    wawvr::t4::ExecutableLayoutId layout{};
    if (!get_configured_renderer_layout(&layout)) {
        return nullptr;
    }
    switch (t4_layout_family_from_id(layout)) {
    case T4LayoutFamily::single_player_1_7_1263:
        return &kSpSceneProfile;
    case T4LayoutFamily::multiplayer_1_7_1263:
        return &kMpSceneProfile;
    case T4LayoutFamily::unsupported:
        return nullptr;
    }
    return nullptr;
}

constexpr std::array<std::uint8_t, 7> kStereoShadowMapSelector = {
    0xC7, 0x45, 0xF4, 0x00, 0x00, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 3> kStereoShadowCookieSelector = {
    0x33, 0xD2, 0x90,
};

constexpr std::size_t kViewportXOffset = 0x00;
constexpr std::size_t kViewportYOffset = 0x04;
constexpr std::size_t kViewportWidthOffset = 0x08;
constexpr std::size_t kViewportHeightOffset = 0x0C;
constexpr std::size_t kTanHalfFovXOffset = 0x10;
constexpr std::size_t kTanHalfFovYOffset = 0x14;
constexpr std::size_t kViewOriginOffset = 0x1C;
constexpr std::size_t kViewAxisOffset = 0x2C;
constexpr std::size_t kNearClipOffset = 0x60;
constexpr std::size_t kMappedRefdefExtent = kNearClipOffset + sizeof(float);
constexpr std::size_t kFrontEndViewInfoCountOffset = 0x144DD0;
constexpr std::size_t kFrontEndViewInfoPointerOffset = 0x144DD4;
constexpr std::size_t kFrontEndViewInfoStride = 0x6D80;
constexpr std::size_t kFrontEndViewCmdsOffset = 0x58CC;

using RenderSceneFunction = void(__cdecl*)(void*, int);
using ClearClientCmdList2DFunction = void(__cdecl*)();

std::atomic<RenderSceneFunction> g_original_render_scene{nullptr};
std::atomic<bool> g_installed{false};
std::atomic<std::uint64_t> g_last_consumed_frame{0};

bool protection_is_readable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffu;
    return access == PAGE_READONLY || access == PAGE_READWRITE ||
           access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

bool protection_is_writable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffu;
    return access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

bool accessible_range(
    const void* const address,
    const std::size_t size,
    const bool require_writable = false) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        !protection_is_readable(memory.Protect) ||
        (require_writable && !protection_is_writable(memory.Protect))) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region_begin =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto region_end = region_begin + memory.RegionSize;
    return begin >= region_begin && begin <= region_end &&
           size <= region_end - begin;
}

// VirtualQuery reports one region at a time. The two exact T4 view records
// cross several image regions with different writable protections, so validate
// every contiguous subrange rather than requiring one MEMORY_BASIC_INFORMATION
// record to cover the complete span.
bool accessible_writable_span(
    const void* const address,
    const std::size_t size,
    const bool emit_diagnostics,
    const char* const diagnostic_name) noexcept {
    constexpr std::size_t kMaximumRegions = 64;
    if (address == nullptr || size == 0) {
        if (emit_diagnostics) {
            stereo_diagnostic_log(
                "StereoDiag %s span rejected: address=%p size=0x%zX",
                diagnostic_name, address, size);
        }
        return false;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) {
        if (emit_diagnostics) {
            stereo_diagnostic_log(
                "StereoDiag %s span rejected: address=%p size=0x%zX overflows",
                diagnostic_name, address, size);
        }
        return false;
    }
    const std::uintptr_t end = begin + size;
    std::uintptr_t cursor = begin;
    std::size_t region_index = 0;

    while (cursor < end && region_index < kMaximumRegions) {
        MEMORY_BASIC_INFORMATION memory{};
        const SIZE_T queried = VirtualQuery(
            reinterpret_cast<const void*>(cursor), &memory,
            sizeof(memory));
        const auto region_begin =
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const bool region_end_valid =
            memory.RegionSize <=
            std::numeric_limits<std::uintptr_t>::max() - region_begin;
        const std::uintptr_t region_end = region_end_valid
            ? region_begin + memory.RegionSize
            : region_begin;
        const bool covers_cursor = queried == sizeof(memory) &&
            region_end_valid && region_begin <= cursor &&
            cursor < region_end;
        const bool committed = memory.State == MEM_COMMIT;
        const bool unguarded =
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        const bool readable = protection_is_readable(memory.Protect);
        const bool writable = protection_is_writable(memory.Protect);
        const bool accepted = covers_cursor && committed && unguarded &&
            readable && writable;

        if (emit_diagnostics) {
            stereo_diagnostic_log(
                "StereoDiag %s region[%zu]: cursor=%p BaseAddress=%p AllocationBase=%p RegionSize=0x%zX State=0x%08lX Protect=0x%08lX Type=0x%08lX queried=%zu accepted=%u",
                diagnostic_name, region_index,
                reinterpret_cast<const void*>(cursor), memory.BaseAddress,
                memory.AllocationBase,
                static_cast<std::size_t>(memory.RegionSize),
                static_cast<unsigned long>(memory.State),
                static_cast<unsigned long>(memory.Protect),
                static_cast<unsigned long>(memory.Type),
                static_cast<std::size_t>(queried), accepted ? 1u : 0u);
        }
        if (!accepted) {
            return false;
        }

        cursor = region_end < end ? region_end : end;
        ++region_index;
    }

    const bool accepted = cursor == end;
    if (emit_diagnostics) {
        stereo_diagnostic_log(
            "StereoDiag %s span result: begin=%p end=%p size=0x%zX regions=%zu accepted=%u",
            diagnostic_name, address, reinterpret_cast<const void*>(end),
            size, region_index, accepted ? 1u : 0u);
    }
    return accepted;
}

template <typename T>
T read_field(const std::byte* const base, const std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

template <typename T>
void write_field(
    std::byte* const base,
    const std::size_t offset,
    const T& value) noexcept {
    std::memcpy(base + offset, &value, sizeof(value));
}

bool read_scene_view(void* const refdef, T4SceneView* const output) noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr || output == nullptr ||
        reinterpret_cast<std::uintptr_t>(refdef) !=
            profile->gameplay_refdef_address ||
        !accessible_range(refdef, kMappedRefdefExtent, true)) {
        return false;
    }
    const auto* const base = static_cast<const std::byte*>(refdef);
    T4SceneView result{};
    result.x = read_field<std::int32_t>(base, kViewportXOffset);
    result.y = read_field<std::int32_t>(base, kViewportYOffset);
    result.width = read_field<std::int32_t>(base, kViewportWidthOffset);
    result.height = read_field<std::int32_t>(base, kViewportHeightOffset);
    result.tan_half_fov_x = read_field<float>(base, kTanHalfFovXOffset);
    result.tan_half_fov_y = read_field<float>(base, kTanHalfFovYOffset);
    result.origin = read_field<wawvr::xr::Vec3f>(base, kViewOriginOffset);
    result.axis = read_field<wawvr::xr::Basis3f>(base, kViewAxisOffset);
    result.near_clip = read_field<float>(base, kNearClipOffset);
    *output = result;
    return true;
}

void write_scene_view(void* const refdef, const T4SceneView& view) noexcept {
    auto* const base = static_cast<std::byte*>(refdef);
    write_field(base, kViewportXOffset, view.x);
    write_field(base, kViewportYOffset, view.y);
    write_field(base, kViewportWidthOffset, view.width);
    write_field(base, kViewportHeightOffset, view.height);
    write_field(base, kTanHalfFovXOffset, view.tan_half_fov_x);
    write_field(base, kTanHalfFovYOffset, view.tan_half_fov_y);
    write_field(base, kViewOriginOffset, view.origin);
    write_field(base, kViewAxisOffset, view.axis);
    write_field(base, kNearClipOffset, view.near_clip);
}

struct FrontendStereoSlots final {
    std::byte* data{};
    std::byte* views{};
    void* client_commands_2d{};
};

bool hud_slot_profile_ready() noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        return false;
    }
    return accessible_range(
               reinterpret_cast<const void*>(
                   profile->clear_client_cmd_list_2d_address),
               profile->expected_clear_client_cmd_list_2d.size()) &&
           std::memcmp(
               reinterpret_cast<const void*>(
                   profile->clear_client_cmd_list_2d_address),
               profile->expected_clear_client_cmd_list_2d.data(),
               profile->expected_clear_client_cmd_list_2d.size()) == 0;
}

bool acquire_frontend_stereo_slots(
    FrontendStereoSlots* const output) noexcept {
    if (output == nullptr) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: output=null");
        return false;
    }
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: renderer profile is not configured");
        return false;
    }
    if (!hud_slot_profile_ready()) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: exact R_ClearClientCmdList2D sentinel mismatch at %p",
            reinterpret_cast<const void*>(
                profile->clear_client_cmd_list_2d_address));
        return false;
    }
    if (!accessible_range(
            reinterpret_cast<const void*>(
                profile->front_end_data_out_pointer_address),
            sizeof(void*))) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: frontEndDataOut global %p is unreadable",
            reinterpret_cast<const void*>(
                profile->front_end_data_out_pointer_address));
        return false;
    }

    std::byte* data = nullptr;
    std::memcpy(
        &data,
        reinterpret_cast<const void*>(
            profile->front_end_data_out_pointer_address),
        sizeof(data));
    if (data == nullptr) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: frontEndDataOut=null");
        return false;
    }
    if (!accessible_range(
            data + kFrontEndViewInfoCountOffset,
            sizeof(std::uint32_t), true)) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: data=%p count@+0x144DD0 is not writable",
            data);
        return false;
    }
    if (!accessible_range(
            data + kFrontEndViewInfoPointerOffset,
            sizeof(void*), true)) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: data=%p views@+0x144DD4 is not writable",
            data);
        return false;
    }
    const std::uint32_t count =
        read_field<std::uint32_t>(data, kFrontEndViewInfoCountOffset);
    if (count != 0u) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: data=%p frontendCount=%u expected=0 views=%p",
            data, count,
            read_field<void*>(data, kFrontEndViewInfoPointerOffset));
        return false;
    }

    std::byte* views =
        read_field<std::byte*>(data, kFrontEndViewInfoPointerOffset);
    static std::atomic_flag span_diagnostic_gate = ATOMIC_FLAG_INIT;
    const bool emit_span_diagnostics =
        !span_diagnostic_gate.test_and_set(std::memory_order_relaxed);
    if (!accessible_writable_span(
            views, kFrontEndViewInfoStride * 2u,
            emit_span_diagnostics, "scene.hud.two-slot")) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: data=%p views=%p two-slot span=0x%zX is not writable",
            data, views, kFrontEndViewInfoStride * 2u);
        return false;
    }

    void* const commands =
        read_field<void*>(views, kFrontEndViewCmdsOffset);
    if (commands == nullptr || !accessible_range(commands, sizeof(std::uint32_t))) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.hud.acquire rejected: data=%p views=%p slot0.cmds=%p readable=%u",
            data, views, commands,
            commands != nullptr &&
                    accessible_range(commands, sizeof(std::uint32_t))
                ? 1u
                : 0u);
        return false;
    }

    *output = {data, views, commands};
    return true;
}

bool frontend_slots_still_owned(
    const FrontendStereoSlots& slots,
    const std::uint32_t expected_count) noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        return false;
    }
    if (!accessible_range(
            reinterpret_cast<const void*>(
                profile->front_end_data_out_pointer_address),
            sizeof(void*))) {
        return false;
    }
    std::byte* current_data = nullptr;
    std::memcpy(
        &current_data,
        reinterpret_cast<const void*>(
            profile->front_end_data_out_pointer_address),
        sizeof(current_data));
    return current_data == slots.data &&
           accessible_range(
               slots.data + kFrontEndViewInfoCountOffset,
               sizeof(std::uint32_t), true) &&
           accessible_range(
               slots.data + kFrontEndViewInfoPointerOffset,
               sizeof(void*), true) &&
           read_field<std::uint32_t>(
               slots.data, kFrontEndViewInfoCountOffset) == expected_count &&
           read_field<std::byte*>(
               slots.data, kFrontEndViewInfoPointerOffset) == slots.views;
}

bool enqueue_stock_fallback(
    void* const refdef,
    const int scene_flags,
    const T4SceneView& stock,
    const FrontendStereoSlots& slots,
    const RenderSceneFunction original) noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        return false;
    }
    write_scene_view(refdef, stock);

    // A fallback draw is safe only while one of the two validated records is
    // unused. At count == 2 another R_RenderScene call would advance T4 to an
    // unvalidated third record, so fail closed instead.
    std::byte* current_data = nullptr;
    if (accessible_range(
            reinterpret_cast<const void*>(
                profile->front_end_data_out_pointer_address),
            sizeof(void*))) {
        std::memcpy(
            &current_data,
            reinterpret_cast<const void*>(
                profile->front_end_data_out_pointer_address),
            sizeof(current_data));
    }
    const bool count_field_writable = accessible_range(
        slots.data + kFrontEndViewInfoCountOffset,
        sizeof(std::uint32_t), true);
    const bool views_field_writable = accessible_range(
        slots.data + kFrontEndViewInfoPointerOffset,
        sizeof(void*), true);
    static std::atomic_flag fallback_span_diagnostic_gate = ATOMIC_FLAG_INIT;
    const bool fallback_span_writable = accessible_writable_span(
        slots.views, kFrontEndViewInfoStride * 2u,
        !fallback_span_diagnostic_gate.test_and_set(
            std::memory_order_relaxed),
        "scene.stock-fallback.two-slot");
    if (current_data != slots.data || !count_field_writable ||
        !views_field_writable ||
        read_field<std::byte*>(
            slots.data, kFrontEndViewInfoPointerOffset) != slots.views ||
        !fallback_span_writable) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.stock-fallback rejected: frontend ownership/range changed savedData=%p currentData=%p savedViews=%p",
            slots.data, current_data, slots.views);
        return false;
    }

    const std::uint32_t count = read_field<std::uint32_t>(
        slots.data, kFrontEndViewInfoCountOffset);
    if (count >= 2u) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.stock-fallback rejected: frontendCount=%u; both validated slots are consumed",
            count);
        return false;
    }
    write_field(
        slots.views +
            static_cast<std::size_t>(count) * kFrontEndViewInfoStride,
        kFrontEndViewCmdsOffset, slots.client_commands_2d);
    original(refdef, scene_flags);
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag scene.stock-fallback accepted: frontendCountBefore=%u targetSlot=%u",
        count, count);
    return true;
}

class RefdefRestore final {
public:
    RefdefRestore(void* const refdef, const T4SceneView& stock) noexcept
        : refdef_(refdef), stock_(stock) {}
    ~RefdefRestore() { write_scene_view(refdef_, stock_); }
    RefdefRestore(const RefdefRestore&) = delete;
    RefdefRestore& operator=(const RefdefRestore&) = delete;

private:
    void* refdef_{};
    T4SceneView stock_{};
};

void __cdecl stereo_scene_thunk(void* const refdef, const int scene_flags) noexcept {
    const auto original =
        g_original_render_scene.load(std::memory_order_acquire);
    if (original == nullptr) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.entry rejected: original R_RenderScene pointer=null");
        return;
    }
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.entry rejected: renderer profile is not configured");
        original(refdef, scene_flags);
        return;
    }

#if defined(WAWVR_HAS_T4_BINDINGS)
    if (profile == &kMpSceneProfile) {
        const T4PresentationState presentation =
            read_t4_presentation_state();
        if (!should_pack_stereo_scene(
                true, presentation.valid, presentation.connection_state,
                presentation.active_connection_state,
                presentation.key_catchers)) {
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.stock-mono accepted: MP stateValid=%u connection=%d active=%d keyCatchers=0x%X",
                presentation.valid ? 1u : 0u,
                presentation.connection_state,
                presentation.active_connection_state,
                presentation.key_catchers);
            original(refdef, scene_flags);
            return;
        }
    }
#endif

    bool began_stereo_calls = false;
    try {
        T4SceneView stock{};
        PendingStereoFrame pending{};
        FrontendStereoSlots slots{};
        const std::uint64_t previous =
            g_last_consumed_frame.load(std::memory_order_acquire);
        if (!stereo_backend_hook_ready()) {
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.stage rejected: exact backend hook is not ready");
            original(refdef, scene_flags);
            return;
        }
        if (!read_scene_view(refdef, &stock)) {
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.stage rejected: refdef=%p expected=%p mappedExtent=0x%zX",
                refdef,
                reinterpret_cast<const void*>(
                    profile->gameplay_refdef_address),
                kMappedRefdefExtent);
            original(refdef, scene_flags);
            return;
        }
        if (!acquire_frontend_stereo_slots(&slots)) {
            original(refdef, scene_flags);
            return;
        }
        if (!try_acquire_pending_stereo_frame(previous, &pending)) {
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.stage rejected: no pending XR frame afterFrame=%llu hudData=%p hudViews=%p hudCmds=%p",
                static_cast<unsigned long long>(previous), slots.data,
                slots.views, slots.client_commands_2d);
            original(refdef, scene_flags);
            return;
        }

        T4StereoSceneViews stereo{};
        if (!build_t4_stereo_scene_views(
                stock, pending.frame, pending.tracking_anchor, &stereo)) {
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.stage rejected: stereo math frame=%llu shouldRender=%u viewsValid=%u stockViewport=(%d,%d %dx%d) fov=(%.6f,%.6f)",
                static_cast<unsigned long long>(pending.frame.frame_id),
                pending.frame.should_render ? 1u : 0u,
                pending.frame.views_valid ? 1u : 0u, stock.x, stock.y,
                stock.width, stock.height, stock.tan_half_fov_x,
                stock.tan_half_fov_y);
            // Consume malformed predicted data once so a repeated gameplay
            // call cannot repeatedly attempt the same rejected transform.
            g_last_consumed_frame.store(
                pending.frame.frame_id, std::memory_order_release);
            original(refdef, scene_flags);
            return;
        }

        g_last_consumed_frame.store(stereo.frame_id, std::memory_order_release);
        RefdefRestore restore(refdef, stock);
        write_scene_view(refdef, stereo.eyes[0]);
        began_stereo_calls = true;
        {
            // The LOD-only clamp is eligible solely while this validated
            // stereo eye is synchronously generating its scene. The scope is
            // gone before every ownership check and stock fallback.
            ScopedStereoLodFovClamp lod_clamp;
            original(refdef, scene_flags);
        }

        if (!frontend_slots_still_owned(slots, 1u) ||
            read_field<void*>(
                slots.views, kFrontEndViewCmdsOffset) !=
                slots.client_commands_2d) {
            std::byte* current_data = nullptr;
            if (accessible_range(
                    reinterpret_cast<const void*>(
                        profile->front_end_data_out_pointer_address),
                    sizeof(void*))) {
                std::memcpy(
                    &current_data,
                    reinterpret_cast<const void*>(
                        profile->front_end_data_out_pointer_address),
                    sizeof(current_data));
            }
            const std::uint32_t count = accessible_range(
                    slots.data + kFrontEndViewInfoCountOffset,
                    sizeof(std::uint32_t))
                ? read_field<std::uint32_t>(
                      slots.data, kFrontEndViewInfoCountOffset)
                : 0xFFFFFFFFu;
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.left rejected after call: savedData=%p currentData=%p savedViews=%p count=%u expected=1 slot0.cmds=%p expectedCmds=%p",
                slots.data, current_data, slots.views, count,
                read_field<void*>(slots.views, kFrontEndViewCmdsOffset),
                slots.client_commands_2d);
            enqueue_stock_fallback(
                refdef, scene_flags, stock, slots, original);
            return;
        }

        reinterpret_cast<ClearClientCmdList2DFunction>(
            profile->clear_client_cmd_list_2d_address)();
        if (!frontend_slots_still_owned(slots, 1u) ||
            read_field<void*>(
                slots.views + kFrontEndViewInfoStride,
                kFrontEndViewCmdsOffset) != nullptr) {
            const std::uint32_t count = accessible_range(
                    slots.data + kFrontEndViewInfoCountOffset,
                    sizeof(std::uint32_t))
                ? read_field<std::uint32_t>(
                      slots.data, kFrontEndViewInfoCountOffset)
                : 0xFFFFFFFFu;
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.hud.clear rejected: data=%p views=%p count=%u expected=1 slot1.cmds=%p expected=null",
                slots.data, slots.views, count,
                read_field<void*>(
                    slots.views + kFrontEndViewInfoStride,
                    kFrontEndViewCmdsOffset));
            enqueue_stock_fallback(
                refdef, scene_flags, stock, slots, original);
            return;
        }

        // Deliberate binocular-HUD policy: both eye records reference the
        // same immutable 2D command stream. The second pending slot was first
        // cleared through T4's own ownership function and verified null.
        write_field(
            slots.views + kFrontEndViewInfoStride,
            kFrontEndViewCmdsOffset, slots.client_commands_2d);
        write_scene_view(refdef, stereo.eyes[1]);
        {
            ScopedStereoLodFovClamp lod_clamp;
            original(refdef, scene_flags);
        }

        if (!frontend_slots_still_owned(slots, 2u) ||
            read_field<void*>(
                slots.views + kFrontEndViewInfoStride,
                kFrontEndViewCmdsOffset) != slots.client_commands_2d) {
            const std::uint32_t count = accessible_range(
                    slots.data + kFrontEndViewInfoCountOffset,
                    sizeof(std::uint32_t))
                ? read_field<std::uint32_t>(
                      slots.data, kFrontEndViewInfoCountOffset)
                : 0xFFFFFFFFu;
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag scene.right rejected after call: data=%p views=%p count=%u expected=2 slot1.cmds=%p expectedCmds=%p; both slots consumed, failing closed",
                slots.data, slots.views, count,
                read_field<void*>(
                    slots.views + kFrontEndViewInfoStride,
                    kFrontEndViewCmdsOffset),
                slots.client_commands_2d);
            return;
        }
        stage_stereo_frame_for_backend(
            stereo.frame_id, stereo.compositor_layout);
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.stage accepted: frame=%llu data=%p views=%p count=2 cmds=%p",
            static_cast<unsigned long long>(stereo.frame_id), slots.data,
            slots.views, slots.client_commands_2d);
    } catch (...) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag scene.stage rejected: exception beganStereoCalls=%u",
            began_stereo_calls ? 1u : 0u);
        // This catch covers publication-lock/allocation failures. The stock
        // scene remains the only safe fallback when stereo setup did not run.
        if (!began_stereo_calls) {
            original(refdef, scene_flags);
        }
    }
}

StereoSceneHookResult map_backend_result(
    const StereoBackendHookResult result) noexcept {
    switch (result) {
    case StereoBackendHookResult::installed:
    case StereoBackendHookResult::already_installed:
        return StereoSceneHookResult::installed;
    case StereoBackendHookResult::profile_mismatch:
        return StereoSceneHookResult::profile_mismatch;
    case StereoBackendHookResult::thread_suspend_failed:
        return StereoSceneHookResult::thread_suspend_failed;
    case StereoBackendHookResult::patch_write_failed:
        return StereoSceneHookResult::patch_write_failed;
    }
    return StereoSceneHookResult::patch_write_failed;
}

[[nodiscard]] bool suspend_scene_patch_threads(
    SuspendedPeerThreads* const suspended) noexcept {
    const auto* const profile = configured_scene_profile();
    if (suspended == nullptr || profile == nullptr) {
        return false;
    }
    const std::array<PeerThreadPatchRange, 3> patch_ranges{{
        {profile->gameplay_scene_call_address, kCallInstructionSize},
        {profile->shadow_map_selector_address,
         profile->expected_shadow_map_selector.size()},
        {profile->shadow_cookie_selector_address,
         profile->expected_shadow_cookie_selector.size()},
    }};
    return suspended->suspend(patch_ranges);
}

bool verify_exact_profile_sites() noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        return false;
    }
    const auto* const call = reinterpret_cast<const std::uint8_t*>(
        profile->gameplay_scene_call_address);
    const auto* const target = reinterpret_cast<const std::uint8_t*>(
        profile->render_scene_address);
    return accessible_range(call, profile->expected_gameplay_call.size()) &&
           accessible_range(
               target, profile->expected_render_scene_entry.size()) &&
           std::memcmp(call, profile->expected_gameplay_call.data(),
                       profile->expected_gameplay_call.size()) == 0 &&
           std::memcmp(target, profile->expected_render_scene_entry.data(),
                       profile->expected_render_scene_entry.size()) == 0 &&
           accessible_range(
               reinterpret_cast<const void*>(
                   profile->shadow_map_selector_address),
               profile->expected_shadow_map_selector.size()) &&
           std::memcmp(
               reinterpret_cast<const void*>(
                   profile->shadow_map_selector_address),
               profile->expected_shadow_map_selector.data(),
               profile->expected_shadow_map_selector.size()) == 0 &&
           accessible_range(
               reinterpret_cast<const void*>(
                   profile->shadow_cookie_selector_address),
               profile->expected_shadow_cookie_selector.size()) &&
           std::memcmp(
               reinterpret_cast<const void*>(
                   profile->shadow_cookie_selector_address),
               profile->expected_shadow_cookie_selector.data(),
               profile->expected_shadow_cookie_selector.size()) == 0 &&
           hud_slot_profile_ready();
}

std::array<std::uint8_t, kCallInstructionSize> replacement_call() noexcept {
    std::array<std::uint8_t, kCallInstructionSize> bytes{};
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        return bytes;
    }
    bytes[0] = 0xE8;
    const auto next_instruction =
        static_cast<std::uint32_t>(
            profile->gameplay_scene_call_address + kCallInstructionSize);
    const auto target = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(&stereo_scene_thunk));
    const std::uint32_t displacement = target - next_instruction;
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    return bytes;
}

bool scene_patch_ownership_ready() noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        return false;
    }
    const auto call = replacement_call();
    return accessible_range(
               reinterpret_cast<const void*>(
                   profile->gameplay_scene_call_address),
               call.size()) &&
           std::memcmp(
               reinterpret_cast<const void*>(
                   profile->gameplay_scene_call_address),
               call.data(), call.size()) == 0 &&
           accessible_range(
               reinterpret_cast<const void*>(
                   profile->shadow_map_selector_address),
               kStereoShadowMapSelector.size()) &&
           std::memcmp(
               reinterpret_cast<const void*>(
                   profile->shadow_map_selector_address),
               kStereoShadowMapSelector.data(),
               kStereoShadowMapSelector.size()) == 0 &&
           accessible_range(
               reinterpret_cast<const void*>(
                   profile->shadow_cookie_selector_address),
               kStereoShadowCookieSelector.size()) &&
           std::memcmp(
               reinterpret_cast<const void*>(
                   profile->shadow_cookie_selector_address),
               kStereoShadowCookieSelector.data(),
               kStereoShadowCookieSelector.size()) == 0 &&
           hud_slot_profile_ready();
}

// Changes the gameplay call and both inlined shadow-selector assignments as
// one ownership-checked transaction while peer threads are suspended by the
// caller.  No bytes are written until both code ranges are writable and all
// three current byte sequences match the requested source state.
bool write_scene_patches(const bool install) noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr ||
        profile->shadow_cookie_selector_address <
            profile->shadow_map_selector_address) {
        return false;
    }
    auto* const call = reinterpret_cast<std::uint8_t*>(
        profile->gameplay_scene_call_address);
    auto* const shadow_map = reinterpret_cast<std::uint8_t*>(
        profile->shadow_map_selector_address);
    auto* const shadow_cookie = reinterpret_cast<std::uint8_t*>(
        profile->shadow_cookie_selector_address);
    const auto replacement = replacement_call();
    const auto& expected_call =
        install ? profile->expected_gameplay_call : replacement;
    const auto& desired_call =
        install ? replacement : profile->expected_gameplay_call;
    const auto& expected_map = install
        ? profile->expected_shadow_map_selector
        : kStereoShadowMapSelector;
    const auto& desired_map = install
        ? kStereoShadowMapSelector
        : profile->expected_shadow_map_selector;
    const auto& expected_cookie = install
        ? profile->expected_shadow_cookie_selector
        : kStereoShadowCookieSelector;
    const auto& desired_cookie = install
        ? kStereoShadowCookieSelector
        : profile->expected_shadow_cookie_selector;

    const std::size_t shadow_patch_span =
        profile->shadow_cookie_selector_address -
            profile->shadow_map_selector_address +
        profile->expected_shadow_cookie_selector.size();
    DWORD old_call_protection = 0;
    DWORD old_shadow_protection = 0;
    if (!VirtualProtect(
            call, kCallInstructionSize, PAGE_EXECUTE_READWRITE,
            &old_call_protection)) {
        return false;
    }
    if (!VirtualProtect(
            shadow_map, shadow_patch_span, PAGE_EXECUTE_READWRITE,
            &old_shadow_protection)) {
        DWORD ignored = 0;
        VirtualProtect(
            call, kCallInstructionSize, old_call_protection, &ignored);
        return false;
    }

    const bool owned =
        std::memcmp(
            call, expected_call.data(), expected_call.size()) == 0 &&
        std::memcmp(
            shadow_map, expected_map.data(), expected_map.size()) == 0 &&
        std::memcmp(
            shadow_cookie, expected_cookie.data(),
            expected_cookie.size()) == 0;
    if (owned) {
        std::memcpy(call, desired_call.data(), desired_call.size());
        std::memcpy(shadow_map, desired_map.data(), desired_map.size());
        std::memcpy(
            shadow_cookie, desired_cookie.data(), desired_cookie.size());
        FlushInstructionCache(
            GetCurrentProcess(), call, kCallInstructionSize);
        FlushInstructionCache(
            GetCurrentProcess(), shadow_map, shadow_patch_span);
    }

    DWORD ignored = 0;
    VirtualProtect(
        shadow_map, shadow_patch_span, old_shadow_protection, &ignored);
    VirtualProtect(
        call, kCallInstructionSize, old_call_protection, &ignored);
    return owned;
}

} // namespace

StereoSceneHookResult install_stereo_scene_hook() noexcept {
    const auto* const profile = configured_scene_profile();
    if (profile == nullptr) {
        return StereoSceneHookResult::profile_mismatch;
    }
    if (g_installed.load(std::memory_order_acquire)) {
        return stereo_backend_hook_ready() && scene_patch_ownership_ready()
                   ? StereoSceneHookResult::already_installed
                   : StereoSceneHookResult::profile_mismatch;
    }
    if (!verify_exact_profile_sites()) {
        return StereoSceneHookResult::profile_mismatch;
    }

    // Decode and independently re-check the relative target instead of
    // trusting the hard-coded address alone.
    std::int32_t displacement = 0;
    std::memcpy(
        &displacement,
        reinterpret_cast<const void*>(
            profile->gameplay_scene_call_address + 1),
        sizeof(displacement));
    const auto decoded_target = static_cast<std::uintptr_t>(
        static_cast<std::uint32_t>(
            profile->gameplay_scene_call_address +
            kCallInstructionSize + displacement));
    if (decoded_target != profile->render_scene_address) {
        return StereoSceneHookResult::target_mismatch;
    }

    const auto backend_result = install_stereo_backend_hook();
    if (backend_result != StereoBackendHookResult::installed &&
        backend_result != StereoBackendHookResult::already_installed) {
        return map_backend_result(backend_result);
    }
    if (!stereo_backend_hook_ready()) {
        static_cast<void>(restore_stereo_backend_hook());
        return StereoSceneHookResult::profile_mismatch;
    }

    StereoSceneHookResult scene_patch_result = StereoSceneHookResult::installed;
    {
        SuspendedPeerThreads suspended;
        if (!suspend_scene_patch_threads(&suspended)) {
            scene_patch_result = StereoSceneHookResult::thread_suspend_failed;
        } else if (!verify_exact_profile_sites()) {
            scene_patch_result = StereoSceneHookResult::profile_mismatch;
        } else {
            g_original_render_scene.store(
                reinterpret_cast<RenderSceneFunction>(
                    profile->render_scene_address),
                std::memory_order_release);
            if (!write_scene_patches(true)) {
                g_original_render_scene.store(
                    nullptr, std::memory_order_release);
                scene_patch_result = StereoSceneHookResult::patch_write_failed;
            }
        }
    }
    if (scene_patch_result != StereoSceneHookResult::installed) {
        static_cast<void>(restore_stereo_backend_hook());
        return scene_patch_result;
    }
    g_last_consumed_frame.store(0, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    stereo_diagnostic_log(
        "StereoDiag scene.shadow policy installed: %s exact R_RenderScene local selectors at %p/%p force SHADOW_NONE before stereo scene generation",
        profile->name,
        reinterpret_cast<const void*>(profile->shadow_map_selector_address),
        reinterpret_cast<const void*>(
            profile->shadow_cookie_selector_address));
    return StereoSceneHookResult::installed;
}

StereoSceneHookResult restore_stereo_scene_hook() noexcept {
    const bool scene_installed =
        g_installed.load(std::memory_order_acquire);
    const bool backend_installed = stereo_backend_hook_installed();
    if (!scene_installed && !backend_installed) {
        return StereoSceneHookResult::already_installed;
    }
    if (scene_installed) {
        SuspendedPeerThreads suspended;
        if (!suspend_scene_patch_threads(&suspended)) {
            return StereoSceneHookResult::thread_suspend_failed;
        }
        if (!write_scene_patches(false)) {
            return StereoSceneHookResult::patch_write_failed;
        }
        g_installed.store(false, std::memory_order_release);
        g_original_render_scene.store(nullptr, std::memory_order_release);
        g_last_consumed_frame.store(0, std::memory_order_release);
    }

    if (backend_installed) {
        const auto backend_result = restore_stereo_backend_hook();
        if (backend_result != StereoBackendHookResult::installed &&
            backend_result != StereoBackendHookResult::already_installed) {
            return map_backend_result(backend_result);
        }
    }
    clear_pending_stereo_frame();
    return StereoSceneHookResult::installed;
}

bool stereo_scene_hook_installed() noexcept {
    return g_installed.load(std::memory_order_acquire) ||
           stereo_backend_hook_installed();
}

bool stereo_scene_ready_for_current_com_frame() noexcept {
    // The exact T4 draw path invokes the patched CG_Draw2D call before the
    // gameplay R_RenderScene call. At both placement boundaries, therefore,
    // the current publication must still be newer than the last frame this
    // scene hook consumed. This rejects a valid-looking publication retained
    // across a Reset/recovery early return without rejecting this frame's HUD.
    const std::uint64_t consumed_frame_id =
        g_last_consumed_frame.load(std::memory_order_acquire);
    if (!g_installed.load(std::memory_order_acquire) ||
        g_original_render_scene.load(std::memory_order_acquire) == nullptr ||
        !stereo_backend_hook_ready() || !scene_patch_ownership_ready() ||
        !pending_stereo_frame_ready_after(consumed_frame_id)) {
        return false;
    }
    // Close teardown/foreign-patch windows opened while the ownership and
    // broker checks ran. This remains a snapshot rather than a reservation;
    // the scene thunk independently fails closed if XR is invalidated later.
    return g_installed.load(std::memory_order_acquire) &&
           g_original_render_scene.load(std::memory_order_acquire) != nullptr &&
           stereo_backend_hook_ready() && scene_patch_ownership_ready() &&
           pending_stereo_frame_ready_after(consumed_frame_id);
}

const char* describe_stereo_scene_hook_result(
    const StereoSceneHookResult result) noexcept {
    switch (result) {
    case StereoSceneHookResult::installed:
        return "installed/restored";
    case StereoSceneHookResult::already_installed:
        return "already in requested state";
    case StereoSceneHookResult::profile_mismatch:
        return "exact call-site or target sentinel mismatch";
    case StereoSceneHookResult::target_mismatch:
        return "relative call did not target exact R_RenderScene";
    case StereoSceneHookResult::thread_suspend_failed:
        return "could not safely suspend peer threads";
    case StereoSceneHookResult::patch_write_failed:
        return "protected call-site write/ownership check failed";
    }
    return "unknown";
}

} // namespace wawvr::mod
