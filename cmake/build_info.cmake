function(reeot_write_build_info out_header)
    set(REEOT_GIT_COMMIT "unknown")
    set(REEOT_GIT_BRANCH "unknown")
    set(REEOT_GIT_DIRTY 0)

    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE git_dir OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE git_rc)
    endif()

    if(GIT_FOUND AND git_rc EQUAL 0)
        foreach(dep HEAD index)
            if(EXISTS "${git_dir}/${dep}")
                set_property(DIRECTORY APPEND PROPERTY
                    CMAKE_CONFIGURE_DEPENDS "${git_dir}/${dep}")
            endif()
        endforeach()

        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=9 HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ERROR_QUIET
            OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ERROR_QUIET
            OUTPUT_VARIABLE branch OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ERROR_QUIET
            OUTPUT_VARIABLE worktree_status)

        if(commit)
            set(REEOT_GIT_COMMIT "${commit}")
        endif()
        if(branch)
            set(REEOT_GIT_BRANCH "${branch}")
        endif()
        if(worktree_status)
            set(REEOT_GIT_DIRTY 1)
        endif()
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
    if(arch MATCHES "amd64|x86_64")
        set(arch amd64)
    elseif(arch MATCHES "aarch64|arm64")
        set(arch arm64)
    endif()
    if(WIN32)
        set(REEOT_BUILD_PLATFORM "win-${arch}")
    elseif(APPLE)
        set(REEOT_BUILD_PLATFORM "mac-${arch}")
    else()
        set(REEOT_BUILD_PLATFORM "linux-${arch}")
    endif()

    set(REEOT_BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    string(TIMESTAMP REEOT_BUILD_TIMESTAMP "%Y%m%d_%H%M")
    set(REEOT_BUILD_TIMESTAMP "${REEOT_BUILD_TIMESTAMP}" PARENT_SCOPE)

    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/build_info.h.in" "${out_header}" @ONLY)

    message(STATUS "reeot v${reeot_VERSION} ${REEOT_GIT_COMMIT} on "
                   "${REEOT_GIT_BRANCH} (dirty=${REEOT_GIT_DIRTY}) "
                   "${REEOT_BUILD_PLATFORM}")
endfunction()
