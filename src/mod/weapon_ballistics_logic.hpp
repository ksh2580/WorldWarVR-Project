// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xr_types.h"

#include <cstdint>

namespace wawvr::mod {

inline constexpr std::uint64_t kMaximumPublishedMuzzleAgeMilliseconds = 150;
inline constexpr std::uint64_t kMaximumPublishedMuzzleGenerationLag = 4;

struct PublishedWeaponMuzzleSnapshot final {
    bool valid{};
    std::uint64_t controller_generation{};
    std::uint64_t publication_milliseconds{};
    wawvr::xr::Vec3f origin{};
};

// Pure fail-closed freshness check shared by the game-thread hook and tests.
// T4 normally consumes the last rendered weapon pose a few command generations
// later, so a small bounded lag is accepted while future/stale publications are
// rejected.
[[nodiscard]] bool published_weapon_muzzle_is_fresh(
    const PublishedWeaponMuzzleSnapshot& muzzle,
    std::uint64_t controller_generation,
    std::uint64_t now_milliseconds) noexcept;

struct PhysicalMuzzleGate final {
    bool hook_enabled{};
    std::uintptr_t firing_entity{};
    std::uintptr_t local_player_entity{};
    std::int32_t entity_number{};
    bool has_client{};
    std::int32_t weapon_type{};
    bool controller_frame_current{};
    bool published_muzzle_fresh{};
};

// Weapon type zero is T4's ordinary bullet path and type two is its projectile
// path (including the Zombies Ray Gun) at the audited FireWeapon callsite.
// Both consume the WeaponParms filled by CalcMuzzlePoints; other weapon types
// retain the native origin.
[[nodiscard]] bool physical_muzzle_gate_allows(
    const PhysicalMuzzleGate& gate) noexcept;

inline constexpr float kMaximumReasonableAdsSpreadDegrees = 180.0F;

struct FixedAdsSpreadGate final {
    bool hook_enabled{};
    std::uintptr_t attacker{};
    std::uintptr_t local_player_entity{};
    std::int32_t entity_number{};
    bool has_client{};
    std::int32_t weapon_type{};
    float ads_spread_degrees{};
};

// Replaces only the authoritative per-shot cone scalar passed from
// FireWeapon to Bullet_Fire. There is deliberately no ADS/player-state input:
// locomotion, FOV, sensitivity, animation, recoil and ammo remain native.
// A nonzero per-weapon ADS value is retained, so shotgun pellets do not
// collapse onto a single ray. Rejected decisions leave `spread_degrees`
// bit-for-bit unchanged.
[[nodiscard]] bool apply_fixed_ads_spread_override(
    const FixedAdsSpreadGate& gate,
    float* spread_degrees) noexcept;

struct FixedAdsVisualSpreadGate final {
    bool hook_enabled{};
    std::int32_t weapon_type{};
    float ads_spread_degrees{};
};

// CG_DrawBulletImpacts obtains local client min/max spread independently of
// the authoritative Bullet_Fire call. Collapse both visual bounds to the
// weapon's authored ADS cone so tracers and predicted impacts follow the same
// tracked shot without changing pellet count or native weapon state.
[[nodiscard]] bool apply_fixed_ads_visual_spread_override(
    const FixedAdsVisualSpreadGate& gate,
    float* minimum_spread_degrees,
    float* maximum_spread_degrees) noexcept;

}  // namespace wawvr::mod
