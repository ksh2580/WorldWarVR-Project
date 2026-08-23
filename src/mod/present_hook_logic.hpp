#pragma once

#include "xr_types.h"

#include "immutable_thunk_route.hpp"

#include <cstddef>
#include <cstdint>
#include <array>

namespace wawvr::mod {

// Exact Windows SDK vtable slots used by this gate.
inline constexpr std::size_t kDirect3DDevice9ResetIndex = 16;
inline constexpr std::size_t kDirect3DSwapChain9PresentIndex = 3;

struct HookSlotPlan final {
    void* original{};
    void* replacement{};
};

struct PresentTargetIdentity final {
    std::uintptr_t device_id{};
    std::uintptr_t swap_chain_id{};
};

enum class PresentTargetMonitorAction : std::uint8_t {
    wait_for_target,
    install_target,
    keep_target,
    rebind_target,
};

// A T4 frontend/profile transition can temporarily remove or replace the
// published D3D9 device and swap chain. A missing candidate is not evidence
// that the installed target has become unsafe, while a different fully
// validated target is a recoverable rebind rather than a renderer shutdown.
// Explicit process/mod shutdown is intentionally handled outside this plan.
[[nodiscard]] PresentTargetMonitorAction plan_present_target_monitor(
    bool target_installed,
    PresentTargetIdentity installed_target,
    bool candidate_validated,
    PresentTargetIdentity candidate_target) noexcept;

// Rebinding is destructive to the old hook slots, so the target observed by
// the monitor must still be the exact validated engine target after teardown
// and immediately before the replacement is installed.
[[nodiscard]] bool present_target_still_current(
    PresentTargetIdentity expected_target,
    bool current_target_validated,
    PresentTargetIdentity current_target) noexcept;

enum class ProjectionSubmission : unsigned char {
    none,
    current,
    previous,
};

struct ProjectionRecoveryPlan final {
    ProjectionSubmission submission{ProjectionSubmission::none};
    bool abandon_xr{};
};

// A lost D3D9 device must not strand a live OpenXR session on zero-layer
// frames. OpenXR permits xrEndFrame to reference the most recently released
// swapchain image again, so retain the prior projection or finite quad until
// capture recovers. Without any previous layer, leave XR rather than display
// the runtime's loading state forever.
[[nodiscard]] ProjectionRecoveryPlan plan_projection_recovery(
    bool current_projection_rendered,
    bool previous_projection_available,
    bool d3d9_device_lost) noexcept;

// Converts the recovery source into an explicit OpenXR primitive. Mono
// presentation is quad-only: a missing current quad or a cached projection
// yields no layer instead of recreating a head-following menu.
[[nodiscard]] wawvr::xr::CompositionLayerKind select_composition_layer_kind(
    ProjectionSubmission submission,
    bool current_quad_valid,
    bool previous_submission_was_quad,
    bool mono_presentation) noexcept;

// Rendering directly into an eye swapchain replaces that swapchain's most
// recently released image. If a multi-eye attempt then fails, old layer
// metadata can no longer safely be resubmitted with the now-mixed images.
[[nodiscard]] bool reusable_layer_intact_after_render_attempt(
    bool current_layer_rendered,
    std::uint32_t released_eye_mask) noexcept;

// A complete render replaces both released images before xrEndFrame. If frame
// submission then fails, old layer metadata must not survive that overwrite.
[[nodiscard]] bool reusable_layer_intact_after_frame_end(
    bool current_layer_rendered,
    bool frame_end_succeeded) noexcept;

struct PostComFramePlan final {
    bool allow_fresh_projection{};
    bool prime_next_frame{};
};

// OpenXR pacing belongs after Com_Frame returns, never inside Present. A
// cooperative no-Present/minimized engine frame still has to end and re-prime
// its XR frame; it simply cannot submit a fresh D3D9 projection.
[[nodiscard]] PostComFramePlan plan_post_com_frame_service(
    bool present_seen,
    bool present_succeeded,
    bool d3d9_device_cooperative,
    bool reusable_projection_available,
    bool reset_in_progress) noexcept;

// Plans one compare/exchange of an existing COM vtable slot.  This avoids
// replacing a live D3D object vptr or guessing the implementation's complete
// (potentially extended) vtable length.
[[nodiscard]] bool build_hook_slot_plan(
    void* current,
    void* replacement,
    HookSlotPlan* plan) noexcept;

enum class VtableOwnership : unsigned char {
    original,
    installed_by_us,
    foreign,
};

[[nodiscard]] VtableOwnership classify_hook_slot_ownership(
    const void* current,
    const void* original,
    const void* replacement) noexcept;

// Bootstrap output is deliberately duplicate-mono: both OpenXR eyes sample
// the complete stock backbuffer.  True stereo replaces this layout later at
// the engine scene-render boundary.
[[nodiscard]] wawvr::xr::StereoSourceLayout duplicate_mono_layout() noexcept;

inline constexpr std::int32_t kT4ActivePresentationConnectionState = 10;
inline constexpr std::uint32_t kT4ConsoleKeyCatcher = 0x01;
inline constexpr std::uint32_t kT4UiKeyCatcher = 0x10;

enum class PresentationMode : std::uint8_t {
    stereo,
    full_frame_mono,
    active_ui_mono,
    active_console_mono,
};

enum class ActiveUiMonoSource : std::uint8_t {
    left_eye,
    right_eye,
    full_frame,
};

// The physical panel anchor belongs to one contiguous mono interval. Record
// stereo/mono transitions even when pixel capture fails so a later mono
// re-entry can never inherit a stale room position from an earlier interval.
[[nodiscard]] bool presentation_transition_resets_comfort_anchor(
    bool previous_mode_valid,
    PresentationMode previous_mode,
    PresentationMode current_mode) noexcept;

// WaW MP enters the team/class UI through the gameplay R_RenderScene call.
// Packing that call while a catcher is active leaves a complete SBS frame
// underneath a mono panel. Render stock mono until MP is both CA_ACTIVE and
// catcher-free; SP retains its already headset-tested scene path.
[[nodiscard]] bool should_pack_stereo_scene(
    bool multiplayer_profile,
    bool state_valid,
    std::int32_t connection_state,
    std::int32_t active_connection_state,
    std::uint32_t key_catchers) noexcept;

// Both exact supported SP and MP images use connection state 10 for the active
// game. The caller supplies the bound active value.
// If exact state binding is unavailable, only a confirmed stereo scene is
// treated as gameplay and every no-scene frame falls back to safe full mono.
[[nodiscard]] PresentationMode classify_presentation_mode(
    bool state_valid,
    std::int32_t connection_state,
    std::uint32_t key_catchers,
    bool stereo_scene_confirmed,
    std::int32_t active_connection_state =
        kT4ActivePresentationConnectionState) noexcept;

[[nodiscard]] wawvr::xr::StereoSourceLayout presentation_layout(
    PresentationMode mode,
    ActiveUiMonoSource active_ui_source) noexcept;

// Supplies the compact symmetric optical envelope used to aspect-fit a mono
// source before it is placed on the finite OpenXR quad. The duplicate pose is
// retained only for the guarded projection fallback.
[[nodiscard]] std::array<wawvr::xr::EyeView, wawvr::xr::kEyeCount>
monoscopic_comfort_views(const wawvr::xr::Posef& center_pose) noexcept;

// Preserves tracked position and yaw while discarding pitch/roll from the
// reference orientation. An off-head HMD cannot make the horizon permanent.
[[nodiscard]] wawvr::xr::Posef leveled_tracking_anchor(
    const wawvr::xr::Posef& head_center,
    const wawvr::xr::Quaternionf* fallback_yaw = nullptr) noexcept;

} // namespace wawvr::mod
