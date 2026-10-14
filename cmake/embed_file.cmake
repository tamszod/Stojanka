# cmake -DINPUT=<file> -DOUTPUT=<header> -DNAME=<symbol> -P embed_file.cmake
# Writes a header with `inline constexpr unsigned char <NAME>[]` and `<NAME>Size`.
# A byte array avoids MSVC's limits on string literal length.
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
  "// Generated from ${INPUT}. Do not edit.\n#pragma once\n#include <cstddef>\n"
  "inline constexpr unsigned char ${NAME}[] = {\n${bytes}0x00};\n"
  "inline constexpr std::size_t ${NAME}Size = ${size};\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
