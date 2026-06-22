# reeot - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reeot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

add_subdirectory(thirdparty/plume)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/zstd/build/cmake zstd_build)

# Sources
set(REEOT_SOURCES
    src/main.cpp
    src/goliath_engine/gpu/headless_stubs.cpp
    src/goliath_engine/gpu/renderer/observe.cpp
    src/goliath_engine/gpu/renderer/video.cpp
    src/goliath_engine/gpu/renderer/d3d_hooks.cpp
    src/goliath_engine/gpu/renderer/shaders.cpp
    src/goliath_engine/gpu/renderer/draw.cpp
    generated/shader_cache.cpp
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

target_link_libraries(reeot PRIVATE libzstd_static)
target_include_directories(reeot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/lib")

add_custom_command(
    TARGET reeot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxcompiler.dll"
            "$<TARGET_FILE_DIR:reeot>/dxcompiler.dll"
    COMMENT "Copying dxcompiler.dll to build output"
    VERBATIM)
