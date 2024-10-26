#!/bin/sh
set -e
set -f # disable globbing.

CFLAGS="${CFLAGS}"
EXTRA_FLAGS=""
CC="${CC:-clang}"
WARNINGS="$(tr -s '\n' ' ' < compile_flags.txt)"
SQLITE_OPTIONS="$(cat sqlite_options.txt)"

error() {
	printf "ERROR: %s\n" "$1"
	exit 1
}

build() {
case $1 in 
  debug)
    EXTRA_FLAGS="-O0"
    ;;
  sanitizer)
    EXTRA_FLAGS="-fsanitize=undefined -fsanitize-trap=all"
    ;;
  release)
    EXTRA_FLAGS="-O3 -march=native"
    ;;
	*)
		error "Build mode \"$1\" unsupported!"
		;;
esac

# shellcheck disable=SC2086
"$CC" $WARNINGS -g3 main.c sqlite3.o -o main.bin $EXTRA_FLAGS $CFLAGS $SQLITE_OPTIONS
}

if [ $# -eq 0 ]; then
	build debug
elif [ $# -eq 1 ]; then
  build "$1"
else
	error "Too many arguments!"
fi
