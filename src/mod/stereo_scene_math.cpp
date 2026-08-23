#include "stereo_scene_math.hpp"

#include "camera_comfort_logic.hpp"

#include <algorithm>
#include <cmath>

namespace wawvr::mod {
namespace {

using wawvr::xr::Basis3f;
using wawvr::xr::EyeView;
using wawvr::xr::NormalizedViewport;
using wawvr::xr::Vec3f;

constexpr float kMaximumFovAngle = 1.55334306f; // 89 degrees
constexpr float kMinimumAxisLengthSquared = 0.64f;
constexpr float kMaximumAxisLengthSquared = 1.44f;
constexpr float kMaximumAxisDot = 0.20f;

bool finite(const float value) noexcept {
    return std::isfinite(value);
}

bool finite(const Vec3f& value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const wawvr::xr::Quaternionf& value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z) &&
           finite(value.w);
}

float dot(const Vec3f& left, const Vec3f& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

bool valid_axis(const Basis3f& axis) noexcept {
    if (!finite(axis.forward) || !finite(axis.left) || !finite(axis.up)) {
        return false;
    }
    const float forward_length = dot(axis.forward, axis.forward);
    const float left_length = dot(axis.left, axis.left);
    const float up_length = dot(axis.up, axis.up);
    return forward_length >= kMinimumAxisLengthSquared &&
           forward_length <= kMaximumAxisLengthSquared &&
           left_length >= kMinimumAxisLengthSquared &&
           left_length <= kMaximumAxisLengthSquared &&
           up_length >= kMinimumAxisLengthSquared &&
           up_length <= kMaximumAxisLengthSquared &&
           std::fabs(dot(axis.forward, axis.left)) <= kMaximumAxisDot &&
           std::fabs(dot(axis.forward, axis.up)) <= kMaximumAxisDot &&
           std::fabs(dot(axis.left, axis.up)) <= kMaximumAxisDot;
}

bool valid_pose(const wawvr::xr::Posef& pose) noexcept {
    if (!finite(pose.position) || !finite(pose.orientation)) {
        return false;
    }
    const auto& q = pose.orientation;
    const float length_squared =
        q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return length_squared > 0.25f && length_squared < 4.0f;
}

bool projection_for_eye(
    const EyeView& eye,
    float* const symmetric_x,
    float* const symmetric_y,
    NormalizedViewport* const destination) noexcept {
    if (symmetric_x == nullptr || symmetric_y == nullptr ||
        destination == nullptr) {
        return false;
    }
    const auto& fov = eye.fov;
    if (!finite(fov.angle_left) || !finite(fov.angle_right) ||
        !finite(fov.angle_up) || !finite(fov.angle_down) ||
        fov.angle_left >= 0.0f || fov.angle_right <= 0.0f ||
        fov.angle_down >= 0.0f || fov.angle_up <= 0.0f ||
        std::fabs(fov.angle_left) >= kMaximumFovAngle ||
        std::fabs(fov.angle_right) >= kMaximumFovAngle ||
        std::fabs(fov.angle_up) >= kMaximumFovAngle ||
        std::fabs(fov.angle_down) >= kMaximumFovAngle) {
        return false;
    }

    const float tangent_left = std::tan(fov.angle_left);
    const float tangent_right = std::tan(fov.angle_right);
    const float tangent_up = std::tan(fov.angle_up);
    const float tangent_down = std::tan(fov.angle_down);
    const float span_x = tangent_right - tangent_left;
    const float span_y = tangent_up - tangent_down;
    *symmetric_x = std::max(-tangent_left, tangent_right);
    *symmetric_y = std::max(-tangent_down, tangent_up);
    if (!finite(*symmetric_x) || !finite(*symmetric_y) ||
        !finite(span_x) || !finite(span_y) || *symmetric_x <= 0.0f ||
        *symmetric_y <= 0.0f || span_x <= 0.0f || span_y <= 0.0f) {
        return false;
    }

    // The engine renders a centred symmetric projection. Draw it through an
    // oversized and offset destination viewport so the runtime's asymmetric
    // projection sees the correct tangent at every output NDC coordinate.
    const float scale_x = 2.0f * *symmetric_x / span_x;
    const float scale_y = 2.0f * *symmetric_y / span_y;
    const float offset_x = -(tangent_right + tangent_left) / span_x;
    const float offset_y = -(tangent_up + tangent_down) / span_y;
    *destination = {
        (1.0f + offset_x - scale_x) * 0.5f,
        (1.0f - offset_y - scale_y) * 0.5f,
        scale_x,
        scale_y,
    };
    return finite(destination->x) && finite(destination->y) &&
           finite(destination->width) && finite(destination->height) &&
           destination->width > 0.0f && destination->width <= 8.0f &&
           destination->height > 0.0f && destination->height <= 8.0f;
}

Vec3f compose_vector(const Basis3f& base, const Vec3f& local) noexcept {
    return {
        local.x * base.forward.x + local.y * base.left.x +
            local.z * base.up.x,
        local.x * base.forward.y + local.y * base.left.y +
            local.z * base.up.y,
        local.x * base.forward.z + local.y * base.left.z +
            local.z * base.up.z,
    };
}

Basis3f compose_axis(const Basis3f& base, const Basis3f& local) noexcept {
    return {
        compose_vector(base, local.forward),
        compose_vector(base, local.left),
        compose_vector(base, local.up),
    };
}

Vec3f add(const Vec3f& left, const Vec3f& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

bool valid_stock(const T4SceneView& stock) noexcept {
    return stock.width >= 2 && stock.width <= 16384 && stock.height > 0 &&
           stock.height <= 16384 && stock.x > -16384 && stock.x < 16384 &&
           stock.y > -16384 && stock.y < 16384 &&
           finite(stock.tan_half_fov_x) && stock.tan_half_fov_x > 0.0f &&
           finite(stock.tan_half_fov_y) && stock.tan_half_fov_y > 0.0f &&
           finite(stock.origin) && valid_axis(stock.axis) &&
           finite(stock.near_clip) && stock.near_clip >= 0.0f;
}

} // namespace

bool build_t4_stereo_scene_views(
    const T4SceneView& stock,
    const wawvr::xr::FrameState& frame,
    const wawvr::xr::Posef& tracking_anchor,
    T4StereoSceneViews* const output,
    const float engine_units_per_meter) noexcept {
    if (output == nullptr) {
        return false;
    }
    *output = {};
    if (!valid_stock(stock) || !frame.should_render || !frame.views_valid ||
        frame.frame_id == 0 || !valid_pose(tracking_anchor) ||
        !finite(engine_units_per_meter) || engine_units_per_meter <= 0.0f ||
        engine_units_per_meter > 1000.0f) {
        return false;
    }

    const std::int32_t left_width = stock.width / 2;
    const std::int32_t right_width = stock.width - left_width;
    if (left_width <= 0 || right_width <= 0) {
        return false;
    }

    T4StereoSceneViews result{};
    result.frame_id = frame.frame_id;
    result.compositor_layout.eyes[0] = {0.0f, 0.0f, 0.5f, 1.0f};
    result.compositor_layout.eyes[1] = {0.5f, 0.0f, 0.5f, 1.0f};

    Basis3f body_axis{};
    if (!gravity_level_t4_camera_axis(stock.axis, &body_axis)) {
        return false;
    }

    for (std::uint32_t eye_index = 0;
         eye_index < wawvr::xr::kEyeCount; ++eye_index) {
        if (!valid_pose(frame.eyes[eye_index].pose)) {
            return false;
        }
        float symmetric_x = 0.0f;
        float symmetric_y = 0.0f;
        if (!projection_for_eye(
                frame.eyes[eye_index], &symmetric_x, &symmetric_y,
                &result.compositor_layout.destinations[eye_index])) {
            return false;
        }

        const wawvr::xr::EnginePose relative_eye =
            wawvr::xr::OpenXrPoseToIwRelative(
                frame.eyes[eye_index].pose, tracking_anchor,
                engine_units_per_meter);
        if (!finite(relative_eye.position) || !valid_axis(relative_eye.axis)) {
            return false;
        }

        T4SceneView& view = result.eyes[eye_index];
        view = stock;
        view.x = eye_index == 0 ? stock.x : stock.x + left_width;
        view.width = eye_index == 0 ? left_width : right_width;
        view.tan_half_fov_x = symmetric_x;
        view.tan_half_fov_y = symmetric_y;
        view.origin = add(stock.origin, compose_vector(body_axis,
                                                       relative_eye.position));
        view.axis = compose_axis(body_axis, relative_eye.axis);
        if (!finite(view.origin) || !valid_axis(view.axis)) {
            return false;
        }
    }

    *output = result;
    return true;
}

} // namespace wawvr::mod
