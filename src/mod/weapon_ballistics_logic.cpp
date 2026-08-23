// SPDX-License-Identifier: GPL-3.0-only
#include "weapon_ballistics_logic.hpp"

#include <cmath>

namespace wawvr::mod {

bool published_weapon_muzzle_is_fresh(
    const PublishedWeaponMuzzleSnapshot& muzzle,
    const std::uint64_t controller_generation,
    const std::uint64_t now_milliseconds) noexcept {
    return muzzle.valid && muzzle.controller_generation != 0 &&
           controller_generation != 0 && now_milliseconds != 0 &&
           muzzle.controller_generation <= controller_generation &&
           controller_generation - muzzle.controller_generation <=
               kMaximumPublishedMuzzleGenerationLag &&
           muzzle.publication_milliseconds != 0 &&
           muzzle.publication_milliseconds <= now_milliseconds &&
           now_milliseconds - muzzle.publication_milliseconds <=
               kMaximumPublishedMuzzleAgeMilliseconds &&
           std::isfinite(muzzle.origin.x) &&
           std::isfinite(muzzle.origin.y) &&
           std::isfinite(muzzle.origin.z);
}

bool physical_muzzle_gate_allows(
    const PhysicalMuzzleGate& gate) noexcept {
    return gate.hook_enabled && gate.firing_entity != 0 &&
           gate.firing_entity == gate.local_player_entity &&
           gate.entity_number == 0 && gate.has_client &&
           (gate.weapon_type == 0 || gate.weapon_type == 2) &&
           gate.controller_frame_current &&
           gate.published_muzzle_fresh;
}

bool apply_fixed_ads_spread_override(
    const FixedAdsSpreadGate& gate,
    float* const spread_degrees) noexcept {
    if (spread_degrees == nullptr || !gate.hook_enabled ||
        gate.attacker == 0 || gate.local_player_entity == 0 ||
        gate.attacker != gate.local_player_entity ||
        gate.entity_number != 0 || !gate.has_client ||
        gate.weapon_type != 0 ||
        !std::isfinite(gate.ads_spread_degrees) ||
        gate.ads_spread_degrees < 0.0F ||
        gate.ads_spread_degrees > kMaximumReasonableAdsSpreadDegrees) {
        return false;
    }

    *spread_degrees = gate.ads_spread_degrees;
    return true;
}

bool apply_fixed_ads_visual_spread_override(
    const FixedAdsVisualSpreadGate& gate,
    float* const minimum_spread_degrees,
    float* const maximum_spread_degrees) noexcept {
    if (minimum_spread_degrees == nullptr ||
        maximum_spread_degrees == nullptr || !gate.hook_enabled ||
        gate.weapon_type != 0 ||
        !std::isfinite(gate.ads_spread_degrees) ||
        gate.ads_spread_degrees < 0.0F ||
        gate.ads_spread_degrees > kMaximumReasonableAdsSpreadDegrees) {
        return false;
    }
    *minimum_spread_degrees = gate.ads_spread_degrees;
    *maximum_spread_degrees = gate.ads_spread_degrees;
    return true;
}

}  // namespace wawvr::mod
