#!/bin/bash
# same-bytes.sh — on the Mac, the guard of a refactor of the compiler that must not change its output
# (what `make compare` and `make fixedpoint` prove on Linux, where stage 0 exists).
#
#   macos/same-bytes.sh --baseline   keeps the current native compiler as the reference
#                                    (build/macos/nihilc1-ref); run it before changing src1/
#   macos/same-bytes.sh              builds src1/ with the reference (a candidate compiler), then
#                                    compiles every program of the corpus with both, with and without
#                                    -O0, and compares the x86-64 output, the map, the messages and
#                                    the exit status, byte for byte
#
# The corpus: tests/, tests/stage1/, the compiler itself, the translator, the assembler, the IDE's
# server, bench/, examples/, site/. EXTRA="a.nihil b.nihil" adds programs of your own.
#   JOBS=N   parallel compilations (default: the number of CPUs)
set -u
cd "$(dirname "$0")/.."
ref=build/macos/nihilc1-ref
if [ "${1:-}" = --baseline ]; then
    cp build/macos/nihilc1-native "$ref" && echo "reference: $ref (from build/macos/nihilc1-native)"
    exit
fi
[ -x "$ref" ] || { echo "same-bytes: no reference; run macos/same-bytes.sh --baseline first" >&2; exit 2; }
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
[ -f build/macos/rt.o ] && [ build/macos/rt.o -nt arm64/rt.c ] || clang -O2 -arch arm64 -Wno-unused-result -c arm64/rt.c -o build/macos/rt.o
make -s build/prelude_text.nihil build/std_text.nihil

# the candidate: src1/ as it is now, compiled by the reference
printf '%s\n' "-map $PWD/src1/nihilc1.nihil" | "$ref" > "$w/c.x86" 2> "$w/c.map" ||
    { grep -v '^nihil-map' "$w/c.map" >&2; echo "same-bytes: src1/ does not compile" >&2; exit 1; }
printf '%s\n%s\n%s\n' "$w/c.x86" "$w/c.map" "$w/c.s" | build/macos/translate 2> "$w/t.err" || { cat "$w/t.err" >&2; exit 1; }
clang -arch arm64 -o "$w/cand" "$w/c.s" build/macos/rt.o || exit 1
cp "$w/cand" build/macos/nihilc1-cand

ls tests/*.nihil tests/stage1/*.nihil src1/nihilc1.nihil macos/translate.nihil asm/asm.nihil \
   ide/server.nihil bench/*.nihil examples/*.nihil site/*.nihil ${EXTRA:-} 2> /dev/null > "$w/corpus"

one() {   # one program, one option: the same output from both compilers?
    w=$1; opt=$2; p=$3
    tag=$(printf '%s' "$opt$p" | shasum | cut -c1-12)
    for c in ref cand; do
        bin=$w/cand; [ $c = ref ] && bin=$REF
        printf '%s\n' "$opt-map $PWD/$p" | "$bin" > "$w/$tag.$c.out" 2> "$w/$tag.$c.err"
        echo $? >> "$w/$tag.$c.err"
    done
    if cmp -s "$w/$tag.ref.out" "$w/$tag.cand.out" && cmp -s "$w/$tag.ref.err" "$w/$tag.cand.err"; then
        rm -f "$w/$tag".*; echo ok
    else
        echo "DIFFERS: ${opt:-(optimized) }$p"
    fi
}
export -f one
export REF=$PWD/$ref
{ sed 's/^/x /' "$w/corpus"; sed 's/^/-O0 /' "$w/corpus"; } |
    xargs -P "${JOBS:-$(sysctl -n hw.ncpu)}" -L 1 bash -c 'o=$0; [ "$o" = x ] && o=""; [ -n "$o" ] && o="$o "; one "'"$w"'" "$o" "$1"' > "$w/result"
n=$(grep -c '^ok$' "$w/result")
bad=$(grep -v '^ok$' "$w/result")
if [ -n "$bad" ]; then
    echo "$bad"
    echo "same-bytes: $n the same, $(printf '%s\n' "$bad" | wc -l | tr -d ' ') different" >&2
    exit 1
fi
echo "same-bytes: all $n compilations give the same bytes ($(wc -l < "$w/corpus" | tr -d ' ') programs, optimized and -O0)"
