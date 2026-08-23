// SPDX-License-Identifier: GPL-3.0-only
// WorldAtWarVR controller implementation. Comparative research history is
// implemented against the validated T4 input contract.
#include "input_mapping.hpp"

#include "camera_comfort_logic.hpp"

#include "xr_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace wawvr::mod {
namespace {

constexpr float kRadiansToDegrees =
    180.0F / 3.14159265358979323846F;

[[nodiscard]] bool finite(const float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] float dot(
    const wawvr::xr::Vec3f& left,
    const wawvr::xr::Vec3f& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] bool finite_vector(const wawvr::xr::Vec3f& value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool valid_orientation(
    const wawvr::xr::Quaternionf& value) noexcept {
    if (!finite(value.x) || !finite(value.y) || !finite(value.z) ||
        !finite(value.w)) {
        return false;
    }
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    return length_squared >= 0.90F && length_squared <= 1.10F;
}

[[nodiscard]] bool valid_basis(const wawvr::xr::Basis3f& axis) noexcept {
    if (!finite_vector(axis.forward) || !finite_vector(axis.left) ||
        !finite_vector(axis.up)) {
        return false;
    }

    const float forward_length = dot(axis.forward, axis.forward);
    const float left_length = dot(axis.left, axis.left);
    const float up_length = dot(axis.up, axis.up);
    constexpr float kMinimumLengthSquared = 0.80F;
    constexpr float kMaximumLengthSquared = 1.20F;
    constexpr float kMaximumAxisDot = 0.20F;
    return forward_length >= kMinimumLengthSquared &&
           forward_length <= kMaximumLengthSquared &&
           left_length >= kMinimumLengthSquared &&
           left_length <= kMaximumLengthSquared &&
           up_length >= kMinimumLengthSquared &&
           up_length <= kMaximumLengthSquared &&
           std::abs(dot(axis.forward, axis.left)) <= kMaximumAxisDot &&
           std::abs(dot(axis.forward, axis.up)) <= kMaximumAxisDot &&
           std::abs(dot(axis.left, axis.up)) <= kMaximumAxisDot;
}

[[nodiscard]] wawvr::xr::Vec3f compose_direction(
    const wawvr::xr::Basis3f& body,
    const wawvr::xr::Vec3f& local) noexcept {
    return {
        local.x * body.forward.x + local.y * body.left.x +
            local.z * body.up.x,
        local.x * body.forward.y + local.y * body.left.y +
            local.z * body.up.y,
        local.x * body.forward.z + local.y * body.left.z +
            local.z * body.up.z,
    };
}

[[nodiscard]] bool bool_held(
    const wawvr::xr::BoolActionState& action) noexcept {
    return action.active && action.current;
}

[[nodiscard]] bool float_held(
    const wawvr::xr::FloatActionState& action) noexcept {
    return action.active && finite(action.current) &&
           action.current >= kControllerButtonThreshold;
}

template <typename Command>
void add_if(
    Command& command,
    const bool held,
    const wawvr::t4::UsercmdButton button) noexcept {
    if (held) {
        wawvr::t4::add_button(command, button);
    }
}

[[nodiscard]] bool remap_stick(
    const wawvr::xr::Vec2ActionState& stick,
    float* x,
    float* y) noexcept {
    if (x == nullptr || y == nullptr || !stick.active ||
        !finite(stick.current.x) || !finite(stick.current.y)) {
        return false;
    }

    const float raw_x = std::clamp(stick.current.x, -1.0F, 1.0F);
    const float raw_y = std::clamp(stick.current.y, -1.0F, 1.0F);
    const float magnitude = std::sqrt(raw_x * raw_x + raw_y * raw_y);
    if (!finite(magnitude) || magnitude <= kControllerStickDeadzone) {
        *x = 0.0F;
        *y = 0.0F;
        return false;
    }

    const float clamped_magnitude = std::min(magnitude, 1.0F);
    const float remapped_magnitude =
        (clamped_magnitude - kControllerStickDeadzone) /
        (1.0F - kControllerStickDeadzone);
    const float scale = remapped_magnitude / magnitude;
    *x = raw_x * scale;
    *y = raw_y * scale;
    return true;
}

[[nodiscard]] std::int8_t saturating_movement_add(
    const std::int8_t native_value,
    const float vr_value) noexcept {
    if (!finite(vr_value)) {
        return native_value;
    }
    const int delta = static_cast<int>(std::lround(
        std::clamp(vr_value, -1.0F, 1.0F) * 127.0F));
    const int combined = std::clamp(
        static_cast<int>(native_value) + delta, -127, 127);
    return static_cast<std::int8_t>(combined);
}

}  // namespace

bool controller_frame_is_current(
    const ControllerFrameSnapshot& snapshot,
    const std::uint64_t now_milliseconds) noexcept {
    if (snapshot.frame.frame_id == 0 || snapshot.frame.actions.sequence == 0 ||
        !snapshot.frame.actions.focused ||
        now_milliseconds < snapshot.publication_milliseconds) {
        return false;
    }
    return now_milliseconds - snapshot.publication_milliseconds <=
           kMaximumControllerFrameAgeMilliseconds;
}

bool controller_aim_degrees(
    const ControllerFrameSnapshot& snapshot,
    const wawvr::xr::Basis3f& camera_axis,
    float* const pitch_degrees,
    float* const yaw_degrees) noexcept {
    if (pitch_degrees == nullptr || yaw_degrees == nullptr ||
        !valid_basis(camera_axis) ||
        !valid_orientation(snapshot.tracking_anchor.orientation)) {
        return false;
    }

    const auto& aim = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Right)].aim;
    if (!aim.active || !aim.orientation_valid ||
        !valid_orientation(aim.pose.orientation)) {
        return false;
    }

    // Position is intentionally zeroed: the first MVP uses T4's native muzzle
    // origin and needs only the controller aim orientation.
    wawvr::xr::Posef controller_orientation{};
    controller_orientation.orientation = aim.pose.orientation;
    wawvr::xr::Posef anchor_orientation{};
    anchor_orientation.orientation = snapshot.tracking_anchor.orientation;
    // The exact T4 global refdef is stock here: the stereo scene thunk applies
    // HMD orientation only to temporary eye views, then restores this global.
    // The controller must therefore be relative to the same frozen anchor as
    // those eyes. Current-head-relative math would incorrectly cancel HMD yaw.
    const wawvr::xr::EnginePose relative =
        wawvr::xr::OpenXrPoseToIwRelative(
            controller_orientation, anchor_orientation, 1.0F);
    if (!valid_basis(relative.axis)) {
        return false;
    }

    wawvr::xr::Basis3f body_axis{};
    if (!gravity_level_t4_camera_axis(camera_axis, &body_axis)) {
        return false;
    }
    wawvr::xr::Vec3f world =
        compose_direction(body_axis, relative.axis.forward);
    const float length_squared = dot(world, world);
    if (!finite_vector(world) || !finite(length_squared) ||
        length_squared <= 1.0e-8F) {
        return false;
    }
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    world.x *= inverse_length;
    world.y *= inverse_length;
    world.z *= inverse_length;

    const float horizontal = std::sqrt(
        world.x * world.x + world.y * world.y);
    const float pitch = -std::atan2(world.z, horizontal) * kRadiansToDegrees;
    const float yaw = std::atan2(world.y, world.x) * kRadiansToDegrees;
    if (!finite(pitch) || !finite(yaw)) {
        return false;
    }

    *pitch_degrees = pitch;
    *yaw_degrees = yaw;
    return true;
}

bool controller_body_yaw_delta_degrees(
    const ControllerFrameSnapshot& snapshot,
    float* const yaw_degrees) noexcept {
    if (yaw_degrees == nullptr || !snapshot.frame.views_valid ||
        !valid_orientation(snapshot.frame.head_center.orientation) ||
        !valid_orientation(snapshot.tracking_anchor.orientation)) {
        return false;
    }

    wawvr::xr::Posef head_orientation{};
    head_orientation.orientation = snapshot.frame.head_center.orientation;
    wawvr::xr::Posef anchor_orientation{};
    anchor_orientation.orientation = snapshot.tracking_anchor.orientation;
    const wawvr::xr::EnginePose relative =
        wawvr::xr::OpenXrPoseToIwRelative(
            head_orientation, anchor_orientation, 1.0F);
    if (!valid_basis(relative.axis)) {
        return false;
    }

    const float horizontal_squared =
        relative.axis.forward.x * relative.axis.forward.x +
        relative.axis.forward.y * relative.axis.forward.y;
    if (!finite(horizontal_squared) || horizontal_squared <= 0.01F) {
        return false;
    }
    const float yaw = std::atan2(
        relative.axis.forward.y, relative.axis.forward.x) *
        kRadiansToDegrees;
    if (!finite(yaw)) {
        return false;
    }
    *yaw_degrees = yaw;
    return true;
}

float consume_snap_turn_degrees(
    const wawvr::xr::Vec2ActionState& right_stick,
    SnapTurnState* const state) noexcept {
    if (state == nullptr) {
        return 0.0F;
    }
    if (!right_stick.active || !finite(right_stick.current.x) ||
        !finite(right_stick.current.y)) {
        state->armed = true;
        return 0.0F;
    }

    const float x = std::clamp(right_stick.current.x, -1.0F, 1.0F);
    const float y = std::clamp(right_stick.current.y, -1.0F, 1.0F);
    const float absolute_x = std::abs(x);
    if (absolute_x < kSnapTurnReleaseThreshold) {
        state->armed = true;
        return 0.0F;
    }
    if (!state->armed ||
        absolute_x < std::abs(y) + kSnapTurnVerticalDominanceMargin) {
        return 0.0F;
    }
    if (x >= kSnapTurnEngageThreshold) {
        state->armed = false;
        return -kSnapTurnDegrees;
    }
    if (x <= -kSnapTurnEngageThreshold) {
        state->armed = false;
        return kSnapTurnDegrees;
    }
    return 0.0F;
}

void reset_snap_turn(SnapTurnState* const state) noexcept {
    if (state != nullptr) {
        *state = {};
    }
}

bool update_sprint_latch(
    const bool input_owned,
    const std::uint64_t action_sequence,
    const wawvr::xr::Vec2ActionState& movement_stick,
    const wawvr::xr::BoolActionState& stick_click,
    SprintLatchState* const state) noexcept {
    if (state == nullptr) {
        return false;
    }

    const bool click_held = stick_click.active && stick_click.current;
    const bool actions_valid =
        action_sequence != 0 && movement_stick.active &&
        stick_click.active && finite(movement_stick.current.x) &&
        finite(movement_stick.current.y);
    if (!input_owned || !actions_valid) {
        state->input_was_owned = false;
        state->click_was_held = click_held;
        state->latched = false;
        state->last_action_sequence = action_sequence;
        return false;
    }

    const float movement_magnitude_squared =
        movement_stick.current.x * movement_stick.current.x +
        movement_stick.current.y * movement_stick.current.y;
    const bool locomotion_held =
        movement_magnitude_squared >
        kControllerStickDeadzone * kControllerStickDeadzone;

    // On the first command after gameplay regains ownership, baseline the
    // physical click. This prevents an L3 held through a menu transition from
    // creating a synthetic sprint edge.
    if (!state->input_was_owned) {
        state->input_was_owned = true;
        state->click_was_held = click_held;
        state->latched = false;
        state->last_action_sequence = action_sequence;
        return false;
    }

    // Several native usercmds may consume one published OpenXR action frame.
    // Update the physical edge only once for each action sequence.
    if (action_sequence != state->last_action_sequence) {
        if (click_held && !state->click_was_held && locomotion_held) {
            state->latched = true;
        }
        state->click_was_held = click_held;
        state->last_action_sequence = action_sequence;
    }
    if (!locomotion_held) {
        state->latched = false;
    }
    return state->latched;
}

void reset_sprint_latch(SprintLatchState* const state) noexcept {
    if (state != nullptr) {
        *state = {};
    }
}

bool update_physical_melee_gesture(
    const bool input_owned,
    const ControllerFrameSnapshot& snapshot,
    const std::uint64_t now_milliseconds,
    PhysicalMeleeState* const state) noexcept {
    if (state == nullptr) {
        return false;
    }

    const auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Right)];
    const bool pose_valid = input_owned && snapshot.frame.views_valid &&
        snapshot.frame.actions.focused &&
        snapshot.frame.actions.sequence != 0 && right.grip.active &&
        right.grip.position_valid &&
        finite_vector(right.grip.pose.position) &&
        finite_vector(snapshot.frame.head_center.position);
    if (!pose_valid ||
        now_milliseconds < snapshot.publication_milliseconds) {
        state->input_was_owned = false;
        state->pose_was_valid = false;
        state->last_action_sequence = snapshot.frame.actions.sequence;
        return false;
    }

    if (snapshot.frame.actions.sequence == state->last_action_sequence) {
        return false;
    }
    state->last_action_sequence = snapshot.frame.actions.sequence;

    const wawvr::xr::Vec3f relative{
        right.grip.pose.position.x - snapshot.frame.head_center.position.x,
        right.grip.pose.position.y - snapshot.frame.head_center.position.y,
        right.grip.pose.position.z - snapshot.frame.head_center.position.z,
    };
    const std::uint64_t sample_milliseconds =
        snapshot.publication_milliseconds;
    if (!state->input_was_owned || !state->pose_was_valid ||
        sample_milliseconds <= state->previous_sample_milliseconds) {
        state->input_was_owned = true;
        state->pose_was_valid = true;
        state->previous_hand_relative_to_head = relative;
        state->previous_sample_milliseconds = sample_milliseconds;
        return false;
    }

    const std::uint64_t elapsed_milliseconds =
        sample_milliseconds - state->previous_sample_milliseconds;
    const float previous_radius_squared = dot(
        state->previous_hand_relative_to_head,
        state->previous_hand_relative_to_head);
    const wawvr::xr::Vec3f travel{
        relative.x - state->previous_hand_relative_to_head.x,
        relative.y - state->previous_hand_relative_to_head.y,
        relative.z - state->previous_hand_relative_to_head.z,
    };
    state->previous_hand_relative_to_head = relative;
    state->previous_sample_milliseconds = sample_milliseconds;

    const float travel_squared = dot(travel, travel);
    if (!finite(travel_squared) || travel_squared < 0.0F) {
        return false;
    }
    const float travel_meters = std::sqrt(travel_squared);
    const float current_radius_squared = dot(relative, relative);
    const float outward_travel =
        finite(previous_radius_squared) && previous_radius_squared >= 0.0F &&
            finite(current_radius_squared) && current_radius_squared >= 0.0F
        ? std::sqrt(current_radius_squared) -
              std::sqrt(previous_radius_squared)
        : 0.0F;
    const float elapsed_seconds =
        static_cast<float>(elapsed_milliseconds) / 1000.0F;
    const float speed = elapsed_seconds > 0.0F
        ? travel_meters / elapsed_seconds
        : 0.0F;
    const auto& left = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Left)];
    const bool weapon_action_held =
        float_held(right.trigger) || bool_held(right.trigger_click) ||
        float_held(right.squeeze) || float_held(left.squeeze);
    if (weapon_action_held ||
        elapsed_milliseconds < kPhysicalMeleeMinimumSampleMilliseconds ||
        elapsed_milliseconds > kPhysicalMeleeMaximumSampleMilliseconds ||
        sample_milliseconds < state->cooldown_until_milliseconds ||
        travel_meters < kPhysicalMeleeMinimumTravelMeters ||
        outward_travel < kPhysicalMeleeMinimumTravelMeters * 0.5F ||
        !finite(speed) ||
        speed < kPhysicalMeleeMinimumSpeedMetersPerSecond) {
        return false;
    }

    state->cooldown_until_milliseconds =
        sample_milliseconds + kPhysicalMeleeCooldownMilliseconds;
    return true;
}

void reset_physical_melee_gesture(
    PhysicalMeleeState* const state) noexcept {
    if (state != nullptr) {
        *state = {};
    }
}

bool snap_turn_gameplay_allowed(
    const std::uint32_t key_catchers,
    const std::int32_t connection_state,
    const std::int32_t active_connection_state) noexcept {
    return connection_state == active_connection_state &&
           (key_catchers & kSnapTurnBlockedKeyCatcherMask) == 0;
}

bool controller_gameplay_input_allowed(
    const std::uint32_t key_catchers,
    const std::int32_t connection_state,
    const std::int32_t active_connection_state) noexcept {
    return connection_state == active_connection_state &&
           key_catchers == 0;
}

void suppress_t4_melee_charge(wawvr::t4::UsercmdSp& command) noexcept {
    command.melee_charge_yaw = 0.0F;
    command.melee_charge_distance = 0;
}

namespace {

template <typename Command>
bool apply_snap_turn_to_t4_command_impl(
    Command& command,
    const float snap_degrees,
    float* const client_yaw_degrees,
    wawvr::xr::Basis3f* const sampled_camera_axis) noexcept {
    if (client_yaw_degrees == nullptr || sampled_camera_axis == nullptr ||
        !finite(snap_degrees) || !finite(*client_yaw_degrees) ||
        !valid_basis(*sampled_camera_axis)) {
        return false;
    }

    const float radians = snap_degrees / kRadiansToDegrees;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    if (!finite(cosine) || !finite(sine)) {
        return false;
    }
    wawvr::xr::Basis3f rotated = *sampled_camera_axis;
    const auto rotate_world_yaw = [cosine, sine](
                                      wawvr::xr::Vec3f* const value) noexcept {
        const float x = value->x;
        const float y = value->y;
        value->x = cosine * x - sine * y;
        value->y = sine * x + cosine * y;
    };
    rotate_world_yaw(&rotated.forward);
    rotate_world_yaw(&rotated.left);
    rotate_world_yaw(&rotated.up);
    if (!valid_basis(rotated)) {
        return false;
    }

    float new_yaw = std::fmod(*client_yaw_degrees + snap_degrees, 360.0F);
    if (new_yaw < 0.0F) {
        new_yaw += 360.0F;
    }
    constexpr float kShortUnitsPerDegree = 65536.0F / 360.0F;
    const auto short_delta = static_cast<std::int32_t>(
        std::lround(snap_degrees * kShortUnitsPerDegree));
    const std::uint32_t current =
        static_cast<std::uint32_t>(command.view_angles[1]);
    command.view_angles[1] = static_cast<std::int32_t>(
        (current + static_cast<std::uint32_t>(short_delta)) & 0xFFFFU);
    *client_yaw_degrees = new_yaw;
    *sampled_camera_axis = rotated;
    return true;
}

template <typename Command>
ControllerInputResult apply_controller_input_impl(
    Command& command,
    const ControllerFrameSnapshot& snapshot,
    const wawvr::xr::Basis3f& camera_axis,
    const std::uint64_t now_milliseconds) noexcept {
    ControllerInputResult result{};
    if (!controller_frame_is_current(snapshot, now_milliseconds)) {
        return result;
    }
    result.frame_accepted = true;

    const auto& actions = snapshot.frame.actions;
    const auto& left = actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Left)];
    const auto& right = actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Right)];

    float stick_x = 0.0F;
    float stick_y = 0.0F;
    if (remap_stick(left.stick, &stick_x, &stick_y) &&
        snapshot.frame.views_valid &&
        valid_orientation(snapshot.frame.head_center.orientation) &&
        valid_orientation(snapshot.tracking_anchor.orientation)) {
        wawvr::xr::Posef head_orientation{};
        head_orientation.orientation = snapshot.frame.head_center.orientation;
        wawvr::xr::Posef anchor_orientation{};
        anchor_orientation.orientation = snapshot.tracking_anchor.orientation;
        const wawvr::xr::EnginePose relative_head =
            wawvr::xr::OpenXrPoseToIwRelative(
                head_orientation, anchor_orientation, 1.0F);
        if (valid_basis(relative_head.axis)) {
            float head_forward = relative_head.axis.forward.x;
            float head_left = relative_head.axis.forward.y;
            const float horizontal_head_length = std::sqrt(
                head_forward * head_forward + head_left * head_left);
            if (finite(horizontal_head_length) &&
                horizontal_head_length > 1.0e-4F) {
                // Pitch changes only the projection length. Normalize the
                // horizontal heading so it cannot reduce movement speed.
                head_forward /= horizontal_head_length;
                head_left /= horizontal_head_length;
                const float move_forward =
                    stick_y * head_forward + stick_x * head_left;
                const float move_right =
                    stick_x * head_forward - stick_y * head_left;
                command.forward_move = saturating_movement_add(
                    command.forward_move, move_forward);
                command.right_move = saturating_movement_add(
                    command.right_move, move_right);
                result.movement_applied = true;
            }
        }
    }

    const std::uint32_t prior_buttons = command.buttons;
    add_if(command, float_held(left.trigger),
           wawvr::t4::UsercmdButton::reload);
    add_if(command, bool_held(left.primary),
           wawvr::t4::UsercmdButton::use);
    add_if(command, bool_held(left.stick_click),
           wawvr::t4::UsercmdButton::sprint);
    add_if(command, float_held(left.squeeze),
           wawvr::t4::UsercmdButton::smoke_grenade);
    add_if(command, bool_held(right.primary),
           wawvr::t4::UsercmdButton::jump);
    add_if(command, bool_held(right.secondary),
           wawvr::t4::UsercmdButton::crouch);
    const bool melee_held = bool_held(right.stick_click);
    add_if(command, melee_held, wawvr::t4::UsercmdButton::melee);
    if constexpr (std::is_same_v<Command, wawvr::t4::UsercmdSp>) {
        if (melee_held) {
            // T4's native command builder serializes target-assisted melee
            // yaw and distance after the normal view angles. Keep the knife
            // action, but clear the charge target so it cannot rotate or
            // lunge the VR camera toward a zombie.
            suppress_t4_melee_charge(command);
            result.melee_comfort_applied = true;
        }
    }
    add_if(command, float_held(right.squeeze),
           wawvr::t4::UsercmdButton::frag_grenade);
    result.gameplay_buttons_applied = command.buttons != prior_buttons;

    float pitch_degrees = 0.0F;
    float yaw_degrees = 0.0F;
    if (controller_aim_degrees(
            snapshot, camera_axis, &pitch_degrees, &yaw_degrees)) {
        const bool trigger_held = float_held(right.trigger) ||
                                  bool_held(right.trigger_click);
        result.weapon_aim_applied = wawvr::t4::apply_vr_weapon_aim(
            command, pitch_degrees, yaw_degrees, trigger_held);
        result.weapon_trigger_applied =
            result.weapon_aim_applied && trigger_held;
        if (result.weapon_aim_applied) {
            result.weapon_pitch_degrees = pitch_degrees;
            result.weapon_yaw_degrees = yaw_degrees;
        }
    }
    return result;
}

}  // namespace

bool apply_snap_turn_to_t4_command(
    wawvr::t4::UsercmdSp& command,
    const float snap_degrees,
    float* const client_yaw_degrees,
    wawvr::xr::Basis3f* const sampled_camera_axis) noexcept {
    return apply_snap_turn_to_t4_command_impl(
        command, snap_degrees, client_yaw_degrees, sampled_camera_axis);
}

bool apply_snap_turn_to_t4_command(
    wawvr::t4::UsercmdMp& command,
    const float snap_degrees,
    float* const client_yaw_degrees,
    wawvr::xr::Basis3f* const sampled_camera_axis) noexcept {
    return apply_snap_turn_to_t4_command_impl(
        command, snap_degrees, client_yaw_degrees, sampled_camera_axis);
}

ControllerInputResult apply_controller_input(
    wawvr::t4::UsercmdSp& command,
    const ControllerFrameSnapshot& snapshot,
    const wawvr::xr::Basis3f& camera_axis,
    const std::uint64_t now_milliseconds) noexcept {
    return apply_controller_input_impl(
        command, snapshot, camera_axis, now_milliseconds);
}

ControllerInputResult apply_controller_input(
    wawvr::t4::UsercmdMp& command,
    const ControllerFrameSnapshot& snapshot,
    const wawvr::xr::Basis3f& camera_axis,
    const std::uint64_t now_milliseconds) noexcept {
    return apply_controller_input_impl(
        command, snapshot, camera_axis, now_milliseconds);
}

}  // namespace wawvr::mod
