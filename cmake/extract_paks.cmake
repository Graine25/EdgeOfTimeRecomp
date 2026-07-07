foreach(arg PKZ_DIR PAK_DIR QUICKBMS BMS_SCRIPT STAMP)
    if(NOT DEFINED ${arg})
        message(FATAL_ERROR "extract_paks: missing -D${arg}")
    endif()
endforeach()

file(GLOB existing_paks "${PAK_DIR}/*.pak")
if(existing_paks)
    list(LENGTH existing_paks pak_count)
    message(STATUS "extract_paks: ${PAK_DIR} already holds ${pak_count} .pak "
                   "files; reusing them.")
    file(TOUCH "${STAMP}")
    return()
endif()

if(NOT EXISTS "${QUICKBMS}" OR NOT EXISTS "${BMS_SCRIPT}")
    message(FATAL_ERROR
        "extract_paks: quickbms not found (QUICKBMS='${QUICKBMS}', "
        "BMS_SCRIPT='${BMS_SCRIPT}'). Set REEOT_QUICKBMS and "
        "REEOT_QUICKBMS_SCRIPT, or populate ${PAK_DIR} yourself.")
endif()

file(GLOB pkz_files "${PKZ_DIR}/*.pkz")
if(NOT pkz_files)
    message(FATAL_ERROR "extract_paks: no .pkz packages under '${PKZ_DIR}'.")
endif()

list(LENGTH pkz_files pkz_count)
message(STATUS "extract_paks: unpacking ${pkz_count} packages into ${PAK_DIR}")
file(MAKE_DIRECTORY "${PAK_DIR}")

set(index 0)
set(failed "")
foreach(pkz IN LISTS pkz_files)
    math(EXPR index "${index} + 1")
    cmake_path(GET pkz FILENAME pkz_name)
    message(STATUS "  [${index}/${pkz_count}] ${pkz_name}")

    execute_process(
        COMMAND "${QUICKBMS}" -o "${BMS_SCRIPT}" "${pkz}" "${PAK_DIR}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        list(APPEND failed "${pkz_name}")
    endif()
endforeach()

file(GLOB produced "${PAK_DIR}/*.pak")
list(LENGTH produced produced_count)
if(NOT produced)
    message(FATAL_ERROR
        "extract_paks: quickbms produced no .pak files. Last error output:\n${err}")
endif()

if(failed)
    list(LENGTH failed failed_count)
    message(STATUS "extract_paks: ${produced_count} .pak files "
                   "(${failed_count} packages reported a short read)")
else()
    message(STATUS "extract_paks: ${produced_count} .pak files")
endif()

file(TOUCH "${STAMP}")
