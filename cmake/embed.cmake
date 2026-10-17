# Converts a binary file into a C source file exposing:
#   const unsigned char SYM[];  const unsigned int SYM_size;
# Usage: cmake -DIN=file -DOUT=file.c -DSYM=name -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR size "${hexlen} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" body "${body}")
file(WRITE "${OUT}" "/* Generated from ${IN} */\n#include <stddef.h>\nconst unsigned char ${SYM}[] = {\n${body}\n};\nconst unsigned int ${SYM}_size = ${size};\n")
