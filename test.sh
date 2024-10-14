#!/bin/sh
set -ex

CC="${CC:-clang}"

"$CC" -O0 -std=c23 -Wall -Wextra -Wno-gnu-alignof-expression -Wconversion -Wno-sign-conversion -g3 -gsplit-dwarf test.c -o test.bin -fsanitize=address,undefined && ./test.bin
