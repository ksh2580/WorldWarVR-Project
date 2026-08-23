// SPDX-License-Identifier: GPL-3.0-only

#include "weapon_placement.hpp"

#include "xr_math.h"

#include <cmath>
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

void check_vector(
    const xr::Vec3f& actual,
    const xr::Vec3f& expected,
    const std::string_view message) {
    check(
        near(actual.x, expected.x) && near(actual.y, expected.y) &&
            near(actual.z, expected.z),
        message);
}

[[nodiscard]] mod::ControllerFrameSnapshot tracked_snapshot() noexcept {
    mod::ControllerFrameSnapshot snapshot{};
    snapshot.frame.frame_id = 10;
    snapshot.frame.views_valid = true;
    snapshot.frame.head_center.orientation.w = 1.0F;
    snapshot.tracking_anchor.orientation.w = 1.0F;
    snapshot.frame.actions.focused = true;
    snapshot.frame.actions.sequence = 11;
    snapshot.publication_milliseconds = 1'000;
    snapshot.generation = 12;

    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    right.grip.active = true;
    right.grip.position_valid = true;
    right.grip.pose.orientation.w = 1.0F;
    right.grip.pose.position = {0.25F, -0.10F, -0.50F};
    right.aim.active = true;
    right.aim.orientation_valid = true;
    right.aim.pose.orientation.w = 1.0F;
    return snapshot;
}

void test_pose_uses_grip_position_and_aim_orientation() {
    const auto snapshot = tracked_snapshot();
    mod::RightControllerWeaponPose pose{};
    check(
        mod::right_controller_weapon_pose(snapshot, 1'001, &pose),
        "valid right-controller weapon pose");
    check_vector(
        pose.grip_position,
        {0.50F * xr::kIwUnitsPerMeter,
         -0.25F * xr::kIwUnitsPerMeter,
         -0.10F * xr::kIwUnitsPerMeter},
        "OpenXR grip converts to IW head-local position");
    check_vector(pose.aim_axis.forward, {1.0F, 0.0F, 0.0F},
                 "identity aim points IW forward");

    auto invalid = snapshot;
    invalid.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)]
        .grip.position_valid = false;
    check(!mod::right_controller_weapon_pose(invalid, 1'001, &pose),
          "invalid grip position fails closed");
}

void test_controller_pose_is_tracking_anchor_relative() {
    auto snapshot = tracked_snapshot();
    constexpr float kHalfSqrtTwo = 0.70710678118F;
    snapshot.frame.head_center.orientation = {
        0.0F, kHalfSqrtTwo, 0.0F, kHalfSqrtTwo};
    auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(xr::Hand::Right)];
    right.aim.pose.orientation = snapshot.frame.head_center.orientation;
    right.grip.pose.position = snapshot.tracking_anchor.position;

    mod::RightControllerWeaponPose pose{};
    check(mod::right_controller_weapon_pose(snapshot, 1'001, &pose),
          "same head/controller yaw is valid");
    check_vector(pose.aim_axis.forward, {0.0F, 1.0F, 0.0F},
                 "controller yaw is retained relative to frozen anchor");
    check_vector(pose.grip_position, {},
                 "anchor-centered grip has zero local translation");
}

void test_rigid_absolute_weapon_attachment() {
    const xr::Vec3f camera_origin{100.0F, 200.0F, 300.0F};
    const xr::Basis3f camera_axis{};
    mod::RightControllerWeaponPose controller{};
    controller.grip_position = {4.0F, 1.0F, 0.0F};
    controller.aim_axis = {};
    mod::WeaponAttachmentState attachment{};
    xr::Vec3f weapon_origin{108.0F, 202.0F, 297.0F};
    xr::Basis3f weapon_axis = camera_axis;

    check(
        mod::apply_right_controller_weapon_placement(
            camera_origin, camera_axis, controller, &attachment,
            &weapon_origin, &weapon_axis),
        "first tracked placement calibrates");
    check(attachment.valid, "attachment calibration retained");
    check_vector(attachment.position, {4.0F, 1.0F, -3.0F},
                 "stock origin captured relative to grip");
    check_vector(weapon_origin, {108.0F, 202.0F, 297.0F},
                 "identity first placement preserves stock origin");

    controller.grip_position = {6.0F, 1.0F, 0.0F};
    controller.aim_axis.forward = {0.0F, 1.0F, 0.0F};
    controller.aim_axis.left = {-1.0F, 0.0F, 0.0F};
    controller.aim_axis.up = {0.0F, 0.0F, 1.0F};
    check(
        mod::apply_right_controller_weapon_placement(
            camera_origin, camera_axis, controller, &attachment,
            &weapon_origin, &weapon_axis),
        "moved and rotated controller placement applies");
    check_vector(weapon_axis.forward, {0.0F, 1.0F, 0.0F},
                 "viewmodel forward follows controller aim");
    check_vector(weapon_origin, {105.0F, 205.0F, 297.0F},
                 "captured attachment translates and rotates rigidly");
}

void test_post_pose_grip_tag_alignment() {
    const xr::Vec3f camera_origin{100.0F, 200.0F, 300.0F};
    xr::Basis3f camera_axis{};
    camera_axis.forward = {0.0F, 1.0F, 0.0F};
    camera_axis.left = {-1.0F, 0.0F, 0.0F};
    camera_axis.up = {0.0F, 0.0F, 1.0F};
    mod::RightControllerWeaponPose controller{};
    controller.grip_position = {10.0F, 2.0F, -3.0F};

    xr::Vec3f tracked_grip_world{};
    check(
        mod::right_controller_grip_world(
            camera_origin, camera_axis, controller, &tracked_grip_world),
        "controller grip converts to stock-refdef world space");
    check_vector(
        tracked_grip_world, {98.0F, 210.0F, 297.0F},
        "camera basis composes tracked grip position");

    xr::Vec3f viewmodel_origin{90.0F, 190.0F, 290.0F};
    const xr::Vec3f current_tag_world{94.0F, 205.0F, 295.0F};
    check(
        mod::align_viewmodel_origin_to_grip(
            tracked_grip_world, current_tag_world, &viewmodel_origin),
        "finite per-model grip-tag correction applies");
    check_vector(
        viewmodel_origin, {94.0F, 195.0F, 292.0F},
        "viewmodel root moves by tracked grip minus animated grip tag");

    xr::Vec3f preserved = viewmodel_origin;
    check(
        !mod::align_viewmodel_origin_to_grip(
            {10'000.0F, 0.0F, 0.0F}, {}, &preserved),
        "implausible tag correction fails closed");
    check_vector(
        preserved, viewmodel_origin,
        "failed grip correction preserves viewmodel origin");
}

void test_axis_to_engine_quaternion() {
    xr::Basis3f yaw_left{};
    yaw_left.forward = {0.0F, 1.0F, 0.0F};
    yaw_left.left = {-1.0F, 0.0F, 0.0F};
    yaw_left.up = {0.0F, 0.0F, 1.0F};
    xr::Quaternionf quaternion{};
    check(mod::iw_axis_to_unit_quaternion(yaw_left, &quaternion),
          "valid IW axis converts to unit quaternion");

    // For the engine row convention a +90-degree yaw is +Z quaternion.
    constexpr float kHalfSqrtTwo = 0.70710678118F;
    check(near(std::abs(quaternion.z), kHalfSqrtTwo) &&
              near(std::abs(quaternion.w), kHalfSqrtTwo),
          "engine quaternion represents 90-degree yaw");
    check(near(quaternion.x, 0.0F) && near(quaternion.y, 0.0F),
          "yaw quaternion has no pitch/roll components");
}

}  // namespace

int main() {
    test_pose_uses_grip_position_and_aim_orientation();
    test_controller_pose_is_tracking_anchor_relative();
    test_rigid_absolute_weapon_attachment();
    test_post_pose_grip_tag_alignment();
    test_axis_to_engine_quaternion();
    return 0;
}
