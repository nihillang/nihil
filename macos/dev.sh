#!/bin/bash
# dev.sh [TEST…] — the compiler as it is in src1/, built natively (build/macos/nihilc1-dev), then
# each test (tests/stage1/NAME) compiled with it and compared with its .out / .err
set -u
cd "$(dirname "$0")/.."
make -s build/prelude_text.nihil build/std_text.nihil || exit 1
macos/translator.sh || exit 1
w=build/macos/dev; mkdir -p $w
printf '%s\n' "-map $PWD/src1/nihilc1.nihil" | build/macos/nihilc1-native > $w/c.x86 2> $w/c.map || { grep -v nihil-map $w/c.map; exit 1; }
printf '%s\n%s\n%s\n' $w/c.x86 $w/c.map $w/c.s | build/macos/translate 2>/dev/null && clang -arch arm64 -o build/macos/nihilc1-dev $w/c.s build/macos/rt.o || exit 1
for t in "$@"; do
  src=tests/stage1/$t.nihil
  if [ -f tests/stage1/$t.err ]; then
    printf '%s\n' "$PWD/$src" | build/macos/nihilc1-dev > /dev/null 2> $w/t.err
    grep -qF "$(cat tests/stage1/$t.err)" $w/t.err && echo "ok    $t" || { echo "FAIL  $t: $(head -2 $w/t.err)"; }
    continue
  fi
  printf '%s\n' "-map $PWD/$src" | build/macos/nihilc1-dev > $w/t.x86 2> $w/t.map || { echo "FAIL  $t (compile): $(grep -v nihil-map $w/t.map | head -3)"; continue; }
  printf '%s\n%s\n%s\n' $w/t.x86 $w/t.map $w/t.s | build/macos/translate 2>/dev/null && clang -arch arm64 -o $w/t.bin $w/t.s build/macos/rt.o || { echo "FAIL  $t (translate)"; continue; }
  $w/t.bin > $w/t.out 2> $w/t.stderr < /dev/null
  if cmp -s $w/t.out tests/stage1/$t.out; then echo "ok    $t"; else echo "FAIL  $t:"; diff tests/stage1/$t.out $w/t.out | head -12; head -3 $w/t.stderr; fi
done
