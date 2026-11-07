# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake: a byte array header.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR length "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "// generated from ${IN}\n#pragma once\nstatic const unsigned char ${NAME}[] = {\n${bytes}\n};\nstatic const unsigned long ${NAME}_len = ${length};\n")
