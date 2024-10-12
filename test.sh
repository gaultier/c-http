#!/bin/sh
set -ex

CC="${CC:-clang}"

"$CC" -std=c11 -Wall -Wextra -Wno-gnu-alignof-expression -Wconversion -Wno-sign-conversion -g3 -gsplit-dwarf test.c -o test.bin -fsanitize=address,undefined && ./test.bin
