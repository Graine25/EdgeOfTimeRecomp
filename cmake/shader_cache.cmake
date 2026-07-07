function(reeot_shader_cache)
    cmake_parse_arguments(ARG ""
        "RECOMP_TARGET;PAK_DIR;PKZ_DIR;XEX;INCLUDE_FILE;OUTPUT_CPP" "" ${ARGN})
    foreach(arg RECOMP_TARGET PAK_DIR PKZ_DIR XEX INCLUDE_FILE OUTPUT_CPP)
        if(NOT ARG_${arg})
            message(FATAL_ERROR "reeot_shader_cache: missing ${arg}")
        endif()
    endforeach()

    set(quickbms_default "${CMAKE_CURRENT_SOURCE_DIR}/../reeot/thirdparty/quickbms/quickbms.exe")
    set(bms_default "${CMAKE_CURRENT_SOURCE_DIR}/../reeot/thirdparty/quickbms/BeenoxSM_Console.bms")
    if(NOT EXISTS "${quickbms_default}")
        set(quickbms_default "${CMAKE_CURRENT_SOURCE_DIR}/../quickbms_win/quickbms.exe")
        set(bms_default "${CMAKE_CURRENT_SOURCE_DIR}/../quickbms_win/BeenoxSM_Console.bms")
    endif()
    set(REEOT_QUICKBMS "${quickbms_default}" CACHE FILEPATH
        "quickbms executable used to unpack .pkz packages")
    set(REEOT_QUICKBMS_SCRIPT "${bms_default}" CACHE FILEPATH
        "quickbms script for the Beenox .pkz format")

    cmake_path(GET ARG_OUTPUT_CPP PARENT_PATH output_dir)
    file(MAKE_DIRECTORY "${output_dir}")

    set(pak_stamp "${CMAKE_CURRENT_BINARY_DIR}/reeot_paks.stamp")
    add_custom_command(
        OUTPUT "${pak_stamp}"
        COMMAND ${CMAKE_COMMAND}
                -DPKZ_DIR=${ARG_PKZ_DIR}
                -DPAK_DIR=${ARG_PAK_DIR}
                -DQUICKBMS=${REEOT_QUICKBMS}
                -DBMS_SCRIPT=${REEOT_QUICKBMS_SCRIPT}
                -DSTAMP=${pak_stamp}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/extract_paks.cmake"
        DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/extract_paks.cmake"
        COMMENT "Resolving unpacked guest packages in ${ARG_PAK_DIR}"
        USES_TERMINAL VERBATIM)

    file(GLOB_RECURSE shader_inputs CONFIGURE_DEPENDS "${ARG_PAK_DIR}/*.pak")

    cmake_path(GET ARG_XEX FILENAME xex_name)
    set(staged_xex "${ARG_PAK_DIR}/${xex_name}")
    add_custom_command(
        OUTPUT "${ARG_OUTPUT_CPP}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ARG_XEX}" "${staged_xex}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_PAK_DIR}" "${ARG_OUTPUT_CPP}" "${ARG_INCLUDE_FILE}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${staged_xex}"
        DEPENDS ${ARG_RECOMP_TARGET} "${ARG_INCLUDE_FILE}" "${ARG_XEX}"
                "${pak_stamp}" ${shader_inputs}
        COMMENT "Recompiling Xenos shaders from ${ARG_PAK_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(reeot_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")

    set(REEOT_HLSL_DUMP_DIR "${CMAKE_BINARY_DIR}/hlsl_dump" CACHE PATH
        "Directory reeot_shader_hlsl_dump writes recompiled HLSL into")
    add_custom_target(reeot_shader_hlsl_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REEOT_HLSL_DUMP_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ARG_XEX}" "${staged_xex}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_PAK_DIR}" "${REEOT_HLSL_DUMP_DIR}"
                "${ARG_INCLUDE_FILE}" --hlsl
        COMMAND ${CMAKE_COMMAND} -E rm -f "${staged_xex}"
        DEPENDS ${ARG_RECOMP_TARGET} "${pak_stamp}"
        COMMENT "Dumping recompiled HLSL to ${REEOT_HLSL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)

    set(REEOT_DXIL_DUMP_DIR "${CMAKE_BINARY_DIR}/dxil_dump" CACHE PATH
        "Directory reeot_shader_dxil_dump writes recompiled DXIL into")
    add_custom_target(reeot_shader_dxil_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REEOT_DXIL_DUMP_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ARG_XEX}" "${staged_xex}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_PAK_DIR}" "${REEOT_DXIL_DUMP_DIR}"
                "${ARG_INCLUDE_FILE}" --dxil
        COMMAND ${CMAKE_COMMAND} -E rm -f "${staged_xex}"
        DEPENDS ${ARG_RECOMP_TARGET} "${pak_stamp}"
        COMMENT "Dumping recompiled DXIL to ${REEOT_DXIL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)
endfunction()
