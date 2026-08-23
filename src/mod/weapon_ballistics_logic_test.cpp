#include "weapon_ballistics_logic.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace wawvr::mod;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

[[nodiscard]] PublishedWeaponMuzzleSnapshot fresh_muzzle() noexcept {
    return {
        true,
        100,
        1'000,
        {10.0F, 20.0F, 30.0F},
    };
}

void test_publication_freshness() {
    const auto fresh = fresh_muzzle();
    check(published_weapon_muzzle_is_fresh(fresh, 100, 1'000),
          "exact generation and timestamp are fresh");
    check(published_weapon_muzzle_is_fresh(fresh, 104, 1'150),
          "bounded generation and time lag are accepted");
    check(!published_weapon_muzzle_is_fresh(fresh, 105, 1'150),
          "excessive generation lag is rejected");
    check(!published_weapon_muzzle_is_fresh(fresh, 99, 1'001),
          "future publication generation is rejected");
    check(!published_weapon_muzzle_is_fresh(fresh, 100, 1'151),
          "stale publication time is rejected");
    check(!published_weapon_muzzle_is_fresh(fresh, 100, 999),
          "clock rollback is rejected");
    check(!published_weapon_muzzle_is_fresh(fresh, 0, 1'000),
          "zero controller generation is rejected");

    auto invalid = fresh;
    invalid.valid = false;
    check(!published_weapon_muzzle_is_fresh(invalid, 100, 1'000),
          "invalidated publication is rejected");
    invalid = fresh;
    invalid.publication_milliseconds = 0;
    check(!published_weapon_muzzle_is_fresh(invalid, 100, 1'000),
          "zero publication time is rejected");
    invalid = fresh;
    invalid.origin.x = std::numeric_limits<float>::quiet_NaN();
    check(!published_weapon_muzzle_is_fresh(invalid, 100, 1'000),
          "non-finite origin is rejected");
}

void test_local_bullet_gate() {
    PhysicalMuzzleGate gate{
        true,
        0x0176C6F0,
        0x0176C6F0,
        0,
        true,
        0,
        true,
        true,
    };
    check(physical_muzzle_gate_allows(gate),
          "audited local bullet path is accepted");
    auto projectile = gate;
    projectile.weapon_type = 2;
    check(physical_muzzle_gate_allows(projectile),
          "audited local projectile path accepts the Ray Gun muzzle");

    auto rejected = gate;
    rejected.hook_enabled = false;
    check(!physical_muzzle_gate_allows(rejected),
          "disabled hook is rejected");
    rejected = gate;
    rejected.firing_entity += 0x378;
    check(!physical_muzzle_gate_allows(rejected),
          "non-local entity is rejected");
    rejected = gate;
    rejected.entity_number = 1;
    check(!physical_muzzle_gate_allows(rejected),
          "nonzero entity number is rejected");
    rejected = gate;
    rejected.has_client = false;
    check(!physical_muzzle_gate_allows(rejected),
          "entity without client is rejected");
    rejected = gate;
    rejected.weapon_type = 1;
    check(!physical_muzzle_gate_allows(rejected),
          "unsupported non-bullet/non-projectile weapon path is rejected");
    rejected = gate;
    rejected.controller_frame_current = false;
    check(!physical_muzzle_gate_allows(rejected),
          "stale controller state is rejected");
    rejected = gate;
    rejected.published_muzzle_fresh = false;
    check(!physical_muzzle_gate_allows(rejected),
          "stale muzzle publication is rejected");
}

[[nodiscard]] FixedAdsSpreadGate accepted_spread_gate() noexcept {
    return {
        true,
        0x0176C6F0,
        0x0176C6F0,
        0,
        true,
        0,
        0.4F,
    };
}

void check_rejected_spread_is_unchanged(
    const FixedAdsSpreadGate& gate,
    const std::string_view message) {
    float spread = 7.25F;
    const auto before = std::bit_cast<std::uint32_t>(spread);
    check(!apply_fixed_ads_spread_override(gate, &spread), message);
    check(std::bit_cast<std::uint32_t>(spread) == before,
          "rejected spread decision preserves native bits");
}

void test_fixed_ads_spread_override() {
    const auto gate = accepted_spread_gate();
    constexpr std::array<float, 4> native_spreads{2.0F, 4.0F, 8.0F, 12.0F};
    for (const float native : native_spreads) {
        float spread = native;
        check(apply_fixed_ads_spread_override(gate, &spread),
              "eligible local VR bullet receives ADS spread");
        check(spread == gate.ads_spread_degrees,
              "movement and firing cone growth cannot change final spread");
    }

    auto shotgun = gate;
    shotgun.ads_spread_degrees = 3.0F;
    float shotgun_spread = 11.0F;
    check(apply_fixed_ads_spread_override(shotgun, &shotgun_spread),
          "eligible shotgun spread is overridden");
    check(shotgun_spread == 3.0F,
          "authored nonzero ADS pellet cone remains nonzero");

    check(!apply_fixed_ads_spread_override(gate, nullptr),
          "null spread output is rejected");

    auto rejected = gate;
    rejected.hook_enabled = false;
    check_rejected_spread_is_unchanged(rejected, "disabled hook is rejected");
    rejected = gate;
    rejected.attacker = 0;
    check_rejected_spread_is_unchanged(rejected, "null attacker is rejected");
    rejected = gate;
    rejected.local_player_entity = 0;
    check_rejected_spread_is_unchanged(rejected, "null local entity is rejected");
    rejected = gate;
    rejected.attacker += 0x378;
    check_rejected_spread_is_unchanged(rejected, "AI attacker is rejected");
    rejected = gate;
    rejected.entity_number = 1;
    check_rejected_spread_is_unchanged(rejected, "nonlocal entity number is rejected");
    rejected = gate;
    rejected.has_client = false;
    check_rejected_spread_is_unchanged(rejected, "attacker without client is rejected");
    rejected = gate;
    rejected.weapon_type = 1;
    check_rejected_spread_is_unchanged(rejected, "projectile weapon is rejected");
    rejected = gate;
    rejected.ads_spread_degrees = -0.01F;
    check_rejected_spread_is_unchanged(rejected, "negative ADS spread is rejected");
    rejected = gate;
    rejected.ads_spread_degrees = std::numeric_limits<float>::infinity();
    check_rejected_spread_is_unchanged(rejected, "infinite ADS spread is rejected");
    rejected = gate;
    rejected.ads_spread_degrees = std::numeric_limits<float>::quiet_NaN();
    check_rejected_spread_is_unchanged(rejected, "NaN ADS spread is rejected");
    rejected = gate;
    rejected.ads_spread_degrees = kMaximumReasonableAdsSpreadDegrees + 1.0F;
    check_rejected_spread_is_unchanged(rejected, "implausible ADS spread is rejected");
}

void test_fixed_ads_visual_spread_override() {
    FixedAdsVisualSpreadGate gate{
        true,
        0,
        1.25F,
    };
    float minimum = 4.0F;
    float maximum = 9.0F;
    check(apply_fixed_ads_visual_spread_override(
              gate, &minimum, &maximum) &&
              minimum == 1.25F && maximum == 1.25F,
          "local tracer and predicted impact use the authored ADS cone");

    auto rejected = gate;
    rejected.weapon_type = 2;
    minimum = 4.0F;
    maximum = 9.0F;
    check(!apply_fixed_ads_visual_spread_override(
              rejected, &minimum, &maximum) &&
              minimum == 4.0F && maximum == 9.0F,
          "projectile visuals keep native non-hitscan spread values");
    rejected = gate;
    rejected.ads_spread_degrees =
        std::numeric_limits<float>::quiet_NaN();
    check(!apply_fixed_ads_visual_spread_override(
              rejected, &minimum, &maximum),
          "non-finite visual ADS spread fails closed");
    check(!apply_fixed_ads_visual_spread_override(
              gate, nullptr, &maximum) &&
              !apply_fixed_ads_visual_spread_override(
                  gate, &minimum, nullptr),
          "null visual spread outputs fail closed");
}

}  // namespace

int main() {
    test_publication_freshness();
    test_local_bullet_gate();
    test_fixed_ads_spread_override();
    test_fixed_ads_visual_spread_override();
    return 0;
}
