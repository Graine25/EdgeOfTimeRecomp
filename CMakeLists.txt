# reeot - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.
#
#

cmake_minimum_required(VERSION 3.25)
project(reeot VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REEOT_D3D12 "Build the D3D12 renderer backend (OFF selects Vulkan)" ${WIN32})
option(REEOT_PROFILING "Compile the Tracy zones into a Release build of reeot" OFF)

set(REEOT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(REEOT_SHADER_COMMON_H
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/XenosRecomp/shader_common.h")
cmake_path(GET REEOT_SHADER_COMMON_H PARENT_PATH REEOT_SHADER_COMMON_DIR)
set(REEOT_SHADER_CACHE_CPP "${CMAKE_CURRENT_SOURCE_DIR}/generated/shader_cache.cpp")
set(REEOT_SHADER_INPUT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_notes/EOT_pak_extracted_shaders/unique")

include(generated/rexglue.cmake)
include(cmake/build_info.cmake)

reeot_write_build_info("${REEOT_GEN_DIR}/core/build_info.h")

option(REEOT_BUILD_INSTALLER "Build the built-in first-run installer" ON)
include(cmake/embed.cmake)
reeot_embed_directory(
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/res/embed"
    HEADER      "${REEOT_GEN_DIR}/embedded.h"
    SOURCES_VAR REEOT_EMBED_SOURCES)

add_subdirectory(thirdparty)

set(REEOT_REFERENCE_PAKS "${CMAKE_CURRENT_SOURCE_DIR}/../reeot/reference_pak" CACHE PATH
    "Directory holding the retail Main.pak / Common.pak the content package copies textures from")
set(REEOT_PKZ_OUTPUT "${CMAKE_BINARY_DIR}/pkz/ReeotUI.pkz")
add_custom_command(
    OUTPUT "${REEOT_PKZ_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/pkz"
    COMMAND pkztool make "${CMAKE_CURRENT_SOURCE_DIR}/tools/pkz/ReeotUI.manifest" "${REEOT_PKZ_OUTPUT}"
            --paks "${REEOT_REFERENCE_PAKS}"
    DEPENDS pkztool
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/pkz/ReeotUI.manifest"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/pkz/reeot_ui_strings.txt"
    COMMENT "Authoring ReeotUI.pkz from tools/pkz/ReeotUI.manifest"
    VERBATIM)
add_custom_target(reeot_pkz ALL DEPENDS "${REEOT_PKZ_OUTPUT}")
include(cmake/host_shaders.cmake)

add_custom_target(reeot_shader_cache_regen
    COMMAND $<TARGET_FILE:XenosRecomp>
            "${REEOT_SHADER_INPUT_DIR}" "${REEOT_SHADER_CACHE_CPP}" "${REEOT_SHADER_COMMON_H}"
    DEPENDS XenosRecomp
    COMMENT "Recompiling EOT Xenos shaders into ${REEOT_SHADER_CACHE_CPP}"
    USES_TERMINAL VERBATIM)

if(NOT EXISTS "${REEOT_SHADER_CACHE_CPP}")
    message(WARNING
        "${REEOT_SHADER_CACHE_CPP} is missing: build the reeot_shader_cache_regen "
        "target first (it takes ~10 minutes), then reconfigure.")
    add_library(reeot_shader_cache OBJECT src/gpu/shaders/shader_cache_empty.cpp)
else()
    add_library(reeot_shader_cache OBJECT "${REEOT_SHADER_CACHE_CPP}")
endif()
target_include_directories(reeot_shader_cache PRIVATE src/gpu/shaders)
target_compile_options(reeot_shader_cache PRIVATE -O0 -w)

# Sources
set(REEOT_SOURCES
    src/main.cpp
    src/reeot_app.cpp
    src/platform/file_dialog.cpp
    src/platform/desktop_shortcut.cpp
    src/platform/process.cpp
    src/platform/fatal_dialog.cpp
    src/platform/crash_handler.cpp
    src/ui/theme.cpp
    src/installer/install_registry.cpp
    src/installer/self_install.cpp
    ${REEOT_EMBED_SOURCES}
    src/core/critical_section.cpp
    src/core/quit.cpp
    src/gpu/settings.cpp
    src/gpu/quality.cpp
    src/gpu/device.cpp
    src/gpu/frame_ring.cpp
    src/gpu/present.cpp
    src/gpu/imgui_overlay.cpp
    src/gpu/bindless.cpp
    src/gpu/sampler_cache.cpp
    src/gpu/format.cpp
    src/gpu/constant_buffers.cpp
    src/gpu/textures.cpp
    src/gpu/surfaces.cpp
    src/gpu/vertex_layout.cpp
    src/gpu/draw.cpp
    src/gpu/resolve.cpp
    src/gpu/trace.cpp
    src/gpu/pipeline/pipeline_cache.cpp
    src/gpu/gpu_profiling.cpp
    src/gpu/gpu_timing.cpp
    src/gpu/pipeline/pso_records.cpp
    src/gpu/pipeline/pso_precache.cpp
    src/gpu/pipeline/pso_predictor.cpp
    src/gpu/hooks/model_load.cpp
    src/gpu/hooks/package_load.cpp
    src/gpu/patches/frame_time.cpp
    src/gpu/patches/update_rate.cpp
    src/gpu/patches/physics_step.cpp
    src/gpu/patches/aspect_ratio.cpp
    src/gpu/patches/movie_aspect.cpp
    src/gpu/patches/post_effects.cpp
    src/gpu/patches/present_effects.cpp
    src/goliath/input/pc_controls.cpp
    src/goliath/ui/aspect_policy.cpp
    src/goliath/ui/text_scale.cpp
    src/goliath/ui/overlays/fps.cpp
    src/goliath/loading/reeot_package.cpp
    src/gpu/shaders/guest_shaders.cpp
    src/gpu/shaders/dxc_link.cpp
    src/gpu/hooks/stubs.cpp
    src/gpu/hooks/device.cpp
    src/gpu/hooks/state.cpp
    src/gpu/hooks/draw.cpp
    src/gpu/hooks/resource.cpp
)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(REEOT_HUD_POLICY_INC "${REEOT_GEN_DIR}/hud_aspect_policy.inc")
add_custom_command(
    OUTPUT "${REEOT_HUD_POLICY_INC}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${REEOT_GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/hud/gen_hud_policy.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/config/eot_hud_aspect.toml"
            "${REEOT_HUD_POLICY_INC}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config/eot_hud_aspect.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/hud/gen_hud_policy.py"
    COMMENT "Compiling the HUD aspect policy"
    VERBATIM)
list(APPEND REEOT_SOURCES "${REEOT_HUD_POLICY_INC}")

if(WIN32)
    add_executable(reeot WIN32 ${REEOT_SOURCES})
else()
    add_executable(reeot ${REEOT_SOURCES})
endif()

rexglue_setup_target(reeot)

if(TARGET reeot_GameLogic)
    target_sources(reeot_GameLogic PRIVATE
        src/gamelogic/debug/debug_mode.cpp
        src/gamelogic/patches/rocket_launch_clearance.cpp
        src/gamelogic/ui/pause_menu.cpp
        src/gamelogic/ui/options_graphics.cpp
        src/gamelogic/ui/save_select.cpp)
    target_include_directories(reeot_GameLogic PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

target_include_directories(reeot PRIVATE
    src
    "${REEOT_GEN_DIR}"
    "${REXSDK_DIR}/thirdparty/stb"
    "${REEOT_SHADER_COMMON_DIR}"
    thirdparty/renderdoc
    "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}")

if(REEOT_BUILD_INSTALLER)
    target_sources(reeot PRIVATE
        src/installer/disc_install.cpp
        src/installer/installer_wizard.cpp)
endif()

set(reeot_program_files
    "$<TARGET_FILE_NAME:reeot>"
    "$<TARGET_FILE_NAME:rex::runtime>"
    "pkz"
    "metadata"
    "gamecontrollerdb.txt")
if(TARGET reeot_GameLogic)
    list(APPEND reeot_program_files "$<TARGET_FILE_NAME:reeot_GameLogic>")
endif()
if(REEOT_D3D12)
    list(APPEND reeot_program_files dxcompiler.dll dxil.dll)
endif()
if(TARGET rex::TracyClient)
    list(APPEND reeot_program_files "$<TARGET_FILE_NAME:rex::TracyClient>")
endif()
set(reeot_program_files_cxx "")
foreach(f IN LISTS reeot_program_files)
    string(APPEND reeot_program_files_cxx "    \"${f}\",\n")
endforeach()
file(GENERATE OUTPUT "${REEOT_GEN_DIR}/installer/program_files.h" CONTENT
"// Generated by CMakeLists.txt - do not edit.
#pragma once

inline constexpr const char *kProgramFiles[] = {
${reeot_program_files_cxx}
};
")
list(JOIN reeot_program_files "\n" reeot_program_files_txt)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:reeot>/program_files.txt"
     CONTENT "${reeot_program_files_txt}\n")

add_custom_command(TARGET reeot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/gamecontrollerdb/gamecontrollerdb.txt"
        $<TARGET_FILE_DIR:reeot>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/res/metadata"
        "$<TARGET_FILE_DIR:reeot>/metadata"
    VERBATIM)

target_compile_definitions(reeot PRIVATE
    _ITERATOR_DEBUG_LEVEL=0
    $<$<BOOL:${REEOT_BUILD_INSTALLER}>:REEOT_BUILD_INSTALLER>
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    $<$<BOOL:${REEOT_D3D12}>:EOT_D3D12>
    $<$<OR:$<BOOL:${REEOT_PROFILING}>,$<NOT:$<CONFIG:Release>>>:EOT_PROFILING>)

target_link_libraries(reeot PRIVATE
    rex::ui
    plume
    reeot_shader_cache
    xxHash::xxhash
    libzstd_static)

if(WIN32)
    target_link_libraries(reeot PRIVATE winmm shell32 ole32 advapi32)
endif()

foreach(shader IN ITEMS copy_vs imgui_vs)
    reeot_host_shader(reeot ${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS blit_ps copy_depth_ps imgui_ps
        resolve_msaa_color_2x_ps resolve_msaa_color_4x_ps resolve_msaa_color_8x_ps
        resolve_msaa_depth_2x_ps resolve_msaa_depth_4x_ps resolve_msaa_depth_8x_ps)
    reeot_host_shader(reeot ${shader} ps_6_0)
endforeach()

if(REEOT_D3D12)
    target_link_libraries(reeot PRIVATE d3d12 dxgi Microsoft::DirectXShaderCompiler)
    add_custom_command(TARGET reeot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxil.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:reeot>
        VERBATIM)
else()
    find_package(Vulkan REQUIRED)
    target_link_libraries(reeot PRIVATE Vulkan::Vulkan)
endif()
