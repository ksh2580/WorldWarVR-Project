// SPDX-License-Identifier: GPL-3.0-only

#include "controller_state.hpp"
#include "input_mapping.hpp"

#include "t4/usercmd.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace wawvr;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

[[nodiscard]] bool near(
    const float left,
    const float right,
    const float tolerance = 0.001F) noexcept {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] xr::Basis3f identity_axis() noexcept {
    return {};
}

[[nodiscard]] mod::ControllerFrameSnapshot valid_snapshot() noexcept {
    mod::ControllerFrameSnapshot snapshot{};
    snapshot.frame.frame_id = 4;
    snapshot.frame.views_valid = true;
    snapshot.frame.head_center.orientation.w = 1.0F;
    snapshot.frame.actions.focused = true;
    snapshot.frame.actions.sequence = 8;
    snapshot.tracking_anchor.orientation.w = 1.0F;
    snapshot.publication_milliseconds = 1'000;

    auto& right_aim = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)].aim;
    right_aim.active = true;
    right_aim.orientation_valid = true;
    right_aim.pose.orientation.w = 1.0F;
    return snapshot;
}

void test_identity_aim_and_trigger() {
    auto snapshot = valid_snapshot();
    auto& trigger = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)].trigger;
    trigger.active = true;
    trigger.current = 0.8F;

    t4::UsercmdSp command{};
    t4::add_button(command, t4::UsercmdButton::reload);
    const auto result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'001);
    check(result.frame_accepted, "fresh focused frame accepted");
    check(result.weapon_aim_applied, "identity controller aim applied");
    check(result.weapon_trigger_applied, "right trigger applied");
    check(command.gun_pitch_short == 0, "identity aim has zero pitch");
    check(command.gun_yaw_short == 0, "identity aim has zero yaw");
    check(t4::has_button(command, t4::UsercmdButton::attack),
          "right trigger injects attack");
    check(t4::has_button(command, t4::UsercmdButton::reload),
          "native buttons are preserved");
}

void test_camera_axis_composition() {
    const auto snapshot = valid_snapshot();
    xr::Basis3f camera{};
    camera.forward = {0.0F, 1.0F, 0.0F};
    camera.left = {-1.0F, 0.0F, 0.0F};
    camera.up = {0.0F, 0.0F, 1.0F};
    float pitch = 99.0F;
    float yaw = 99.0F;
    check(mod::controller_aim_degrees(
              snapshot, camera, &pitch, &yaw),
          "camera-composed aim is valid");
    check(std::abs(pitch) < 0.001F, "camera-composed pitch");
    check(std::abs(yaw - 90.0F) < 0.001F, "camera-composed yaw");
}

void test_controller_aim_is_relative_to_tracking_anchor() {
    auto snapshot = valid_snapshot();
    constexpr float kHalfSqrtTwo = 0.70710678118F;
    const xr::Quaternionf head_yaw_left = {
        0.0F, kHalfSqrtTwo, 0.0F, kHalfSqrtTwo};
    snapshot.frame.head_center.orientation = head_yaw_left;
    snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)]
        .aim.pose.orientation = head_yaw_left;

    // T4's global refdef remains stock outside the temporary stereo scene
    // calls. A controller pointing ahead with a +90-degree HMD yaw must retain
    // that anchor-relative yaw when composed through this stock camera.
    xr::Basis3f camera{};
    float pitch = 99.0F;
    float yaw = 99.0F;
    check(mod::controller_aim_degrees(
              snapshot, camera, &pitch, &yaw),
          "tracking-anchor-relative controller aim is valid");
    check(std::abs(pitch) < 0.001F,
          "tracking-anchor-relative controller pitch");
    check(std::abs(yaw - 90.0F) < 0.001F,
          "stock refdef does not cancel tracked HMD/controller yaw");
}

void test_hmd_oriented_movement_and_buttons() {
    auto snapshot = valid_snapshot();
    auto& left = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Left)];
    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    left.stick.active = true;
    left.stick.current = {0.0F, 1.0F};
    left.trigger = {true, 0.8F, false};
    left.squeeze = {true, 0.8F, false};
    left.primary = {true, true, true};
    left.stick_click = {true, true, true};
    right.primary = {true, true, true};
    right.secondary = {true, true, true};
    right.stick_click = {true, true, true};
    right.squeeze = {true, 0.8F, false};

    t4::UsercmdSp command{};
    command.melee_charge_yaw = 73.0F;
    command.melee_charge_distance = 91;
    const auto result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'010);
    check(result.movement_applied && command.forward_move == 127 &&
              command.right_move == 0,
          "identity HMD maps stick-up to forward movement");
    check(t4::has_button(command, t4::UsercmdButton::reload), "reload map");
    check(t4::has_button(command, t4::UsercmdButton::use), "use map");
    check(t4::has_button(command, t4::UsercmdButton::sprint), "sprint map");
    check(!t4::has_button(command, t4::UsercmdButton::aim_down_sights),
          "VR controller no longer injects desktop ADS");
    check(t4::has_button(command, t4::UsercmdButton::jump), "jump map");
    check(t4::has_button(command, t4::UsercmdButton::crouch), "crouch map");
    check(t4::has_button(command, t4::UsercmdButton::melee), "melee map");
    check(result.melee_comfort_applied &&
              std::abs(command.melee_charge_yaw) < 0.001F &&
              command.melee_charge_distance == 0,
          "VR melee keeps the knife button but removes target yaw and lunge");
    check(t4::has_button(command, t4::UsercmdButton::frag_grenade),
          "right grip maps to the T4 frag-grenade bit for Zombies");
    check(t4::has_button(command, t4::UsercmdButton::smoke_grenade),
          "left grip maps to the tactical/smoke offhand slot");

    // A +90 degree OpenXR yaw points HMD forward toward IW left. Stick-up
    // therefore becomes negative right-move in the unchanged body basis.
    constexpr float kHalfSqrtTwo = 0.70710678118F;
    snapshot.frame.head_center.orientation =
        {0.0F, kHalfSqrtTwo, 0.0F, kHalfSqrtTwo};
    command = {};
    const auto rotated = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'011);
    check(rotated.movement_applied, "rotated HMD movement applied");
    check(std::abs(static_cast<int>(command.forward_move)) <= 1 &&
              command.right_move == -127,
          "left-facing HMD rotates stick-up into body-left movement");

    // Looking 60 degrees upward leaves only half of the raw head-forward
    // vector in the horizontal plane. Normalizing that projection keeps full
    // stick travel at full movement speed.
    constexpr float kSinThirty = 0.5F;
    constexpr float kCosThirty = 0.86602540378F;
    snapshot.frame.head_center.orientation = {
        kSinThirty, 0.0F, 0.0F, kCosThirty};
    command = {};
    const auto pitched = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'012);
    check(pitched.movement_applied && command.forward_move == 127 &&
              command.right_move == 0,
          "HMD pitch does not reduce horizontal movement speed");
}

void test_campaign_controls_never_suspend_locomotion() {
    auto snapshot = valid_snapshot();
    auto& left = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Left)];
    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    left.stick = {true, {0.0F, 1.0F}, true};
    right.thumbrest = {true, true, true};

    t4::UsercmdSp sp_command{};
    const auto sp_result = mod::apply_controller_input(
        sp_command, snapshot, identity_axis(), 1'001);
    check(sp_result.movement_applied && sp_command.forward_move == 127 &&
              sp_command.right_move == 0,
          "passive Touch thumbrest contact cannot stop SP/Zombies locomotion");

    t4::UsercmdMp mp_command{};
    const auto mp_result = mod::apply_controller_input(
        mp_command, snapshot, identity_axis(), 1'001);
    check(mp_result.movement_applied && mp_command.forward_move == 127 &&
              mp_command.right_move == 0,
          "passive Touch thumbrest contact does not alter MP locomotion");

    right.thumbrest = {};
    left.secondary = {true, true, true};
    sp_command = {};
    const auto secondary_sp_result = mod::apply_controller_input(
        sp_command, snapshot, identity_axis(), 1'002);
    check(secondary_sp_result.movement_applied &&
              sp_command.forward_move == 127 && sp_command.right_move == 0,
          "deliberate campaign chord cannot stop SP/Zombies locomotion");

    mp_command = {};
    const auto secondary_mp_result = mod::apply_controller_input(
        mp_command, snapshot, identity_axis(), 1'002);
    check(secondary_mp_result.movement_applied &&
              mp_command.forward_move == 127 && mp_command.right_move == 0,
          "left-secondary mission modifier does not alter MP locomotion");
}

void test_tactical_grip_is_additive_and_does_not_erase_native_ads() {
    auto snapshot = valid_snapshot();
    auto& left = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Left)];
    left.squeeze = {true, 0.8F, true};

    t4::UsercmdSp command{};
    t4::add_button(command, t4::UsercmdButton::aim_down_sights);
    const auto held_result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'001);
    check(held_result.gameplay_buttons_applied &&
              t4::has_button(command, t4::UsercmdButton::aim_down_sights) &&
              t4::has_button(command, t4::UsercmdButton::smoke_grenade),
          "left grip adds tactical grenade without erasing native ADS input");

    left.squeeze.current = 0.0F;
    command = {};
    const auto released_result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'002);
    check(!released_result.gameplay_buttons_applied &&
              !t4::has_button(command, t4::UsercmdButton::smoke_grenade) &&
              !t4::has_button(command, t4::UsercmdButton::aim_down_sights),
          "released left grip injects neither tactical grenade nor ADS");
}

void test_melee_charge_is_unconditionally_suppressed() {
    t4::UsercmdSp command{};
    command.melee_charge_yaw = -132.5F;
    command.melee_charge_distance = 255;
    t4::add_button(command, t4::UsercmdButton::melee);

    mod::suppress_t4_melee_charge(command);

    check(t4::has_button(command, t4::UsercmdButton::melee),
          "melee charge suppression preserves the ordinary knife button");
    check(std::abs(command.melee_charge_yaw) < 0.001F &&
              command.melee_charge_distance == 0,
          "melee charge suppression uses T4's native no-charge sentinel");
}

void test_controller_aligned_frag_hold_and_release() {
    auto snapshot = valid_snapshot();
    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    constexpr float kHalfSqrtTwo = 0.70710678118F;
    right.aim.pose.orientation =
        {0.0F, kHalfSqrtTwo, 0.0F, kHalfSqrtTwo};

    t4::UsercmdSp command{};
    command.view_angles = {111, 222, 333};
    right.squeeze = {true, mod::kControllerButtonThreshold - 0.01F, false};
    auto result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'001);
    check(result.weapon_aim_applied &&
              !t4::has_button(command, t4::UsercmdButton::frag_grenade),
          "sub-threshold grip keeps controller aim but does not prime a frag");
    check(command.view_angles == std::array<std::int32_t, 3>{111, 222, 333},
          "controller frag aiming never turns the body or HMD camera");

    command = {};
    command.view_angles = {111, 222, 333};
    right.squeeze.current = mod::kControllerButtonThreshold;
    result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'002);
    check(t4::has_button(command, t4::UsercmdButton::frag_grenade) &&
              result.weapon_aim_applied,
          "held grip primes the native frag with controller-authored gun angles");
    check(command.gun_yaw_short == static_cast<std::int16_t>(16384),
          "held frag carries the right-controller yaw into native offhand aim");
    check(command.view_angles == std::array<std::int32_t, 3>{111, 222, 333},
          "held frag leaves native view angles unchanged");

    command = {};
    right.squeeze = {true, 0.0F, true};
    result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'003);
    check(!t4::has_button(command, t4::UsercmdButton::frag_grenade),
          "releasing grip stops the VR frag hold so native code throws");

    command = {};
    t4::add_button(command, t4::UsercmdButton::frag_grenade);
    result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'004);
    check(t4::has_button(command, t4::UsercmdButton::frag_grenade),
          "VR release never erases a native keyboard/mouse frag bit");
}

void test_focus_staleness_and_invalid_pose_fail_closed() {
    const t4::UsercmdSp original = [] {
        t4::UsercmdSp command{};
        command.buttons = 0x12340000U;
        command.forward_move = 11;
        command.gun_pitch_short = 321;
        return command;
    }();

    auto snapshot = valid_snapshot();
    t4::UsercmdSp command = original;
    auto result = mod::apply_controller_input(
        command, snapshot, identity_axis(),
        snapshot.publication_milliseconds +
            mod::kMaximumControllerFrameAgeMilliseconds + 1);
    check(!result.frame_accepted &&
              std::memcmp(&command, &original, sizeof(command)) == 0,
          "stale controller frame cannot mutate command");

    snapshot.frame.actions.focused = false;
    command = original;
    result = mod::apply_controller_input(command, snapshot, identity_axis(), 1'001);
    check(!result.frame_accepted &&
              std::memcmp(&command, &original, sizeof(command)) == 0,
          "unfocused session cannot mutate command");

    snapshot = valid_snapshot();
    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    right.trigger = {true, 1.0F, false};
    right.aim.pose.orientation.x =
        std::numeric_limits<float>::quiet_NaN();
    command = original;
    result = mod::apply_controller_input(command, snapshot, identity_axis(), 1'001);
    check(result.frame_accepted && !result.weapon_aim_applied &&
              !t4::has_button(command, t4::UsercmdButton::attack),
          "non-finite aim suppresses VR aim and trigger atomically");
    check(command.gun_pitch_short == original.gun_pitch_short,
          "invalid aim preserves native gun angle");
}

void test_high_level_actions_do_not_guess_usercmd_fields() {
    auto snapshot = valid_snapshot();
    snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)].aim.active = false;
    auto& left = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Left)];
    left.secondary = {true, true, true};
    snapshot.frame.actions.menu = {true, true, true};

    t4::UsercmdSp command{};
    command.weapon = 7;
    static_cast<void>(mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'001));
    check(command.weapon == 7 && command.buttons == 0,
          "weapon-next/menu actions never guess at raw usercmd fields");
}

void test_multiplayer_command_prefix_and_active_state() {
    auto snapshot = valid_snapshot();
    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    right.trigger = {true, 0.9F, false};
    right.stick_click = {true, true, true};

    t4::UsercmdMp command{};
    command.opaque_after_angles.fill(std::byte{0xA5});
    const auto result = mod::apply_controller_input(
        command, snapshot, identity_axis(), 1'001);
    check(result.weapon_aim_applied && result.weapon_trigger_applied,
          "MP command accepts the shared controller aim/button prefix");
    check(t4::has_button(command, t4::UsercmdButton::attack) &&
              t4::has_button(command, t4::UsercmdButton::melee),
          "MP command receives attack and ordinary melee buttons");
    for (const auto value : command.opaque_after_angles) {
        check(value == std::byte{0xA5},
              "MP opaque tail is never treated as SP melee-charge state");
    }
    check(!result.melee_comfort_applied,
          "MP does not claim an unverified melee-charge write");
    check(mod::controller_gameplay_input_allowed(
              0, 10, mod::kT4MpActiveConnectionState) &&
              !mod::controller_gameplay_input_allowed(
                  0, 9, mod::kT4MpActiveConnectionState),
          "MP controller ownership uses live-verified active connection state 10");
    check(mod::snap_turn_gameplay_allowed(
              0, 10, mod::kT4MpActiveConnectionState),
          "MP snap turn uses live-verified active connection state 10");
}

void test_horizontal_snap_turn_hysteresis() {
    mod::SnapTurnState state{};
    xr::Vec2ActionState stick{true, {0.80F, 0.0F}, true};
    check(near(mod::consume_snap_turn_degrees(stick, &state), -45.0F),
          "right stick engages one rightward snap");
    check(near(mod::consume_snap_turn_degrees(stick, &state), 0.0F),
          "held stick cannot repeat snap");

    stick.current.x = 0.40F;
    check(near(mod::consume_snap_turn_degrees(stick, &state), 0.0F),
          "stick above release threshold stays latched");
    stick.current.x = 0.20F;
    check(near(mod::consume_snap_turn_degrees(stick, &state), 0.0F) &&
              state.armed,
          "centered stick rearms snap");
    stick.current.x = -0.90F;
    check(near(mod::consume_snap_turn_degrees(stick, &state), 45.0F),
          "left stick engages one leftward snap");

    mod::reset_snap_turn(&state);
    stick.current = {0.85F, 0.90F};
    check(near(mod::consume_snap_turn_degrees(stick, &state), 0.0F) &&
              state.armed,
          "vertically dominant right stick never turns");
    stick.active = false;
    state.armed = false;
    check(near(mod::consume_snap_turn_degrees(stick, &state), 0.0F) &&
              state.armed,
          "inactive stick safely rearms snap");
}

void test_click_to_sprint_latch() {
    mod::SprintLatchState state{};
    xr::Vec2ActionState movement_stick{
        true, {0.0F, 1.0F}, true};
    xr::BoolActionState stick_click{true, false, true};

    check(!mod::update_sprint_latch(
              true, 1, movement_stick, stick_click, &state),
          "first owned sprint frame baselines a released L3");
    stick_click.current = true;
    check(mod::update_sprint_latch(
              true, 2, movement_stick, stick_click, &state),
          "fresh L3 edge latches sprint while locomoting");
    check(mod::update_sprint_latch(
              true, 2, movement_stick, stick_click, &state),
          "repeated usercmd for one XR sequence preserves one latch");

    stick_click.current = false;
    check(mod::update_sprint_latch(
              true, 3, movement_stick, stick_click, &state),
          "released L3 keeps sprint latched while the stick is moving");
    movement_stick.current = {};
    check(!mod::update_sprint_latch(
              true, 4, movement_stick, stick_click, &state),
          "returning locomotion stick to neutral clears sprint");

    movement_stick.current = {0.0F, 1.0F};
    stick_click.current = true;
    check(mod::update_sprint_latch(
              true, 5, movement_stick, stick_click, &state),
          "a later L3 edge can latch sprint again");
    check(!mod::update_sprint_latch(
              false, 6, movement_stick, stick_click, &state),
          "UI, focus, connection, or stale-frame ownership loss clears sprint");
    check(!mod::update_sprint_latch(
              true, 7, movement_stick, stick_click, &state),
          "held L3 on gameplay recovery is baselined without a synthetic edge");
    stick_click.current = false;
    check(!mod::update_sprint_latch(
              true, 8, movement_stick, stick_click, &state),
          "release after recovery rearms a future sprint click");
    stick_click.current = true;
    check(mod::update_sprint_latch(
              true, 9, movement_stick, stick_click, &state),
          "fresh post-recovery click latches normally");

    mod::reset_sprint_latch(&state);
    check(!state.input_was_owned && !state.click_was_held &&
              !state.latched && state.last_action_sequence == 0,
          "explicit XR/input reset clears all sprint latch state");
    check(!mod::update_sprint_latch(
              true, 10, movement_stick, stick_click, nullptr),
          "null sprint latch state fails closed");
}

void test_snap_turn_requires_exact_gameplay_state_and_no_ui_catcher() {
    check(mod::snap_turn_gameplay_allowed(0, 10),
          "exact T4 connection state 10 permits gameplay snap turn");
    check(!mod::snap_turn_gameplay_allowed(0, 9) &&
              !mod::snap_turn_gameplay_allowed(0, 11),
          "neighboring T4 connection states cannot receive snap turn");
    check(!mod::snap_turn_gameplay_allowed(0x08U, 10),
          "T4 key catcher 0x08 owns input and blocks snap turn");
    check(!mod::snap_turn_gameplay_allowed(0x10U, 10),
          "T4 key catcher 0x10 owns input and blocks snap turn");
    check(!mod::snap_turn_gameplay_allowed(0x18U, 10),
          "combined UI catcher bits block snap turn");
    check(mod::snap_turn_gameplay_allowed(0x04U, 10),
          "unrelated catcher bits do not broaden the verified gate");
}

void test_controller_gameplay_input_is_owned_by_gameplay_not_ui() {
    check(mod::controller_gameplay_input_allowed(0, 10),
          "active T4 gameplay without UI permits controller usercmd input");
    check(!mod::controller_gameplay_input_allowed(0x10U, 10),
          "native T4 UI catcher blocks controller usercmd input");
    check(!mod::controller_gameplay_input_allowed(0, 9) &&
              !mod::controller_gameplay_input_allowed(0, 11),
          "non-gameplay connection states cannot receive controller usercmd input");
    check(!mod::controller_gameplay_input_allowed(0x08U, 10) &&
              !mod::controller_gameplay_input_allowed(0x01U, 10),
          "other native input owners also block controller usercmd input");
}

void test_snap_turn_updates_t4_yaw_and_same_command_camera() {
    t4::UsercmdSp command{};
    command.view_angles[1] = 1'000;
    float client_yaw = 10.0F;
    xr::Basis3f camera{};
    check(mod::apply_snap_turn_to_t4_command(
              command, -45.0F, &client_yaw, &camera),
          "valid rightward snap applies to exact T4 state");
    check(command.view_angles[1] == ((1'000 - 8'192) & 0xFFFF),
          "post-serialization command yaw receives -8192 units");
    check(near(client_yaw, 325.0F),
          "live float yaw wraps into 0..360 degrees");
    constexpr float kHalfSqrtTwo = 0.70710678118F;
    check(near(camera.forward.x, kHalfSqrtTwo) &&
              near(camera.forward.y, -kHalfSqrtTwo),
          "sampled stock camera rotates right for same-command gun aim");

    const t4::UsercmdSp before = command;
    const float yaw_before = client_yaw;
    camera.forward.x = std::numeric_limits<float>::quiet_NaN();
    check(!mod::apply_snap_turn_to_t4_command(
              command, 45.0F, &client_yaw, &camera) &&
              std::memcmp(&command, &before, sizeof(command)) == 0 &&
              client_yaw == yaw_before,
          "invalid camera fails closed without partial yaw mutation");
}

void test_body_yaw_delta_tracks_physical_heading() {
    auto snapshot = valid_snapshot();
    constexpr float kHalfSqrtTwo = 0.70710678118F;
    snapshot.frame.head_center.orientation =
        {0.0F, kHalfSqrtTwo, 0.0F, kHalfSqrtTwo};
    float yaw = 0.0F;
    check(mod::controller_body_yaw_delta_degrees(snapshot, &yaw) &&
              near(yaw, 90.0F),
          "physical left turn produces matching positive T4 body yaw");

    auto& right_aim = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)].aim;
    right_aim.pose.orientation = snapshot.frame.head_center.orientation;
    float pitch_before = 0.0F;
    float weapon_yaw_before = 0.0F;
    xr::Basis3f camera{};
    check(mod::controller_aim_degrees(
              snapshot, camera, &pitch_before, &weapon_yaw_before),
          "pre-rebase controller world aim is valid");

    t4::UsercmdSp command{};
    float client_yaw = 0.0F;
    check(mod::apply_snap_turn_to_t4_command(
              command, yaw, &client_yaw, &camera),
          "body catch-up applies the extracted yaw to native state");

    snapshot.tracking_anchor.orientation =
        snapshot.frame.head_center.orientation;
    check(mod::controller_body_yaw_delta_degrees(snapshot, &yaw) &&
              near(yaw, 0.0F),
          "rebasing the tracking anchor consumes physical body yaw once");
    float pitch_after = 0.0F;
    float weapon_yaw_after = 0.0F;
    check(mod::controller_aim_degrees(
              snapshot, camera, &pitch_after, &weapon_yaw_after) &&
              near(weapon_yaw_after, weapon_yaw_before),
          "equal body and anchor yaw transfers preserve visible controller aim");

    auto& left = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Left)];
    left.stick = {true, {0.0F, 1.0F}, true};
    command = {};
    const auto movement = mod::apply_controller_input(
        command, snapshot, camera, 1'001);
    check(movement.movement_applied && command.forward_move == 127 &&
              command.right_move == 0,
          "stick-up is native forward after body and HMD heading synchronize");

    command = {};
    check(mod::apply_snap_turn_to_t4_command(
              command, -45.0F, &client_yaw, &camera),
          "snap turn remains independently applicable after body catch-up");
    float combined_pitch = 0.0F;
    float combined_yaw = 0.0F;
    check(mod::controller_aim_degrees(
              snapshot, camera, &combined_pitch, &combined_yaw) &&
              near(combined_yaw, 45.0F),
          "physical +90 and virtual -45 compose once instead of double-rotating");
}

void test_physical_melee_gesture() {
    auto snapshot = valid_snapshot();
    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    right.grip.active = true;
    right.grip.position_valid = true;
    right.grip.pose.position = {0.20F, 0.0F, 0.0F};
    mod::PhysicalMeleeState state{};

    snapshot.frame.actions.sequence = 10;
    snapshot.publication_milliseconds = 1'000;
    check(!mod::update_physical_melee_gesture(
              true, snapshot, 1'001, &state),
          "first tracked hand pose only establishes the melee baseline");

    snapshot.frame.actions.sequence = 11;
    snapshot.publication_milliseconds = 1'080;
    right.grip.pose.position.x += 0.10F;
    check(mod::update_physical_melee_gesture(
              true, snapshot, 1'081, &state),
          "fast ten-centimetre right-hand swing triggers native melee once");
    check(!mod::update_physical_melee_gesture(
              true, snapshot, 1'081, &state),
          "several commands consuming one OpenXR sample cannot duplicate melee");

    snapshot.frame.actions.sequence = 12;
    snapshot.publication_milliseconds = 1'160;
    right.grip.pose.position.x += 0.10F;
    check(!mod::update_physical_melee_gesture(
              true, snapshot, 1'161, &state),
          "physical melee cooldown blocks animation-spam swings");

    snapshot.frame.actions.sequence = 13;
    snapshot.publication_milliseconds = 1'600;
    right.trigger = {true, 1.0F, true};
    right.grip.pose.position.x += 0.40F;
    check(!mod::update_physical_melee_gesture(
              true, snapshot, 1'601, &state),
          "firing blocks accidental physical melee");

    snapshot.frame.actions.sequence = 14;
    snapshot.publication_milliseconds = 2'100;
    right.trigger = {};
    snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Left)].squeeze =
            {true, 1.0F, true};
    right.grip.pose.position.x += 0.40F;
    check(!mod::update_physical_melee_gesture(
              true, snapshot, 2'101, &state),
          "holding the tactical-grenade grip blocks a throw from becoming melee");

    mod::reset_physical_melee_gesture(&state);
    check(!state.input_was_owned && !state.pose_was_valid &&
              state.last_action_sequence == 0,
          "explicit physical-melee reset clears tracking history");
}

void test_controller_frame_broker() {
    mod::clear_controller_frame();
    mod::ControllerFrameSnapshot read{};
    check(!mod::read_controller_frame(&read), "cleared broker has no frame");

    xr::FrameState frame{};
    frame.frame_id = 44;
    frame.actions.sequence = 45;
    xr::Posef anchor{};
    anchor.orientation = {0.0F, 0.0F, 0.0F, 1.0F};
    mod::publish_controller_frame(frame, anchor);
    check(mod::read_controller_frame(&read), "published broker frame readable");
    check(read.frame.frame_id == 44 && read.frame.actions.sequence == 45 &&
              read.generation != 0 && read.publication_milliseconds != 0,
          "broker returns one consistent snapshot");

    const xr::Quaternionf body_aligned{
        0.0F, 0.70710678F, 0.0F, 0.70710678F};
    check(mod::rebase_controller_frame_tracking_anchor(
              read.generation, read.frame.frame_id,
              read.tracking_anchor.orientation, body_aligned),
          "exact controller generation accepts body-yaw anchor rebase");
    check(mod::read_controller_frame(&read) &&
              near(read.tracking_anchor.orientation.y, body_aligned.y),
          "controller consumers observe body-aligned anchor immediately");
    xr::Quaternionf consumed{};
    check(mod::consume_controller_tracking_anchor_rebase(&consumed) &&
              near(consumed.y, body_aligned.y) &&
              !mod::consume_controller_tracking_anchor_rebase(&consumed),
          "Present consumes one queued long-lived anchor update");
    check(!mod::rebase_controller_frame_tracking_anchor(
              read.generation - 1, read.frame.frame_id,
              read.tracking_anchor.orientation, body_aligned),
          "stale controller generation cannot rewrite the anchor");

    const xr::Quaternionf recovered_alignment{
        0.0F, -0.38268343F, 0.0F, 0.92387953F};
    check(mod::rebase_controller_frame_tracking_anchor(
              read.generation, read.frame.frame_id,
              read.tracking_anchor.orientation, recovered_alignment),
          "exact controller generation queues a recovery-path anchor rebase");
    mod::clear_controller_frame();
    check(!mod::read_controller_frame(&read), "broker clear invalidates frame");
    check(mod::consume_controller_tracking_anchor_rebase(&consumed) &&
              near(consumed.y, recovered_alignment.y),
          "temporary frame clear preserves the queued body-yaw transfer");

    mod::publish_controller_frame(frame, anchor);
    check(mod::read_controller_frame(&read) &&
              mod::rebase_controller_frame_tracking_anchor(
                  read.generation, read.frame.frame_id,
                  read.tracking_anchor.orientation, body_aligned),
          "full-teardown fixture queues an anchor rebase");
    mod::discard_controller_tracking_anchor_rebase();
    check(!mod::consume_controller_tracking_anchor_rebase(&consumed),
          "full XR teardown discards a stale queued anchor transfer");
    mod::clear_controller_frame();
    check(!mod::read_controller_frame(nullptr), "broker rejects null reader");
}

}  // namespace

int main() {
    test_identity_aim_and_trigger();
    test_camera_axis_composition();
    test_controller_aim_is_relative_to_tracking_anchor();
    test_hmd_oriented_movement_and_buttons();
    test_campaign_controls_never_suspend_locomotion();
    test_tactical_grip_is_additive_and_does_not_erase_native_ads();
    test_melee_charge_is_unconditionally_suppressed();
    test_controller_aligned_frag_hold_and_release();
    test_focus_staleness_and_invalid_pose_fail_closed();
    test_high_level_actions_do_not_guess_usercmd_fields();
    test_multiplayer_command_prefix_and_active_state();
    test_horizontal_snap_turn_hysteresis();
    test_click_to_sprint_latch();
    test_snap_turn_requires_exact_gameplay_state_and_no_ui_catcher();
    test_controller_gameplay_input_is_owned_by_gameplay_not_ui();
    test_snap_turn_updates_t4_yaw_and_same_command_camera();
    test_body_yaw_delta_tracks_physical_heading();
    test_physical_melee_gesture();
    test_controller_frame_broker();
    return 0;
}
