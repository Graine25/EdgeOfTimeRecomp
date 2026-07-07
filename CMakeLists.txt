# reeot - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reeot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REEOT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

include(generated/rexglue.cmake)
include(cmake/build_info.cmake)
include(cmake/shaders.cmake)
include(cmake/shader_cache.cmake)
add_subdirectory(thirdparty)

reeot_host_shader(present_blit_vs vs_6_0)
reeot_host_shader(present_blit_ps ps_6_0)

#
set(REEOT_SHADER_PAK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../reeot/shader_notes/EOT_pak"
    CACHE PATH "Directory of unpacked .pak files the shader cache is built from")
set(REEOT_SHADER_PKZ_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../reeot/assets/Data"
    CACHE PATH "Directory of .pkz packages to unpack when PAK_DIR is empty")
set(REEOT_SHADER_XEX "${CMAKE_CURRENT_SOURCE_DIR}/assets/Default.xex"
    CACHE FILEPATH "Guest executable scanned for XEX-resident shaders")

set(REEOT_SHADER_COMMON_H
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/reeot-XenosRecomp/XenosRecomp/shader_common.h")
cmake_path(GET REEOT_SHADER_COMMON_H PARENT_PATH REEOT_SHADER_COMMON_DIR)

reeot_shader_cache(
    RECOMP_TARGET XenosRecomp
    PAK_DIR       "${REEOT_SHADER_PAK_DIR}"
    PKZ_DIR       "${REEOT_SHADER_PKZ_DIR}"
    XEX           "${REEOT_SHADER_XEX}"
    INCLUDE_FILE  "${REEOT_SHADER_COMMON_H}"
    OUTPUT_CPP    "${REEOT_GEN_DIR}/shader_cache.cpp")

reeot_write_build_info("${REEOT_GEN_DIR}/core/build_info.h")

# Sources
set(REEOT_SOURCES
    src/main.cpp

    src/core/app_root.cpp
    src/core/app_root.h
    src/core/dbg_print.cpp
    src/core/encoding.cpp
    src/core/encoding.h
    src/core/hooks.h
    src/core/i18n.h
    src/core/logging.h
    src/core/memory_helpers.cpp
    src/core/memory_helpers.h
    src/core/perf.cpp
    src/core/perf.h
    src/core/profiling.h
    src/core/settings.cpp
    src/core/settings.h
    src/core/shutdown.h
    src/core/threading.cpp
    src/core/threading.h
    src/core/time_util.h
    src/core/windows_lean.h

    src/gpu/device/device.cpp
    src/gpu/device/device.h
    src/gpu/device/host_heap_arena.cpp
    src/gpu/device/host_heap_arena.h
    src/gpu/device/host_resource_heap.cpp
    src/gpu/device/host_resource_heap.h
    src/gpu/device/texture_upload.cpp
    src/gpu/device/texture_upload.h
    src/gpu/device/native_texture_mirror.cpp
    src/gpu/device/native_texture_mirror.h
    src/gpu/gpu.h
    src/gpu/guest/buffers.cpp
    src/gpu/guest/buffers.h
    src/gpu/guest/d3d.h
    src/gpu/guest/format.cpp
    src/gpu/guest/format.h
    src/gpu/guest/resources.cpp
    src/gpu/guest/resources.h
    src/gpu/hooks/buffer.cpp
    src/gpu/hooks/device.cpp
    src/gpu/hooks/draw.cpp
    src/gpu/hooks/resource.cpp
    src/gpu/hooks/shader.cpp
    src/gpu/hooks/state.cpp
    src/gpu/hooks/stubs.cpp
    src/gpu/shaders/dxc_link.cpp
    src/gpu/shaders/dxc_link.h
    src/gpu/shaders/guest_shaders.cpp
    src/gpu/shaders/guest_shaders.h
    src/gpu/shaders/shader_cache.h
    src/gpu/shaders/shader_linker.cpp
    src/gpu/shaders/shader_linker.h
    ${REEOT_SHADER_HEADERS}

    "${REEOT_GEN_DIR}/shader_cache.cpp"

    src/platform/native_window.cpp
    src/platform/native_window.h
)

if(WIN32)
    add_executable(reeot WIN32 ${REEOT_SOURCES})
else()
    add_executable(reeot ${REEOT_SOURCES})
endif()

rexglue_setup_target(reeot GPU_PLUGINS xenos)

target_include_directories(reeot PRIVATE
    src src/gpu/shaders "${REEOT_GEN_DIR}" "${REEOT_SHADER_COMMON_DIR}")

target_link_libraries(reeot PRIVATE rex::ui o1heap plume)
if(WIN32)
    target_link_libraries(reeot PRIVATE d3d12 dxgi)
endif()

target_link_libraries(reeot PRIVATE libzstd_static)
target_include_directories(reeot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/reeot-XenosRecomp/thirdparty/zstd/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/reeot-XenosRecomp/thirdparty/xxHash")
if(WIN32)
    target_link_libraries(reeot PRIVATE Microsoft::DirectXShaderCompiler)

    foreach(dxc_dll dxcompiler dxil)
        add_custom_command(TARGET reeot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/reeot-XenosRecomp/thirdparty/dxc-bin/bin/x64/${dxc_dll}.dll"
                "$<TARGET_FILE_DIR:reeot>"
            VERBATIM)
    endforeach()
endif()
