// SPDX-License-Identifier: GPL-3.0-only
#include "controller_state.hpp"
#include "tracking_anchor_sync.hpp"

#include <windows.h>

#include <cstring>
#include <type_traits>

namespace wawvr::mod {
namespace {

static_assert(std::is_trivially_copyable_v<ControllerFrameSnapshot>);

SRWLOCK g_controller_frame_lock = SRWLOCK_INIT;
ControllerFrameSnapshot g_controller_frame{};
bool g_controller_frame_available = false;
std::uint64_t g_controller_frame_generation = 0;
wawvr::xr::Quaternionf g_pending_tracking_anchor_orientation{};
bool g_tracking_anchor_rebase_pending = false;

}  // namespace

void publish_controller_frame(
    const wawvr::xr::FrameState& frame,
    const wawvr::xr::Posef& tracking_anchor) noexcept {
    ControllerFrameSnapshot next{};
    next.frame = frame;
    next.tracking_anchor = tracking_anchor;
    next.publication_milliseconds = GetTickCount64();

    TrackingAnchorSyncLock sync(tracking_anchor_sync_mutex());
    AcquireSRWLockExclusive(&g_controller_frame_lock);
    next.generation = ++g_controller_frame_generation;
    g_controller_frame = next;
    g_controller_frame_available = true;
    ReleaseSRWLockExclusive(&g_controller_frame_lock);
}

void clear_controller_frame() noexcept {
    TrackingAnchorSyncLock sync(tracking_anchor_sync_mutex());
    AcquireSRWLockExclusive(&g_controller_frame_lock);
    ++g_controller_frame_generation;
    g_controller_frame = {};
    g_controller_frame.generation = g_controller_frame_generation;
    g_controller_frame_available = false;
    ReleaseSRWLockExclusive(&g_controller_frame_lock);
}

void discard_controller_tracking_anchor_rebase() noexcept {
    TrackingAnchorSyncLock sync(tracking_anchor_sync_mutex());
    AcquireSRWLockExclusive(&g_controller_frame_lock);
    g_pending_tracking_anchor_orientation = {};
    g_tracking_anchor_rebase_pending = false;
    ReleaseSRWLockExclusive(&g_controller_frame_lock);
}

bool read_controller_frame(ControllerFrameSnapshot* const snapshot) noexcept {
    if (snapshot == nullptr) {
        return false;
    }

    TrackingAnchorSyncLock sync(tracking_anchor_sync_mutex());
    AcquireSRWLockShared(&g_controller_frame_lock);
    const bool available = g_controller_frame_available;
    if (available) {
        *snapshot = g_controller_frame;
    }
    ReleaseSRWLockShared(&g_controller_frame_lock);
    return available;
}

bool rebase_controller_frame_tracking_anchor(
    const std::uint64_t expected_generation,
    const std::uint64_t expected_frame_id,
    const wawvr::xr::Quaternionf& expected_orientation,
    const wawvr::xr::Quaternionf& desired_orientation) noexcept {
    TrackingAnchorSyncLock sync(tracking_anchor_sync_mutex());
    AcquireSRWLockExclusive(&g_controller_frame_lock);
    const bool matches = g_controller_frame_available &&
        g_controller_frame.generation == expected_generation &&
        g_controller_frame.frame.frame_id == expected_frame_id &&
        std::memcmp(
            &g_controller_frame.tracking_anchor.orientation,
            &expected_orientation, sizeof(expected_orientation)) == 0;
    if (matches) {
        g_controller_frame.tracking_anchor.orientation = desired_orientation;
        g_pending_tracking_anchor_orientation = desired_orientation;
        g_tracking_anchor_rebase_pending = true;
    }
    ReleaseSRWLockExclusive(&g_controller_frame_lock);
    return matches;
}

bool consume_controller_tracking_anchor_rebase(
    wawvr::xr::Quaternionf* const desired_orientation) noexcept {
    if (desired_orientation == nullptr) {
        return false;
    }
    TrackingAnchorSyncLock sync(tracking_anchor_sync_mutex());
    AcquireSRWLockExclusive(&g_controller_frame_lock);
    const bool pending = g_tracking_anchor_rebase_pending;
    if (pending) {
        *desired_orientation = g_pending_tracking_anchor_orientation;
        g_pending_tracking_anchor_orientation = {};
        g_tracking_anchor_rebase_pending = false;
    }
    ReleaseSRWLockExclusive(&g_controller_frame_lock);
    return pending;
}

}  // namespace wawvr::mod
