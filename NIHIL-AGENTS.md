# Nihil for coding agents (short reference, v4.9)

Nihil has **no keywords**: blocks are driven by the type of the expression before `{`.
Statements end with `;`. Comments: `//`, `/* */`. Compile: `nihil prog.nihil out`; run: `nihil run prog.nihil`.
Full spec only if needed: `SPEC.md` §5 optionals, §20 records, §21 library, §22 MAP, §28 LIST.

## From C / Go / Python

| You would write | Nihil |
|---|---|
| `if (c) {…} else if (d) {…} else {…}` | `c { … } \|\| d { … } \|\| { … }` |
| `while (c) {…}` | `* c { … }` |
| `for (i = 0; i < n; i++)` | `i = 0; * i < n { …; i = i + 1; }` or `n { … }` (repeat n times, no index) |
| `for x in v` / `for i, x := range v` | `v : x { … }` / `v : x, i { … }` (x, i must be names used nowhere else, not even as a global later in the file) |
| `x += 1; x++` | `x = x + 1;` (no compound operators) |
| `int x = 0; var x = 0` | `x = 0;` (type inferred, then locked) |
| `print(x)`, `printf("%d\n", x)` | `>> x; >> "\n";` (`>>` never adds a newline) |
| `scanf/input()` (one value per line) | `x = 0; << x;` (x must exist; its type picks the parser) |
| `return v; return;` | `: v;` / `:;` |
| `def f(a, b):` / `func f(a, b int64) int64` | `f(a = 0, b = 0) { … }` (literal defaults give the types) |
| `exit(n)` | `:: n;` |
| `null` / `None` / `nil` | `_` (only for optionals, `x? = 0;`) |
| `str(n)`, `"a" + n` | `"a" + $n` (`$` turns INT/FLOAT/BOOL into text) |
| `len(s)` | `#s` |
| `a and b`, `a or b`, `not a` | `a && b`, `a \|\| b`, `!a` |

## Traps (read these)

1. **An INT before `{` is a repeat count, not a truth test.** `n { … }` runs n times; write `n != 0 { … }` for a test.
2. **`||` has three roles:** after `}` it is else/else-if; with an optional on the left it is a fallback
   (`v = opt || 0;`); with a plain BOOL on the left it is logical OR. For OR on an optional BOOL: `(x || false) || y`.
3. **Types are locked** at the first assignment; no implicit conversions. INT→FLOAT: `.n` or `.(expr)` (`.7` is a
   syntax error: write `0.7`); FLOAT→INT: `#f` (truncates).
4. **Locals never hide globals.** Assigning to a name that exists at top level, inside any block or method,
   changes the global. Only parameters can shadow. Give helper locals distinct names.
5. Integer math is int64, wraps on overflow; `/` truncates toward zero, `%` has the dividend's sign (as C).
6. Bit operators bind tighter than comparisons (`a & 1 == 0` is `(a & 1) == 0`); `a < b < c` is an error.
7. `<<`/`>>` at the start of a statement are input/output; inside expressions they are shifts.
8. No empty literals: use `[0 ; 0]` (array), `["" ; *]` (list), `{"": 0 ; 16}` (map).
9. Strings are immutable bytes; `s[i]` is an INT 0–255. `+` joins only STRING + STRING.
10. A method used as a value must end every path with `: value;` (otherwise a runtime error).

## Core syntax

```nihil
n = 0; total = 0; best = -1;
<< n;
i = 0;
* i < n {
    v = 0; << v;
    v > best { best = v; }
    v % 2 == 0 { total = total + v; } || v < 0 { total = total - 1; } || { }
    i = i + 1;
}
>> total; >> "\n"; >> best; >> "\n";
```

Methods (top level only; arguments may be omitted from the right):

```nihil
clamp(x = 0, lo = 0, hi = 100) {
    x < lo { : lo; }
    x > hi { : hi; }
    : x;
}
find(v = [0], x = 0) {          // returns INT? because of `: _;`
    v : e, i { e == x { : i; } }
    : _;
}
pos = find([4, 8, 15], 8) || -1;
>> clamp(150); >> " "; >> pos; >> "\n";
```

## Values

- Types: `INT`, `FLOAT` (`1.5`, needs digits on both sides), `BOOL`, `STRING`, `ARRAY` (fixed size, one element type),
  `LIST`, `MAP`, records, optionals `T?`. Arrays, lists, maps, records and strings are references.
- Arrays: `a = [1, 2, 3]; z = [0 ; 100]; a[0] = 9; #a`. Out-of-bounds index is a runtime error (exit 2).
- Optionals: `b? = 0; b = _; b == _ { … } x = b || 7;` — `||` is the only way to read the value.
- LIST: `l = [0 ; *]; l[] = 5; #l; l : x { … }`.
- MAP: `m = {"a": 1}; m["b"] = 2; c = m["z"] || 0; m["a"] = _;` (delete); `m.keys()`, `m.contains(k)`.
- Records: `P = @[code: INT(6); name: STRING(12)];` (top level) then `p = P(7, "x"); p.name`.

## Library (method calls on values; each returns a new value)

STRING: `find(t)` (INT?), `contains`, `starts_with`, `ends_with`, `slice(a, b)`, `split(sep)`, `replace(a, b)`,
`trim`, `upper`, `lower`, `repeat(n)`, `int()` (INT?), `float()` (FLOAT?), `json(key)` (STRING?).
ARRAY: `sort`, `find(x)` (INT?), `contains`, `reverse`, `sum`, `min`/`max` (optional), `join(sep)` (STRING arrays).
Records array: `rows.where(cond)`, `count(cond)`, `first(cond)`, `sort(key)`, `sum(expr)` (field names inside).

## I/O and program control

`>> x;` stdout, `!>> x;` stderr, `<< x;` read a line (end of input gives 0/""/false, or `_` for optionals),
`@path` read a file (STRING), `@>> path, text;` write a file, `@ "other.nihil";` include (top level, own line),
`:: code;` exit. Runtime errors print `nihil: <reason>` and exit 2. `r? = ?f();` catches a runtime error (`?!` = message).
