#!/bin/sh
# bench/workload/run.sh [COMPILER] — workload.nihil against the same program in C (cc -O2) and Go, for
# SIZES sales (default 100 000 and 1 000 000), ROUNDS runs each (default 5, the three languages taking
# turns), with the median of each phase (timed inside each program, the same way), of their sum and of
# the peak memory. Every run's JSON and
# table must be the same bytes as the first one's, or it stops. COMPILER is called as
# COMPILER PROGRAM.nihil OUT: `nihil` (the default) or ./nihilc1.sh from a build. Needs cc and go.
set -eu
export LC_ALL=C
cd "$(dirname "$0")/../.."
compiler=${1:-nihil}
rounds=${ROUNDS:-5}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" bench/workload/workload.nihil "$work/nihil"
cc -O2 -o "$work/c" bench/workload/workload.c
(cd bench/workload && go build -o "$work/go" workload.go)
if [ "$(uname -s)" = Darwin ]; then
    machine="$(sysctl -n machdep.cpu.brand_string), macOS $(sw_vers -productVersion); Nihil's x86-64 code translated to ARM64, C and Go native"
    timer="/usr/bin/time -l"
else
    machine="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'), $(nproc) CPUs, Linux $(uname -r); all three native"
    timer="/usr/bin/time -v"
fi
echo "$machine"
echo "$("$compiler" version 2>/dev/null || echo nihil) · $(cc --version | head -1) · $(go version | cut -d' ' -f3)"
for n in ${SIZES:-100000 1000000}; do
    : > "$work/results"
    r=1
    while [ "$r" -le "$rounds" ]; do
        for lang in nihil c go; do
            mkdir -p "$work/d"
            N=$n DIR=$work/d $timer "$work/$lang" > "$work/out" 2> "$work/err"
            if [ -f "$work/first.json" ]; then
                cmp -s "$work/d/out.json" "$work/first.json" && cmp -s "$work/d/sales.dat" "$work/first.dat" && cmp -s "$work/out" "$work/first.out" \
                    || { echo "$lang ($n sales) wrote other bytes" >&2; exit 1; }
            else
                cp "$work/d/out.json" "$work/first.json"; cp "$work/d/sales.dat" "$work/first.dat"; cp "$work/out" "$work/first.out"
            fi
            awk -v lang=$lang '
                /^(generate|table|aggregate|rank|json) [0-9]+$/ { printf "%s %s %s\n", lang, $1, $2; total += $2 }
                END { printf "%s total %d\n", lang, total }
                /Maximum resident set size/ { printf "%s memory %d\n", lang, $NF / 1024 }
                /maximum resident set size/ { printf "%s memory %d\n", lang, $1 / 1048576 }' "$work/err" >> "$work/results"
            rm -rf "$work/d"
        done
        r=$((r + 1))
    done
    echo
    echo "$n sales: $(cat "$work/first.out")"
    echo "| ms (median of $rounds) | Nihil | C | Go | Nihil / C | Nihil / Go |"
    echo "|---|---:|---:|---:|---:|---:|"
    for what in generate table aggregate rank json total memory; do
        awk -v what=$what '
            $2 == what { v[$1, ++n[$1]] = $3 }
            function med(l,   k, i, j, t, a) { k = n[l]; for (i = 1; i <= k; i++) a[i] = v[l, i]
                for (i = 1; i <= k; i++) for (j = i + 1; j <= k; j++) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
                return a[int((k + 1) / 2)] }
            function ratio(a, b) { return b > 0 ? sprintf("%.1f×", a / b) : "–" }
            END { a = med("nihil"); b = med("c"); g = med("go"); label = what; if (what == "memory") label = "peak memory (MB)"
                  printf "| %s | %d | %d | %d | %s | %s |\n", label, a, b, g, ratio(a, b), ratio(a, g) }' "$work/results"
    done
done
