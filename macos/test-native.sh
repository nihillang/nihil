#!/bin/bash
# test.sh — the test battery (tests/ and tests/stage1/) run natively on macOS: each program is
# compiled by the native compiler (build/macos/nihilc1-native) (-map), translated to ARM64 (macos/translate.nihil), built with
# clang and arm64/rt.c, run, and compared with its .out, .code and .stderr (SPEC §13)
cd "$(dirname "$0")/.."
T=build/macos/t
mkdir -p $T
# 1. compile every program in the VM at once (the compiler is x86)
#    NIHIL_ARM=1: the compiler writes the program in ARM64 (-arm); NIHIL_O0=1: no optimizations;
#    NIHILC1_NATIVE=path: another native compiler
native=${NIHILC1_NATIVE:-build/macos/nihilc1-native}
opt=""; [ "${NIHIL_O0:-0}" = 1 ] && opt="-O0 "; [ "${NIHIL_ARM:-0}" = 1 ] && opt="$opt-arm "
for src in tests/*.nihil tests/stage1/*.nihil; do n=$(echo $src | tr / _); printf "%s\n" "$opt-map $PWD/$src" | $native > $T/$n.x86 2> $T/$n.map; done
macos/translator.sh || exit 1
ok=0; failed=0; skipped=0
clang -O2 -arch arm64 -Wno-unused-result -c arm64/rt.c -o $T/rt.o
for src in tests/*.nihil tests/stage1/*.nihil; do
  n=$(echo $src | tr / _); name=${src%.nihil}
  if ! grep -q nihil-map $T/$n.map 2>/dev/null; then skipped=$((skipped + 1)); continue; fi
  if ! printf '%s\n%s\n%s\n' $T/$n.x86 $T/$n.map $T/$n.s | build/macos/translate 2> $T/$n.terr; then
    failed=$((failed + 1)); echo "FAILED  $src: translation: $(head -3 $T/$n.terr | tr '\n' ' ')"; continue
  fi
  if ! clang -arch arm64 -o $T/$n.bin $T/$n.s $T/rt.o 2> $T/$n.cerr; then
    failed=$((failed + 1)); echo "FAILED  $src: clang: $(head -2 $T/$n.cerr | tr '\n' ' ')"; continue
  fi
  input=/dev/null; [ -f $name.in ] && input=$name.in
  (cd "$(dirname "$src")/.." 2>/dev/null; cd "$OLDPWD"; perl -e 'alarm shift; exec @ARGV' 60 $T/$n.bin < $input > $T/$n.out 2> $T/$n.err); code=$?
  exp=0; [ -f $name.code ] && exp=$(cat $name.code)
  : > $T/$n.exp; [ -f $name.out ] && cp $name.out $T/$n.exp
  if [ "$code" != "$exp" ]; then failed=$((failed + 1)); echo "FAILED  $src: exit $code, expected $exp $(head -c 120 $T/$n.err)"; continue; fi
  if ! cmp -s $T/$n.out $T/$n.exp; then failed=$((failed + 1)); echo "FAILED  $src: output differs"; diff $T/$n.exp $T/$n.out | head -6; continue; fi
  if [ -f $name.stderr ] && ! cmp -s $T/$n.err $name.stderr; then failed=$((failed + 1)); echo "FAILED  $src: stderr differs"; diff $name.stderr $T/$n.err | head -4; continue; fi
  ok=$((ok + 1))
done
echo "$ok passed, $failed failed, $skipped did not compile (compile-error tests are not run here)"
