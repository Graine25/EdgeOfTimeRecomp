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
add_subdirectory(thirdparty)

reeot_host_shader(present_blit_vs vs_6_0)
reeot_host_shader(present_blit_ps ps_6_0)

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
    src/gpu/device/native_texture_mirror.cpp
    src/gpu/device/native_texture_mirror.h
    src/gpu/gpu.h
    src/gpu/guest/d3d.h
    src/gpu/guest/format.cpp
    src/gpu/guest/format.h
    src/gpu/guest/resources.cpp
    src/gpu/guest/resources.h
    src/gpu/hooks/device.cpp
    src/gpu/hooks/resource.cpp
    src/gpu/hooks/state.cpp
    src/gpu/hooks/stubs.cpp
    ${REEOT_SHADER_HEADERS}

    src/platform/native_window.cpp
    src/platform/native_window.h
)

if(WIN32)
    add_executable(reeot WIN32 ${REEOT_SOURCES})
else()
    add_executable(reeot ${REEOT_SOURCES})
endif()

rexglue_setup_target(reeot GPU_PLUGINS xenos)

target_include_directories(reeot PRIVATE src "${REEOT_GEN_DIR}")

target_link_libraries(reeot PRIVATE rex::ui o1heap plume)
if(WIN32)
    target_link_libraries(reeot PRIVATE d3d12 dxgi)
endif()
