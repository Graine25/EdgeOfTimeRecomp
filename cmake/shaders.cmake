if(NOT REEOT_DXC)
    find_program(REEOT_DXC NAMES dxc dxc.exe REQUIRED)
endif()
message(STATUS "reeot: dxc at ${REEOT_DXC}")

set(REEOT_HLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/hlsl")

function(reeot_host_shader STEM PROFILE)
    set(out "${REEOT_GEN_DIR}/src/gpu/shaders/hlsl/${STEM}.hlsl.dxil.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${REEOT_DXC}
                -T ${PROFILE} -HV 2021 -all-resources-bound
                -Fh "${out}" "${REEOT_HLSL_DIR}/${STEM}.hlsl"
                -Vn g_${STEM}_dxil
        DEPENDS "${REEOT_HLSL_DIR}/${STEM}.hlsl"
        COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, dxil)"
        VERBATIM)
    set(REEOT_SHADER_HEADERS ${REEOT_SHADER_HEADERS} "${out}" PARENT_SCOPE)
endfunction()
