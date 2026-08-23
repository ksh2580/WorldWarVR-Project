set(
    WAWVR_MOD_SOURCES
    bootstrap.cpp
    bootstrap.hpp
    direct_boot_patch.cpp
    direct_boot_patch.hpp
    dllmain.cpp
    t4_layout_selector.hpp
)

if(TARGET wawvr_xr)
    list(
        APPEND WAWVR_MOD_SOURCES
        controller_state.cpp
        controller_state.hpp
        camera_comfort_logic.cpp
        camera_comfort_logic.hpp
        comfort_panel_constants.hpp
        immutable_thunk_route.hpp
        menu_button_gesture.cpp
        menu_button_gesture.hpp
        menu_panel_logic.cpp
        menu_panel_logic.hpp
        menu_surface_logic.cpp
        menu_surface_logic.hpp
        peer_thread_quiescence.cpp
        peer_thread_quiescence.hpp
        present_hook.cpp
        present_hook.hpp
        present_hook_logic.cpp
        present_hook_logic.hpp
        stereo_backend_hook.cpp
        stereo_backend_hook.hpp
        stereo_diagnostics.hpp
        stereo_frame_broker.cpp
        stereo_frame_broker.hpp
        stereo_scene_hook.cpp
        stereo_scene_hook.hpp
        stereo_scene_math.cpp
        stereo_scene_math.hpp
        tracking_anchor_sync.hpp
    )
endif()

if(TARGET wawvr_xr AND TARGET wawvr_t4)
    list(
        APPEND WAWVR_MOD_SOURCES
        frame_boundary_hook.cpp
        frame_boundary_hook.hpp
        lod_fov_clamp.cpp
        lod_fov_clamp.hpp
        lod_fov_clamp_logic.hpp
        hud_placement_logic.cpp
        hud_placement_logic.hpp
        weapon_camera_patch.cpp
        weapon_camera_patch.hpp
        input_hook.cpp
        input_hook.hpp
        input_mapping.cpp
        input_mapping.hpp
        menu_input_logic.cpp
        menu_input_logic.hpp
        native_gameplay_command_logic.cpp
        native_gameplay_command_logic.hpp
        t4_menu_input.cpp
        t4_menu_input.hpp
        t4_hud_placement.cpp
        t4_hud_placement.hpp
        t4_hud_placement_hook.cpp
        t4_presentation_state.cpp
        t4_presentation_state.hpp
        viewmodel_filter.cpp
        viewmodel_filter.hpp
        weapon_placement.cpp
        weapon_placement.hpp
        weapon_ballistics_logic.cpp
        weapon_ballistics_logic.hpp
        rocket_barrage_logic.cpp
        rocket_barrage_logic.hpp
        weapon_hook.cpp
        weapon_hook.hpp
    )
endif()

add_library(WorldAtWarVR SHARED ${WAWVR_MOD_SOURCES})

target_link_libraries(WorldAtWarVR PRIVATE wawvr_project_options)
target_compile_definitions(WorldAtWarVR PRIVATE WAWVR_BUILD_DLL=1)

if(TARGET wawvr_t4)
    target_link_libraries(WorldAtWarVR PRIVATE wawvr_t4)
    target_compile_definitions(WorldAtWarVR PRIVATE WAWVR_HAS_T4_BINDINGS=1)
endif()

if(TARGET wawvr_xr)
    target_link_libraries(WorldAtWarVR PRIVATE wawvr_xr d3d9)
    target_compile_definitions(WorldAtWarVR PRIVATE WAWVR_HAS_XR=1)
endif()

if(TARGET wawvr_xr AND TARGET wawvr_t4)
    target_compile_definitions(WorldAtWarVR PRIVATE WAWVR_HAS_INPUT=1)
endif()

set_target_properties(
    WorldAtWarVR
    PROPERTIES
        OUTPUT_NAME "WorldWarVR"
        PREFIX ""
)

if(WAWVR_BUILD_TESTS AND BUILD_TESTING)
    add_executable(WorldAtWarVRDllSmokeTest dll_smoke_test.cpp)
    target_link_libraries(WorldAtWarVRDllSmokeTest PRIVATE wawvr_project_options)
    add_dependencies(WorldAtWarVRDllSmokeTest WorldAtWarVR)

    add_test(
        NAME mod.dll_smoke
        COMMAND WorldAtWarVRDllSmokeTest $<TARGET_FILE:WorldAtWarVR>
    )

    add_executable(
        WorldAtWarVRDirectBootPatchTest
        direct_boot_patch.cpp
        direct_boot_patch.hpp
        direct_boot_patch_test.cpp
    )
    target_link_libraries(
        WorldAtWarVRDirectBootPatchTest
        PRIVATE wawvr_project_options
    )
    add_test(
        NAME mod.direct_boot_patch_logic
        COMMAND WorldAtWarVRDirectBootPatchTest
    )

    add_executable(
        WorldAtWarVRLodFovClampLogicTest
        lod_fov_clamp_logic.hpp
        lod_fov_clamp_logic_test.cpp
    )
    target_link_libraries(
        WorldAtWarVRLodFovClampLogicTest
        PRIVATE wawvr_project_options
    )
    add_test(
        NAME mod.lod_fov_clamp_logic
        COMMAND WorldAtWarVRLodFovClampLogicTest
    )

    if(TARGET wawvr_t4)
        add_executable(
            WorldAtWarVRT4LayoutSelectorTest
            t4_layout_selector.hpp
            t4_layout_selector_test.cpp
        )
        target_link_libraries(
            WorldAtWarVRT4LayoutSelectorTest
            PRIVATE wawvr_project_options wawvr_t4
        )
        add_test(
            NAME mod.t4_layout_selector
            COMMAND WorldAtWarVRT4LayoutSelectorTest
        )
    endif()

    add_executable(
        WorldAtWarVRMenuButtonGestureTest
        menu_button_gesture.cpp
        menu_button_gesture.hpp
        menu_button_gesture_test.cpp
    )
    target_link_libraries(
        WorldAtWarVRMenuButtonGestureTest
        PRIVATE wawvr_project_options
    )
    add_test(
        NAME mod.menu_button_gesture
        COMMAND WorldAtWarVRMenuButtonGestureTest
    )

    add_executable(
        WorldAtWarVRMenuInputLogicTest
        menu_input_logic.cpp
        menu_input_logic.hpp
        menu_input_logic_test.cpp
    )
    target_link_libraries(
        WorldAtWarVRMenuInputLogicTest
        PRIVATE wawvr_project_options
    )
    add_test(
        NAME mod.menu_input_logic
        COMMAND WorldAtWarVRMenuInputLogicTest
    )

    add_executable(
        WorldAtWarVRNativeGameplayCommandLogicTest
        native_gameplay_command_logic.cpp
        native_gameplay_command_logic.hpp
        native_gameplay_command_logic_test.cpp
    )
    target_link_libraries(
        WorldAtWarVRNativeGameplayCommandLogicTest
        PRIVATE wawvr_project_options
    )
    add_test(
        NAME mod.native_gameplay_command_logic
        COMMAND WorldAtWarVRNativeGameplayCommandLogicTest
    )

    add_executable(
        WorldAtWarVRRocketBarrageLogicTest
        rocket_barrage_logic.cpp
        rocket_barrage_logic.hpp
        rocket_barrage_logic_test.cpp
    )
    target_link_libraries(
        WorldAtWarVRRocketBarrageLogicTest
        PRIVATE wawvr_project_options
    )
    add_test(
        NAME mod.rocket_barrage_logic
        COMMAND WorldAtWarVRRocketBarrageLogicTest
    )

    add_executable(
        WorldAtWarVRHudPlacementLogicTest
        hud_placement_logic.cpp
        hud_placement_logic.hpp
        hud_placement_logic_test.cpp
    )
    target_link_libraries(
        WorldAtWarVRHudPlacementLogicTest
        PRIVATE wawvr_project_options
    )
    add_test(
        NAME mod.hud_placement_logic
        COMMAND WorldAtWarVRHudPlacementLogicTest
    )

    if(TARGET wawvr_xr)
        add_executable(
            WorldAtWarVRPeerThreadQuiescenceTest
            peer_thread_quiescence.cpp
            peer_thread_quiescence.hpp
            peer_thread_quiescence_test.cpp
        )
        target_link_libraries(
            WorldAtWarVRPeerThreadQuiescenceTest
            PRIVATE wawvr_project_options
        )
        add_test(
            NAME mod.peer_thread_quiescence
            COMMAND WorldAtWarVRPeerThreadQuiescenceTest
        )

        add_executable(
            WorldAtWarVRMenuPanelLogicTest
            menu_panel_logic.cpp
            menu_panel_logic.hpp
            menu_panel_logic_test.cpp
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.cpp"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.h"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
        )
        target_link_libraries(
            WorldAtWarVRMenuPanelLogicTest
            PRIVATE wawvr_project_options
        )
        target_include_directories(
            WorldAtWarVRMenuPanelLogicTest
            PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
        )
        add_test(
            NAME mod.menu_panel_logic
            COMMAND WorldAtWarVRMenuPanelLogicTest
        )

        add_executable(
            WorldAtWarVRMenuSurfaceLogicTest
            menu_panel_logic.cpp
            menu_panel_logic.hpp
            menu_surface_logic.cpp
            menu_surface_logic.hpp
            menu_surface_logic_test.cpp
            present_hook_logic.cpp
            present_hook_logic.hpp
            "${CMAKE_SOURCE_DIR}/src/xr/compositor_math.cpp"
            "${CMAKE_SOURCE_DIR}/src/xr/compositor_math.hpp"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.cpp"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.h"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
        )
        target_link_libraries(
            WorldAtWarVRMenuSurfaceLogicTest
            PRIVATE wawvr_project_options
        )
        target_include_directories(
            WorldAtWarVRMenuSurfaceLogicTest
            PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
        )
        add_test(
            NAME mod.menu_surface_logic
            COMMAND WorldAtWarVRMenuSurfaceLogicTest
        )

        add_executable(
            WorldAtWarVRCameraComfortLogicTest
            camera_comfort_logic.cpp
            camera_comfort_logic.hpp
            camera_comfort_logic_test.cpp
            "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
        )
        target_link_libraries(
            WorldAtWarVRCameraComfortLogicTest
            PRIVATE wawvr_project_options
        )
        target_include_directories(
            WorldAtWarVRCameraComfortLogicTest
            PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
        )
        add_test(
            NAME mod.camera_comfort_logic
            COMMAND WorldAtWarVRCameraComfortLogicTest
        )

        add_executable(
            WorldAtWarVRStereoFrameBrokerTest
            stereo_frame_broker.cpp
            stereo_frame_broker.hpp
            stereo_frame_broker_test.cpp
            stereo_diagnostics.hpp
            "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
        )
        target_link_libraries(
            WorldAtWarVRStereoFrameBrokerTest
            PRIVATE wawvr_project_options
        )
        target_include_directories(
            WorldAtWarVRStereoFrameBrokerTest
            PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
        )
        add_test(
            NAME mod.stereo_frame_broker
            COMMAND WorldAtWarVRStereoFrameBrokerTest
        )
    endif()

    if(TARGET wawvr_xr)
        add_executable(
            WorldAtWarVRPresentHookLogicTest
            immutable_thunk_route.hpp
            present_hook_logic.cpp
            present_hook_logic.hpp
            present_hook_logic_test.cpp
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.cpp"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.h"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
        )
        target_link_libraries(
            WorldAtWarVRPresentHookLogicTest
            PRIVATE wawvr_project_options
        )
        target_include_directories(
            WorldAtWarVRPresentHookLogicTest
            PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
        )
        add_test(
            NAME mod.present_hook_logic
            COMMAND WorldAtWarVRPresentHookLogicTest
        )

        add_executable(
            WorldAtWarVRStereoSceneMathTest
            camera_comfort_logic.cpp
            camera_comfort_logic.hpp
            stereo_scene_math.cpp
            stereo_scene_math.hpp
            stereo_scene_math_test.cpp
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.cpp"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_math.h"
            "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
        )
        target_link_libraries(
            WorldAtWarVRStereoSceneMathTest
            PRIVATE wawvr_project_options
        )
        target_include_directories(
            WorldAtWarVRStereoSceneMathTest
            PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
        )
        add_test(
            NAME mod.stereo_scene_math
            COMMAND WorldAtWarVRStereoSceneMathTest
        )

        if(TARGET wawvr_t4)
            add_executable(
                WorldAtWarVRInputMappingTest
                camera_comfort_logic.cpp
                camera_comfort_logic.hpp
                controller_state.cpp
                controller_state.hpp
                input_mapping.cpp
                input_mapping.hpp
                input_mapping_test.cpp
                "${CMAKE_SOURCE_DIR}/src/xr/xr_math.cpp"
                "${CMAKE_SOURCE_DIR}/src/xr/xr_math.h"
                "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
            )
            target_link_libraries(
                WorldAtWarVRInputMappingTest
                PRIVATE wawvr_project_options wawvr_t4
            )
            target_include_directories(
                WorldAtWarVRInputMappingTest
                PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
            )
            add_test(
                NAME mod.input_mapping
                COMMAND WorldAtWarVRInputMappingTest
            )

            add_executable(
                WorldAtWarVRWeaponPlacementTest
                controller_state.cpp
                controller_state.hpp
                weapon_placement.cpp
                weapon_placement.hpp
                weapon_placement_test.cpp
                "${CMAKE_SOURCE_DIR}/src/xr/xr_math.cpp"
                "${CMAKE_SOURCE_DIR}/src/xr/xr_math.h"
                "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
            )
            target_link_libraries(
                WorldAtWarVRWeaponPlacementTest
                PRIVATE wawvr_project_options wawvr_t4
            )
            target_include_directories(
                WorldAtWarVRWeaponPlacementTest
                PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
            )
            add_test(
                NAME mod.weapon_placement
                COMMAND WorldAtWarVRWeaponPlacementTest
            )

            add_executable(
                WorldAtWarVRWeaponBallisticsLogicTest
                weapon_ballistics_logic.cpp
                weapon_ballistics_logic.hpp
                weapon_ballistics_logic_test.cpp
                "${CMAKE_SOURCE_DIR}/src/xr/xr_types.h"
            )
            target_link_libraries(
                WorldAtWarVRWeaponBallisticsLogicTest
                PRIVATE wawvr_project_options
            )
            target_include_directories(
                WorldAtWarVRWeaponBallisticsLogicTest
                PRIVATE "${CMAKE_SOURCE_DIR}/src/xr"
            )
            add_test(
                NAME mod.weapon_ballistics_logic
                COMMAND WorldAtWarVRWeaponBallisticsLogicTest
            )

            add_executable(
                WorldAtWarVRViewmodelFilterTest
                viewmodel_filter.cpp
                viewmodel_filter.hpp
                viewmodel_filter_test.cpp
            )
            target_link_libraries(
                WorldAtWarVRViewmodelFilterTest
                PRIVATE wawvr_project_options
            )
            add_test(
                NAME mod.viewmodel_filter
                COMMAND WorldAtWarVRViewmodelFilterTest
            )
        endif()
    endif()
endif()
