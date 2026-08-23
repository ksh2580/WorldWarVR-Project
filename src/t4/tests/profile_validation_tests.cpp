#include "t4/address.hpp"
#include "t4/pe_image.hpp"
#include "t4/profile.hpp"
#include "t4/runtime_win32.hpp"
#include "t4/sha256.hpp"
#include "t4/usercmd.hpp"
#include "t4/validation.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace wawvr::t4;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void check_same_layout_descriptors(const ExecutableProfile& left,
                                   const ExecutableProfile& right,
                                   const std::string_view message) {
    check(left.layout == right.layout && left.sites.size() == right.sites.size() &&
              left.data_symbols.size() == right.data_symbols.size(),
          message);
    for (std::size_t index = 0; index < left.sites.size(); ++index) {
        const auto& a = left.sites[index];
        const auto& b = right.sites[index];
        check(a.id == b.id && a.role == b.role && a.use == b.use &&
                  a.rva == b.rva &&
                  a.minimum_patch_bytes == b.minimum_patch_bytes &&
                  std::ranges::equal(a.expected_bytes(), b.expected_bytes()),
              message);
    }
    for (std::size_t index = 0; index < left.data_symbols.size(); ++index) {
        const auto& a = left.data_symbols[index];
        const auto& b = right.data_symbols[index];
        check(a.id == b.id && a.rva == b.rva && a.size == b.size, message);
    }
}

void put_u16(std::vector<std::uint8_t>& bytes, const std::size_t offset,
             const std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u32(std::vector<std::uint8_t>& bytes, const std::size_t offset,
             const std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24U);
}

std::vector<std::uint8_t> synthetic_pe() {
    std::vector<std::uint8_t> bytes(0x400, 0);
    put_u16(bytes, 0x00, 0x5A4D);
    put_u32(bytes, 0x3C, 0x80);
    put_u32(bytes, 0x80, 0x00004550);

    constexpr std::size_t coff = 0x84;
    put_u16(bytes, coff + 0, 0x014C);
    put_u16(bytes, coff + 2, 1);
    put_u32(bytes, coff + 4, 0x12345678);
    put_u16(bytes, coff + 16, 0xE0);
    put_u16(bytes, coff + 18, 0x0103);

    constexpr std::size_t optional = coff + 20;
    put_u16(bytes, optional + 0, 0x010B);
    put_u32(bytes, optional + 16, 0x1000);
    put_u32(bytes, optional + 28, 0x00400000);
    put_u32(bytes, optional + 32, 0x1000);
    put_u32(bytes, optional + 36, 0x200);
    put_u32(bytes, optional + 56, 0x2000);
    put_u32(bytes, optional + 60, 0x200);
    put_u16(bytes, optional + 70, 0);

    constexpr std::size_t section = optional + 0xE0;
    constexpr std::string_view name = ".text";
    std::copy(name.begin(), name.end(), bytes.begin() + section);
    put_u32(bytes, section + 8, 0x200);
    put_u32(bytes, section + 12, 0x1000);
    put_u32(bytes, section + 16, 0x200);
    put_u32(bytes, section + 20, 0x200);
    put_u32(bytes, section + 36, 0x60000020);
    return bytes;
}

void test_sha256_vectors() {
    const std::span<const std::uint8_t> empty{};
    check(sha256_hex(sha256(empty)) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty SHA-256 vector");

    constexpr std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
    check(sha256_hex(sha256(abc)) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc SHA-256 vector");
}

void test_exact_profile_constants() {
    const auto& profile = t4_sp_1_7_1263_profile();
    check(profile.variant == ExecutableVariant::single_player,
          "SP executable variant");
    check(profile.layout == ExecutableLayoutId::t4_sp_1_7_1263 &&
              profile.on_disk_code == OnDiskCodeState::plain,
          "SP executable layout and plain on-disk state");
    check(sha256_hex(profile.sha256) ==
              "f26d45524bfff7e44c8ebab4d758ca524edfb0fb7d52352b6c95e1e908799361",
          "local T4 SHA-256 profile constant");
    check(profile.file_size == 5'550'080, "local T4 file size profile constant");
    check(profile.entry_point_rva == Rva{0x003AF316},
          "local T4 entry point profile constant");
    check(profile.sites.size() == 50,
          "SP profile contains every verified and only verified site");
    check(profile.data_symbols.size() == 12,
          "SP profile contains every verified data symbol");

    const auto* render = find_hook_site(profile, HookSiteId::render_backend_begin);
    check(render != nullptr, "render hook exists in exact profile");
    constexpr std::array<std::uint8_t, 8> expected{
        0x83, 0xE8, 0x00, 0x55, 0x8B, 0x6C, 0x24, 0x0C,
    };
    check(std::ranges::equal(render->expected_bytes(), expected),
          "render hook expected bytes profile constant");

    const auto* lod_load =
        find_hook_site(profile, HookSiteId::lod_tan_half_fov_y_load);
    constexpr std::array<std::uint8_t, 5> expected_lod_load{
        0xF3, 0x0F, 0x10, 0x48, 0x14,
    };
    check(lod_load != nullptr && lod_load->rva == Rva{0x002DEA1B} &&
              lod_load->role == HookRole::render &&
              lod_load->use == SiteUse::detour_candidate &&
              lod_load->minimum_patch_bytes == expected_lod_load.size() &&
              std::ranges::equal(
                  lod_load->expected_bytes(), expected_lod_load),
          "SP LOD-only tanHalfFovY movss site and exact context");

    const auto* frame_call =
        find_hook_site(profile, HookSiteId::main_loop_com_frame_call);
    check(frame_call != nullptr, "main-loop Com_Frame call exists");
    check(frame_call->rva == Rva{0x001FF7BD},
          "main-loop Com_Frame call RVA");
    check(frame_call->role == HookRole::frame_boundary &&
              frame_call->use == SiteUse::detour_candidate,
          "main-loop Com_Frame call is the approved frame-boundary patch");
    constexpr std::array<std::uint8_t, 5> expected_frame_call{
        0xE8, 0x6E, 0xEB, 0xF9, 0xFF,
    };
    check(std::ranges::equal(
              frame_call->expected_bytes(), expected_frame_call),
          "main-loop Com_Frame call bytes");

    const auto* frame_context = find_hook_site(
        profile, HookSiteId::main_loop_com_frame_context_sentinel);
    check(frame_context != nullptr &&
              frame_context->rva == Rva{0x001FF7B1} &&
              frame_context->expected_size == 29 &&
              frame_context->use == SiteUse::validation_only,
          "main-loop Com_Frame context sentinel");

    const auto* com_frame_entry =
        find_hook_site(profile, HookSiteId::com_frame_entry_sentinel);
    check(com_frame_entry != nullptr &&
              com_frame_entry->rva == Rva{0x0019E330} &&
              com_frame_entry->expected_size == 19 &&
              com_frame_entry->use == SiteUse::validation_only,
          "Com_Frame entry/ABI sentinel");

    const auto* scr_place_setup = find_hook_site(
        profile,
        HookSiteId::scr_place_setup_float_viewport_entry_sentinel);
    constexpr std::array<std::uint8_t, 8> expected_scr_place_width_store{
        0xF3, 0x0F, 0x11, 0x67, 0x28,
        0xF3, 0x0F, 0x11,
    };
    check(scr_place_setup != nullptr &&
              scr_place_setup->rva == Rva{0x0007A1C0} &&
              scr_place_setup->expected_size == 47 &&
              scr_place_setup->role == HookRole::render &&
              scr_place_setup->use == SiteUse::validation_only &&
              std::ranges::equal(
                  scr_place_setup->expected_bytes().subspan(15, 8),
                  expected_scr_place_width_store),
          "ScrPlace_SetupFloatViewport exact EDI/stack ABI sentinel");

    const auto* scr_place_init = find_hook_site(
        profile, HookSiteId::scr_place_view_zero_init_context_sentinel);
    constexpr std::array<std::uint8_t, 5> expected_scr_place_object{
        0xBF, 0x18, 0x73, 0x95, 0x00,
    };
    constexpr std::array<std::uint8_t, 5> expected_scr_place_call{
        0xE8, 0xAA, 0x54, 0xE3, 0xFF,
    };
    check(scr_place_init != nullptr &&
              scr_place_init->rva == Rva{0x00244CFB} &&
              scr_place_init->expected_size == 47 &&
              scr_place_init->use == SiteUse::validation_only &&
              std::ranges::equal(
                  scr_place_init->expected_bytes().first(
                      expected_scr_place_object.size()),
                  expected_scr_place_object) &&
              std::ranges::equal(
                  scr_place_init->expected_bytes().subspan(22, 5),
                  expected_scr_place_call),
          "renderer scrPlaceView[0] setup call context sentinel");

    const auto* cg_draw_2d =
        find_hook_site(profile, HookSiteId::cg_draw_2d_call);
    constexpr std::array<std::uint8_t, 5> expected_cg_draw_2d_call{
        0xE8, 0xF0, 0x5F, 0xFD, 0xFF,
    };
    check(cg_draw_2d != nullptr &&
              cg_draw_2d->rva == Rva{0x000628AB} &&
              cg_draw_2d->role == HookRole::render &&
              cg_draw_2d->use == SiteUse::detour_candidate &&
              std::ranges::equal(
                  cg_draw_2d->expected_bytes(), expected_cg_draw_2d_call),
          "CG_DrawActiveFrame sole CG_Draw2D callsite");

    const auto* cg_draw_2d_context = find_hook_site(
        profile, HookSiteId::cg_draw_2d_call_context_sentinel);
    check(cg_draw_2d_context != nullptr &&
              cg_draw_2d_context->rva == Rva{0x0006289D} &&
              cg_draw_2d_context->expected_size == 35 &&
              cg_draw_2d_context->use == SiteUse::validation_only &&
              cg_draw_2d_context->expected_bytes()[12] == 0x8B &&
              cg_draw_2d_context->expected_bytes()[13] == 0xC5 &&
              std::ranges::equal(
                  cg_draw_2d_context->expected_bytes().subspan(14, 5),
                  expected_cg_draw_2d_call),
          "CG_Draw2D EAX-local-client call context sentinel");

    const auto* cl_key_event = find_hook_site(
        profile, HookSiteId::cl_key_event_entry_sentinel);
    check(cl_key_event != nullptr &&
              cl_key_event->rva == Rva{0x000780F0} &&
              cl_key_event->expected_size == 42 &&
              cl_key_event->use == SiteUse::validation_only &&
              cl_key_event->expected_bytes().front() == 0x81 &&
              cl_key_event->expected_bytes().back() == 0x57,
          "CL_KeyEvent exact entry/ABI sentinel");
    const auto* cl_key_dispatch = find_hook_site(
        profile, HookSiteId::cl_key_event_dispatch_context_sentinel);
    check(cl_key_dispatch != nullptr &&
              cl_key_dispatch->rva == Rva{0x0019B67F} &&
              cl_key_dispatch->expected_size == 27 &&
              cl_key_dispatch->use == SiteUse::validation_only,
          "CL_KeyEvent native dispatch context sentinel");

    const auto* ui_mouse_event = find_hook_site(
        profile, HookSiteId::ui_mouse_event_entry_sentinel);
    constexpr std::array<std::uint8_t, 8> expected_ui_store{
        0xF3, 0x0F, 0x11, 0x05, 0x30, 0xE9, 0x08, 0x02,
    };
    check(ui_mouse_event != nullptr &&
              ui_mouse_event->rva == Rva{0x001BB4F0} &&
              ui_mouse_event->expected_size == 43 &&
              ui_mouse_event->use == SiteUse::validation_only &&
              std::ranges::equal(
                  ui_mouse_event->expected_bytes().last(
                      expected_ui_store.size()),
                  expected_ui_store),
          "UI_MouseEvent exact two-argument entry sentinel");
    const auto* ui_mouse_native_call = find_hook_site(
        profile, HookSiteId::ui_mouse_event_native_call);
    constexpr std::array<std::uint8_t, 5> expected_ui_mouse_native_call{
        0xE8, 0x2B, 0xDB, 0xF7, 0xFF,
    };
    check(ui_mouse_native_call != nullptr &&
              ui_mouse_native_call->rva == Rva{0x0023D9C0} &&
              ui_mouse_native_call->minimum_patch_bytes == 5 &&
              ui_mouse_native_call->expected_size == 5 &&
              ui_mouse_native_call->use == SiteUse::detour_candidate &&
              std::ranges::equal(
                  ui_mouse_native_call->expected_bytes(),
                  expected_ui_mouse_native_call),
          "CL_MouseEvent exact UI_MouseEvent call patch site");
    const auto* ui_mouse_call = find_hook_site(
        profile, HookSiteId::ui_mouse_event_call_context_sentinel);
    check(ui_mouse_call != nullptr &&
              ui_mouse_call->rva == Rva{0x0023D9A0} &&
              ui_mouse_call->expected_size == 44 &&
              ui_mouse_call->use == SiteUse::validation_only,
          "UI_MouseEvent native CL_MouseEvent call context sentinel");

    const auto* cbuf_add_text = find_hook_site(
        profile, HookSiteId::cbuf_add_text_entry_sentinel);
    check(cbuf_add_text != nullptr &&
              cbuf_add_text->rva == Rva{0x00194200} &&
              cbuf_add_text->expected_size == 48 &&
              cbuf_add_text->use == SiteUse::validation_only &&
              cbuf_add_text->expected_bytes()[8] == 0x8B &&
              cbuf_add_text->expected_bytes()[9] == 0xF0 &&
              cbuf_add_text->expected_bytes()[10] == 0x8B &&
              cbuf_add_text->expected_bytes()[11] == 0xF9,
          "Cbuf_AddText exact EAX-text/ECX-buffer entry sentinel");
    const auto* winmain_cbuf = find_hook_site(
        profile, HookSiteId::winmain_cbuf_add_text_call_context_sentinel);
    constexpr std::array<std::uint8_t, 12> expected_winmain_cbuf{
        0xB8, 0x90, 0x32, 0x88, 0x00,
        0x33, 0xC9,
        0xE8, 0x67, 0x4A, 0xF9, 0xFF,
    };
    check(winmain_cbuf != nullptr &&
              winmain_cbuf->rva == Rva{0x001FF78D} &&
              winmain_cbuf->expected_size == expected_winmain_cbuf.size() &&
              std::ranges::equal(
                  winmain_cbuf->expected_bytes(), expected_winmain_cbuf),
          "WinMain Cbuf_AddText register-ABI call context sentinel");

    const auto* weapnext_identity = find_hook_site(
        profile, HookSiteId::weapnext_command_identity_sentinel);
    check(weapnext_identity != nullptr &&
              weapnext_identity->rva == Rva{0x00036269} &&
              weapnext_identity->expected_size == 47 &&
              weapnext_identity->expected_bytes()[0] == 0xBE &&
              weapnext_identity->expected_bytes()[1] == 0x30 &&
              weapnext_identity->expected_bytes()[2] == 0xD1 &&
              weapnext_identity->expected_bytes()[14] == 0xB9 &&
              weapnext_identity->expected_bytes()[15] == 0xE0 &&
              weapnext_identity->expected_bytes()[16] == 0x9D &&
              weapnext_identity->use == SiteUse::validation_only,
          "weapnext string and handler identity sentinel");
    const auto* weapnext_registration = find_hook_site(
        profile, HookSiteId::weapnext_command_registration_sentinel);
    check(weapnext_registration != nullptr &&
              weapnext_registration->rva == Rva{0x00036298} &&
              weapnext_registration->expected_size == 33 &&
              weapnext_registration->use == SiteUse::validation_only,
          "weapnext native command registration sentinel");
    const auto* weapnext_handler = find_hook_site(
        profile, HookSiteId::weapnext_handler_entry_sentinel);
    check(weapnext_handler != nullptr &&
              weapnext_handler->rva == Rva{0x00069DE0} &&
              weapnext_handler->expected_size == 47 &&
              weapnext_handler->use == SiteUse::validation_only,
          "weapnext native handler entry sentinel");

    const auto* post_build = find_hook_site(profile, HookSiteId::post_build_usercmd);
    check(post_build != nullptr, "post-build usercmd hook exists in exact profile");
    check(post_build->rva == Rva{0x0023E96C}, "post-build usercmd RVA");
    check(post_build->minimum_patch_bytes == 5, "post-build hook instruction width");
    constexpr std::array<std::uint8_t, 13> expected_post_build{
        0xB9, 0x0E, 0x00, 0x00, 0x00, 0x8B, 0xF0,
        0x8D, 0x7C, 0x24, 0x10, 0xF3, 0xA5,
    };
    check(std::ranges::equal(post_build->expected_bytes(), expected_post_build),
          "post-build usercmd expected bytes profile constant");

    const auto* auto_melee_branch = find_hook_site(
        profile, HookSiteId::auto_melee_enabled_branch);
    check(auto_melee_branch != nullptr &&
              auto_melee_branch->rva == Rva{0x00003334} &&
              auto_melee_branch->minimum_patch_bytes == 6 &&
              auto_melee_branch->role == HookRole::input &&
              auto_melee_branch->use == SiteUse::detour_candidate,
          "auto-melee target-aim branch is an exact input patch site");
    constexpr std::array<std::uint8_t, 6> expected_auto_melee_branch{
        0x0F, 0x84, 0x03, 0x02, 0x00, 0x00,
    };
    check(auto_melee_branch != nullptr && std::ranges::equal(
              auto_melee_branch->expected_bytes(),
              expected_auto_melee_branch),
          "auto-melee target-aim branch bytes");
    const auto* auto_melee_context = find_hook_site(
        profile, HookSiteId::auto_melee_enabled_context_sentinel);
    check(auto_melee_context != nullptr &&
              auto_melee_context->rva == Rva{0x0000331C} &&
              auto_melee_context->expected_size == 48 &&
              auto_melee_context->use == SiteUse::validation_only,
          "auto-melee dvar and downstream-target context sentinel");

    const auto* viewmodel =
        find_hook_site(profile, HookSiteId::viewmodel_weapon_call);
    check(viewmodel != nullptr, "viewmodel weapon call exists in exact profile");
    check(viewmodel->rva == Rva{0x00069D1E}, "viewmodel weapon call RVA");
    check(viewmodel->use == SiteUse::detour_candidate,
          "viewmodel weapon call is patchable");
    constexpr std::array<std::uint8_t, 5> expected_viewmodel{
        0xE8, 0x7D, 0xFA, 0xFF, 0xFF,
    };
    check(std::ranges::equal(viewmodel->expected_bytes(), expected_viewmodel),
          "viewmodel weapon call expected bytes profile constant");

    const auto* pose_update =
        find_hook_site(profile, HookSiteId::viewmodel_pose_update_call);
    check(pose_update != nullptr,
          "viewmodel pose-update call exists in exact profile");
    check(pose_update->rva == Rva{0x000698EC},
          "viewmodel pose-update call RVA");
    check(pose_update->role == HookRole::weapon_path &&
              pose_update->use == SiteUse::detour_candidate,
          "viewmodel pose-update call is an approved weapon-path patch");
    check(pose_update->minimum_patch_bytes == 5,
          "viewmodel pose-update call instruction width");
    constexpr std::array<std::uint8_t, 5> expected_pose_update{
        0xE8, 0x0F, 0xE2, 0xFF, 0xFF,
    };
    check(std::ranges::equal(pose_update->expected_bytes(), expected_pose_update),
          "viewmodel pose-update call expected bytes profile constant");

    const auto* camera_tag = find_hook_site(
        profile, HookSiteId::viewmodel_camera_tag_matrix_call);
    check(camera_tag != nullptr && camera_tag->rva == Rva{0x0002DD2D},
          "viewmodel tag_camera matrix call RVA");
    check(camera_tag->role == HookRole::weapon_path &&
              camera_tag->use == SiteUse::detour_candidate &&
              camera_tag->minimum_patch_bytes == 5,
          "viewmodel tag_camera matrix call is an approved camera patch");
    constexpr std::array<std::uint8_t, 5> expected_camera_tag{
        0xE8, 0x5E, 0x52, 0x01, 0x00,
    };
    check(std::ranges::equal(
              camera_tag->expected_bytes(), expected_camera_tag),
          "viewmodel tag_camera matrix call bytes");
    const auto* camera_context = find_hook_site(
        profile, HookSiteId::viewmodel_camera_tag_matrix_context_sentinel);
    check(camera_context != nullptr &&
              camera_context->rva == Rva{0x0002DD10} &&
              camera_context->expected_size == 46 &&
              camera_context->use == SiteUse::validation_only,
          "CG_ApplyViewAnimation tag_camera/refdef context sentinel");

    const auto* authoritative =
        find_hook_site(profile, HookSiteId::authoritative_gun_angles_sentinel);
    check(authoritative != nullptr && authoritative->rva == Rva{0x000E8D10},
          "authoritative gun-angle copy sentinel");
    check(authoritative->use == SiteUse::validation_only,
          "authoritative gun-angle copy is not patchable");

    const auto* muzzle =
        find_hook_site(profile, HookSiteId::calc_muzzle_points_sentinel);
    check(muzzle != nullptr && muzzle->rva == Rva{0x00151380},
          "CalcMuzzlePoints sentinel");
    check(muzzle->use == SiteUse::validation_only,
          "CalcMuzzlePoints is not patchable");

    const auto* fire_muzzle = find_hook_site(
        profile, HookSiteId::fire_weapon_calc_muzzle_call);
    check(fire_muzzle != nullptr &&
              fire_muzzle->rva == Rva{0x001515E3},
          "FireWeapon primary CalcMuzzlePoints call RVA");
    check(fire_muzzle->role == HookRole::weapon_path &&
              fire_muzzle->use == SiteUse::detour_candidate &&
              fire_muzzle->minimum_patch_bytes == 5,
          "FireWeapon Calc call is the approved ballistics patch");
    constexpr std::array<std::uint8_t, 5> expected_fire_muzzle{
        0xE8, 0x98, 0xFD, 0xFF, 0xFF,
    };
    check(std::ranges::equal(
              fire_muzzle->expected_bytes(), expected_fire_muzzle),
          "FireWeapon Calc call bytes");

    const auto* fire_context = find_hook_site(
        profile, HookSiteId::fire_weapon_calc_muzzle_context_sentinel);
    check(fire_context != nullptr &&
              fire_context->rva == Rva{0x001515CD} &&
              fire_context->expected_size == 48 &&
              fire_context->use == SiteUse::validation_only,
          "FireWeapon weaponParms/Calc call context sentinel");

    const auto* bullet_call = find_hook_site(
        profile, HookSiteId::fire_weapon_bullet_fire_call);
    check(bullet_call != nullptr &&
              bullet_call->rva == Rva{0x001516BE},
          "FireWeapon ordinary-bullet Bullet_Fire call RVA");
    check(bullet_call->role == HookRole::weapon_path &&
              bullet_call->use == SiteUse::detour_candidate &&
              bullet_call->minimum_patch_bytes == 5,
          "FireWeapon Bullet_Fire call is an approved spread patch");
    constexpr std::array<std::uint8_t, 5> expected_bullet_call{
        0xE8, 0x4D, 0x51, 0xF9, 0xFF,
    };
    check(std::ranges::equal(
              bullet_call->expected_bytes(), expected_bullet_call),
          "FireWeapon Bullet_Fire call bytes");

    const auto* bullet_context = find_hook_site(
        profile, HookSiteId::fire_weapon_bullet_fire_context_sentinel);
    check(bullet_context != nullptr &&
              bullet_context->rva == Rva{0x001516A1} &&
              bullet_context->expected_size == 44 &&
              bullet_context->use == SiteUse::validation_only,
          "FireWeapon ordinary-bullet argument/call context sentinel");

    const auto* bullet_entry = find_hook_site(
        profile, HookSiteId::bullet_fire_entry_sentinel);
    check(bullet_entry != nullptr &&
              bullet_entry->rva == Rva{0x000E6810} &&
              bullet_entry->expected_size == 41 &&
              bullet_entry->use == SiteUse::validation_only,
          "Bullet_Fire weaponParms ABI entry sentinel");

    const auto* spread_consumer = find_hook_site(
        profile, HookSiteId::bullet_fire_spread_argument_sentinel);
    check(spread_consumer != nullptr &&
              spread_consumer->rva == Rva{0x000E68F8} &&
              spread_consumer->expected_size == 33 &&
              spread_consumer->use == SiteUse::validation_only,
          "Bullet_Fire authoritative spread consumer sentinel");

    const auto* client_spread_call = find_hook_site(
        profile, HookSiteId::draw_bullet_impacts_get_spread_call);
    constexpr std::array<std::uint8_t, 5> expected_client_spread_call{
        0xE8, 0x4F, 0x4D, 0xFB, 0xFF,
    };
    check(client_spread_call != nullptr &&
              client_spread_call->rva == Rva{0x00068DCC} &&
              client_spread_call->use == SiteUse::detour_candidate &&
              std::ranges::equal(
                  client_spread_call->expected_bytes(),
                  expected_client_spread_call),
          "local bullet client-spread call is the approved visual-effects patch");
    const auto* client_spread_context = find_hook_site(
        profile,
        HookSiteId::draw_bullet_impacts_get_spread_context_sentinel);
    check(client_spread_context != nullptr &&
              client_spread_context->rva == Rva{0x00068DC0} &&
              client_spread_context->expected_size == 20 &&
              client_spread_context->use == SiteUse::validation_only,
          "local bullet spread output ABI context is independently validated");
    const auto* get_spread_entry = find_hook_site(
        profile, HookSiteId::get_spread_for_weapon_entry_sentinel);
    check(get_spread_entry != nullptr &&
              get_spread_entry->rva == Rva{0x0001DB20} &&
              get_spread_entry->expected_size == 32 &&
              get_spread_entry->use == SiteUse::validation_only,
          "BG_GetSpreadForWeapon custom output ABI is independently validated");

    const auto* client_origin_call = find_hook_site(
        profile, HookSiteId::draw_bullet_impacts_view_origin_call);
    constexpr std::array<std::uint8_t, 5> expected_client_origin_call{
        0xE8, 0x1C, 0xFC, 0xFF, 0xFF,
    };
    check(client_origin_call != nullptr &&
              client_origin_call->rva == Rva{0x00068DDF} &&
              client_origin_call->use == SiteUse::detour_candidate &&
              std::ranges::equal(
                  client_origin_call->expected_bytes(),
                  expected_client_origin_call),
          "local bullet client-origin call is the approved visual-effects patch");
    const auto* client_origin_context = find_hook_site(
        profile,
        HookSiteId::draw_bullet_impacts_view_origin_context_sentinel);
    check(client_origin_context != nullptr &&
              client_origin_context->rva == Rva{0x00068DD4} &&
              client_origin_context->expected_size == 35 &&
              client_origin_context->use == SiteUse::validation_only,
          "local bullet origin/output stack context is independently validated");
    const auto* client_origin_entry = find_hook_site(
        profile, HookSiteId::client_bullet_view_origin_entry_sentinel);
    check(client_origin_entry != nullptr &&
              client_origin_entry->rva == Rva{0x00068A00} &&
              client_origin_entry->expected_size == 34 &&
              client_origin_entry->use == SiteUse::validation_only,
          "local bullet view-origin helper custom ABI is independently validated");

    constexpr std::array<std::uint8_t, 36> expected_registration{
        0x80, 0x81, 0x85, 0x00, 0x90, 0xE6, 0x4E, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x70, 0x81, 0x85, 0x00, 0x20, 0xE8, 0x4E, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x5C, 0x81, 0x85, 0x00, 0x20, 0xE7, 0x4E, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    const auto* get_player_angles_registration = find_hook_site(
        profile, HookSiteId::get_player_angles_registration_sentinel);
    check(get_player_angles_registration != nullptr &&
              get_player_angles_registration->rva == Rva{0x0043C0D0} &&
              get_player_angles_registration->role == HookRole::weapon_path &&
              get_player_angles_registration->use == SiteUse::validation_only &&
              std::ranges::equal(get_player_angles_registration->expected_bytes(),
                                 expected_registration),
          "getplayerangles exact script-method registration record");

    constexpr std::array<std::uint8_t, 45> expected_get_player_angles_entry{
        0x8B, 0x44, 0x24, 0x04, 0x8B, 0xC8, 0xC1, 0xE9,
        0x10, 0x56, 0x57, 0x75, 0x3F, 0x0F, 0xB7, 0xC0,
        0x8B, 0xF0, 0x69, 0xF6, 0x78, 0x03, 0x00, 0x00,
        0x81, 0xC6, 0xF0, 0xC6, 0x76, 0x01, 0x83, 0xBE,
        0x80, 0x01, 0x00, 0x00, 0x00, 0x75, 0x42, 0x50,
        0x68, 0x94, 0x82, 0x85, 0x00,
    };
    const auto* get_player_angles_entry = find_hook_site(
        profile, HookSiteId::get_player_angles_entry_sentinel);
    check(get_player_angles_entry != nullptr &&
              get_player_angles_entry->rva == Rva{0x000EE820} &&
              get_player_angles_entry->use == SiteUse::validation_only &&
              std::ranges::equal(get_player_angles_entry->expected_bytes(),
                                 expected_get_player_angles_entry),
          "GScr_GetPlayerAngles exact entity-handle entry sentinel");

    constexpr std::array<std::uint8_t, 26> expected_get_player_angles_context{
        0x8B, 0x96, 0x80, 0x01, 0x00, 0x00,
        0x81, 0xC2, 0x24, 0x01, 0x00, 0x00,
        0x52, 0x33, 0xC0,
        0xE8, 0xA3, 0xC0, 0x1A, 0x00,
        0x83, 0xC4, 0x04, 0x5F, 0x5E, 0xC3,
    };
    const auto* get_player_angles_context = find_hook_site(
        profile, HookSiteId::get_player_angles_return_context_sentinel);
    check(get_player_angles_context != nullptr &&
              get_player_angles_context->rva == Rva{0x000EE889} &&
              get_player_angles_context->use == SiteUse::validation_only &&
              std::ranges::equal(get_player_angles_context->expected_bytes(),
                                 expected_get_player_angles_context),
          "GScr_GetPlayerAngles exact Scr_AddVector return context");

    constexpr std::array<std::uint8_t, 5> expected_scr_add_vector_call{
        0xE8, 0xA3, 0xC0, 0x1A, 0x00,
    };
    const auto* scr_add_vector_call = find_hook_site(
        profile, HookSiteId::get_player_angles_scr_add_vector_call);
    check(scr_add_vector_call != nullptr &&
              scr_add_vector_call->rva == Rva{0x000EE898} &&
              scr_add_vector_call->role == HookRole::weapon_path &&
              scr_add_vector_call->use == SiteUse::detour_candidate &&
              scr_add_vector_call->minimum_patch_bytes == 5 &&
              std::ranges::equal(scr_add_vector_call->expected_bytes(),
                                 expected_scr_add_vector_call),
          "GScr_GetPlayerAngles exact patchable Scr_AddVector call");

    constexpr std::array<std::uint8_t, 47> expected_scr_add_vector_entry{
        0x51, 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x57,
        0x8B, 0xF8, 0xE8, 0x51, 0x94, 0xFF, 0xFF, 0x8B,
        0xF7, 0x69, 0xF6, 0x20, 0x43, 0x00, 0x00, 0x8B,
        0x86, 0x10, 0x47, 0xBD, 0x03, 0x3B, 0x86, 0x04,
        0x47, 0xBD, 0x03, 0x75, 0x0A, 0x68, 0xAC, 0xAC,
        0x89, 0x00, 0xE8, 0x51, 0x3F, 0xF6, 0xFF,
    };
    const auto* scr_add_vector_entry = find_hook_site(
        profile, HookSiteId::scr_add_vector_entry_sentinel);
    check(scr_add_vector_entry != nullptr &&
              scr_add_vector_entry->rva == Rva{0x0029A940} &&
              scr_add_vector_entry->use == SiteUse::validation_only &&
              std::ranges::equal(scr_add_vector_entry->expected_bytes(),
                                 expected_scr_add_vector_entry),
          "Scr_AddVector exact EAX-instance/stack-vector ABI entry");

    constexpr std::array<std::uint8_t, 40> expected_current_weapon_identity{
        0x8B, 0xB6, 0x80, 0x01, 0x00, 0x00,
        0x0F, 0xB6, 0x86, 0xF8, 0x20, 0x00, 0x00,
        0x8B, 0xB6, 0x04, 0x01, 0x00, 0x00, 0x85, 0xF6,
        0x76, 0x17, 0x8B, 0x14, 0xB5, 0x70, 0x67, 0x8F,
        0x00, 0x8B, 0x02, 0x50, 0x33, 0xC0, 0xE8, 0xBF,
        0xCE, 0x1A, 0x00,
    };
    const auto* current_weapon_identity = find_hook_site(
        profile, HookSiteId::get_current_weapon_identity_sentinel);
    check(current_weapon_identity != nullptr &&
              current_weapon_identity->rva == Rva{0x000ED8F9} &&
              current_weapon_identity->use == SiteUse::validation_only &&
              std::ranges::equal(current_weapon_identity->expected_bytes(),
                                 expected_current_weapon_identity),
          "getCurrentWeapon exact primary weapon identity sentinel");

    constexpr std::array<std::uint8_t, 45>
        expected_current_weapon_fallback_identity{
            0x85, 0xC0, 0x7E, 0x17, 0x8B, 0x0C, 0x85, 0x70,
            0x67, 0x8F, 0x00, 0x8B, 0x11, 0x52, 0x33, 0xC0,
            0xE8, 0xA4, 0xCE, 0x1A, 0x00, 0x83, 0xC4, 0x04,
            0x5F, 0x5E, 0xC3, 0x68, 0x18, 0x85, 0x81, 0x00,
            0x33, 0xC0, 0xE8, 0x92, 0xCE, 0x1A, 0x00, 0x83,
            0xC4, 0x04, 0x5F, 0x5E, 0xC3,
        };
    const auto* current_weapon_fallback_identity = find_hook_site(
        profile, HookSiteId::get_current_weapon_fallback_identity_sentinel);
    check(current_weapon_fallback_identity != nullptr &&
              current_weapon_fallback_identity->rva == Rva{0x000ED927} &&
              current_weapon_fallback_identity->use == SiteUse::validation_only &&
              std::ranges::equal(
                  current_weapon_fallback_identity->expected_bytes(),
                  expected_current_weapon_fallback_identity),
          "getCurrentWeapon exact action-slot fallback identity sentinel");

    constexpr std::array<std::uint8_t, 23>
        expected_weapon_definition_registration{
            0x56,
            0x8B, 0x35, 0xBC, 0xE3, 0x6D, 0x04,
            0x83, 0xC6, 0x01,
            0x89, 0x35, 0xBC, 0xE3, 0x6D, 0x04,
            0x89, 0x04, 0xB5, 0x70, 0x67, 0x8F, 0x00,
        };
    const auto* weapon_definition_registration = find_hook_site(
        profile, HookSiteId::weapon_definition_registration_sentinel);
    check(weapon_definition_registration != nullptr &&
              weapon_definition_registration->rva == Rva{0x0001D360} &&
              weapon_definition_registration->role == HookRole::weapon_path &&
              weapon_definition_registration->use == SiteUse::validation_only &&
              std::ranges::equal(
                  weapon_definition_registration->expected_bytes(),
                  expected_weapon_definition_registration),
          "weapon-definition exact count/table registration sentinel");

    const auto* ring = find_data_symbol(profile, DataSymbolId::command_ring);
    check(ring != nullptr && ring->rva == Rva{0x02CFD700}, "usercmd ring RVA");
    check(ring->size == 128U * sizeof(UsercmdSp), "usercmd ring extent");
    const auto* command_number =
        find_data_symbol(profile, DataSymbolId::command_number);
    check(command_number != nullptr && command_number->rva == Rva{0x02CFF300},
          "usercmd number RVA");
    const auto* gun_pitch =
        find_data_symbol(profile, DataSymbolId::cgame_gun_pitch_degrees);
    const auto* gun_yaw =
        find_data_symbol(profile, DataSymbolId::cgame_gun_yaw_degrees);
    check(gun_pitch != nullptr && gun_pitch->rva == Rva{0x0312B65C},
          "client gun pitch RVA");
    check(gun_yaw != nullptr && gun_yaw->rva == Rva{0x0312B660},
          "client gun yaw RVA");
    const auto* view_yaw =
        find_data_symbol(profile, DataSymbolId::client_view_yaw_degrees);
    const auto* key_catchers =
        find_data_symbol(profile, DataSymbolId::key_catchers);
    const auto* connection_state =
        find_data_symbol(profile, DataSymbolId::connection_state);
    check(view_yaw != nullptr && view_yaw->rva == Rva{0x02C7D6D4},
          "live client view-yaw RVA");
    check(key_catchers != nullptr && key_catchers->rva == Rva{0x02C58424},
          "key-catcher RVA");
    check(connection_state != nullptr &&
              connection_state->rva == Rva{0x02C5842C},
          "connection-state RVA");
    const auto* refdef_viewport = find_data_symbol(
        profile, DataSymbolId::gameplay_refdef_viewport);
    check(refdef_viewport != nullptr &&
              refdef_viewport->rva == Rva{0x03120338} &&
              refdef_viewport->size == 0x10,
          "main SP refdef viewport RVA and exact consumed extent");
    const auto* scr_place_view = find_data_symbol(
        profile, DataSymbolId::scr_place_view_zero);
    check(scr_place_view != nullptr &&
              scr_place_view->rva == Rva{0x00557318} &&
              scr_place_view->size == 0x48,
          "scrPlaceView[0] RVA and exact ScreenPlacement extent");
    const auto* local_player =
        find_data_symbol(profile, DataSymbolId::local_player_entity);
    check(local_player != nullptr &&
              local_player->rva == Rva{0x0136C6F0} &&
              local_player->size == 0x378,
          "SP g_entities[0] local-player extent");
    const auto* weapon_definitions = find_data_symbol(
        profile, DataSymbolId::weapon_definition_pointer_table);
    check(weapon_definitions != nullptr &&
              weapon_definitions->rva == Rva{0x004F6770} &&
              weapon_definitions->size == 0x400,
          "SP weapon-definition pointer-table RVA and exact extent");
    const auto* weapon_definition_count = find_data_symbol(
        profile, DataSymbolId::weapon_definition_count);
    check(weapon_definition_count != nullptr &&
              weapon_definition_count->rva == Rva{0x042DE3BC} &&
              weapon_definition_count->size == sizeof(std::uint32_t),
          "SP weapon-definition count RVA and exact extent");
}

void test_multiplayer_profile_constants() {
    const auto& profile = t4_mp_1_7_1263_profile();
    check(profile.variant == ExecutableVariant::multiplayer,
          "MP executable variant");
    check(profile.layout == ExecutableLayoutId::t4_mp_1_7_1263 &&
              profile.on_disk_code == OnDiskCodeState::plain,
          "MP executable layout and plain on-disk state");
    check(profile.observed_filename == "t4mp.exe", "MP observed filename");
    check(profile.product_version == "1.7x (file version 1.7.0.0)",
          "MP file/product version");
    check(profile.engine_build.empty(),
          "MP profile does not invent a non-embedded engine build");
    check(profile.file_size == 5'505'024, "MP exact file size");
    check(sha256_hex(profile.sha256) ==
              "943bb93001ad2ed465b6652c27fb649b5f0c5b24097e18a27a588ac35b3457a0",
          "MP exact SHA-256");
    check(profile.machine == 0x014C && profile.section_count == 5,
          "MP PE machine and section count");
    check(profile.coff_characteristics == 0x0103 &&
              profile.dll_characteristics == 0,
          "MP PE characteristics");
    check(profile.coff_timestamp == 0x4AEA1F42,
          "MP COFF timestamp");
    check(profile.preferred_image_base == 0x00400000 &&
              profile.entry_point_rva == Rva{0x003A8256} &&
              profile.size_of_image == 0x11176000,
          "MP image layout identity");

    const auto profiles = supported_profiles();
    check(profiles.size() == 4,
          "plain and Steam SP/MP profiles are enumerable");
    check(profiles[0] == &t4_sp_1_7_1263_profile() &&
              profiles[1] == &profile &&
              profiles[2] == &t4_steam_sp_1_7_1263_profile() &&
              profiles[3] == &t4_steam_mp_1_7_1263_profile(),
          "supported profile enumeration preserves the existing profiles");
    check(find_supported_profile(profile.file_size, profile.sha256) == &profile,
          "exact MP identity selects MP profile");
    check(find_supported_profile(t4_sp_1_7_1263_profile().file_size,
                                 t4_sp_1_7_1263_profile().sha256) ==
              &t4_sp_1_7_1263_profile(),
          "exact SP identity still selects SP profile");
    auto unknown_hash = profile.sha256;
    unknown_hash.bytes[0] ^= 0xFF;
    check(find_supported_profile(profile.file_size, unknown_hash) == nullptr,
          "unknown exact identity is rejected");

    struct ExpectedSite final {
        HookSiteId id;
        Rva rva;
        std::uint8_t expected_size;
    };
    constexpr std::array<ExpectedSite, 41> expected_sites{{
        {HookSiteId::gameplay_render_scene_call, 0x0003E73E, 5},
        {HookSiteId::render_backend_begin, 0x002BAB26, 8},
        {HookSiteId::lod_tan_half_fov_y_load, 0x002B75FB, 5},
        {HookSiteId::diagnostic_print_sentinel, 0x001623E3, 6},
        {HookSiteId::main_loop_com_frame_call, 0x001D08FF, 5},
        {HookSiteId::main_loop_com_frame_context_sentinel, 0x001D08E5, 31},
        {HookSiteId::com_frame_entry_sentinel, 0x001668D0, 19},
        {HookSiteId::scr_place_setup_float_viewport_entry_sentinel, 0x000A58E0, 47},
        {HookSiteId::scr_place_view_zero_init_context_sentinel, 0x00099CC3, 25},
        {HookSiteId::cg_draw_2d_call, 0x0007A128, 5},
        {HookSiteId::cg_draw_2d_call_context_sentinel, 0x0007A111, 44},
        {HookSiteId::post_build_usercmd, 0x00090083, 5},
        {HookSiteId::auto_melee_enabled_branch, 0x0000329E, 6},
        {HookSiteId::auto_melee_enabled_context_sentinel, 0x00003290, 45},
        {HookSiteId::cl_key_event_entry_sentinel, 0x00094B50, 42},
        {HookSiteId::cl_key_event_dispatch_context_sentinel, 0x0016380F, 27},
        {HookSiteId::ui_mouse_event_entry_sentinel, 0x0019BDD0, 45},
        {HookSiteId::ui_mouse_event_native_call, 0x0008F090, 5},
        {HookSiteId::ui_mouse_event_call_context_sentinel, 0x0008F070, 44},
        {HookSiteId::cbuf_add_text_entry_sentinel, 0x0015C130, 48},
        {HookSiteId::winmain_cbuf_add_text_call_context_sentinel, 0x001D087A, 12},
        {HookSiteId::weapnext_command_identity_sentinel, 0x00039599, 47},
        {HookSiteId::weapnext_command_registration_sentinel, 0x000395C8, 33},
        {HookSiteId::weapnext_handler_entry_sentinel, 0x0007FD40, 46},
        {HookSiteId::viewmodel_weapon_call, 0x0007FC68, 5},
        {HookSiteId::viewmodel_pose_update_call, 0x0007F531, 5},
        {HookSiteId::viewmodel_camera_tag_matrix_call, 0x00030501, 5},
        {HookSiteId::viewmodel_camera_tag_matrix_context_sentinel, 0x000304E4, 46},
        {HookSiteId::calc_muzzle_points_sentinel, 0x00145C40, 16},
        {HookSiteId::fire_weapon_calc_muzzle_call, 0x00145D13, 5},
        {HookSiteId::fire_weapon_calc_muzzle_context_sentinel, 0x00145CFD, 48},
        {HookSiteId::fire_weapon_bullet_fire_call, 0x00145D9A, 5},
        {HookSiteId::fire_weapon_bullet_fire_context_sentinel, 0x00145D85, 36},
        {HookSiteId::bullet_fire_entry_sentinel, 0x000E2740, 43},
        {HookSiteId::bullet_fire_spread_argument_sentinel, 0x000E278F, 46},
        {HookSiteId::draw_bullet_impacts_get_spread_call, 0x0007EA42, 5},
        {HookSiteId::draw_bullet_impacts_get_spread_context_sentinel,
         0x0007EA36, 20},
        {HookSiteId::get_spread_for_weapon_entry_sentinel,
         0x00020620, 32},
        {HookSiteId::draw_bullet_impacts_view_origin_call, 0x0007EA55, 5},
        {HookSiteId::draw_bullet_impacts_view_origin_context_sentinel,
         0x0007EA4A, 35},
        {HookSiteId::client_bullet_view_origin_entry_sentinel,
         0x0007E6C0, 34},
    }};
    check(profile.sites.size() == expected_sites.size(),
          "MP profile contains every verified and only verified site");
    for (const auto& expected : expected_sites) {
        const auto* site = find_hook_site(profile, expected.id);
        check(site != nullptr && site->rva == expected.rva &&
                  site->expected_size == expected.expected_size,
              "MP hook-site RVA and expected-byte extent");
        check(site->expected_size <= HookSite::kMaximumExpectedBytes,
              "MP hook-site expected bytes fit storage");
    }
    const auto* mp_ui_mouse_native_call = find_hook_site(
        profile, HookSiteId::ui_mouse_event_native_call);
    constexpr std::array<std::uint8_t, 5> expected_mp_ui_mouse_native_call{
        0xE8, 0x3B, 0xCD, 0x10, 0x00,
    };
    check(mp_ui_mouse_native_call != nullptr &&
              mp_ui_mouse_native_call->minimum_patch_bytes == 5 &&
              mp_ui_mouse_native_call->use == SiteUse::detour_candidate &&
              std::ranges::equal(
                  mp_ui_mouse_native_call->expected_bytes(),
                  expected_mp_ui_mouse_native_call),
          "MP CL_MouseEvent exact UI_MouseEvent call patch site");
    const auto* lod_load =
        find_hook_site(profile, HookSiteId::lod_tan_half_fov_y_load);
    constexpr std::array<std::uint8_t, 5> expected_lod_load{
        0xF3, 0x0F, 0x10, 0x48, 0x14,
    };
    check(lod_load != nullptr && lod_load->role == HookRole::render &&
              lod_load->use == SiteUse::detour_candidate &&
              lod_load->minimum_patch_bytes == expected_lod_load.size() &&
              std::ranges::equal(
                  lod_load->expected_bytes(), expected_lod_load),
          "MP LOD-only tanHalfFovY movss site and exact context");
    check(find_hook_site(profile,
                         HookSiteId::authoritative_gun_angles_sentinel) == nullptr,
          "unverified MP authoritative gun-angle site is absent");
    check(find_hook_site(
              profile, HookSiteId::get_player_angles_scr_add_vector_call) == nullptr &&
              find_hook_site(
                  profile, HookSiteId::get_player_angles_registration_sentinel) == nullptr &&
              find_hook_site(
                  profile, HookSiteId::get_player_angles_entry_sentinel) == nullptr &&
              find_hook_site(
                  profile, HookSiteId::get_player_angles_return_context_sentinel) == nullptr &&
              find_hook_site(
                  profile, HookSiteId::scr_add_vector_entry_sentinel) == nullptr &&
              find_hook_site(
                  profile, HookSiteId::get_current_weapon_identity_sentinel) == nullptr &&
              find_hook_site(
                  profile,
                  HookSiteId::get_current_weapon_fallback_identity_sentinel) == nullptr &&
              find_hook_site(
                  profile,
                  HookSiteId::weapon_definition_registration_sentinel) == nullptr,
          "SP-only campaign targeting sites are absent from MP profile");

    const auto* post_build =
        find_hook_site(profile, HookSiteId::post_build_usercmd);
    constexpr std::array<std::uint8_t, 5> expected_create_cmd_call{
        0xE8, 0xC8, 0xFE, 0xFF, 0xFF,
    };
    check(post_build != nullptr &&
              post_build->use == SiteUse::detour_candidate &&
              post_build->minimum_patch_bytes == 5 &&
              std::ranges::equal(post_build->expected_bytes(),
                                 expected_create_cmd_call),
          "MP CL_CreateCmd wrapper call site");

    struct ExpectedData final {
        DataSymbolId id;
        Rva rva;
        std::uint32_t size;
    };
    constexpr std::array<ExpectedData, 10> expected_data{{
        {DataSymbolId::command_ring, 0x00BAB334, 128U * 0x2CU},
        {DataSymbolId::command_number, 0x00BAC934, sizeof(std::uint32_t)},
        {DataSymbolId::cgame_gun_pitch_degrees, 0x005EEBC4, sizeof(float)},
        {DataSymbolId::cgame_gun_yaw_degrees, 0x005EEBC8, sizeof(float)},
        {DataSymbolId::client_view_yaw_degrees, 0x00B6B318, sizeof(float)},
        {DataSymbolId::key_catchers, 0x00B44784, sizeof(std::uint32_t)},
        {DataSymbolId::connection_state, 0x00B4478C, sizeof(std::int32_t)},
        {DataSymbolId::gameplay_refdef_viewport, 0x005E676C,
         4U * sizeof(std::int32_t)},
        {DataSymbolId::scr_place_view_zero, 0x00D65970, 0x48},
        {DataSymbolId::local_player_entity, 0x01B8E300, 0x330},
    }};
    check(profile.data_symbols.size() == expected_data.size(),
          "MP profile contains every verified data symbol");
    check(find_data_symbol(
              profile, DataSymbolId::weapon_definition_pointer_table) == nullptr &&
              find_data_symbol(
                  profile, DataSymbolId::weapon_definition_count) == nullptr,
          "SP-only weapon-definition table/count are absent from MP profile");
    for (const auto& expected : expected_data) {
        const auto* symbol = find_data_symbol(profile, expected.id);
        check(symbol != nullptr && symbol->rva == expected.rva &&
                  symbol->size == expected.size,
              "MP data-symbol RVA and extent");
    }
}

void test_steam_profile_constants() {
    const auto& plain_sp = t4_sp_1_7_1263_profile();
    const auto& steam_sp = t4_steam_sp_1_7_1263_profile();
    check(steam_sp.id ==
              "t4-steam-sp-1.7.1263-cl350073-732900d1" &&
              steam_sp.variant == ExecutableVariant::single_player &&
              steam_sp.layout == ExecutableLayoutId::t4_sp_1_7_1263 &&
              steam_sp.on_disk_code == OnDiskCodeState::steam_drm_wrapped,
          "Steam SP identity, variant, layout, and wrapped state");
    check(steam_sp.observed_filename == "CoDWaW.exe" &&
              steam_sp.product_version ==
                  "1.7x (file version 1.7.0.0)" &&
              steam_sp.engine_build == "1.7.1263 CL(350073)",
          "Steam SP descriptive metadata");
    check(steam_sp.file_size == 5'902'336 &&
              sha256_hex(steam_sp.sha256) ==
                  "732900d158982c33e3121f0b86d22230be79839bbcbfe3bdfc1238f408a7d64d",
          "Steam SP complete-file identity");
    check(steam_sp.machine == 0x014C && steam_sp.section_count == 6 &&
              steam_sp.coff_characteristics == 0x0103 &&
              steam_sp.dll_characteristics == 0 &&
              steam_sp.coff_timestamp == 0x4AEA1F46 &&
              steam_sp.preferred_image_base == 0x00400000 &&
              steam_sp.entry_point_rva == Rva{0x04ABB2ED} &&
              steam_sp.size_of_image == 0x04B11000,
          "Steam SP PE identity");
    check_same_layout_descriptors(
        steam_sp, plain_sp,
        "Steam SP reuses only the completely verified SP layout descriptors");

    const auto& plain_mp = t4_mp_1_7_1263_profile();
    const auto& steam_mp = t4_steam_mp_1_7_1263_profile();
    check(steam_mp.id == "t4-steam-mp-1.7.0.0-7d0b518a" &&
              steam_mp.variant == ExecutableVariant::multiplayer &&
              steam_mp.layout == ExecutableLayoutId::t4_mp_1_7_1263 &&
              steam_mp.on_disk_code == OnDiskCodeState::steam_drm_wrapped,
          "Steam MP identity, variant, layout, and wrapped state");
    check(steam_mp.observed_filename == "CoDWaWmp.exe" &&
              steam_mp.product_version ==
                  "1.7x (file version 1.7.0.0)" &&
              steam_mp.engine_build.empty(),
          "Steam MP descriptive metadata");
    check(steam_mp.file_size == 5'857'280 &&
              sha256_hex(steam_mp.sha256) ==
                  "7d0b518a4bd267ffdb6d0203ad8f3721603b172ac13ba2abcdb32584f759d36c",
          "Steam MP complete-file identity");
    check(steam_mp.machine == 0x014C && steam_mp.section_count == 6 &&
              steam_mp.coff_characteristics == 0x0103 &&
              steam_mp.dll_characteristics == 0 &&
              steam_mp.coff_timestamp == 0x4AEA1F42 &&
              steam_mp.preferred_image_base == 0x00400000 &&
              steam_mp.entry_point_rva == Rva{0x111762ED} &&
              steam_mp.size_of_image == 0x111CC000,
          "Steam MP PE identity");
    check_same_layout_descriptors(
        steam_mp, plain_mp,
        "Steam MP reuses only the completely verified MP layout descriptors");

    check(find_supported_profile(steam_sp.file_size, steam_sp.sha256) ==
              &steam_sp &&
              find_supported_profile(steam_mp.file_size, steam_mp.sha256) ==
                  &steam_mp,
          "exact Steam identities select their profiles");
    check(find_supported_profile(steam_sp.file_size, plain_sp.sha256) ==
              nullptr &&
              find_supported_profile(steam_mp.file_size, plain_mp.sha256) ==
                  nullptr,
          "Steam size with a plain-image hash is rejected");
    auto changed_steam_hash = steam_sp.sha256;
    changed_steam_hash.bytes.back() ^= 0x01;
    check(find_supported_profile(steam_sp.file_size, changed_steam_hash) ==
              nullptr,
          "changed Steam identity is rejected");
}

void test_usercmd_weapon_aim() {
    check(sizeof(UsercmdSp) == 0x38, "SP usercmd size");
    check(encode_short_angle_degrees(0.0F) == std::int16_t{0}, "zero angle encoding");
    check(encode_short_angle_degrees(90.0F) == std::int16_t{0x4000},
          "positive quarter-turn encoding");
    check(encode_short_angle_degrees(-90.0F) == std::int16_t{-0x4000},
          "negative quarter-turn encoding");
    check(encode_short_angle_degrees(180.0F) ==
              std::numeric_limits<std::int16_t>::min(),
          "half-turn encoding wraps to signed short");
    check(encode_short_angle_degrees(450.0F) == std::int16_t{0x4000},
          "multi-turn angle encoding");
    check(!encode_short_angle_degrees(std::numeric_limits<float>::quiet_NaN()),
          "non-finite angle is rejected");

    UsercmdSp command{};
    add_button(command, UsercmdButton::reload);
    check(apply_vr_weapon_aim(command, -12.5F, 91.0F, true),
          "finite controller aim applies");
    check(has_button(command, UsercmdButton::reload), "native button is preserved");
    check(has_button(command, UsercmdButton::attack), "VR trigger adds attack");
    check(command.gun_pitch_short == *encode_short_angle_degrees(-12.5F),
          "controller pitch reaches gun-angle field");
    check(command.gun_yaw_short == *encode_short_angle_degrees(91.0F),
          "controller yaw reaches gun-angle field");

    const auto prior_pitch = command.gun_pitch_short;
    const auto prior_yaw = command.gun_yaw_short;
    const auto prior_buttons = command.buttons;
    check(!apply_vr_weapon_aim(command, std::numeric_limits<float>::infinity(), 0.0F,
                              false),
          "invalid controller pose fails");
    check(command.gun_pitch_short == prior_pitch && command.gun_yaw_short == prior_yaw &&
              command.buttons == prior_buttons,
          "invalid controller pose is atomic");

    check(apply_vr_weapon_aim(command, 0.0F, 0.0F, false),
          "released VR trigger still applies aim");
    check(has_button(command, UsercmdButton::attack),
          "released VR trigger does not erase native attack state");
}

void test_address_helpers() {
    const auto render_rva = preferred_va_to_rva(0x006E8B96, 0x00400000);
    check(render_rva.has_value(), "VA to RVA returns a value");
    check(*render_rva == Rva{0x002E8B96}, "VA to RVA value");
    check(!preferred_va_to_rva(0x003FFFFF, 0x00400000), "VA below image base");
    const auto render_va = rva_to_preferred_va(0x002E8B96, 0x00400000);
    check(render_va.has_value(), "RVA to VA returns a value");
    check(*render_va == std::uint32_t{0x006E8B96}, "RVA to VA value");

    std::array<std::uint8_t, 8> bytes{};
    const ModuleView view{bytes.data(), bytes.size()};
    check(view.contains(0, bytes.size()), "module contains complete range");
    check(view.contains(static_cast<Rva>(bytes.size()), 0), "module accepts empty end range");
    check(!view.contains(static_cast<Rva>(bytes.size()), 1), "module rejects overflow range");
}

void test_pe_parser_and_rva_mapping() {
    auto bytes = synthetic_pe();
    const auto parsed = parse_pe32(bytes);
    check(parsed.ok(), "synthetic PE parses");
    check(parsed.image->machine == 0x014C, "synthetic PE machine");
    check(parsed.image->entry_point_rva == 0x1000, "synthetic PE entry point");
    check(parsed.image->sections.size() == 1, "synthetic PE section count");
    const auto file_offset = rva_to_file_offset(*parsed.image, 0x1010, 4, bytes.size());
    check(file_offset.has_value(), "RVA maps into raw section");
    check(*file_offset == std::size_t{0x210}, "RVA raw offset");
    check(!rva_to_file_offset(*parsed.image, 0x2000, 1, bytes.size()),
          "virtual-only RVA is rejected");

    bytes[0] = 0;
    check(!parse_pe32(bytes).ok(), "bad DOS signature is rejected");
}

void test_wrapped_on_disk_validation_policy() {
    constexpr HookSite site{
        .id = HookSiteId::diagnostic_print_sentinel,
        .name = "synthetic encrypted hook bytes",
        .role = HookRole::identity_sentinel,
        .use = SiteUse::validation_only,
        .rva = 0x1010,
        .minimum_patch_bytes = 0,
        .expected_size = 5,
        .expected = {0x11, 0x22, 0x33, 0x44, 0x55},
    };

    auto bytes = synthetic_pe();
    const ExecutableProfile plain{
        .id = "synthetic-plain",
        .variant = ExecutableVariant::single_player,
        .layout = ExecutableLayoutId::t4_sp_1_7_1263,
        .on_disk_code = OnDiskCodeState::plain,
        .file_size = bytes.size(),
        .sha256 = sha256(bytes),
        .machine = 0x014C,
        .section_count = 1,
        .coff_characteristics = 0x0103,
        .dll_characteristics = 0,
        .coff_timestamp = 0x12345678,
        .preferred_image_base = 0x00400000,
        .entry_point_rva = 0x1000,
        .size_of_image = 0x2000,
        .sites = std::span<const HookSite>(&site, 1),
    };
    const auto plain_report = validate_executable_file(bytes, plain);
    check(!plain_report.ok() &&
              std::ranges::any_of(
                  plain_report.issues,
                  [](const ValidationIssue& issue) {
                      return issue.code == ValidationCode::hook_bytes_mismatch;
                  }),
          "plain profile validates on-disk hook bytes");

    auto wrapped = plain;
    wrapped.id = "synthetic-wrapped";
    wrapped.on_disk_code = OnDiskCodeState::steam_drm_wrapped;
    check(validate_executable_file(bytes, wrapped).ok(),
          "wrapped profile skips only encrypted on-disk hook bytes");

    auto wrong_hash = wrapped;
    wrong_hash.sha256.bytes.front() ^= 0x01;
    const auto hash_report = validate_executable_file(bytes, wrong_hash);
    check(!hash_report.ok() &&
              std::ranges::any_of(
                  hash_report.issues,
                  [](const ValidationIssue& issue) {
                      return issue.code == ValidationCode::sha256_mismatch;
                  }),
          "wrapped profile still validates the complete-file SHA-256");

    auto changed_header_bytes = bytes;
    constexpr std::size_t coff_timestamp_offset = 0x84 + 4;
    changed_header_bytes[coff_timestamp_offset] ^= 0x01;
    auto wrong_header = wrapped;
    wrong_header.sha256 = sha256(changed_header_bytes);
    const auto header_report =
        validate_executable_file(changed_header_bytes, wrong_header);
    check(!header_report.ok() &&
              std::ranges::any_of(
                  header_report.issues,
                  [](const ValidationIssue& issue) {
                      return issue.code ==
                          ValidationCode::coff_timestamp_mismatch;
                  }),
          "wrapped profile still validates exact PE headers");

    std::array<std::uint8_t, 0x1020> mapped{};
    auto mapped_report = validate_loaded_hook_sites(
        {mapped.data(), mapped.size()}, wrapped);
    check(!mapped_report.ok() &&
              mapped_report.issues.front().code ==
                  ValidationCode::hook_bytes_mismatch,
          "wrapped state never skips mapped hook-byte validation");
    std::copy(site.expected_bytes().begin(), site.expected_bytes().end(),
              mapped.begin() + site.rva);
    check(validate_loaded_hook_sites(
              {mapped.data(), mapped.size()}, wrapped).ok(),
          "restored wrapped mapped hook bytes pass");
}

void test_mapped_hook_sentinels() {
    constexpr HookSite site{
        .id = HookSiteId::render_backend_begin,
        .name = "synthetic hook",
        .role = HookRole::render,
        .use = SiteUse::detour_candidate,
        .rva = 0x10,
        .minimum_patch_bytes = 5,
        .expected_size = 5,
        .expected = {0x11, 0x22, 0x33, 0x44, 0x55},
    };
    const ExecutableProfile profile{
        .sites = std::span<const HookSite>(&site, 1),
    };

    std::array<std::uint8_t, 0x20> image{};
    std::copy(site.expected_bytes().begin(), site.expected_bytes().end(),
              image.begin() + site.rva);
    check(validate_loaded_hook_sites({image.data(), image.size()}, profile).ok(),
          "matching hook bytes pass");

    image[site.rva + 2] ^= 0xFF;
    const auto rejected = validate_loaded_hook_sites({image.data(), image.size()}, profile);
    check(!rejected.ok(), "changed hook byte is rejected");
    check(rejected.issues.front().code == ValidationCode::hook_bytes_mismatch,
          "changed hook byte has specific failure code");
}

void test_lod_site_mapped_context_for_all_profiles() {
    constexpr std::array<std::uint8_t, 5> expected{
        0xF3, 0x0F, 0x10, 0x48, 0x14,
    };
    for (const auto* const exact_profile : supported_profiles()) {
        const auto* const site = find_hook_site(
            *exact_profile, HookSiteId::lod_tan_half_fov_y_load);
        check(site != nullptr &&
                  std::ranges::equal(site->expected_bytes(), expected),
              "every exact profile carries the mapped LOD context");

        auto isolated_profile = *exact_profile;
        isolated_profile.sites = std::span<const HookSite>(site, 1);
        std::vector<std::uint8_t> mapped(
            static_cast<std::size_t>(site->rva) + site->expected_size);
        std::ranges::copy(
            site->expected_bytes(), mapped.begin() + site->rva);
        check(validate_loaded_hook_sites(
                  {mapped.data(), mapped.size()}, isolated_profile).ok(),
              "all plain and wrapped profiles accept the exact mapped LOD context");

        mapped[site->rva + 2] ^= 0x01;
        const auto rejected = validate_loaded_hook_sites(
            {mapped.data(), mapped.size()}, isolated_profile);
        check(!rejected.ok() &&
                  rejected.issues.front().code ==
                      ValidationCode::hook_bytes_mismatch,
              "all plain and wrapped profiles reject a changed mapped LOD context");
    }
}

void test_loaded_code_readiness_poll() {
    constexpr HookSite site{
        .id = HookSiteId::diagnostic_print_sentinel,
        .name = "synthetic readiness sentinel",
        .role = HookRole::identity_sentinel,
        .use = SiteUse::validation_only,
        .rva = 0x10,
        .minimum_patch_bytes = 0,
        .expected_size = 5,
        .expected = {0x11, 0x22, 0x33, 0x44, 0x55},
    };
    const ExecutableProfile profile{
        .id = "synthetic-readiness",
        .variant = ExecutableVariant::single_player,
        .layout = ExecutableLayoutId::t4_sp_1_7_1263,
        .on_disk_code = OnDiskCodeState::steam_drm_wrapped,
        .sites = std::span<const HookSite>(&site, 1),
    };
    check(kWrappedCodeReadinessTimeoutMs == 30'000 &&
              kWrappedCodeReadinessIntervalMs == 25,
          "wrapped-code readiness bounds are fixed at 30 seconds / 25 ms");

    std::array<std::uint8_t, 0x20> image{};
    std::copy(site.expected_bytes().begin(), site.expected_bytes().end(),
              image.begin() + site.rva);
    const auto ready = wait_for_loaded_code_readiness(
        {image.data(), image.size()}, profile, 0, 25);
    check(ready.ready() && ready.attempts == 1,
          "already-restored mapped code is accepted immediately");

    image[site.rva + 2] ^= 0xFF;
    const auto timed_out = wait_for_loaded_code_readiness(
        {image.data(), image.size()}, profile, 0, 25);
    check(!timed_out.ready() &&
              timed_out.status == LoadedCodeReadinessStatus::timed_out &&
              timed_out.attempts == 1,
          "unrestored mapped code times out without being accepted");
}

void optionally_validate_real_executable(const int argc, char** argv) {
    if (argc != 2) {
        return;
    }
    std::ifstream input(argv[1], std::ios::binary);
    check(static_cast<bool>(input), "real executable opens");
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    const auto digest = sha256(bytes);
    const auto* profile = find_supported_profile(bytes.size(), digest);
    check(profile != nullptr, "real executable has a supported exact identity");
    const auto report = validate_executable_file(bytes, *profile);
    check(report.ok(), "real executable matches its exact T4 profile");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        test_sha256_vectors();
        test_exact_profile_constants();
        test_multiplayer_profile_constants();
        test_steam_profile_constants();
        test_usercmd_weapon_aim();
        test_address_helpers();
        test_pe_parser_and_rva_mapping();
        test_wrapped_on_disk_validation_policy();
        test_mapped_hook_sentinels();
        test_lod_site_mapped_context_for_all_profiles();
        test_loaded_code_readiness_poll();
        optionally_validate_real_executable(argc, argv);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
