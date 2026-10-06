cmake_minimum_required(VERSION 3.24)

project(XRFrameGenerationBridge VERSION 0.223.0 LANGUAGES CXX)

if(MSVC)
    # This static layer is linked into OptiScaler and injected into hosts that
    # can preload an older game-local MSVCP140.dll. Initialize mutex/CV storage
    # through that loaded runtime instead of relying on the newer toolset's
    # constexpr representation.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

# V23043 embeds the unchanged layer implementation in the OptiScaler module.
set(XRFG_TEST_LAYER_DLL "" CACHE FILEPATH "Combined DLL tested by the layer harnesses")
set(XRFG_LAYER_KIND STATIC)
set(XRFG_COMPARISON_ROUTE "standard" CACHE STRING "standard or djules comparison runtime")
set_property(CACHE XRFG_COMPARISON_ROUTE PROPERTY STRINGS standard djules)
if(XRFG_COMPARISON_ROUTE STREQUAL "djules")
    set(XRFG_LAYER_SOURCE variants/djules/openxr_layer.cpp)
    set(XRFG_SYNTHESIS_SOURCE variants/djules/d3d12_frame_synthesizer.cpp)
    set(XRFG_SYNTHESIS_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/variants/djules/frame_synthesis.hlsl")
    add_compile_definitions(OFXR_DJULES_COMPARISON=1)
elseif(XRFG_COMPARISON_ROUTE STREQUAL "standard")
    set(XRFG_LAYER_SOURCE src/layer/openxr_layer.cpp)
    set(XRFG_SYNTHESIS_SOURCE src/d3d12/d3d12_frame_synthesizer.cpp)
    set(XRFG_SYNTHESIS_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl")
else()
    message(FATAL_ERROR "Unknown XRFG_COMPARISON_ROUTE")
endif()


option(XRFG_BUILD_LAYER "Build the Windows OpenXR API layer" ON)
option(XRFG_BUILD_TESTS "Build the core test executable" ON)
option(XRFG_BUILD_STANDALONE "Build the Windows standalone tray launcher" OFF)

if(XRFG_BUILD_STANDALONE)
    message(FATAL_ERROR "The embedded target has no standalone DLL/tray. Use the OFXR repository for that product.")
endif()

set(XRFG_OPENXR_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/OpenXR-SDK"
    CACHE PATH
    "Path to Khronos OpenXR-SDK containing include/openxr/openxr.h")

set(XRFG_FIDELITYFX_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/FidelityFX-SDK-v1.1.4/sdk"
    CACHE PATH
    "Path to the pinned AMD FidelityFX SDK v1.1.4 source tree")

set(XRFG_NVIDIA_OPTICAL_FLOW_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7"
    CACHE PATH
    "Path to the NVIDIA Optical Flow SDK 5.0.7 root")

set(XRFG_FIDELITYFX_API_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../external/FidelityFX-SDK/ffx-api/include/ffx_api"
    CACHE PATH
    "Path to the modular FidelityFX API headers used by FSR frame generation")

add_library(xrfg_core STATIC
    src/core/fps_overlay_model.cpp
    src/core/image.cpp
    src/core/optical_flow.cpp
    src/core/pose.cpp)
target_include_directories(xrfg_core PUBLIC include)
target_compile_features(xrfg_core PUBLIC cxx_std_20)

add_executable(xrfg_reference src/reference/main.cpp)
target_link_libraries(xrfg_reference PRIVATE xrfg_core)

# Separate-process consumer for V30006's RAM-only frame cadence capture.
# The injected layer never performs a disk write or starts a worker thread.
add_executable(xrfg_pose_cadence_watch
    src/reference/pose_cadence_watch.cpp
    src/common/ram_timing.cpp)
target_include_directories(xrfg_pose_cadence_watch PRIVATE include)
target_compile_features(xrfg_pose_cadence_watch PRIVATE cxx_std_20)

if(MSVC)
    foreach(target xrfg_core xrfg_reference xrfg_pose_cadence_watch)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endforeach()
endif()

if(XRFG_BUILD_TESTS)
    enable_testing()
    if(WIN32)
        add_executable(xrfg_streamline_native_interface_tests tests/streamline_native_interface_tests.cpp)
        target_link_libraries(xrfg_streamline_native_interface_tests PRIVATE d3d12)
        target_include_directories(xrfg_streamline_native_interface_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../OptiScaler/hooks")
        target_compile_features(xrfg_streamline_native_interface_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_streamline_native_interface_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(xrfg_streamline_native_interface_tests PRIVATE /W4 /permissive- /EHsc)
        endif()
        add_test(NAME xrfg_streamline_native_interface_tests COMMAND xrfg_streamline_native_interface_tests)
    endif()
    add_executable(xrfg_two_output_schedule_tests tests/two_output_schedule_tests.cpp)
    target_include_directories(xrfg_two_output_schedule_tests PRIVATE include)
    target_compile_features(xrfg_two_output_schedule_tests PRIVATE cxx_std_20)
    add_test(NAME xrfg_two_output_schedule_tests COMMAND xrfg_two_output_schedule_tests)
    add_executable(xrfg_midpoint_schedule_tests tests/midpoint_schedule_tests.cpp)
    target_include_directories(xrfg_midpoint_schedule_tests PRIVATE include)
    target_compile_features(xrfg_midpoint_schedule_tests PRIVATE cxx_std_20)
    add_test(NAME xrfg_midpoint_schedule_tests COMMAND xrfg_midpoint_schedule_tests)
    add_executable(xrfg_luke_color_token_tests tests/luke_color_token_tests.cpp)
    target_include_directories(xrfg_luke_color_token_tests PRIVATE include)
    target_compile_features(xrfg_luke_color_token_tests PRIVATE cxx_std_20)
    add_test(NAME xrfg_luke_color_token_tests COMMAND xrfg_luke_color_token_tests)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../tests/ofxr_host_tests.cmake")
    add_executable(xrfg_fsr_cadence_probe_tests tests/fsr_cadence_probe_tests.cpp)
    target_include_directories(xrfg_fsr_cadence_probe_tests PRIVATE include)
    target_compile_features(xrfg_fsr_cadence_probe_tests PRIVATE cxx_std_20)
    target_compile_definitions(xrfg_fsr_cadence_probe_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME xrfg_fsr_cadence_probe_tests COMMAND xrfg_fsr_cadence_probe_tests)
    add_executable(xrfg_three_slot_buffer_tests tests/three_slot_buffer_tests.cpp)
    target_include_directories(xrfg_three_slot_buffer_tests PRIVATE include)
    target_compile_features(xrfg_three_slot_buffer_tests PRIVATE cxx_std_20)
    add_test(NAME xrfg_three_slot_buffer_tests COMMAND xrfg_three_slot_buffer_tests)
    add_executable(xrfg_luke_counter_probe_tests tests/luke_counter_probe_tests.cpp)
    target_include_directories(xrfg_luke_counter_probe_tests PRIVATE include)
    target_compile_features(xrfg_luke_counter_probe_tests PRIVATE cxx_std_20)
    add_test(NAME xrfg_luke_counter_probe_tests COMMAND xrfg_luke_counter_probe_tests)
    add_executable(xrfg_core_tests tests/core_tests.cpp)
    target_link_libraries(xrfg_core_tests PRIVATE xrfg_core)
    if(MSVC)
        target_compile_options(xrfg_core_tests PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(xrfg_core_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME xrfg_core_tests COMMAND xrfg_core_tests)
    if(WIN32)
        add_executable(xrfg_luke_world_scale_config_tests tests/luke_world_scale_config_tests.cpp)
        target_include_directories(xrfg_luke_world_scale_config_tests PRIVATE include)
        target_compile_features(xrfg_luke_world_scale_config_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_luke_world_scale_config_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME xrfg_luke_world_scale_config COMMAND xrfg_luke_world_scale_config_tests)
    endif()
    add_executable(xrfg_fps_overlay_tests tests/fps_overlay_tests.cpp)
    target_link_libraries(xrfg_fps_overlay_tests PRIVATE xrfg_core)
    add_test(NAME xrfg_fps_overlay_tests COMMAND xrfg_fps_overlay_tests)

    add_executable(xrfg_optical_flow_tests tests/optical_flow_tests.cpp)
    target_link_libraries(xrfg_optical_flow_tests PRIVATE xrfg_core)
    if(MSVC)
        target_compile_options(xrfg_optical_flow_tests PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(xrfg_optical_flow_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME xrfg_optical_flow_tests COMMAND xrfg_optical_flow_tests)
endif()

if(WIN32 AND XRFG_BUILD_LAYER)
    if(NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_platform_defines.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_loader_negotiation.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_platform.h")
        message(FATAL_ERROR
            "XRFG_OPENXR_SDK_ROOT must point to the required Khronos OpenXR-SDK generated headers")
    endif()

    find_program(XRFG_FXC_EXECUTABLE fxc.exe
        HINTS
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
    if(NOT XRFG_FXC_EXECUTABLE)
        message(FATAL_ERROR "fxc.exe from the Windows SDK is required for frame-synthesis shaders")
    endif()

    set(XRFG_FIDELITYFX_INCLUDE_DIR
        "${XRFG_FIDELITYFX_SDK_ROOT}/include")
    set(XRFG_FIDELITYFX_LIBRARY_DIR
        "${XRFG_FIDELITYFX_SDK_ROOT}/bin/ffx_sdk")
    set(XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY
        "${XRFG_FIDELITYFX_LIBRARY_DIR}/ofxr_ffx_opticalflow_x64.lib")
    set(XRFG_FIDELITYFX_DX12_LIBRARY
        "${XRFG_FIDELITYFX_LIBRARY_DIR}/ofxr_ffx_backend_dx12_x64.lib")
    if(NOT EXISTS "${XRFG_FIDELITYFX_INCLUDE_DIR}/FidelityFX/host/ffx_opticalflow.h" OR
       NOT EXISTS "${XRFG_FIDELITYFX_INCLUDE_DIR}/FidelityFX/host/backends/dx12/ffx_dx12.h" OR
       NOT EXISTS "${XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY}" OR
       NOT EXISTS "${XRFG_FIDELITYFX_DX12_LIBRARY}")
        message(FATAL_ERROR
            "Pinned FidelityFX SDK v1.1.4 headers and Release DX12/Optical Flow libraries are required")
    endif()

    set(XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR
        "${XRFG_NVIDIA_OPTICAL_FLOW_SDK_ROOT}/NvOFInterface")
    if(NOT EXISTS "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}/nvOpticalFlowCommon.h" OR
       NOT EXISTS "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}/nvOpticalFlowD3D12.h")
        message(FATAL_ERROR
            "NVIDIA Optical Flow SDK 5.0.7 interface headers are required")
    endif()

    set(XRFG_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
    set(XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/game_motion_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_PACK_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/pack_flow_input_shader.hpp")
    set(XRFG_FULLSCREEN_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/fullscreen_vertex_shader.hpp")
    set(XRFG_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_REPROJECT_GENERATED_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/reproject_generated_pixel_shader.hpp")
    set(XRFG_REPROJECT_LATE_HUD_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/reproject_late_hud_pixel_shader.hpp")
    set(XRFG_FSR_CINEMA_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/fsr_cinema_vertex_shader.hpp")
    set(XRFG_FSR_CINEMA_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/fsr_cinema_pixel_shader.hpp")
    set(XRFG_CINEMA_FLATTEN_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/cinema_flatten_vertex_shader.hpp")
    set(XRFG_CINEMA_FLATTEN_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/cinema_flatten_pixel_shader.hpp")
    set(XRFG_NVIDIA_LUMA_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_pack_luma_input_shader.hpp")
    set(XRFG_NVIDIA_PACK_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_pack_flow_input_shader.hpp")
    set(XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_bidirectional_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_fast_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_LUKE_DEPTH_CONVERT_HEADER "${XRFG_GENERATED_SHADER_DIR}/luke_depth_convert_shader.hpp")
    set(XRFG_LUKE_COMPOSE_MOTION_HEADER "${XRFG_GENERATED_SHADER_DIR}/luke_compose_motion_shader.hpp")
    add_custom_command(
        OUTPUT
            "${XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_PACK_INPUT_HEADER}"
            "${XRFG_FULLSCREEN_VERTEX_HEADER}"
            "${XRFG_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_REPROJECT_GENERATED_PIXEL_HEADER}"
            "${XRFG_REPROJECT_LATE_HUD_PIXEL_HEADER}"
            "${XRFG_FSR_CINEMA_VERTEX_HEADER}"
            "${XRFG_FSR_CINEMA_PIXEL_HEADER}"
            "${XRFG_CINEMA_FLATTEN_VERTEX_HEADER}"
            "${XRFG_CINEMA_FLATTEN_PIXEL_HEADER}"
            "${XRFG_NVIDIA_PACK_INPUT_HEADER}"
            "${XRFG_NVIDIA_LUMA_INPUT_HEADER}"
            "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_LUKE_DEPTH_CONVERT_HEADER}"
            "${XRFG_LUKE_COMPOSE_MOTION_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${XRFG_GENERATED_SHADER_DIR}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeGameMotionMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_game_motion_synthesize_midpoint_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackFlowInput
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_PACK_INPUT_HEADER}" /Vn g_xrfg_pack_flow_input_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E FullscreenTriangleVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_FULLSCREEN_VERTEX_HEADER}" /Vn g_xrfg_fullscreen_vertex_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_synthesize_midpoint_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E ReprojectGeneratedPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_REPROJECT_GENERATED_PIXEL_HEADER}" /Vn g_xrfg_reproject_generated_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E ReprojectGeneratedWithLateHudPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_REPROJECT_LATE_HUD_PIXEL_HEADER}" /Vn g_xrfg_reproject_late_hud_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E CinemaPanelVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_FSR_CINEMA_VERTEX_HEADER}" /Vn g_xrfg_fsr_cinema_vertex_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/fsr_cinema_projection.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E CinemaPanelPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_FSR_CINEMA_PIXEL_HEADER}" /Vn g_xrfg_fsr_cinema_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/fsr_cinema_projection.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackNvidiaFlowInput
            /D XRFG_NVIDIA_LUMA_INPUT=1 /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_LUMA_INPUT_HEADER}" /Vn g_xrfg_nvidia_pack_luma_input_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackNvidiaFlowInput
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_PACK_INPUT_HEADER}" /Vn g_xrfg_nvidia_pack_flow_input_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E CinemaFlattenVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_CINEMA_FLATTEN_VERTEX_HEADER}" /Vn g_xrfg_cinema_flatten_vertex_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/fsr_cinema_projection.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E CinemaFlattenPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_CINEMA_FLATTEN_PIXEL_HEADER}" /Vn g_xrfg_cinema_flatten_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/fsr_cinema_projection.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_synthesize_midpoint_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaBidirectionalMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_bidirectional_synthesize_midpoint_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaFastMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_fast_synthesize_midpoint_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaFastBidirectionalMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader
            "${XRFG_SYNTHESIS_SHADER}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E ConvertLukeDepth
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_LUKE_DEPTH_CONVERT_HEADER}" /Vn g_xrfg_luke_depth_convert_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/luke_depth_convert.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E ComposeLukeSameEyeMotion
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_LUKE_COMPOSE_MOTION_HEADER}" /Vn g_xrfg_luke_compose_motion_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/luke_compose_motion.hlsl"
        DEPENDS
            "${XRFG_SYNTHESIS_SHADER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/fsr_cinema_projection.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/luke_depth_convert.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/luke_compose_motion.hlsl"
        VERBATIM)

    add_library(xrfg_d3d12_history STATIC
        "${XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER}"
        src/common/dlss_motion_vectors.cpp
        src/common/two_frame_capture.cpp
        src/common/fsr_frame_generation.cpp
        src/d3d11/d3d11_d3d12_interop.cpp
        src/d3d11/d3d11_bridge.cpp
        src/vulkan/vulkan_session_bridge.cpp
        src/d3d11/d3d11_color_identity.cpp
        src/d3d12/d3d12_history.cpp
        src/d3d12/d3d12_fsr_frame_generator.cpp
        ${XRFG_SYNTHESIS_SOURCE}
        src/d3d12/luke_afw.cpp
        src/d3d12/luke_color_origin.cpp
        "${XRFG_PACK_INPUT_HEADER}"
        "${XRFG_FULLSCREEN_VERTEX_HEADER}"
        "${XRFG_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_REPROJECT_GENERATED_PIXEL_HEADER}"
        "${XRFG_REPROJECT_LATE_HUD_PIXEL_HEADER}"
        "${XRFG_FSR_CINEMA_VERTEX_HEADER}"
        "${XRFG_FSR_CINEMA_PIXEL_HEADER}"
        "${XRFG_NVIDIA_PACK_INPUT_HEADER}"
        "${XRFG_NVIDIA_LUMA_INPUT_HEADER}"
        "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_LUKE_DEPTH_CONVERT_HEADER}"
        "${XRFG_LUKE_COMPOSE_MOTION_HEADER}")
    target_include_directories(xrfg_d3d12_history
        PUBLIC include
        PRIVATE
            "${XRFG_GENERATED_SHADER_DIR}"
            "${XRFG_FIDELITYFX_INCLUDE_DIR}"
            "${XRFG_FIDELITYFX_API_INCLUDE_DIR}"
            "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}")
    set(XRFG_VULKAN_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/vulkan/include")
    if(NOT EXISTS "${XRFG_VULKAN_INCLUDE_DIR}/vulkan/vk_layer.h")
        message(FATAL_ERROR "Initialize the pinned OptiScaler Vulkan-Headers submodule")
    endif()
    target_include_directories(xrfg_d3d12_history PUBLIC "${XRFG_VULKAN_INCLUDE_DIR}")
    add_library(OFXR_vulkan_queue_layer SHARED src/vulkan_layer/vulkan_queue_layer.cpp)
    target_include_directories(OFXR_vulkan_queue_layer PRIVATE "${XRFG_VULKAN_INCLUDE_DIR}")
    target_compile_features(OFXR_vulkan_queue_layer PRIVATE cxx_std_20)
    target_compile_definitions(OFXR_vulkan_queue_layer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VK_NO_PROTOTYPES)
    if(MSVC)
        target_compile_options(OFXR_vulkan_queue_layer PRIVATE /W4 /permissive- /EHsc)
    endif()
    target_compile_features(xrfg_d3d12_history PUBLIC cxx_std_20)
    target_compile_options(xrfg_d3d12_history PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/ffx_private_symbols.h")
    target_link_libraries(xrfg_d3d12_history PUBLIC
        xrfg_core
        d3d11
        d3dcompiler
        d3d12
        dxgi
        "${XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY}"
        "${XRFG_FIDELITYFX_DX12_LIBRARY}")
    target_compile_definitions(xrfg_d3d12_history PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(xrfg_d3d12_history PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(xrfg_bridge_flight_logger STATIC
        src/layer/bridge_flight_logger.cpp
        src/layer/luke_counter_probe.cpp
        src/common/ram_timing.cpp
        include/xrfg/bridge_flight_logger.hpp)
    add_library(xrfg_frame_capture STATIC src/d3d11/frame_capture.cpp)
    target_include_directories(xrfg_frame_capture PUBLIC include)
    target_compile_features(xrfg_frame_capture PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_frame_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(xrfg_frame_capture PUBLIC d3d11 d3dcompiler user32)
    target_include_directories(xrfg_bridge_flight_logger PUBLIC include)
    target_link_libraries(xrfg_d3d12_history PUBLIC xrfg_bridge_flight_logger)
    target_compile_features(xrfg_bridge_flight_logger PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_bridge_flight_logger PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XRFG_IMPLEMENTATION_VERSION=${PROJECT_VERSION_MINOR})
    if(MSVC)
        target_compile_options(xrfg_bridge_flight_logger PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(xrfg_implicit_layer STATIC
        src/common/embedded_control.cpp
        src/common/implicit_layer.cpp
        include/xrfg/implicit_layer.hpp)
    target_include_directories(xrfg_implicit_layer PUBLIC include)
    target_compile_features(xrfg_implicit_layer PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_implicit_layer PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
    target_link_libraries(xrfg_implicit_layer PUBLIC advapi32)
    if(MSVC)
        target_compile_options(xrfg_implicit_layer PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(xrfg_steamvr_delivery STATIC src/layer/steamvr_delivery.cpp)
    target_include_directories(xrfg_steamvr_delivery PUBLIC include PRIVATE external/OpenVR/include)
    target_compile_features(xrfg_steamvr_delivery PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_steamvr_delivery PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(xrfg_steamvr_delivery PUBLIC xrfg_bridge_flight_logger)

    add_library(xrfg_fps_overlay STATIC src/layer/openxr_fps_overlay.cpp)
    target_include_directories(xrfg_fps_overlay PUBLIC include "${XRFG_OPENXR_SDK_ROOT}/include")
    target_compile_features(xrfg_fps_overlay PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_fps_overlay PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
        XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_PLATFORM_WIN32)
    target_link_libraries(xrfg_fps_overlay PUBLIC xrfg_core xrfg_steamvr_delivery d3d11 d3d12 dxgi)
    if(MSVC)
        target_compile_options(xrfg_fps_overlay PRIVATE /W4 /permissive- /EHsc)
    endif()
    if(XRFG_BUILD_TESTS)
        add_executable(xrfg_openxr_fps_overlay_tests tests/openxr_fps_overlay_tests.cpp)
        target_link_libraries(xrfg_openxr_fps_overlay_tests PRIVATE xrfg_fps_overlay)
        add_test(NAME xrfg_openxr_fps_overlay_d3d12 COMMAND xrfg_openxr_fps_overlay_tests)
        add_test(NAME xrfg_openxr_fps_overlay_d3d11 COMMAND xrfg_openxr_fps_overlay_tests d3d11)
    endif()

    add_library(XR_APILAYER_XRFrameBridge_diagnostic ${XRFG_LAYER_KIND}
        ${XRFG_LAYER_SOURCE})
    target_include_directories(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        include
        "${XRFG_OPENXR_SDK_ROOT}/include")
    target_link_libraries(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        xrfg_core
        xrfg_d3d12_history
        xrfg_bridge_flight_logger
        xrfg_fps_overlay
        xrfg_implicit_layer
        psapi xrfg_frame_capture)
    target_compile_features(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE cxx_std_20)
    target_compile_definitions(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XR_USE_GRAPHICS_API_D3D11
        XR_USE_GRAPHICS_API_D3D12
        XR_USE_PLATFORM_WIN32)
    if(MSVC)
        target_compile_options(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_custom_command(TARGET XR_APILAYER_XRFrameBridge_diagnostic POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/layer/ofxr_bridge.ini"
            "$<TARGET_FILE_DIR:XR_APILAYER_XRFrameBridge_diagnostic>/ofxr_bridge.ini")

    if(XRFG_BUILD_STANDALONE)
        add_library(xrfg_standalone_core STATIC
            src/tray/standalone_launcher.cpp
            src/common/ram_timing.cpp
            include/xrfg/standalone_launcher.hpp)
        target_include_directories(xrfg_standalone_core PUBLIC include)
        target_compile_features(xrfg_standalone_core PUBLIC cxx_std_20)
        target_link_libraries(xrfg_standalone_core PUBLIC xrfg_implicit_layer xrfg_core)
        target_compile_definitions(xrfg_standalone_core PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE)
        if(MSVC)
            target_compile_options(xrfg_standalone_core PRIVATE
                /W4 /permissive- /EHsc)
        endif()

        enable_language(RC)
        add_executable(OFXRBridgeTray WIN32
            src/tray/main.cpp
            src/tray/resources.rc)
        target_link_libraries(OFXRBridgeTray PRIVATE
            xrfg_standalone_core
            comctl32
            ole32
            shell32
            user32)
        target_compile_features(OFXRBridgeTray PRIVATE cxx_std_20)
        target_compile_definitions(OFXRBridgeTray PRIVATE
            XRFG_IMPLEMENTATION_VERSION=${PROJECT_VERSION_MINOR}
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE)
        if(MSVC)
            target_compile_options(OFXRBridgeTray PRIVATE
                /W4 /permissive- /EHsc)
        endif()
        add_dependencies(OFXRBridgeTray
            XR_APILAYER_XRFrameBridge_diagnostic)
        add_custom_command(TARGET OFXRBridgeTray POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${XRFG_TEST_LAYER_DLL}"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE_DIR:XR_APILAYER_XRFrameBridge_diagnostic>/ofxr_bridge.ini"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/ofxr_bridge.ini")

        if(XRFG_BUILD_TESTS)
            add_executable(xrfg_standalone_launcher_tests
                tests/standalone_launcher_tests.cpp)
            target_link_libraries(xrfg_standalone_launcher_tests PRIVATE
                xrfg_standalone_core)
            target_compile_features(xrfg_standalone_launcher_tests PRIVATE
                cxx_std_20)
            if(MSVC)
                target_compile_options(xrfg_standalone_launcher_tests PRIVATE
                    /W4 /permissive- /EHsc)
                target_compile_definitions(xrfg_standalone_launcher_tests PRIVATE
                    NOMINMAX
                    WIN32_LEAN_AND_MEAN
                    UNICODE
                    _UNICODE)
            endif()
            add_test(NAME xrfg_standalone_launcher_tests
                COMMAND xrfg_standalone_launcher_tests)
            add_executable(xrfg_tray_lifecycle_tests tests/tray_lifecycle_tests.cpp)
            target_link_libraries(xrfg_tray_lifecycle_tests PRIVATE
                xrfg_standalone_core comctl32 ole32 shell32 user32)
            target_compile_definitions(xrfg_tray_lifecycle_tests PRIVATE
                XRFG_IMPLEMENTATION_VERSION=${PROJECT_VERSION_MINOR}
                NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
            if(MSVC)
                target_compile_options(xrfg_tray_lifecycle_tests PRIVATE /W4 /permissive- /EHsc)
            endif()
            add_test(NAME xrfg_tray_lifecycle_tests COMMAND xrfg_tray_lifecycle_tests)
        endif()
    endif()

    if(XRFG_BUILD_TESTS)
        add_executable(xrfg_bridge_flight_logger_tests
            tests/bridge_flight_logger_tests.cpp)
        target_link_libraries(xrfg_bridge_flight_logger_tests PRIVATE
            xrfg_bridge_flight_logger)
        target_compile_features(xrfg_bridge_flight_logger_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_bridge_flight_logger_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_bridge_flight_logger_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_bridge_flight_logger_tests
                COMMAND xrfg_bridge_flight_logger_tests)
        add_executable(xrfg_ram_timing_tests tests/ram_timing_tests.cpp)
        target_link_libraries(xrfg_ram_timing_tests PRIVATE xrfg_bridge_flight_logger)
        target_compile_features(xrfg_ram_timing_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_ram_timing_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME xrfg_ram_timing_tests COMMAND xrfg_ram_timing_tests)
        add_test(NAME xrfg_ram_timing_disabled COMMAND xrfg_ram_timing_tests disabled)
        set_tests_properties(xrfg_ram_timing_tests PROPERTIES TIMEOUT 30)

        add_executable(xrfg_frame_capture_tests tests/frame_capture_tests.cpp)
        target_link_libraries(xrfg_frame_capture_tests PRIVATE xrfg_frame_capture)
        target_compile_definitions(xrfg_frame_capture_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME xrfg_frame_capture_tests COMMAND xrfg_frame_capture_tests)
        set_tests_properties(xrfg_frame_capture_tests PROPERTIES TIMEOUT 30)

        add_executable(xrfg_three_slot_director_tests
            tests/three_slot_director_tests.cpp
            src/common/embedded_control.cpp)
        add_executable(xrfg_d3d11_bridge_tests tests/d3d11_bridge_tests.cpp)
        target_link_libraries(xrfg_d3d11_bridge_tests PRIVATE xrfg_d3d12_history)
        target_compile_definitions(xrfg_d3d11_bridge_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME xrfg_d3d11_bridge_tests COMMAND xrfg_d3d11_bridge_tests)
        set_tests_properties(xrfg_d3d11_bridge_tests PROPERTIES SKIP_RETURN_CODE 77)
        add_executable(xrfg_d3d11_color_identity_tests tests/d3d11_color_identity_tests.cpp)
        add_executable(xrfg_flow_input_encoding_tests tests/flow_input_encoding_tests.cpp)
        target_include_directories(xrfg_flow_input_encoding_tests PRIVATE
            include "${XRFG_GENERATED_SHADER_DIR}")
        target_compile_features(xrfg_flow_input_encoding_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_flow_input_encoding_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(xrfg_flow_input_encoding_tests PRIVATE d3d12 dxgi)
        add_dependencies(xrfg_flow_input_encoding_tests xrfg_d3d12_history)
        add_test(NAME xrfg_flow_input_encoding COMMAND xrfg_flow_input_encoding_tests)
        set_tests_properties(xrfg_flow_input_encoding PROPERTIES TIMEOUT 30)
        add_executable(xrfg_luke_color_origin_tests tests/luke_color_origin_tests.cpp src/d3d12/luke_color_origin.cpp)
        target_include_directories(xrfg_luke_color_origin_tests PRIVATE include)
        target_compile_features(xrfg_luke_color_origin_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_luke_color_origin_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(xrfg_luke_color_origin_tests PRIVATE d3d12 dxgi xrfg_bridge_flight_logger)
        add_test(NAME xrfg_luke_color_origin COMMAND xrfg_luke_color_origin_tests)
        set_tests_properties(xrfg_luke_color_origin PROPERTIES TIMEOUT 30)
        target_link_libraries(xrfg_d3d11_color_identity_tests PRIVATE xrfg_d3d12_history)
        target_compile_features(xrfg_d3d11_color_identity_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_d3d11_color_identity_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME xrfg_d3d11_color_identity COMMAND xrfg_d3d11_color_identity_tests)
        set_tests_properties(xrfg_d3d11_color_identity PROPERTIES TIMEOUT 30)
        target_include_directories(xrfg_three_slot_director_tests PRIVATE
            include "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_three_slot_director_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_three_slot_director_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN XR_USE_GRAPHICS_API_D3D11
            XR_USE_GRAPHICS_API_D3D12 XR_USE_PLATFORM_WIN32 OFXR_PROVIDER_API_EXPORTS)
        target_link_libraries(xrfg_three_slot_director_tests PRIVATE
            xrfg_core xrfg_d3d12_history xrfg_bridge_flight_logger
            xrfg_fps_overlay xrfg_implicit_layer psapi xrfg_frame_capture)
        add_test(NAME xrfg_three_slot_director_tests COMMAND xrfg_three_slot_director_tests)
        add_test(NAME xrfg_depth_history_routes COMMAND xrfg_three_slot_director_tests depth-history)
        add_test(NAME xrfg_vulkan_bridge_contract COMMAND xrfg_three_slot_director_tests vulkan-contract)
        if(XRFG_COMPARISON_ROUTE STREQUAL "djules")
            add_test(NAME xrfg_djules_binding_queue COMMAND xrfg_three_slot_director_tests binding-queue)
            add_test(NAME xrfg_djules_display_clock COMMAND xrfg_three_slot_director_tests display-clock)
            add_test(NAME xrfg_djules_absolute_grid COMMAND xrfg_three_slot_director_tests absolute-grid)
            add_test(NAME xrfg_djules_triple_buffer COMMAND xrfg_three_slot_director_tests triple-buffer)
            add_test(NAME xrfg_djules_triple_inline_gpu COMMAND xrfg_three_slot_d3d12_tests triple-inline)
            add_test(NAME xrfg_djules_triple_director_gpu COMMAND xrfg_three_slot_d3d12_tests triple-core)
            add_test(NAME xrfg_djules_triple_switch_gpu COMMAND xrfg_three_slot_d3d12_tests triple-switch)
            add_test(NAME xrfg_djules_deep_readiness_gpu COMMAND xrfg_three_slot_d3d12_tests deep-readiness)
            add_test(NAME xrfg_djules_deep_readiness COMMAND xrfg_three_slot_director_tests deep-readiness)
            add_test(NAME xrfg_djules_inline_policy COMMAND xrfg_three_slot_director_tests inline-policy)
            add_test(NAME xrfg_djules_phase_acquisition COMMAND xrfg_three_slot_director_tests phase-acquisition)
            set_tests_properties(xrfg_djules_binding_queue PROPERTIES TIMEOUT 15)
        endif()
        add_test(NAME xrfg_luke_admission COMMAND xrfg_three_slot_director_tests luke-unit)
        add_test(NAME xrfg_luke_inline_diagnostic COMMAND xrfg_three_slot_director_tests luke-inline-diagnostic)
        add_test(NAME xrfg_luke_afw_output_slot COMMAND xrfg_three_slot_director_tests luke-afw-slot)
        add_test(NAME xrfg_luke_post_afw_director_packet COMMAND xrfg_three_slot_director_tests post-afw-packet)
        set_tests_properties(xrfg_luke_post_afw_director_packet PROPERTIES TIMEOUT 30)
        set_tests_properties(xrfg_luke_afw_output_slot PROPERTIES TIMEOUT 30)
        add_test(NAME xrfg_luke_cycle_clock COMMAND xrfg_three_slot_director_tests luke-clock)
        set_tests_properties(xrfg_luke_cycle_clock PROPERTIES TIMEOUT 10)
        add_test(NAME xrfg_luke_gpu_pipeline COMMAND xrfg_three_slot_director_tests luke-gpu)
        add_test(NAME xrfg_luke_ram_pipeline COMMAND xrfg_three_slot_director_tests luke-ram)
        set_tests_properties(xrfg_luke_admission xrfg_luke_gpu_pipeline xrfg_luke_ram_pipeline PROPERTIES TIMEOUT 30)
        add_test(NAME xrfg_three_slot_composition_tests COMMAND xrfg_three_slot_director_tests composition)
        add_test(NAME xrfg_three_slot_capture_integration COMMAND xrfg_three_slot_director_tests capture)
        set_tests_properties(xrfg_three_slot_capture_integration PROPERTIES TIMEOUT 60)
        set_tests_properties(xrfg_three_slot_composition_tests PROPERTIES TIMEOUT 60)
        add_test(NAME xrfg_three_slot_transition_tests COMMAND xrfg_three_slot_director_tests transition)
        set_tests_properties(xrfg_three_slot_transition_tests PROPERTIES TIMEOUT 60)
        set_tests_properties(xrfg_three_slot_director_tests PROPERTIES TIMEOUT 20)
        add_test(NAME xrfg_three_slot_nvidia_d3d11_pipeline
              COMMAND xrfg_three_slot_director_tests gpu)
        add_test(NAME xrfg_ram_timing_d3d11 COMMAND xrfg_three_slot_director_tests ram)
        add_test(NAME xrfg_capture_gap_d3d11 COMMAND xrfg_three_slot_director_tests gap)
        set_tests_properties(xrfg_three_slot_nvidia_d3d11_pipeline PROPERTIES TIMEOUT 30)

        add_executable(xrfg_three_slot_d3d12_tests tests/three_slot_d3d12_tests.cpp
            src/common/embedded_control.cpp)
        target_include_directories(xrfg_three_slot_d3d12_tests PRIVATE include "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_three_slot_d3d12_tests PRIVATE cxx_std_20)
        target_compile_definitions(xrfg_three_slot_d3d12_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
            XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_PLATFORM_WIN32 OFXR_PROVIDER_API_EXPORTS)
        target_link_libraries(xrfg_three_slot_d3d12_tests PRIVATE xrfg_core xrfg_d3d12_history
            xrfg_bridge_flight_logger xrfg_fps_overlay xrfg_implicit_layer psapi xrfg_frame_capture)
        add_test(NAME xrfg_three_slot_d3d12_split COMMAND xrfg_three_slot_d3d12_tests)
        add_test(NAME xrfg_ram_timing_d3d12 COMMAND xrfg_three_slot_d3d12_tests ram)
        add_test(NAME xrfg_three_slot_d3d12_array COMMAND xrfg_three_slot_d3d12_tests array)
        add_test(NAME xrfg_legacy_nvidia_queue_gpu COMMAND xrfg_three_slot_d3d12_tests legacy-queue)
        add_test(NAME xrfg_legacy_nvidia_queue_vdxr_gpu COMMAND xrfg_three_slot_d3d12_tests legacy-queue-vdxr)
        add_test(NAME xrfg_legacy_nvidia_queue_slow_gpu COMMAND xrfg_three_slot_d3d12_tests legacy-queue-slow)
        add_test(NAME xrfg_legacy_nvidia_early_current_gpu COMMAND xrfg_three_slot_d3d12_tests legacy-queue-early-b)
        add_test(NAME xrfg_new_nvidia_midpoint_cadence_gpu COMMAND xrfg_three_slot_d3d12_tests midpoint-cadence)
        add_test(NAME xrfg_fsr_inline_missing_guides COMMAND xrfg_three_slot_d3d12_tests fsr-inline-wait)
        add_test(NAME xrfg_fsr_post_hud_contract
            COMMAND "${CMAKE_COMMAND}"
                "-DXRFG_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/fsr_post_hud_contract.cmake")
        if(EXISTS "${XRFG_FIDELITYFX_SDK_ROOT}/../PrebuiltSignedDLL/amd_fidelityfx_dx12.dll")
            add_custom_command(TARGET xrfg_three_slot_d3d12_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${XRFG_FIDELITYFX_SDK_ROOT}/../PrebuiltSignedDLL/amd_fidelityfx_dx12.dll"
                    "$<TARGET_FILE_DIR:xrfg_three_slot_d3d12_tests>/ofxr_amd_fidelityfx_framegeneration_dx12.dll")
            add_test(NAME xrfg_fsr_inline_gpu COMMAND xrfg_three_slot_d3d12_tests fsr-inline)
            add_test(NAME xrfg_fsr_steamvr_queue_gpu COMMAND xrfg_three_slot_d3d12_tests fsr-queue)
            add_test(NAME xrfg_fsr_steamvr_queue_slow_gpu COMMAND xrfg_three_slot_d3d12_tests fsr-queue-slow)
            add_test(NAME xrfg_fsr_steamvr_queue_switch_gpu COMMAND xrfg_three_slot_d3d12_tests fsr-queue-switch)
            add_test(NAME xrfg_fsr_steamvr_queue_pending_gpu COMMAND xrfg_three_slot_d3d12_tests fsr-queue-gpu-pending)
            set_tests_properties(xrfg_fsr_inline_gpu PROPERTIES TIMEOUT 60)
        endif()
        set_tests_properties(xrfg_fsr_inline_missing_guides PROPERTIES TIMEOUT 60)
        add_test(NAME xrfg_capture_gap_d3d12_split COMMAND xrfg_three_slot_d3d12_tests gap-split)
        add_test(NAME xrfg_capture_gap_d3d12_array COMMAND xrfg_three_slot_d3d12_tests gap-array)
        set_tests_properties(xrfg_capture_gap_d3d11 xrfg_capture_gap_d3d12_split xrfg_capture_gap_d3d12_array PROPERTIES TIMEOUT 30)
        add_test(NAME xrfg_three_slot_d3d12_repeated_eye COMMAND xrfg_three_slot_d3d12_tests repeat)
        add_test(NAME xrfg_three_slot_d3d12_promotion_split COMMAND xrfg_three_slot_d3d12_tests promotion-split)
        add_test(NAME xrfg_three_slot_d3d12_promotion_array COMMAND xrfg_three_slot_d3d12_tests promotion-array)
        set_tests_properties(xrfg_three_slot_d3d12_split xrfg_three_slot_d3d12_array
            xrfg_three_slot_d3d12_repeated_eye xrfg_three_slot_d3d12_promotion_split
            xrfg_three_slot_d3d12_promotion_array PROPERTIES TIMEOUT 60)

        add_executable(xrfg_layer_smoke tests/layer_smoke.cpp)
        target_include_directories(xrfg_layer_smoke PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_layer_smoke PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_layer_smoke PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_layer_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_test(
            NAME xrfg_layer_negotiation
            COMMAND xrfg_layer_smoke ${XRFG_TEST_LAYER_DLL})
        add_test(NAME xrfg_layer_process_exclusion COMMAND "${CMAKE_COMMAND}"
            "-DSMOKE=$<TARGET_FILE:xrfg_layer_smoke>" "-DLAYER=${XRFG_TEST_LAYER_DLL}"
            "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/process-exclusion-fixture"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_process_exclusion.cmake")

        add_executable(xrfg_layer_call_chain tests/layer_call_chain.cpp)
        target_include_directories(xrfg_layer_call_chain PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_layer_call_chain PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_layer_call_chain PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_layer_call_chain PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN
                XR_USE_GRAPHICS_API_D3D11
                XR_USE_GRAPHICS_API_D3D12
                XR_USE_PLATFORM_WIN32)
        endif()
        target_link_libraries(xrfg_layer_call_chain PRIVATE d3d11 d3d12 dxgi)
        foreach(mode inverted-fov d3d11-inverted-fov)
            add_test(NAME xrfg_layer_${mode} COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-${mode}.log" ${mode})
        endforeach()
        add_dependencies(xrfg_layer_call_chain XR_APILAYER_XRFrameBridge_diagnostic)
        add_executable(xrfg_layer_manual_disarm_tests tests/layer_manual_disarm_tests.cpp)
        target_include_directories(xrfg_layer_manual_disarm_tests PRIVATE "${XRFG_OPENXR_SDK_ROOT}/include")
        target_link_libraries(xrfg_layer_manual_disarm_tests PRIVATE xrfg_implicit_layer d3d11 d3d12 dxgi)
        target_compile_definitions(xrfg_layer_manual_disarm_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_PLATFORM_WIN32)
        if(MSVC)
            target_compile_options(xrfg_layer_manual_disarm_tests PRIVATE /W4 /permissive- /EHsc)
        endif()
        add_dependencies(xrfg_layer_manual_disarm_tests XR_APILAYER_XRFrameBridge_diagnostic)
        foreach(mode inline d3d11 steamvr flight)
            add_test(NAME xrfg_manual_disarm_${mode} COMMAND xrfg_layer_manual_disarm_tests
                ${XRFG_TEST_LAYER_DLL} ${mode})
            set_tests_properties(xrfg_manual_disarm_${mode} PROPERTIES TIMEOUT 30)
        endforeach()
        foreach(marker on off hidden)
            add_test(NAME xrfg_layer_marker_d3d11_${marker} COMMAND xrfg_layer_manual_disarm_tests
                ${XRFG_TEST_LAYER_DLL} d3d11 ${marker})
            set_tests_properties(xrfg_layer_marker_d3d11_${marker} PROPERTIES TIMEOUT 30)
        endforeach()
        add_test(
            NAME xrfg_layer_call_chain
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-call-chain-test.log")
        add_test(NAME xrfg_layer_acquire_ahead
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=${XRFG_TEST_LAYER_DLL}"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/d02-acquire-ahead"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_acquire_ahead.cmake")
        add_test(
            NAME xrfg_layer_presenter_swapchain_lifetime
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-presenter-swapchain-lifetime.log"
                flight-destroy-swapchain)
        add_test(
            NAME xrfg_layer_presenter_space_lifetime
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-presenter-space-lifetime.log"
                steamvr-destroy-space)

        add_test(
            NAME xrfg_layer_split_eye_call_chain
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-split-eye-call-chain-test.log"
                split-eye)

        add_test(
            NAME xrfg_layer_cropped_split_eye_call_chain
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-cropped-split-eye-call-chain-test.log"
                cropped-split-eye)

        add_test(
            NAME xrfg_layer_double_wide_call_chain
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-double-wide-call-chain-test.log"
                double-wide)

        add_test(
            NAME xrfg_layer_d3d11_interop
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-d3d11-interop-test.log"
                d3d11-interop)

        add_test(
            NAME xrfg_layer_d3d11_double_wide
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-d3d11-double-wide-test.log"
                d3d11-double-wide)

        add_test(
            NAME xrfg_layer_steamvr_inline
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-steamvr-inline-test.log"
                steamvr-inline)

        add_test(
            NAME xrfg_layer_steamvr_presenter_transition
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-steamvr-presenter-test.log"
                steamvr-presenter)

        add_test(
            NAME xrfg_layer_pipelined_presenter
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-pipelined-presenter-test.log"
                flight-simulator)

        add_test(
            NAME xrfg_layer_native_time_rewrite
            COMMAND xrfg_layer_call_chain
                ${XRFG_TEST_LAYER_DLL}
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-native-time-rewrite-test.log"
                native-time-rewrite)

        add_test(
            NAME xrfg_layer_flight_logging
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=${XRFG_TEST_LAYER_DLL}"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/flight-logging-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_flight_logging.cmake")

        add_test(
            NAME xrfg_layer_d3d11_interop_logging
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=${XRFG_TEST_LAYER_DLL}"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/d3d11-interop-logging-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_d3d11_interop_logging.cmake")

        add_executable(xrfg_luke_depth_gpu_tests tests/luke_depth_gpu_tests.cpp)
        add_executable(xrfg_luke_vulkan_interop_tests tests/luke_vulkan_interop_tests.cpp src/d3d12/luke_color_origin.cpp)
        target_include_directories(xrfg_luke_vulkan_interop_tests PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}/../../external/vulkan/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../external/nvngx_dlss_sdk")
        target_compile_definitions(xrfg_luke_vulkan_interop_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_compile_features(xrfg_luke_vulkan_interop_tests PRIVATE cxx_std_20)
        target_link_libraries(xrfg_luke_vulkan_interop_tests PRIVATE d3d12 dxgi xrfg_bridge_flight_logger
            "${CMAKE_CURRENT_SOURCE_DIR}/../../OptiScaler/library/vulkan/vulkan-1.lib")
        add_test(NAME xrfg_luke_vulkan_interop COMMAND xrfg_luke_vulkan_interop_tests)
        set_tests_properties(xrfg_luke_vulkan_interop PROPERTIES TIMEOUT 25)
        target_include_directories(xrfg_luke_depth_gpu_tests PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
        target_link_libraries(xrfg_luke_depth_gpu_tests PRIVATE d3d12 dxgi)
        target_compile_features(xrfg_luke_depth_gpu_tests PRIVATE cxx_std_20)
        add_dependencies(xrfg_luke_depth_gpu_tests xrfg_implicit_layer)
        add_test(NAME xrfg_luke_depth_gpu COMMAND xrfg_luke_depth_gpu_tests)
        add_executable(xrfg_d3d12_history_tests tests/d3d12_history_tests.cpp)
        target_include_directories(xrfg_d3d12_history_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
        target_include_directories(xrfg_d3d12_history_tests PRIVATE include)
        target_link_libraries(xrfg_d3d12_history_tests PRIVATE
            xrfg_d3d12_history
            d3d12
            dxgi)
        target_compile_features(xrfg_d3d12_history_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_d3d12_history_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_d3d12_history_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
        if(XRFG_COMPARISON_ROUTE STREQUAL "djules")
            add_test(NAME xrfg_djules_extra_output_gpu COMMAND xrfg_d3d12_history_tests djules-extra)
        endif()
        add_test(NAME xrfg_d3d12_history_tests COMMAND xrfg_d3d12_history_tests)
        add_test(NAME xrfg_luke_protected_guides COMMAND xrfg_d3d12_history_tests protected-guides)



        add_executable(xrfg_vertical_fov_tests tests/vertical_fov_tests.cpp)
        target_include_directories(xrfg_vertical_fov_tests PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
        target_link_libraries(xrfg_vertical_fov_tests PRIVATE xrfg_d3d12_history d3d12 dxgi)
        target_compile_features(xrfg_vertical_fov_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_vertical_fov_tests PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_vertical_fov_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_vertical_fov_tests COMMAND xrfg_vertical_fov_tests)
        add_executable(xrfg_synthetic_marker_tests tests/synthetic_marker_tests.cpp)
        target_include_directories(xrfg_synthetic_marker_tests PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
        target_link_libraries(xrfg_synthetic_marker_tests PRIVATE xrfg_d3d12_history d3d12 dxgi)
        target_compile_features(xrfg_synthetic_marker_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_synthetic_marker_tests PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_synthetic_marker_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_synthetic_marker_tests COMMAND xrfg_synthetic_marker_tests)

        add_executable(xrfg_nvidia_optical_flow_probe
            tests/nvidia_optical_flow_probe.cpp)
        target_include_directories(xrfg_nvidia_optical_flow_probe PRIVATE
            "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}")
        target_link_libraries(xrfg_nvidia_optical_flow_probe PRIVATE d3d12 dxgi)
        target_compile_features(xrfg_nvidia_optical_flow_probe PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_nvidia_optical_flow_probe PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_nvidia_optical_flow_probe PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
    endif()
endif()
