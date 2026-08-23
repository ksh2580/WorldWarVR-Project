#include "stereo_scene_math.hpp"

#include <cmath>
#include <cstdio>

namespace {

bool near(const float left, const float right, const float tolerance = 1.0e-4f) {
    return std::fabs(left - right) <= tolerance;
}

bool check(const bool condition, const char* const message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

wawvr::mod::T4SceneView stock_view() {
    wawvr::mod::T4SceneView stock{};
    stock.x = 10;
    stock.y = 20;
    stock.width = 1001;
    stock.height = 600;
    stock.tan_half_fov_x = 1.0f;
    stock.tan_half_fov_y = 0.75f;
    stock.origin = {100.0f, 200.0f, 300.0f};
    stock.axis = {};
    stock.near_clip = 4.0f;
    return stock;
}

wawvr::xr::FrameState symmetric_frame() {
    wawvr::xr::FrameState frame{};
    frame.frame_id = 42;
    frame.should_render = true;
    frame.views_valid = true;
    frame.head_center.orientation = {};
    frame.eyes[0].pose.orientation = {};
    frame.eyes[1].pose.orientation = {};
    frame.eyes[0].pose.position.x = -0.032f;
    frame.eyes[1].pose.position.x = 0.032f;
    for (auto& eye : frame.eyes) {
        eye.fov.angle_left = -std::atan(1.0f);
        eye.fov.angle_right = std::atan(1.0f);
        eye.fov.angle_down = -std::atan(0.8f);
        eye.fov.angle_up = std::atan(0.8f);
    }
    return frame;
}

} // namespace

int main() {
    using namespace wawvr;
    bool passed = true;

    const mod::T4SceneView stock = stock_view();
    const xr::FrameState frame = symmetric_frame();
    xr::Posef anchor{};
    mod::T4StereoSceneViews stereo{};
    passed &= check(
        mod::build_t4_stereo_scene_views(stock, frame, anchor, &stereo),
        "symmetric stereo view build failed");
    passed &= check(stereo.frame_id == 42, "frame id was not retained");
    passed &= check(
        stereo.eyes[0].x == 10 && stereo.eyes[0].width == 500 &&
            stereo.eyes[1].x == 510 && stereo.eyes[1].width == 501,
        "odd packed viewport was not losslessly split");
    passed &= check(
        near(stereo.eyes[0].origin.y, 200.0f + 0.032f * xr::kIwUnitsPerMeter) &&
            near(stereo.eyes[1].origin.y, 200.0f - 0.032f * xr::kIwUnitsPerMeter),
        "OpenXR right/left eye positions did not map to IW left axis");
    passed &= check(
        near(stereo.compositor_layout.destinations[0].x, 0.0f) &&
            near(stereo.compositor_layout.destinations[0].y, 0.0f) &&
            near(stereo.compositor_layout.destinations[0].width, 1.0f) &&
            near(stereo.compositor_layout.destinations[0].height, 1.0f),
        "symmetric projection should use the complete destination");

    xr::FrameState asymmetric = frame;
    asymmetric.frame_id = 43;
    asymmetric.eyes[0].fov.angle_left = -std::atan(1.2f);
    asymmetric.eyes[0].fov.angle_right = std::atan(0.8f);
    asymmetric.eyes[0].fov.angle_down = -std::atan(0.7f);
    asymmetric.eyes[0].fov.angle_up = std::atan(0.9f);
    passed &= check(
        mod::build_t4_stereo_scene_views(stock, asymmetric, anchor, &stereo),
        "asymmetric stereo view build failed");
    passed &= check(
        near(stereo.eyes[0].tan_half_fov_x, 1.2f) &&
            near(stereo.eyes[0].tan_half_fov_y, 0.9f),
        "engine did not receive centred max-tangent FOV");
    passed &= check(
        near(stereo.compositor_layout.destinations[0].x, 0.0f) &&
            near(stereo.compositor_layout.destinations[0].width, 1.2f) &&
            near(stereo.compositor_layout.destinations[0].y, 0.0f) &&
            near(stereo.compositor_layout.destinations[0].height, 1.125f),
        "asymmetric FOV destination remap was incorrect");

    // A 90-degree OpenXR yaw composed into an identity game camera should
    // rotate IW forward onto IW left.
    constexpr float half_sqrt_two = 0.7071067811865475f;
    xr::FrameState turned = frame;
    turned.frame_id = 44;
    turned.eyes[0].pose.orientation =
        {0.0f, half_sqrt_two, 0.0f, half_sqrt_two};
    turned.eyes[1].pose.orientation = turned.eyes[0].pose.orientation;
    passed &= check(
        mod::build_t4_stereo_scene_views(stock, turned, anchor, &stereo),
        "head-turned stereo view build failed");
    passed &= check(
        near(stereo.eyes[0].axis.forward.x, 0.0f) &&
            near(stereo.eyes[0].axis.forward.y, 1.0f),
        "head orientation was not composed into the stock camera");

    mod::T4SceneView animated = stock;
    animated.axis.forward = {0.8660254f, 0.0f, -0.5f};
    animated.axis.left = {0.25f, 0.8660254f, 0.4330127f};
    animated.axis.up = {0.4330127f, -0.5f, 0.75f};
    passed &= check(
        mod::build_t4_stereo_scene_views(animated, frame, anchor, &stereo),
        "animated stock camera could not build a comfort-stable view");
    passed &= check(
        near(stereo.eyes[0].axis.forward.z, 0.0f) &&
            near(stereo.eyes[0].axis.left.z, 0.0f) &&
            near(stereo.eyes[0].axis.up.z, 1.0f),
        "stock melee/recoil pitch and roll leaked into the HMD camera");

    xr::FrameState invalid = frame;
    invalid.views_valid = false;
    passed &= check(
        !mod::build_t4_stereo_scene_views(stock, invalid, anchor, &stereo),
        "invalid OpenXR views did not fail closed");
    mod::T4SceneView bad_stock = stock;
    bad_stock.width = 1;
    passed &= check(
        !mod::build_t4_stereo_scene_views(bad_stock, frame, anchor, &stereo),
        "one-pixel stock viewport did not fail closed");

    return passed ? 0 : 1;
}
