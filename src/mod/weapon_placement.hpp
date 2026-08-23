// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "controller_state.hpp"

#include "xr_types.h"

#include <cstdint>

namespace wawvr::mod {

// Camera-local right-hand pose used by the viewmodel attachment. Position is
// taken from the OpenXR grip pose while orientation is taken from the aim pose.
struct RightControllerWeaponPose final {
    wawvr::xr::Vec3f grip_position{};
    wawvr::xr::Basis3f aim_axis{};
};

// Persistent controller-to-viewmodel offset captured from T4's normal Colt
// placement. The orientation calibration is deliberately canonical rather
// than dependent on how the controller happened to be held at startup.
struct WeaponAttachmentState final {
    bool valid{};
    wawvr::xr::Vec3f position{};
    wawvr::xr::Basis3f axis{};
};

// Builds C_current relative to H_anchor. T4's viewmodel refdef is stock here;
// the stereo scene hook applies HMD transforms later to temporary eye refdefs
// and restores the stock global.
[[nodiscard]] bool right_controller_weapon_pose(
    const ControllerFrameSnapshot& snapshot,
    std::uint64_t now_milliseconds,
    RightControllerWeaponPose* pose) noexcept;

// Rigid controller attachment in IW world-space rows (forward/left/up). On
// first use the function captures the normal viewmodel offset; subsequent
// calls move that attachment with the right controller.
[[nodiscard]] bool apply_right_controller_weapon_placement(
    const wawvr::xr::Vec3f& camera_origin,
    const wawvr::xr::Basis3f& camera_axis,
    const RightControllerWeaponPose& controller,
    WeaponAttachmentState* attachment,
    wawvr::xr::Vec3f* weapon_origin,
    wawvr::xr::Basis3f* weapon_axis) noexcept;

// Converts the tracked grip from camera-local IW coordinates into the stock
// refdef's world space. The viewmodel's grip bone is aligned to this point
// after T4 has evaluated the normal weapon/arms animation.
[[nodiscard]] bool right_controller_grip_world(
    const wawvr::xr::Vec3f& camera_origin,
    const wawvr::xr::Basis3f& camera_axis,
    const RightControllerWeaponPose& controller,
    wawvr::xr::Vec3f* grip_world) noexcept;

// Applies the second-stage grip correction to the T4 viewmodel root.
// Implausibly large or non-finite tag deltas fail closed and preserve the
// stock pose.
[[nodiscard]] bool align_viewmodel_origin_to_grip(
    const wawvr::xr::Vec3f& tracked_grip_world,
    const wawvr::xr::Vec3f& viewmodel_grip_tag_world,
    wawvr::xr::Vec3f* viewmodel_origin) noexcept;

// T4's GfxPlacement stores a unit quaternion. This conversion follows the
// engine's AxisToQuat row convention and is kept independent of proprietary
// engine calls so the hook remains testable and fail-closed.
[[nodiscard]] bool iw_axis_to_unit_quaternion(
    const wawvr::xr::Basis3f& axis,
    wawvr::xr::Quaternionf* quaternion) noexcept;

}  // namespace wawvr::mod
