# Usage: cmake -DINPUT=file -DOUTPUT=file.cpp -DSYMBOL=name -P embed.cmake
file(READ ${INPUT} _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _size "${_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
file(WRITE ${OUTPUT} "// Generated from ${INPUT}. Do not edit.\nextern const unsigned char ${SYMBOL}[] = {${_bytes}};\nextern const unsigned long ${SYMBOL}_size = ${_size};\n")
