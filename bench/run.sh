#!/bin/sh
# bench/run.sh [COMPILER] — bench/tables.nihil for 10 000, 100 000 and 1 000 000 records (SIZES), each
# with its peak memory. COMPILER is a command like `nihil` (the default) or ./nihilc1.sh: it is
# called as COMPILER PROGRAM.nihil OUT.
set -eu
cd "$(dirname "$0")/.."
compiler=${1:-nihil}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" bench/tables.nihil "$work/tables"
if [ "$(uname -s)" = Darwin ]; then
    echo "$(sysctl -n machdep.cpu.brand_string), macOS $(sw_vers -productVersion) (the x86-64 code translated to ARM64)"
else
    echo "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'), $(nproc) CPUs, Linux $(uname -r)"
fi
for n in ${SIZES:-10000 100000 1000000}; do
    echo
    if [ "$(uname -s)" = Darwin ]; then
        N=$n DIR=$work/data /usr/bin/time -l "$work/tables" 2> "$work/time"
        mem=$(awk '/maximum resident set size/ { print int($1 / 1048576) }' "$work/time")
    else
        N=$n DIR=$work/data /usr/bin/time -v "$work/tables" 2> "$work/time"
        mem=$(awk -F: '/Maximum resident set size/ { print int($2 / 1024) }' "$work/time")
    fi
    echo "peak memory: $mem MB"
done
