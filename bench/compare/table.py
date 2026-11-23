# bench/compare/table.py DIR SIZE… — the table of bench/compare/run.sh: each operation, in the order
# the three programs print them, with Nihil's time, C's and Go's (and, for one record at a time, each).
import re, sys
work, sizes = sys.argv[1], sys.argv[2:]
NAMES = ["make the records in memory", "write the table (atomic, synced)", "read the whole table",
         "where(active && price < 200.0)", "count(category == 7)", "sum(price * qty)", "sort(-price)",
         "first key lookup (builds the index)", "lookup by key", "read record i",
         "change by key (1 fsync)", "add by key (2 fsync)", "delete by key (2 fsync)"]
def rows(lang, n):
    out = []
    for line in open(f"{work}/{lang}-{n}.txt"):
        m = re.search(r": ([0-9.]+) ms(?: \((\d+), (?:< )?([0-9.]+) µs each\))?", line)
        if m: out.append((float(m.group(1)), m.group(2), m.group(3)))
        m = re.match(r"peak memory: (\d+) MB", line)
        if m: mem = m.group(1)
    return out, mem
def cell(t, single):
    ms, count, each = t
    if single: return f"{float(each):.1f} µs" if float(each) < 1000 else f"{float(each) / 1000:.2f} ms"
    return f"{ms:.0f} ms" if ms >= 10 else f"{ms:.1f} ms" if ms > 0 else "< 10 ms"
for n in sizes:
    r = {lang: rows(lang, n) for lang in ("nihil", "c", "go")}
    print(f"\n{int(n):,} records".replace(",", " "))
    print(f"| operation | Nihil | C | Go |\n|---|---|---|---|")
    for i, name in enumerate(NAMES):
        single = i >= 8
        print(f"| {name}{' (each)' if single else ''} | " + " | ".join(cell(r[l][0][i], single) for l in ("nihil", "c", "go")) + " |")
    print("| peak memory | " + " | ".join(r[l][1] + " MB" for l in ("nihil", "c", "go")) + " |")
