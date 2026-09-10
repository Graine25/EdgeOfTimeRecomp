if(NOT DEFINED INPUT OR NOT DEFINED SYMBOL OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "embed_one.cmake requires -DINPUT, -DSYMBOL, -DOUTPUT")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")

set(size_definition "")
if(WITH_SIZE)
    set(size_definition
        "#include <cstddef>\nextern const size_t ${SYMBOL}_size;\nconst size_t ${SYMBOL}_size = ${size};\n")
endif()

file(WRITE "${OUTPUT}"
"// Generated from ${INPUT} by cmake/embed_one.cmake - do not edit.\n"
"#include <cstdint>\n"
"extern const uint8_t ${SYMBOL}[];\n"
"const uint8_t ${SYMBOL}[] = {${bytes}};\n"
"${size_definition}")
