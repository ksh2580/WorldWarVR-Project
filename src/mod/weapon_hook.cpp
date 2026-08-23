// SPDX-License-Identifier: GPL-3.0-only
// WorldAtWarVR's independently structured T4/WaW viewmodel integration.
// Implemented against validated T4 weapon and command boundaries.
#include "weapon_hook.hpp"

#include "camera_comfort_logic.hpp"

#include "controller_state.hpp"
#include "input_mapping.hpp"
#include "peer_thread_quiescence.hpp"
#include "rocket_barrage_logic.hpp"
#include "stereo_diagnostics.hpp"
#include "t4_layout_selector.hpp"
#include "viewmodel_filter.hpp"
#include "weapon_ballistics_logic.hpp"
#include "weapon_placement.hpp"

#include "t4/hook_api.hpp"
#include "t4/profile.hpp"
#include "xr_types.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace wawvr::mod {

extern "C" void __cdecl wawvr_apply_physical_muzzle_from_bridge(
    void* weapon_parms,
    void* firing_entity) noexcept;
extern "C" void __cdecl wawvr_apply_fixed_ads_spread_from_bridge(
    void* weapon_parms,
    void* attacker,
    float* spread_degrees) noexcept;
extern "C" const float* __cdecl
wawvr_select_rocket_barrage_angles_from_bridge(
    void* entity,
    const float* native_angles) noexcept;

namespace {

constexpr std::size_t kCallInstructionSize = 5;
constexpr std::size_t kSpClientCurrentWeaponOffset = 0x104;
constexpr std::size_t kSpClientFallbackWeaponOffset = 0x20F8;
constexpr std::size_t kWeaponDefinitionNameOffset = 0x00;
constexpr std::size_t kWeaponDefinitionPointerTableExtent =
    (kMaximumSerializedT4WeaponId + 1U) * sizeof(std::uint32_t);
constexpr std::array<std::uint8_t, 15> kSpCgAddPlayerWeaponSentinel{
    0xA1, 0xE0, 0x32, 0x47, 0x03,
    0x8B, 0x50, 0x20,
    0x83, 0xEC, 0x18,
    0xF6, 0xC2, 0x06,
    0x53,
};
constexpr std::array<std::uint8_t, 31> kSpCgUpdateViewModelPoseSentinel{
    0x51, 0x85, 0xC0, 0x74, 0x10, 0x6A, 0x30, 0x83,
    0xC0, 0x14, 0x6A, 0x00, 0x50, 0xE8, 0x2E, 0x84,
    0x34, 0x00, 0x83, 0xC4, 0x0C, 0xB8, 0xE4, 0xCF,
    0x5C, 0x03, 0xB9, 0xA8, 0xB6, 0x52, 0x03,
};
constexpr std::array<std::uint8_t, 25> kSpCgDObjGetWorldTagPosSentinel{
    0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x56, 0x8D,
    0x44, 0x24, 0x0B, 0x50, 0x51, 0x8B, 0xCF, 0xC6,
    0x44, 0x24, 0x13, 0xFE, 0xE8, 0xD7, 0x93, 0x1C, 0x00,
};
constexpr std::array<std::uint8_t, 36> kSpViewmodelPoseContextSentinel{
    0x83, 0xBD, 0x48, 0x01, 0x00, 0x00, 0x09, 0x75,
    0x09, 0x8B, 0x44, 0x24, 0x38, 0xE8, 0xE6, 0x14,
    0x1C, 0x00, 0x8B, 0x07, 0xE8, 0x0F, 0xE2, 0xFF,
    0xFF, 0x83, 0x7C, 0x24, 0x3C, 0x00, 0x0F, 0x84,
    0x00, 0x01, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 100> kSpViewmodelCompositionSentinel{
    0x8B, 0x43, 0x0C, 0x83, 0xC4, 0x0C, 0x85, 0xC0,
    0x0F, 0x84, 0xD2, 0x01, 0x00, 0x00, 0x8B, 0x4B,
    0x4C, 0x85, 0xC9, 0x0F, 0x84, 0xC7, 0x01, 0x00,
    0x00, 0x66, 0x8B, 0x15, 0x5A, 0x3C, 0xF3, 0x01,
    0x53, 0x66, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x00,
    0x66, 0x89, 0x54, 0x24, 0x30, 0xC6, 0x44, 0x24,
    0x2A, 0x00, 0xC6, 0x44, 0x24, 0x32, 0x00, 0x89,
    0x4C, 0x24, 0x24, 0x89, 0x44, 0x24, 0x2C, 0xE8,
    0xAC, 0xFD, 0xFF, 0xFF, 0x8B, 0xC8, 0x8B, 0x44,
    0x24, 0x3C, 0x83, 0xC4, 0x04, 0x50, 0x51, 0x89,
    0x4D, 0x30, 0x8D, 0x4C, 0x24, 0x28, 0x6A, 0x02,
    0x8D, 0x86, 0x00, 0x08, 0x00, 0x00, 0x51, 0xE8,
    0x2C, 0x9C, 0x13, 0x00,
};
constexpr std::array<std::uint8_t, 18> kMpCgAddPlayerWeaponSentinel{
    0x83, 0xEC, 0x1C, 0x53, 0x8B, 0x5C, 0x24, 0x2C,
    0x55, 0x56, 0x57, 0x8B, 0xF8, 0xA1, 0x04, 0xFD,
    0x98, 0x00,
};
constexpr std::array<std::uint8_t, 31> kMpCgUpdateViewModelPoseSentinel{
    0x51, 0x85, 0xC0, 0x74, 0x10, 0x6A, 0x30, 0x83,
    0xC0, 0x14, 0x6A, 0x00, 0x50, 0xE8, 0x9E, 0xB6,
    0x32, 0x00, 0x83, 0xC4, 0x0C, 0xB8, 0x9C, 0xC3,
    0xA8, 0x00, 0xB9, 0x04, 0xEC, 0x9E, 0x00,
};
constexpr std::array<std::uint8_t, 25> kMpCgDObjGetWorldTagPosSentinel{
    0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x56, 0x8D,
    0x44, 0x24, 0x0B, 0x50, 0x51, 0x8B, 0xCF, 0xC6,
    0x44, 0x24, 0x13, 0xFE, 0xE8, 0x67, 0x5C, 0x19, 0x00,
};
constexpr std::array<std::uint8_t, 36> kMpViewmodelPoseContextSentinel{
    0x83, 0xBD, 0x48, 0x01, 0x00, 0x00, 0x09, 0x75,
    0x09, 0x8B, 0x44, 0x24, 0x38, 0xE8, 0xB1, 0x88,
    0x1B, 0x00, 0x8B, 0x03, 0xE8, 0x9A, 0xE2, 0xFF,
    0xFF, 0x83, 0x7C, 0x24, 0x3C, 0x00, 0x0F, 0x84,
    0x30, 0x01, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 100> kMpViewmodelCompositionSentinel{
    0x8B, 0x43, 0x0C, 0x83, 0xC4, 0x0C, 0x85, 0xC0,
    0x0F, 0x84, 0xD2, 0x01, 0x00, 0x00, 0x8B, 0x4B,
    0x4C, 0x85, 0xC9, 0x0F, 0x84, 0xC7, 0x01, 0x00,
    0x00, 0x66, 0x8B, 0x15, 0xDA, 0xA9, 0x21, 0x02,
    0x53, 0x66, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x00,
    0x66, 0x89, 0x54, 0x24, 0x30, 0xC6, 0x44, 0x24,
    0x2A, 0x00, 0xC6, 0x44, 0x24, 0x32, 0x00, 0x89,
    0x4C, 0x24, 0x24, 0x89, 0x44, 0x24, 0x2C, 0xE8,
    0xFD, 0xFD, 0xFF, 0xFF, 0x8B, 0xC8, 0x8B, 0x44,
    0x24, 0x3C, 0x83, 0xC4, 0x04, 0x50, 0x51, 0x89,
    0x4D, 0x2C, 0x8D, 0x4C, 0x24, 0x28, 0x6A, 0x02,
    0x8D, 0x86, 0x00, 0x06, 0x00, 0x00, 0x51, 0xE8,
    0x1D, 0xB3, 0x0E, 0x00,
};
constexpr std::array<std::uint8_t, 50> kDObjBoneLayoutSentinel{
    0x55, 0x8B, 0x69, 0x64, 0x56, 0x57, 0x0F, 0xB6,
    0x79, 0x09, 0x0F, 0xB6, 0x49, 0x0A, 0x3B, 0xC1,
    0x89, 0x6C, 0x24, 0x0C, 0x73, 0x1C, 0x33, 0xC9,
    0x85, 0xFF, 0x7E, 0x16, 0x90, 0x8B, 0x74, 0x8D,
    0x00, 0x0F, 0xB6, 0x56, 0x04, 0x3B, 0xC2, 0x72,
    0x52, 0x83, 0xC1, 0x01, 0x2B, 0xC2, 0x3B, 0xCF,
    0x7C, 0xEB,
};
constexpr std::array<std::uint8_t, 34> kRendererModelLayoutSentinel{
    0x8B, 0x42, 0x64, 0x8B, 0x04, 0xB0, 0x0F, 0xB6,
    0x48, 0x04, 0x89, 0x4C, 0x24, 0x10, 0x8B, 0x4D,
    0x08, 0x0F, 0xBE, 0x4C, 0x31, 0x48, 0x85, 0xC9,
    0x0F, 0x8C, 0xF8, 0x01, 0x00, 0x00, 0x8B, 0x50,
    0x1C, 0x89,
};
constexpr std::array<std::uint8_t, 48> kRendererHidePartBitsSentinel{
    0xF3, 0x0F, 0x7E, 0x42, 0x50, 0xC1, 0xE8, 0x05,
    0x85, 0xF6, 0x66, 0x0F, 0xD6, 0x84, 0x24, 0x90,
    0x00, 0x00, 0x00, 0xF3, 0x0F, 0x7E, 0x42, 0x58,
    0x66, 0xC7, 0x44, 0x24, 0x44, 0x00, 0x00, 0x66,
    0x0F, 0xD6, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00,
    0x0F, 0x86, 0x6F, 0x01, 0x00, 0x00, 0xF7, 0xD8,
};
constexpr std::array<std::uint8_t, 57> kRendererHiddenSurfaceCullSentinel{
    0x8B, 0x8C, 0x24, 0x9C, 0x00, 0x00, 0x00, 0x8B,
    0xBC, 0x24, 0x98, 0x00, 0x00, 0x00, 0x23, 0xFA,
    0x23, 0xCE, 0x0B, 0xCF, 0x8B, 0xBC, 0x24, 0x94,
    0x00, 0x00, 0x00, 0x23, 0xF8, 0x0B, 0xCF, 0x8B,
    0xBC, 0x24, 0x90, 0x00, 0x00, 0x00, 0x23, 0xBC,
    0x24, 0xA0, 0x00, 0x00, 0x00, 0x0B, 0xCF, 0x8B,
    0x7C, 0x24, 0x24, 0x74, 0x0B, 0xC7, 0x07, 0xFD,
    0xFF,
};

struct WeaponExecutableLayout final {
    T4LayoutFamily family{T4LayoutFamily::unsupported};
    bool add_player_weapon_uses_eax_centity{};
    bool overwrite_authoritative_weapon_basis{};
    wawvr::t4::Rva cg_add_player_weapon_rva{};
    std::span<const std::uint8_t> cg_add_player_weapon_sentinel{};
    wawvr::t4::Rva cg_update_viewmodel_pose_rva{};
    std::span<const std::uint8_t> cg_update_viewmodel_pose_sentinel{};
    wawvr::t4::Rva cg_dobj_get_world_tag_pos_rva{};
    std::span<const std::uint8_t> cg_dobj_get_world_tag_pos_sentinel{};
    wawvr::t4::Rva viewmodel_pose_context_rva{};
    std::span<const std::uint8_t> viewmodel_pose_context_sentinel{};
    wawvr::t4::Rva viewmodel_composition_rva{};
    std::span<const std::uint8_t> viewmodel_composition_sentinel{};
    wawvr::t4::Rva dobj_bone_layout_rva{};
    wawvr::t4::Rva renderer_model_layout_rva{};
    wawvr::t4::Rva renderer_hide_part_bits_rva{};
    wawvr::t4::Rva renderer_hidden_surface_cull_rva{};
    wawvr::t4::Rva gameplay_refdef_origin_rva{};
    wawvr::t4::Rva gameplay_refdef_axis_rva{};
    wawvr::t4::Rva viewmodel_axis_origin_rva{};
    wawvr::t4::Rva viewmodel_pose_rva{};
    std::size_t viewmodel_pose_extent{};
    wawvr::t4::Rva tag_flash_word_rva{};
    wawvr::t4::Rva tag_inhand_word_rva{};
    wawvr::t4::Rva tag_origin_word_rva{};
    wawvr::t4::Rva tag_weapon_word_rva{};
    wawvr::t4::Rva tag_weapon_right_word_rva{};
    std::size_t weapon_parms_size{};
    std::size_t weapon_parms_forward_offset{};
    std::size_t weapon_parms_right_offset{};
    std::size_t weapon_parms_up_offset{};
    std::size_t weapon_parms_muzzle_trace_offset{};
    std::size_t weapon_parms_weapon_definition_offset{};
    std::size_t gentity_size{};
    std::size_t gentity_number_offset{};
    std::size_t gentity_client_offset{};
    std::size_t weapon_definition_type_offset{};
    std::size_t weapon_definition_ads_spread_offset{};
};

const WeaponExecutableLayout kSpWeaponLayout{
    .family = T4LayoutFamily::single_player_1_7_1263,
    .add_player_weapon_uses_eax_centity = false,
    .overwrite_authoritative_weapon_basis = false,
    .cg_add_player_weapon_rva = 0x000697A0,
    .cg_add_player_weapon_sentinel = kSpCgAddPlayerWeaponSentinel,
    .cg_update_viewmodel_pose_rva = 0x00067B00,
    .cg_update_viewmodel_pose_sentinel = kSpCgUpdateViewModelPoseSentinel,
    .cg_dobj_get_world_tag_pos_rva = 0x00043030,
    .cg_dobj_get_world_tag_pos_sentinel = kSpCgDObjGetWorldTagPosSentinel,
    .viewmodel_pose_context_rva = 0x000698D8,
    .viewmodel_pose_context_sentinel = kSpViewmodelPoseContextSentinel,
    .viewmodel_composition_rva = 0x00064C60,
    .viewmodel_composition_sentinel = kSpViewmodelCompositionSentinel,
    .dobj_bone_layout_rva = 0x0020C433,
    .renderer_model_layout_rva = 0x0031E0B0,
    .renderer_hide_part_bits_rva = 0x0031E121,
    .renderer_hidden_surface_cull_rva = 0x0031E21D,
    .gameplay_refdef_origin_rva = 0x03120354,
    .gameplay_refdef_axis_rva = 0x03120364,
    .viewmodel_axis_origin_rva = 0x0312B6CC,
    .viewmodel_pose_rva = 0x031CCFB4,
    .viewmodel_pose_extent = 0x40,
    .tag_flash_word_rva = 0x01B33C42,
    .tag_inhand_word_rva = 0x01B33C4E,
    .tag_origin_word_rva = 0x01B33C58,
    .tag_weapon_word_rva = 0x01B33C5A,
    .tag_weapon_right_word_rva = 0x01B33C60,
    .weapon_parms_size = 0x40,
    .weapon_parms_forward_offset = 0x00,
    .weapon_parms_right_offset = 0x0C,
    .weapon_parms_up_offset = 0x18,
    .weapon_parms_muzzle_trace_offset = 0x24,
    .weapon_parms_weapon_definition_offset = 0x3C,
    .gentity_size = 0x378,
    .gentity_number_offset = 0x00,
    .gentity_client_offset = 0x180,
    .weapon_definition_type_offset = 0x144,
    .weapon_definition_ads_spread_offset = 0x830,
};

const WeaponExecutableLayout kMpWeaponLayout{
    .family = T4LayoutFamily::multiplayer_1_7_1263,
    .add_player_weapon_uses_eax_centity = true,
    .overwrite_authoritative_weapon_basis = true,
    .cg_add_player_weapon_rva = 0x0007F410,
    .cg_add_player_weapon_sentinel = kMpCgAddPlayerWeaponSentinel,
    .cg_update_viewmodel_pose_rva = 0x0007D7D0,
    .cg_update_viewmodel_pose_sentinel = kMpCgUpdateViewModelPoseSentinel,
    .cg_dobj_get_world_tag_pos_rva = 0x000464E0,
    .cg_dobj_get_world_tag_pos_sentinel = kMpCgDObjGetWorldTagPosSentinel,
    .viewmodel_pose_context_rva = 0x0007F51D,
    .viewmodel_pose_context_sentinel = kMpViewmodelPoseContextSentinel,
    .viewmodel_composition_rva = 0x0007C18F,
    .viewmodel_composition_sentinel = kMpViewmodelCompositionSentinel,
    .dobj_bone_layout_rva = 0x001DC173,
    .renderer_model_layout_rva = 0x002F5E80,
    .renderer_hide_part_bits_rva = 0x002F5EF1,
    .renderer_hidden_surface_cull_rva = 0x002F5FED,
    .gameplay_refdef_origin_rva = 0x005E6788,
    .gameplay_refdef_axis_rva = 0x005E6798,
    .viewmodel_axis_origin_rva = 0x005EEC28,
    .viewmodel_pose_rva = 0x0068C368,
    .viewmodel_pose_extent = 0x40,
    .tag_flash_word_rva = 0x01E1A9C2,
    .tag_inhand_word_rva = 0x01E1A9CE,
    .tag_origin_word_rva = 0x01E1A9D8,
    .tag_weapon_word_rva = 0x01E1A9DA,
    .tag_weapon_right_word_rva = 0x01E1A9E0,
    .weapon_parms_size = 0x40,
    .weapon_parms_forward_offset = 0x00,
    .weapon_parms_right_offset = 0x0C,
    .weapon_parms_up_offset = 0x18,
    .weapon_parms_muzzle_trace_offset = 0x24,
    .weapon_parms_weapon_definition_offset = 0x3C,
    .gentity_size = 0x330,
    .gentity_number_offset = 0x00,
    .gentity_client_offset = 0x184,
    .weapon_definition_type_offset = 0x144,
    .weapon_definition_ads_spread_offset = 0x830,
};

[[nodiscard]] const WeaponExecutableLayout* weapon_layout_for_profile(
    const wawvr::t4::ExecutableProfile& profile) noexcept {
    switch (select_t4_layout_family(profile)) {
    case T4LayoutFamily::single_player_1_7_1263:
        return &kSpWeaponLayout;
    case T4LayoutFamily::multiplayer_1_7_1263:
        return &kMpWeaponLayout;
    case T4LayoutFamily::unsupported:
        return nullptr;
    }
    return nullptr;
}

struct GfxScaledPlacement final {
    wawvr::xr::Quaternionf quaternion{};
    wawvr::xr::Vec3f origin{};
    float scale{};
};

static_assert(sizeof(GfxScaledPlacement) == 0x20);
static_assert(offsetof(GfxScaledPlacement, quaternion) == 0x00);
static_assert(offsetof(GfxScaledPlacement, origin) == 0x10);
static_assert(offsetof(GfxScaledPlacement, scale) == 0x1C);

using CgAddPlayerWeaponFunction = void(__cdecl*)(
    std::int32_t,
    const GfxScaledPlacement*,
    const void*,
    void*,
    std::int32_t);

struct FinalVisibleAim final {
    bool valid{};
    std::uint64_t controller_generation{};
    std::uint64_t publication_milliseconds{};
    float pitch_degrees{};
    float yaw_degrees{};
    wawvr::xr::Basis3f axis{};
};

struct ActiveWeaponPoseContext final {
    bool valid{};
    std::uint64_t controller_generation{};
    wawvr::xr::Vec3f tracked_grip_world{};
};

struct GripTagBinding final {
    std::uintptr_t address{};
    const char* name{};
};

std::atomic<bool> g_weapon_hook_installed{false};
std::atomic<bool> g_weapon_hook_enabled{false};
std::atomic<bool> g_campaign_targeting_hook_installed{false};
std::atomic<bool> g_campaign_targeting_hook_enabled{false};
const WeaponExecutableLayout* g_weapon_layout = nullptr;
std::uintptr_t g_original_add_player_weapon = 0;
std::uintptr_t g_original_update_viewmodel_pose = 0;
std::uintptr_t g_original_calc_muzzle_points = 0;
std::uintptr_t g_original_bullet_fire = 0;
std::uintptr_t g_original_client_bullet_view_origin = 0;
std::uintptr_t g_original_get_spread_for_weapon = 0;
std::uintptr_t g_original_scr_add_vector = 0;
std::uintptr_t g_cg_dobj_get_world_tag_pos = 0;
std::uintptr_t g_camera_origin_address = 0;
std::uintptr_t g_camera_axis_address = 0;
std::uintptr_t g_cgame_gun_pitch_address = 0;
std::uintptr_t g_cgame_gun_yaw_address = 0;
std::uintptr_t g_viewmodel_axis_origin_address = 0;
std::uintptr_t g_viewmodel_pose_address = 0;
std::array<GripTagBinding, 4> g_grip_tags{};
std::uintptr_t g_tag_flash_address = 0;
std::uintptr_t g_local_player_entity_address = 0;
std::uintptr_t g_weapon_definition_pointer_table_address = 0;
std::uintptr_t g_weapon_definition_count_address = 0;
WeaponAttachmentState g_weapon_attachment{};
wawvr::xr::Posef g_weapon_attachment_anchor{};
bool g_weapon_attachment_anchor_valid = false;
thread_local ActiveWeaponPoseContext g_active_weapon_pose{};
SRWLOCK g_final_visible_aim_lock = SRWLOCK_INIT;
FinalVisibleAim g_final_visible_aim{};
SRWLOCK g_published_muzzle_lock = SRWLOCK_INIT;
PublishedWeaponMuzzleSnapshot g_published_muzzle{};
thread_local std::array<float, 3> g_campaign_rocket_angles{};

[[nodiscard]] const float* preserve_native_campaign_angles(
    const float* const native_angles,
    const char* const reason) noexcept {
    static std::atomic_flag rejected_once = ATOMIC_FLAG_INIT;
    stereo_diagnostic_log_once(
        rejected_once,
        "CampaignDiag rocket-angle substitution rejected: %s; native angles preserved",
        reason);
    return native_angles;
}

void log_campaign_angle_acceptance_once(
    const std::uint32_t weapon_id,
    const float pitch_degrees,
    const float yaw_degrees) noexcept {
    static std::atomic_flag accepted_once = ATOMIC_FLAG_INIT;
    stereo_diagnostic_log_once(
        accepted_once,
        "CampaignDiag rocket-angle substitution accepted: exactSpProfile=1 localPlayer=0 weaponId=%u pitch=%.3f yaw=%.3f",
        weapon_id, pitch_degrees, yaw_degrees);
}

[[nodiscard]] bool weapon_hook_disabled_by_environment() noexcept {
    std::array<wchar_t, 8> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"WAWVR_DISABLE_WEAPON", value.data(),
        static_cast<DWORD>(value.size()));
    return length == 1 && value[0] == L'1';
}

[[nodiscard]] bool protection_is_readable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffU;
    return access == PAGE_READONLY || access == PAGE_READWRITE ||
           access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] bool protection_is_writable(const DWORD protection) noexcept {
    const DWORD access = protection & 0xffU;
    return access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] bool accessible_range(
    const void* const address,
    const std::size_t size,
    const bool writable) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (writable ? !protection_is_writable(memory.Protect)
                  : !protection_is_readable(memory.Protect))) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region_begin =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto region_end = region_begin + memory.RegionSize;
    return begin >= region_begin && begin <= region_end &&
           size <= region_end - begin;
}

[[nodiscard]] bool bytes_match(
    const void* const address,
    const std::span<const std::uint8_t> expected) noexcept {
    return accessible_range(address, expected.size(), false) &&
           std::memcmp(address, expected.data(), expected.size()) == 0;
}

[[nodiscard]] bool make_relative_call(
    const std::uintptr_t source,
    const std::uintptr_t destination,
    std::array<std::uint8_t, kCallInstructionSize>* const output) noexcept {
    if (output == nullptr) {
        return false;
    }
    const std::int64_t displacement =
        static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(source + kCallInstructionSize);
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    (*output)[0] = 0xE8;
    const auto encoded = static_cast<std::int32_t>(displacement);
    std::memcpy(output->data() + 1, &encoded, sizeof(encoded));
    return true;
}

[[nodiscard]] std::uintptr_t decode_relative_call_target(
    const std::uintptr_t source,
    const std::array<std::uint8_t, kCallInstructionSize>& bytes) noexcept {
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    return static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(source + kCallInstructionSize) +
        displacement);
}

[[nodiscard]] float normalize_degrees(float value) noexcept {
    value = std::fmod(value, 360.0F);
    if (value < 0.0F) {
        value += 360.0F;
    }
    return value;
}

[[nodiscard]] bool same_pose(
    const wawvr::xr::Posef& left,
    const wawvr::xr::Posef& right) noexcept {
    return left.orientation.x == right.orientation.x &&
           left.orientation.y == right.orientation.y &&
           left.orientation.z == right.orientation.z &&
           left.orientation.w == right.orientation.w &&
           left.position.x == right.position.x &&
           left.position.y == right.position.y &&
           left.position.z == right.position.z;
}

[[nodiscard]] bool aim_degrees_from_forward(
    wawvr::xr::Vec3f forward,
    float* const pitch,
    float* const yaw) noexcept {
    if (pitch == nullptr || yaw == nullptr || !std::isfinite(forward.x) ||
        !std::isfinite(forward.y) || !std::isfinite(forward.z)) {
        return false;
    }
    const float length_squared =
        forward.x * forward.x + forward.y * forward.y +
        forward.z * forward.z;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-8F) {
        return false;
    }
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    forward.x *= inverse_length;
    forward.y *= inverse_length;
    forward.z *= inverse_length;
    constexpr float kRadiansToDegrees =
        180.0F / 3.14159265358979323846F;
    const float horizontal =
        std::sqrt(forward.x * forward.x + forward.y * forward.y);
    *pitch = normalize_degrees(
        std::atan2(-forward.z, horizontal) * kRadiansToDegrees);
    *yaw = normalize_degrees(
        std::atan2(forward.y, forward.x) * kRadiansToDegrees);
    return std::isfinite(*pitch) && std::isfinite(*yaw);
}

[[nodiscard]] bool finite_basis(
    const wawvr::xr::Basis3f& axis) noexcept {
    return std::isfinite(axis.forward.x) &&
           std::isfinite(axis.forward.y) &&
           std::isfinite(axis.forward.z) &&
           std::isfinite(axis.left.x) && std::isfinite(axis.left.y) &&
           std::isfinite(axis.left.z) && std::isfinite(axis.up.x) &&
           std::isfinite(axis.up.y) && std::isfinite(axis.up.z);
}

void publish_final_visible_aim(
    const std::uint64_t generation,
    const float pitch,
    const float yaw,
    const wawvr::xr::Basis3f& axis) noexcept {
    if (generation == 0 || !std::isfinite(pitch) || !std::isfinite(yaw) ||
        !finite_basis(axis)) {
        return;
    }
    AcquireSRWLockExclusive(&g_final_visible_aim_lock);
    g_final_visible_aim = {
        true, generation, GetTickCount64(), pitch, yaw, axis};
    ReleaseSRWLockExclusive(&g_final_visible_aim_lock);
}

[[nodiscard]] bool read_fresh_final_visible_aim(
    const std::uint64_t controller_generation,
    const std::uint64_t now_milliseconds,
    FinalVisibleAim* const output) noexcept {
    if (output == nullptr || controller_generation == 0 ||
        now_milliseconds == 0) {
        return false;
    }
    AcquireSRWLockShared(&g_final_visible_aim_lock);
    const FinalVisibleAim snapshot = g_final_visible_aim;
    ReleaseSRWLockShared(&g_final_visible_aim_lock);
    constexpr std::uint64_t kMaximumRenderedAimAgeMilliseconds = 150;
    constexpr std::uint64_t kMaximumGenerationLag = 4;
    if (!snapshot.valid || snapshot.controller_generation == 0 ||
        snapshot.controller_generation > controller_generation ||
        controller_generation - snapshot.controller_generation >
            kMaximumGenerationLag ||
        snapshot.publication_milliseconds == 0 ||
        now_milliseconds < snapshot.publication_milliseconds ||
        now_milliseconds - snapshot.publication_milliseconds >
            kMaximumRenderedAimAgeMilliseconds ||
        !std::isfinite(snapshot.pitch_degrees) ||
        !std::isfinite(snapshot.yaw_degrees) || !finite_basis(snapshot.axis)) {
        return false;
    }
    *output = snapshot;
    return true;
}

[[nodiscard]] bool read_final_visible_weapon_basis(
    const std::uint64_t controller_generation,
    const std::uint64_t now_milliseconds,
    wawvr::xr::Basis3f* const axis) noexcept {
    FinalVisibleAim snapshot{};
    if (axis == nullptr ||
        !read_fresh_final_visible_aim(
            controller_generation, now_milliseconds, &snapshot)) {
        return false;
    }
    *axis = snapshot.axis;
    return true;
}

void publish_muzzle(
    const std::uint64_t generation,
    const wawvr::xr::Vec3f& origin) noexcept {
    if (generation == 0 || !std::isfinite(origin.x) ||
        !std::isfinite(origin.y) || !std::isfinite(origin.z)) {
        return;
    }
    AcquireSRWLockExclusive(&g_published_muzzle_lock);
    g_published_muzzle = {true, generation, GetTickCount64(), origin};
    ReleaseSRWLockExclusive(&g_published_muzzle_lock);
}

void invalidate_published_muzzle() noexcept {
    AcquireSRWLockExclusive(&g_published_muzzle_lock);
    g_published_muzzle = {};
    ReleaseSRWLockExclusive(&g_published_muzzle_lock);
}

[[nodiscard]] bool read_tag_word(
    const std::uintptr_t address,
    std::uint16_t* const tag) noexcept {
    if (tag == nullptr ||
        !accessible_range(
            reinterpret_cast<const void*>(address), sizeof(*tag), false)) {
        return false;
    }
    std::uint16_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    if (value == 0) {
        return false;
    }
    *tag = value;
    return true;
}

}  // namespace

extern "C" void __cdecl wawvr_post_update_viewmodel_pose(
    void* viewmodel_dobj) noexcept;
extern "C" void __cdecl wawvr_override_local_bullet_origin(
    std::int32_t local_client_number,
    void* output_origin) noexcept;
extern "C" void __cdecl wawvr_override_local_bullet_spread(
    const void* weapon_definition,
    float* minimum_spread_degrees,
    float* maximum_spread_degrees) noexcept;
extern "C" void __cdecl wawvr_add_player_weapon_bridge(
    std::int32_t local_client_number,
    GfxScaledPlacement* placement,
    const void* player_state,
    void* centity,
    std::int32_t draw_gun) noexcept;

#if defined(_MSC_VER) && defined(_M_IX86)
extern "C" __declspec(naked) void
wawvr_add_player_weapon_mp_entry_bridge() noexcept {
    __asm {
        // Native MP contract: EAX=centity and four caller-cleaned stack args.
        // Snapshot the untouched native stack before marshalling the fifth,
        // register-only argument into the ordinary C++ bridge signature.
        mov edx, esp
        push dword ptr [edx + 0x10]
        push eax
        push dword ptr [edx + 0x0C]
        push dword ptr [edx + 0x08]
        push dword ptr [edx + 0x04]
        call wawvr_add_player_weapon_bridge
        add esp, 0x14
        ret
    }
}

extern "C" __declspec(naked) void __cdecl
wawvr_call_add_player_weapon_mp_original(
    std::int32_t,
    const GfxScaledPlacement*,
    const void*,
    void*,
    std::int32_t) noexcept {
    __asm {
        push ebp
        mov ebp, esp
        // Recreate the stock MP contract: centity in EAX, with only the four
        // native arguments on the stock function's caller-cleaned stack.
        push dword ptr [ebp + 0x18]
        push dword ptr [ebp + 0x10]
        push dword ptr [ebp + 0x0C]
        push dword ptr [ebp + 0x08]
        mov eax, dword ptr [ebp + 0x14]
        call dword ptr [g_original_add_player_weapon]
        add esp, 0x10
        mov esp, ebp
        pop ebp
        ret
    }
}

extern "C" __declspec(naked) int __cdecl
wawvr_call_dobj_get_world_tag_pos(
    void*, std::uint32_t, const void*, wawvr::xr::Vec3f*) noexcept {
    __asm {
        push ebp
        mov ebp, esp
        push edi
        mov edi, dword ptr [ebp + 8]
        mov ecx, dword ptr [ebp + 0x0C]
        push dword ptr [ebp + 0x14]
        push dword ptr [ebp + 0x10]
        call dword ptr [g_cg_dobj_get_world_tag_pos]
        add esp, 8
        pop edi
        mov esp, ebp
        pop ebp
        ret
    }
}

extern "C" __declspec(naked) void __cdecl
wawvr_call_update_viewmodel_pose(void*) noexcept {
    __asm {
        mov eax, dword ptr [esp + 4]
        call dword ptr [g_original_update_viewmodel_pose]
        ret
    }
}

extern "C" __declspec(naked) void
wawvr_update_viewmodel_pose_bridge() noexcept {
    __asm {
        push eax
        call dword ptr [g_original_update_viewmodel_pose]
        pop eax
        push eax
        call wawvr_post_update_viewmodel_pose
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void
wawvr_calc_muzzle_points_bridge() noexcept {
    __asm {
        // Entry contract at FireWeapon+0x53:
        // EAX = weaponParms*, [ESP+4] = firing gentity*. Preserve the
        // original call's custom ABI by duplicating its stack argument.
        push eax
        push dword ptr [esp + 8]
        call dword ptr [g_original_calc_muzzle_points]
        lea esp, [esp + 4]

        // Preserve every caller-observable register, flag and FP/SIMD value
        // while the ordinary C++ helper performs only the guarded +0x24 write.
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push dword ptr [ebx + 44]
        push dword ptr [ebx + 36]
        call wawvr_apply_physical_muzzle_from_bridge
        add esp, 8
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        lea esp, [esp + 4]
        ret
    }
}

extern "C" __declspec(naked) void
wawvr_bullet_fire_bridge() noexcept {
    __asm {
        // Entry contract at FireWeapon's ordinary-bullet call:
        // ESI=weaponParms*, [ESP+4]=attacker, [ESP+8]=final spread.
        // Preserve every caller-observable value while the guarded helper
        // considers replacing only that one stack float.
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, 0FFFFFFF0h
        fxsave [esp]
        lea eax, [ebx + 44]
        push eax
        push dword ptr [ebx + 40]
        push dword ptr [ebx + 4]
        call wawvr_apply_fixed_ads_spread_from_bridge
        add esp, 12
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd

        // Tail transfer preserves Bullet_Fire's original cdecl stack and
        // return address. Disabled/rejected decisions are stock behavior.
        jmp dword ptr [g_original_bullet_fire]
    }
}

extern "C" __declspec(naked) void
wawvr_client_bullet_spread_bridge() noexcept {
    __asm {
        // Exact local DrawBulletImpacts call contract:
        // ESI=playerState*, EDX=minSpread*, ECX=maxSpread*,
        // [ESP+4]=WeaponDef*. Save the output pointers privately, duplicate
        // the caller-cleaned WeaponDef argument, then run native first.
        push ecx
        push edx
        push dword ptr [esp + 0x0C]
        call dword ptr [g_original_get_spread_for_weapon]
        add esp, 4

        // Preserve the native return state while the guarded helper collapses
        // both local visual bounds to the authored ADS spread.
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push dword ptr [ebx + 40]
        push dword ptr [ebx + 36]
        push dword ptr [ebx + 48]
        call wawvr_override_local_bullet_spread
        add esp, 12
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        lea esp, [esp + 8]
        ret
    }
}

extern "C" __declspec(naked) void
wawvr_client_bullet_view_origin_bridge() noexcept {
    __asm {
        // Exact DrawBulletImpacts contract:
        // EAX=localClientNum, [ESP+4]=playerState*, [ESP+8]=outOrigin.
        // Keep a private copy of the register-only client number and recreate
        // the original helper's two stack arguments.
        push eax
        mov edx, esp
        push dword ptr [edx + 0x0C]
        push dword ptr [edx + 0x08]
        call dword ptr [g_original_client_bullet_view_origin]
        add esp, 8
        test al, al
        jz stock_return

        // The stock AL result remains authoritative. On success only, preserve
        // every caller-observable register and FP/SIMD value while replacing
        // the written eye origin with the fresh tracked tag_flash.
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push dword ptr [ebx + 48]
        push dword ptr [ebx + 36]
        call wawvr_override_local_bullet_origin
        add esp, 8
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd

    stock_return:
        lea esp, [esp + 4]
        ret
    }
}

extern "C" __declspec(naked) void
wawvr_get_player_angles_scr_add_vector_bridge() noexcept {
    __asm {
        // Exact SP callsite contract inside GScr_GetPlayerAngles:
        // [ESP] is the native return address, [ESP+4] is float angles[3],
        // and ESI is the resolved gentity. Replace only the vector pointer
        // handed to Scr_AddVector; no native view/player state is written.
        mov eax, dword ptr [esp + 4]
        push eax
        push esi
        call wawvr_select_rocket_barrage_angles_from_bridge
        add esp, 8
        mov dword ptr [esp + 4], eax
        xor eax, eax
        jmp dword ptr [g_original_scr_add_vector]
    }
}
#else
extern "C" int __cdecl wawvr_call_dobj_get_world_tag_pos(
    void*, std::uint32_t, const void*, wawvr::xr::Vec3f*) noexcept {
    return 0;
}

extern "C" void __cdecl wawvr_call_update_viewmodel_pose(void*) noexcept {}
extern "C" void wawvr_add_player_weapon_mp_entry_bridge() noexcept {}
extern "C" void __cdecl wawvr_call_add_player_weapon_mp_original(
    std::int32_t,
    const GfxScaledPlacement*,
    const void*,
    void*,
    std::int32_t) noexcept {}
extern "C" void wawvr_update_viewmodel_pose_bridge() noexcept {}
extern "C" void wawvr_calc_muzzle_points_bridge() noexcept {}
extern "C" void wawvr_bullet_fire_bridge() noexcept {}
extern "C" void wawvr_client_bullet_spread_bridge() noexcept {}
extern "C" void wawvr_client_bullet_view_origin_bridge() noexcept {}
extern "C" void
wawvr_get_player_angles_scr_add_vector_bridge() noexcept {}
#endif

extern "C" const float* __cdecl
wawvr_select_rocket_barrage_angles_from_bridge(
    void* const entity,
    const float* const native_angles) noexcept {
    const bool campaign_enabled =
        g_campaign_targeting_hook_enabled.load(std::memory_order_acquire);
    if (!campaign_enabled) {
        return native_angles;
    }
    const WeaponExecutableLayout* const layout = g_weapon_layout;
    if (!g_weapon_hook_enabled.load(std::memory_order_acquire) ||
        layout == nullptr ||
        layout->family != T4LayoutFamily::single_player_1_7_1263 ||
        native_angles == nullptr || entity == nullptr ||
        reinterpret_cast<std::uintptr_t>(entity) !=
            g_local_player_entity_address ||
        g_weapon_definition_pointer_table_address == 0 ||
        g_weapon_definition_count_address == 0 ||
        g_camera_axis_address == 0) {
        return preserve_native_campaign_angles(
            native_angles, "validated SP hook prerequisites unavailable");
    }

    const auto entity_address = reinterpret_cast<std::uintptr_t>(entity);
    const auto client_pointer_address =
        entity_address + layout->gentity_client_offset;
    if (!accessible_range(
            reinterpret_cast<const void*>(client_pointer_address),
            sizeof(std::uint32_t), false)) {
        return preserve_native_campaign_angles(
            native_angles, "local-player client pointer unreadable");
    }

    std::uint32_t client_address32 = 0;
    std::memcpy(
        &client_address32,
        reinterpret_cast<const void*>(client_pointer_address),
        sizeof(client_address32));
    const std::uintptr_t client_address = client_address32;
    if (client_address == 0 ||
        client_address > std::numeric_limits<std::uintptr_t>::max() -
            kSpClientFallbackWeaponOffset - 1U ||
        !accessible_range(
            reinterpret_cast<const void*>(
                client_address + kSpClientCurrentWeaponOffset),
            sizeof(std::uint32_t), false) ||
        !accessible_range(
            reinterpret_cast<const void*>(
                client_address + kSpClientFallbackWeaponOffset),
            sizeof(std::uint8_t), false)) {
        return preserve_native_campaign_angles(
            native_angles, "local-player weapon fields unreadable");
    }

    std::uint32_t primary_weapon_id = 0;
    std::uint8_t fallback_weapon_id_byte = 0;
    std::uint32_t weapon_count = 0;
    std::memcpy(
        &primary_weapon_id,
        reinterpret_cast<const void*>(
            client_address + kSpClientCurrentWeaponOffset),
        sizeof(primary_weapon_id));
    std::memcpy(
        &fallback_weapon_id_byte,
        reinterpret_cast<const void*>(
            client_address + kSpClientFallbackWeaponOffset),
        sizeof(fallback_weapon_id_byte));
    if (!accessible_range(
            reinterpret_cast<const void*>(
                g_weapon_definition_count_address),
            sizeof(weapon_count), false)) {
        return preserve_native_campaign_angles(
            native_angles, "weapon-definition count unreadable");
    }
    std::memcpy(
        &weapon_count,
        reinterpret_cast<const void*>(g_weapon_definition_count_address),
        sizeof(weapon_count));

    const std::uint32_t fallback_weapon_id = fallback_weapon_id_byte;
    const std::uint32_t effective_weapon_id =
        primary_weapon_id != 0 ? primary_weapon_id : fallback_weapon_id;
    if (weapon_count == 0 ||
        weapon_count > kMaximumSerializedT4WeaponId ||
        effective_weapon_id == 0 || effective_weapon_id > weapon_count) {
        return preserve_native_campaign_angles(
            native_angles, "effective weapon id outside validated bounds");
    }

    const std::uintptr_t table_slot =
        g_weapon_definition_pointer_table_address +
        effective_weapon_id * sizeof(std::uint32_t);
    if (!accessible_range(
            reinterpret_cast<const void*>(table_slot),
            sizeof(std::uint32_t), false)) {
        return preserve_native_campaign_angles(
            native_angles, "weapon-definition table slot unreadable");
    }
    std::uint32_t weapon_definition_address32 = 0;
    std::memcpy(
        &weapon_definition_address32,
        reinterpret_cast<const void*>(table_slot),
        sizeof(weapon_definition_address32));
    const std::uintptr_t weapon_definition_address =
        weapon_definition_address32;
    if (weapon_definition_address == 0 ||
        !accessible_range(
            reinterpret_cast<const void*>(
                weapon_definition_address + kWeaponDefinitionNameOffset),
            sizeof(std::uint32_t), false)) {
        return preserve_native_campaign_angles(
            native_angles, "weapon-definition record unreadable");
    }

    std::uint32_t weapon_name_address32 = 0;
    std::memcpy(
        &weapon_name_address32,
        reinterpret_cast<const void*>(
            weapon_definition_address + kWeaponDefinitionNameOffset),
        sizeof(weapon_name_address32));
    const std::uintptr_t weapon_name_address = weapon_name_address32;
    constexpr std::size_t kRocketNameBytes =
        kRocketBarrageWeaponName.size() + 1U;
    if (weapon_name_address == 0 ||
        !accessible_range(
            reinterpret_cast<const void*>(weapon_name_address),
            kRocketNameBytes, false)) {
        return preserve_native_campaign_angles(
            native_angles, "weapon-name span unreadable");
    }
    std::array<char, kRocketNameBytes> weapon_name{};
    std::memcpy(
        weapon_name.data(),
        reinterpret_cast<const void*>(weapon_name_address),
        weapon_name.size());
    if (weapon_name.back() != '\0') {
        return preserve_native_campaign_angles(
            native_angles, "weapon name is not exactly terminated");
    }

    ControllerFrameSnapshot controller{};
    wawvr::xr::Basis3f camera_axis{};
    const std::uint64_t now_milliseconds = GetTickCount64();
    const bool frame_available = read_controller_frame(&controller);
    const bool frame_current = frame_available &&
        controller_frame_is_current(controller, now_milliseconds);
    if (!frame_current ||
        !accessible_range(
            reinterpret_cast<const void*>(g_camera_axis_address),
            sizeof(camera_axis), false)) {
        return preserve_native_campaign_angles(
            native_angles, "controller frame stale or camera axis unreadable");
    }
    std::memcpy(
        &camera_axis,
        reinterpret_cast<const void*>(g_camera_axis_address),
        sizeof(camera_axis));
    float pitch_degrees = 0.0F;
    float yaw_degrees = 0.0F;
    if (!controller_aim_degrees(
            controller, camera_axis, &pitch_degrees, &yaw_degrees)) {
        return preserve_native_campaign_angles(
            native_angles, "controller aim conversion rejected");
    }

    const RocketBarrageAngleGate gate{
        campaign_enabled &&
            g_weapon_hook_enabled.load(std::memory_order_acquire),
        true,
        reinterpret_cast<std::uintptr_t>(entity),
        g_local_player_entity_address,
        true,
        primary_weapon_id,
        fallback_weapon_id,
        weapon_count,
        weapon_definition_address != 0,
        std::string_view(
            weapon_name.data(), kRocketBarrageWeaponName.size()),
        controller.frame.actions.focused,
        frame_current,
        pitch_degrees,
        yaw_degrees,
    };
    const RocketBarrageAngleSelection selection =
        select_rocket_barrage_angles(gate);
    if (!selection.substitute ||
        !g_campaign_targeting_hook_enabled.load(
            std::memory_order_acquire)) {
        return preserve_native_campaign_angles(
            native_angles, "exact local rocket-barrage gate rejected");
    }
    log_campaign_angle_acceptance_once(
        selection.effective_weapon_id,
        selection.angles[0], selection.angles[1]);
    g_campaign_rocket_angles = selection.angles;
    return g_campaign_rocket_angles.data();
}

extern "C" void __cdecl wawvr_override_local_bullet_origin(
    const std::int32_t local_client_number,
    void* const output_origin) noexcept {
    if (local_client_number != 0 ||
        !g_weapon_hook_enabled.load(std::memory_order_acquire) ||
        !accessible_range(
            output_origin, sizeof(wawvr::xr::Vec3f), true)) {
        return;
    }

    ControllerFrameSnapshot controller{};
    const std::uint64_t now_milliseconds = GetTickCount64();
    RightControllerWeaponPose ignored_pose{};
    if (!read_controller_frame(&controller) ||
        !right_controller_weapon_pose(
            controller, now_milliseconds, &ignored_pose)) {
        return;
    }
    wawvr::xr::Vec3f physical_muzzle{};
    if (!read_published_weapon_muzzle(
            controller.generation, now_milliseconds, &physical_muzzle) ||
        !g_weapon_hook_enabled.load(std::memory_order_acquire)) {
        return;
    }

    wawvr::xr::Vec3f native_origin{};
    std::memcpy(&native_origin, output_origin, sizeof(native_origin));
    std::memcpy(output_origin, &physical_muzzle, sizeof(physical_muzzle));
    WAWVR_STEREO_DIAG_ONCE(
        "WeaponDiag aligned local client tracer/impact origin from eye %.2f %.2f %.2f to tracked tag_flash %.2f %.2f %.2f",
        native_origin.x, native_origin.y, native_origin.z,
        physical_muzzle.x, physical_muzzle.y, physical_muzzle.z);
}

extern "C" void __cdecl wawvr_override_local_bullet_spread(
    const void* const weapon_definition,
    float* const minimum_spread_degrees,
    float* const maximum_spread_degrees) noexcept {
    const bool hook_enabled =
        g_weapon_hook_enabled.load(std::memory_order_acquire);
    const WeaponExecutableLayout* const layout = g_weapon_layout;
    if (!hook_enabled || layout == nullptr || weapon_definition == nullptr ||
        !accessible_range(
            minimum_spread_degrees, sizeof(float), true) ||
        !accessible_range(
            maximum_spread_degrees, sizeof(float), true)) {
        return;
    }

    const auto weapon_definition_address =
        reinterpret_cast<std::uintptr_t>(weapon_definition);
    if (!accessible_range(
            reinterpret_cast<const void*>(
                weapon_definition_address +
                layout->weapon_definition_type_offset),
            sizeof(std::int32_t), false) ||
        !accessible_range(
            reinterpret_cast<const void*>(
                weapon_definition_address +
                layout->weapon_definition_ads_spread_offset),
            sizeof(float), false)) {
        return;
    }

    std::int32_t weapon_type = -1;
    float ads_spread_degrees = 0.0F;
    float native_minimum_spread_degrees = 0.0F;
    float native_maximum_spread_degrees = 0.0F;
    std::memcpy(
        &weapon_type,
        reinterpret_cast<const void*>(
            weapon_definition_address +
            layout->weapon_definition_type_offset),
        sizeof(weapon_type));
    std::memcpy(
        &ads_spread_degrees,
        reinterpret_cast<const void*>(
            weapon_definition_address +
            layout->weapon_definition_ads_spread_offset),
        sizeof(ads_spread_degrees));
    std::memcpy(
        &native_minimum_spread_degrees, minimum_spread_degrees,
        sizeof(native_minimum_spread_degrees));
    std::memcpy(
        &native_maximum_spread_degrees, maximum_spread_degrees,
        sizeof(native_maximum_spread_degrees));

    float replacement_minimum_spread_degrees =
        native_minimum_spread_degrees;
    float replacement_maximum_spread_degrees =
        native_maximum_spread_degrees;
    const FixedAdsVisualSpreadGate gate{
        hook_enabled,
        weapon_type,
        ads_spread_degrees,
    };
    if (!apply_fixed_ads_visual_spread_override(
            gate, &replacement_minimum_spread_degrees,
            &replacement_maximum_spread_degrees) ||
        !g_weapon_hook_enabled.load(std::memory_order_acquire)) {
        return;
    }

    std::memcpy(
        minimum_spread_degrees, &replacement_minimum_spread_degrees,
        sizeof(replacement_minimum_spread_degrees));
    std::memcpy(
        maximum_spread_degrees, &replacement_maximum_spread_degrees,
        sizeof(replacement_maximum_spread_degrees));
    WAWVR_STEREO_DIAG_ONCE(
        "WeaponDiag fixed local tracer/impact spread: native=%.3f..%.3f applied-ads=%.3f",
        native_minimum_spread_degrees, native_maximum_spread_degrees,
        replacement_maximum_spread_degrees);
}

extern "C" void __cdecl wawvr_apply_physical_muzzle_from_bridge(
    void* const weapon_parms,
    void* const firing_entity) noexcept {
    const bool hook_enabled =
        g_weapon_hook_enabled.load(std::memory_order_acquire);
    const WeaponExecutableLayout* const layout = g_weapon_layout;
    if (!hook_enabled || layout == nullptr ||
        g_local_player_entity_address == 0 ||
        !accessible_range(weapon_parms, layout->weapon_parms_size, true) ||
        !accessible_range(firing_entity, layout->gentity_size, false)) {
        return;
    }

    const auto weapon_bytes = reinterpret_cast<std::uint8_t*>(weapon_parms);
    const auto entity_bytes =
        reinterpret_cast<const std::uint8_t*>(firing_entity);
    std::int32_t entity_number = -1;
    std::uint32_t client_address = 0;
    std::uint32_t weapon_definition_address = 0;
    std::memcpy(
        &entity_number, entity_bytes + layout->gentity_number_offset,
        sizeof(entity_number));
    std::memcpy(
        &client_address, entity_bytes + layout->gentity_client_offset,
        sizeof(client_address));
    std::memcpy(
        &weapon_definition_address,
        weapon_bytes + layout->weapon_parms_weapon_definition_offset,
        sizeof(weapon_definition_address));
    if (weapon_definition_address == 0 ||
        !accessible_range(
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(weapon_definition_address) +
                layout->weapon_definition_type_offset),
            sizeof(std::int32_t), false)) {
        return;
    }

    std::int32_t weapon_type = -1;
    std::memcpy(
        &weapon_type,
        reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(weapon_definition_address) +
            layout->weapon_definition_type_offset),
        sizeof(weapon_type));

    ControllerFrameSnapshot controller{};
    const std::uint64_t now_milliseconds = GetTickCount64();
    RightControllerWeaponPose ignored_pose{};
    const bool controller_current = read_controller_frame(&controller) &&
        right_controller_weapon_pose(
            controller, now_milliseconds, &ignored_pose);
    wawvr::xr::Vec3f physical_muzzle{};
    const bool muzzle_fresh = controller_current &&
        read_published_weapon_muzzle(
            controller.generation, now_milliseconds, &physical_muzzle);
    wawvr::xr::Basis3f authoritative_axis{};
    const bool authoritative_axis_fresh =
        !layout->overwrite_authoritative_weapon_basis ||
        (controller_current && read_final_visible_weapon_basis(
            controller.generation, now_milliseconds,
            &authoritative_axis));
    const PhysicalMuzzleGate gate{
        hook_enabled,
        reinterpret_cast<std::uintptr_t>(firing_entity),
        g_local_player_entity_address,
        entity_number,
        client_address != 0,
        weapon_type,
        controller_current,
        muzzle_fresh,
    };
    if (!physical_muzzle_gate_allows(gate) ||
        !authoritative_axis_fresh ||
        !g_weapon_hook_enabled.load(std::memory_order_acquire)) {
        return;
    }

    wawvr::xr::Vec3f native_muzzle{};
    std::memcpy(
        &native_muzzle,
        weapon_bytes + layout->weapon_parms_muzzle_trace_offset,
        sizeof(native_muzzle));
    if (layout->overwrite_authoritative_weapon_basis) {
        // WeaponParms uses forward/right/up, while IW's visible model basis
        // uses forward/left/up. Multiplayer's server-side shot simulation
        // consumes these vectors instead of the SP Wii-angle fields.
        const wawvr::xr::Vec3f authoritative_right{
            -authoritative_axis.left.x,
            -authoritative_axis.left.y,
            -authoritative_axis.left.z,
        };
        std::memcpy(
            weapon_bytes + layout->weapon_parms_forward_offset,
            &authoritative_axis.forward, sizeof(authoritative_axis.forward));
        std::memcpy(
            weapon_bytes + layout->weapon_parms_right_offset,
            &authoritative_right, sizeof(authoritative_right));
        std::memcpy(
            weapon_bytes + layout->weapon_parms_up_offset,
            &authoritative_axis.up, sizeof(authoritative_axis.up));
    }
    std::memcpy(
        weapon_bytes + layout->weapon_parms_muzzle_trace_offset,
        &physical_muzzle, sizeof(physical_muzzle));
    WAWVR_STEREO_DIAG_ONCE(
        "WeaponDiag routed local bullet origin from native %.2f %.2f %.2f to corrected tag_flash %.2f %.2f %.2f",
        native_muzzle.x, native_muzzle.y, native_muzzle.z,
        physical_muzzle.x, physical_muzzle.y, physical_muzzle.z);
}

extern "C" void __cdecl wawvr_apply_fixed_ads_spread_from_bridge(
    void* const weapon_parms,
    void* const attacker,
    float* const spread_degrees) noexcept {
    const bool hook_enabled =
        g_weapon_hook_enabled.load(std::memory_order_acquire);
    const WeaponExecutableLayout* const layout = g_weapon_layout;
    if (!hook_enabled || layout == nullptr ||
        g_local_player_entity_address == 0 ||
        !accessible_range(weapon_parms, layout->weapon_parms_size, false) ||
        !accessible_range(attacker, layout->gentity_size, false) ||
        !accessible_range(spread_degrees, sizeof(float), true)) {
        return;
    }

    const auto weapon_bytes =
        reinterpret_cast<const std::uint8_t*>(weapon_parms);
    const auto entity_bytes =
        reinterpret_cast<const std::uint8_t*>(attacker);
    std::int32_t entity_number = -1;
    std::uint32_t client_address = 0;
    std::uint32_t weapon_definition_address = 0;
    std::memcpy(
        &entity_number, entity_bytes + layout->gentity_number_offset,
        sizeof(entity_number));
    std::memcpy(
        &client_address, entity_bytes + layout->gentity_client_offset,
        sizeof(client_address));
    std::memcpy(
        &weapon_definition_address,
        weapon_bytes + layout->weapon_parms_weapon_definition_offset,
        sizeof(weapon_definition_address));
    if (weapon_definition_address == 0) {
        return;
    }

    const auto weapon_definition =
        static_cast<std::uintptr_t>(weapon_definition_address);
    if (!accessible_range(
            reinterpret_cast<const void*>(
                weapon_definition + layout->weapon_definition_type_offset),
            sizeof(std::int32_t), false) ||
        !accessible_range(
            reinterpret_cast<const void*>(
                weapon_definition +
                layout->weapon_definition_ads_spread_offset),
            sizeof(float), false)) {
        return;
    }

    std::int32_t weapon_type = -1;
    float ads_spread_degrees = 0.0F;
    float native_spread_degrees = 0.0F;
    std::memcpy(
        &weapon_type,
        reinterpret_cast<const void*>(
            weapon_definition + layout->weapon_definition_type_offset),
        sizeof(weapon_type));
    std::memcpy(
        &ads_spread_degrees,
        reinterpret_cast<const void*>(
            weapon_definition +
            layout->weapon_definition_ads_spread_offset),
        sizeof(ads_spread_degrees));
    std::memcpy(
        &native_spread_degrees, spread_degrees,
        sizeof(native_spread_degrees));

    const FixedAdsSpreadGate gate{
        hook_enabled,
        reinterpret_cast<std::uintptr_t>(attacker),
        g_local_player_entity_address,
        entity_number,
        client_address != 0,
        weapon_type,
        ads_spread_degrees,
    };
    float replacement_spread_degrees = native_spread_degrees;
    if (!apply_fixed_ads_spread_override(
            gate, &replacement_spread_degrees) ||
        !g_weapon_hook_enabled.load(std::memory_order_acquire)) {
        return;
    }

    std::memcpy(
        spread_degrees, &replacement_spread_degrees,
        sizeof(replacement_spread_degrees));
    WAWVR_STEREO_DIAG_ONCE(
        "WeaponDiag fixed local VR firearm spread: native=%.3f applied-ads=%.3f (ADS state and locomotion unchanged)",
        native_spread_degrees, replacement_spread_degrees);
}

extern "C" void __cdecl wawvr_post_update_viewmodel_pose(
    void* const viewmodel_dobj) noexcept {
    const ActiveWeaponPoseContext context = g_active_weapon_pose;
    if (!g_weapon_hook_enabled.load(std::memory_order_acquire) ||
        viewmodel_dobj == nullptr) {
        return;
    }

    const ViewmodelFilterStatus filter_status =
        hide_t4_viewmodel_hand_surfaces(
            viewmodel_dobj, &accessible_range);
    if (filter_status == ViewmodelFilterStatus::applied ||
        filter_status == ViewmodelFilterStatus::already_hidden) {
        WAWVR_STEREO_DIAG_ONCE(
            "WeaponDiag gun-only viewmodel active; hand/arm surfaces hidden while model-zero skeleton remains animated");
    } else {
        WAWVR_STEREO_DIAG_ONCE(
            "WeaponDiag gun-only viewmodel rejected DObj layout: %s",
            viewmodel_filter_status_name(filter_status));
    }

    if (!context.valid || g_original_update_viewmodel_pose == 0 ||
        g_cg_dobj_get_world_tag_pos == 0 ||
        !accessible_range(
            reinterpret_cast<const void*>(g_viewmodel_axis_origin_address),
            sizeof(wawvr::xr::Vec3f), true) ||
        !accessible_range(
            reinterpret_cast<const void*>(g_viewmodel_pose_address),
            0x40, false)) {
        return;
    }

    wawvr::xr::Vec3f grip_tag_world{};
    const char* selected_tag_name = nullptr;
    for (const auto& candidate : g_grip_tags) {
        std::uint16_t tag = 0;
        if (!read_tag_word(candidate.address, &tag)) {
            continue;
        }
        if (wawvr_call_dobj_get_world_tag_pos(
                viewmodel_dobj, tag,
                reinterpret_cast<const void*>(g_viewmodel_pose_address),
                &grip_tag_world) != 0) {
            selected_tag_name = candidate.name;
            break;
        }
    }

    if (selected_tag_name == nullptr) {
        WAWVR_STEREO_DIAG_ONCE(
            "WeaponDiag no usable viewmodel grip tag; tried tag_weapon_right, tag_weapon, tag_inhand, tag_origin");
        return;
    }

    wawvr::xr::Vec3f original_origin{};
    std::memcpy(
        &original_origin,
        reinterpret_cast<const void*>(g_viewmodel_axis_origin_address),
        sizeof(original_origin));
    wawvr::xr::Vec3f corrected_origin = original_origin;
    if (!align_viewmodel_origin_to_grip(
            context.tracked_grip_world, grip_tag_world,
            &corrected_origin)) {
        WAWVR_STEREO_DIAG_ONCE(
            "WeaponDiag rejected non-finite or implausible grip-tag correction");
        return;
    }

    const float correction_x = corrected_origin.x - original_origin.x;
    const float correction_y = corrected_origin.y - original_origin.y;
    const float correction_z = corrected_origin.z - original_origin.z;
    const float correction_length = std::sqrt(
        correction_x * correction_x + correction_y * correction_y +
        correction_z * correction_z);
    std::memcpy(
        reinterpret_cast<void*>(g_viewmodel_axis_origin_address),
        &corrected_origin, sizeof(corrected_origin));
    wawvr_call_update_viewmodel_pose(viewmodel_dobj);

    std::uint16_t flash_tag = 0;
    wawvr::xr::Vec3f muzzle_world{};
    if (read_tag_word(g_tag_flash_address, &flash_tag) &&
        wawvr_call_dobj_get_world_tag_pos(
            viewmodel_dobj, flash_tag,
            reinterpret_cast<const void*>(g_viewmodel_pose_address),
            &muzzle_world) != 0) {
        publish_muzzle(context.controller_generation, muzzle_world);
        WAWVR_STEREO_DIAG_ONCE(
            "WeaponDiag published corrected tag_flash muzzle at %.2f %.2f %.2f",
            muzzle_world.x, muzzle_world.y, muzzle_world.z);
    }

    WAWVR_STEREO_DIAG_ONCE(
        "WeaponDiag aligned %s to tracked right grip (correction %.2f IW units)",
        selected_tag_name, correction_length);
}

extern "C" void __cdecl wawvr_add_player_weapon_bridge(
    const std::int32_t local_client_number,
    GfxScaledPlacement* const placement,
    const void* const player_state,
    void* const centity,
    const std::int32_t draw_gun) noexcept {
    const ActiveWeaponPoseContext previous_context = g_active_weapon_pose;
    g_active_weapon_pose = {};
    const bool hook_enabled =
        g_weapon_hook_enabled.load(std::memory_order_acquire);
    if (hook_enabled) {
        // A weapon switch, hidden model, or failed tag evaluation must not
        // leave the previous model's muzzle eligible for another shot.
        invalidate_published_muzzle();
    }
    if (hook_enabled &&
        accessible_range(placement, sizeof(*placement), true) &&
        accessible_range(
            reinterpret_cast<const void*>(g_camera_origin_address),
            sizeof(wawvr::xr::Vec3f), false) &&
        accessible_range(
            reinterpret_cast<const void*>(g_camera_axis_address),
            sizeof(wawvr::xr::Basis3f), false) &&
        accessible_range(
            reinterpret_cast<const void*>(g_cgame_gun_pitch_address),
            sizeof(float), true) &&
        accessible_range(
            reinterpret_cast<const void*>(g_cgame_gun_yaw_address),
            sizeof(float), true)) {
        ControllerFrameSnapshot snapshot{};
        if (read_controller_frame(&snapshot)) {
            if (!g_weapon_attachment_anchor_valid ||
                !same_pose(
                    snapshot.tracking_anchor,
                    g_weapon_attachment_anchor)) {
                g_weapon_attachment = {};
                g_weapon_attachment_anchor = snapshot.tracking_anchor;
                g_weapon_attachment_anchor_valid = true;
            }
            RightControllerWeaponPose controller{};
            wawvr::xr::Vec3f camera_origin{};
            wawvr::xr::Basis3f camera_axis{};
            std::memcpy(
                &camera_origin,
                reinterpret_cast<const void*>(g_camera_origin_address),
                sizeof(camera_origin));
            std::memcpy(
                &camera_axis,
                reinterpret_cast<const void*>(g_camera_axis_address),
                sizeof(camera_axis));

            wawvr::xr::Basis3f body_axis{};
            const bool body_axis_valid =
                gravity_level_t4_camera_axis(camera_axis, &body_axis);
            if (body_axis_valid) {
                camera_axis = body_axis;
            }

            // Start from the canonical camera axis rather than the stock
            // sway/bob orientation, making the final barrel an absolute
            // function of tracked controller aim.
            wawvr::xr::Vec3f weapon_origin = placement->origin;
            wawvr::xr::Basis3f weapon_axis = camera_axis;
            if (body_axis_valid && right_controller_weapon_pose(
                    snapshot, GetTickCount64(), &controller) &&
                apply_right_controller_weapon_placement(
                    camera_origin, camera_axis, controller,
                    &g_weapon_attachment, &weapon_origin, &weapon_axis)) {
                wawvr::xr::Quaternionf weapon_quaternion{};
                float pitch = 0.0F;
                float yaw = 0.0F;
                if (iw_axis_to_unit_quaternion(
                        weapon_axis, &weapon_quaternion) &&
                    aim_degrees_from_forward(
                        weapon_axis.forward, &pitch, &yaw)) {
                    placement->origin = weapon_origin;
                    placement->quaternion = weapon_quaternion;
                    std::memcpy(
                        reinterpret_cast<void*>(g_cgame_gun_pitch_address),
                        &pitch, sizeof(pitch));
                    std::memcpy(
                        reinterpret_cast<void*>(g_cgame_gun_yaw_address),
                        &yaw, sizeof(yaw));
                    publish_final_visible_aim(
                        snapshot.generation, pitch, yaw, weapon_axis);
                    wawvr::xr::Vec3f tracked_grip_world{};
                    if (player_state != nullptr && draw_gun != 0 &&
                        right_controller_grip_world(
                            camera_origin, camera_axis, controller,
                            &tracked_grip_world)) {
                        g_active_weapon_pose = {
                            true, snapshot.generation, tracked_grip_world};
                    }
                    WAWVR_STEREO_DIAG_ONCE(
                        "WeaponDiag applied rigid right-controller viewmodel placement at CG_AddPlayerWeapon");
                }
            }
        }
    }

    const std::uintptr_t original = g_original_add_player_weapon;
    const WeaponExecutableLayout* const layout = g_weapon_layout;
    if (original != 0 && layout != nullptr) {
        if (layout->add_player_weapon_uses_eax_centity) {
            wawvr_call_add_player_weapon_mp_original(
                local_client_number, placement, player_state, centity,
                draw_gun);
        } else {
            reinterpret_cast<CgAddPlayerWeaponFunction>(original)(
                local_client_number, placement, player_state, centity,
                draw_gun);
        }
    }
    g_active_weapon_pose = previous_context;
}

WeaponHookInstallResult install_weapon_viewmodel_hook(
    const wawvr::t4::ValidatedBindings& bindings) noexcept {
    WeaponHookInstallResult result{};
    if (g_weapon_hook_installed.load(std::memory_order_acquire)) {
        result.status = WeaponHookStatus::already_installed;
        return result;
    }
    if (weapon_hook_disabled_by_environment()) {
        result.status = WeaponHookStatus::disabled_by_environment;
        return result;
    }

#if !defined(_MSC_VER) || !defined(_M_IX86)
    static_cast<void>(bindings);
    result.status = WeaponHookStatus::rejected_wrong_profile;
    return result;
#else
    const auto& bound = bindings.profile();
    const WeaponExecutableLayout* const layout =
        weapon_layout_for_profile(bound);
    if (layout == nullptr) {
        result.status = WeaponHookStatus::rejected_wrong_profile;
        return result;
    }
    const std::uintptr_t add_player_weapon_bridge_address =
        layout->add_player_weapon_uses_eax_centity
        ? reinterpret_cast<std::uintptr_t>(
              &wawvr_add_player_weapon_mp_entry_bridge)
        : reinterpret_cast<std::uintptr_t>(
              &wawvr_add_player_weapon_bridge);

    const auto prepared_outer = wawvr::t4::prepare_inline_hook(
        bindings, wawvr::t4::HookSiteId::viewmodel_weapon_call,
        add_player_weapon_bridge_address);
    const auto prepared_pose = wawvr::t4::prepare_inline_hook(
        bindings, wawvr::t4::HookSiteId::viewmodel_pose_update_call,
        reinterpret_cast<std::uintptr_t>(
            &wawvr_update_viewmodel_pose_bridge));
    const auto prepared_ballistics = wawvr::t4::prepare_inline_hook(
        bindings, wawvr::t4::HookSiteId::fire_weapon_calc_muzzle_call,
        reinterpret_cast<std::uintptr_t>(
            &wawvr_calc_muzzle_points_bridge));
    const auto prepared_spread = wawvr::t4::prepare_inline_hook(
        bindings, wawvr::t4::HookSiteId::fire_weapon_bullet_fire_call,
        reinterpret_cast<std::uintptr_t>(&wawvr_bullet_fire_bridge));
    const auto prepared_client_effects = wawvr::t4::prepare_inline_hook(
        bindings,
        wawvr::t4::HookSiteId::draw_bullet_impacts_view_origin_call,
        reinterpret_cast<std::uintptr_t>(
            &wawvr_client_bullet_view_origin_bridge));
    const auto prepared_client_spread = wawvr::t4::prepare_inline_hook(
        bindings,
        wawvr::t4::HookSiteId::draw_bullet_impacts_get_spread_call,
        reinterpret_cast<std::uintptr_t>(
            &wawvr_client_bullet_spread_bridge));
    if (!prepared_outer.ok() || !prepared_pose.ok() ||
        !prepared_ballistics.ok() || !prepared_spread.ok() ||
        !prepared_client_effects.ok() || !prepared_client_spread.ok()) {
        result.status = WeaponHookStatus::preparation_failed;
        return result;
    }
    result.target = prepared_outer.hook->target;
    result.ballistics_target = prepared_ballistics.hook->target;
    result.spread_target = prepared_spread.hook->target;
    result.client_effects_target =
        prepared_client_effects.hook->target;
    result.client_spread_target =
        prepared_client_spread.hook->target;

    const auto original_add_player_weapon = bindings.module().address(
        layout->cg_add_player_weapon_rva,
        layout->cg_add_player_weapon_sentinel.size());
    const auto original_update_pose = bindings.module().address(
        layout->cg_update_viewmodel_pose_rva,
        layout->cg_update_viewmodel_pose_sentinel.size());
    const auto get_world_tag_pos = bindings.module().address(
        layout->cg_dobj_get_world_tag_pos_rva,
        layout->cg_dobj_get_world_tag_pos_sentinel.size());
    const auto calc_muzzle_points = bindings.site_address(
        wawvr::t4::HookSiteId::calc_muzzle_points_sentinel);
    const auto fire_weapon_context = bindings.site_address(
        wawvr::t4::HookSiteId::fire_weapon_calc_muzzle_context_sentinel);
    const auto bullet_fire = bindings.site_address(
        wawvr::t4::HookSiteId::bullet_fire_entry_sentinel);
    const auto fire_weapon_bullet_context = bindings.site_address(
        wawvr::t4::HookSiteId::fire_weapon_bullet_fire_context_sentinel);
    const auto bullet_spread_context = bindings.site_address(
        wawvr::t4::HookSiteId::bullet_fire_spread_argument_sentinel);
    const auto client_bullet_view_origin = bindings.site_address(
        wawvr::t4::HookSiteId::
            client_bullet_view_origin_entry_sentinel);
    const auto client_bullet_origin_context = bindings.site_address(
        wawvr::t4::HookSiteId::
            draw_bullet_impacts_view_origin_context_sentinel);
    const auto get_spread_for_weapon = bindings.site_address(
        wawvr::t4::HookSiteId::get_spread_for_weapon_entry_sentinel);
    const auto client_bullet_spread_context = bindings.site_address(
        wawvr::t4::HookSiteId::
            draw_bullet_impacts_get_spread_context_sentinel);
    const auto pose_context = bindings.module().address(
        layout->viewmodel_pose_context_rva,
        layout->viewmodel_pose_context_sentinel.size());
    const auto viewmodel_composition = bindings.module().address(
        layout->viewmodel_composition_rva,
        layout->viewmodel_composition_sentinel.size());
    const auto dobj_bone_layout = bindings.module().address(
        layout->dobj_bone_layout_rva, kDObjBoneLayoutSentinel.size());
    const auto renderer_model_layout = bindings.module().address(
        layout->renderer_model_layout_rva,
        kRendererModelLayoutSentinel.size());
    const auto renderer_hide_part_bits = bindings.module().address(
        layout->renderer_hide_part_bits_rva,
        kRendererHidePartBitsSentinel.size());
    const auto renderer_hidden_surface_cull = bindings.module().address(
        layout->renderer_hidden_surface_cull_rva,
        kRendererHiddenSurfaceCullSentinel.size());
    const auto camera_origin = bindings.module().address(
        layout->gameplay_refdef_origin_rva, sizeof(wawvr::xr::Vec3f));
    const auto camera_axis = bindings.module().address(
        layout->gameplay_refdef_axis_rva, sizeof(wawvr::xr::Basis3f));
    const auto viewmodel_axis_origin = bindings.module().address(
        layout->viewmodel_axis_origin_rva, sizeof(wawvr::xr::Vec3f));
    const auto viewmodel_pose = bindings.module().address(
        layout->viewmodel_pose_rva, layout->viewmodel_pose_extent);
    const auto tag_weapon_right = bindings.module().address(
        layout->tag_weapon_right_word_rva, sizeof(std::uint16_t));
    const auto tag_weapon = bindings.module().address(
        layout->tag_weapon_word_rva, sizeof(std::uint16_t));
    const auto tag_inhand = bindings.module().address(
        layout->tag_inhand_word_rva, sizeof(std::uint16_t));
    const auto tag_origin = bindings.module().address(
        layout->tag_origin_word_rva, sizeof(std::uint16_t));
    const auto tag_flash = bindings.module().address(
        layout->tag_flash_word_rva, sizeof(std::uint16_t));
    const auto gun_pitch = bindings.data_address(
        wawvr::t4::DataSymbolId::cgame_gun_pitch_degrees, sizeof(float));
    const auto gun_yaw = bindings.data_address(
        wawvr::t4::DataSymbolId::cgame_gun_yaw_degrees, sizeof(float));
    const auto local_player_entity = bindings.data_address(
        wawvr::t4::DataSymbolId::local_player_entity,
        layout->gentity_size);
    if (!original_add_player_weapon.has_value() ||
        !original_update_pose.has_value() ||
        !get_world_tag_pos.has_value() || !calc_muzzle_points.has_value() ||
        !fire_weapon_context.has_value() || !bullet_fire.has_value() ||
        !fire_weapon_bullet_context.has_value() ||
        !bullet_spread_context.has_value() ||
        !client_bullet_view_origin.has_value() ||
        !client_bullet_origin_context.has_value() ||
        !get_spread_for_weapon.has_value() ||
        !client_bullet_spread_context.has_value() ||
        !pose_context.has_value() ||
        !viewmodel_composition.has_value() ||
        !dobj_bone_layout.has_value() ||
        !renderer_model_layout.has_value() ||
        !renderer_hide_part_bits.has_value() ||
        !renderer_hidden_surface_cull.has_value() ||
        !camera_origin.has_value() || !camera_axis.has_value() ||
        !viewmodel_axis_origin.has_value() || !viewmodel_pose.has_value() ||
        !tag_weapon_right.has_value() || !tag_weapon.has_value() ||
        !tag_inhand.has_value() || !tag_origin.has_value() ||
        !tag_flash.has_value() || !gun_pitch.has_value() ||
        !gun_yaw.has_value() || !local_player_entity.has_value()) {
        result.status = WeaponHookStatus::address_out_of_range;
        return result;
    }
    result.original = *original_add_player_weapon;
    result.ballistics_original = *calc_muzzle_points;
    result.spread_original = *bullet_fire;
    result.client_effects_original = *client_bullet_view_origin;
    result.client_spread_original = *get_spread_for_weapon;

    if (prepared_outer.hook->expected_size != kCallInstructionSize ||
        prepared_pose.hook->expected_size != kCallInstructionSize ||
        prepared_ballistics.hook->expected_size != kCallInstructionSize ||
        prepared_spread.hook->expected_size != kCallInstructionSize ||
        prepared_client_effects.hook->expected_size !=
            kCallInstructionSize ||
        prepared_client_spread.hook->expected_size !=
            kCallInstructionSize) {
        result.status = WeaponHookStatus::original_target_mismatch;
        return result;
    }
    std::array<std::uint8_t, kCallInstructionSize> original_outer_call{};
    std::array<std::uint8_t, kCallInstructionSize> original_pose_call{};
    std::array<std::uint8_t, kCallInstructionSize>
        original_ballistics_call{};
    std::array<std::uint8_t, kCallInstructionSize> original_spread_call{};
    std::array<std::uint8_t, kCallInstructionSize>
        original_client_effects_call{};
    std::array<std::uint8_t, kCallInstructionSize>
        original_client_spread_call{};
    std::copy_n(
        prepared_outer.hook->expected.begin(), kCallInstructionSize,
        original_outer_call.begin());
    std::copy_n(
        prepared_pose.hook->expected.begin(), kCallInstructionSize,
        original_pose_call.begin());
    std::copy_n(
        prepared_ballistics.hook->expected.begin(), kCallInstructionSize,
        original_ballistics_call.begin());
    std::copy_n(
        prepared_spread.hook->expected.begin(), kCallInstructionSize,
        original_spread_call.begin());
    std::copy_n(
        prepared_client_effects.hook->expected.begin(),
        kCallInstructionSize,
        original_client_effects_call.begin());
    std::copy_n(
        prepared_client_spread.hook->expected.begin(),
        kCallInstructionSize,
        original_client_spread_call.begin());
    if (original_outer_call[0] != 0xE8 ||
        original_pose_call[0] != 0xE8 ||
        original_ballistics_call[0] != 0xE8 ||
        original_spread_call[0] != 0xE8 ||
        original_client_effects_call[0] != 0xE8 ||
        original_client_spread_call[0] != 0xE8 ||
        decode_relative_call_target(
            prepared_outer.hook->target, original_outer_call) !=
            *original_add_player_weapon ||
        decode_relative_call_target(
            prepared_pose.hook->target, original_pose_call) !=
            *original_update_pose ||
        decode_relative_call_target(
            prepared_ballistics.hook->target,
            original_ballistics_call) != *calc_muzzle_points ||
        decode_relative_call_target(
            prepared_spread.hook->target, original_spread_call) !=
            *bullet_fire ||
        decode_relative_call_target(
            prepared_client_effects.hook->target,
            original_client_effects_call) !=
            *client_bullet_view_origin ||
        decode_relative_call_target(
            prepared_client_spread.hook->target,
            original_client_spread_call) !=
            *get_spread_for_weapon) {
        result.status = WeaponHookStatus::original_target_mismatch;
        return result;
    }
    if (!bytes_match(
            reinterpret_cast<const void*>(*original_add_player_weapon),
            layout->cg_add_player_weapon_sentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*original_update_pose),
            layout->cg_update_viewmodel_pose_sentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*get_world_tag_pos),
            layout->cg_dobj_get_world_tag_pos_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::calc_muzzle_points_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::fire_weapon_calc_muzzle_context_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::bullet_fire_entry_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::fire_weapon_bullet_fire_context_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::bullet_fire_spread_argument_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                draw_bullet_impacts_view_origin_context_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                client_bullet_view_origin_entry_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                draw_bullet_impacts_get_spread_context_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                get_spread_for_weapon_entry_sentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*pose_context),
            layout->viewmodel_pose_context_sentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*viewmodel_composition),
            layout->viewmodel_composition_sentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*dobj_bone_layout),
            kDObjBoneLayoutSentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*renderer_model_layout),
            kRendererModelLayoutSentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*renderer_hide_part_bits),
            kRendererHidePartBitsSentinel) ||
        !bytes_match(
            reinterpret_cast<const void*>(*renderer_hidden_surface_cull),
            kRendererHiddenSurfaceCullSentinel)) {
        result.status = WeaponHookStatus::original_sentinel_mismatch;
        return result;
    }

    std::array<std::uint8_t, kCallInstructionSize> outer_replacement{};
    std::array<std::uint8_t, kCallInstructionSize> pose_replacement{};
    std::array<std::uint8_t, kCallInstructionSize> ballistics_replacement{};
    std::array<std::uint8_t, kCallInstructionSize> spread_replacement{};
    std::array<std::uint8_t, kCallInstructionSize>
        client_effects_replacement{};
    std::array<std::uint8_t, kCallInstructionSize>
        client_spread_replacement{};
    if (!make_relative_call(
            prepared_outer.hook->target,
            add_player_weapon_bridge_address,
            &outer_replacement) ||
        !make_relative_call(
            prepared_pose.hook->target,
            reinterpret_cast<std::uintptr_t>(
                &wawvr_update_viewmodel_pose_bridge),
            &pose_replacement) ||
        !make_relative_call(
            prepared_ballistics.hook->target,
            reinterpret_cast<std::uintptr_t>(
                &wawvr_calc_muzzle_points_bridge),
            &ballistics_replacement) ||
        !make_relative_call(
            prepared_spread.hook->target,
            reinterpret_cast<std::uintptr_t>(&wawvr_bullet_fire_bridge),
            &spread_replacement) ||
        !make_relative_call(
            prepared_client_effects.hook->target,
            reinterpret_cast<std::uintptr_t>(
                &wawvr_client_bullet_view_origin_bridge),
            &client_effects_replacement) ||
        !make_relative_call(
            prepared_client_spread.hook->target,
            reinterpret_cast<std::uintptr_t>(
                &wawvr_client_bullet_spread_bridge),
            &client_spread_replacement)) {
        result.status = WeaponHookStatus::jump_out_of_range;
        return result;
    }

    const std::array<PeerThreadPatchRange, 6> patch_ranges{{
        {prepared_outer.hook->target, kCallInstructionSize},
        {prepared_pose.hook->target, kCallInstructionSize},
        {prepared_ballistics.hook->target, kCallInstructionSize},
        {prepared_spread.hook->target, kCallInstructionSize},
        {prepared_client_effects.hook->target, kCallInstructionSize},
        {prepared_client_spread.hook->target, kCallInstructionSize},
    }};
    SuspendedPeerThreads suspended;
    PeerThreadQuiesceResult quiesce{};
    if (!suspended.suspend(patch_ranges, &quiesce)) {
        result.status = WeaponHookStatus::thread_suspend_failed;
        result.system_error = quiesce.system_error;
        return result;
    }

    auto* const outer_target = reinterpret_cast<std::uint8_t*>(
        prepared_outer.hook->target);
    auto* const pose_target = reinterpret_cast<std::uint8_t*>(
        prepared_pose.hook->target);
    auto* const ballistics_target = reinterpret_cast<std::uint8_t*>(
        prepared_ballistics.hook->target);
    auto* const spread_target = reinterpret_cast<std::uint8_t*>(
        prepared_spread.hook->target);
    auto* const client_effects_target =
        reinterpret_cast<std::uint8_t*>(
            prepared_client_effects.hook->target);
    auto* const client_spread_target =
        reinterpret_cast<std::uint8_t*>(
            prepared_client_spread.hook->target);
    if (!bytes_match(outer_target, original_outer_call) ||
        !bytes_match(pose_target, original_pose_call) ||
        !bytes_match(ballistics_target, original_ballistics_call) ||
        !bytes_match(spread_target, original_spread_call) ||
        !bytes_match(
            client_effects_target, original_client_effects_call) ||
        !bytes_match(
            client_spread_target, original_client_spread_call)) {
        result.status = WeaponHookStatus::expected_bytes_changed;
        return result;
    }

    const std::uintptr_t viewmodel_protection_begin = std::min({
        prepared_outer.hook->target,
        prepared_pose.hook->target,
        prepared_client_effects.hook->target,
        prepared_client_spread.hook->target,
    });
    const std::uintptr_t viewmodel_protection_end = std::max({
        prepared_outer.hook->target + kCallInstructionSize,
        prepared_pose.hook->target + kCallInstructionSize,
        prepared_client_effects.hook->target + kCallInstructionSize,
        prepared_client_spread.hook->target + kCallInstructionSize,
    });
    auto* const viewmodel_protection_target =
        reinterpret_cast<void*>(viewmodel_protection_begin);
    const std::size_t viewmodel_protection_size =
        viewmodel_protection_end - viewmodel_protection_begin;
    const std::uintptr_t ballistics_protection_begin = std::min(
        prepared_ballistics.hook->target, prepared_spread.hook->target);
    const std::uintptr_t ballistics_protection_end = std::max(
        prepared_ballistics.hook->target + kCallInstructionSize,
        prepared_spread.hook->target + kCallInstructionSize);
    auto* const ballistics_protection_target =
        reinterpret_cast<void*>(ballistics_protection_begin);
    const std::size_t ballistics_protection_size =
        ballistics_protection_end - ballistics_protection_begin;
    DWORD old_viewmodel_protection = 0;
    if (!VirtualProtect(
            viewmodel_protection_target, viewmodel_protection_size,
            PAGE_EXECUTE_READWRITE, &old_viewmodel_protection)) {
        result.status = WeaponHookStatus::target_protection_failed;
        result.system_error = GetLastError();
        return result;
    }
    DWORD old_ballistics_protection = 0;
    if (!VirtualProtect(
            ballistics_protection_target, ballistics_protection_size,
            PAGE_EXECUTE_READWRITE, &old_ballistics_protection)) {
        const DWORD protection_error = GetLastError();
        DWORD ignored = 0;
        if (!VirtualProtect(
                viewmodel_protection_target, viewmodel_protection_size,
                old_viewmodel_protection, &ignored)) {
            result.status = WeaponHookStatus::protection_restore_failed;
            result.system_error = GetLastError();
            return result;
        }
        result.status = WeaponHookStatus::target_protection_failed;
        result.system_error = protection_error;
        return result;
    }

    const auto restore_original_viewmodel_calls = [&]() noexcept {
        std::memcpy(
            outer_target, original_outer_call.data(),
            original_outer_call.size());
        std::memcpy(
            pose_target, original_pose_call.data(),
            original_pose_call.size());
        std::memcpy(
            client_effects_target,
            original_client_effects_call.data(),
            original_client_effects_call.size());
        std::memcpy(
            client_spread_target,
            original_client_spread_call.data(),
            original_client_spread_call.size());
        FlushInstructionCache(
            GetCurrentProcess(), viewmodel_protection_target,
            viewmodel_protection_size);
    };
    const auto restore_original_ballistics_call = [&]() noexcept {
        std::memcpy(
            ballistics_target, original_ballistics_call.data(),
            original_ballistics_call.size());
        std::memcpy(
            spread_target, original_spread_call.data(),
            original_spread_call.size());
        FlushInstructionCache(
            GetCurrentProcess(), ballistics_protection_target,
            ballistics_protection_size);
    };
    const auto restore_protections = [&]() noexcept -> DWORD {
        DWORD first_error = 0;
        DWORD ignored = 0;
        if (!VirtualProtect(
                ballistics_protection_target, ballistics_protection_size,
                old_ballistics_protection, &ignored)) {
            first_error = GetLastError();
        }
        if (!VirtualProtect(
                viewmodel_protection_target, viewmodel_protection_size,
                old_viewmodel_protection, &ignored) &&
            first_error == 0) {
            first_error = GetLastError();
        }
        return first_error;
    };

    if (std::memcmp(
            outer_target, original_outer_call.data(),
            original_outer_call.size()) != 0 ||
        std::memcmp(
            pose_target, original_pose_call.data(),
            original_pose_call.size()) != 0 ||
        std::memcmp(
            ballistics_target, original_ballistics_call.data(),
            original_ballistics_call.size()) != 0 ||
        std::memcmp(
            spread_target, original_spread_call.data(),
            original_spread_call.size()) != 0 ||
        std::memcmp(
            client_effects_target,
            original_client_effects_call.data(),
            original_client_effects_call.size()) != 0 ||
        std::memcmp(
            client_spread_target,
            original_client_spread_call.data(),
            original_client_spread_call.size()) != 0) {
        const DWORD restore_error = restore_protections();
        result.status = restore_error == 0
            ? WeaponHookStatus::expected_bytes_changed
            : WeaponHookStatus::protection_restore_failed;
        result.system_error = restore_error;
        return result;
    }

    // Initialize every bridge dependency before any replacement call becomes
    // visible. Peer threads remain suspended until this function returns.
    g_weapon_layout = layout;
    g_original_add_player_weapon = *original_add_player_weapon;
    g_original_update_viewmodel_pose = *original_update_pose;
    g_original_calc_muzzle_points = *calc_muzzle_points;
    g_original_bullet_fire = *bullet_fire;
    g_original_client_bullet_view_origin =
        *client_bullet_view_origin;
    g_original_get_spread_for_weapon = *get_spread_for_weapon;
    g_cg_dobj_get_world_tag_pos = *get_world_tag_pos;
    g_camera_origin_address = *camera_origin;
    g_camera_axis_address = *camera_axis;
    g_cgame_gun_pitch_address = *gun_pitch;
    g_cgame_gun_yaw_address = *gun_yaw;
    g_viewmodel_axis_origin_address = *viewmodel_axis_origin;
    g_viewmodel_pose_address = *viewmodel_pose;
    g_grip_tags = {{
        {*tag_weapon_right, "tag_weapon_right"},
        {*tag_weapon, "tag_weapon"},
        {*tag_inhand, "tag_inhand"},
        {*tag_origin, "tag_origin"},
    }};
    g_tag_flash_address = *tag_flash;
    g_local_player_entity_address = *local_player_entity;

    std::memcpy(
        ballistics_target, ballistics_replacement.data(),
        ballistics_replacement.size());
    std::memcpy(
        spread_target, spread_replacement.data(),
        spread_replacement.size());
    std::memcpy(
        client_effects_target,
        client_effects_replacement.data(),
        client_effects_replacement.size());
    std::memcpy(
        client_spread_target,
        client_spread_replacement.data(),
        client_spread_replacement.size());
    std::memcpy(pose_target, pose_replacement.data(), pose_replacement.size());
    std::memcpy(
        outer_target, outer_replacement.data(), outer_replacement.size());
    if (std::memcmp(
            outer_target, outer_replacement.data(),
            outer_replacement.size()) != 0 ||
        std::memcmp(
            pose_target, pose_replacement.data(),
            pose_replacement.size()) != 0 ||
        std::memcmp(
            ballistics_target, ballistics_replacement.data(),
            ballistics_replacement.size()) != 0 ||
        std::memcmp(
            spread_target, spread_replacement.data(),
            spread_replacement.size()) != 0 ||
        std::memcmp(
            client_effects_target,
            client_effects_replacement.data(),
            client_effects_replacement.size()) != 0 ||
        std::memcmp(
            client_spread_target,
            client_spread_replacement.data(),
            client_spread_replacement.size()) != 0) {
        restore_original_viewmodel_calls();
        restore_original_ballistics_call();
        const DWORD restore_error = restore_protections();
        result.status = restore_error == 0
            ? WeaponHookStatus::patch_write_failed
            : WeaponHookStatus::protection_restore_failed;
        result.system_error = restore_error;
        return result;
    }
    if (!FlushInstructionCache(
            GetCurrentProcess(), viewmodel_protection_target,
            viewmodel_protection_size) ||
        !FlushInstructionCache(
            GetCurrentProcess(), ballistics_protection_target,
            ballistics_protection_size)) {
        result.system_error = GetLastError();
        restore_original_viewmodel_calls();
        restore_original_ballistics_call();
        const DWORD restore_error = restore_protections();
        if (restore_error != 0) {
            result.system_error = restore_error;
            result.status = WeaponHookStatus::protection_restore_failed;
        } else {
            result.status = WeaponHookStatus::patch_cache_flush_failed;
        }
        return result;
    }
    const DWORD restore_error = restore_protections();
    if (restore_error != 0) {
        result.system_error = restore_error;

        // One range may already be RX. Re-open each range independently,
        // restore only the ranges made writable, then restore their exact
        // original protections again. Disabled bridges remain safe even if a
        // hostile third-party hook prevents a complete rollback.
        DWORD ignored = 0;
        const bool viewmodel_writable = VirtualProtect(
            viewmodel_protection_target, viewmodel_protection_size,
            PAGE_EXECUTE_READWRITE, &ignored) != FALSE;
        const bool ballistics_writable = VirtualProtect(
            ballistics_protection_target, ballistics_protection_size,
            PAGE_EXECUTE_READWRITE, &ignored) != FALSE;
        if (viewmodel_writable) {
            restore_original_viewmodel_calls();
            VirtualProtect(
                viewmodel_protection_target, viewmodel_protection_size,
                old_viewmodel_protection, &ignored);
        }
        if (ballistics_writable) {
            restore_original_ballistics_call();
            VirtualProtect(
                ballistics_protection_target, ballistics_protection_size,
                old_ballistics_protection, &ignored);
        }
        result.status = WeaponHookStatus::protection_restore_failed;
        return result;
    }
    g_weapon_hook_enabled.store(true, std::memory_order_release);
    g_weapon_hook_installed.store(true, std::memory_order_release);
    result.status = WeaponHookStatus::installed;
    return result;
#endif
}

CampaignTargetingHookInstallResult
install_campaign_rocket_targeting_hook(
    const wawvr::t4::ValidatedBindings& bindings) noexcept {
    CampaignTargetingHookInstallResult result{};
    if (g_campaign_targeting_hook_installed.load(
            std::memory_order_acquire)) {
        result.status = CampaignTargetingHookStatus::already_installed;
        return result;
    }

#if !defined(_MSC_VER) || !defined(_M_IX86)
    static_cast<void>(bindings);
    result.status = CampaignTargetingHookStatus::rejected_wrong_profile;
    return result;
#else
    const auto& bound = bindings.profile();
    const auto layout = select_t4_layout_family(bound);
    if (layout == T4LayoutFamily::multiplayer_1_7_1263) {
        result.status = CampaignTargetingHookStatus::not_applicable;
        return result;
    }
    if (layout != T4LayoutFamily::single_player_1_7_1263) {
        result.status = CampaignTargetingHookStatus::rejected_wrong_profile;
        return result;
    }
    if (!g_weapon_hook_installed.load(std::memory_order_acquire) ||
        !g_weapon_hook_enabled.load(std::memory_order_acquire) ||
        g_weapon_layout != &kSpWeaponLayout) {
        result.status =
            CampaignTargetingHookStatus::dependency_unavailable;
        return result;
    }

    const auto prepared = wawvr::t4::prepare_inline_hook(
        bindings,
        wawvr::t4::HookSiteId::get_player_angles_scr_add_vector_call,
        reinterpret_cast<std::uintptr_t>(
            &wawvr_get_player_angles_scr_add_vector_bridge));
    if (!prepared.ok()) {
        result.status = CampaignTargetingHookStatus::preparation_failed;
        return result;
    }
    result.target = prepared.hook->target;

    const auto scr_add_vector = bindings.site_address(
        wawvr::t4::HookSiteId::scr_add_vector_entry_sentinel);
    const auto local_player_entity = bindings.data_address(
        wawvr::t4::DataSymbolId::local_player_entity,
        kSpWeaponLayout.gentity_size);
    const auto weapon_definition_pointer_table = bindings.data_address(
        wawvr::t4::DataSymbolId::weapon_definition_pointer_table,
        kWeaponDefinitionPointerTableExtent);
    const auto weapon_definition_count = bindings.data_address(
        wawvr::t4::DataSymbolId::weapon_definition_count,
        sizeof(std::uint32_t));
    const auto camera_axis = bindings.module().address(
        kSpWeaponLayout.gameplay_refdef_axis_rva,
        sizeof(wawvr::xr::Basis3f));
    if (!scr_add_vector.has_value() ||
        !local_player_entity.has_value() ||
        !weapon_definition_pointer_table.has_value() ||
        !weapon_definition_count.has_value() || !camera_axis.has_value()) {
        result.status = CampaignTargetingHookStatus::address_out_of_range;
        return result;
    }

    if (!bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                get_player_angles_registration_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::get_player_angles_entry_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                get_player_angles_return_context_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::scr_add_vector_entry_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                get_current_weapon_identity_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                get_current_weapon_fallback_identity_sentinel) ||
        !bindings.site_bytes_still_match(
            wawvr::t4::HookSiteId::
                weapon_definition_registration_sentinel)) {
        result.status =
            CampaignTargetingHookStatus::original_sentinel_mismatch;
        return result;
    }
    if (prepared.hook->expected_size != kCallInstructionSize) {
        result.status =
            CampaignTargetingHookStatus::original_target_mismatch;
        return result;
    }

    std::array<std::uint8_t, kCallInstructionSize> original_call{};
    std::copy_n(
        prepared.hook->expected.begin(), kCallInstructionSize,
        original_call.begin());
    if (original_call[0] != 0xE8 ||
        decode_relative_call_target(
            prepared.hook->target, original_call) != *scr_add_vector) {
        result.status =
            CampaignTargetingHookStatus::original_target_mismatch;
        return result;
    }
    result.original = *scr_add_vector;

    std::array<std::uint8_t, kCallInstructionSize> replacement{};
    if (!make_relative_call(
            prepared.hook->target,
            reinterpret_cast<std::uintptr_t>(
                &wawvr_get_player_angles_scr_add_vector_bridge),
            &replacement)) {
        result.status = CampaignTargetingHookStatus::jump_out_of_range;
        return result;
    }

    const std::array<PeerThreadPatchRange, 1> patch_ranges{{
        {prepared.hook->target, kCallInstructionSize},
    }};
    SuspendedPeerThreads suspended;
    PeerThreadQuiesceResult quiesce{};
    if (!suspended.suspend(patch_ranges, &quiesce)) {
        result.status = CampaignTargetingHookStatus::thread_suspend_failed;
        result.system_error = quiesce.system_error;
        return result;
    }

    auto* const target =
        reinterpret_cast<std::uint8_t*>(prepared.hook->target);
    if (!bytes_match(target, original_call)) {
        result.status =
            CampaignTargetingHookStatus::expected_bytes_changed;
        return result;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(
            target, kCallInstructionSize, PAGE_EXECUTE_READWRITE,
            &old_protection)) {
        result.status =
            CampaignTargetingHookStatus::target_protection_failed;
        result.system_error = GetLastError();
        return result;
    }
    const auto restore_protection = [&]() noexcept -> DWORD {
        DWORD ignored = 0;
        if (!VirtualProtect(
                target, kCallInstructionSize, old_protection, &ignored)) {
            return GetLastError();
        }
        return 0;
    };
    const auto restore_original_call = [&]() noexcept {
        std::memcpy(target, original_call.data(), original_call.size());
        FlushInstructionCache(
            GetCurrentProcess(), target, kCallInstructionSize);
    };

    if (std::memcmp(
            target, original_call.data(), original_call.size()) != 0) {
        const DWORD restore_error = restore_protection();
        result.status = restore_error == 0
            ? CampaignTargetingHookStatus::expected_bytes_changed
            : CampaignTargetingHookStatus::protection_restore_failed;
        result.system_error = restore_error;
        return result;
    }

    // Publish every bridge dependency before the replacement call becomes
    // visible. The bridge remains disabled until bytes and RX protection are
    // both verified, and all peer threads remain suspended in this scope.
    g_original_scr_add_vector = *scr_add_vector;
    g_weapon_definition_pointer_table_address =
        *weapon_definition_pointer_table;
    g_weapon_definition_count_address = *weapon_definition_count;
    g_local_player_entity_address = *local_player_entity;
    g_camera_axis_address = *camera_axis;

    std::memcpy(target, replacement.data(), replacement.size());
    if (std::memcmp(
            target, replacement.data(), replacement.size()) != 0) {
        restore_original_call();
        const DWORD restore_error = restore_protection();
        result.status = restore_error == 0
            ? CampaignTargetingHookStatus::patch_write_failed
            : CampaignTargetingHookStatus::protection_restore_failed;
        result.system_error = restore_error;
        return result;
    }
    if (!FlushInstructionCache(
            GetCurrentProcess(), target, kCallInstructionSize)) {
        result.system_error = GetLastError();
        restore_original_call();
        const DWORD restore_error = restore_protection();
        if (restore_error != 0) {
            result.status =
                CampaignTargetingHookStatus::protection_restore_failed;
            result.system_error = restore_error;
        } else {
            result.status =
                CampaignTargetingHookStatus::patch_cache_flush_failed;
        }
        return result;
    }
    const DWORD restore_error = restore_protection();
    if (restore_error != 0) {
        result.system_error = restore_error;
        g_campaign_targeting_hook_enabled.store(
            false, std::memory_order_release);

        // Re-open only this proven five-byte call and roll it back. The
        // original target stays published so a hostile concurrent patch can
        // still fail through the disabled pass-through bridge.
        DWORD ignored = 0;
        if (VirtualProtect(
                target, kCallInstructionSize, PAGE_EXECUTE_READWRITE,
                &ignored)) {
            restore_original_call();
            VirtualProtect(
                target, kCallInstructionSize, old_protection, &ignored);
        }
        result.status =
            CampaignTargetingHookStatus::protection_restore_failed;
        return result;
    }

    g_campaign_targeting_hook_enabled.store(
        true, std::memory_order_release);
    g_campaign_targeting_hook_installed.store(
        true, std::memory_order_release);
    result.status = CampaignTargetingHookStatus::installed;
    return result;
#endif
}

const char* weapon_hook_status_name(const WeaponHookStatus status) noexcept {
    switch (status) {
    case WeaponHookStatus::installed: return "installed";
    case WeaponHookStatus::already_installed: return "already-installed";
    case WeaponHookStatus::disabled_by_environment: return "disabled-by-environment";
    case WeaponHookStatus::rejected_wrong_profile: return "rejected-wrong-profile";
    case WeaponHookStatus::input_dependency_unavailable: return "input-dependency-unavailable";
    case WeaponHookStatus::preparation_failed: return "preparation-failed";
    case WeaponHookStatus::address_out_of_range: return "address-out-of-range";
    case WeaponHookStatus::original_target_mismatch: return "original-target-mismatch";
    case WeaponHookStatus::original_sentinel_mismatch: return "original-sentinel-mismatch";
    case WeaponHookStatus::jump_out_of_range: return "jump-out-of-range";
    case WeaponHookStatus::thread_suspend_failed: return "thread-suspend-failed";
    case WeaponHookStatus::target_protection_failed: return "target-protection-failed";
    case WeaponHookStatus::expected_bytes_changed: return "expected-bytes-changed";
    case WeaponHookStatus::patch_write_failed: return "patch-write-failed";
    case WeaponHookStatus::patch_cache_flush_failed: return "patch-cache-flush-failed";
    case WeaponHookStatus::protection_restore_failed: return "protection-restore-failed";
    }
    return "unknown";
}

const char* campaign_targeting_hook_status_name(
    const CampaignTargetingHookStatus status) noexcept {
    switch (status) {
    case CampaignTargetingHookStatus::installed: return "installed";
    case CampaignTargetingHookStatus::already_installed: return "already-installed";
    case CampaignTargetingHookStatus::not_applicable: return "not-applicable";
    case CampaignTargetingHookStatus::dependency_unavailable: return "dependency-unavailable";
    case CampaignTargetingHookStatus::rejected_wrong_profile: return "rejected-wrong-profile";
    case CampaignTargetingHookStatus::preparation_failed: return "preparation-failed";
    case CampaignTargetingHookStatus::address_out_of_range: return "address-out-of-range";
    case CampaignTargetingHookStatus::original_target_mismatch: return "original-target-mismatch";
    case CampaignTargetingHookStatus::original_sentinel_mismatch: return "original-sentinel-mismatch";
    case CampaignTargetingHookStatus::jump_out_of_range: return "jump-out-of-range";
    case CampaignTargetingHookStatus::thread_suspend_failed: return "thread-suspend-failed";
    case CampaignTargetingHookStatus::target_protection_failed: return "target-protection-failed";
    case CampaignTargetingHookStatus::expected_bytes_changed: return "expected-bytes-changed";
    case CampaignTargetingHookStatus::patch_write_failed: return "patch-write-failed";
    case CampaignTargetingHookStatus::patch_cache_flush_failed: return "patch-cache-flush-failed";
    case CampaignTargetingHookStatus::protection_restore_failed: return "protection-restore-failed";
    }
    return "unknown";
}

bool read_final_visible_weapon_aim(
    const std::uint64_t controller_generation,
    const std::uint64_t now_milliseconds,
    float* const pitch_degrees,
    float* const yaw_degrees) noexcept {
    FinalVisibleAim snapshot{};
    if (pitch_degrees == nullptr || yaw_degrees == nullptr ||
        !read_fresh_final_visible_aim(
            controller_generation, now_milliseconds, &snapshot)) {
        return false;
    }
    *pitch_degrees = snapshot.pitch_degrees;
    *yaw_degrees = snapshot.yaw_degrees;
    return true;
}

bool read_published_weapon_muzzle(
    const std::uint64_t controller_generation,
    const std::uint64_t now_milliseconds,
    wawvr::xr::Vec3f* const origin) noexcept {
    if (origin == nullptr) {
        return false;
    }
    AcquireSRWLockShared(&g_published_muzzle_lock);
    const PublishedWeaponMuzzleSnapshot snapshot = g_published_muzzle;
    ReleaseSRWLockShared(&g_published_muzzle_lock);
    if (!published_weapon_muzzle_is_fresh(
            snapshot, controller_generation, now_milliseconds)) {
        return false;
    }
    *origin = snapshot.origin;
    return true;
}

void request_weapon_hook_shutdown() noexcept {
    // The script bridge depends on the tracked-weapon globals. Disable it
    // first; neither callsite is unpatched during process detach.
    g_campaign_targeting_hook_enabled.store(
        false, std::memory_order_release);
    g_weapon_hook_enabled.store(false, std::memory_order_release);
}

bool weapon_viewmodel_hook_installed() noexcept {
    return g_weapon_hook_installed.load(std::memory_order_acquire);
}

bool weapon_viewmodel_hook_enabled() noexcept {
    return g_weapon_hook_enabled.load(std::memory_order_acquire);
}

}  // namespace wawvr::mod
