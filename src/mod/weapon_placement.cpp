// SPDX-License-Identifier: GPL-3.0-only
// WorldAtWarVR rigid-controller placement implementation. Comparative
// implemented against the validated T4 weapon-placement contract.
#include "weapon_placement.hpp"

#include "input_mapping.hpp"
#include "xr_math.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace wawvr::mod {
namespace {

[[nodiscard]] bool finite(const float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] float dot(
    const wawvr::xr::Vec3f& left,
    const wawvr::xr::Vec3f& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] wawvr::xr::Vec3f add(
    const wawvr::xr::Vec3f& left,
    const wawvr::xr::Vec3f& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] wawvr::xr::Vec3f subtract(
    const wawvr::xr::Vec3f& left,
    const wawvr::xr::Vec3f& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] wawvr::xr::Vec3f compose(
    const wawvr::xr::Basis3f& basis,
    const wawvr::xr::Vec3f& local) noexcept {
    return {
        local.x * basis.forward.x + local.y * basis.left.x +
            local.z * basis.up.x,
        local.x * basis.forward.y + local.y * basis.left.y +
            local.z * basis.up.y,
        local.x * basis.forward.z + local.y * basis.left.z +
            local.z * basis.up.z,
    };
}

[[nodiscard]] wawvr::xr::Vec3f project_rows(
    const wawvr::xr::Basis3f& basis,
    const wawvr::xr::Vec3f& world) noexcept {
    return {
        dot(world, basis.forward),
        dot(world, basis.left),
        dot(world, basis.up),
    };
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
    constexpr float kMinimumLengthSquared = 0.80F;
    constexpr float kMaximumLengthSquared = 1.20F;
    constexpr float kMaximumAxisDot = 0.20F;
    const float forward_length = dot(axis.forward, axis.forward);
    const float left_length = dot(axis.left, axis.left);
    const float up_length = dot(axis.up, axis.up);
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

[[nodiscard]] wawvr::xr::Vec3f row(
    const wawvr::xr::Basis3f& axis,
    const std::size_t index) noexcept {
    switch (index) {
    case 0: return axis.forward;
    case 1: return axis.left;
    default: return axis.up;
    }
}

void set_row(
    wawvr::xr::Basis3f* const axis,
    const std::size_t index,
    const wawvr::xr::Vec3f& value) noexcept {
    if (index == 0) {
        axis->forward = value;
    } else if (index == 1) {
        axis->left = value;
    } else {
        axis->up = value;
    }
}

[[nodiscard]] float component(
    const wawvr::xr::Vec3f& value,
    const std::size_t index) noexcept {
    switch (index) {
    case 0: return value.x;
    case 1: return value.y;
    default: return value.z;
    }
}

}  // namespace

bool right_controller_weapon_pose(
    const ControllerFrameSnapshot& snapshot,
    const std::uint64_t now_milliseconds,
    RightControllerWeaponPose* const pose) noexcept {
    if (pose == nullptr || snapshot.frame.frame_id == 0 ||
        snapshot.frame.actions.sequence == 0 ||
        !snapshot.frame.actions.focused || !snapshot.frame.views_valid ||
        now_milliseconds < snapshot.publication_milliseconds ||
        now_milliseconds - snapshot.publication_milliseconds >
            kMaximumControllerFrameAgeMilliseconds ||
        !valid_orientation(snapshot.tracking_anchor.orientation)) {
        return false;
    }

    const auto& right = snapshot.frame.actions.hands[
        static_cast<std::uint32_t>(wawvr::xr::Hand::Right)];
    if (!right.grip.active || !right.grip.position_valid ||
        !finite_vector(right.grip.pose.position) || !right.aim.active ||
        !right.aim.orientation_valid ||
        !valid_orientation(right.aim.pose.orientation)) {
        return false;
    }

    const wawvr::xr::EnginePose grip_relative =
        wawvr::xr::OpenXrPoseToIwRelative(
            right.grip.pose, snapshot.tracking_anchor,
            wawvr::xr::kIwUnitsPerMeter);
    const wawvr::xr::EnginePose aim_relative =
        wawvr::xr::OpenXrPoseToIwRelative(
            right.aim.pose, snapshot.tracking_anchor, 1.0F);
    if (!finite_vector(grip_relative.position) ||
        !valid_basis(aim_relative.axis)) {
        return false;
    }

    pose->grip_position = grip_relative.position;
    pose->aim_axis = aim_relative.axis;
    return true;
}

bool apply_right_controller_weapon_placement(
    const wawvr::xr::Vec3f& camera_origin,
    const wawvr::xr::Basis3f& camera_axis,
    const RightControllerWeaponPose& controller,
    WeaponAttachmentState* const attachment,
    wawvr::xr::Vec3f* const weapon_origin,
    wawvr::xr::Basis3f* const weapon_axis) noexcept {
    if (attachment == nullptr || weapon_origin == nullptr ||
        weapon_axis == nullptr || !finite_vector(camera_origin) ||
        !valid_basis(camera_axis) ||
        !finite_vector(controller.grip_position) ||
        !valid_basis(controller.aim_axis) ||
        !finite_vector(*weapon_origin) || !valid_basis(*weapon_axis)) {
        return false;
    }

    if (!attachment->valid) {
        const wawvr::xr::Vec3f origin_camera_local = project_rows(
            camera_axis, subtract(*weapon_origin, camera_origin));
        attachment->position = subtract(
            origin_camera_local, controller.grip_position);

        for (std::size_t weapon_row = 0; weapon_row < 3; ++weapon_row) {
            set_row(
                &attachment->axis, weapon_row,
                project_rows(camera_axis, row(*weapon_axis, weapon_row)));
        }
        attachment->valid = true;
    }

    if (!finite_vector(attachment->position) ||
        !valid_basis(attachment->axis)) {
        return false;
    }

    const wawvr::xr::Vec3f origin_camera_local = add(
        controller.grip_position,
        compose(controller.aim_axis, attachment->position));
    *weapon_origin = add(
        camera_origin, compose(camera_axis, origin_camera_local));

    wawvr::xr::Basis3f final_axis{};
    for (std::size_t weapon_row = 0; weapon_row < 3; ++weapon_row) {
        const wawvr::xr::Vec3f attachment_row =
            row(attachment->axis, weapon_row);
        const wawvr::xr::Vec3f weapon_row_camera_local =
            compose(controller.aim_axis, attachment_row);
        set_row(
            &final_axis, weapon_row,
            compose(camera_axis, weapon_row_camera_local));
    }

    if (!finite_vector(*weapon_origin) || !valid_basis(final_axis)) {
        return false;
    }
    *weapon_axis = final_axis;
    return true;
}

bool right_controller_grip_world(
    const wawvr::xr::Vec3f& camera_origin,
    const wawvr::xr::Basis3f& camera_axis,
    const RightControllerWeaponPose& controller,
    wawvr::xr::Vec3f* const grip_world) noexcept {
    if (grip_world == nullptr || !finite_vector(camera_origin) ||
        !valid_basis(camera_axis) ||
        !finite_vector(controller.grip_position)) {
        return false;
    }
    const wawvr::xr::Vec3f result = add(
        camera_origin, compose(camera_axis, controller.grip_position));
    if (!finite_vector(result)) {
        return false;
    }
    *grip_world = result;
    return true;
}

bool align_viewmodel_origin_to_grip(
    const wawvr::xr::Vec3f& tracked_grip_world,
    const wawvr::xr::Vec3f& viewmodel_grip_tag_world,
    wawvr::xr::Vec3f* const viewmodel_origin) noexcept {
    if (viewmodel_origin == nullptr ||
        !finite_vector(tracked_grip_world) ||
        !finite_vector(viewmodel_grip_tag_world) ||
        !finite_vector(*viewmodel_origin)) {
        return false;
    }

    const wawvr::xr::Vec3f correction = subtract(
        tracked_grip_world, viewmodel_grip_tag_world);
    constexpr float kMaximumCorrectionIwUnits = 512.0F;
    const float length_squared = dot(correction, correction);
    if (!finite(length_squared) ||
        length_squared >
            kMaximumCorrectionIwUnits * kMaximumCorrectionIwUnits) {
        return false;
    }

    const wawvr::xr::Vec3f corrected = add(*viewmodel_origin, correction);
    if (!finite_vector(corrected)) {
        return false;
    }
    *viewmodel_origin = corrected;
    return true;
}

bool iw_axis_to_unit_quaternion(
    const wawvr::xr::Basis3f& axis,
    wawvr::xr::Quaternionf* const quaternion) noexcept {
    if (quaternion == nullptr || !valid_basis(axis)) {
        return false;
    }

    const std::array<std::array<float, 3>, 3> matrix{{
        {{axis.forward.x, axis.forward.y, axis.forward.z}},
        {{axis.left.x, axis.left.y, axis.left.z}},
        {{axis.up.x, axis.up.y, axis.up.z}},
    }};
    std::array<std::array<float, 4>, 4> test{};
    test[0] = {
        matrix[1][2] - matrix[2][1],
        matrix[2][0] - matrix[0][2],
        matrix[0][1] - matrix[1][0],
        matrix[0][0] + matrix[1][1] + matrix[2][2] + 1.0F,
    };
    test[1] = {
        matrix[2][0] + matrix[0][2],
        matrix[2][1] + matrix[1][2],
        matrix[2][2] - matrix[1][1] - matrix[0][0] + 1.0F,
        test[0][2],
    };
    test[2] = {
        matrix[0][0] - matrix[1][1] - matrix[2][2] + 1.0F,
        matrix[1][0] + matrix[0][1],
        test[1][0],
        test[0][0],
    };
    test[3] = {
        test[2][1],
        matrix[1][1] - matrix[0][0] - matrix[2][2] + 1.0F,
        test[1][1],
        test[0][1],
    };

    std::size_t best = 0;
    float best_length_squared = 0.0F;
    for (std::size_t index = 0; index < test.size(); ++index) {
        float length_squared = 0.0F;
        for (const float value : test[index]) {
            length_squared += value * value;
        }
        if (length_squared > best_length_squared) {
            best = index;
            best_length_squared = length_squared;
        }
    }
    if (!finite(best_length_squared) || best_length_squared < 1.0F) {
        return false;
    }

    const float inverse_length = 1.0F / std::sqrt(best_length_squared);
    const auto& value = test[best];
    wawvr::xr::Quaternionf result{
        value[0] * inverse_length,
        value[1] * inverse_length,
        value[2] * inverse_length,
        value[3] * inverse_length,
    };
    if (!valid_orientation(result)) {
        return false;
    }
    *quaternion = result;
    return true;
}

}  // namespace wawvr::mod
