// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "controller_state.hpp"

#include "t4/usercmd.hpp"
#include "xr_types.h"

#include <cstdint>

namespace wawvr::mod {

inline constexpr std::uint64_t kMaximumControllerFrameAgeMilliseconds = 250;
inline constexpr float kControllerStickDeadzone = 0.20F;
inline constexpr float kControllerButtonThreshold = 0.55F;
inline constexpr float kSnapTurnEngageThreshold = 0.75F;
inline constexpr float kSnapTurnReleaseThreshold = 0.35F;
inline constexpr float kSnapTurnVerticalDominanceMargin = 0.15F;
inline constexpr float kSnapTurnDegrees = 45.0F;
inline constexpr float kBodyYawSyncMinimumDegrees = 0.25F;
inline constexpr std::uint32_t kSnapTurnBlockedKeyCatcherMask = 0x08U | 0x10U;
inline constexpr std::int32_t kT4SpActiveConnectionState = 10;
inline constexpr std::int32_t kT4MpActiveConnectionState = 10;
// Kept as a source-compatible alias for the established SP tests/callers.
inline constexpr std::int32_t kT4ActiveConnectionState =
    kT4SpActiveConnectionState;

struct SnapTurnState final {
    bool armed{true};
};

struct SprintLatchState final {
    bool input_was_owned{};
    bool click_was_held{};
    bool latched{};
    std::uint64_t last_action_sequence{};
};

inline constexpr std::uint64_t kPhysicalMeleeMinimumSampleMilliseconds = 8;
inline constexpr std::uint64_t kPhysicalMeleeMaximumSampleMilliseconds = 150;
inline constexpr std::uint64_t kPhysicalMeleeCooldownMilliseconds = 450;
inline constexpr float kPhysicalMeleeMinimumTravelMeters = 0.04F;
inline constexpr float kPhysicalMeleeMinimumSpeedMetersPerSecond = 0.90F;

struct PhysicalMeleeState final {
    bool input_was_owned{};
    bool pose_was_valid{};
    wawvr::xr::Vec3f previous_hand_relative_to_head{};
    std::uint64_t previous_sample_milliseconds{};
    std::uint64_t last_action_sequence{};
    std::uint64_t cooldown_until_milliseconds{};
};

// A normal thumbstick click is much shorter than a comfortable sustained
// press in VR. Latch sprint on a fresh L3 edge and keep submitting the native
// sprint bit until the locomotion stick returns to neutral. Ownership loss,
// stale/inactive actions, and UI entry clear the latch immediately.
[[nodiscard]] bool update_sprint_latch(
    bool input_owned,
    std::uint64_t action_sequence,
    const wawvr::xr::Vec2ActionState& movement_stick,
    const wawvr::xr::BoolActionState& stick_click,
    SprintLatchState* state) noexcept;

void reset_sprint_latch(SprintLatchState* state) noexcept;

// Detects one deliberate fast right-hand swing relative to the HMD. Sampling
// is advanced only once per OpenXR action sequence, so the several T4
// usercmds that can consume one predicted frame cannot synthesize duplicates.
// Trigger/grip holds suppress the gesture to avoid knifing while firing or
// throwing a grenade. Right-stick click remains an independent fallback.
[[nodiscard]] bool update_physical_melee_gesture(
    bool input_owned,
    const ControllerFrameSnapshot& snapshot,
    std::uint64_t now_milliseconds,
    PhysicalMeleeState* state) noexcept;

void reset_physical_melee_gesture(PhysicalMeleeState* state) noexcept;

struct ControllerInputResult final {
    bool frame_accepted{};
    bool movement_applied{};
    bool gameplay_buttons_applied{};
    bool melee_comfort_applied{};
    bool weapon_aim_applied{};
    bool weapon_trigger_applied{};
    float weapon_pitch_degrees{};
    float weapon_yaw_degrees{};
};

[[nodiscard]] bool controller_frame_is_current(
    const ControllerFrameSnapshot& snapshot,
    std::uint64_t now_milliseconds) noexcept;

// Converts H_anchor^-1 * C_current into IW local space, composes it through the
// stock/body camera basis, then derives T4 pitch/yaw degrees. The exact stereo
// thunk restores this global refdef after its temporary eye draws.
[[nodiscard]] bool controller_aim_degrees(
    const ControllerFrameSnapshot& snapshot,
    const wawvr::xr::Basis3f& camera_axis,
    float* pitch_degrees,
    float* yaw_degrees) noexcept;

// Returns the HMD's horizontal yaw relative to the frozen tracking anchor in
// T4's positive-left convention. The input hook transfers this angle into the
// native body yaw while rebasing the anchor by the same amount, keeping the
// visible HMD/controller pose stationary as the hidden player body catches up.
[[nodiscard]] bool controller_body_yaw_delta_degrees(
    const ControllerFrameSnapshot& snapshot,
    float* yaw_degrees) noexcept;

// Horizontal-only snap latch. Positive IW yaw turns left, so
// right-stick-right produces -45 degrees. The caller owns the exact T4
// view-angle write and gameplay/UI gating.
[[nodiscard]] float consume_snap_turn_degrees(
    const wawvr::xr::Vec2ActionState& right_stick,
    SnapTurnState* state) noexcept;

void reset_snap_turn(SnapTurnState* state) noexcept;

// Both exact supported SP and MP images report connection state 10 during
// active gameplay. Catchers 0x08 and 0x10 both own horizontal input, so neither
// may leak a gameplay snap.
[[nodiscard]] bool snap_turn_gameplay_allowed(
    std::uint32_t key_catchers,
    std::int32_t connection_state,
    std::int32_t active_connection_state =
        kT4SpActiveConnectionState) noexcept;

// Usercmd controller movement, weapon input, and A/B gameplay bindings are
// owned only by exact active gameplay with no native key catcher. This prevents
// the same controller actions from leaking through UI, console, or another
// engine input owner.
[[nodiscard]] bool controller_gameplay_input_allowed(
    std::uint32_t key_catchers,
    std::int32_t connection_state,
    std::int32_t active_connection_state =
        kT4SpActiveConnectionState) noexcept;

// T4 treats a nonzero completed-command melee charge distance as permission
// to rotate/lunge toward a target. Zero is the engine's native no-charge
// sentinel, so the VR post-build hook clears these dedicated fields even if
// controller tracking is temporarily stale. The ordinary melee button stays
// untouched and still drives the knife damage/animation path.
void suppress_t4_melee_charge(wawvr::t4::UsercmdSp& command) noexcept;

// Applies one consumed snap to both T4's live float yaw and the already-built
// command (the hook runs after native serialization), and rotates the sampled
// stock camera for same-command controller gun alignment.
[[nodiscard]] bool apply_snap_turn_to_t4_command(
    wawvr::t4::UsercmdSp& command,
    float snap_degrees,
    float* client_yaw_degrees,
    wawvr::xr::Basis3f* sampled_camera_axis) noexcept;

[[nodiscard]] bool apply_snap_turn_to_t4_command(
    wawvr::t4::UsercmdMp& command,
    float snap_degrees,
    float* client_yaw_degrees,
    wawvr::xr::Basis3f* sampled_camera_axis) noexcept;

// Applies only independently verified T4 usercmd fields. Native keyboard and
// mouse values are preserved: buttons are ORed and movement is saturating-add.
[[nodiscard]] ControllerInputResult apply_controller_input(
    wawvr::t4::UsercmdSp& command,
    const ControllerFrameSnapshot& snapshot,
    const wawvr::xr::Basis3f& camera_axis,
    std::uint64_t now_milliseconds) noexcept;

[[nodiscard]] ControllerInputResult apply_controller_input(
    wawvr::t4::UsercmdMp& command,
    const ControllerFrameSnapshot& snapshot,
    const wawvr::xr::Basis3f& camera_axis,
    std::uint64_t now_milliseconds) noexcept;

}  // namespace wawvr::mod
