// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "t4/bindings.hpp"
#include "xr_types.h"

#include <cstdint>

namespace wawvr::mod {

inline constexpr wawvr::t4::Rva kCgAddPlayerWeaponRva = 0x000697A0;
inline constexpr wawvr::t4::Rva kGameplayRefdefOriginRva = 0x03120354;
inline constexpr wawvr::t4::Rva kGameplayRefdefAxisRvaForWeapon = 0x03120364;

enum class WeaponHookStatus : std::uint8_t {
    installed,
    already_installed,
    disabled_by_environment,
    rejected_wrong_profile,
    input_dependency_unavailable,
    preparation_failed,
    address_out_of_range,
    original_target_mismatch,
    original_sentinel_mismatch,
    jump_out_of_range,
    thread_suspend_failed,
    target_protection_failed,
    expected_bytes_changed,
    patch_write_failed,
    patch_cache_flush_failed,
    protection_restore_failed,
};

struct WeaponHookInstallResult final {
    WeaponHookStatus status{WeaponHookStatus::preparation_failed};
    std::uint32_t system_error{};
    std::uintptr_t target{};
    std::uintptr_t original{};
    std::uintptr_t ballistics_target{};
    std::uintptr_t ballistics_original{};
    std::uintptr_t spread_target{};
    std::uintptr_t spread_original{};
    std::uintptr_t client_effects_target{};
    std::uintptr_t client_effects_original{};
    std::uintptr_t client_spread_target{};
    std::uintptr_t client_spread_original{};

    [[nodiscard]] bool ok() const noexcept {
        return status == WeaponHookStatus::installed ||
               status == WeaponHookStatus::already_installed;
    }
};

enum class CampaignTargetingHookStatus : std::uint8_t {
    installed,
    already_installed,
    not_applicable,
    dependency_unavailable,
    rejected_wrong_profile,
    preparation_failed,
    address_out_of_range,
    original_target_mismatch,
    original_sentinel_mismatch,
    jump_out_of_range,
    thread_suspend_failed,
    target_protection_failed,
    expected_bytes_changed,
    patch_write_failed,
    patch_cache_flush_failed,
    protection_restore_failed,
};

struct CampaignTargetingHookInstallResult final {
    CampaignTargetingHookStatus status{
        CampaignTargetingHookStatus::preparation_failed};
    std::uint32_t system_error{};
    std::uintptr_t target{};
    std::uintptr_t original{};

    [[nodiscard]] bool ok() const noexcept {
        return status == CampaignTargetingHookStatus::installed ||
               status == CampaignTargetingHookStatus::already_installed ||
               status == CampaignTargetingHookStatus::not_applicable;
    }
};

[[nodiscard]] WeaponHookInstallResult install_weapon_viewmodel_hook(
    const wawvr::t4::ValidatedBindings& bindings) noexcept;

// SP-only script-angle substitution for Little Resistance's rocket_barrage
// designator. This patches only the GScr_GetPlayerAngles -> Scr_AddVector call;
// it never writes native player, camera, or HMD angles.
[[nodiscard]] CampaignTargetingHookInstallResult
install_campaign_rocket_targeting_hook(
    const wawvr::t4::ValidatedBindings& bindings) noexcept;

[[nodiscard]] const char* weapon_hook_status_name(
    WeaponHookStatus status) noexcept;

[[nodiscard]] const char* campaign_targeting_hook_status_name(
    CampaignTargetingHookStatus status) noexcept;

// Returns the most recent final visible weapon ray when it is a fresh render
// from this controller stream. T4 builds the next usercmd before rendering it,
// so exact generation equality would reject the normal one-frame-old result.
[[nodiscard]] bool read_final_visible_weapon_aim(
    std::uint64_t controller_generation,
    std::uint64_t now_milliseconds,
    float* pitch_degrees,
    float* yaw_degrees) noexcept;

// Returns the corrected viewmodel tag_flash only when it was published by a
// recent render from the caller's controller stream.
[[nodiscard]] bool read_published_weapon_muzzle(
    std::uint64_t controller_generation,
    std::uint64_t now_milliseconds,
    wawvr::xr::Vec3f* origin) noexcept;

void request_weapon_hook_shutdown() noexcept;
[[nodiscard]] bool weapon_viewmodel_hook_installed() noexcept;
[[nodiscard]] bool weapon_viewmodel_hook_enabled() noexcept;

}  // namespace wawvr::mod
