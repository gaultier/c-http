#!/bin/sh
set -ex


CC="${CC:-clang}"

curl --retry 5 -sSL https://www.sqlite.org/2024/sqlite-amalgamation-3470000.zip -o sqlite-amalgamation-3470000.zip
# TODO: checksum check.

unzip -u -o sqlite-amalgamation-3470000.zip

cp sqlite-amalgamation-3470000/sqlite3.c sqlite-amalgamation-3470000/sqlite3.h  .
rm -r ./sqlite-amalgamation-3470000 ./sqlite-amalgamation-3470000.zip


SQLITE_OPTIONS="$(cat sqlite_options.txt)"

# shellcheck disable=SC2086
"$CC" sqlite3.c -c -O3 -march=native -g $SQLITE_OPTIONS
