#!/bin/sh
# Syntax-check the game's C units against the port's SDK headers; prints the first errors of each failing unit.
PORT="$(cd "$(dirname "$0")" && pwd)"
cd "${DOJO_DECOMP_DIR:-$PORT/..}"
CLANG="$PORT/toolchain/llvm-mingw/bin/i686-w64-mingw32-clang.exe"
FLAGS="-std=gnu89 -fsyntax-only -fms-extensions -DTARGET_PC -Dmain=GameMain \
 -I$PORT/sdk/include -Iinclude -I$PORT/build/generated -Wno-everything -Werror=implicit-function-declaration -ferror-limit=50"
for f in ${@:-src/*.c}; do
  out=$("$CLANG" $FLAGS "$f" 2>&1 | grep -E "error" )
  [ -n "$out" ] && echo "$out"
done
exit 0
