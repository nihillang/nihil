#!/bin/bash
# try.sh PROGRAM.nihil [OUT] — the bootstrap path: compiles with the x86 compiler in the VM (-map),
# translates to ARM64 here and builds a native macOS executable with clang and arm64/rt.c
set -e
cd "$(dirname "$0")/.."
src=$1; out=${2:-/tmp/nihil_try}
abs=$(cd "$(dirname "$src")" && pwd)/$(basename "$src")
colima ssh -- bash -c "cd '$PWD' && printf '%s\n' '-map $abs' | build/nihilc1 > /tmp/try.x86 2> /tmp/try.map; cp /tmp/try.x86 /tmp/try.map '$PWD/build/macos/'"
macos/translator.sh
printf '%s\n%s\n%s\n' build/macos/try.x86 build/macos/try.map build/macos/try.s | build/macos/translate
clang -O2 -arch arm64 -Wno-unused-result -o "$out" build/macos/try.s arm64/rt.c
