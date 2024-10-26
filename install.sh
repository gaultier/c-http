#!/bin/sh
set -ex


CC="${CC:-clang}"

SQLITE_OPTIONS="$(tr -s '\n' ' ' < sqlite_options.txt)"

# shellcheck disable=SC2086
"$CC" sqlite3.c -c -O3 -march=native -g $SQLITE_OPTIONS
