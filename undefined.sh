#!/bin/sh
# Builds the port and writes the symbols the link still lacks to port/build/undefined.txt.
cd "$(dirname "$0")"
toolchain/cmake/bin/cmake --build build > build/build.log 2>&1
grep -oE "undefined symbol: _?[^ ]+" build/build.log | sed -E 's/undefined symbol: _?//' | sort -u > build/undefined.txt
echo "$(wc -l < build/undefined.txt) undefined"
grep -E "error:" build/build.log | grep -v "undefined symbol\|linker command failed" | head -20
exit 0
