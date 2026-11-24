#!/bin/bash
# translator.sh — build/macos/translate, the translator (macos/translate.nihil) as a native program, made
# again when its source changed (build/macos/translate.nihil is the copy it was made from): the native
# compiler emits its x86-64 code, the translator there is (or a seed, macos/seed.sh) translates it, and
# the new one must translate itself into the same assembly (a fixed point). Without a native compiler yet,
# the seed is the translator until there is one.
set -eu
cd "$(dirname "$0")/.."
t=build/macos/translate
[ -x $t ] && cmp -s $t.nihil macos/translate.nihil && exit 0
mkdir -p build/macos
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
if [ -x $t ]; then cp $t $w/old; else macos/seed.sh $w/old; fi
if [ ! -x build/macos/nihilc1-native ]; then cp $w/old $t; rm -f $t.nihil; exit 0; fi
[ -f build/macos/rt.o ] && [ build/macos/rt.o -nt arm64/rt.c ] || clang -O2 -arch arm64 -Wno-unused-result -c arm64/rt.c -o build/macos/rt.o
make -s build/prelude_text.nihil build/std_text.nihil
printf '%s\n' "-map $PWD/macos/translate.nihil" | build/macos/nihilc1-native > $w/t.x86 2> $w/t.map ||
    { grep -v '^nihil-map' $w/t.map >&2; exit 1; }
printf '%s\n%s\n%s\n' $w/t.x86 $w/t.map $w/t.s | $w/old 2> /dev/null
clang -arch arm64 -o $w/new $w/t.s build/macos/rt.o
printf '%s\n%s\n%s\n' $w/t.x86 $w/t.map $w/t2.s | $w/new 2> /dev/null
cmp -s $w/t.s $w/t2.s || { echo "macos/translator.sh: the new translator does not translate itself into the same assembly" >&2; exit 1; }
mv $w/new $t
cp macos/translate.nihil $t.nihil
