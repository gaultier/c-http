#!/bin/sh
set -ex
clang -std=c11 -Wall -Wextra -g main.c -fsanitize=address -Wno-gnu-alignof-expression -o main.bin
