#!/bin/sh
# Syntax-check the game's C units against the port's SDK headers; prints the first errors of each failing unit.
cd "$(dirname "$0")/.."
CLANG="/c/Program Files/LLVM/bin/clang.exe"
FLAGS="--target=i686-pc-windows-msvc -std=gnu89 -fsyntax-only -fms-extensions -DTARGET_PC -Dmain=GameMain \
 -Iport/sdk/include -Iinclude -Iport/build/generated -Wno-everything -Werror=implicit-function-declaration -ferror-limit=50"
for f in ${@:-src/*.c}; do
  out=$("$CLANG" $FLAGS "$f" 2>&1 | grep -E "error" )
  [ -n "$out" ] && echo "$out"
done
exit 0
