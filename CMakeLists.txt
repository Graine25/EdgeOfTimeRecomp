# reeot - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reeot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

add_subdirectory(thirdparty/plume)

# Sources
set(REEOT_SOURCES
    src/main.cpp
    src/goliath_engine/gpu/headless_stubs.cpp
    src/goliath_engine/gpu/renderer/observe.cpp
    src/goliath_engine/gpu/renderer/video.cpp
    src/goliath_engine/gpu/renderer/d3d_hooks.cpp
)

if(WIN32)
    add_executable(reeot WIN32 ${REEOT_SOURCES})
else()
    add_executable(reeot ${REEOT_SOURCES})
endif()

rexglue_setup_target(reeot)

target_link_libraries(reeot PRIVATE plume)
if(WIN32)
    target_link_libraries(reeot PRIVATE d3d12 dxgi dxguid)
endif()

target_include_directories(reeot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash")
