#!/bin/sh
set -ex
clang -std=c11 -Wall -Wextra -g  -fsanitize=address -Wno-gnu-alignof-expression test.c -o test.bin && ./test.bin
