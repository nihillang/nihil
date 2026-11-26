#!/bin/bash
# test-native-c.sh — the test battery (tests/ and tests/stage1/) compiled straight into ARM64 by the
# compiler (-native) and linked with the C runtime (macos/native/rt.c): no translator. Compared with
# each test's .out, .code and .stderr (SPEC §13), as macos/test-native.sh does.
#   NIHIL_O0=1: no optimizations; NIHILC1_NATIVE=path: another native compiler
#   VERBOSE=1: the link errors in full (the routines the C runtime still lacks)
cd "$(dirname "$0")/.."
T=build/macos/tc
mkdir -p $T
native=${NIHILC1_NATIVE:-build/macos/nihilc1-native}
opt=""; [ "${NIHIL_O0:-0}" = 1 ] && opt="-O0 "
{ clang -O2 -arch arm64 -Wno-unused-result -c macos/native/rt.c -o $T/a.o && clang -O2 -arch arm64 -Wno-unused-result -c arm64/rt.c -o $T/b.o && ld -r $T/a.o $T/b.o -o $T/rt.o; } || exit 1
ok=0; failed=0; skipped=0; nolink=0
: > $T/missing
for src in tests/*.nihil tests/stage1/*.nihil; do
  n=$(echo $src | tr / _); name=${src%.nihil}
  if ! printf "%s\n" "$opt-native $PWD/$src" | $native > $T/$n.s 2> $T/$n.msg; then skipped=$((skipped + 1)); continue; fi
  if ! clang -arch arm64 -o $T/$n.bin $T/$n.s $T/rt.o 2> $T/$n.cerr; then
    nolink=$((nolink + 1)); grep -o '"_nh_[a-z_]*"' $T/$n.cerr >> $T/missing
    [ "${VERBOSE:-0}" = 1 ] && echo "LINK    $src: $(grep -o '_nh_[a-z_]*' $T/$n.cerr | sort -u | tr '\n' ' ')$(grep -v Undefined $T/$n.cerr | head -2 | tr '\n' ' ')"
    continue
  fi
  input=/dev/null; [ -f $name.in ] && input=$name.in
  perl -e 'alarm shift; exec @ARGV' ${TIMEOUT:-60} $T/$n.bin < $input > $T/$n.out 2> $T/$n.err; code=$?
  exp=0; [ -f $name.code ] && exp=$(cat $name.code)
  : > $T/$n.exp; [ -f $name.out ] && cp $name.out $T/$n.exp
  if [ "$code" != "$exp" ]; then failed=$((failed + 1)); echo "FAILED  $src: exit $code, expected $exp $(head -c 120 $T/$n.err)"; continue; fi
  if ! cmp -s $T/$n.out $T/$n.exp; then failed=$((failed + 1)); echo "FAILED  $src: output differs"; diff $T/$n.exp $T/$n.out | head -6; continue; fi
  if [ -f $name.stderr ] && ! cmp -s $T/$n.err $name.stderr; then failed=$((failed + 1)); echo "FAILED  $src: stderr differs"; diff $name.stderr $T/$n.err | head -4; continue; fi
  ok=$((ok + 1))
done
echo "$ok passed, $failed failed, $nolink need routines the C runtime lacks, $skipped did not compile"
[ -s $T/missing ] && { echo "routines missing (programs needing each):"; sort $T/missing | uniq -c | sort -rn | head -40; }
