#include "bootstrap.hpp"
#include "direct_boot_patch.hpp"

#if defined(WAWVR_HAS_INPUT)
#include "input_hook.hpp"
#include "weapon_hook.hpp"
#endif

#if defined(WAWVR_HAS_XR) && defined(WAWVR_HAS_T4_BINDINGS)
#include "frame_boundary_hook.hpp"
#include "lod_fov_clamp.hpp"
#include "t4_hud_placement.hpp"
#include "t4_menu_input.hpp"
#include "t4_presentation_state.hpp"
#include "weapon_camera_patch.hpp"
#endif
#if defined(WAWVR_HAS_XR)
#include "present_hook.hpp"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#if defined(WAWVR_HAS_T4_BINDINGS)
#include "t4_layout_selector.hpp"
#include "t4/profile.hpp"
#include "t4/runtime_win32.hpp"
#endif

namespace wawvr::mod {
namespace {

std::atomic<bool> g_bootstrap_complete{false};
std::atomic<bool> g_game_build_validated{false};

bool direct_boot_disabled_by_environment() noexcept {
    std::array<wchar_t, 8> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"WAWVR_DISABLE_DIRECT_BOOT", value.data(),
        static_cast<DWORD>(value.size()));
    return length == 1 && value[0] == L'1';
}

std::filesystem::path module_directory(HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path.data(), path.data() + length).parent_path();
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

std::filesystem::path bootstrap_log_path(HMODULE module) {
    return module_directory(module) / "WorldWarVR.log";
}

void write_bootstrap_header(HMODULE module) {
    std::ofstream log(bootstrap_log_path(module), std::ios::app);
    if (log) {
        log << '[' << timestamp() << "] World War VR DLL loaded\n";
    }

    OutputDebugStringW(L"WorldAtWarVR: bootstrap DLL loaded\n");
}

bool validate_game_build(HMODULE module) {
    std::ofstream log(bootstrap_log_path(module), std::ios::app);

#if defined(WAWVR_HAS_T4_BINDINGS)
    const auto result =
        wawvr::t4::validate_and_bind_supported_current_process();

    if (!result.capture_error.empty()) {
        if (log) {
            log << "T4 process capture failed: " << result.capture_error << '\n';
        }
        OutputDebugStringW(L"WorldAtWarVR: process capture failed; hooks disabled\n");
        return false;
    }

    if (!result.ok()) {
        if (log) {
            log << "T4 executable validation failed; hooks disabled\n";
            for (const auto& issue : result.validation.issues) {
                log << "  - " << issue.message << '\n';
            }
        }
        OutputDebugStringW(L"WorldAtWarVR: unsupported executable; hooks disabled\n");
        return false;
    }

    const auto mod_layout =
        select_t4_layout_family(result.bindings->profile());
    if (mod_layout == T4LayoutFamily::unsupported) {
        if (log) {
            log << "validated executable selected an unsupported or inconsistent mod layout; hooks disabled\n";
        }
        OutputDebugStringW(
            L"WorldAtWarVR: executable layout configuration failed; hooks disabled\n");
        return false;
    }

#if defined(WAWVR_HAS_XR)
    const bool renderer_hooks_configured = configure_renderer_hooks(
        result.bindings->profile().layout);
    if (!renderer_hooks_configured) {
        if (log) {
            log << "renderer profile configuration rejected; hooks disabled\n";
        }
        OutputDebugStringW(
            L"WorldAtWarVR: renderer profile configuration failed; hooks disabled\n");
        return false;
    }
#endif
    g_game_build_validated.store(true, std::memory_order_release);
    DirectBootPatchResult direct_boot{};
    if (mod_layout == T4LayoutFamily::multiplayer_1_7_1263) {
        direct_boot.status = DirectBootPatchStatus::not_applicable;
    } else if (direct_boot_disabled_by_environment()) {
        direct_boot.status = DirectBootPatchStatus::disabled_by_environment;
    } else {
        direct_boot = install_direct_boot_patch(*result.bindings);
    }
#if defined(WAWVR_HAS_XR)
    const LodFovClampPatchResult lod_fov_clamp_patch =
        install_lod_fov_clamp_patch(*result.bindings);
    const bool t4_presentation_state_bound =
        bind_t4_presentation_state(*result.bindings);
    const bool t4_menu_input_bound =
        bind_t4_menu_input(*result.bindings);
    const T4HudPlacementBindResult t4_hud_placement =
        bind_t4_hud_placement(*result.bindings);
    T4HudPlacementDrawHookInstallResult t4_hud_draw_hook{};
    if (t4_hud_placement.ok()) {
        t4_hud_draw_hook =
            install_t4_hud_placement_draw_hook(*result.bindings);
    } else {
        t4_hud_draw_hook.status =
            T4HudPlacementDrawHookStatus::placement_not_bound;
    }
    const FrameBoundaryHookInstallResult frame_boundary_hook =
        install_frame_boundary_hook(*result.bindings);
    const MeleeCameraPatchResult melee_camera_patch =
        install_melee_camera_patch(*result.bindings);
    WeaponCameraPatchResult weapon_camera_patch{};
    if (frame_boundary_hook.ok()) {
        weapon_camera_patch = install_weapon_camera_patch(*result.bindings);
    } else {
        weapon_camera_patch.status =
            WeaponCameraPatchStatus::frame_boundary_unavailable;
    }
#endif
#if defined(WAWVR_HAS_INPUT)
    const InputHookInstallResult input_hook =
        install_controller_input_hook(*result.bindings);
    WeaponHookInstallResult weapon_hook{};
    if (input_hook.ok()) {
        weapon_hook = install_weapon_viewmodel_hook(*result.bindings);
    } else {
        weapon_hook.status = WeaponHookStatus::input_dependency_unavailable;
    }
    CampaignTargetingHookInstallResult campaign_targeting_hook{};
    if (input_hook.ok() && weapon_hook.ok()) {
        campaign_targeting_hook =
            install_campaign_rocket_targeting_hook(*result.bindings);
    } else {
        campaign_targeting_hook.status =
            CampaignTargetingHookStatus::dependency_unavailable;
    }
#endif
    if (log) {
        log << "T4 executable profile validated: "
            << result.bindings->profile().id << '\n';

        log << "direct-boot patch: profile=" << result.bindings->profile().id
            << " context-rva=0x" << std::hex << std::uppercase
            << kStartupIntroContextRva
            << " branch-rva=0x" << kStartupIntroBranchRva
            << " expected-opcode=0x"
            << static_cast<unsigned int>(kStartupIntroConditionalBranch)
            << " replacement-opcode=0x"
            << static_cast<unsigned int>(kStartupIntroUnconditionalBranch)
            << std::dec << " status="
            << direct_boot_patch_status_name(direct_boot.status);
        if (direct_boot.system_error != 0) {
            log << " win32-error=" << direct_boot.system_error;
        }
        if (!direct_boot.ok() && direct_boot.observed_size != 0) {
            log << " observed-context=";
            for (std::size_t index = 0; index < direct_boot.observed_size; ++index) {
                if (index != 0) {
                    log << ' ';
                }
                log << std::hex << std::uppercase << std::setw(2)
                    << std::setfill('0')
                    << static_cast<unsigned int>(direct_boot.observed[index]);
            }
            log << std::dec << std::setfill(' ');
        }
        log << '\n';
        if (direct_boot.status == DirectBootPatchStatus::not_applicable) {
            log << "direct-boot behavior: not applicable to multiplayer\n";
        } else if (direct_boot.ok()) {
            log << "direct-boot behavior: suppressed startup command "
                   "`cinematic Treyarch`; launcher +devmap remains eligible\n";
        } else {
            log << "direct-boot behavior: patch rejected; executable memory "
                   "was left or restored to its original behavior\n";
        }
#if defined(WAWVR_HAS_INPUT)
        log << "controller-input hook: profile="
            << result.bindings->profile().id
            << " site-rva=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::post_build_usercmd)->rva)
            << " camera-axis-rva=0x"
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_data_symbol(
                       result.bindings->profile(),
                       wawvr::t4::DataSymbolId::gameplay_refdef_viewport)->rva +
                   kGameplayRefdefAxisOffset)
            << " target=0x" << input_hook.target
            << " trampoline=0x" << input_hook.trampoline
            << std::dec << " status="
            << input_hook_status_name(input_hook.status);
        if (input_hook.system_error != 0) {
            log << " win32-error=" << input_hook.system_error;
        }
        log << '\n';
        log << "controller-weapon viewmodel hook: profile="
            << result.bindings->profile().id
            << " site-rva=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::viewmodel_weapon_call)->rva)
            << " target=0x" << weapon_hook.target
            << " original=0x" << weapon_hook.original
            << " ballistics-target=0x" << weapon_hook.ballistics_target
            << " calc-muzzle=0x" << weapon_hook.ballistics_original
            << " spread-target=0x" << weapon_hook.spread_target
            << " bullet-fire=0x" << weapon_hook.spread_original
            << " client-effects-target=0x"
            << weapon_hook.client_effects_target
            << " client-view-origin=0x"
            << weapon_hook.client_effects_original
            << " client-spread-target=0x"
            << weapon_hook.client_spread_target
            << " get-spread=0x"
            << weapon_hook.client_spread_original
            << std::dec << " status="
            << weapon_hook_status_name(weapon_hook.status);
        if (weapon_hook.system_error != 0) {
            log << " win32-error=" << weapon_hook.system_error;
        }
        log << '\n';
        log << "campaign rocket-barrage targeting hook: profile="
            << result.bindings->profile().id
            << " target=0x" << std::hex << std::uppercase
            << campaign_targeting_hook.target
            << " Scr_AddVector=0x" << campaign_targeting_hook.original
            << std::dec << " status="
            << campaign_targeting_hook_status_name(
                   campaign_targeting_hook.status);
        if (campaign_targeting_hook.system_error != 0) {
            log << " win32-error="
                << campaign_targeting_hook.system_error;
        }
        log << '\n';
#endif
#if defined(WAWVR_HAS_XR)
        log << "stereo-scene LOD tanHalfFovY clamp: profile="
            << result.bindings->profile().id
            << " site-rva=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::lod_tan_half_fov_y_load)->rva)
            << " target=0x" << lod_fov_clamp_patch.target
            << " continuation=0x" << lod_fov_clamp_patch.continuation
            << std::dec << " status="
            << lod_fov_clamp_patch_status_name(
                   lod_fov_clamp_patch.status);
        if (lod_fov_clamp_patch.system_error != 0) {
            log << " win32-error=" << lod_fov_clamp_patch.system_error;
        }
        log << '\n';
        log << "T4 presentation-state binding: key-catchers-rva=0x"
            << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_data_symbol(
                       result.bindings->profile(),
                       wawvr::t4::DataSymbolId::key_catchers)->rva)
            << " connection-state-rva=0x"
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_data_symbol(
                       result.bindings->profile(),
                       wawvr::t4::DataSymbolId::connection_state)->rva)
            << std::dec << " status="
            << (t4_presentation_state_bound ? "bound" : "unavailable")
            << '\n';
        log << "T4 native menu input: CL_KeyEvent-rva=0x"
            << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::cl_key_event_entry_sentinel)
                       ->rva)
            << " UI_MouseEvent-rva=0x"
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::ui_mouse_event_entry_sentinel)
                       ->rva)
            << " Cbuf_AddText-rva=0x"
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::cbuf_add_text_entry_sentinel)
                       ->rva)
            << std::dec << " status="
            << (t4_menu_input_bound ? "bound" : "unavailable")
            << '\n';
        log << "T4 binocular HUD placement: setup-rva=0x"
            << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::
                           scr_place_setup_float_viewport_entry_sentinel)
                       ->rva)
            << " scr-place-rva=0x"
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_data_symbol(
                       result.bindings->profile(),
                       wawvr::t4::DataSymbolId::scr_place_view_zero)->rva)
            << " refdef-viewport-rva=0x"
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_data_symbol(
                       result.bindings->profile(),
                       wawvr::t4::DataSymbolId::gameplay_refdef_viewport)->rva)
            << std::dec << " status="
            << t4_hud_placement_bind_status_name(t4_hud_placement.status)
            << '\n';
        log << "same-frame CG_Draw2D HUD placement boundary: profile="
            << result.bindings->profile().id
            << " callsite-rva=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::cg_draw_2d_call)->rva)
            << " target=0x" << t4_hud_draw_hook.target
            << " original=0x" << t4_hud_draw_hook.original
            << std::dec << " status="
            << t4_hud_placement_draw_hook_status_name(
                   t4_hud_draw_hook.status);
        if (t4_hud_draw_hook.system_error != 0) {
            log << " win32-error=" << t4_hud_draw_hook.system_error;
        }
        log << '\n';
        log << "post-Com_Frame XR boundary: profile="
            << result.bindings->profile().id
            << " callsite-rva=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::main_loop_com_frame_call)->rva)
            << " target=0x" << frame_boundary_hook.target
            << " original=0x" << frame_boundary_hook.original
            << std::dec << " status="
            << frame_boundary_hook_status_name(frame_boundary_hook.status);
        if (frame_boundary_hook.system_error != 0) {
            log << " win32-error=" << frame_boundary_hook.system_error;
        }
        log << '\n';
        log << "weapon tag_camera suppression: profile="
            << result.bindings->profile().id
            << " site-rva=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::viewmodel_camera_tag_matrix_call)
                       ->rva)
            << " target=0x" << weapon_camera_patch.target
            << std::dec << " status="
            << weapon_camera_patch_status_name(weapon_camera_patch.status);
        if (weapon_camera_patch.system_error != 0) {
            log << " win32-error=" << weapon_camera_patch.system_error;
        }
        log << '\n';
        if (weapon_camera_patch.ok() &&
            weapon_camera_patch.status !=
                WeaponCameraPatchStatus::frame_boundary_unavailable) {
            log << "weapon-camera behavior: native viewmodel animation retained; "
                   "tag_camera cannot move or roll the VR refdef during reload, "
                   "sprint, or first spawn\n";
        }
        log << "auto-melee target-aim suppression: profile="
            << result.bindings->profile().id
            << " site-rva=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(
                   wawvr::t4::find_hook_site(
                       result.bindings->profile(),
                       wawvr::t4::HookSiteId::auto_melee_enabled_branch)->rva)
            << " target=0x" << melee_camera_patch.target
            << std::dec << " status="
            << melee_camera_patch_status_name(melee_camera_patch.status);
        if (melee_camera_patch.system_error != 0) {
            log << " win32-error=" << melee_camera_patch.system_error;
        }
        log << '\n';
        if (melee_camera_patch.ok()) {
            log << "melee-camera behavior: target pitch/yaw and command charge "
                   "lunge are suppressed; native knife action remains enabled\n";
        }
        log << "renderer-bootstrap=present-capture + post-Com_Frame-openxr\n";
#else
        log << "OpenXR target was not included; renderer hooks remain disabled\n";
#endif
#if defined(WAWVR_HAS_XR)
    if (!lod_fov_clamp_patch.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: stereo-scene LOD-only FOV clamp rejected; stock LOD preserved\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: exact stereo-scene LOD-only FOV clamp installed\n");
    }
    if (!frame_boundary_hook.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: post-Com_Frame hook rejected; XR renderer disabled\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: exact post-Com_Frame XR boundary installed\n");
    }
    if (!t4_hud_draw_hook.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: same-frame CG_Draw2D HUD placement hook rejected\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: exact same-frame CG_Draw2D HUD placement boundary installed\n");
    }
    if (weapon_camera_patch.status ==
            WeaponCameraPatchStatus::disabled_by_environment) {
        OutputDebugStringW(
            L"WorldAtWarVR: weapon-camera stabilization disabled by environment\n");
    } else if (!weapon_camera_patch.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: weapon tag_camera patch rejected; stock camera preserved\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: reload/sprint tag_camera motion suppressed\n");
    }
    if (melee_camera_patch.status ==
            MeleeCameraPatchStatus::disabled_by_environment) {
        OutputDebugStringW(
            L"WorldAtWarVR: auto-melee camera suppression disabled by environment\n");
    } else if (!melee_camera_patch.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: auto-melee camera patch rejected; command lunge clear remains active\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: auto-melee target camera rotation suppressed\n");
    }
#endif
    }
    if (direct_boot.status == DirectBootPatchStatus::not_applicable) {
        OutputDebugStringW(
            L"WorldAtWarVR: SP direct-boot patch not applicable in multiplayer\n");
    } else if (direct_boot.status == DirectBootPatchStatus::disabled_by_environment) {
        OutputDebugStringW(
            L"WorldAtWarVR: direct-boot patch disabled by environment\n");
    } else if (!direct_boot.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: direct-boot patch rejected; continuing without it\n");
    } else {
        OutputDebugStringW(L"WorldAtWarVR: direct-map boot patch installed\n");
    }
#if defined(WAWVR_HAS_INPUT)
    if (input_hook.status == InputHookStatus::disabled_by_environment) {
        OutputDebugStringW(
            L"WorldAtWarVR: controller input disabled by environment\n");
    } else if (!input_hook.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: controller input hook rejected; native input preserved\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: exact T4 controller usercmd hook installed\n");
    }
    if (weapon_hook.status == WeaponHookStatus::disabled_by_environment) {
        OutputDebugStringW(
            L"WorldAtWarVR: controller weapon viewmodel disabled by environment\n");
    } else if (!weapon_hook.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: controller weapon viewmodel hook rejected; stock weapon preserved\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: tracked-controller weapon viewmodel hook installed\n");
    }
    if (campaign_targeting_hook.status ==
        CampaignTargetingHookStatus::not_applicable) {
        OutputDebugStringW(
            L"WorldAtWarVR: campaign rocket targeting is not applicable in multiplayer\n");
    } else if (!campaign_targeting_hook.ok()) {
        OutputDebugStringW(
            L"WorldAtWarVR: campaign rocket targeting hook rejected; native script angles preserved\n");
    } else {
        OutputDebugStringW(
            L"WorldAtWarVR: Little Resistance rocket targeting follows the tracked controller\n");
    }
#endif
    OutputDebugStringW(L"WorldAtWarVR: exact T4 build validated\n");
    return true;
#else
    if (log) {
        log << "T4 bindings were not included in this build; hooks disabled\n";
    }
    return false;
#endif
}

} // namespace

DWORD WINAPI bootstrap_thread(void* module_handle) noexcept {
    try {
        const auto module = static_cast<HMODULE>(module_handle);
        write_bootstrap_header(module);
        const bool validated = validate_game_build(module);
        g_bootstrap_complete.store(true, std::memory_order_release);
#if defined(WAWVR_HAS_XR) && defined(WAWVR_HAS_T4_BINDINGS)
        if (validated && frame_boundary_hook_installed()) {
            run_present_hook_monitor(module);
        }
        // The Present monitor removes the stereo-scene hook first, so no new
        // thread-local stereo clamp scopes can begin before this exact E9 is
        // restored. Any residual in-flight bridge is handled fail-closed by
        // peer-thread quiescence.
        const LodFovClampRestoreStatus lod_fov_clamp_restore =
            restore_lod_fov_clamp_patch();
        // Request the final full-width write while the validated WinMain
        // boundary is still callable. The bootstrap thread only waits; it
        // never invokes the proprietary setup ABI itself.
        constexpr std::uint32_t kHudQuiesceTimeoutMilliseconds = 2'000;
        const T4HudPlacementQuiesceResult hud_quiesce_wait =
            request_t4_hud_placement_quiesce(
                kHudQuiesceTimeoutMilliseconds);
        const WeaponCameraPatchResult camera_restore =
            restore_weapon_camera_patch();
        const MeleeCameraPatchResult melee_camera_restore =
            restore_melee_camera_patch();
        // Capture an acknowledgement that may have arrived while the camera
        // patch was being restored. A zero-duration second wait does not call
        // engine code or extend shutdown latency.
        const T4HudPlacementQuiesceResult hud_quiesce_cleanup =
            hud_quiesce_wait.ok()
                ? hud_quiesce_wait
                : request_t4_hud_placement_quiesce(0);
        T4HudPlacementQuiesceResult hud_quiesce_final =
            hud_quiesce_cleanup;
        T4HudPlacementDrawHookRestoreStatus hud_draw_restore =
            T4HudPlacementDrawHookRestoreStatus::not_installed;
        FrameBoundaryRestoreStatus boundary_restore =
            FrameBoundaryRestoreStatus::not_installed;
        if (hud_quiesce_cleanup.ok()) {
            request_frame_boundary_hook_shutdown();
            request_t4_hud_placement_draw_hook_shutdown();
            // Taking the central exclusive lock here drains any service that
            // had already passed an outer gate. Late entrants then see
            // disabled state without reading the binding snapshot.
            hud_quiesce_final =
                disable_t4_hud_placement_services_and_drain();
            hud_draw_restore = restore_t4_hud_placement_draw_hook();
            boundary_restore = restore_frame_boundary_hook();
        }
        if (hud_quiesce_cleanup.ok() && hud_quiesce_final.ok()) {
            clear_t4_hud_placement();
        }
        clear_t4_menu_input();
        clear_t4_presentation_state();
        std::ofstream log(bootstrap_log_path(module), std::ios::app);
        if (log) {
            log << "stereo-scene LOD tanHalfFovY clamp restore: "
                << lod_fov_clamp_restore_status_name(
                       lod_fov_clamp_restore)
                << '\n';
            log << "weapon tag_camera suppression restore: "
                << weapon_camera_patch_status_name(camera_restore.status)
                << '\n';
            log << "auto-melee target-aim suppression restore: "
                << melee_camera_patch_status_name(
                       melee_camera_restore.status)
                << '\n';
            log << "post-Com_Frame hook restore: "
                << frame_boundary_restore_status_name(boundary_restore)
                << '\n';
            log << "same-frame CG_Draw2D HUD placement hook restore: "
                << t4_hud_placement_draw_hook_restore_status_name(
                       hud_draw_restore)
                << '\n';
            log << "scrPlaceView[0] game-thread quiesce wait: "
                << t4_hud_placement_quiesce_status_name(
                       hud_quiesce_wait.status)
                << " apply="
                << t4_hud_placement_apply_status_name(
                       hud_quiesce_wait.apply_status)
                << " wait-result=0x" << std::hex << std::uppercase
                << hud_quiesce_wait.wait_result << std::dec << '\n';
            log << "scrPlaceView[0] final drain: "
                << t4_hud_placement_quiesce_status_name(
                       hud_quiesce_final.status)
                << " apply="
                << t4_hud_placement_apply_status_name(
                       hud_quiesce_final.apply_status)
                << " binding="
                << (hud_quiesce_cleanup.ok() && hud_quiesce_final.ok()
                        ? "cleared"
                        : "retained")
                << " cleanup="
                << (hud_quiesce_cleanup.ok() ? "completed" : "deferred")
                << '\n';
        }
#elif defined(WAWVR_HAS_XR)
        static_cast<void>(validated);
#else
        static_cast<void>(validated);
#endif
    } catch (...) {
        OutputDebugStringW(L"WorldAtWarVR: bootstrap logging failed\n");
        g_bootstrap_complete.store(true, std::memory_order_release);
    }
    return 0;
}

bool bootstrap_complete() noexcept {
    return g_bootstrap_complete.load(std::memory_order_acquire);
}

bool game_build_validated() noexcept {
    return g_game_build_validated.load(std::memory_order_acquire);
}

} // namespace wawvr::mod
