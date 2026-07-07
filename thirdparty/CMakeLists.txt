if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(plume EXCLUDE_FROM_ALL)

add_subdirectory(reeot-XenosRecomp EXCLUDE_FROM_ALL)

if(NOT TARGET xxHash::xxhash)
    set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
    set(build_shared_prev ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(reeot-XenosRecomp/thirdparty/xxHash/cmake_unofficial
                     EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${build_shared_prev} CACHE BOOL "" FORCE)
endif()
