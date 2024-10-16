#!/bin/sh
set -ex
set -f # disable globbing.

CC="${CC:-clang}"
WARNINGS="$(tr -s '\n' ' ' < compile_flags.txt)"

# shellcheck disable=SC2086
"$CC" -O0 $WARNINGS -g3 test.c -o test.bin -fsanitize=address,undefined && ./test.bin
