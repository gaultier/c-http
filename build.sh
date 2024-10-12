#!/bin/sh
set -ex

EXTRA_FLAGS=""

error() {
	printf "ERROR: %s\n" "$1"
	exit 1
}

build() {
case $1 in 
  debug)
    EXTRA_FLAGS="-O0"
    ;;
  asan)
    EXTRA_FLAGS="-fsanitize=address "
    ;;
  release)
    EXTRA_FLAGS="-O3"
    ;;
	*)
		error "Build mode \"$1\" unsupported!"
		;;
esac

clang -std=c11 -Wall -Wextra -g -Wno-gnu-alignof-expression main.c -o main.bin "$EXTRA_FLAGS"
}

if [ $# -eq 0 ]; then
	build debug
elif [ $# -eq 1 ]; then
  build $1
else
	error "Too many arguments!"
fi
