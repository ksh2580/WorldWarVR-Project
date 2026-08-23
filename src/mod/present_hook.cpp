#include "present_hook.hpp"

#include "controller_state.hpp"
#include "menu_button_gesture.hpp"
#include "menu_panel_logic.hpp"
#include "menu_surface_logic.hpp"
#include "present_hook_logic.hpp"
#include "stereo_diagnostics.hpp"
#include "stereo_frame_broker.hpp"
#include "stereo_scene_hook.hpp"
#include "t4_layout_selector.hpp"
#include "tracking_anchor_sync.hpp"
#if defined(WAWVR_HAS_T4_BINDINGS)
#include "t4_menu_input.hpp"
#include "t4_presentation_state.hpp"
#endif

#include "d3d11_compositor.h"
#include "d3d9_cpu_capture.h"
#include "openxr_runtime.h"

#include <d3d9.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>

namespace wawvr::mod {
namespace {

// Clean-room facts for the exact, non-ASLR SP/MP executables.
// run_present_hook_monitor is entered only after the complete file and mapped
// image profile has passed src/t4 validation and selected one of these layouts.
struct PresentExecutableProfile final {
    const char* name{};
    std::uintptr_t d3d9_device_pointer_address{};
    std::uintptr_t target_window_index_address{};
    std::uintptr_t window_count_address{};
    std::uintptr_t window_zero_swap_chain_address{};
    std::uintptr_t rb_swap_buffers_address{};
    std::array<std::uint8_t, 40> rb_swap_buffers_sentinel{};
};

constexpr PresentExecutableProfile kSpPresentProfile{
    .name = "T4 SP 1.7.1263",
    .d3d9_device_pointer_address = 0x03BF3B08u,
    .target_window_index_address = 0x03BF6774u,
    .window_count_address = 0x03BF6778u,
    .window_zero_swap_chain_address = 0x03BF6780u,
    .rb_swap_buffers_address = 0x006FBE50u,
    .rb_swap_buffers_sentinel = {
        0x51, 0xA1, 0x74, 0x67, 0xBF, 0x03, 0x56, 0x57,
        0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0xC1, 0xE0,
        0x04, 0x8B, 0x80, 0x80, 0x67, 0xBF, 0x03, 0x8B,
        0x08, 0x8B, 0x51, 0x0C, 0x6A, 0x00, 0x6A, 0x00,
        0x50, 0xFF, 0xD2, 0x3D, 0x68, 0x08, 0x76, 0x88,
    },
};

constexpr PresentExecutableProfile kMpPresentProfile{
    .name = "T4 MP 1.7.1263",
    .d3d9_device_pointer_address = 0x1087DD08u,
    .target_window_index_address = 0x10880974u,
    .window_count_address = 0x10880978u,
    .window_zero_swap_chain_address = 0x10880980u,
    .rb_swap_buffers_address = 0x006D6F90u,
    .rb_swap_buffers_sentinel = {
        0x51, 0xA1, 0x74, 0x09, 0x88, 0x10, 0x56, 0x57,
        0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0xC1, 0xE0,
        0x04, 0x8B, 0x80, 0x80, 0x09, 0x88, 0x10, 0x8B,
        0x08, 0x8B, 0x51, 0x0C, 0x6A, 0x00, 0x6A, 0x00,
        0x50, 0xFF, 0xD2, 0x3D, 0x68, 0x08, 0x76, 0x88,
    },
};

constexpr int kRendererLayoutUnconfigured = -1;
constexpr int kRendererLayoutSinglePlayer = 0;
constexpr int kRendererLayoutMultiplayer = 1;
std::atomic<int> g_renderer_layout{kRendererLayoutUnconfigured};

[[nodiscard]] constexpr int renderer_layout_code(
    const wawvr::t4::ExecutableLayoutId layout) noexcept {
    switch (t4_layout_family_from_id(layout)) {
    case T4LayoutFamily::single_player_1_7_1263:
        return kRendererLayoutSinglePlayer;
    case T4LayoutFamily::multiplayer_1_7_1263:
        return kRendererLayoutMultiplayer;
    case T4LayoutFamily::unsupported:
        return kRendererLayoutUnconfigured;
    }
    return kRendererLayoutUnconfigured;
}

[[nodiscard]] const PresentExecutableProfile*
configured_present_profile() noexcept {
    wawvr::t4::ExecutableLayoutId layout{};
    if (!get_configured_renderer_layout(&layout)) {
        return nullptr;
    }
    switch (t4_layout_family_from_id(layout)) {
    case T4LayoutFamily::single_player_1_7_1263:
        return &kSpPresentProfile;
    case T4LayoutFamily::multiplayer_1_7_1263:
        return &kMpPresentProfile;
    case T4LayoutFamily::unsupported:
        return nullptr;
    }
    return nullptr;
}

constexpr std::size_t kT4WindowStride = 0x10u;
constexpr std::uint32_t kMaximumSaneWindowCount = 8;
constexpr DWORD kTargetPollMilliseconds = 50;
constexpr ULONGLONG kInstallRetryMilliseconds = 2'000;
constexpr ULONGLONG kRuntimeRetryMilliseconds = 10'000;
constexpr ULONGLONG kRecoveryTransitionLogMilliseconds = 5'000;

std::atomic<bool> g_headset_unavailable_notice_shown{false};

DWORD WINAPI show_headset_unavailable_notice(void*) noexcept {
    MessageBoxW(
        nullptr,
        L"World War VR could not find a PC VR headset through the active "
        L"OpenXR runtime.\n\n"
        L"For Meta Quest 2/3, wake the headset, connect with Quest Link or "
        L"Air Link, enter the PC VR environment, and make Meta Quest Link "
        L"the active OpenXR runtime. If you use SteamVR or Virtual Desktop, "
        L"start SteamVR and make it the active OpenXR runtime.\n\n"
        L"The game will keep checking every 10 seconds, so VR can recover "
        L"after the headset becomes available. Otherwise, close the game, "
        L"correct the PC VR connection, and launch again.\n\n"
        L"If this continues, send WorldWarVR.log to the mod author.",
        L"World War VR - Headset not available",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    return 0;
}

void notify_headset_unavailable_once() noexcept {
    bool expected = false;
    if (!g_headset_unavailable_notice_shown.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    const HANDLE thread = CreateThread(
        nullptr, 0, &show_headset_unavailable_notice, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
}

[[nodiscard]] ActiveUiMonoSource configured_active_ui_source() noexcept {
    std::array<wchar_t, 16> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"WAWVR_MENU_SOURCE", value.data(),
        static_cast<DWORD>(value.size()));
    if (length != 0 && length < value.size()) {
        if (_wcsicmp(value.data(), L"left") == 0) {
            return ActiveUiMonoSource::left_eye;
        }
        if (_wcsicmp(value.data(), L"right") == 0) {
            return ActiveUiMonoSource::right_eye;
        }
        if (_wcsicmp(value.data(), L"full") == 0) {
            return ActiveUiMonoSource::full_frame;
        }
    }
    // Exact T4 CL_CGameRendering queues its stock UI after both packed scene
    // eyes without a per-eye scrPlace remap. Preserve that complete
    // backbuffer by default. Explicit left/right values remain probe aids.
    return ActiveUiMonoSource::full_frame;
}

[[nodiscard]] const char* active_ui_source_name(
    const ActiveUiMonoSource source) noexcept {
    switch (source) {
    case ActiveUiMonoSource::left_eye: return "left";
    case ActiveUiMonoSource::right_eye: return "right";
    case ActiveUiMonoSource::full_frame: return "full";
    }
    return "unknown";
}

[[nodiscard]] const char* presentation_mode_name(
    const PresentationMode mode) noexcept {
    switch (mode) {
    case PresentationMode::stereo: return "gameplay-stereo";
    case PresentationMode::full_frame_mono: return "full-frame-mono";
    case PresentationMode::active_ui_mono: return "active-ui-mono";
    case PresentationMode::active_console_mono: return "active-console-mono";
    }
    return "unknown";
}

[[nodiscard]] const char* composition_layer_kind_name(
    const wawvr::xr::CompositionLayerKind kind) noexcept {
    switch (kind) {
    case wawvr::xr::CompositionLayerKind::none: return "none";
    case wawvr::xr::CompositionLayerKind::projection: return "projection";
    case wawvr::xr::CompositionLayerKind::quad: return "quad";
    }
    return "unknown";
}

using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SwapChainPresentFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

constexpr std::size_t kMaximumProcessLifetimeD3DThunkRoutes = 32;
ImmutableThunkRouteTable<
    IDirect3DSwapChain9*, SwapChainPresentFunction,
    kMaximumProcessLifetimeD3DThunkRoutes>
    g_swap_present_thunk_routes;
ImmutableThunkRouteTable<
    void**, SwapChainPresentFunction,
    kMaximumProcessLifetimeD3DThunkRoutes>
    g_swap_present_vtable_routes;
ImmutableThunkRouteTable<
    IDirect3DDevice9*, ResetFunction,
    kMaximumProcessLifetimeD3DThunkRoutes>
    g_reset_thunk_routes;
ImmutableThunkRouteTable<
    void**, ResetFunction,
    kMaximumProcessLifetimeD3DThunkRoutes>
    g_reset_vtable_routes;

[[nodiscard]] bool route_registration_usable(
    const ImmutableThunkRouteRegistration result) noexcept {
    return result == ImmutableThunkRouteRegistration::inserted ||
           result == ImmutableThunkRouteRegistration::already_registered;
}

[[nodiscard]] const char* route_registration_name(
    const ImmutableThunkRouteRegistration result) noexcept {
    switch (result) {
    case ImmutableThunkRouteRegistration::inserted:
        return "inserted";
    case ImmutableThunkRouteRegistration::already_registered:
        return "already registered";
    case ImmutableThunkRouteRegistration::conflicting_callback:
        return "conflicting callback";
    case ImmutableThunkRouteRegistration::capacity_exhausted:
        return "capacity exhausted";
    case ImmutableThunkRouteRegistration::invalid:
        return "invalid";
    }
    return "unknown";
}

std::atomic<bool> g_shutdown_requested{false};
std::atomic<bool> g_present_hook_installed{false};
std::atomic<bool> g_mono_xr_ready{false};
std::atomic<HMODULE> g_log_module{nullptr};
std::mutex g_log_mutex;
std::ofstream g_log_stream;
std::filesystem::path g_log_stream_path;
std::chrono::steady_clock::time_point g_log_last_flush{};

const char* level_name(const wawvr::xr::LogLevel level) noexcept {
    switch (level) {
    case wawvr::xr::LogLevel::Debug:
        return "debug";
    case wawvr::xr::LogLevel::Info:
        return "info";
    case wawvr::xr::LogLevel::Warning:
        return "warning";
    case wawvr::xr::LogLevel::Error:
        return "error";
    }
    return "unknown";
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &value);
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

std::filesystem::path module_log_path(const HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return std::filesystem::current_path() / "WorldWarVR.log";
    }
    return std::filesystem::path(path.data(), path.data() + length)
               .parent_path() /
           "WorldWarVR.log";
}

void append_log(
    const HMODULE module,
    const wawvr::xr::LogLevel level,
    const std::string_view message) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        const std::filesystem::path path = module_log_path(module);
        if (!g_log_stream.is_open() || g_log_stream_path != path) {
            if (g_log_stream.is_open()) {
                g_log_stream.flush();
                g_log_stream.close();
            }
            g_log_stream.clear();
            g_log_stream.open(path, std::ios::app);
            g_log_stream_path = path;
            g_log_last_flush = {};
        }
        if (g_log_stream) {
            g_log_stream << '[' << timestamp() << "] [" << level_name(level)
                         << "] " << message << '\n';
            const auto now = std::chrono::steady_clock::now();
            const bool urgent =
                level == wawvr::xr::LogLevel::Warning ||
                level == wawvr::xr::LogLevel::Error;
            if (urgent || g_log_last_flush.time_since_epoch().count() == 0 ||
                now - g_log_last_flush >= std::chrono::seconds(1)) {
                g_log_stream.flush();
                g_log_last_flush = now;
            }
        }
        std::string debugger_line = "WorldAtWarVR [";
        debugger_line += level_name(level);
        debugger_line += "]: ";
        debugger_line.append(message.data(), message.size());
        debugger_line += '\n';
        OutputDebugStringA(debugger_line.c_str());
    } catch (...) {
        OutputDebugStringA("WorldAtWarVR: renderer logging failed\n");
    }
}

bool protection_is_readable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffu;
    return access == PAGE_READONLY || access == PAGE_READWRITE ||
           access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

bool protection_is_executable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffu;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

bool readable_range(const void* const address, const std::size_t size) noexcept {
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
    const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto region_end = region_begin + memory.RegionSize;
    return begin >= region_begin && begin <= region_end &&
           size <= region_end - begin;
}

bool executable_pointer(const void* const address) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    return address != nullptr &&
           VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) &&
           memory.State == MEM_COMMIT &&
           (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
           protection_is_executable(memory.Protect);
}

template <typename T>
bool read_absolute(const std::uintptr_t address, T* const value) noexcept {
    const auto* const source = reinterpret_cast<const T*>(address);
    if (value == nullptr || !readable_range(source, sizeof(T))) {
        return false;
    }
    *value = *source;
    return true;
}

void* read_vtable_entry(
    void* const object,
    const std::size_t index,
    void*** const table_output = nullptr) noexcept {
    if (!readable_range(object, sizeof(void*))) {
        return nullptr;
    }
    auto** const table = *reinterpret_cast<void***>(object);
    if (!readable_range(table + index, sizeof(void*))) {
        return nullptr;
    }
    if (table_output != nullptr) {
        *table_output = table;
    }
    return table[index];
}

[[nodiscard]] SwapChainPresentFunction lookup_swap_present_route(
    IDirect3DSwapChain9* const swap_chain) noexcept {
    void** table = nullptr;
    (void)read_vtable_entry(
        swap_chain, kDirect3DSwapChain9PresentIndex, &table);
    return lookup_immutable_thunk_route(
        g_swap_present_thunk_routes, swap_chain,
        g_swap_present_vtable_routes, table);
}

[[nodiscard]] ResetFunction lookup_reset_route(
    IDirect3DDevice9* const device) noexcept {
    void** table = nullptr;
    (void)read_vtable_entry(device, kDirect3DDevice9ResetIndex, &table);
    return lookup_immutable_thunk_route(
        g_reset_thunk_routes, device, g_reset_vtable_routes, table);
}

bool verify_rb_swap_buffers_sentinel(
    const PresentExecutableProfile& profile) noexcept {
    const auto* const code = reinterpret_cast<const std::uint8_t*>(
        profile.rb_swap_buffers_address);
    return readable_range(code, profile.rb_swap_buffers_sentinel.size()) &&
           std::memcmp(
               code, profile.rb_swap_buffers_sentinel.data(),
               profile.rb_swap_buffers_sentinel.size()) == 0;
}

struct T4PresentTarget final {
    IDirect3DDevice9* device{};
    IDirect3DSwapChain9* swap_chain{};
    std::int32_t target_window_index{};
    std::uint32_t window_count{};
};

bool read_t4_present_target(
    const PresentExecutableProfile& profile,
    T4PresentTarget* const target) noexcept {
    if (target == nullptr) {
        return false;
    }
    *target = {};
    if (!read_absolute(
            profile.d3d9_device_pointer_address, &target->device) ||
        !read_absolute(
            profile.target_window_index_address,
            &target->target_window_index) ||
        !read_absolute(profile.window_count_address, &target->window_count) ||
        target->device == nullptr || target->target_window_index < 0 ||
        target->window_count == 0 ||
        target->window_count > kMaximumSaneWindowCount ||
        static_cast<std::uint32_t>(target->target_window_index) >=
            target->window_count) {
        return false;
    }

    // D3D9CpuCapture currently reads implicit swapchain zero through the
    // device. Fail closed if T4 ever selects a different output window.
    if (target->target_window_index != 0) {
        return false;
    }
    const auto swap_chain_slot =
        profile.window_zero_swap_chain_address +
        static_cast<std::uintptr_t>(target->target_window_index) *
            kT4WindowStride;
    return read_absolute(swap_chain_slot, &target->swap_chain) &&
           target->swap_chain != nullptr;
}

bool environment_disables_present_hook() noexcept {
    std::array<wchar_t, 16> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"WAWVR_DISABLE_XR", value.data(), static_cast<DWORD>(value.size()));
    return length != 0 && length < value.size() &&
           (value[0] == L'1' || value[0] == L'y' || value[0] == L'Y' ||
            value[0] == L't' || value[0] == L'T');
}

bool environment_disables_eye_local_bloom() noexcept {
    static const bool disabled = []() noexcept {
        std::array<wchar_t, 16> value{};
        const DWORD length = GetEnvironmentVariableW(
            L"WAWVR_DISABLE_EYE_LOCAL_BLOOM", value.data(),
            static_cast<DWORD>(value.size()));
        return length != 0 && length < value.size() &&
               (value[0] == L'1' || value[0] == L'y' ||
                value[0] == L'Y' || value[0] == L't' ||
                value[0] == L'T');
    }();
    return disabled;
}

class VtableSlotPatch final {
public:
    bool Install(
        void* const object,
        const std::size_t index,
        void* const expected,
        void* const replacement) noexcept {
        HookSlotPlan plan{};
        void** table = nullptr;
        if (!build_hook_slot_plan(expected, replacement, &plan) ||
            !executable_pointer(expected) || !executable_pointer(replacement) ||
            read_vtable_entry(object, index, &table) != expected) {
            return false;
        }

        slot_ = table + index;
        original_ = plan.original;
        replacement_ = plan.replacement;
        DWORD old_protection = 0;
        if (!VirtualProtect(
                slot_, sizeof(void*), PAGE_EXECUTE_READWRITE,
                &old_protection)) {
            Clear();
            return false;
        }
        auto* const slot = reinterpret_cast<PVOID volatile*>(slot_);
        void* const observed = InterlockedCompareExchangePointer(
            slot, replacement_, original_);
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(void*), old_protection, &ignored);
        if (observed != original_) {
            Clear();
            return false;
        }
        installed_ = true;
        FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(void*));
        return true;
    }

    VtableOwnership Restore() noexcept {
        if (!installed_ || slot_ == nullptr ||
            !readable_range(slot_, sizeof(void*))) {
            return VtableOwnership::foreign;
        }
        const auto ownership = classify_hook_slot_ownership(
            *slot_, original_, replacement_);
        if (ownership != VtableOwnership::installed_by_us) {
            installed_ = false;
            return ownership;
        }

        DWORD old_protection = 0;
        if (!VirtualProtect(
                slot_, sizeof(void*), PAGE_EXECUTE_READWRITE,
                &old_protection)) {
            return VtableOwnership::foreign;
        }
        auto* const slot = reinterpret_cast<PVOID volatile*>(slot_);
        void* const observed = InterlockedCompareExchangePointer(
            slot, original_, replacement_);
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(void*), old_protection, &ignored);
        if (observed != replacement_) {
            return VtableOwnership::foreign;
        }
        installed_ = false;
        FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(void*));
        return VtableOwnership::original;
    }

private:
    void Clear() noexcept {
        slot_ = nullptr;
        original_ = nullptr;
        replacement_ = nullptr;
        installed_ = false;
    }

    void** slot_{};
    void* original_{};
    void* replacement_{};
    bool installed_ = false;
};

class PresentHookController final {
public:
    explicit PresentHookController(const HMODULE module) noexcept : module_(module) {
        // The controller is process-lifetime storage. Publish it once and keep
        // it stable across target generations; retired thunk routes remain
        // independently callable even after final shutdown clears this pointer.
        instance_.store(this, std::memory_order_release);
    }

    bool Install(const T4PresentTarget& target) noexcept {
        if (target_swap_chain_.load(std::memory_order_acquire) != nullptr) {
            return false;
        }
        T4PresentTarget current_target{};
        if (!ReadValidatedTargetMatching(target, &current_target)) {
            Log(wawvr::xr::LogLevel::Warning,
                "D3D9 target changed before hook installation; renderer monitor will retry");
            return false;
        }
        void** swap_vtable = nullptr;
        void** reset_vtable = nullptr;
        const auto swap_present = read_vtable_entry(
            current_target.swap_chain, kDirect3DSwapChain9PresentIndex,
            &swap_vtable);
        const auto reset = read_vtable_entry(
            current_target.device, kDirect3DDevice9ResetIndex,
            &reset_vtable);
        HookSlotPlan swap_plan{};
        if (!build_hook_slot_plan(
                swap_present, reinterpret_cast<void*>(&SwapPresentThunk),
                &swap_plan) ||
            !executable_pointer(swap_present)) {
            Log(wawvr::xr::LogLevel::Warning,
                "T4 swap-chain Present slot failed executable-pointer validation");
            return false;
        }

        LogFormatted(
            wawvr::xr::LogLevel::Info,
            "Validated RB_SwapBuffers target: index=%ld windowCount=%lu device=%p swapChain=%p Present=%p",
            static_cast<long>(current_target.target_window_index),
            static_cast<unsigned long>(current_target.window_count),
            static_cast<void*>(current_target.device),
            static_cast<void*>(current_target.swap_chain), swap_present);

        const auto present_vtable_route =
            g_swap_present_vtable_routes.Register(
                swap_vtable,
                reinterpret_cast<SwapChainPresentFunction>(swap_present));
        const auto present_route = g_swap_present_thunk_routes.Register(
            current_target.swap_chain,
            reinterpret_cast<SwapChainPresentFunction>(swap_present));
        if (!route_registration_usable(present_vtable_route)) {
            LogFormatted(
                wawvr::xr::LogLevel::Warning,
                "Swap-chain Present thunk route rejected (vtable=%s object=%s); target left unpatched",
                route_registration_name(present_vtable_route),
                route_registration_name(present_route));
            return false;
        }
        if (!route_registration_usable(present_route)) {
            LogFormatted(
                wawvr::xr::LogLevel::Info,
                "Swap-chain object route unavailable (%s); the validated shared-vtable route remains active",
                route_registration_name(present_route));
        }

        if (!swap_present_patch_.Install(
                current_target.swap_chain, kDirect3DSwapChain9PresentIndex,
                swap_present, reinterpret_cast<void*>(&SwapPresentThunk))) {
            Log(wawvr::xr::LogLevel::Warning,
                "Swap-chain Present slot changed during installation; hook skipped");
            return false;
        }
        target_device_.store(current_target.device, std::memory_order_release);
        target_swap_chain_.store(
            current_target.swap_chain, std::memory_order_release);
        g_present_hook_installed.store(true, std::memory_order_release);
        Log(wawvr::xr::LogLevel::Info,
            "Exact T4 swap-chain Present hook installed (RB_SwapBuffers vtable +0x0C)");

        HookSlotPlan reset_plan{};
        if (build_hook_slot_plan(
                reset, reinterpret_cast<void*>(&ResetThunk), &reset_plan) &&
            executable_pointer(reset)) {
            const auto reset_vtable_route = g_reset_vtable_routes.Register(
                reset_vtable, reinterpret_cast<ResetFunction>(reset));
            const auto reset_route = g_reset_thunk_routes.Register(
                current_target.device,
                reinterpret_cast<ResetFunction>(reset));
            if (!route_registration_usable(reset_vtable_route)) {
                LogFormatted(
                    wawvr::xr::LogLevel::Warning,
                    "D3D9 Reset thunk route rejected (vtable=%s object=%s); Present hook remains active",
                    route_registration_name(reset_vtable_route),
                    route_registration_name(reset_route));
            } else {
                if (!route_registration_usable(reset_route)) {
                    LogFormatted(
                        wawvr::xr::LogLevel::Info,
                        "D3D9 Reset object route unavailable (%s); the validated shared-vtable route remains active",
                        route_registration_name(reset_route));
                }
                if (device_reset_patch_.Install(
                        current_target.device, kDirect3DDevice9ResetIndex,
                        reset, reinterpret_cast<void*>(&ResetThunk))) {
                    reset_hook_installed_.store(true, std::memory_order_release);
                    Log(wawvr::xr::LogLevel::Info,
                        "D3D9 Reset slot hook installed without changing the device vptr");
                } else {
                    Log(wawvr::xr::LogLevel::Warning,
                        "D3D9 Reset slot changed during installation; Present hook remains active");
                }
            }
        } else {
            Log(wawvr::xr::LogLevel::Warning,
                "D3D9 Reset slot failed validation; Present hook remains active");
        }
        // Scene/backend installation is intentionally deferred to the first
        // post-Com_Frame service while render_mutex_ is held. Publishing the
        // Present thunk must not race a second installer on the game thread.
        if (!ReadValidatedTargetMatching(current_target, nullptr) ||
            g_shutdown_requested.load(std::memory_order_acquire)) {
            PauseFrameServiceForTargetRefresh();
            RestoreD3DSlotsForRebind();
            Log(wawvr::xr::LogLevel::Warning,
                "D3D9 target changed during hook installation; new slots restored and retry scheduled");
            return false;
        }
        target_refresh_requested_.store(false, std::memory_order_release);
        frame_service_ready_.store(true, std::memory_order_release);
        return true;
    }

    bool Rebind(const T4PresentTarget& target) noexcept {
        IDirect3DDevice9* const previous_device =
            target_device_.load(std::memory_order_acquire);
        IDirect3DSwapChain9* const previous_swap_chain =
            target_swap_chain_.load(std::memory_order_acquire);
        if (previous_device == target.device &&
            previous_swap_chain == target.swap_chain) {
            ResumeFrameServiceForCurrentTarget();
            return true;
        }

        PauseFrameServiceForTargetRefresh();
        bool reset_was_idle = false;
        if (!reset_in_progress_.compare_exchange_strong(
                reset_was_idle, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            Log(wawvr::xr::LogLevel::Info,
                "Renderer target rebind deferred until the active D3D9 Reset completes");
            return false;
        }
        LogFormatted(
            wawvr::xr::LogLevel::Info,
            "Validated T4 D3D target replacement: oldDevice=%p oldSwapChain=%p newDevice=%p newSwapChain=%p; rebinding renderer hooks",
            static_cast<void*>(previous_device),
            static_cast<void*>(previous_swap_chain),
            static_cast<void*>(target.device),
            static_cast<void*>(target.swap_chain));

        try {
            std::lock_guard<std::mutex> lock(render_mutex_);
            ShutdownXr("validated T4 D3D target recreation");
            RestoreD3DSlotsForRebind();
            ResetBoundaryObservations();
            next_runtime_attempt_ = 0;
        } catch (...) {
            reset_in_progress_.store(false, std::memory_order_release);
            Log(wawvr::xr::LogLevel::Error,
                "Renderer rebind could not acquire its state lock; replacement target was not patched");
            return false;
        }

        T4PresentTarget refreshed_target{};
        const bool target_remains_current =
            ReadValidatedTargetMatching(target, &refreshed_target);
        if (!target_remains_current) {
            reset_in_progress_.store(false, std::memory_order_release);
            frame_service_ready_.store(false, std::memory_order_release);
            target_refresh_requested_.store(true, std::memory_order_release);
            Log(wawvr::xr::LogLevel::Warning,
                "Validated T4 D3D target changed during XR teardown; stale replacement was not patched and monitor will retry");
            return false;
        }

        const bool installed = Install(refreshed_target);
        reset_in_progress_.store(false, std::memory_order_release);
        if (installed) {
            target_refresh_requested_.store(false, std::memory_order_release);
            Log(wawvr::xr::LogLevel::Info,
                "Replacement D3D9 target rebound; OpenXR restart scheduled at the next exact post-Com_Frame boundary");
        } else {
            frame_service_ready_.store(false, std::memory_order_release);
            target_refresh_requested_.store(true, std::memory_order_release);
            Log(wawvr::xr::LogLevel::Warning,
                "Replacement D3D9 target was not yet safe to hook; renderer monitor will retry");
        }
        return installed;
    }

    void PauseFrameServiceForTargetRefresh() noexcept {
        frame_service_ready_.store(false, std::memory_order_release);
        if (!target_refresh_requested_.exchange(
                true, std::memory_order_acq_rel)) {
            Log(wawvr::xr::LogLevel::Info,
                "T4 D3D target is changing; XR frame service paused until a validated target is available");
        }
    }

    void ResumeFrameServiceForCurrentTarget() noexcept {
        if (target_swap_chain_.load(std::memory_order_acquire) == nullptr ||
            reset_in_progress_.load(std::memory_order_acquire)) {
            return;
        }
        if (target_refresh_requested_.exchange(
                false, std::memory_order_acq_rel)) {
            Log(wawvr::xr::LogLevel::Info,
                "Validated T4 D3D target remained current; XR frame service resumed");
        }
        frame_service_ready_.store(true, std::memory_order_release);
    }

    void ShutdownRenderState() noexcept {
        frame_service_ready_.store(false, std::memory_order_release);
        try {
            std::lock_guard<std::mutex> lock(render_mutex_);
            ShutdownXr("cooperative renderer shutdown");
        } catch (...) {
            Log(wawvr::xr::LogLevel::Error,
                "Renderer shutdown could not acquire its state lock; hooks will still be restored fail-closed");
        }
    }

    void RestoreSlots() noexcept {
        frame_service_ready_.store(false, std::memory_order_release);
        clear_pending_stereo_frame();
        clear_controller_frame();
        if (stereo_scene_hook_installed()) {
            const auto result = restore_stereo_scene_hook();
            if (result == StereoSceneHookResult::installed) {
                Log(wawvr::xr::LogLevel::Info,
                    "T4 gameplay R_RenderScene call site restored");
            } else {
                LogFormatted(
                    wawvr::xr::LogLevel::Warning,
                    "T4 gameplay scene hook restore failed closed: %s",
                    describe_stereo_scene_hook_result(result));
            }
        }
        RestoreD3DSlotsForRebind();
        PresentHookController* expected = this;
        instance_.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    [[nodiscard]] IDirect3DDevice9* target_device() const noexcept {
        return target_device_.load(std::memory_order_acquire);
    }

    [[nodiscard]] IDirect3DSwapChain9* target_swap_chain() const noexcept {
        return target_swap_chain_.load(std::memory_order_acquire);
    }

    static void ServicePublishedAfterComFrame() noexcept {
        auto* const instance = instance_.load(std::memory_order_acquire);
        if (instance == nullptr ||
            !instance->frame_service_ready_.load(std::memory_order_acquire) ||
            g_shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lock(instance->render_mutex_);
            if (g_shutdown_requested.load(std::memory_order_acquire) ||
                !instance->frame_service_ready_.load(
                    std::memory_order_acquire)) {
                return;
            }
            instance->ServiceAfterComFrame();
        } catch (...) {
            instance->Log(
                wawvr::xr::LogLevel::Error,
                "Unhandled exception at post-Com_Frame XR boundary; XR path stopped safely");
            try {
                std::lock_guard<std::mutex> lock(instance->render_mutex_);
                instance->ShutdownXr("post-Com_Frame exception");
            } catch (...) {
                instance->frame_service_ready_.store(
                    false, std::memory_order_release);
                g_shutdown_requested.store(true, std::memory_order_release);
                instance->Log(
                    wawvr::xr::LogLevel::Error,
                    "Post-Com_Frame exception cleanup could not acquire its state lock; renderer shutdown requested");
            }
        }
    }

private:
    static std::atomic<PresentHookController*> instance_;

    static HRESULT STDMETHODCALLTYPE SwapPresentThunk(
        IDirect3DSwapChain9* const swap_chain,
        const RECT* const source_rect,
        const RECT* const destination_rect,
        const HWND destination_window,
        const RGNDATA* const dirty_region,
        const DWORD flags) noexcept {
        auto* const instance = instance_.load(std::memory_order_acquire);
        if (instance == nullptr) {
            const auto original = lookup_swap_present_route(swap_chain);
            return original != nullptr
                       ? original(
                             swap_chain, source_rect, destination_rect,
                             destination_window, dirty_region, flags)
                       : D3DERR_INVALIDCALL;
        }
        return instance->OnSwapPresent(
            swap_chain, source_rect, destination_rect, destination_window,
            dirty_region, flags);
    }

    static HRESULT STDMETHODCALLTYPE ResetThunk(
        IDirect3DDevice9* const device,
        D3DPRESENT_PARAMETERS* const parameters) noexcept {
        auto* const instance = instance_.load(std::memory_order_acquire);
        if (instance == nullptr) {
            const auto original = lookup_reset_route(device);
            return original != nullptr
                       ? original(device, parameters)
                       : D3DERR_INVALIDCALL;
        }
        return instance->OnReset(device, parameters);
    }

    HRESULT OnSwapPresent(
        IDirect3DSwapChain9* const swap_chain,
        const RECT* const source_rect,
        const RECT* const destination_rect,
        const HWND destination_window,
        const RGNDATA* const dirty_region,
        const DWORD flags) noexcept {
        const auto original = lookup_swap_present_route(swap_chain);
        if (original == nullptr) {
            return D3DERR_INVALIDCALL;
        }

        static thread_local bool inside_hook = false;
        bool owns_present =
            !inside_hook &&
            swap_chain == target_swap_chain_.load(std::memory_order_acquire) &&
            frame_service_ready_.load(std::memory_order_acquire) &&
            !reset_in_progress_.load(std::memory_order_acquire) &&
            !g_shutdown_requested.load(std::memory_order_acquire);
        const bool entered_hook = owns_present;
        if (owns_present) {
            inside_hook = true;
            try {
                std::lock_guard<std::mutex> lock(render_mutex_);
                owns_present =
                    swap_chain ==
                        target_swap_chain_.load(std::memory_order_acquire) &&
                    frame_service_ready_.load(std::memory_order_acquire) &&
                    !reset_in_progress_.load(std::memory_order_acquire) &&
                    !g_shutdown_requested.load(std::memory_order_acquire);
                if (owns_present) {
                    CaptureVrFrameBeforePresent(
                        target_device_.load(std::memory_order_acquire));
                }
            } catch (...) {
                Log(wawvr::xr::LogLevel::Error,
                    "Unhandled exception during pre-Present capture; desktop Present preserved");
            }
        }

        const HRESULT result = original(
            swap_chain, source_rect, destination_rect, destination_window,
            dirty_region, flags);

        if (entered_hook) {
            try {
                std::lock_guard<std::mutex> lock(render_mutex_);
                if (owns_present &&
                    swap_chain ==
                        target_swap_chain_.load(std::memory_order_acquire) &&
                    !reset_in_progress_.load(std::memory_order_acquire)) {
                    RecordPresentResult(result);
                }
            } catch (...) {
                Log(wawvr::xr::LogLevel::Error,
                    "Unhandled exception recording D3D9 Present result");
            }
            inside_hook = false;
        }
        return result;
    }

    HRESULT OnReset(
        IDirect3DDevice9* const device,
        D3DPRESENT_PARAMETERS* const parameters) noexcept {
        const auto original = lookup_reset_route(device);
        if (original == nullptr) {
            return D3DERR_INVALIDCALL;
        }
        if (device != target_device_.load(std::memory_order_acquire) ||
            reset_in_progress_.exchange(true, std::memory_order_acq_rel)) {
            return original(device, parameters);
        }

        try {
            std::lock_guard<std::mutex> lock(render_mutex_);
            device_loss_seen_since_boundary_ = true;
            reset_seen_since_boundary_ = true;
            clear_pending_stereo_frame();
            clear_controller_frame();
            capture_.NotifyExternalDeviceLoss();
            Log(wawvr::xr::LogLevel::Info,
                "D3D9 Reset entered: capture paused; pending OpenXR frame retained for post-Com_Frame service");
        } catch (...) {
            Log(wawvr::xr::LogLevel::Error,
                "Unhandled exception while preparing for D3D9 Reset; native Reset preserved");
        }
        const HRESULT result = original(device, parameters);
        try {
            std::lock_guard<std::mutex> lock(render_mutex_);
            latest_reset_result_ = result;
            if (SUCCEEDED(result)) {
                Log(wawvr::xr::LogLevel::Info,
                    "D3D9 Reset completed; post-Com_Frame service will preserve or re-prime XR after the engine frame unwinds");
            } else {
                LogFormatted(
                    wawvr::xr::LogLevel::Warning,
                    "D3D9 Reset returned 0x%08lx; capture remains paused while T4 retries desktop recovery",
                    result);
            }
        } catch (...) {
            Log(wawvr::xr::LogLevel::Error,
                "Unhandled exception while recording D3D9 Reset result");
        }
        reset_in_progress_.store(false, std::memory_order_release);
        return result;
    }

    struct PreparedPendingFrame final {
        bool active{};
        bool presentation_observed{};
        std::uint64_t observed_frame_id{};
        wawvr::xr::FrameState frame{};
        wawvr::xr::StereoSourceLayout layout{};
        PresentationMode presentation_mode{PresentationMode::stereo};
        PresentationMode observed_presentation_mode{PresentationMode::stereo};
        bool t4_state_valid{};
        std::int32_t connection_state{};
        std::uint32_t key_catchers{};
        bool stereo_scene{};
        wawvr::xr::D3D9CpuCaptureResult capture_result{
            wawvr::xr::D3D9CpuCaptureResult::unavailable};
    };

    void CaptureVrFrameBeforePresent(IDirect3DDevice9* const device) {
        if (device == nullptr || !runtime_.initialized() ||
            !pending_frame_active_) {
            return;
        }

        // The gameplay scene hook used this frame's predicted pose/FOV while
        // T4 built the current backbuffer. Copy the legacy D3D9 image before
        // Present. This callback deliberately performs no D3D11 or OpenXR
        // work; the validated post-Com_Frame callsite owns submission/pacing.
        wawvr::xr::StereoSourceLayout layout{};
        const bool stereo_scene = try_get_rendered_stereo_layout(
            pending_frame_.frame_id, &layout);
        bool t4_state_valid = false;
        std::int32_t connection_state = 0;
        std::int32_t active_connection_state = 10;
        std::uint32_t key_catchers = 0;
#if defined(WAWVR_HAS_T4_BINDINGS)
        const T4PresentationState t4_state = read_t4_presentation_state();
        t4_state_valid = t4_state.valid;
        connection_state = t4_state.connection_state;
        active_connection_state = t4_state.active_connection_state;
        key_catchers = t4_state.key_catchers;
#endif
        const PresentationMode presentation_mode =
            classify_presentation_mode(
                t4_state_valid, connection_state, key_catchers,
                stereo_scene, active_connection_state);
        if (!presentation_state_log_valid_ ||
            t4_state_valid != last_logged_t4_state_valid_ ||
            connection_state != last_logged_connection_state_ ||
            active_connection_state !=
                last_logged_active_connection_state_ ||
            key_catchers != last_logged_key_catchers_ ||
            presentation_mode != last_logged_presentation_mode_) {
            LogFormatted(
                wawvr::xr::LogLevel::Info,
                "T4 presentation transition: stateValid=%u connection=%d active=%d keyCatchers=0x%X stereoScene=%u mode=%s",
                t4_state_valid ? 1u : 0u, connection_state,
                active_connection_state, key_catchers,
                stereo_scene ? 1u : 0u,
                presentation_mode_name(presentation_mode));
            presentation_state_log_valid_ = true;
            last_logged_t4_state_valid_ = t4_state_valid;
            last_logged_connection_state_ = connection_state;
            last_logged_active_connection_state_ =
                active_connection_state;
            last_logged_key_catchers_ = key_catchers;
            last_logged_presentation_mode_ = presentation_mode;
        }
        prepared_frame_.presentation_observed = true;
        prepared_frame_.observed_frame_id = pending_frame_.frame_id;
        prepared_frame_.observed_presentation_mode = presentation_mode;
        if (presentation_mode != PresentationMode::stereo) {
            layout = presentation_layout(
                presentation_mode, active_ui_source_);
        }
        if (pending_frame_.should_render && pending_frame_.views_valid &&
            (stereo_scene ||
             presentation_mode != PresentationMode::stereo)) {
            const wawvr::xr::D3D9CpuCaptureResult capture_result =
                capture_.CaptureBackBuffer(device, &pending_frame_);
            if (capture_result ==
                wawvr::xr::D3D9CpuCaptureResult::captured) {
                // Keep the newest successful target-swapchain image until the
                // main-loop boundary consumes it. This also lets frontend,
                // loading, and cinematic frames replace the prior projection.
                prepared_frame_.active = true;
                prepared_frame_.frame = pending_frame_;
                prepared_frame_.layout = layout;
                prepared_frame_.presentation_mode = presentation_mode;
                prepared_frame_.t4_state_valid = t4_state_valid;
                prepared_frame_.connection_state = connection_state;
                prepared_frame_.key_catchers = key_catchers;
                prepared_frame_.stereo_scene = stereo_scene;
                prepared_frame_.capture_result = capture_result;
                if (!logged_active_capture_path_) {
                    Log(wawvr::xr::LogLevel::Info,
                        "Active capture path: synchronous D3D9 CPU readback/upload fallback (the validated T4 host does not own a D3D9Ex/CreateDeviceEx and ResetEx shared-resource lifecycle)");
                    logged_active_capture_path_ = true;
                }
            } else if (capture_result ==
                       wawvr::xr::D3D9CpuCaptureResult::device_lost) {
                device_loss_seen_since_boundary_ = true;
            }
        } else if (pending_frame_.should_render && pending_frame_.views_valid &&
                   !stereo_scene &&
                   presentation_mode == PresentationMode::stereo &&
                   !logged_missing_stereo_backend_) {
            Log(wawvr::xr::LogLevel::Warning,
                "Skipped fresh headset projection because the exact T4 backend did not confirm both eye draws; the last valid projection will be retained when available");
            logged_missing_stereo_backend_ = true;
        }
    }

    bool FinishPendingFrameAfterComFrame(
        const bool allow_fresh_projection,
        const bool d3d9_device_lost,
        const ULONGLONG now) {
        if (!pending_frame_active_) {
            prepared_frame_ = {};
            return runtime_.initialized();
        }
        const PreparedPendingFrame prepared = prepared_frame_;
        prepared_frame_ = {};
        // The OpenXR frame belongs to the post-Com_Frame boundary regardless
        // of whether this engine frame reached a capturable Present.
        const wawvr::xr::FrameState frame = pending_frame_;
        if (prepared.presentation_observed &&
            prepared.observed_frame_id == frame.frame_id) {
            const bool reset_comfort_anchor =
                presentation_transition_resets_comfort_anchor(
                    last_observed_presentation_mode_valid_,
                    last_observed_presentation_mode_,
                    prepared.observed_presentation_mode);
            if (reset_comfort_anchor) {
                const bool entering_mono =
                    prepared.observed_presentation_mode !=
                    PresentationMode::stereo;
                if (last_observed_presentation_mode_valid_) {
                    Log(wawvr::xr::LogLevel::Info,
                        entering_mono
                            ? "Observed stereo-to-mono transition; next finite panel capture will latch a fresh room anchor"
                            : "Observed mono-to-stereo transition; retired the finite panel anchor independently of capture availability");
                }
                comfort_pose_ = {};
                comfort_pose_valid_ = false;
                comfort_quad_ = {};
                comfort_quad_valid_ = false;

                // A projection and a finite quad are not interchangeable
                // recovery layers. Reusing the prior stereo projection after
                // entering a menu recreates a translation-infinite,
                // head-following screen; reusing a menu quad after gameplay
                // resumes leaves stale UI in the room. Retire the complete
                // prior interval and allow this frame to publish only a layer
                // constructed for the newly observed presentation class.
                last_projection_available_ = false;
                last_projection_eyes_ = {};
                last_quad_layer_ = {};
                last_submission_was_quad_ = false;
                visible_menu_quad_ = {};
                visible_menu_quad_valid_ = false;
#if defined(WAWVR_HAS_T4_BINDINGS)
                last_menu_surface_ = {};
                last_menu_surface_valid_ = false;
                visible_menu_surface_ = {};
                visible_menu_surface_valid_ = false;
#endif
            }
            last_observed_presentation_mode_ =
                prepared.observed_presentation_mode;
            last_observed_presentation_mode_valid_ = true;
        }
        wawvr::xr::D3D11SourceFrame source{};
        bool rendered = false;
        std::uint32_t released_eye_mask = 0;
        wawvr::xr::QuadLayer current_quad{};
        bool current_quad_valid = false;
        wawvr::xr::CompositorReticle current_reticle{};
#if defined(WAWVR_HAS_T4_BINDINGS)
        MenuPointerSurface current_menu_surface{};
        bool current_menu_surface_valid = false;
#endif
        if (prepared.active &&
            prepared.frame.frame_id == frame.frame_id &&
            allow_fresh_projection &&
            prepared.capture_result ==
                wawvr::xr::D3D9CpuCaptureResult::captured &&
            capture_.UploadLatest(
                runtime_.d3d11_device(), runtime_.d3d11_context(), &source)) {
            last_source_width_ = source.width;
            last_source_height_ = source.height;
            if (prepared.presentation_mode != PresentationMode::stereo) {
                if (!comfort_pose_valid_) {
                    // Keep menu/cinematic screens upright even if the HMD is
                    // briefly rolled or resting at an angle on entry. Yaw and
                    // position still determine where the screen is anchored.
                    comfort_pose_ = leveled_tracking_anchor(
                        frame.head_center,
                        tracking_anchor_valid_
                            ? &tracking_anchor_.orientation
                            : nullptr);
                    comfort_pose_valid_ = true;
                    comfort_quad_valid_ = build_world_menu_panel(
                        comfort_pose_, &comfort_quad_);
                    if (comfort_quad_valid_) {
                        LogFormatted(
                            wawvr::xr::LogLevel::Info,
                            "Gravity-level room-anchored VR panel latched on entry: mode=%s UI-source=%s distance=%.2fm size=%.3fx%.3fm",
                            presentation_mode_name(prepared.presentation_mode),
                            active_ui_source_name(active_ui_source_),
                            kMenuPanelDistanceMeters,
                            comfort_quad_.size_meters.x,
                            comfort_quad_.size_meters.y);
                    } else {
                        Log(wawvr::xr::LogLevel::Error,
                            "Could not construct a finite menu panel from the valid comfort anchor; withholding mono composition for this frame");
                    }
                } else if (!comfort_quad_valid_) {
                    comfort_quad_valid_ = build_world_menu_panel(
                        comfort_pose_, &comfort_quad_);
                }
                const auto comfort_views =
                    monoscopic_comfort_views(comfort_pose_);
                source.rendered_views_valid = true;
                for (std::uint32_t eye = 0;
                     eye < wawvr::xr::kEyeCount; ++eye) {
                    source.rendered_eyes[eye] = comfort_views[eye];
                }
                if (comfort_quad_valid_) {
                    current_quad = comfort_quad_;
                    current_quad_valid = true;
                }
            } else {
                if (comfort_pose_valid_) {
                    Log(wawvr::xr::LogLevel::Info,
                        "Exited mono comfort screen; restored capture-time stereo eye poses");
                }
                comfort_pose_valid_ = false;
                comfort_quad_ = {};
                comfort_quad_valid_ = false;
            }
#if defined(WAWVR_HAS_T4_BINDINGS)
            if (current_quad_valid && prepared.t4_state_valid) {
                current_menu_surface_valid = build_menu_pointer_surface(
                    current_quad, prepared.layout, source.width,
                    source.height, prepared.connection_state,
                    prepared.key_catchers, &current_menu_surface);
            }
            if (current_menu_surface_valid && frame.actions.focused) {
                const auto& right = frame.actions.hands[
                    static_cast<std::uint32_t>(wawvr::xr::Hand::Right)];
                const MenuPointerHit raw_hit = point_at_world_menu_panel(
                    right.aim, current_menu_surface.panel);
                if (raw_hit.valid && remap_menu_pointer_to_content(
                        raw_hit,
                        current_menu_surface.content_viewport).valid) {
                    current_reticle = {
                        .visible = true,
                        .target_eye = current_menu_surface.panel.source_eye,
                        .u = raw_hit.u,
                        .v = raw_hit.v,
                    };
                }
            }
#endif
            rendered = compositor_.RenderStereo(
                runtime_, frame, source, prepared.layout,
                current_reticle.visible ? &current_reticle : nullptr,
                &released_eye_mask,
                wawvr::xr::CompositorEffects{
                    .enable_eye_local_bloom =
                        prepared.presentation_mode ==
                            PresentationMode::stereo &&
                        !environment_disables_eye_local_bloom(),
                });
        }

        if (!reusable_layer_intact_after_render_attempt(
                rendered, released_eye_mask)) {
            // At least one swapchain now contains the failed attempt while
            // another may still contain the prior frame. Old pose/layer
            // metadata would submit a visually corrupt hybrid, so sacrifice
            // one recovery frame and wait for a complete fresh render.
            last_projection_available_ = false;
            last_projection_eyes_ = {};
            last_quad_layer_ = {};
            last_submission_was_quad_ = false;
            visible_menu_quad_ = {};
            visible_menu_quad_valid_ = false;
#if defined(WAWVR_HAS_T4_BINDINGS)
            last_menu_surface_ = {};
            last_menu_surface_valid_ = false;
            visible_menu_surface_ = {};
            visible_menu_surface_valid_ = false;
#endif
            if (!logged_partial_compositor_overwrite_) {
                LogFormatted(
                    wawvr::xr::LogLevel::Warning,
                    "Invalidated reusable OpenXR layer after an incomplete compositor update (released-eye-mask=0x%X)",
                    released_eye_mask);
                logged_partial_compositor_overwrite_ = true;
            }
        }

        const bool capture_device_lost =
            prepared.capture_result ==
            wawvr::xr::D3D9CpuCaptureResult::device_lost;
        const ProjectionRecoveryPlan recovery = plan_projection_recovery(
            rendered,
            last_projection_available_,
            d3d9_device_lost || capture_device_lost);
        wawvr::xr::D3D11SourceFrame previous_projection{};
        const wawvr::xr::QuadLayer* submitted_quad = nullptr;
        const bool mono_presentation =
            last_observed_presentation_mode_valid_ &&
            last_observed_presentation_mode_ != PresentationMode::stereo;
        const wawvr::xr::CompositionLayerKind requested_layer_kind =
            select_composition_layer_kind(
                recovery.submission, current_quad_valid,
                last_submission_was_quad_, mono_presentation);
        wawvr::xr::CompositionLayerSubmission layer_submission{
            .kind = requested_layer_kind,
        };
        if (requested_layer_kind ==
            wawvr::xr::CompositionLayerKind::quad) {
            submitted_quad =
                recovery.submission == ProjectionSubmission::current
                    ? &current_quad
                    : &last_quad_layer_;
            layer_submission.quad = submitted_quad;
        } else if (requested_layer_kind ==
                   wawvr::xr::CompositionLayerKind::projection) {
            if (recovery.submission == ProjectionSubmission::previous) {
                previous_projection = PreviousProjectionSource();
                layer_submission.projection_source = &previous_projection;
            } else {
                layer_submission.projection_source = &source;
            }
        }
        const wawvr::xr::EndFrameResult end_frame = runtime_.EndFrame(
            frame, layer_submission);
        const bool ended = end_frame.frame_ended;
        const bool accepted_current =
            ended &&
            recovery.submission == ProjectionSubmission::current &&
            end_frame.accepted_kind !=
                wawvr::xr::CompositionLayerKind::none;
        const bool accepted_previous =
            ended &&
            recovery.submission == ProjectionSubmission::previous &&
            end_frame.accepted_kind !=
                wawvr::xr::CompositionLayerKind::none;
        const bool current_quad_accepted =
            accepted_current &&
            end_frame.accepted_kind ==
                wawvr::xr::CompositionLayerKind::quad;
        if (!logged_layer_kind_valid_ ||
             requested_layer_kind != last_logged_requested_layer_kind_ ||
             end_frame.accepted_kind != last_logged_accepted_layer_kind_) {
            if (requested_layer_kind ==
                    wawvr::xr::CompositionLayerKind::quad &&
                submitted_quad != nullptr) {
                LogFormatted(
                    end_frame.accepted_kind == requested_layer_kind
                        ? wawvr::xr::LogLevel::Info
                        : wawvr::xr::LogLevel::Warning,
                    "OpenXR layer requested=quad accepted=%s LOCAL-pose=(%.3f, %.3f, %.3f | %.4f, %.4f, %.4f, %.4f) size=%.3fx%.3fm",
                    composition_layer_kind_name(end_frame.accepted_kind),
                    submitted_quad->pose.position.x,
                    submitted_quad->pose.position.y,
                    submitted_quad->pose.position.z,
                    submitted_quad->pose.orientation.x,
                    submitted_quad->pose.orientation.y,
                    submitted_quad->pose.orientation.z,
                    submitted_quad->pose.orientation.w,
                    submitted_quad->size_meters.x,
                    submitted_quad->size_meters.y);
            } else {
                LogFormatted(
                    requested_layer_kind == end_frame.accepted_kind
                        ? wawvr::xr::LogLevel::Info
                        : wawvr::xr::LogLevel::Warning,
                    "OpenXR layer requested=%s accepted=%s",
                    composition_layer_kind_name(requested_layer_kind),
                    composition_layer_kind_name(end_frame.accepted_kind));
            }
            last_logged_requested_layer_kind_ = requested_layer_kind;
            last_logged_accepted_layer_kind_ = end_frame.accepted_kind;
            logged_layer_kind_valid_ = true;
        }
        pending_frame_ = {};
        pending_frame_active_ = false;
        clear_pending_stereo_frame();

        if (!reusable_layer_intact_after_frame_end(
                rendered, accepted_current)) {
            // The compositor has already replaced/released both swapchain
            // images, so cached metadata for the older images is no longer a
            // valid recovery layer even if xrEndFrame rejected this frame.
            last_projection_available_ = false;
            last_projection_eyes_ = {};
            last_quad_layer_ = {};
            last_submission_was_quad_ = false;
            visible_menu_quad_ = {};
            visible_menu_quad_valid_ = false;
#if defined(WAWVR_HAS_T4_BINDINGS)
            last_menu_surface_ = {};
            last_menu_surface_valid_ = false;
            visible_menu_surface_ = {};
            visible_menu_surface_valid_ = false;
#endif
            Log(wawvr::xr::LogLevel::Error,
                "Invalidated all reusable OpenXR layers after a completed compositor render failed frame submission");
        }

        if (accepted_current) {
            logged_partial_compositor_overwrite_ = false;
            last_projection_available_ = true;
            last_submission_was_quad_ = current_quad_accepted;
            if (current_quad_accepted) {
                last_quad_layer_ = current_quad;
                visible_menu_quad_ = current_quad;
                visible_menu_quad_valid_ = true;
            } else {
                last_quad_layer_ = {};
                visible_menu_quad_ = {};
                visible_menu_quad_valid_ = false;
            }
#if defined(WAWVR_HAS_T4_BINDINGS)
            if (current_quad_accepted && current_menu_surface_valid) {
                last_menu_surface_ = current_menu_surface;
                last_menu_surface_valid_ = true;
                visible_menu_surface_ = current_menu_surface;
                visible_menu_surface_valid_ = true;
            } else {
                last_menu_surface_ = {};
                last_menu_surface_valid_ = false;
                visible_menu_surface_ = {};
                visible_menu_surface_valid_ = false;
            }
#endif
            for (std::uint32_t eye = 0; eye < wawvr::xr::kEyeCount; ++eye) {
                last_projection_eyes_[eye] =
                    source.rendered_views_valid
                        ? source.rendered_eyes[eye]
                        : frame.eyes[eye];
            }
            if (using_recovery_projection_) {
                if (now >= next_recovery_transition_log_) {
                    Log(wawvr::xr::LogLevel::Info,
                        "Fresh D3D9 capture resumed; stopped reusing the recovery projection");
                    next_recovery_transition_log_ =
                        now + kRecoveryTransitionLogMilliseconds;
                }
                using_recovery_projection_ = false;
            }
        } else if (accepted_previous) {
            const bool previous_quad_accepted =
                end_frame.accepted_kind ==
                wawvr::xr::CompositionLayerKind::quad;
            if (previous_quad_accepted && last_submission_was_quad_) {
                visible_menu_quad_ = last_quad_layer_;
                visible_menu_quad_valid_ = true;
            } else {
                visible_menu_quad_ = {};
                visible_menu_quad_valid_ = false;
            }
#if defined(WAWVR_HAS_T4_BINDINGS)
            if (previous_quad_accepted && last_submission_was_quad_ &&
                last_menu_surface_valid_) {
                visible_menu_surface_ = last_menu_surface_;
                visible_menu_surface_valid_ = true;
            } else {
                visible_menu_surface_ = {};
                visible_menu_surface_valid_ = false;
            }
#endif
            if (!using_recovery_projection_ &&
                now >= next_recovery_transition_log_) {
                Log(wawvr::xr::LogLevel::Warning,
                    "D3D9 frame unavailable; reusing the last released OpenXR composition layer instead of submitting a loading frame");
                next_recovery_transition_log_ =
                    now + kRecoveryTransitionLogMilliseconds;
            }
            using_recovery_projection_ = true;
        } else if (ended) {
            visible_menu_quad_ = {};
            visible_menu_quad_valid_ = false;
#if defined(WAWVR_HAS_T4_BINDINGS)
            visible_menu_surface_ = {};
            visible_menu_surface_valid_ = false;
#endif
        }

        if (recovery.submission == ProjectionSubmission::previous &&
            !using_recovery_projection_) {
            if (now >= next_recovery_transition_log_) {
                Log(wawvr::xr::LogLevel::Warning,
                    "D3D9 frame unavailable; waiting to reuse the last released OpenXR composition layer");
                next_recovery_transition_log_ =
                    now + kRecoveryTransitionLogMilliseconds;
            }
        }

        if (accepted_current && prepared.stereo_scene &&
            end_frame.accepted_kind ==
                wawvr::xr::CompositionLayerKind::projection &&
            prepared.presentation_mode == PresentationMode::stereo &&
            !logged_first_stereo_submission_) {
            Log(wawvr::xr::LogLevel::Info,
                "Submitted first exact-frame T4 side-by-side stereo image with asymmetric OpenXR viewport remap");
            logged_first_stereo_submission_ = true;
            logged_missing_stereo_backend_ = false;
        }
        if (accepted_current && prepared.stereo_scene &&
            end_frame.accepted_kind ==
                wawvr::xr::CompositionLayerKind::projection &&
            prepared.presentation_mode == PresentationMode::stereo &&
            !automatic_gameplay_recenter_completed_ &&
            !explicit_recenter_captured_) {
            // The first valid HMD pose normally arrives while Nacht is still
            // loading.  The player may put on or level the headset after that,
            // which
            // leaves the playable camera translated or rolled until a manual
            // recenter.  The first confirmed gameplay stereo frame is the
            // earliest reliable signal that the player view actually exists;
            // capture the following valid pose as the real startup centre.
            automatic_gameplay_recenter_pending_ = true;
        }
        if (recovery.abandon_xr) {
            ShutdownXr(
                "D3D9 device was lost before any reusable XR projection existed");
            next_runtime_attempt_ = now + kRuntimeRetryMilliseconds;
            return false;
        }
        if (!ended && runtime_.exit_requested()) {
            ShutdownXr("OpenXR frame submission failed fatally");
            next_runtime_attempt_ = now + kRuntimeRetryMilliseconds;
            return false;
        }
        return runtime_.initialized();
    }

    void RecordPresentResult(const HRESULT present_result) {
        present_seen_since_boundary_ = true;
        latest_present_result_ = present_result;
        if (present_result == D3DERR_DEVICELOST ||
            present_result == D3DERR_DEVICENOTRESET) {
            device_loss_seen_since_boundary_ = true;
            capture_.NotifyExternalDeviceLoss();
        }
    }

    void ServiceAfterComFrame() {
        const ULONGLONG now = GetTickCount64();
        if (reset_in_progress_.load(std::memory_order_acquire)) {
            return;
        }
        if (!PublishedTargetMatchesEngine()) {
            PauseFrameServiceForTargetRefresh();
            return;
        }
        if (!stereo_scene_hook_installed() &&
            !scene_hook_permanently_disabled_ &&
            now >= next_scene_hook_attempt_) {
            TryInstallStereoSceneHook(now);
        }
        IDirect3DDevice9* const device =
            target_device_.load(std::memory_order_acquire);
        const HRESULT cooperative_result =
            device != nullptr
                ? device->TestCooperativeLevel()
                : D3DERR_INVALIDCALL;
        const bool present_seen = present_seen_since_boundary_;
        const HRESULT present_result = latest_present_result_;
        const bool present_succeeded =
            present_seen && SUCCEEDED(present_result);
        const bool device_cooperative = SUCCEEDED(cooperative_result);
        const bool device_lost =
            device_loss_seen_since_boundary_ ||
            present_result == D3DERR_DEVICELOST ||
            present_result == D3DERR_DEVICENOTRESET ||
            cooperative_result == D3DERR_DEVICELOST ||
            cooperative_result == D3DERR_DEVICENOTRESET;
        const PostComFramePlan service_plan =
            plan_post_com_frame_service(
                present_seen, present_succeeded, device_cooperative,
                last_projection_available_, false);

        if (cooperative_result == D3DERR_DEVICELOST ||
            cooperative_result == D3DERR_DEVICENOTRESET) {
            capture_.NotifyExternalDeviceLoss();
        }

        if ((present_seen && !present_succeeded) || !device_cooperative) {
            if (!logged_present_failure_ ||
                present_result != last_present_result_ ||
                cooperative_result != last_cooperative_result_) {
                LogFormatted(
                    wawvr::xr::LogLevel::Warning,
                    "D3D9 frame-boundary state unavailable: Present=%s0x%08lx TestCooperativeLevel=0x%08lx; fresh capture paused",
                    present_seen ? "" : "not-seen/",
                    present_seen ? present_result : D3D_OK,
                    cooperative_result);
                logged_present_failure_ = true;
                last_present_result_ = present_result;
                last_cooperative_result_ = cooperative_result;
            }
        } else if (logged_present_failure_) {
            Log(wawvr::xr::LogLevel::Info,
                "D3D9 Present and cooperative state recovered; XR priming resumed");
            logged_present_failure_ = false;
        }

        if (pending_frame_active_ &&
            !FinishPendingFrameAfterComFrame(
                service_plan.allow_fresh_projection &&
                    !device_loss_seen_since_boundary_ &&
                    !reset_seen_since_boundary_,
                device_lost,
                now)) {
            ResetBoundaryObservations();
            return;
        }

        if (device_lost && !last_projection_available_ &&
            runtime_.initialized() && !pending_frame_active_) {
            ShutdownXr(
                "D3D9 remained lost before the first reusable XR projection");
            next_runtime_attempt_ = now + kRuntimeRetryMilliseconds;
            ResetBoundaryObservations();
            return;
        }

        ResetBoundaryObservations();

        if (!runtime_.initialized() && device_cooperative &&
            now >= next_runtime_attempt_) {
            InitializeXr(now);
        }

        if (runtime_.initialized() && service_plan.prime_next_frame) {
            PrimeNextFrame(now);
        } else if (runtime_.initialized()) {
            clear_pending_stereo_frame();
            clear_controller_frame();
            if (!runtime_.PollEvents() && runtime_.exit_requested()) {
                ShutdownXr("OpenXR runtime requested exit during D3D9 recovery");
                next_runtime_attempt_ = now + kRuntimeRetryMilliseconds;
            }
        }
    }

    void ResetBoundaryObservations() noexcept {
        present_seen_since_boundary_ = false;
        device_loss_seen_since_boundary_ = false;
        reset_seen_since_boundary_ = false;
        latest_present_result_ = D3D_OK;
        latest_reset_result_ = D3D_OK;
    }

    wawvr::xr::D3D11SourceFrame PreviousProjectionSource() const noexcept {
        wawvr::xr::D3D11SourceFrame source{};
        source.rendered_views_valid = last_projection_available_;
        if (last_projection_available_) {
            for (std::uint32_t eye = 0; eye < wawvr::xr::kEyeCount; ++eye) {
                source.rendered_eyes[eye] = last_projection_eyes_[eye];
            }
        }
        return source;
    }

    bool PrimeNextFrame(const ULONGLONG now) {
        prepared_frame_ = {};
        if (!runtime_.PollEvents()) {
            clear_pending_stereo_frame();
            clear_controller_frame();
            if (runtime_.exit_requested()) {
                ShutdownXr("OpenXR runtime requested exit");
                next_runtime_attempt_ = now + kRuntimeRetryMilliseconds;
            }
            return false;
        }

        wawvr::xr::FrameState next{};
        if (!runtime_.BeginFrame(&next)) {
            clear_pending_stereo_frame();
            clear_controller_frame();
            return false;
        }
        pending_frame_ = next;
        pending_frame_active_ = true;

        wawvr::xr::Quaternionf body_aligned_anchor_orientation{};
        const bool body_anchor_rebase_pending =
            consume_controller_tracking_anchor_rebase(
                &body_aligned_anchor_orientation);
        if (tracking_anchor_valid_ && body_anchor_rebase_pending) {
            // The game thread transferred the prior frame's physical HMD yaw
            // into T4's native body. Apply the identical yaw-only rebase to
            // Present's long-lived anchor before publishing this prediction;
            // position remains owned exclusively by explicit recenter.
            tracking_anchor_.orientation =
                body_aligned_anchor_orientation;
        }

        const bool menu_pressed =
            next.actions.menu.active && next.actions.menu.current;
        const MenuButtonGestureUpdate menu_gesture =
            update_menu_button_gesture(
            menu_pressed,
            next.views_valid,
            now,
            kControllerMenuHoldMilliseconds,
            &menu_button_gesture_state_);
        if (menu_gesture.hold_started) {
            LogFormatted(
                wawvr::xr::LogLevel::Info,
                "Left-menu gesture started; release before %llu ms for Escape or keep holding to recenter",
                static_cast<unsigned long long>(
                    kControllerMenuHoldMilliseconds));
        }
        if (menu_gesture.waiting_for_valid_pose) {
            Log(wawvr::xr::LogLevel::Info,
                "Recenter hold complete; preserving timer while awaiting the next valid HMD pose");
        }
        if (automatic_gameplay_recenter_pending_ && next.views_valid) {
            // An automatic capture must never redefine physical pitch/roll:
            // the player may happen to glance up or down on this frame. Keep
            // gravity level and refresh only position + yaw. The deliberate
            // one-second Menu hold below does the same immediately on demand.
            const wawvr::xr::Quaternionf* const prior_yaw =
                tracking_anchor_valid_
                    ? &tracking_anchor_.orientation
                    : nullptr;
            tracking_anchor_ = leveled_tracking_anchor(
                next.head_center, prior_yaw);
            tracking_anchor_valid_ = true;
            automatic_gameplay_recenter_pending_ = false;
            automatic_gameplay_recenter_completed_ = true;
            LogFormatted(
                wawvr::xr::LogLevel::Info,
                "Automatic first-gameplay-frame recenter refreshed position + yaw with a gravity-level anchor: position=(%.3f, %.3f, %.3f) orientation=(%.4f, %.4f, %.4f, %.4f)",
                tracking_anchor_.position.x,
                tracking_anchor_.position.y,
                tracking_anchor_.position.z,
                tracking_anchor_.orientation.x,
                tracking_anchor_.orientation.y,
                tracking_anchor_.orientation.z,
                tracking_anchor_.orientation.w);
        }
        if (menu_gesture.recenter_capture_requested) {
            // A manual recenter must always restore a gravity-level horizon.
            // Capturing the complete current quaternion made a tilted HMD
            // pose the new permanent reference, which is exactly the wrong
            // behavior when recenter is invoked to correct tilt.
            const wawvr::xr::Quaternionf* const prior_yaw =
                tracking_anchor_valid_
                    ? &tracking_anchor_.orientation
                    : nullptr;
            tracking_anchor_ = leveled_tracking_anchor(
                next.head_center, prior_yaw);
            tracking_anchor_valid_ = true;
            automatic_gameplay_recenter_pending_ = false;
            automatic_gameplay_recenter_completed_ = true;
            explicit_recenter_captured_ = true;
            if (last_projection_available_ && last_submission_was_quad_) {
                // A static menu may not issue another Present. Reposition the
                // already released texture immediately so recenter cannot
                // resurrect the old world-space panel pose through recovery.
                comfort_pose_ = tracking_anchor_;
                comfort_pose_valid_ = true;
                comfort_quad_valid_ = build_world_menu_panel(
                    comfort_pose_, &comfort_quad_);
                if (comfort_quad_valid_) {
                    // Rebase only the metadata that the next recovery
                    // xrEndFrame will submit. The currently visible layer is
                    // still at its old pose, so its ray surface must remain
                    // unchanged until that EndFrame succeeds and promotes
                    // the rebased descriptor below.
                    last_quad_layer_ = comfort_quad_;
#if defined(WAWVR_HAS_T4_BINDINGS)
                    if (last_menu_surface_valid_) {
                        last_menu_surface_.panel = comfort_quad_;
                    }
#endif
                } else {
                    last_projection_available_ = false;
                    last_submission_was_quad_ = false;
                    last_quad_layer_ = {};
                    visible_menu_quad_ = {};
                    visible_menu_quad_valid_ = false;
#if defined(WAWVR_HAS_T4_BINDINGS)
                    last_menu_surface_ = {};
                    last_menu_surface_valid_ = false;
                    visible_menu_surface_ = {};
                    visible_menu_surface_valid_ = false;
#endif
                }
            } else {
                comfort_pose_valid_ = false;
                comfort_quad_ = {};
                comfort_quad_valid_ = false;
                visible_menu_quad_ = {};
                visible_menu_quad_valid_ = false;
#if defined(WAWVR_HAS_T4_BINDINGS)
                visible_menu_surface_ = {};
                visible_menu_surface_valid_ = false;
#endif
            }
            LogFormatted(
                wawvr::xr::LogLevel::Info,
                "Manual recenter captured position + yaw with a gravity-level anchor: position=(%.3f, %.3f, %.3f) orientation=(%.4f, %.4f, %.4f, %.4f); horizon is level and current translation/yaw are zeroed",
                tracking_anchor_.position.x,
                tracking_anchor_.position.y,
                tracking_anchor_.position.z,
                tracking_anchor_.orientation.x,
                tracking_anchor_.orientation.y,
                tracking_anchor_.orientation.z,
                tracking_anchor_.orientation.w);
        }

#if defined(WAWVR_HAS_T4_BINDINGS)
        const T4MenuInputServiceResult menu_input =
            service_t4_menu_input_after_com_frame(
                next,
                last_source_width_,
                last_source_height_,
                active_ui_source_,
                visible_menu_surface_valid_
                    ? &visible_menu_surface_
                    : nullptr,
                menu_gesture.short_tap_released,
                now,
                &menu_input_state_);
        if (menu_input.cursor_submitted &&
            !logged_first_native_menu_cursor_) {
            Log(wawvr::xr::LogLevel::Info,
                "Native T4 UI cursor path is active: right-controller ray + trigger, with left stick + A fallback");
            logged_first_native_menu_cursor_ = true;
        }
        if (menu_input.pointer_submitted &&
            !logged_first_controller_menu_ray_) {
            Log(wawvr::xr::LogLevel::Info,
                "Right OpenXR aim ray intersected the finite menu panel and moved the native T4 cursor");
            logged_first_controller_menu_ray_ = true;
        }
        if (menu_input.menu_button_tapped) {
            Log(wawvr::xr::LogLevel::Info,
                "Left-menu short release dispatched a balanced native Escape tap");
        }
        if (menu_input.confirm_tapped) {
            if (menu_input.confirm_via_enter) {
                Log(wawvr::xr::LogLevel::Info,
                    menu_input.pointer_confirm_tapped
                        ? "VR trigger dispatched a balanced native Enter tap at the pointed in-match MP menu item"
                        : "VR A dispatched a balanced native Enter tap in the active MP menu");
            } else {
                Log(wawvr::xr::LogLevel::Info,
                    menu_input.pointer_confirm_tapped
                        ? "VR trigger dispatched a balanced native Mouse1 tap at the pointed menu item"
                        : "VR A dispatched a balanced native Mouse1 menu tap");
            }
        }
        if (menu_input.back_tapped) {
            Log(wawvr::xr::LogLevel::Info,
                "VR B dispatched a balanced native Escape menu tap");
        }
        if (menu_input.pezbot_autofill_queued) {
            Log(wawvr::xr::LogLevel::Info,
                "PeZBOT autofill armed nine bots for the next MP map/mode lifecycle");
        }
        if (menu_ui_action_invalidates_pointer_surface(
                menu_input.menu_button_tapped,
                menu_input.confirm_tapped,
                menu_input.back_tapped)) {
            // T4 can switch pages synchronously while leaving connection
            // state and key catchers unchanged. Keep displaying the last
            // released quad during recovery, but do not let any controller
            // target that stale page. A fresh successful UI submission will
            // publish a new exact descriptor in FinishPendingFrame.
            last_menu_surface_ = {};
            last_menu_surface_valid_ = false;
            visible_menu_surface_ = {};
            visible_menu_surface_valid_ = false;
        }
#endif

        if (next.should_render && next.views_valid) {
            if (!tracking_anchor_valid_) {
                tracking_anchor_ = leveled_tracking_anchor(next.head_center);
                tracking_anchor_valid_ = true;
                Log(wawvr::xr::LogLevel::Info,
                    "Captured first valid OpenXR head centre with yaw-only horizon-level tracking anchor");
            }
            {
                TrackingAnchorSyncLock anchor_publication(
                    tracking_anchor_sync_mutex());
                publish_pending_stereo_frame(next, tracking_anchor_);
                publish_controller_frame(next, tracking_anchor_);
            }
            if (!logged_first_primed_frame_) {
                LogFormatted(
                    wawvr::xr::LogLevel::Info,
                    "Primed OpenXR frame %llu for the following T4 simulation/render",
                    static_cast<unsigned long long>(next.frame_id));
                logged_first_primed_frame_ = true;
            }
        } else {
            TrackingAnchorSyncLock anchor_publication(
                tracking_anchor_sync_mutex());
            clear_pending_stereo_frame();
            clear_controller_frame();
        }
        return true;
    }

    void TryInstallStereoSceneHook(const ULONGLONG now) noexcept {
        const auto result = install_stereo_scene_hook();
        if (result == StereoSceneHookResult::installed ||
            result == StereoSceneHookResult::already_installed) {
            const auto* const profile = configured_present_profile();
            LogFormatted(
                wawvr::xr::LogLevel::Info,
                "%s exact gameplay scene call hook active",
                profile != nullptr ? profile->name : "Configured T4");
            return;
        }

        LogFormatted(
            wawvr::xr::LogLevel::Warning,
            "T4 gameplay stereo hook not installed: %s; mono XR remains active",
            describe_stereo_scene_hook_result(result));
        if (result == StereoSceneHookResult::profile_mismatch ||
            result == StereoSceneHookResult::target_mismatch) {
            scene_hook_permanently_disabled_ = true;
        } else {
            next_scene_hook_attempt_ = now + kInstallRetryMilliseconds;
        }
    }

    void InitializeXr(const ULONGLONG now) {
        next_runtime_attempt_ = now + kRuntimeRetryMilliseconds;
        host_callbacks_ = {this, &XrLogThunk};
        Log(wawvr::xr::LogLevel::Info,
            "Attempting OpenXR initialization from exact post-Com_Frame boundary");
        if (!capture_.Initialize(host_callbacks_)) {
            Log(wawvr::xr::LogLevel::Error,
                "D3D9 CPU capture initialization failed");
            return;
        }

        wawvr::xr::RuntimeConfig config{};
        config.application_name = "World War VR";
        config.engine_name = "IW/T4 compatibility layer";
        config.render_scale = 1.0f;
        config.reference_space = wawvr::xr::ReferenceSpace::Local;
        config.prefer_srgb_swapchain = true;
        if (!runtime_.Initialize(config, host_callbacks_)) {
            capture_.Shutdown();
            if (runtime_.last_initialization_failure() ==
                wawvr::xr::InitializationFailure::headset_unavailable) {
                LogFormatted(
                    wawvr::xr::LogLevel::Error,
                    "OpenXR runtime '%s' reports no PC VR headset is available; wake/connect the headset and enter Quest Link/Air Link or start the intended SteamVR runtime",
                    runtime_.active_runtime_name());
                Log(wawvr::xr::LogLevel::Warning,
                    "Desktop rendering remains visible while World War VR retries OpenXR every 10 seconds");
                notify_headset_unavailable_once();
            } else if (runtime_.last_initialization_failure() ==
                       wawvr::xr::InitializationFailure::runtime_unavailable) {
                Log(wawvr::xr::LogLevel::Error,
                    "No active OpenXR runtime is available; configure Meta Quest Link or SteamVR as the active OpenXR runtime and relaunch");
            } else {
                Log(wawvr::xr::LogLevel::Warning,
                    "OpenXR initialization unavailable; desktop game continues and retry is scheduled");
            }
            return;
        }
        compositor_initialized_ = compositor_.Initialize(runtime_, host_callbacks_);
        if (!compositor_initialized_) {
            ShutdownXr("initial compositor failure");
            return;
        }
        g_mono_xr_ready.store(true, std::memory_order_release);
        Log(wawvr::xr::LogLevel::Info,
            "OpenXR path ready: exact gameplay stereo plus finite room-anchored mono panels for frontend UI, loading screens, console, and cinematics");
    }

    void ShutdownXr(const std::string_view reason) noexcept {
        bool teardown_exception = false;
        bool runtime_was_initialized = false;
        try {
            runtime_was_initialized = runtime_.initialized();
        } catch (...) {
            teardown_exception = true;
        }
        if (runtime_was_initialized) {
            LogFormatted(
                wawvr::xr::LogLevel::Info,
                "Shutting down XR path: %.*s",
                static_cast<int>(reason.size()), reason.data());
        }
        try {
            compositor_.Shutdown();
        } catch (...) {
            teardown_exception = true;
        }
        try {
            capture_.Shutdown();
        } catch (...) {
            teardown_exception = true;
        }
        try {
            runtime_.Shutdown();
        } catch (...) {
            teardown_exception = true;
        }
        pending_frame_ = {};
        pending_frame_active_ = false;
        prepared_frame_ = {};
        last_projection_eyes_ = {};
        last_projection_available_ = false;
        last_quad_layer_ = {};
        last_submission_was_quad_ = false;
        tracking_anchor_ = {};
        tracking_anchor_valid_ = false;
        comfort_pose_ = {};
        comfort_pose_valid_ = false;
        comfort_quad_ = {};
        comfort_quad_valid_ = false;
        last_observed_presentation_mode_ = PresentationMode::stereo;
        last_observed_presentation_mode_valid_ = false;
        last_logged_presentation_mode_ = PresentationMode::stereo;
        last_logged_connection_state_ = 0;
        last_logged_active_connection_state_ = 10;
        last_logged_key_catchers_ = 0;
        last_logged_t4_state_valid_ = false;
        presentation_state_log_valid_ = false;
        last_logged_requested_layer_kind_ =
            wawvr::xr::CompositionLayerKind::none;
        last_logged_accepted_layer_kind_ =
            wawvr::xr::CompositionLayerKind::none;
        logged_layer_kind_valid_ = false;
        visible_menu_quad_ = {};
        visible_menu_quad_valid_ = false;
        menu_button_gesture_state_ = {};
#if defined(WAWVR_HAS_T4_BINDINGS)
        menu_input_state_ = {};
        last_menu_surface_ = {};
        last_menu_surface_valid_ = false;
        visible_menu_surface_ = {};
        visible_menu_surface_valid_ = false;
#endif
        last_source_width_ = 0;
        last_source_height_ = 0;
        automatic_gameplay_recenter_pending_ = false;
        automatic_gameplay_recenter_completed_ = false;
        explicit_recenter_captured_ = false;
        using_recovery_projection_ = false;
        next_recovery_transition_log_ = 0;
        logged_present_failure_ = false;
        logged_partial_compositor_overwrite_ = false;
        last_present_result_ = D3D_OK;
        last_cooperative_result_ = D3D_OK;
        ResetBoundaryObservations();
        clear_pending_stereo_frame();
        clear_controller_frame();
        discard_controller_tracking_anchor_rebase();
        compositor_initialized_ = false;
        g_mono_xr_ready.store(false, std::memory_order_release);
        if (teardown_exception) {
            Log(wawvr::xr::LogLevel::Error,
                "One or more XR teardown stages raised an exception; remaining stages and local state cleanup were still completed");
        }
    }

    static void XrLogThunk(
        void* const user_data,
        const wawvr::xr::LogLevel level,
        const char* const message) noexcept {
        if (user_data != nullptr && message != nullptr) {
            static_cast<PresentHookController*>(user_data)->Log(level, message);
        }
    }

    void Log(
        const wawvr::xr::LogLevel level,
        const std::string_view message) const noexcept {
        append_log(module_, level, message);
    }

    void LogFormatted(
        const wawvr::xr::LogLevel level,
        const char* const format,
        ...) const noexcept {
        std::array<char, 1024> buffer{};
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
        va_end(arguments);
        buffer.back() = '\0';
        Log(level, buffer.data());
    }

    void LogRestoreResult(
        const char* const name,
        const VtableOwnership ownership) const noexcept {
        if (ownership == VtableOwnership::original) {
            LogFormatted(wawvr::xr::LogLevel::Info, "%s slot restored", name);
        } else {
            LogFormatted(
                wawvr::xr::LogLevel::Warning,
                "%s slot ownership changed; foreign value preserved", name);
        }
    }

    [[nodiscard]] bool PublishedTargetMatchesEngine() const noexcept {
        const auto* const profile = configured_present_profile();
        T4PresentTarget current{};
        return profile != nullptr &&
               read_t4_present_target(*profile, &current) &&
               current.device ==
                   target_device_.load(std::memory_order_acquire) &&
               current.swap_chain ==
                   target_swap_chain_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool ReadValidatedTargetMatching(
        const T4PresentTarget& expected,
        T4PresentTarget* const refreshed) const noexcept {
        const auto* const profile = configured_present_profile();
        T4PresentTarget current{};
        const bool current_validated =
            profile != nullptr && read_t4_present_target(*profile, &current);
        const PresentTargetIdentity expected_identity{
            reinterpret_cast<std::uintptr_t>(expected.device),
            reinterpret_cast<std::uintptr_t>(expected.swap_chain)};
        const PresentTargetIdentity current_identity{
            reinterpret_cast<std::uintptr_t>(current.device),
            reinterpret_cast<std::uintptr_t>(current.swap_chain)};
        if (!present_target_still_current(
                expected_identity, current_validated, current_identity)) {
            return false;
        }
        if (refreshed != nullptr) {
            *refreshed = current;
        }
        return true;
    }

    void RestoreD3DSlotsForRebind() noexcept {
        frame_service_ready_.store(false, std::memory_order_release);
        if (reset_hook_installed_.exchange(false, std::memory_order_acq_rel)) {
            LogRestoreResult("D3D9 Reset", device_reset_patch_.Restore());
        }
        if (g_present_hook_installed.exchange(
                false, std::memory_order_acq_rel)) {
            LogRestoreResult(
                "swap-chain Present", swap_present_patch_.Restore());
        }
        ClearPublishedTargetIdentity();
    }

    void ClearPublishedTargetIdentity() noexcept {
        target_device_.store(nullptr, std::memory_order_release);
        target_swap_chain_.store(nullptr, std::memory_order_release);
    }

    HMODULE module_{};
    VtableSlotPatch swap_present_patch_;
    VtableSlotPatch device_reset_patch_;
    std::atomic<IDirect3DDevice9*> target_device_{nullptr};
    std::atomic<IDirect3DSwapChain9*> target_swap_chain_{nullptr};
    std::atomic<bool> reset_hook_installed_{false};
    std::atomic<bool> reset_in_progress_{false};
    std::atomic<bool> frame_service_ready_{false};
    std::atomic<bool> target_refresh_requested_{false};
    std::mutex render_mutex_;
    wawvr::xr::HostCallbacks host_callbacks_{};
    wawvr::xr::OpenXrRuntime runtime_;
    wawvr::xr::D3D9CpuCapture capture_;
    wawvr::xr::D3D11Compositor compositor_;
    wawvr::xr::FrameState pending_frame_{};
    PreparedPendingFrame prepared_frame_{};
    std::array<wawvr::xr::EyeView, wawvr::xr::kEyeCount>
        last_projection_eyes_{};
    wawvr::xr::Posef tracking_anchor_{};
    wawvr::xr::Posef comfort_pose_{};
    wawvr::xr::QuadLayer comfort_quad_{};
    wawvr::xr::QuadLayer visible_menu_quad_{};
    wawvr::xr::QuadLayer last_quad_layer_{};
    PresentationMode last_observed_presentation_mode_{
        PresentationMode::stereo};
    PresentationMode last_logged_presentation_mode_{
        PresentationMode::stereo};
    wawvr::xr::CompositionLayerKind last_logged_requested_layer_kind_{
        wawvr::xr::CompositionLayerKind::none};
    wawvr::xr::CompositionLayerKind last_logged_accepted_layer_kind_{
        wawvr::xr::CompositionLayerKind::none};
    MenuButtonGestureState menu_button_gesture_state_{};
#if defined(WAWVR_HAS_T4_BINDINGS)
    T4MenuInputState menu_input_state_{};
    MenuPointerSurface last_menu_surface_{};
    MenuPointerSurface visible_menu_surface_{};
#endif
    std::uint32_t last_source_width_{};
    std::uint32_t last_source_height_{};
    std::int32_t last_logged_connection_state_{};
    std::int32_t last_logged_active_connection_state_{10};
    std::uint32_t last_logged_key_catchers_{};
    bool compositor_initialized_ = false;
    bool pending_frame_active_ = false;
    bool last_projection_available_ = false;
    bool tracking_anchor_valid_ = false;
    bool comfort_pose_valid_ = false;
    bool comfort_quad_valid_ = false;
    bool visible_menu_quad_valid_ = false;
    bool last_submission_was_quad_ = false;
    bool last_observed_presentation_mode_valid_ = false;
    bool last_logged_t4_state_valid_ = false;
    bool presentation_state_log_valid_ = false;
    bool logged_layer_kind_valid_ = false;
    bool automatic_gameplay_recenter_pending_ = false;
    bool automatic_gameplay_recenter_completed_ = false;
    bool explicit_recenter_captured_ = false;
    bool logged_first_primed_frame_ = false;
    bool logged_first_stereo_submission_ = false;
    bool logged_active_capture_path_ = false;
    bool logged_missing_stereo_backend_ = false;
#if defined(WAWVR_HAS_T4_BINDINGS)
    bool logged_first_native_menu_cursor_ = false;
    bool logged_first_controller_menu_ray_ = false;
    bool last_menu_surface_valid_ = false;
    bool visible_menu_surface_valid_ = false;
#endif
    bool using_recovery_projection_ = false;
    bool logged_present_failure_ = false;
    bool logged_partial_compositor_overwrite_ = false;
    bool scene_hook_permanently_disabled_ = false;
    bool present_seen_since_boundary_ = false;
    bool device_loss_seen_since_boundary_ = false;
    bool reset_seen_since_boundary_ = false;
    HRESULT latest_present_result_ = D3D_OK;
    HRESULT latest_reset_result_ = D3D_OK;
    HRESULT last_present_result_ = D3D_OK;
    HRESULT last_cooperative_result_ = D3D_OK;
    ULONGLONG next_recovery_transition_log_ = 0;
    ULONGLONG next_runtime_attempt_ = 0;
    ULONGLONG next_scene_hook_attempt_ = 0;
    ActiveUiMonoSource active_ui_source_{configured_active_ui_source()};
};

std::atomic<PresentHookController*> PresentHookController::instance_{nullptr};

} // namespace

bool configure_renderer_hooks(
    const wawvr::t4::ExecutableLayoutId layout) noexcept {
    const int desired = renderer_layout_code(layout);
    if (desired == kRendererLayoutUnconfigured) {
        return false;
    }
    int expected = kRendererLayoutUnconfigured;
    return g_renderer_layout.compare_exchange_strong(
               expected, desired, std::memory_order_acq_rel,
               std::memory_order_acquire) ||
           expected == desired;
}

bool get_configured_renderer_layout(
    wawvr::t4::ExecutableLayoutId* const layout) noexcept {
    if (layout == nullptr) {
        return false;
    }
    switch (g_renderer_layout.load(std::memory_order_acquire)) {
    case kRendererLayoutSinglePlayer:
        *layout = wawvr::t4::ExecutableLayoutId::t4_sp_1_7_1263;
        return true;
    case kRendererLayoutMultiplayer:
        *layout = wawvr::t4::ExecutableLayoutId::t4_mp_1_7_1263;
        return true;
    default:
        return false;
    }
}

void service_present_hook_after_com_frame() noexcept {
    PresentHookController::ServicePublishedAfterComFrame();
}

void input_diagnostic_log(const char* const format, ...) noexcept {
    if (format == nullptr) {
        return;
    }
    std::array<char, 1024> buffer{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    buffer.back() = '\0';

    const HMODULE module = g_log_module.load(std::memory_order_acquire);
    if (module != nullptr) {
        append_log(module, wawvr::xr::LogLevel::Info, buffer.data());
    } else {
        std::string debugger_line = "WorldAtWarVR [info]: ";
        debugger_line += buffer.data();
        debugger_line += '\n';
        OutputDebugStringA(debugger_line.c_str());
    }
}

void stereo_diagnostic_log(const char* const format, ...) noexcept {
    if (format == nullptr) {
        return;
    }
    std::array<char, 1024> buffer{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    buffer.back() = '\0';

    const HMODULE module = g_log_module.load(std::memory_order_acquire);
    if (module != nullptr) {
        append_log(module, wawvr::xr::LogLevel::Warning, buffer.data());
    } else {
        std::string debugger_line = "WorldAtWarVR [warning]: ";
        debugger_line += buffer.data();
        debugger_line += '\n';
        OutputDebugStringA(debugger_line.c_str());
    }
}

void stereo_diagnostic_log_once(
    std::atomic_flag& gate,
    const char* const format,
    ...) noexcept {
    if (gate.test_and_set(std::memory_order_relaxed) || format == nullptr) {
        return;
    }
    std::array<char, 1024> buffer{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    buffer.back() = '\0';

    stereo_diagnostic_log("%s", buffer.data());
}

void request_present_hook_shutdown() noexcept {
    g_shutdown_requested.store(true, std::memory_order_release);
}

bool present_hook_installed() noexcept {
    return g_present_hook_installed.load(std::memory_order_acquire);
}

bool mono_xr_ready() noexcept {
    return g_mono_xr_ready.load(std::memory_order_acquire);
}

void run_present_hook_monitor(const HMODULE module) noexcept {
    g_log_module.store(module, std::memory_order_release);
    const PresentExecutableProfile* const profile =
        configured_present_profile();
    if (profile == nullptr) {
        append_log(
            module, wawvr::xr::LogLevel::Error,
            "Renderer hooks were not configured by an exact executable profile; hooks disabled");
        return;
    }
    if (environment_disables_present_hook()) {
        append_log(
            module, wawvr::xr::LogLevel::Warning,
            "WAWVR_DISABLE_XR is set; D3D9/OpenXR hooks are disabled");
        return;
    }
    if (!verify_rb_swap_buffers_sentinel(*profile)) {
        std::array<char, 192> message{};
        std::snprintf(
            message.data(), message.size(),
            "%s RB_SwapBuffers bytes at 0x%08llX did not match the exact profile; hooks disabled",
            profile->name,
            static_cast<unsigned long long>(
                profile->rb_swap_buffers_address));
        append_log(
            module, wawvr::xr::LogLevel::Error,
            message.data());
        return;
    }
    std::array<char, 160> validation_message{};
    std::snprintf(
        validation_message.data(), validation_message.size(),
        "%s RB_SwapBuffers exact 40-byte sentinel validated at 0x%08llX",
        profile->name,
        static_cast<unsigned long long>(profile->rb_swap_buffers_address));
    append_log(
        module, wawvr::xr::LogLevel::Info,
        validation_message.data());

    HMODULE pinned_module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned_module)) {
        append_log(
            module, wawvr::xr::LogLevel::Warning,
            "Could not pin WorldWarVR.dll; continuing with cooperative lifetime");
    }

    // Process-lifetime ownership avoids destroying XR state from DllMain.
    auto* const controller = new (std::nothrow) PresentHookController(module);
    if (controller == nullptr) {
        append_log(
            module, wawvr::xr::LogLevel::Error,
            "Could not allocate Present-hook controller; desktop game continues");
        return;
    }

    append_log(
        module, wawvr::xr::LogLevel::Info,
        "Waiting for T4 target window/swap-chain after validated D3D initialization");
    ULONGLONG next_install_attempt = 0;
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        T4PresentTarget target{};
        const bool target_ready = read_t4_present_target(*profile, &target);
        const ULONGLONG now = GetTickCount64();
        const PresentTargetIdentity installed_target{
            reinterpret_cast<std::uintptr_t>(controller->target_device()),
            reinterpret_cast<std::uintptr_t>(controller->target_swap_chain())};
        const PresentTargetIdentity candidate_target{
            reinterpret_cast<std::uintptr_t>(target.device),
            reinterpret_cast<std::uintptr_t>(target.swap_chain)};
        const bool target_installed = installed_target.swap_chain_id != 0;
        const auto action = plan_present_target_monitor(
            target_installed, installed_target, target_ready,
            candidate_target);
        if (action == PresentTargetMonitorAction::install_target &&
            now >= next_install_attempt) {
            if (!controller->Install(target)) {
                next_install_attempt = now + kInstallRetryMilliseconds;
            }
        } else if (action == PresentTargetMonitorAction::rebind_target &&
                   now >= next_install_attempt) {
            if (!controller->Rebind(target)) {
                next_install_attempt = now + kInstallRetryMilliseconds;
            } else {
                next_install_attempt = 0;
            }
        } else if (action == PresentTargetMonitorAction::keep_target &&
                   target_installed) {
            if (target_ready) {
                controller->ResumeFrameServiceForCurrentTarget();
            } else {
                controller->PauseFrameServiceForTargetRefresh();
            }
        }
        Sleep(kTargetPollMilliseconds);
    }

    controller->ShutdownRenderState();
    controller->RestoreSlots();
    append_log(
        module, wawvr::xr::LogLevel::Info,
        "Present-hook monitor stopped");
}

} // namespace wawvr::mod
