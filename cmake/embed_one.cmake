if(NOT DEFINED INPUT OR NOT DEFINED SYMBOL OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "embed_one.cmake requires -DINPUT, -DSYMBOL, -DOUTPUT")
endif()

file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")

file(WRITE "${OUTPUT}"
"// Generated from ${INPUT} by cmake/embed_one.cmake - do not edit.\n"
"#include <cstdint>\n"
"extern const uint8_t ${SYMBOL}[];\n"
"const uint8_t ${SYMBOL}[] = {${bytes}};\n")
