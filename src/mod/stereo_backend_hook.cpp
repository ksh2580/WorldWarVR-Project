#include "stereo_backend_hook.hpp"

#include "peer_thread_quiescence.hpp"
#include "present_hook.hpp"
#include "stereo_diagnostics.hpp"
#include "stereo_frame_broker.hpp"
#include "t4_layout_selector.hpp"

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace wawvr::mod {
namespace {

static_assert(sizeof(void*) == 4, "The exact T4 backend hook is x86-only");

constexpr std::size_t kCallSize = 5;

template <std::size_t Size>
struct ExactSentinel final {
    std::uintptr_t address{};
    std::array<std::uint8_t, Size> expected{};
};

struct StereoBackendExecutableProfile final {
    const char* name{};
    std::uintptr_t synchronous_draw_call_address{};
    std::uintptr_t smp_draw_call_address{};
    std::uintptr_t setup_clear_call_address{};
    std::uintptr_t scene_clear_call_address{};
    std::uintptr_t draw_3d_internal_address{};
    std::uintptr_t clear_screen_address{};
    std::uintptr_t standard_emissive_callback_push_address{};
    std::uintptr_t standard_emissive_callback_address{};
    std::uintptr_t back_end_data_pointer_address{};
    std::array<std::uint8_t, kCallSize> expected_synchronous_draw_call{};
    std::array<std::uint8_t, kCallSize> expected_smp_draw_call{};
    std::array<std::uint8_t, kCallSize> expected_setup_clear_call{};
    std::array<std::uint8_t, kCallSize> expected_scene_clear_call{};
    std::array<std::uint8_t, 14> expected_draw_3d_internal_entry{};
    std::array<std::uint8_t, 16> expected_clear_screen_entry{};
    std::array<std::uint8_t, kCallSize>
        expected_standard_emissive_callback_push{};
    std::array<std::uint8_t, 16>
        expected_standard_emissive_callback_entry{};
    ExactSentinel<58> standard_emissive_callback_tail{};
    ExactSentinel<12> view_stride{};
    ExactSentinel<16> split_lit_primary{};
    ExactSentinel<19> split_lit_secondary{};
    ExactSentinel<16> lit_list{};
    ExactSentinel<16> emissive_list{};
    ExactSentinel<22> decal_list{};
    ExactSentinel<14> list_metadata{};
    ExactSentinel<6> point_light_base{};
    ExactSentinel<9> point_light_info{};
    ExactSentinel<9> point_light_stride{};
    ExactSentinel<6> point_light_count{};
    ExactSentinel<46> emissive_spot_layout{};
};

constexpr StereoBackendExecutableProfile kSpBackendProfile{
    .name = "T4 SP 1.7.1263",
    .synchronous_draw_call_address = 0x006FC112u,
    .smp_draw_call_address = 0x006FC4F8u,
    .setup_clear_call_address = 0x006E8439u,
    .scene_clear_call_address = 0x006E858Du,
    .draw_3d_internal_address = 0x006E8B90u,
    .clear_screen_address = 0x0072BB30u,
    .standard_emissive_callback_push_address = 0x006E7E30u,
    .standard_emissive_callback_address = 0x006E7D00u,
    .back_end_data_pointer_address = 0x03DCB4CCu,
    .expected_synchronous_draw_call = {0xE8, 0x79, 0xCA, 0xFE, 0xFF},
    .expected_smp_draw_call = {0xE8, 0x93, 0xC6, 0xFE, 0xFF},
    .expected_setup_clear_call = {0xE8, 0xF2, 0x36, 0x04, 0x00},
    .expected_scene_clear_call = {0xE8, 0x9E, 0x35, 0x04, 0x00},
    .expected_draw_3d_internal_entry = {
        0x51, 0xA1, 0x30, 0x85, 0x65, 0x04, 0x83,
        0xE8, 0x00, 0x55, 0x8B, 0x6C, 0x24, 0x0C,
    },
    .expected_clear_screen_entry = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0, 0x83, 0xEC,
        0x34, 0x53, 0x8A, 0x5D, 0x0C, 0x56, 0x8B, 0xF0,
    },
    .expected_standard_emissive_callback_push = {
        0x68, 0x00, 0x7D, 0x6E, 0x00,
    },
    .expected_standard_emissive_callback_entry = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x0F, 0x57,
        0xC0, 0x56, 0x8B, 0x75, 0x0C, 0xF3, 0x0F, 0x10,
    },
    .standard_emissive_callback_tail = {0x006E7D82u, {
        0x68, 0xF0, 0xEF, 0x89, 0x00, 0x6A, 0xFF, 0xE8,
        0x22, 0x0A, 0x0E, 0x00, 0x8B, 0x7D, 0x10, 0x8B,
        0x45, 0x08, 0x57, 0x56, 0x05, 0xD8, 0x65, 0x00,
        0x00, 0x50, 0x6A, 0x00, 0xE8, 0x8D, 0x0C, 0x01,
        0x00, 0x83, 0xC4, 0x10, 0xE8, 0xFF, 0x09, 0x0E,
        0x00, 0x8B, 0xB7, 0x90, 0x00, 0x00, 0x00, 0xE8,
        0x6A, 0xE5, 0xFF, 0xFF, 0x5F, 0x5E, 0x8B, 0xE5,
        0x5D, 0xC3,
    }},
    .view_stride = {0x006DCEEBu, {
        0x69, 0xDB, 0x80, 0x6D, 0x00, 0x00,
        0x03, 0x98, 0xD4, 0x4D, 0x14, 0x00,
    }},
    .split_lit_primary = {0x006DDADEu, {
        0x6A, 0x2C, 0x8D, 0xBB, 0x54, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x57, 0xE8, 0x52, 0x24, 0x0D, 0x00,
    }},
    .split_lit_secondary = {0x006DDB78u, {
        0x6A, 0x2C, 0x8D, 0xB3, 0x80, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x56, 0x89, 0x57, 0x04, 0xE8, 0xB5,
        0x23, 0x0D, 0x00,
    }},
    .lit_list = {0x006DDCAFu, {
        0x6A, 0x2C, 0x8D, 0xB3, 0xAC, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x56, 0xE8, 0x81, 0x22, 0x0D, 0x00,
    }},
    .emissive_list = {0x006DDD4Au, {
        0x6A, 0x2C, 0x8D, 0x83, 0xD8, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x50, 0xE8, 0xE6, 0x21, 0x0D, 0x00,
    }},
    .decal_list = {0x006DDE3Au, {
        0x6A, 0x2C, 0x8D, 0xB3, 0x04, 0x66, 0x00, 0x00,
        0x6A, 0x00, 0x56, 0x89, 0x83, 0xDC, 0x65, 0x00,
        0x00, 0xE8, 0xF0, 0x20, 0x0D, 0x00,
    }},
    .list_metadata = {0x006DDAF9u, {
        0x89, 0x57, 0x08, 0x89, 0x5F, 0x0C, 0xD9,
        0x06, 0xD9, 0x5F, 0x14, 0x83, 0xC4, 0x0C,
    }},
    .point_light_base = {0x00728A38u, {
        0x81, 0xC3, 0xCC, 0x53, 0x00, 0x00,
    }},
    .point_light_info = {0x00728A61u, {
        0x6A, 0x2C, 0x8D, 0x43, 0xE4, 0x6A, 0x00, 0x50, 0xE8,
    }},
    .point_light_stride = {0x00728B6Au, {
        0x89, 0x4B, 0xE4, 0x89, 0x43, 0xE8, 0x83, 0xC3, 0x6C,
    }},
    .point_light_count = {0x006DDC2Bu, {
        0x89, 0x83, 0xA0, 0x55, 0x00, 0x00,
    }},
    .emissive_spot_layout = {0x006DC64Cu, {
        0x8D, 0xBD, 0xA8, 0x55, 0x00, 0x00,
        0xB9, 0x10, 0x00, 0x00, 0x00,
        0xBE, 0x78, 0x26, 0xD5, 0x03,
        0xF3, 0xA5,
        0x89, 0x9D, 0xA4, 0x55, 0x00, 0x00,
        0x89, 0x9D, 0xE8, 0x55, 0x00, 0x00,
        0x89, 0x9D, 0xEC, 0x55, 0x00, 0x00,
        0xC7, 0x85, 0xF0, 0x55, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    }},
};

constexpr StereoBackendExecutableProfile kMpBackendProfile{
    .name = "T4 MP 1.7.1263",
    .synchronous_draw_call_address = 0x006D7242u,
    .smp_draw_call_address = 0x006D762Cu,
    .setup_clear_call_address = 0x006BA3C9u,
    .scene_clear_call_address = 0x006BA51Du,
    .draw_3d_internal_address = 0x006BAB20u,
    .clear_screen_address = 0x006FDC70u,
    .standard_emissive_callback_push_address = 0x006B9DC0u,
    .standard_emissive_callback_address = 0x006B9C90u,
    .back_end_data_pointer_address = 0x10CF9B5Cu,
    .expected_synchronous_draw_call = {0xE8, 0xD9, 0x38, 0xFE, 0xFF},
    .expected_smp_draw_call = {0xE8, 0xEF, 0x34, 0xFE, 0xFF},
    .expected_setup_clear_call = {0xE8, 0xA2, 0x38, 0x04, 0x00},
    .expected_scene_clear_call = {0xE8, 0x4E, 0x37, 0x04, 0x00},
    .expected_draw_3d_internal_entry = {
        0x51, 0xA1, 0x28, 0x93, 0x2F, 0x11, 0x83,
        0xE8, 0x00, 0x55, 0x8B, 0x6C, 0x24, 0x0C,
    },
    .expected_clear_screen_entry = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0, 0x83, 0xEC,
        0x34, 0x53, 0x8A, 0x5D, 0x0C, 0x56, 0x8B, 0xF0,
    },
    .expected_standard_emissive_callback_push = {
        0x68, 0x90, 0x9C, 0x6B, 0x00,
    },
    .expected_standard_emissive_callback_entry = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x0F, 0x57,
        0xC0, 0x56, 0x8B, 0x75, 0x0C, 0xF3, 0x0F, 0x10,
    },
    .standard_emissive_callback_tail = {0x006B9D12u, {
        0x68, 0xC0, 0x24, 0x89, 0x00, 0x6A, 0xFF, 0xE8,
        0xDA, 0x99, 0x10, 0x00, 0x8B, 0x7D, 0x10, 0x8B,
        0x45, 0x08, 0x57, 0x56, 0x05, 0xD8, 0x65, 0x00,
        0x00, 0x50, 0x6A, 0x00, 0xE8, 0x4D, 0x9E, 0x01,
        0x00, 0x83, 0xC4, 0x10, 0xE8, 0xC3, 0x99, 0x10,
        0x00, 0x8B, 0xB7, 0x90, 0x00, 0x00, 0x00, 0xE8,
        0xCA, 0xE5, 0xFF, 0xFF, 0x5F, 0x5E, 0x8B, 0xE5,
        0x5D, 0xC3,
    }},
    .view_stride = {0x006B586Bu, {
        0x69, 0xDB, 0x80, 0x6D, 0x00, 0x00,
        0x03, 0x98, 0xD4, 0x4D, 0x14, 0x00,
    }},
    .split_lit_primary = {0x006B6590u, {
        0x6A, 0x2C, 0x8D, 0xBB, 0x54, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x57, 0xE8, 0xE0, 0x28, 0x0F, 0x00,
    }},
    .split_lit_secondary = {0x006B662Au, {
        0x6A, 0x2C, 0x8D, 0xB3, 0x80, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x56, 0x89, 0x57, 0x04, 0xE8, 0x43,
        0x28, 0x0F, 0x00,
    }},
    .lit_list = {0x006B675Fu, {
        0x6A, 0x2C, 0x8D, 0xB3, 0xAC, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x56, 0xE8, 0x11, 0x27, 0x0F, 0x00,
    }},
    .emissive_list = {0x006B67FAu, {
        0x6A, 0x2C, 0x8D, 0x83, 0xD8, 0x65, 0x00, 0x00,
        0x6A, 0x00, 0x50, 0xE8, 0x76, 0x26, 0x0F, 0x00,
    }},
    .decal_list = {0x006B68EAu, {
        0x6A, 0x2C, 0x8D, 0xB3, 0x04, 0x66, 0x00, 0x00,
        0x6A, 0x00, 0x56, 0x89, 0x83, 0xDC, 0x65, 0x00,
        0x00, 0xE8, 0x80, 0x25, 0x0F, 0x00,
    }},
    .list_metadata = {0x006B65ABu, {
        0x89, 0x57, 0x08, 0x89, 0x5F, 0x0C, 0xD9,
        0x06, 0xD9, 0x5F, 0x14, 0x83, 0xC4, 0x0C,
    }},
    .point_light_base = {0x00701D78u, {
        0x81, 0xC3, 0xCC, 0x53, 0x00, 0x00,
    }},
    .point_light_info = {0x00701DA1u, {
        0x6A, 0x2C, 0x8D, 0x43, 0xE4, 0x6A, 0x00, 0x50, 0xE8,
    }},
    .point_light_stride = {0x00701EAAu, {
        0x89, 0x4B, 0xE4, 0x89, 0x43, 0xE8, 0x83, 0xC3, 0x6C,
    }},
    .point_light_count = {0x006B66DBu, {
        0x89, 0x83, 0xA0, 0x55, 0x00, 0x00,
    }},
    .emissive_spot_layout = {0x006B4F7Cu, {
        0x8D, 0xBD, 0xA8, 0x55, 0x00, 0x00,
        0xB9, 0x10, 0x00, 0x00, 0x00,
        0xBE, 0x78, 0x07, 0x9E, 0x10,
        0xF3, 0xA5,
        0x89, 0x9D, 0xA4, 0x55, 0x00, 0x00,
        0x89, 0x9D, 0xE8, 0x55, 0x00, 0x00,
        0x89, 0x9D, 0xEC, 0x55, 0x00, 0x00,
        0xC7, 0x85, 0xF0, 0x55, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    }},
};

[[nodiscard]] const StereoBackendExecutableProfile*
configured_backend_profile() noexcept {
    wawvr::t4::ExecutableLayoutId layout{};
    if (!get_configured_renderer_layout(&layout)) {
        return nullptr;
    }
    switch (t4_layout_family_from_id(layout)) {
    case T4LayoutFamily::single_player_1_7_1263:
        return &kSpBackendProfile;
    case T4LayoutFamily::multiplayer_1_7_1263:
        return &kMpBackendProfile;
    case T4LayoutFamily::unsupported:
        return nullptr;
    }
    return nullptr;
}

constexpr std::size_t kViewInfoIndexOffset = 0x144DCC;
constexpr std::size_t kViewInfoCountOffset = 0x144DD0;
constexpr std::size_t kViewInfoPointerOffset = 0x144DD4;
constexpr std::size_t kViewInfoStride = 0x6D80;
constexpr std::size_t kViewOriginOffset = 0x100;
constexpr std::size_t kScissorViewportOffset = 0x174;
constexpr std::size_t kRendererDeviceOffset = 0x90;
constexpr std::size_t kDrawListViewInfoOffset = 0x0C;
constexpr std::size_t kDrawListViewOriginOffset = 0x14;
constexpr std::size_t kDrawListLightOffset = 0x24;
constexpr std::size_t kDrawListCameraViewOffset = 0x28;
constexpr std::size_t kIsRenderingFullScreenOffset = 0x13A4;
constexpr std::size_t kNeedsFloatZOffset = 0x13A8;
constexpr std::size_t kPointLightPartitionOffset = 0x5370;
constexpr std::size_t kPointLightInfoOffset = 0x40;
constexpr std::size_t kPointLightPartitionStride = 0x6C;
constexpr std::size_t kPointLightCountOffset = 0x55A0;
constexpr std::size_t kEmissiveSpotLightIndexOffset = 0x55A4;
constexpr std::size_t kEmissiveSpotDrawSurfCountOffset = 0x55E8;
constexpr std::size_t kEmissiveSpotDrawSurfsOffset = 0x55EC;
constexpr std::size_t kEmissiveSpotLightCountOffset = 0x55F0;
constexpr std::uint32_t kMaximumPointLightPartitions = 4;
constexpr std::uint32_t kMaximumDrawSurfCount = 131072;
constexpr std::size_t kDrawSurfSize = 8;
constexpr std::array<std::size_t, 5> kCoreDrawListOffsets = {
    0x6554, 0x6580, 0x65AC, 0x65D8, 0x6604,
};
static_assert(
    kPointLightPartitionOffset +
            kMaximumPointLightPartitions * kPointLightPartitionStride ==
        0x5520);
static_assert(kEmissiveSpotDrawSurfsOffset + sizeof(void*) <= kViewInfoStride);
using Draw3DInternalFunction = void(__cdecl*)(const void*);
using StandardEmissiveCallback = void(__cdecl*)(
    const void*, void*, void*, std::uintptr_t, std::uintptr_t);

std::atomic<bool> g_installed{false};
std::atomic<std::uintptr_t> g_original_standard_emissive_callback{0};
thread_local bool g_suppress_secondary_full_clear = false;
thread_local std::uint32_t g_diagnostic_eye_phase = 0;
thread_local std::uint32_t g_diagnostic_primary_clear_calls = 0;
thread_local std::uint32_t g_diagnostic_secondary_clear_calls = 0;

struct FxStateTelemetry final {
    bool initialized{};
    std::uint32_t emissive_draw_surf_count{};
    std::uint32_t point_light_count{};
    std::uint32_t emissive_spot_light_count{};
    std::uint32_t fullscreen_flag{};
    std::uint8_t needs_float_z{};
    ULONGLONG next_log_milliseconds{};
};

thread_local FxStateTelemetry g_fx_state_telemetry{};

void __cdecl stereo_backend_draw_thunk(const void* selected_view) noexcept;
void __cdecl stereo_emissive_scissor_thunk(
    const void* view, void* source, void* state,
    std::uintptr_t unused_first,
    std::uintptr_t unused_second) noexcept;
bool __cdecl should_suppress_secondary_clear() noexcept;

#if defined(_MSC_VER) && defined(_M_IX86)
// The T4 clear helper receives its color pointer in EAX and leaves four cdecl
// arguments on the caller's stack. This exact naked bridge preserves EAX,
// tail-calls the stock helper normally, and returns directly only for the
// second eye while our backend wrapper is active.
__declspec(naked) void stereo_clear_thunk_sp() noexcept {
    __asm {
        push eax
        call should_suppress_secondary_clear
        test al, al
        pop eax
        jnz suppress_clear
        mov edx, 0072BB30h
        jmp edx
    suppress_clear:
        ret
    }
}

__declspec(naked) void stereo_clear_thunk_mp() noexcept {
    __asm {
        push eax
        call should_suppress_secondary_clear
        test al, al
        pop eax
        jnz suppress_clear
        mov edx, 006FDC70h
        jmp edx
    suppress_clear:
        ret
    }
}
#else
#error The exact T4 clear bridge requires the 32-bit MSVC ABI.
#endif

struct PatchSite final {
    std::uintptr_t address{};
    std::array<std::uint8_t, kCallSize> original{};
    const void* replacement_target{};
};

std::array<std::uint8_t, kCallSize> relative_call(
    const std::uintptr_t address,
    const void* const target) noexcept {
    std::array<std::uint8_t, kCallSize> result{};
    result[0] = 0xE8;
    const auto next = static_cast<std::uint32_t>(address + kCallSize);
    const auto destination = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(target));
    const std::uint32_t displacement = destination - next;
    std::memcpy(result.data() + 1, &displacement, sizeof(displacement));
    return result;
}

std::array<std::uint8_t, kCallSize> absolute_push(
    const void* const target) noexcept {
    std::array<std::uint8_t, kCallSize> result{};
    result[0] = 0x68;
    const auto destination = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(target));
    std::memcpy(result.data() + 1, &destination, sizeof(destination));
    return result;
}

std::array<PatchSite, 4> patch_sites(
    const StereoBackendExecutableProfile& profile) noexcept {
    const void* const clear_thunk =
        &profile == &kSpBackendProfile
            ? reinterpret_cast<const void*>(&stereo_clear_thunk_sp)
            : reinterpret_cast<const void*>(&stereo_clear_thunk_mp);
    return {{
        {profile.synchronous_draw_call_address,
         profile.expected_synchronous_draw_call,
         reinterpret_cast<const void*>(&stereo_backend_draw_thunk)},
        {profile.smp_draw_call_address, profile.expected_smp_draw_call,
         reinterpret_cast<const void*>(&stereo_backend_draw_thunk)},
        {profile.setup_clear_call_address, profile.expected_setup_clear_call,
         clear_thunk},
        {profile.scene_clear_call_address, profile.expected_scene_clear_call,
         clear_thunk},
    }};
}

PatchSite emissive_callback_patch_site(
    const StereoBackendExecutableProfile& profile) noexcept {
    return {
        profile.standard_emissive_callback_push_address,
        profile.expected_standard_emissive_callback_push,
        reinterpret_cast<const void*>(&stereo_emissive_scissor_thunk),
    };
}

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
    const bool writable = false) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        !protection_is_readable(memory.Protect) ||
        (writable && !protection_is_writable(memory.Protect))) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region_begin =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto region_end = region_begin + memory.RegionSize;
    return begin >= region_begin && begin <= region_end &&
           size <= region_end - begin;
}

enum class WritableSpanRejectReason : std::uint8_t {
    none,
    invalid_argument,
    address_overflow,
    query_or_gap,
    not_committed,
    guarded_or_noaccess,
    not_readable,
    not_writable,
    region_limit,
};

struct WritableSpanValidation final {
    WritableSpanRejectReason reason{WritableSpanRejectReason::none};
    std::size_t regions{};
    std::uintptr_t cursor{};
    MEMORY_BASIC_INFORMATION memory{};
};

const char* describe_span_rejection(
    const WritableSpanRejectReason reason) noexcept {
    switch (reason) {
    case WritableSpanRejectReason::none:
        return "none";
    case WritableSpanRejectReason::invalid_argument:
        return "invalid-argument";
    case WritableSpanRejectReason::address_overflow:
        return "address-overflow";
    case WritableSpanRejectReason::query_or_gap:
        return "query-or-gap";
    case WritableSpanRejectReason::not_committed:
        return "not-committed";
    case WritableSpanRejectReason::guarded_or_noaccess:
        return "guarded-or-noaccess";
    case WritableSpanRejectReason::not_readable:
        return "not-readable";
    case WritableSpanRejectReason::not_writable:
        return "not-writable";
    case WritableSpanRejectReason::region_limit:
        return "region-limit";
    }
    return "unknown";
}

bool accessible_writable_span(
    const void* const address,
    const std::size_t size,
    WritableSpanValidation* const output) noexcept {
    constexpr std::size_t kMaximumRegions = 512;
    WritableSpanValidation result{};
    result.cursor = reinterpret_cast<std::uintptr_t>(address);
    if (address == nullptr || size == 0) {
        result.reason = WritableSpanRejectReason::invalid_argument;
        if (output != nullptr) {
            *output = result;
        }
        return false;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) {
        result.reason = WritableSpanRejectReason::address_overflow;
        if (output != nullptr) {
            *output = result;
        }
        return false;
    }
    const std::uintptr_t end = begin + size;
    std::uintptr_t cursor = begin;

    while (cursor < end) {
        result.cursor = cursor;
        if (result.regions >= kMaximumRegions) {
            result.reason = WritableSpanRejectReason::region_limit;
            if (output != nullptr) {
                *output = result;
            }
            return false;
        }

        MEMORY_BASIC_INFORMATION memory{};
        const SIZE_T queried = VirtualQuery(
            reinterpret_cast<const void*>(cursor), &memory,
            sizeof(memory));
        result.memory = memory;
        const auto region_begin =
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const bool region_end_valid =
            memory.RegionSize <=
            std::numeric_limits<std::uintptr_t>::max() - region_begin;
        const std::uintptr_t region_end = region_end_valid
            ? region_begin + memory.RegionSize
            : region_begin;
        if (queried != sizeof(memory) || !region_end_valid ||
            region_begin > cursor || cursor >= region_end) {
            result.reason = WritableSpanRejectReason::query_or_gap;
        } else if (memory.State != MEM_COMMIT) {
            result.reason = WritableSpanRejectReason::not_committed;
        } else if (
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            result.reason = WritableSpanRejectReason::guarded_or_noaccess;
        } else if (!protection_is_readable(memory.Protect)) {
            result.reason = WritableSpanRejectReason::not_readable;
        } else if (!protection_is_writable(memory.Protect)) {
            result.reason = WritableSpanRejectReason::not_writable;
        }
        if (result.reason != WritableSpanRejectReason::none) {
            if (output != nullptr) {
                *output = result;
            }
            return false;
        }

        ++result.regions;
        cursor = region_end < end ? region_end : end;
    }

    result.cursor = end;
    if (output != nullptr) {
        *output = result;
    }
    return true;
}

template <typename T>
T read_value(const std::byte* const base, const std::size_t offset) noexcept {
    T result{};
    std::memcpy(&result, base + offset, sizeof(result));
    return result;
}

template <typename T>
void write_value(
    std::byte* const base,
    const std::size_t offset,
    const T& value) noexcept {
    std::memcpy(base + offset, &value, sizeof(value));
}

bool apply_stereo_emissive_scissor(
    const void* const view,
    const void* const state) noexcept {
    if (g_diagnostic_eye_phase == 0 ||
        !accessible_range(
            view, kScissorViewportOffset + sizeof(LONG) * 4u) ||
        !accessible_range(
            state, kRendererDeviceOffset + sizeof(void*))) {
        return false;
    }

    const auto* const view_bytes =
        static_cast<const std::byte*>(view);
    const LONG x = read_value<LONG>(view_bytes, kScissorViewportOffset);
    const LONG y = read_value<LONG>(
        view_bytes, kScissorViewportOffset + sizeof(LONG));
    const LONG width = read_value<LONG>(
        view_bytes, kScissorViewportOffset + sizeof(LONG) * 2u);
    const LONG height = read_value<LONG>(
        view_bytes, kScissorViewportOffset + sizeof(LONG) * 3u);
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        width > std::numeric_limits<LONG>::max() - x ||
        height > std::numeric_limits<LONG>::max() - y) {
        return false;
    }

    const auto* const state_bytes =
        static_cast<const std::byte*>(state);
    IDirect3DDevice9* const device = read_value<IDirect3DDevice9*>(
        state_bytes, kRendererDeviceOffset);
    if (device == nullptr) {
        return false;
    }

    const RECT scissor = {x, y, x + width, y + height};
    const HRESULT enable_result = device->SetRenderState(
        D3DRS_SCISSORTESTENABLE, TRUE);
    if (FAILED(enable_result)) {
        WAWVR_STEREO_DIAG_ONCE(
            "Stereo emissive scissor repair could not enable scissoring: phase=%u hr=0x%08lX",
            g_diagnostic_eye_phase,
            static_cast<unsigned long>(enable_result));
        return false;
    }
    const HRESULT rectangle_result = device->SetScissorRect(&scissor);
    if (FAILED(rectangle_result)) {
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        WAWVR_STEREO_DIAG_ONCE(
            "Stereo emissive scissor repair could not set the eye rectangle: phase=%u hr=0x%08lX",
            g_diagnostic_eye_phase,
            static_cast<unsigned long>(rectangle_result));
        return false;
    }
    return true;
}

void __cdecl stereo_emissive_scissor_thunk(
    const void* const view,
    void* const source,
    void* const state,
    const std::uintptr_t unused_first,
    const std::uintptr_t unused_second) noexcept {
    const auto original_address =
        g_original_standard_emissive_callback.load(std::memory_order_acquire);
    if (original_address == 0) {
        return;
    }
    if (g_diagnostic_eye_phase != 0 &&
        !apply_stereo_emissive_scissor(view, state)) {
        WAWVR_STEREO_DIAG_ONCE(
            "Stereo emissive scissor repair rejected an invalid eye/state record: phase=%u view=%p state=%p",
            g_diagnostic_eye_phase, view, state);
    }

    const auto original = reinterpret_cast<StandardEmissiveCallback>(
        original_address);
    original(
        view, source, state, unused_first, unused_second);
}

bool bytes_equal(
    const std::uintptr_t address,
    const std::uint8_t* const expected,
    const std::size_t size) noexcept {
    const auto* const current =
        reinterpret_cast<const std::uint8_t*>(address);
    return accessible_range(current, size) &&
           std::memcmp(current, expected, size) == 0;
}

bool decoded_call_targets(
    const std::uintptr_t address,
    const std::uintptr_t target) noexcept {
    std::int32_t displacement = 0;
    std::memcpy(
        &displacement, reinterpret_cast<const void*>(address + 1),
        sizeof(displacement));
    const auto decoded = static_cast<std::uintptr_t>(
        static_cast<std::uint32_t>(address + kCallSize + displacement));
    return decoded == target;
}

template <std::size_t Size>
bool sentinel_equal(const ExactSentinel<Size>& sentinel) noexcept {
    return bytes_equal(
        sentinel.address, sentinel.expected.data(), sentinel.expected.size());
}

bool verify_layout_sentinels(
    const StereoBackendExecutableProfile& profile) noexcept {
    return sentinel_equal(profile.standard_emissive_callback_tail) &&
           sentinel_equal(profile.view_stride) &&
           sentinel_equal(profile.split_lit_primary) &&
           sentinel_equal(profile.split_lit_secondary) &&
           sentinel_equal(profile.lit_list) &&
           sentinel_equal(profile.emissive_list) &&
           sentinel_equal(profile.decal_list) &&
           sentinel_equal(profile.list_metadata) &&
           sentinel_equal(profile.point_light_base) &&
           sentinel_equal(profile.point_light_info) &&
           sentinel_equal(profile.point_light_stride) &&
           sentinel_equal(profile.point_light_count) &&
           sentinel_equal(profile.emissive_spot_layout);
}

bool verify_original_profile() noexcept {
    const auto* const profile = configured_backend_profile();
    if (profile == nullptr) {
        return false;
    }
    const auto sites = patch_sites(*profile);
    for (const auto& site : sites) {
        if (!bytes_equal(
                site.address, site.original.data(), site.original.size())) {
            return false;
        }
    }
    const auto callback_site = emissive_callback_patch_site(*profile);
    if (!bytes_equal(
            callback_site.address, callback_site.original.data(),
            callback_site.original.size())) {
        return false;
    }
    return decoded_call_targets(
               profile->synchronous_draw_call_address,
               profile->draw_3d_internal_address) &&
           decoded_call_targets(
               profile->smp_draw_call_address,
               profile->draw_3d_internal_address) &&
           decoded_call_targets(
               profile->setup_clear_call_address,
               profile->clear_screen_address) &&
           decoded_call_targets(
               profile->scene_clear_call_address,
               profile->clear_screen_address) &&
           bytes_equal(
               profile->draw_3d_internal_address,
               profile->expected_draw_3d_internal_entry.data(),
               profile->expected_draw_3d_internal_entry.size()) &&
           bytes_equal(
               profile->clear_screen_address,
               profile->expected_clear_screen_entry.data(),
               profile->expected_clear_screen_entry.size()) &&
           bytes_equal(
               profile->standard_emissive_callback_address,
               profile->expected_standard_emissive_callback_entry.data(),
               profile->expected_standard_emissive_callback_entry.size()) &&
           verify_layout_sentinels(*profile);
}

bool verify_replacement_ownership() noexcept {
    const auto* const profile = configured_backend_profile();
    if (profile == nullptr) {
        return false;
    }
    const auto sites = patch_sites(*profile);
    for (const auto& site : sites) {
        const auto replacement =
            relative_call(site.address, site.replacement_target);
        if (!bytes_equal(
                site.address, replacement.data(), replacement.size())) {
            return false;
        }
    }
    const auto callback_site = emissive_callback_patch_site(*profile);
    const auto callback_replacement =
        absolute_push(callback_site.replacement_target);
    return bytes_equal(
               callback_site.address, callback_replacement.data(),
               callback_replacement.size()) &&
           verify_layout_sentinels(*profile) &&
           bytes_equal(
               profile->draw_3d_internal_address,
               profile->expected_draw_3d_internal_entry.data(),
               profile->expected_draw_3d_internal_entry.size()) &&
           bytes_equal(
               profile->clear_screen_address,
               profile->expected_clear_screen_entry.data(),
               profile->expected_clear_screen_entry.size()) &&
           bytes_equal(
               profile->standard_emissive_callback_address,
               profile->expected_standard_emissive_callback_entry.data(),
               profile->expected_standard_emissive_callback_entry.size());
}

[[nodiscard]] bool suspend_backend_patch_threads(
    SuspendedPeerThreads* const suspended) noexcept {
    const auto* const profile = configured_backend_profile();
    if (suspended == nullptr || profile == nullptr) {
        return false;
    }
    const auto sites = patch_sites(*profile);
    std::array<PeerThreadPatchRange, 5> patch_ranges{};
    for (std::size_t index = 0; index < sites.size(); ++index) {
        patch_ranges[index] = {sites[index].address, kCallSize};
    }
    patch_ranges.back() = {
        profile->standard_emissive_callback_push_address, kCallSize};
    return suspended->suspend(patch_ranges);
}

class WritablePatchPages final {
public:
    bool Acquire() noexcept {
        const auto* const profile = configured_backend_profile();
        if (profile == nullptr) {
            return false;
        }
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const auto page_size = static_cast<std::uintptr_t>(info.dwPageSize);
        if (page_size == 0) {
            return false;
        }
        pages_[0].address = reinterpret_cast<void*>(
            profile->synchronous_draw_call_address & ~(page_size - 1u));
        pages_[1].address = reinterpret_cast<void*>(
            profile->setup_clear_call_address & ~(page_size - 1u));
        pages_[2].address = reinterpret_cast<void*>(
            profile->standard_emissive_callback_push_address &
            ~(page_size - 1u));
        for (auto& page : pages_) {
            if (!VirtualProtect(
                    page.address, page_size, PAGE_EXECUTE_READWRITE,
                    &page.old_protection)) {
                Restore(page_size);
                return false;
            }
            ++acquired_;
        }
        page_size_ = page_size;
        return true;
    }

    ~WritablePatchPages() { Restore(page_size_); }

private:
    struct Page final {
        void* address{};
        DWORD old_protection{};
    };

    void Restore(const std::uintptr_t page_size) noexcept {
        while (acquired_ != 0) {
            auto& page = pages_[--acquired_];
            DWORD ignored = 0;
            VirtualProtect(
                page.address, page_size, page.old_protection, &ignored);
        }
    }

    std::array<Page, 3> pages_{};
    std::size_t acquired_{};
    std::uintptr_t page_size_{};
};

bool write_all_patches(const bool install) noexcept {
    const auto* const profile = configured_backend_profile();
    if (profile == nullptr) {
        return false;
    }
    const auto sites = patch_sites(*profile);
    const auto callback_site = emissive_callback_patch_site(*profile);
    const auto callback_replacement =
        absolute_push(callback_site.replacement_target);
    WritablePatchPages pages;
    if (!pages.Acquire()) {
        return false;
    }
    for (const auto& site : sites) {
        const auto replacement =
            relative_call(site.address, site.replacement_target);
        const auto& expected = install ? site.original : replacement;
        if (std::memcmp(
                reinterpret_cast<const void*>(site.address),
                expected.data(), expected.size()) != 0) {
            return false;
        }
    }
    const auto& expected_callback =
        install ? callback_site.original : callback_replacement;
    if (std::memcmp(
            reinterpret_cast<const void*>(callback_site.address),
            expected_callback.data(), expected_callback.size()) != 0) {
        return false;
    }
    for (const auto& site : sites) {
        const auto replacement =
            relative_call(site.address, site.replacement_target);
        const auto& desired = install ? replacement : site.original;
        std::memcpy(
            reinterpret_cast<void*>(site.address), desired.data(),
            desired.size());
    }
    const auto& desired_callback =
        install ? callback_replacement : callback_site.original;
    std::memcpy(
        reinterpret_cast<void*>(callback_site.address),
        desired_callback.data(), desired_callback.size());
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

const char* validate_draw_surf_span(
    const void* const draw_surfs,
    const std::uint32_t count) noexcept {
    if (count > kMaximumDrawSurfCount) {
        return "count-over-cap";
    }
    if (count != 0 && draw_surfs == nullptr) {
        return "nonzero-count-null-surfaces";
    }
    if (count != 0 &&
        count > std::numeric_limits<std::size_t>::max() / kDrawSurfSize) {
        return "surface-byte-overflow";
    }
    if (count != 0 &&
        !accessible_range(
            draw_surfs, static_cast<std::size_t>(count) * kDrawSurfSize)) {
        return "surface-range-unreadable";
    }
    return nullptr;
}

const char* validate_draw_list_reference(
    const std::byte* const list,
    const std::byte* const expected_owner) noexcept {
    if (read_value<const void*>(list, kDrawListViewInfoOffset) !=
        expected_owner) {
        return "owner-mismatch";
    }
    return validate_draw_surf_span(
        read_value<const void*>(list, 0),
        read_value<std::uint32_t>(list, sizeof(void*)));
}

void rebase_draw_list_reference(
    std::byte* const first_list,
    const std::byte* const final_list,
    const std::byte* const first_view) noexcept {
    // T4's scalar fields below are the non-owning draw-list references and
    // metadata that remain valid only in the final generated view. Preserve
    // the earlier eye's opaque +0x10 field and all owning renderer structures.
    write_value(
        first_list, 0, read_value<const void*>(final_list, 0));
    write_value(
        first_list, sizeof(void*),
        read_value<std::uint32_t>(final_list, sizeof(void*)));
    write_value(
        first_list, 0x08,
        read_value<std::uint32_t>(final_list, 0x08));
    const void* const first_owner = first_view;
    write_value(first_list, kDrawListViewInfoOffset, first_owner);
    std::memcpy(
        first_list + kDrawListViewOriginOffset,
        first_view + kViewOriginOffset, sizeof(float) * 4u);
    write_value(
        first_list, kDrawListLightOffset,
        read_value<const void*>(final_list, kDrawListLightOffset));
    write_value(
        first_list, kDrawListCameraViewOffset,
        read_value<std::uint32_t>(
            final_list, kDrawListCameraViewOffset));
}

void log_fx_state_change(
    const std::byte* const final,
    const std::uint32_t point_light_count,
    const std::uint32_t emissive_spot_light_count) noexcept {
    // This is deliberately a startup-only diagnostic switch. The state can
    // change several times per second during authored effects; keeping that
    // telemetry enabled in ordinary play adds render-thread file I/O and can
    // itself create the frame-time spikes it is meant to investigate.
    static const bool diagnostics_enabled = []() noexcept {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(
                   L"WAWVR_FX_STEREO_DIAGNOSTICS", value, 2) == 1 &&
            value[0] == L'1';
    }();
    if (!diagnostics_enabled) {
        return;
    }

    const std::uint32_t emissive_draw_surf_count =
        read_value<std::uint32_t>(
            final + kCoreDrawListOffsets[3], sizeof(void*));
    const std::uint32_t fullscreen_flag =
        read_value<std::uint32_t>(final, kIsRenderingFullScreenOffset);
    const std::uint8_t needs_float_z =
        read_value<std::uint8_t>(final, kNeedsFloatZOffset);
    auto& previous = g_fx_state_telemetry;
    const bool changed = !previous.initialized ||
        previous.emissive_draw_surf_count != emissive_draw_surf_count ||
        previous.point_light_count != point_light_count ||
        previous.emissive_spot_light_count !=
            emissive_spot_light_count ||
        previous.fullscreen_flag != fullscreen_flag ||
        previous.needs_float_z != needs_float_z;
    if (!changed) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    const bool auxiliary_light_activated =
        (point_light_count != 0 && previous.point_light_count == 0) ||
        (emissive_spot_light_count != 0 &&
         previous.emissive_spot_light_count == 0);
    if (previous.initialized && !auxiliary_light_activated &&
        now < previous.next_log_milliseconds) {
        return;
    }

    stereo_diagnostic_log(
        "StereoDiag backend.fx-state: emissiveDrawSurfCount=%u pointLightCount=%u emissiveSpotLightCount=%u needsFloatZ=%u isRenderingFullScreen=%u",
        emissive_draw_surf_count, point_light_count,
        emissive_spot_light_count,
        static_cast<unsigned int>(needs_float_z), fullscreen_flag);
    previous.initialized = true;
    previous.emissive_draw_surf_count = emissive_draw_surf_count;
    previous.point_light_count = point_light_count;
    previous.emissive_spot_light_count = emissive_spot_light_count;
    previous.fullscreen_flag = fullscreen_flag;
    previous.needs_float_z = needs_float_z;
    previous.next_log_milliseconds = now + 200;
}

bool validate_and_rebase_draw_lists(
    std::byte* const first,
    const std::byte* const final) noexcept {
    for (std::size_t list_index = 0;
         list_index < kCoreDrawListOffsets.size(); ++list_index) {
        const std::size_t offset = kCoreDrawListOffsets[list_index];
        const auto* const final_list = final + offset;
        const auto final_owner = read_value<const void*>(
            final_list, kDrawListViewInfoOffset);
        const std::uint32_t count =
            read_value<std::uint32_t>(final_list, sizeof(void*));
        const auto draw_surfs = read_value<const void*>(final_list, 0);
        const char* const reason =
            validate_draw_list_reference(final_list, final);
        if (reason != nullptr) {
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag backend.list rejected: ordinal=%zu offset=+0x%zX reason=%s count=%u owner=%p expectedOwner=%p surfaces=%p",
                list_index, offset, reason, count, final_owner, final,
                draw_surfs);
            return false;
        }
    }

    const std::uint32_t point_light_count =
        read_value<std::uint32_t>(final, kPointLightCountOffset);
    if (point_light_count > kMaximumPointLightPartitions) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.aux-list rejected: pointLightCount=%u cap=%u final=%p",
            point_light_count, kMaximumPointLightPartitions, final);
        return false;
    }
    for (std::uint32_t point_light_index = 0;
         point_light_index < point_light_count; ++point_light_index) {
        const std::size_t partition_offset =
            kPointLightPartitionOffset +
            static_cast<std::size_t>(point_light_index) *
                kPointLightPartitionStride;
        const auto* const final_info =
            final + partition_offset + kPointLightInfoOffset;
        const auto final_owner = read_value<const void*>(
            final_info, kDrawListViewInfoOffset);
        const auto final_light = read_value<const void*>(
            final_info, kDrawListLightOffset);
        const void* const expected_light = final + partition_offset;
        const std::uint32_t count = read_value<std::uint32_t>(
            final_info, sizeof(void*));
        const auto draw_surfs = read_value<const void*>(final_info, 0);
        const char* reason =
            validate_draw_list_reference(final_info, final);
        if (reason == nullptr && final_light != expected_light) {
            reason = "light-owner-mismatch";
        }
        if (reason != nullptr) {
            WAWVR_STEREO_DIAG_ONCE(
                "StereoDiag backend.point-light rejected: ordinal=%u info=+0x%zX reason=%s count=%u owner=%p expectedOwner=%p light=%p expectedLight=%p surfaces=%p",
                point_light_index,
                partition_offset + kPointLightInfoOffset, reason, count,
                final_owner, final, final_light, expected_light, draw_surfs);
            return false;
        }
    }

    const std::uint32_t emissive_spot_light_count =
        read_value<std::uint32_t>(final, kEmissiveSpotLightCountOffset);
    if (emissive_spot_light_count > 1u) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.emissive-spot rejected: lightCount=%u cap=1 final=%p",
            emissive_spot_light_count, final);
        return false;
    }
    const std::uint32_t emissive_spot_draw_surf_count =
        emissive_spot_light_count != 0u
            ? read_value<std::uint32_t>(
                  final, kEmissiveSpotDrawSurfCountOffset)
            : 0u;
    const void* const emissive_spot_draw_surfs =
        emissive_spot_light_count != 0u
            ? read_value<const void*>(
                  final, kEmissiveSpotDrawSurfsOffset)
            : nullptr;
    if (const char* const reason = validate_draw_surf_span(
            emissive_spot_draw_surfs,
            emissive_spot_draw_surf_count);
        reason != nullptr) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.emissive-spot rejected: reason=%s lightCount=%u drawSurfCount=%u surfaces=%p final=%p",
            reason, emissive_spot_light_count,
            emissive_spot_draw_surf_count, emissive_spot_draw_surfs, final);
        return false;
    }

    log_fx_state_change(
        final, point_light_count, emissive_spot_light_count);

    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.lists accepted: first=%p final=%p counts=[%u,%u,%u,%u,%u] surfaces=[%p,%p,%p,%p,%p]",
        first, final,
        read_value<std::uint32_t>(final + kCoreDrawListOffsets[0], sizeof(void*)),
        read_value<std::uint32_t>(final + kCoreDrawListOffsets[1], sizeof(void*)),
        read_value<std::uint32_t>(final + kCoreDrawListOffsets[2], sizeof(void*)),
        read_value<std::uint32_t>(final + kCoreDrawListOffsets[3], sizeof(void*)),
        read_value<std::uint32_t>(final + kCoreDrawListOffsets[4], sizeof(void*)),
        read_value<const void*>(final + kCoreDrawListOffsets[0], 0),
        read_value<const void*>(final + kCoreDrawListOffsets[1], 0),
        read_value<const void*>(final + kCoreDrawListOffsets[2], 0),
        read_value<const void*>(final + kCoreDrawListOffsets[3], 0),
        read_value<const void*>(final + kCoreDrawListOffsets[4], 0));
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.aux-lists accepted: first=%p final=%p pointLightCount=%u emissiveSpotLightCount=%u emissiveSpotDrawSurfCount=%u emissiveSpotDrawSurfs=%p",
        first, final, point_light_count, emissive_spot_light_count,
        emissive_spot_draw_surf_count, emissive_spot_draw_surfs);

    for (const std::size_t offset : kCoreDrawListOffsets) {
        rebase_draw_list_reference(
            first + offset, final + offset, first);
    }
    for (std::uint32_t point_light_index = 0;
         point_light_index < point_light_count; ++point_light_index) {
        const std::size_t info_offset =
            kPointLightPartitionOffset +
            static_cast<std::size_t>(point_light_index) *
                kPointLightPartitionStride +
            kPointLightInfoOffset;
        rebase_draw_list_reference(
            first + info_offset, final + info_offset, first);
    }
    write_value(first, kPointLightCountOffset, point_light_count);
    write_value(
        first, kEmissiveSpotLightIndexOffset,
        read_value<std::uint32_t>(final, kEmissiveSpotLightIndexOffset));
    write_value(
        first, kEmissiveSpotLightCountOffset,
        emissive_spot_light_count);
    write_value(
        first, kEmissiveSpotDrawSurfCountOffset,
        emissive_spot_draw_surf_count);
    write_value(
        first, kEmissiveSpotDrawSurfsOffset,
        emissive_spot_draw_surfs);
    write_value(
        first, kNeedsFloatZOffset,
        read_value<std::uint8_t>(final, kNeedsFloatZOffset));

    if (point_light_count != 0u || emissive_spot_light_count != 0u) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.active-aux-lighting rebased: pointLightCount=%u emissiveSpotLightCount=%u emissiveSpotDrawSurfCount=%u",
            point_light_count, emissive_spot_light_count,
            emissive_spot_draw_surf_count);
    }
    return true;
}

class BackendViewIndexRestore final {
public:
    BackendViewIndexRestore(
        std::byte* const data,
        const std::uint32_t saved) noexcept
        : data_(data), saved_(saved) {}
    ~BackendViewIndexRestore() {
        write_value(data_, kViewInfoIndexOffset, saved_);
        g_suppress_secondary_full_clear = false;
        g_diagnostic_eye_phase = 0;
    }

private:
    std::byte* data_{};
    std::uint32_t saved_{};
};

void __cdecl stereo_backend_draw_thunk(
    const void* const selected_view) noexcept {
    const auto* const profile = configured_backend_profile();
    if (profile == nullptr) {
        return;
    }
    const auto original = reinterpret_cast<Draw3DInternalFunction>(
        profile->draw_3d_internal_address);

    std::uint64_t frame_id = 0;
    wawvr::xr::StereoSourceLayout layout{};
    if (!stereo_backend_hook_ready()) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.confirm rejected: hook installed=%u replacement/profile ownership not ready",
            g_installed.load(std::memory_order_acquire) ? 1u : 0u);
        original(selected_view);
        return;
    }
    if (!try_get_staged_stereo_frame(&frame_id, &layout)) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.confirm rejected: no staged broker frame selectedView=%p",
            selected_view);
        original(selected_view);
        return;
    }
    if (!accessible_range(
            reinterpret_cast<const void*>(
                profile->back_end_data_pointer_address),
            sizeof(void*))) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.confirm rejected: stagedFrame=%llu backEndData global %p unreadable selectedView=%p",
            static_cast<unsigned long long>(frame_id),
            reinterpret_cast<const void*>(
                profile->back_end_data_pointer_address),
            selected_view);
        original(selected_view);
        return;
    }

    std::byte* data = nullptr;
    std::memcpy(
        &data,
        reinterpret_cast<const void*>(
            profile->back_end_data_pointer_address),
        sizeof(data));
    WritableSpanValidation data_span{};
    if (!accessible_writable_span(
            data, kViewInfoPointerOffset + sizeof(void*), &data_span)) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.data-span rejected: stagedFrame=%llu begin=%p size=0x%zX reason=%s acceptedRegions=%zu cursor=%p BaseAddress=%p AllocationBase=%p RegionSize=0x%zX State=0x%08lX Protect=0x%08lX",
            static_cast<unsigned long long>(frame_id), data,
            kViewInfoPointerOffset + sizeof(void*),
            describe_span_rejection(data_span.reason), data_span.regions,
            reinterpret_cast<const void*>(data_span.cursor),
            data_span.memory.BaseAddress, data_span.memory.AllocationBase,
            static_cast<std::size_t>(data_span.memory.RegionSize),
            static_cast<unsigned long>(data_span.memory.State),
            static_cast<unsigned long>(data_span.memory.Protect));
        original(selected_view);
        return;
    }
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.data-span accepted: stagedFrame=%llu begin=%p size=0x%zX regions=%zu",
        static_cast<unsigned long long>(frame_id), data,
        kViewInfoPointerOffset + sizeof(void*), data_span.regions);
    const std::uint32_t count =
        read_value<std::uint32_t>(data, kViewInfoCountOffset);
    if (count != 2u) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.confirm rejected: stagedFrame=%llu data=%p backendCount=%u expected=2 backendIndex=%u views=%p selectedView=%p",
            static_cast<unsigned long long>(frame_id), data, count,
            read_value<std::uint32_t>(data, kViewInfoIndexOffset),
            read_value<void*>(data, kViewInfoPointerOffset), selected_view);
        original(selected_view);
        return;
    }

    const std::uint32_t saved_index =
        read_value<std::uint32_t>(data, kViewInfoIndexOffset);
    std::byte* views = read_value<std::byte*>(data, kViewInfoPointerOffset);
    if (saved_index != 1u) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.confirm rejected: stagedFrame=%llu data=%p backendCount=2 backendIndex=%u expected=1 views=%p selectedView=%p",
            static_cast<unsigned long long>(frame_id), data, saved_index,
            views, selected_view);
        original(selected_view);
        return;
    }
    WritableSpanValidation views_span{};
    if (!accessible_writable_span(
            views, kViewInfoStride * 2u, &views_span)) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.views-span rejected: stagedFrame=%llu begin=%p size=0x%zX reason=%s acceptedRegions=%zu cursor=%p BaseAddress=%p AllocationBase=%p RegionSize=0x%zX State=0x%08lX Protect=0x%08lX",
            static_cast<unsigned long long>(frame_id), views,
            kViewInfoStride * 2u,
            describe_span_rejection(views_span.reason), views_span.regions,
            reinterpret_cast<const void*>(views_span.cursor),
            views_span.memory.BaseAddress, views_span.memory.AllocationBase,
            static_cast<std::size_t>(views_span.memory.RegionSize),
            static_cast<unsigned long>(views_span.memory.State),
            static_cast<unsigned long>(views_span.memory.Protect));
        original(selected_view);
        return;
    }
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.views-span accepted: stagedFrame=%llu begin=%p size=0x%zX regions=%zu",
        static_cast<unsigned long long>(frame_id), views,
        kViewInfoStride * 2u, views_span.regions);
    std::byte* const first = views;
    std::byte* const final = views + kViewInfoStride;
    if (selected_view != final) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.confirm rejected: stagedFrame=%llu selectedView=%p expectedFinal=%p first=%p data=%p count=2 index=1",
            static_cast<unsigned long long>(frame_id), selected_view, final,
            first, data);
        original(selected_view);
        return;
    }
    if (!validate_and_rebase_draw_lists(first, final)) {
        WAWVR_STEREO_DIAG_ONCE(
            "StereoDiag backend.confirm rejected: stagedFrame=%llu core/auxiliary draw-list validation/rebase failed first=%p final=%p",
            static_cast<unsigned long long>(frame_id), first, final);
        original(selected_view);
        return;
    }

    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.dual-eye accepted: frame=%llu data=%p views=%p selected=%p",
        static_cast<unsigned long long>(frame_id), data, views, selected_view);
    BackendViewIndexRestore restore(data, saved_index);
    write_value(data, kViewInfoIndexOffset, std::uint32_t{0});
    g_suppress_secondary_full_clear = false;
    g_diagnostic_primary_clear_calls = 0;
    g_diagnostic_eye_phase = 1;
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.eye0 call begin: frame=%llu view=%p index=0 clearSuppress=0",
        static_cast<unsigned long long>(frame_id), first);
    original(first);
    g_diagnostic_eye_phase = 0;
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.eye0 call complete: frame=%llu view=%p clearCalls=%u",
        static_cast<unsigned long long>(frame_id), first,
        g_diagnostic_primary_clear_calls);

    write_value(data, kViewInfoIndexOffset, std::uint32_t{1});
    g_suppress_secondary_full_clear = true;
    g_diagnostic_secondary_clear_calls = 0;
    g_diagnostic_eye_phase = 2;
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.eye1 call begin: frame=%llu view=%p index=1 clearSuppress=1",
        static_cast<unsigned long long>(frame_id), final);
    original(final);
    g_diagnostic_eye_phase = 0;
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.eye1 call complete: frame=%llu view=%p suppressedClearCalls=%u",
        static_cast<unsigned long long>(frame_id), final,
        g_diagnostic_secondary_clear_calls);
    g_suppress_secondary_full_clear = false;

    mark_stereo_frame_rendered(frame_id, layout);
    WAWVR_STEREO_DIAG_ONCE(
        "StereoDiag backend.confirm mark requested: frame=%llu both eye calls returned",
        static_cast<unsigned long long>(frame_id));
}

bool __cdecl should_suppress_secondary_clear() noexcept {
    if (g_diagnostic_eye_phase == 1u) {
        ++g_diagnostic_primary_clear_calls;
    } else if (g_diagnostic_eye_phase == 2u) {
        ++g_diagnostic_secondary_clear_calls;
    }
    return g_suppress_secondary_full_clear;
}

} // namespace

StereoBackendHookResult install_stereo_backend_hook() noexcept {
    const auto* const profile = configured_backend_profile();
    if (profile == nullptr) {
        return StereoBackendHookResult::profile_mismatch;
    }
    if (g_installed.load(std::memory_order_acquire)) {
        return stereo_backend_hook_ready()
                   ? StereoBackendHookResult::already_installed
                   : StereoBackendHookResult::profile_mismatch;
    }
    if (!verify_original_profile()) {
        return StereoBackendHookResult::profile_mismatch;
    }
    g_original_standard_emissive_callback.store(
        profile->standard_emissive_callback_address,
        std::memory_order_release);
    SuspendedPeerThreads suspended;
    if (!suspend_backend_patch_threads(&suspended)) {
        g_original_standard_emissive_callback.store(
            0, std::memory_order_release);
        return StereoBackendHookResult::thread_suspend_failed;
    }
    if (!verify_original_profile()) {
        g_original_standard_emissive_callback.store(
            0, std::memory_order_release);
        return StereoBackendHookResult::profile_mismatch;
    }
    if (!write_all_patches(true)) {
        g_original_standard_emissive_callback.store(
            0, std::memory_order_release);
        return StereoBackendHookResult::patch_write_failed;
    }
    g_installed.store(true, std::memory_order_release);
    return StereoBackendHookResult::installed;
}

StereoBackendHookResult restore_stereo_backend_hook() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return StereoBackendHookResult::already_installed;
    }
    SuspendedPeerThreads suspended;
    if (!suspend_backend_patch_threads(&suspended)) {
        return StereoBackendHookResult::thread_suspend_failed;
    }
    if (!verify_replacement_ownership()) {
        return StereoBackendHookResult::profile_mismatch;
    }
    if (!write_all_patches(false)) {
        return StereoBackendHookResult::patch_write_failed;
    }
    g_installed.store(false, std::memory_order_release);
    g_suppress_secondary_full_clear = false;
    return StereoBackendHookResult::installed;
}

bool stereo_backend_hook_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

bool stereo_backend_hook_ready() noexcept {
    return g_installed.load(std::memory_order_acquire) &&
           verify_replacement_ownership();
}

} // namespace wawvr::mod
