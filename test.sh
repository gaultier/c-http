#!/bin/sh
set -ex

CC="${CC:-clang}"

"$CC" -std=c11 -Wall -Wextra -g  -fsanitize=address -Wno-gnu-alignof-expression test.c -o test.bin && ./test.bin
