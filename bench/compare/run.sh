#!/bin/sh
# bench/compare/run.sh [COMPILER] — bench/tables.nihil against the same work in C (cc -O2) and Go,
# for 100 000 and 1 000 000 records (SIZES), each with its peak memory, then a table of the three
# (bench/compare/table.py). COMPILER is called as COMPILER PROGRAM.nihil OUT: `nihil` (the default)
# or ./nihilc1.sh from a build. Needs cc, go and python3.
set -eu
cd "$(dirname "$0")/../.."
compiler=${1:-nihil}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" bench/tables.nihil "$work/nihil"
cc -O2 -o "$work/c" bench/compare/tables.c
go build -o "$work/go" bench/compare/tables.go
if [ "$(uname -s)" = Darwin ]; then
    machine="$(sysctl -n machdep.cpu.brand_string), macOS $(sw_vers -productVersion); Nihil's x86-64 code translated to ARM64, C and Go native"
else
    machine="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'), $(nproc) CPUs, Linux $(uname -r); all three native"
fi
echo "$machine"
echo "cc: $(cc --version | head -1); $(go version)"
for n in ${SIZES:-100000 1000000}; do
    for lang in nihil c go; do
        if [ "$(uname -s)" = Darwin ]; then
            N=$n DIR=$work/data /usr/bin/time -l "$work/$lang" > "$work/$lang-$n.txt" 2> "$work/time"
            awk '/maximum resident set size/ { print "peak memory: " int($1 / 1048576) " MB" }' "$work/time" >> "$work/$lang-$n.txt"
        else
            N=$n DIR=$work/data /usr/bin/time -v "$work/$lang" > "$work/$lang-$n.txt" 2> "$work/time"
            awk -F: '/Maximum resident set size/ { print "peak memory: " int($2 / 1024) " MB" }' "$work/time" >> "$work/$lang-$n.txt"
        fi
    done
done
python3 bench/compare/table.py "$work" ${SIZES:-100000 1000000}
