#include "menu_button_gesture.hpp"

#include <iostream>

namespace {

int failures = 0;

void expect(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_short_tap_emits_only_on_release() {
    wawvr::mod::MenuButtonGestureState state{};
    auto update = wawvr::mod::update_menu_button_gesture(
        true, true, 100, 1'000, &state);
    expect(update.hold_started && !update.short_tap_released,
           "menu press starts timing without opening the menu immediately");
    update = wawvr::mod::update_menu_button_gesture(
        false, true, 500, 1'000, &state);
    expect(update.hold_released && update.short_tap_released,
           "sub-threshold release emits one Escape tap request");
    update = wawvr::mod::update_menu_button_gesture(
        false, true, 501, 1'000, &state);
    expect(!update.short_tap_released,
           "released menu button cannot repeat Escape");
}

void test_long_hold_recenters_and_consumes_release() {
    wawvr::mod::MenuButtonGestureState state{};
    static_cast<void>(wawvr::mod::update_menu_button_gesture(
        true, true, 100, 1'000, &state));
    auto update = wawvr::mod::update_menu_button_gesture(
        true, true, 1'100, 1'000, &state);
    expect(update.long_hold_recognized &&
               update.recenter_capture_requested &&
               state.long_hold_recognized && state.recenter_captured,
           "one-second hold requests one recenter capture");
    update = wawvr::mod::update_menu_button_gesture(
        true, true, 1'200, 1'000, &state);
    expect(!update.recenter_capture_requested,
           "continued long hold cannot recenter repeatedly");
    update = wawvr::mod::update_menu_button_gesture(
        false, true, 1'300, 1'000, &state);
    expect(update.hold_released && !update.short_tap_released,
           "release after a consumed hold never emits Escape");
}

void test_invalid_pose_still_consumes_long_hold() {
    wawvr::mod::MenuButtonGestureState state{};
    static_cast<void>(wawvr::mod::update_menu_button_gesture(
        true, false, 100, 1'000, &state));
    auto update = wawvr::mod::update_menu_button_gesture(
        true, false, 1'100, 1'000, &state);
    expect(update.long_hold_recognized && update.waiting_for_valid_pose &&
               !update.recenter_capture_requested,
           "threshold consumes gesture while waiting for valid tracking");
    update = wawvr::mod::update_menu_button_gesture(
        true, false, 1'200, 1'000, &state);
    expect(!update.waiting_for_valid_pose,
           "invalid-pose wait notification is emitted once");
    update = wawvr::mod::update_menu_button_gesture(
        false, false, 1'300, 1'000, &state);
    expect(!update.short_tap_released,
           "invalid tracking cannot turn a one-second hold into Escape");
}

void test_pose_recovery_captures_without_restarting_timer() {
    wawvr::mod::MenuButtonGestureState state{};
    static_cast<void>(wawvr::mod::update_menu_button_gesture(
        true, false, 100, 1'000, &state));
    static_cast<void>(wawvr::mod::update_menu_button_gesture(
        true, false, 1'100, 1'000, &state));
    const auto update = wawvr::mod::update_menu_button_gesture(
        true, true, 1'101, 1'000, &state);
    expect(update.recenter_capture_requested && state.recenter_captured,
           "first recovered pose is captured without another one-second hold");
}

void test_delayed_release_is_classified_by_elapsed_time() {
    wawvr::mod::MenuButtonGestureState state{};
    static_cast<void>(wawvr::mod::update_menu_button_gesture(
        true, true, 100, 1'000, &state));
    const auto update = wawvr::mod::update_menu_button_gesture(
        false, true, 1'100, 1'000, &state);
    expect(update.long_hold_recognized &&
               update.recenter_capture_requested &&
               !update.short_tap_released,
           "a delayed release after one second recenters instead of Escape");
}

}  // namespace

int main() {
    test_short_tap_emits_only_on_release();
    test_long_hold_recenters_and_consumes_release();
    test_invalid_pose_still_consumes_long_hold();
    test_pose_recovery_captures_without_restarting_timer();
    test_delayed_release_is_classified_by_elapsed_time();
    if (failures != 0) {
        std::cerr << failures << " menu-button gesture test(s) failed\n";
        return 1;
    }
    std::cout << "menu-button gesture tests passed\n";
    return 0;
}
