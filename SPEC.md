# Nihil — Language Specification (v4.0)

> **Nihil**: an imperative, minimalist language compiled straight to x86_64 machine
> code, with no libc, no linker and no VM. The name is Latin for
> *nothing*: Nihil programs have no keywords at all, only symbols, and `_` is the
> value that is not there. Source files use the `.nihil` extension; the
> compiler is `nihilc`.
>
> Appendix A lists every change since v1.0 and the reason for it. v2.0 renamed
> the language (it was *Cru*, then *Ket*; *Nihil* since v2.2) and moved the whole
> project to English.

---

## 1. Overview and constraints

- **Philosophy:** imperative and structured, ultra-minimal syntax, **no
  keywords** (there is no `if`, `else`, `for`, `while`, `fn`, `let`, `return`
  or `null`), and **strong static typing by inference**. The only reserved
  names are the literals `true` and `false` and the symbol `_`.
- **Target:** native x86_64 machine code for Linux, with no libc: only direct
  system calls.
- **Output:** an **ELF64** executable that the compiler assembles in memory and
  writes with mode `0755`. No `ld`.
- **Bootstrap compiler (stage 0):** x86_64 assembly (NASM, Intel syntax),
  using only `openat`, `read`, `write`, `close`, `mmap`, `chmod` and `exit`.
- **Self-hosting:** stage 0 implements just the **core** needed to rewrite the
  compiler in Nihil (§10). Stage 1 (`nihilc1`, §15) is that compiler, written in
  Nihil; it compiles itself and implements the rest of the language.

Usage:

```text
nihilc input.nihil [output]       # default output: a.out
```

---

## 2. Lexical structure

| Element | Form |
|---|---|
| Identifier | `[A-Za-z_][A-Za-z0-9_]*`, except a lone `_` |
| Integer | decimal `123`, hexadecimal `0x7F` |
| Character | `'a'`, `'\n'` → an **INT** holding the byte value |
| Float | `0.1`, `3.0`, `1.5e-10` (needs digits on both sides of the point) |
| String | `"text"` |
| Boolean | `true`, `false` |
| Absence | `_` |
| Comments | `// to the end of the line` and `/* block */` (they do not nest) |

Escapes in strings and characters: `\n \t \r \0 \\ \' \" \xHH`.

Spaces, tabs and line breaks only separate tokens. `;` ends a command.

---

## 3. Types and representation

Every value fits in **one 64-bit register**, except optionals.

| Type | Representation |
|---|---|
| `INT` | signed 64-bit integer |
| `FLOAT` | IEEE-754 `double`. Its 64 bits live in `rax` like any other value; arithmetic goes through `xmm0` and `xmm1` (SSE2) |
| `BOOL` | 0 or 1 in a 64-bit register; 8 bytes in memory |
| `STRING` | pointer to the bytes; the **length is in the 8 bytes before the pointer**, and a 0 byte follows the text. Immutable |
| `ARRAY` | pointer to 8-byte elements; the **length is in the 8 bytes before**. Fixed size, mutable contents |
| optional `T?` (any T) | value + presence flag: `rax` + `rdx` in registers, two 8-byte words in memory. `_` is value 0 and flag 0 |

- Arrays and strings are passed and assigned **by reference**: after `a = b;`
  both names point to the same block.
- An array's type is inferred from its first element. All elements must have
  the same type.
- There are no implicit conversions between types.

---

## 4. Variables and scope

- A variable is created by its first assignment. Its type comes from the value
  and is **locked**: assigning a value of another type is a compile error.
- **Globals** are the variables assigned at the top level, outside any block.
  The prescan collects them (§9.2), so every method sees them, whatever their
  order in the text.
- **Locals** are created inside a method or a `{ }` block and disappear at the
  `}` that closes the block.
- **Name lookup:** the current block, then the enclosing blocks, then the
  method's parameters, then the globals. If the name is found nowhere, the
  assignment creates a local.
- **Locals never hide globals.** If a name belongs to a global, an assignment
  inside a block or method changes the global, whatever the order of the
  declarations. Only a **parameter**, which is declared explicitly, can have the
  name of a global and hide it inside its method. A parameter with a method's
  name is an error.
- Reading a variable that does not exist yet is a compile error.
- **A global is born at the top level.** If a name is assigned at the top level,
  using it inside a block *before* that assignment in the text is a compile
  error, not a local with the same name:
  ```text
  3 { total = total + 1; }   // error: 'total' does not exist here yet (it is created on line 2)
  total = 0;
  ```
  This way a global is never read before it gets a value, and no name means two
  different things in the same program.
- **Inside methods** this ordering rule does not apply: a method may use a
  global created further down. If it runs before that assignment, it reads the
  initial value: `0`, `false` or `""`. STRING and ARRAY globals start as empty
  values, never as null pointers.

---

## 5. Absence (`_`) and optionals (`?`)

- `_` means "no value".
- Ordinary variables **do not accept** `_`.
- The `?` suffix at **creation** declares an optional: `b? = 10;`. From then on
  the variable is used without the `?`: `b = _;`, `b = 7;`.
- The initial value must be concrete, because it gives the type. To start
  empty, write `b? = 0; b = _;`. `b? = _;` is a compile error.
- `x == _` and `x != _` test for presence. They are only valid when `x` is
  optional.
- **The only way to extract the value is the `||` fallback:** `v = b || 5;`
  gives a non-optional `INT`.
- `>> b;` with an optional `b` prints the value, or `_` when it is empty.
- A T converts to T? automatically, and so does `_`, when assigning, passing an
  argument and returning. The other direction never happens automatically, only
  through `||`.
- A variable without `?` accepts neither T? nor `_`; the error message suggests
  `name? = …`.
- Optionals take part in no arithmetic, indexing, `#`, conditions or `==`, except
  `== _` and `!= _`. Arrays of optionals are not supported yet.
- **Optional parameters:** `f(n? = 0)` accepts INT, INT? or `_`. The default
  (when the argument is omitted) is present.
- **Optional return:** if a method has `: value;` of type T and also `: _;` (or a
  `: x;` where x is a T?), it returns T?.

  ```text
  find(v = [0], x = 0) {
      i = 0;
      * i < #v { v[i] == x { : i; } i = i + 1; }
      : _;                         // find returns INT?
  }
  pos = find(nums, 8) || -1;
  ```
- **`@? path`** is like `@` (§6.1), but returns `STRING?`: `_` when the file
  cannot be opened, instead of a runtime error.

### 5.1 The three roles of `||`

| Context | Meaning |
|---|---|
| After `}` | `else` / `else if` (§7.3) |
| **Optional** left side | fallback: the left value if present, otherwise the right one |
| Non-optional `BOOL` left side | logical OR with short-circuit |

An optional `BOOL` on the left follows the second row: `x || y` is a fallback.
For a logical OR with an optional `x`, write `(x || false) || y`.

---

## 6. Expressions

### 6.1 Precedence (lowest to highest)

| Level | Operators | Operands |
|---|---|---|
| 1 | `\|\|` | §5.1 |
| 2 | `&&` | BOOL, short-circuit |
| 3 | `== != < > <= >=` | same type; **non-associative** (`a < b < c` is an error) |
| 4 | `\|` | INT (bits) |
| 5 | `^` | INT (bits) |
| 6 | `&` | INT (bits) |
| 7 | `<< >>` | INT, shifts (`>>` is arithmetic) |
| 8 | `+ -` | INT or FLOAT; `+` also concatenates STRINGs |
| 9 | `* / %` | INT or FLOAT (`%` INT only) |
| 10 | unary `- ! ~ # @ @? $ .` | see below |
| 11 | postfix `x[i]`, `r.field`, `f(...)` | indexing, field, call |

Unary operators:

| Operator | Meaning |
|---|---|
| `-x` | negation (INT or FLOAT) |
| `!x` | BOOL not |
| `~x` | INT bitwise not |
| `#x` | length of a STRING or ARRAY; for a FLOAT, the integer part (truncation toward zero) |
| `@path` | the whole contents of a file, as a STRING (runtime error if it cannot be read) |
| `@?path` | the same, as a `STRING?` (`_` if it cannot be read) |
| `$bytes` | a new STRING with the low byte of each element of an ARRAY of INT |
| `.n` | INT → FLOAT |

- Bit operators sit **above** comparisons, so `a & 1 == 0` is `(a & 1) == 0`.
  This fixes C's classic trap.
- `<<` and `>>` are shifts **inside expressions**. At the start of a command they
  are input and output (§8). There is no ambiguity: in `>> a >> 2;` the first
  `>>` is the output command and the second is a shift.
- `==` between STRINGs compares their contents.
- `s[i]` on a STRING gives an INT from 0 to 255 and is read-only. `a[i]` on an
  ARRAY reads and writes.
- An **index out of bounds** ends the program with a runtime error (§8.3).
- INT arithmetic wraps around at 64 bits (`9223372036854775807 + 1` is the
  minimum).
- `/` truncates toward zero and `%` takes the sign of the dividend, as in C
  (`-7 / 2 == -3`, `-7 % 2 == -1`). `MIN / -1` gives `MIN` and `MIN % -1` gives
  `0`.
- Division by zero is a runtime error.
- Shifts use only the low 6 bits of the count (`1 << 64 == 1`).
- The largest decimal literal is `9223372036854775807`; the minimum is written
  `-9223372036854775807 - 1`. Hexadecimal literals take up to 16 digits and can
  express any 64-bit pattern (`0xFFFFFFFFFFFFFFFF == -1`).
- `$` is how text is built: fill an array and convert it. `$` also turns an INT,
  FLOAT or BOOL into its text (`$42` is `"42"`, `$2.5` is `"2.5"`, `$true` is
  `"true"`; stage 1), and the library (§21) has `s.int()` and `s.float()` for the
  other way.
  ```text
  buf = [0 ; 2];  buf[0] = 'o'; buf[1] = 'k';
  name = $buf;
  contents = @name;
  ```

### 6.1.1 FLOAT

- **Literals:** `0.5`, `3.0`, `1.5e-10`, `2.0E+8`. They need digits on both sides
  of the point; the exponent is optional. `.5` and `1e5` are errors. Literals are
  rounded correctly to the nearest `double`. Going past the largest `double` is
  a compile error.
- **Operations:** `+ - * /` and comparisons between two FLOATs, with IEEE-754
  semantics: `x / 0.0` is `inf`, `0.0 / 0.0` is `nan`, and with `nan` only `!=`
  is true. `%`, bit operators and mixing with INT are compile errors.
- **Conversions** (no keywords):
  - `.n`: INT → FLOAT ("gains the decimal point"), as in `.(#v)`;
  - `#f`: FLOAT → INT, truncating toward zero ("integer part"). Outside the INT
    range it gives the minimum INT.
- **Text:** `>>` writes the **shortest decimal that reads back as the same
  number** (`0.1`, not `0.10000000000000001`), always with a point (`3.0`). It
  uses scientific notation outside 10⁻⁷ … 10²¹ (`1.0e+21`, `5.0e-324`).
  Specials: `inf`, `-inf`, `nan`, `-0.0`.
- Arrays of FLOAT, FLOAT?, parameters (`x = 0.0`, `y = -1.5`) and return values
  work as for the other types.

### 6.1.2 Concatenation

`"a" + "b"` creates a new STRING. Only STRING + STRING; for numbers, build the
text with `$` or print it in parts.

### 6.2 Array constructors

```text
nums = [10, 20, 30];     // literal
buf  = [0 ; 65536];      // repetition: 65536 zeroed INTs
n    = #buf;             // 65536
```

An empty `[]` is a compile error, because there is nothing to take the type
from. Use `[0 ; 0]`.

- Elements can be INT, BOOL, STRING or FLOAT. Arrays of arrays are not
  supported yet.
- `a[i] = v;` changes an element. At the start of a command, `name[…]` is an
  assignment only if the matching `]` is followed by `=`; otherwise it is an
  expression, as in `flags[i] { … }` or `v[0] == 3 { … }`.
- Arrays cannot be compared with `==`.
- `a + b` joins two arrays of the same type into a new one, the elements of `a`
  first (`[1, 2] + [3]` is `[1, 2, 3]`; stage 1). Neither changes. Each `+`
  copies both, so growing an array one element at a time costs *n*² copies;
  for many elements, count first and fill `[x ; n]`.
- Arrays (and STRINGs and records) live in a heap with a **garbage
  collector**: what the program can no longer reach is freed and its memory
  used again (§12.4). There is nothing to free by hand.

---

## 7. Control flow (no keywords)

Every `{ ... }` block is governed by the expression right before it. The
**type** of that expression decides what the block does.

### 7.1 Plain scope (no prefix)

Runs once and just opens a scope.

```text
{ t = 1; b = b + t; }
```

### 7.2 Counted repetition (`INT` prefix)

```text
3 { b = b + 1; }
n * 2 { >> "."; }
```

- The count is evaluated **once**, on entry.
- A count `<= 0` does not run the block.
- The counter is hidden: a stack slot in stage 0, and in stage 1 a register when
  one is free. Neither `syscall` (which destroys `rcx` and `r11`), nor calls, nor
  nested loops affect it.

### 7.3 Conditional (`BOOL` prefix)

```text
x > 10 {
    >> "large";
} || x > 5 {
    >> "medium";
} || {
    >> "small";
}
```

- The branches are exclusive: only the first one whose condition is true runs.
- `} || cond {` works as `else if`; `} || {` as the final `else`.

### 7.4 Conditional loop (`*` prefix)

```text
* b < 10 { b = b + 1; }
```

The condition is evaluated again before each turn. A `*` at the start of a
command is always the loop; multiplication never appears in that position.

### 7.5 Prefixed calls

```text
3 blink();           // calls 3 times
m > 20 add(5);       // calls if m > 20
```

---

## 8. Input and output

### 8.1 Commands

| Command | Effect |
|---|---|
| `>> expr;` | Writes text: INT in decimal, BOOL as `true`/`false`, STRING as is, FLOAT as in §6.1.1, an empty optional as `_`. **No newline is added.** |
| `<< x;` | Reads a line (without the `\n`) and converts it to the type of `x` (§8.2). |
| `>>> expr;` | Writes **one raw byte**: the low 8 bits of an INT. |
| `<<< x;` | Reads **one raw byte** into the INT `x`; gives `-1` at the end of input. |
| `!>> expr;` | Like `>>`, but to **standard error**. It flushes standard output first, to keep the order on a terminal. |
| `:: expr;` / `::;` | **Ends the program from anywhere**, including from inside methods, with exit code `expr` (an INT) or 0. Output is flushed first. |
| `... expr;` | **Waits** `expr` milliseconds (an INT; `<= 0` does not wait). Output is flushed first, so what was written shows up before the pause. Uses `nanosleep` and resumes after a signal (stage 1). |
| `@>> path, text;` | **Writes a file**: creates it or replaces its contents with the STRING `text` (stage 1). Atomic: the text goes to `path.nihil-tmp`, which is synced to disk (`fsync`) and renamed over `path`, so a crash leaves the old file or the new one, never a mix. A new file gets mode `0644` (minus the umask); a replaced file gets those permissions too. |
| `@+>> path, text;` | **Appends** `text` to the file, creating it if needed, and syncs it to disk before going on (stage 1). |
| `@[i]>> path, r;` | **Changes record `i`** of a table file in place, writing only that line (§20.4, stage 1). |
| `@{}>> path, r;` | **Writes `r` under its key**: replaces the record with the same key, or adds `r` (§20.5, stage 1). |
| `NAME @{k} path = _;` | **Deletes** the record with key `k`; `NAME @?{k} path = _;` does nothing if there is none (§20.5, stage 1). |

### 8.1.1 Writing files

Both `path` and `text` are STRINGs (bytes: use `$` to write an ARRAY of INT).
If the file cannot be written, the program **ends with a runtime error** and
exit code 2, naming the file and the reason:

```text
nihil: could not write 'notes/today.txt': no such file or directory
```

The reasons spelled out are: no such file or directory, permission denied, it
is a directory, a part of the path is not a directory, no space left on the
device, read-only file system, the path is too long (over 4080 bytes), disk
quota exceeded; any other one is shown as `error N` (the `errno`). A failed
`@>>` removes its temporary file. Both commands sync every write, which makes
them durable but slow in a tight loop (a few milliseconds each): build the text
first and write it once when you can.

### 8.2 Rules for `<<`

- `<< x;` requires that `x` already exist: its type says how to convert the
  line (INT, BOOL, STRING or FLOAT, optionals included). A `\r` before the `\n`
  is dropped.
- With an optional `x`, at the end of input `x` gets `_`.
- With a non-optional `x`, at the end of input `x` gets `0`, `""`, `false` or
  `0.0`.
- Text that is not valid for the type is a runtime error
  (`nihil: the input is not a valid INT: 'twelve'`).

### 8.3 Buffers and runtime errors

- Standard output goes through a 64 KiB buffer. It is flushed when full, at the
  end of the program and before any read (`<<` or `<<<`), so prompts appear
  before the program waits for input.
- A runtime error (index out of bounds, division by zero, invalid input, …)
  flushes the output, writes `nihil: <reason>` to standard error and exits with
  code **2**. The existing ones are: `division by zero`,
  `index 5 out of bounds (size 3)`, `negative array size`, `array too large`,
  `out of memory`, `could not read 'x'`,
  `method 'f' ended without returning a value` and the input errors of `<<`.

---

## 9. Methods

### 9.1 Declaration, call and return

```text
max(a = 0, b = 0) {
    a > b { : a; }
    : b;
}

m = max(10, 25);
```

- Methods can only be declared **at the top level**.
- At the top level, `name() {` and `name(p = … ) {` are **declarations**: empty
  parentheses, or parentheses starting with `name =` or `name?`. Any other form
  is a call: `even(4) { … }` calls `even` and uses the result for the block.
  Inside a block or method, `f() { … }` is always a call.
- Each parameter's type comes from its default value: `x = 0` gives INT,
  `x? = 0` an optional INT, `v = [0]` an ARRAY of INT.
- **The default must be a literal**: `0`, `-1`, `0xFF`, `'a'`, `"text"`,
  `true`, `false` or a FLOAT. It is put in place of every omitted argument.
- For an ARRAY parameter the default is a **one-element** literal, such as
  `v = [0]`, `l = [""]` or `f = [false]`. Each call that omits the argument gets
  a **new** array with that element.
- Arguments can be omitted **from the right**; the omitted ones get their
  defaults.
- Scalars are passed by value; STRING and ARRAY by reference.
- `: expr;` returns the value. The return type is that of the first `:` with a
  known type, and every other `:` must match it (with the optional rule of §5).
- `:;` returns without a value. A method without `: expr` cannot appear in an
  expression, only as a command (`f();`). Mixing `:;` and `: expr;` in the same
  method is an error.
- A method with a value that reaches the end of its body without `:` ends the
  program with the runtime error `nihil: method 'f' ended without returning a
  value`.
- **At the top level:** `: expr;` ends the program with exit code `expr` (an
  INT; the system keeps only the low 8 bits, so `: 256 + 7;` exits with 7), and
  `:;` exits with 0. Reaching the end of the file also exits with 0. Output is
  flushed before exiting.

### 9.2 Order, recursion and passes

1. **Prescan (tokens only):** registers the globals and the methods with their
   parameter types.
2. **Compilation passes:** each pass compiles the whole program. A call to a
   method whose return type is not known yet gets a **provisional** type, which
   operators accept without complaint; the types discovered are kept for the
   next pass.
   - If a pass used no provisional type, its code is final. Most programs are
     done after the first pass.
   - If an already known type changes (an INT return becomes INT? when a `: _;`
     shows up), the pass is redone.
   - If nothing new was discovered, the next pass is **strict**: a provisional
     type in it becomes an error.

So a method can call another one declared further down, and recursion (mutual
recursion too) works. A recursive method needs at least one `:` whose type does
not depend on itself:

```text
fact(n = 0) {
    n <= 1 { : 1; }      // fixes the return type as INT
    : n * fact(n - 1);
}
```

---

## 10. The stage 0 core

The assembly compiler implements **only** what the compiler written in Nihil
needs:

- INT, BOOL, STRING and ARRAY (of INT, BOOL or STRING), with literals,
  characters and hexadecimals;
- every INT and BOOL operator (§6), `#`, indexing, `[v ; n]`;
- globals, locals, blocks and the four forms of control flow;
- methods, recursion, the passes, `:`;
- `>>`, `>>>`, `<<<`, `!>>`, `::`, `@`, `$` and inclusion (§16).

Optionals, FLOAT, STRING concatenation, `<<`, `...`, `@>>`/`@+>>`, data models
and the optimizations exist only in stage 1 (§17).

---

## 11. Grammar (EBNF)

```ebnf
program         ::= command*

command         ::= method                      (* top level only *)
                  | model                       (* top level only, §20 *)
                  | ">>"  expression ";"
                  | "!>>" expression ";"
                  | "::" [ expression ] ";"
                  | "..." expression ";"
                  | ( "@>>" | "@+>>" ) expression "," expression ";"
                  | "@" "[" expression "]" ">>" expression "," expression ";"
                  | "@" "{" "}" ">>" expression "," expression ";"   (* by key, §20.5 *)
                  | IDENT ( "@" | "@?" ) "{" expression "}" unary "=" "_" ";"   (* delete by key *)
                  | ">>>" expression ";"
                  | "<<"  IDENT ";"
                  | "<<<" IDENT ";"
                  | ":" [ expression ] ";"
                  | "*" expression block
                  | assignment ";"
                  | [ expression ] block ( "||" [ expression ] block )*
                  | [ expression ] call ";"
                  | "@" STRING ";"              (* inclusion, §16 *)

method          ::= IDENT "(" [ param ( "," param )* ] ")" block
param           ::= IDENT [ "?" ] "=" ( literal | IDENT "(" ")" | "[" IDENT "(" ")" "]" )
model           ::= IDENT "=" "@" "[" field ( ";" field )* [ ";" ] "]" ";"
field           ::= [ "#" [ "+" ] ] IDENT ":" ( ( "INT" | "STRING" | "FLOAT" ) "(" INTEGER ")" | "BOOL" )
block           ::= "{" command* "}"
assignment      ::= IDENT [ "?" ] [ "[" expression "]" ] "=" expression
                  | IDENT ( "[" expression "]" | "." IDENT )* "." IDENT "=" expression
call            ::= IDENT "(" [ expression ( "," expression )* ] ")"

expression      ::= or
or              ::= and  ( "||" and )*
and             ::= comp ( "&&" comp )*
comp            ::= bor  [ ( "==" | "!=" | "<" | ">" | "<=" | ">=" ) bor ]
bor             ::= bxor ( "|" bxor )*
bxor            ::= band ( "^" band )*
band            ::= shift ( "&" shift )*
shift           ::= sum  ( ( "<<" | ">>" ) sum )*
sum             ::= prod ( ( "+" | "-" ) prod )*
prod            ::= unary ( ( "*" | "/" | "%" ) unary )*
unary           ::= ( "-" | "!" | "~" | "#" | "@" | "@?" | "$" | "." ) unary | postfix
postfix         ::= primary ( "[" expression "]" | "." IDENT | "." IDENT "(" [ args ] ")" )*   (* a field, or the library, §21 *)
primary         ::= INTEGER | CHARACTER | FLOAT | STRING | "true" | "false" | "_"
                  | "[" expression ( "," expression )* "]"
                  | "[" expression ";" expression "]"
                  | call                        (* also MODEL( … ): a new record *)
                  | IDENT ( "@" | "@?" ) unary  (* IDENT is a model: reads a table *)
                  | IDENT ( "@" | "@?" ) "[" expression "]" unary   (* one record, §20.4 *)
                  | IDENT "@" "#" unary         (* how many records *)
                  | IDENT ( "@" | "@?" ) "{" expression "}" unary   (* by key, §20.5 *)
                  | IDENT
                  | "(" expression ")"
```

### 11.1 Command disambiguation

The grammar is LL(1) except at the start of a command that begins with `IDENT`.
The parser decides in this order:

1. `IDENT` followed by `=`, or by `?` `=`: **assignment**.
2. `IDENT [` … `] =` (scanning for the matching `]`): **indexed assignment**.
3. At the top level, `IDENT (` … `) {` in declaration form (§9.1): **method
   declaration**.
4. Otherwise: parse an **expression** and look at the next token:
   - `{` → a block governed by the expression;
   - `IDENT` → a prefixed call (§7.5);
   - `;` → valid only if the whole expression is a call;
   - anything else → error.

---

## 12. Compiler architecture (stage 0)

### 12.1 Memory

A single `mmap` (`PROT_READ|PROT_WRITE`, `MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE`)
reserves the arenas:

| Arena | Size | Use |
|---|---|---|
| Source | 16 MiB | expanded source (§16) |
| Symbols | 4 MiB | 32-byte records |
| Code | 16 MiB | ELF header + machine code + constant data |
| Fixups | 4 MiB | pending jumps and references |
| Constants | 16 MiB | decoded string literals, copied after the code |
| Parameters | 1 MiB | type and default of every method parameter |
| Raw files | 16 MiB | files as read, before inclusions are expanded |
| Names | 1 MiB | paths of the included files |

**Symbol record (32 bytes):**

| Field | Bytes | Contents |
|---|---|---|
| `name_off` | 4 | position of the name in the source arena |
| `name_len` | 2 | length of the name |
| `type` | 1 | type (or return type of a method) |
| `flags` | 1 | global, method, created, parameter |
| `level` | 2 | scope depth |
| — | 2 | reserved |
| `nparam` | 4 | methods: number of parameters |
| `address` | 8 | global address, `rbp` offset, or method code position |
| `extra` | 8 | global: line of creation; method: first parameter index |

Names are compared **byte by byte**, never by hash alone, so a collision never
merges two variables. The table is a stack: opening a block marks the top and
closing it drops the block's symbols.

### 12.2 Code generation

- **Stack machine:** every expression ends in `rax`. Binary operators use
  `push`/`pop`. Stage 0 does no register allocation.
- **Locals** in `[rbp - 8n]`, **globals** at fixed absolute addresses in the
  data segment (`mov rax, [disp32]`), known as early as the prescan.
- **Own calling convention** (there is no libc to talk to): arguments are pushed
  left to right, the result comes back in `rax` (and `rdx` for optionals), and
  the caller removes the arguments.
- **Runtime routines** (printing, output buffer, errors, files, …) live in
  `src/runtime.asm`. It is assembled separately and copied whole to the start of
  every executable, at the entry point. Its first instruction is a
  `jmp program`, followed by a table of `jmp routine` at fixed offsets
  (`src/layout.inc`); programs call the routines through that table.
- **Fixups:** control-flow jumps are patched as soon as the target is known.
  The fixup table holds references to constants (strings emitted after the code)
  and calls to methods.
- **Stack frame:** the top level opens a frame (`push rbp; mov rbp, rsp;
  sub rsp, N`). Every local and every loop counter takes a slot `[rbp - 8k]`.
  Closing a block frees its slots for reuse, and N (the largest use, aligned to
  16) is patched at the end.

### 12.3 Generated executable

```text
0x000  ELF64 header                 64 bytes
0x040  PT_LOAD code  (R+X)          56 bytes   offset 0, vaddr 0x400000
0x078  PT_LOAD data  (R+W)          56 bytes   no bytes in the file, vaddr 0x40000000
0x0B0  runtime: jmp program + routine table (entry = 0x4000B0)
       program code
       constant data (strings, aligned to 8)
```

- The code segment is **not** writable.
- The data segment exists only in memory (`p_filesz = 0`); the kernel hands it
  over zeroed. Layout:

  | Offset | Contents |
  |---|---|
  | `+0x0`–`+0x28` | pending output bytes, (unused), input (position, size, end) |
  | `+0x40`–`+0x1A8` | the garbage collector's state and its 21 free lists (§12.4) |
  | `+0x1000` | standard output buffer, 64 KiB |
  | `+0x11000` | standard input buffer, 64 KiB |
  | `+0x21000` | globals, 8 bytes each (16 for optionals) |

- Every STRING or ARRAY global is set to a length-0 constant before the first
  line of the program runs, and is never a null pointer.
- The fixed address `0x40000000` gives absolute addresses known in advance that
  fit in a `disp32`.

### 12.4 Memory and the garbage collector

The heap is one reservation of address space (64 GiB, or a quarter of that
until the kernel accepts it), in 64 KiB pages. Every block starts with a header
word (`n << 8 | marked << 1 | used`) and then the length word the program sees,
so a value points 16 bytes into its block; the layout of §3 is unchanged.
Blocks up to 32 KiB come in 21 size classes (32, 48, 64, 96, 128 … 32768 bytes),
each page holding blocks of one class; larger blocks take whole pages. New
memory is always zero, as the code relies on (§ Appendix, v1.5).

The collector is **conservative mark and sweep**. Values carry no type at run
time, so every qword in the registers, on the stack and in the globals (from
`+0x21000` to the end of the data segment, read from the ELF header) that
points into a used block keeps it, and the blocks reached are scanned the same
way. The top of the stack comes from `prctl(PR_GET_AUXV)` (AT_EXECFN) or, on
an older kernel, `/proc/self/maps`; without it nothing is collected. Large
blocks are scanned only where the kernel has memory for them (`mincore`), so
big arrays never written cost nothing.

A collection runs when the new blocks since the last one pass the larger of
16 MiB and what was in use after it. It frees unmarked blocks into the free
lists of their class, keeps up to 16 MiB of empty small pages for reuse, and
gives the other free pages back to the kernel (`madvise(MADV_DONTNEED)`).

Everything lives in the runtime: the compilers emit the same code as before,
so stage 0 and stage 1 still produce identical bytes. A number that happens to
look like an address keeps a block alive for no reason (the price of being
conservative); nothing reachable is ever freed. `make gcstress` builds a copy
whose runtime collects every 256 KiB and runs the stage 1 tests, the fixed
point and floatcheck with it.

---

## 13. Tests

- `make test` runs `tests/run.sh` with stage 0.
  - Every `tests/NAME.nihil` is compiled and run. Its output is compared with
    `NAME.out` (default: empty), its exit code with `NAME.code` (default: `0`),
    its stderr with `NAME.stderr` (if present), and it reads `NAME.in` as input
    (if present). `readelf` must accept the binary, and its mode must be `755`.
  - Every `tests/errors/NAME.nihil` **must fail** to compile, and its stderr must
    contain the text of `NAME.err`.
- `make test1` runs the same suite, plus `tests/stage1/`, with stage 1;
  `make test1-O0` does it without optimizations.
- `make compare`, `make fixedpoint` and `make floatcheck`: §15, §19 and §18.

---

## 14. Milestones

| # | Delivered |
|---|---|
| 1 | Infrastructure: arenas, reading the source, ELF with two segments, `sys_exit(0)`, `chmod 755`, test suite |
| 2 | Full lexer, INT expressions (all operators), globals, `>>` of INT and string literals, `: expr;` at the top level, output buffer |
| 3 | Blocks, locals, the four forms of control flow |
| 4 | Methods, parameters, `:`, passes, recursion |
| 5 | Self-hosting core: ARRAY, indexable STRING, `#`, `[v ; n]`, `>>>`, `<<<`, runtime errors |
| 6 | **Self-hosting**: the compiler rewritten in Nihil (`src1/`), compiled by stage 0, with the same output and a fixed point (§15); then `::`, `!>>`, `@`, `$` and inclusion (§16) |
| 7 | **Optionals**: `_`, `?`, the `\|\|` fallback, `@?` (stage 1) |
| 8 | **FLOAT** (SSE2, exact literals and printing, `.n` and `#f`), `<<` and STRING concatenation (stage 1) |
| 9 | **Optimizations** (§19, stage 1) |

---

## 15. Stage 1 (`nihilc1`)

The compiler written in Nihil lives in `src1/`. `src1/nihilc1.nihil` is just a list of
inclusions (§16). `build/runtime.nihil` is generated from `runtime.bin`, and
`build/prelude_text.nihil` from the prelude (§18).

- **Same output as stage 0.** Without optimizations (`NIHIL_O0=1`), `nihilc1`
  mirrors the algorithm of `nihilc` and produces exactly the same bytes.
  `make compare` checks this for every test program.
- **Fixed point** (`make fixedpoint`, three parts):
  1. without optimizations, `nihilc1` compiles itself into the binary `nihilc` made;
  2. optimized, `nihilc2` (`nihilc1` compiled by itself) = `nihilc3` (`nihilc2` by
     itself);
  3. `nihilc2`, without optimizations, produces exactly `nihilc1` again.
- **Protocol:** `nihilc1` reads the path of the program from stdin (one line,
  optionally prefixed with `-O0 `), reads the files itself with `@`, writes the
  executable to stdout and errors to stderr (with exit code 1). `nihilc1.sh` gives
  it the same command line as `nihilc`.
- `nihilc1` reports compile errors with `!>>` and exits with `:: 1;`.
- The only difference in behavior between the stages is an **included file that
  cannot be opened**: `nihilc` gives a compile error with a position, `nihilc1` the
  runtime error of `@`. Both messages contain `could not read '…'`.

## 16. File inclusion

```text
@ "lexer.nihil";
@ "../common/util.nihil";
```

- `@ "file";` puts the contents of the file in place of the line, before any
  other analysis.
- It is valid only **at the top level, between commands, alone on its line**. A
  `//` comment may follow the `;`.
- The path is **relative to the directory of the including file**, or absolute
  if it starts with `/`.
- **Each file is included once.** A repeated inclusion of the same path is
  ignored, so two files can include the same utility. Paths are compared after
  normalizing them (stage 1, v4.1): `.` and empty segments go and `dir/..`
  cancels out, so `api/../services/../models/m.nihil` and `models/m.nihil`
  are one file; a `..` with nothing before it stays (`../x.nihil`). Symbolic
  links are not followed.
- **The standard library** (stage 1, v4.2): a path starting with `std:` names a
  library that ships inside the compiler, wherever the program is:
  `@ "std:mcp.nihil";`. The libraries are the files of `std/` in the language's
  repository, made into text inside `nihilc1` at build time (as the prelude, §18), so
  nothing has to be installed next to the program. A relative inclusion inside a
  library names another library. An unknown name is a compile error that lists the
  libraries. Today: `std:env.nihil` (the `.env` file and the environment: `env(key,
  default)`, `env_int`), `std:json.nihil` (`json_bool`, `json_text`,
  `json_text_or_null`), `std:mcp.nihil` (an MCP server over stdio: `mcp_serve`,
  `arg_int`, `arg_text`; §27) and `std:time.nihil` (clocks: `now_us()` and `now_ms()`,
  microseconds and milliseconds on the monotonic clock, for timing; `unix_time()`, seconds
  since 1970 by the system clock). Their locals start with `__` (§18).
- Error messages point to the original file and line, e.g.
  `nihilc: src1/lexer.nihil:12:5: …` and `it is created on line 2 in util.nihil`.
- Implementation: each file is read into the raw-files arena and copied into the
  "expanded source". A table of **segments** (offset in the expanded source →
  file and line) translates positions back.

## 17. Stage policy from M7 on

- **Stage 0** (`nihilc`, assembly) is frozen at M6 and serves only for
  bootstrapping.
- New features go only into **stage 1** (`nihilc1`). The source of `nihilc1` itself
  stays free of them, so stage 0 can still compile it. When that stops being
  true, bootstrapping will start from an already compiled `nihilc1`.
- Guarantees:
  - for programs that do not use the new features, both stages produce the same
    bytes (`make compare`);
  - the fixed point still holds (`make fixedpoint`);
  - stage-1-only tests live in `tests/stage1/` (`make test1`).

## 18. Prelude

Some of the language's routines are written in the language itself, in
`src1/bigint.nihil`, `src1/float_text.nihil` and `src1/prelude.nihil`:
- big integers;
- exact FLOAT ⇄ text conversion;
- reading lines (`<<`).

- `nihilc1` **includes `bigint.nihil` and `float_text.nihil` itself**, to convert FLOAT
  literals at compile time.
- All three go, as text, **to the start of every program that uses FLOAT,
  `<<`, records or the library**, in a segment named `<prelude>`. Programs that
  use none of them do not get the prelude.
- **Only what the program reaches is compiled.** In each pass the compiler
  notes every call (which method makes it, which one it calls) and then follows
  them from the top level and from the program's own methods; the prelude
  methods no chain reaches are left out of the next pass (their tokens are read
  and dropped). A change in that set means one more pass, as a new type does; a
  method left out once and then needed (because a type changed) is never left
  out again, so the passes always settle. `x = 1.5; >> x;` gets the FLOAT
  printing and the big integers, 20 KB in all; none of the records or the
  library.
- The conversion is checked against Python (`make floatcheck`: 100,000 cases plus
  the edge cases): the text reads back as the same number, is the shortest
  possible, and parsing is exact.
- **Names starting with `__nihil_` are reserved.**
- **A lesson for libraries:** by the rule in §4, an assignment inside a method
  changes a global with the same name. Code meant to be included in someone
  else's program must give its locals names no program uses; the prelude
  prefixes all of them with `__`. Parameters do not have this problem, because
  they can hide globals.

## 19. Optimizations (stage 1)

`nihilc1` optimizes by default. With `-O0` (`NIHIL_O0=1 ./nihilc1.sh`) it produces
exactly the same bytes as stage 0. Every optimization rewrites the stack
machine's code right after emitting it, and only when no jump targets the
rewritten stretch.

| Optimization | Before | After |
|---|---|---|
| Direct operand | `push rax ; <load> ; mov rcx, rax ; pop rax` | `mov rcx, <load>` |
| Increment | `x = x + 1` in 4 instructions | `inc x`; `x = x ± k` becomes `add`/`sub x, k` (k fits in imm32) |
| Compare + branch | `cmp ; setcc ; movzx ; test ; jz` | `cmp ; jcc` (inverted) |
| Simple fallback | `test rdx ; jnz ; <load>` | `test rdx ; mov rcx, <load> ; cmovz rax, rcx` |
| Registers | locals and counters in `[rbp - x]` | non-optional locals and loop counters of methods in `rbx`, `r12` … `r15` |

**How many registers each method saves** is learned: in the first pass a
method uses none and counts how many it would use (up to 5). The next pass saves
exactly those in the prologue and restores them at every `:`. A method that
needs none, like `fib`, gets no extra `push`. The runtime preserves these
registers.

**Verification:**
- the whole suite runs with and without optimizations (`make test1`,
  `make test1-O0`);
- `tests/stage1/optimizations.nihil` exercises the edge cases;
- the fixed point (§15) checks that the optimized compiler behaves exactly like
  the unoptimized one.

**Measured gain** (best of several runs): FLOAT 3×, INT loops 1.4×, the compiler
itself 1.6×, little where the cost is `idiv` or memory access.

**Not done yet:**
- null pointers for STRING? and ARRAY? (it would change the representation of
  every optional);
- dead code removal;
- parameters in registers;
- register allocation across commands (every expression still goes through
  `rax`).

---

## 20. Data models and records (stage 1)

A **data model** describes a record: its fields, their types and how wide each
one is in a file. Records model data in memory, and a file of records is a
**fixed-width table**: one record per line, every line the same length.

```text
PRODUCT = @[code: INT(6); name: STRING(12); price: FLOAT(8); active: BOOL];

p = PRODUCT(7, "Coffee", 12.5, true);   // a record
>> p.name;                              // a field
p.price = 13.75;                        // fields can change
@+>> "products.dat", p;                 // appends its line to a file
rows = PRODUCT @ "products.dat";        // reads the table: ARRAY of PRODUCT
>> rows[0].name;
@>> "products.dat", rows;               // rewrites the whole table, atomically
```

### 20.1 Declaring a model

`NAME = @[field: TYPE(width); …];` at the **top level**, outside any block.
It can be used anywhere in the program, before or after its line. The name
follows the rules of any name; a model cannot receive a value.

| Field type | Width | In the file |
|---|---|---|
| `INT(w)` | 1 … 20 characters (with the sign) | decimal, aligned to the **right** |
| `FLOAT(w)` | 1 … 40 characters | shortest text that reads back the same (§6.1.1), aligned to the right |
| `STRING(w)` | 1 … 65535 **bytes** | the bytes, aligned to the **left** |
| `BOOL` | always 1 (no width) | `T` or `F` |

Spaces fill the rest of each field. A `;` may follow the last field. Field names
are checked at compile time: a misspelled field is a compile error.

### 20.2 Records

- `NAME(a, b, …)` creates a record; the arguments go to the fields in order and
  must have their exact types (no conversions: write `12.0`, not `12`, for a
  FLOAT). Omitted fields start as `0`, `""`, `0.0` and `false`; `NAME()` is an
  empty record.
- `r.field` reads a field and `r.field = value;` changes it, also through an
  index: `rows[i].price = 4.5;`.
- A record is a **reference**, like an ARRAY: `q = p;` makes both names point to
  the same record.
- Records can be elements of an ARRAY (`[p, q]`); `[r ; n]` makes **n independent
  copies** of `r`. They can be parameters (the default is `NAME()`, or `[NAME()]`
  for an ARRAY of records), return values, optionals (`: _;` gives `NAME?`) and
  globals (a global record starts as `NAME()`).
- `>>` does not print a record; `$r` is its line (with the `\n`) and `$rows` the
  whole table. Records cannot be compared with `==`.

### 20.3 Table files

- `NAME @ path` reads the file and returns an `ARRAY of NAME`. Every line must
  have exactly the model's width (the last one may lack its `\n`). Numbers may
  have spaces around them; an empty number is 0, an empty BOOL is `false`; a
  STRING loses the spaces on its right (the ones on the left stay).
- `NAME @? path` is the same, but a file that cannot be read gives an **empty
  table** instead of an error: the usual way to start the first run.
- `@>> path, r;` / `@>> path, rows;` write a record or a table, and `@+>> path,
  r;` appends records (§8.1.1): the text is `$r` or `$rows`.

A value that does not fit (`INT(3)` holding `1234`, a 61-byte text in a
`STRING(50)`, a STRING with a line break) ends the program when it is written,
and a line that does not match the model ends it when it is read, both with exit
code 2:

```text
nihil: field 'name' of PRODUCT does not fit in STRING(12): the text has 15 bytes
nihil: 'products.dat', line 3: field 'code' is not a valid INT: 'abc'
nihil: 'products.dat', line 4: the line has 30 bytes, but a PRODUCT record has 27
```

### 20.4 One record at a time

Because every line has the same width, record *i* starts at byte *i × (width +
1)*, so one record can be read without loading the file:

| Expression | Type | Result |
|---|---|---|
| `NAME @[i] path` | `NAME` | record *i* (counting from 0); an error if the file or the record does not exist |
| `NAME @?[i] path` | `NAME?` | the same, but `_` when the file or the record does not exist |
| `NAME @# path` | `INT` | how many records the file has, from its size; 0 if the file does not exist |

Only the bytes of that line are read (`pread`): record 50 000 costs the same as
record 0. The size must be a whole number of lines (the last one may lack its
`\n`); otherwise the file was cut in the middle of a record, which is an error:

```text
nihil: 'products.dat' has no record 7 (it has 5: 0 to 4)
nihil: 'products.dat', line 6: the file ends in the middle of a PRODUCT record (300 bytes is not a whole number of 55-byte lines)
```

The path after `@`, `@[i]` or `@#` is a unary expression: a computed path goes in
parentheses, `NAME @[i] (dir + "/products.dat")`.

**Changing one record in place:** `@[i]>> path, r;` writes record `r` as line
*i* of the file and leaves every other byte as it is: one `pwrite` of that line,
then `fsync`. The model comes from the type of `r`.

```text
p = PRODUCT @[3] "products.dat";
p.price = 13.75;
@[3]>> "products.dat", p;                        // only line 3 changes
@[PRODUCT @# "products.dat"]>> "products.dat", q; // i = the count adds a line
```

- *i* goes from 0 to the count: *i* equal to the count adds a line at the end, a
  file that does not exist has 0 records, and a larger *i* is an error (a table
  never has holes). If the last line lacks its `\n`, it gets one first.
- The file is checked as by `@#`, so one cut in the middle of a record is an
  error and nothing is written. A value that does not fit stops the program
  before anything is written, too.
- One line is small, so in practice it is written whole; but unlike `@>>`
  (temporary file + `rename`) it is not atomic against a crash in the middle of
  the write. To change many records at once safely, rewrite the table with `@>>`.

```text
nihil: 'products.dat' has no record 7 to write (it has 5: 0 to 4, or 5 to add one)
```

### 20.5 Keys

One field of a model can be its **key**, marked with `#`. It must be INT or
STRING, and no two records of a table may have the same key.

```text
PRODUCT = @[#code: INT(6); name: STRING(24); price: FLOAT(10)];

@{}>> "products.dat", PRODUCT(42, "Coffee", 12.5);   // adds it (a new key)
p = PRODUCT @{42} "products.dat";                    // the record with code 42
p.price = 13.75;
@{}>> "products.dat", p;                             // same key: replaced in place
q? = PRODUCT @?{99} "products.dat";                  // _ : there is none
PRODUCT @{42} "products.dat" = _;                    // deletes it
```

| Form | Result |
|---|---|
| `NAME @{k} path` | the record whose key is `k` (an INT or STRING, the key's type); an error if there is none |
| `NAME @?{k} path` | `NAME?`: the same, but `_` when there is none or the file does not exist |
| `@{}>> path, r;` | writes `r` under its own key: replaces that record in place (one line), or adds `r` at the end |
| `NAME @{k} path = _;` | deletes the record with key `k` (it "becomes nothing"); an error if there is none |
| `NAME @?{k} path = _;` | the same, but nothing happens when there is none or the file does not exist |

**Deleting** keeps the table without holes: the last record moves into the
place of the deleted one and the file gets one line shorter (`ftruncate`). So
the order of the records changes, and record numbers (`@[i]`) are not stable
in a table that is changed by key. In the index, the deleted slot is emptied
and the slots after it move back where needed (deletion for linear probing),
so the index stays right without being rebuilt.

A STRING key is compared without the spaces on its right, as the table file
keeps it. The table stays an ordinary table file: `NAME @ path`, `@[i]`, `@#`
and the other writes still work on it.

**The index.** Next to the table, `path.nihil-idx` holds a hash table on disk:
16 bytes of header, then one 4-byte slot per position, with the record number
(+ 1) or 0 for an empty slot. Finding a key costs a few small reads (the slot,
then the record, whose key is always checked), whatever the size of the table;
adding one costs a write of the line and of one slot. The index has at least
twice as many slots as records and doubles when it gets half full.

**Files kept open.** The runtime keeps up to four files open between reads (`@[i]`, `@#`,
key operations): each read asks the kernel what is at the path now (`stat`) and uses the open
file only while it is the same one (device and inode). A file written in place is the same file;
one replaced by a rename (`@>>`, an editor, another program) is opened again, and one removed is
forgotten. So a read still sees the table as it is now, without opening it each time. The key
operations go further: they keep the table and its index open themselves, so a lookup is
one `stat` of the table (its size and stamp) and two reads, the index slot and the record;
the index is read through the open file only while it is known to match the table.

**Writers wait for each other.** A write by key (`@{}>>`, adding, changing or numbering a record,
and `NAME @{k} path = _`) reads the table and the index and then changes them, so it holds an
exclusive lock (`flock`) on `<path>.nihil-lock` from the lookup to the last write: two programs
writing the same table by key run one after the other, and an automatic number is never given
twice. The lock is a file of its own because `@>>` replaces the table. Reads take no lock (the
`stat` before each one shows them what a writer did). The lock is cooperative: a program that does
not take it, such as an editor, is not held back, and `@>>`, `@+>>` and `@[i]>>` do not take it
(a whole-table write is atomic, so with two of them at once the last one wins). A failure caught
by a protected call (§25) may leave the lock held until the program's next operation by key.

A program also keeps, in memory, the last index it found to match its table (the
table's size and stamp, and the index's capacity): a key operation on a table
that has not changed since reads only the slots and the record, not the index's
header, and the record's line is read once.

The index is a **cache** of the table: its header keeps the size and the
*stamp* (modification time and inode) the table had when the index was last
right. When they do not match, because the table was written by `@>>`, `@+>>`,
another program or an editor, the next key operation reads the whole table
once and rebuilds the index (atomically, like `@>>`). A missing or damaged
index is rebuilt the same way, so it can always be deleted. `@[i]>>` keeps the
index right when the key of line *i* does not change.

Each key operation changes the table first and syncs it to disk (`fsync`),
then the index slots (synced), then the header (not synced: if it is lost, the
index is rebuilt). A crash leaves at worst an index that no longer matches,
which is rebuilt. A deletion syncs the moved record before cutting the file;
the cut itself is not synced, because if it is lost the last record is there
twice, byte for byte, and the rebuild recognizes that and finishes the deletion
(two different records with the same key are still an error). So a key write
and a deletion cost two `fsync`s each: one for the table, one for the index.

```text
nihil: 'products.dat' has no PRODUCT with code = 99
nihil: 'products.dat' has no PRODUCT with code = 99 to delete
nihil: 'products.dat', line 3: the key code = 1 is also in line 1, and a key is unique
```

**Automatic keys.** `#+` marks an INT key that the table numbers by itself:
`@{}>>` gives a record whose key is **0** the next number (1, 2, 3, …) before
writing it, and the record keeps it, so the program can read it right after.

```text
ORDER = @[#+id: INT(8); client: STRING(20); total: FLOAT(10)];

o = ORDER(0, "Ana", 99.9);
@{}>> "orders.dat", o;            // o.id is now the next number
>> "order #" + $o.id + "\n";
@{}>> "orders.dat", o;            // o has its number: replaced in place, not numbered again
```

- The last number given is kept in `path.nihil-seq` (its decimal text, then the
  table's size and stamp right after that write), so a number is **not given
  again** after its record is deleted.
- A record written with a number by hand (not 0) keeps it; a number past the
  counter moves the counter there.
- If `path.nihil-seq` is missing, or the table changed after it was written (in
  some other way: `@>>`, `@+>>`, an editor, or the counter is behind), the next
  number is the largest key of the table + 1, past the counter (found by reading
  the table once); while the size and stamp kept with the counter still match
  the table, the counter is used as it is, without reading the table.
- Only `@{}>>` numbers records; `@[i]>>` and `@+>>` write the key as it is.
- The key must be INT: `an automatic key (#+) must be INT`. The counter is
  read and written by each numbered write under the table's lock (see
  *Writers wait for each other* above), so two programs never take one number.

### 20.6 Methods of a model

A method declared with a model's name and a `.` in front belongs to that model.
Its first parameter is the record itself, and it is called on a record with a
`.`, like the library (§21):

```text
SHELF = @[module: INT(3); level: INT(3); width: FLOAT(12); used: FLOAT(12)];

SHELF.free(s = SHELF()) {                // s is the record ("self")
    : s.width - s.used;
}

SHELF.place(s = SHELF(), it = ITEM()) {
    s.used = s.used + it.width * .it.fronts;     // a record is a reference: this shelf changes
}

top = SHELF(1, 5, 338.0, 300.0);
>> top.free();                           // 38.0
top.place(soap);
shelves[i].free() > 0.0 { … }
>> top.grow(10.0).free();                // calls chain when a method returns a record
```

- The name is `MODEL.name`, with the `.` touching both names. It takes no name
  away from the program: `free` can still be a variable or another method, and
  two models can each have a `free`.
- The first parameter must have the model's type (`s = SHELF()`); the others
  are as in any method (§9).
- `value.name(…)` is resolved when compiling, from the model of the value: it is
  an ordinary call, with no table of methods and no cost. A name the model does
  not have is a compile error that lists the ones it has.
- There is no inheritance: a model can hold another one's fields, and its
  methods can call that model's methods.

### 20.7 Implementation

A record is a block of 8-byte slots with the ARRAY layout (§3); a STRING field
holds the address of its text. Its type is a model number, so each model is a
distinct type. The compiler reads the models before the methods and globals (so
any part of the program can use them) and keeps, for each one, constants with
the field types and widths, the field names and the model's name. The text
conversions are prelude methods (§18) that receive those constants; programs
with models always get the prelude. The prelude builds text one byte per character, in a
STRING it has just allocated (`__nihil_bytes(n)`, `__nihil_poke(s, i, b)`), and
the FLOAT conversion reuses one buffer: writing or reading a table costs about
two to three times the size of the file in memory at its peak.

## 21. The library (stage 1)

The library is called **on a value**: `value.name(arguments)`. The names exist
only after a `.`, so they take no name from the program (a method or variable
called `find` is still fine) and the language keeps no reserved words. Every
result is a new value: nothing changes the STRING or the ARRAY it is called on.

```text
line = "  pear, apple, fig  ";
words = line.trim().split(", ");        // ["pear", "apple", "fig"]
>> words.sort().join(" < ");            // apple < fig < pear
n = "42".int() || 0;                    // 42; "4x".int() is _
>> "total: " + $(n * 2) + "\n";         // $ of an INT is its text
```

**STRING**

| Call | Result |
|---|---|
| `s.find(t)`, `s.find(t, from)` | `INT?`: the first position of `t` (from `from`), or `_` |
| `s.contains(t)` | `BOOL`: `t` appears in `s` |
| `s.starts_with(t)`, `s.ends_with(t)` | `BOOL` |
| `s.slice(a)`, `s.slice(a, b)` | the bytes `a` … `b - 1` (to the end without `b`); negative counts from the end, and both are kept inside the text |
| `s.split(sep)` | `ARRAY of STRING`: the parts between each `sep` (`""` gives single bytes) |
| `s.replace(a, b)` | every `a` replaced by `b` |
| `s.trim()` | without spaces, tabs and line breaks at both ends |
| `s.upper()`, `s.lower()` | ASCII and Latin-1 letters changed (`"ação".upper()` is `"AÇÃO"`; À–Þ ⇄ à–þ, but not `×`, `÷`, `ß`, `ÿ`); the size stays and other bytes are kept |
| `s.repeat(n)` | `s` `n` times |
| `s.int()` | `INT?`: the number in `s` (spaces around allowed, a sign), or `_` if it is not one or does not fit |
| `s.float()` | `FLOAT?`: the same for a FLOAT (exact, §6.1.1) |

**ARRAY of INT, FLOAT or STRING**

| Call | Result |
|---|---|
| `v.sort()` | a new ARRAY in increasing order (merge sort: stable, *n* log *n*; STRING byte by byte) |
| `v.find(x)` | `INT?`: the first position of `x`, or `_` |
| `v.contains(x)` | `BOOL`: `x` is in `v` |
| `v.reverse()` | a new ARRAY, last element first |
| `v.sum()`, `v.min()`, `v.max()` | INT and FLOAT only: the total (0 for none); the smallest and largest as `INT?`/`FLOAT?`, `_` for none |
| `parts.join(sep)` | ARRAY of STRING only: one STRING with `sep` between the parts |

The older names `has`, `starts` and `ends` still work as synonyms of `contains`,
`starts_with` and `ends_with`, so programs written before v3.7 compile unchanged.

### 21.1 Queries on an ARRAY of records

On an ARRAY of records the argument is an **expression of the fields**: inside
it, the model's field names stand for the fields of each element in turn. It
can use any other name of the program, locals of the method included, and
other calls; a field name hides a variable with the same name there.

```text
rows = PRODUCT @ "products.dat";
cheap = rows.where(price <= limit && active);   // limit: a local, say
>> $rows.sort(price);                            // the table in order of price
best? = rows.sort(-price).first(active);         // the most expensive active one
total = rows.where(active).sum(price);
```

| Call | Result |
|---|---|
| `rows.where(cond)` | a new ARRAY with the records for which the BOOL `cond` is true, in order |
| `rows.count(cond)` | `INT`: how many |
| `rows.first(cond)` | `NAME?`: the first one, or `_` |
| `rows.sort(key)` | a new ARRAY in increasing order of the INT, FLOAT or STRING `key` (stable; `-key` for a number gives decreasing order) |
| `rows.sum(expr)` | the total of an INT or FLOAT expression (0 for none) |

The compiler writes the loop where the call is (the expression is compiled
once, with the element in a hidden local), so the expression is a full part of
the program and costs no call per element. Only `sort` calls the prelude, with
the keys already computed. The records are the same ones (a record is a
reference, §20.2): `where` and `sort` make new ARRAYs, not new records.

### 21.2 JSON

A JSON value stays **text**: each call finds a part of it and gives that part's
text, so reading JSON needs no type that changes at run time. Numbers become
INT or FLOAT with `.int()` and `.float()`; `true`, `false` and `null` are compared as text.

```text
layout = row.json("structure") || "{}";         // STRING?: the value of a key, as JSON text
modules = layout.json_values();                 // the values of an object, in order
width = (modules[0].json("width") || "0").float() || 0.0;
name = (p.json("name") || "").json_text() || "";   // the text of a JSON string: quotes and \n, \u00e9 … decoded
>> name.json_quote();                           // and back: a JSON string, for writing JSON
```

| Call | Result |
|---|---|
| `s.json(key)` | `STRING?`: the text of the value of `key` in the object `s`, or `_` |
| `s.json_items()` | `ARRAY of STRING`: the texts of the items of the array `s` |
| `s.json_keys()`, `s.json_values()` | the keys (decoded) and the value texts of the object `s`, in the same order |
| `s.json_text()` | `STRING?`: the decoded text of the JSON string `s` (UTF-8, surrogate pairs included), or `_` |
| `s.json_quote()` | `s` as a JSON string, with its quotes and escapes |

Each call reads its part from the start, so to visit every value of a large
object use `json_values()` (one pass) rather than `json(key)` for each key.
Reading is tolerant: an array or object that is cut short gives what was
complete. The loop that finds where a value ends is native (`rt_json_end`), so a 10 MB file of
planograms is read and walked in about 0.1 s (about 90 MB/s).

A name the type does not have is a compile error that lists the ones it has:

```text
nihilc: f.nihil:2:7: STRING has no method 'length': it has find, contains, starts_with, ends_with, slice, split, replace, trim, upper, lower, repeat, int, float, json, …
```

The library is written in Nihil, in the prelude (§18), as methods named
`__nihil_s_find`, `__nihil_ai_sort` (ARRAY of INT), `__nihil_af_*` (FLOAT) and
`__nihil_as_*` (STRING); the compiler passes the value as the first argument.
`$` of an INT is a runtime routine, so it needs no prelude. Only the prelude
methods a program reaches go into its executable (§18), so the library costs
nothing to a program that does not use it.

## 22. MAP (stage 1)

A **MAP** holds values by key, in memory: a hash table. The key is an INT or a
STRING; the value can be any type (INT, FLOAT, BOOL, STRING, a record, an
ARRAY). Its type is written `MAP of K to V`.

```text
age = {"ana": 30, "bob": 25};          // the types come from the first pair
sales = {0: 0.0 ; 100000};             // an empty map of that type, with room for 100,000 keys
age["carla"] = 41;                     // adds or replaces
x = age["ana"] || 0;                   // m[k] is V?: _ when the key is not there
sales[k] = (sales[k] || 0.0) + q;      // the usual way to add up by key
age["bob"] = _;                        // deletes (nothing happens if it is not there)
>> #age;                               // how many keys
names = age.keys().sort();             // m.keys(), m.values(): new ARRAYs
age.contains("ana") { … }              // (has, the older name, still works)
```

- Every pair of a literal must have the same types as the first one; `{k: v ;
  n}` uses its pair only for the types.
- A map is a **reference**, like an ARRAY or a record: `n = m;` makes both names
  point to the same table, and a method that receives a map can change it. A
  parameter's default is a pair of literals, `f(m = {"": 0})`, and gives a new
  empty map on every call; a global map starts empty.
- `m.keys()` and `m.values()` come in the table's order, which is not the order
  of insertion: sort them when the order matters.
- Maps cannot be compared, printed whole, turned into text with `$` or put in an
  ARRAY.

**Implementation.** The table is prelude code (`__nihil_map_*`): open
addressing with linear probing, at most two thirds full (it doubles when it
gets fuller), FNV-1a for STRING keys and Fibonacci hashing for INT keys.
Deleting moves back the entries after the hole, so the table never fills up
with deleted slots. Adding 15 million values over 2 million INT keys takes
about 3.5 s and 160 MB.

## 23. Network: a TCP server and HTTP (stage 1)

A port and a connection are INTs (the sockets), and the calls are made on them,
like the library (§21). Failures follow the rest of the library: a call that can
fail gives an optional (`_`) or a `BOOL`, never an error code. A server answers one
connection at a time:

```text
srv? = 8740.listen();                    // only 127.0.0.1; 8740.listen_all() opens it to the network
srv == _ { !>> "port 8740 is taken\n"; :: 1; }
port = srv || -1;
* true {
    c? = port.accept();                  // waits for a connection (what was printed shows up first)
    conn = c || -1;
    req? = conn.receive();               // the whole request: headers and the body (Content-Length)
    req == _ { conn.close(); }           // the client closed before asking anything
    || (req || "").http_path() == "/health" {
        conn.respond(200, "application/json", "{\"ok\": true}");   // the response, then closes
    } || {
        conn.respond(404, "text/plain", "not found");
    }
}
```

| Call | Result |
|---|---|
| `port.listen()`, `port.listen_all()` | `INT?`: the listening socket, or `_` if the port cannot be opened; `0.listen()` takes any free port |
| `srv.accept()` | `INT?`: the next connection (waits until one arrives), or `_` if the port failed |
| `c.receive()` | `STRING?`: the request, its headers and a body of `Content-Length` bytes; `_` if the client closed before sending anything |
| `c.send(text)` | `BOOL`: all of `text` was sent (`false`: the client went away, which is not the end of the program) |
| `c.respond(status, type, body)` | `BOOL`: an HTTP/1.1 response with `Content-Type`, `Content-Length` and `Connection: close`, then closes; `false` if the client went away. No `Access-Control-Allow-Origin`: a page of another site must not read what a local server answers |
| `c.close()` | closes the connection or the port |
| `req.http_method()`, `req.http_path()` | `"GET"`; the path without the query, decoded (`"/plan/293"`) |
| `req.http_param(name)` | `STRING?`: the value of `name` in the query, decoded, or `_` |
| `req.http_header(name)` | `STRING?`: the value of a header (the name in any case, the value without the spaces around), or `_` |
| `req.http_host_is_local()` | `BOOL`: the `Host` header names this machine (`localhost`, `127.0.0.1` or `[::1]`, any port) |
| `req.http_body()`, `s.url_decode()` | what follows the headers; `%XX` and `+` decoded |

The runtime has five routines (`socket`/`bind`/`listen`, `accept4`, `read`,
`sendto` with `MSG_NOSIGNAL`, `close`); HTTP is prelude code. There is no TLS:
for the internet, put the server behind a proxy that has it.

**The client side.**

```text
r? = "http://127.0.0.1:11434/api/tags".http_get();          // the whole response, or _
p? = "http://localhost:8740/plan/293".http_post("application/json", "{\"month\": 2}");
status = (r || "").http_status() || 0;                        // 200
body = (r || "").http_body();                                 // what came after the headers
c? = "127.0.0.1".connect(8740);                               // a raw connection: send, receive, close
```

| Call | Result |
|---|---|
| `host.connect(port)` | `INT?`: a connection to `host:port`, on which `send`, `receive` and `close` work; `_` if the host is unknown or does not answer. The host is an IPv4 address (`"10.0.0.5"`), `"localhost"` or a name in `/etc/hosts` (no DNS yet) |
| `url.http_get()`, `url.http_post(type, body)`, `url.http_request(method, type, body)` | `STRING?`: the whole response (status line, headers, a blank line, the body: put back together when it came in chunks), or `_` if the URL is not `http://…` or the server does not answer. The request says `Connection: close`, and the answer is read until the server closes |
| `resp.http_status()` | `INT?`: the status of a response (`"HTTP/1.1 404 Not Found"` → `404`) |
| `url.http_open(method, type, body)` | `INT?`: sends the request (HTTP/1.0, so the body comes without chunks) and reads only the response's headers; the connection is left where the body starts |
| `c.read()` | `STRING?`: the next piece the other side sent (what arrived, up to 64 KiB; it waits for one), or `_` when it closed |

A body read as it arrives (a model writing its answer word by word):

```text
c? = "http://localhost:11434/api/chat".http_open("POST", "application/json", request);
conn = c || -1;
piece? = conn.read();
* piece != _ { >> piece || ""; piece = conn.read(); }
conn.close();
```

`resp.http_header(name)` and `resp.http_body()` read a response as they read a request. There is
no TLS: `https://…` gives `_` (a local proxy can add it). The runtime has one more routine,
`rt_net_connect` (`socket`, `connect`); finding the address and HTTP are prelude code.

`accept` gives every connection a read deadline of 15 s: one that opens and sends nothing
(browsers open connections ahead of time) delays the next request at most that long, instead of
holding a server that serves one request at a time. `receive` reads into a buffer that doubles.

**A server for one machine.** `listen()` takes only connections from the machine itself, but
the browser on it (or on another machine, through an SSH tunnel) reaches the port too, and so
does every page open in that browser. A server that answers only its own pages should:
- refuse a request whose `Host` is not local (`req.http_host_is_local()`): a site whose name is
  made to point at 127.0.0.1 (DNS rebinding) sends its own name there;
- for anything that changes or runs something, ask for a secret its own page carries (the IDE
  draws one at every start and checks the `X-Nihil-Token` header of every `/api/` call).

A page of another site cannot read the secret, because no answer allows another origin.

## 24. Directories and processes (stage 1)

Paths are STRINGs, and a command is an ARRAY of STRING (the program and its
arguments); the calls are library methods (§21), so no name is reserved. Like
the rest of the library, a call that can fail gives an optional or a `BOOL`.

```text
"out/2026".make_dir();                           // makes it, and the folders above it
names? = "data".list();                          // ARRAY of STRING?: sorted, without . and ..
"data/sales.dat".is_file() { … }
n? = "data/sales.dat".size();                    // INT?: bytes
tail? = "run.log".read_from(1000);               // STRING?: from byte 1000 to the end
page? = "data/sales.dat".read_from(80 * 100, 80 * 50);   // or at most n bytes

code? = ["zstd", "-d", "-f", "big.dat.zst"].run();       // runs it and waits: its exit code
p? = ["sort", "-u"].start("sorted.txt", "", "work");     // its output to sorted.txt; it runs inside "work"
sorter = p || -1;
sorter.write("b\na\nb\n");                        // its input
sorter.close_input();
>> sorter.wait();                                // 0
```

| Call | Result |
|---|---|
| `path.list()` | `ARRAY of STRING?`: the names in the directory, sorted, without `.` and `..`; `_` if it cannot be opened |
| `path.is_dir()`, `path.is_file()` | `BOOL` |
| `path.make_dir()` | `BOOL`: the directory exists at the end (the missing ones above it are made too) |
| `path.remove()` | `BOOL`: the file was removed |
| `path.size()` | `INT?`: the size of the file in bytes |
| `path.read_from(at)`, `path.read_from(at, n)` | `STRING?`: the bytes from position `at`, to the end or at most `n`; `""` if there are none there. To follow a file that grows, or read one part of a big one |
| `cmd.start(out, err, dir)` | `INT?`: starts the program `cmd[0]` (looked up in `PATH` without a `/`) with the arguments `cmd[1 …]`; its output goes to the file `out` and its errors to `err` (`""` keeps this program's), and it runs in the directory `dir` (`""` keeps this one); `out` and `err` are opened here, relative to this program, before the child moves to `dir`; `_` if it could not start |
| `cmd.run()` | `INT?`: starts it with this program's output, waits, and gives its exit code |
| `p.write(text)` | `BOOL`: all of `text` went to the process's input (`false`: it closed it) |
| `p.close_input()` | the process sees the end of its input |
| `p.done()` | `INT?`: the exit code if it has finished (given once), `_` while it runs |
| `p.wait()` | `INT`: waits for it to finish; its exit code (`-1` if it was already seen) |
| `p.kill()` | `BOOL`: the process was told to stop at once |

An exit code is the program's own, or `128 +` the signal that ended it (`137`
for `kill`). The process's input is a pipe; a child that closes it does not stop
this program. Nothing else leaks into the child: every other file it would
inherit is closed first.

**Implementation.** The runtime has one routine, `rt_syscall` (the kernel call
with up to six arguments, for the prelude only); `openat`, `getdents64`,
`newfstatat`, `mkdir`, `unlinkat`, `pread64`, `pipe2`, `fork`, `dup2`,
`chdir`, `close_range`, `execve`, `wait4` and `kill` are prelude code on top of
it. The IDE server (`ide/server.nihil`) is written with them and §23.

## 25. Protected calls (v4.0)

> v4.0: `tests/stage1/protected_calls.nihil` and the `tests/stage1/errors/protected_*` tests
> define the behaviour.

A `?` before a call makes a runtime error inside it a missing value instead of the end of the
program. A server (an MCP server, an HTTP API) keeps running when one request fails.

```text
r? = ?proposal(plan, month);          // T?: the result, or _ when a runtime error happened inside
r == _ { >> "failed: " + ?! + "\n"; } // ?! : the text of that error
?save(report);                        // a command: the error, if any, is only in ?!
```

- `?` goes before a call: a method, a method of a model or a library call
  (`?s.slice(a, b)`). Before anything else it is a compile error ("'?' goes before a call").
- The result is `T?` for a call of type `T` (still `T?` when `T` is already optional). It is
  assigned like any optional: `y = ?f();` with a plain `y` is the usual error ("'y' is not
  optional and cannot receive INT?").
- **Caught:** every runtime error of §8.3 raised anywhere inside the call, however deep: index
  out of bounds, division by zero, invalid input, `could not read`, `could not write`, a method
  without a value, a table error. **Not caught:** `out of memory` (the heap may be inconsistent)
  and `:: n` (the program ending on purpose).
- `?!` is a STRING: the message of the error the last protected call caught (as it would have
  been written after `nihil: `), or `""` when that call ended normally. Nested protections
  work: the innermost one catches, and an outer call that then ends normally leaves `?!` empty.
- What was written before the error stays written: the output buffer is not touched.
- A protected call costs a few instructions when nothing fails, and the stack does not grow
  when it is used in a loop.

**Implementation.** A stack of protections in the runtime, 64 levels deep: the stack pointer,
the frame pointer, the address to resume at and the optimizer's registers (rbx, r12–r15: a
local kept in one must not change when the call is abandoned), 8 words each. The compiler
emits `RT_PROTECT` (with the resume address) before the call and `RT_UNPROTECT` after it.
Every runtime error starts with `rt_err_begin` and ends with `rt_err_end`: with a protection
active, what the error writes to the standard error goes to `?!`'s text (4000 bytes) instead,
and `rt_err_end` restores the stack, the frame and the registers and resumes with `_`. The
prelude's errors (table files, JSON, the input) go through `RT_FAIL`, so they are caught
too. `out of memory` turns the protections off before it is written. macOS runs the same
runtime, translated (`macos/translate.nihil`). The collector scans whatever stack it
finds, so nothing else changes.

## 26. JSON out and in (v4.0)

> v4.0: `tests/stage1/to_json.nihil` and `from_json.nihil`.

```text
>> plan.to_json();                       // a record: {"id": 293, "store": 1, "name": "REFRIGERANTES", …}
>> plans.to_json();                      // an ARRAY: [{…}, {…}]
>> totals.to_json();                     // a MAP: {"1": 12.5, "2": 3.0}
p = PLANOGRAM.from_json(text);           // a record from a JSON object
ps = PLANOGRAM.from_json_items(text);    // an ARRAY of records from a JSON array of objects
```

- `x.to_json()` works on every type and gives a STRING:
  - INT: its digits;
  - FLOAT: the text `>>` writes (§6.1.1), with `inf`, `-inf` and `nan` as `null`;
  - BOOL: `true` or `false`;
  - STRING: quoted and escaped, like `json_quote()`;
  - an optional: `null` for `_`;
  - an ARRAY: `[a, b]`;
  - a record: an object with the model's field names in declaration order;
  - a MAP: an object with the keys as text, **sorted** (INT keys by number, STRING keys by
    bytes), so that the text is the same every time.
  
  The separators are `", "` and `": "`, as Python's `json.dumps` writes them.
- `MODEL.from_json(text)` reads a JSON object into a new record, field by field by name:
  - a missing field keeps its default (`0`, `0.0`, `false`, `""`), and keys the model does
    not have are ignored;
  - a number is accepted for a FLOAT field (`18` → `18.0`), and a text holding a number for
    an INT or FLOAT field (`"4"` → `4`);
  - any other mismatch is a runtime error, which `?` catches:
    `field 'fronts' of ITEM is not an INT in the JSON: "many"`;
  - the whole text must be one valid JSON object by JSON's grammar (RFC 8259), with only
    spaces after it: a cut or broken document (`{"qty": 5`, a comma too many, unpaired
    brackets, a bad escape, a `\u` surrogate without its pair, text after the object) is a runtime error,
    `ITEM.from_json: the text is not a valid JSON object`, and is never read as a record.
    The `s.json(…)` calls of §21.2 stay lenient: they look for one part of the text.
- `MODEL.from_json_items(text)` does the same for each object of a JSON array, after checking
  the whole array the same way.

**Implementation.** Prelude code over the model's list of fields, which the compiler already
passes to the table reader: `__nihil_r_to_json(schema, names, record)` and
`__nihil_r_from_json`; `to_json` of the other types is prelude code by type.

## 27. Tools from methods (v4.0)

> v4.0: `tests/stage1/tools.nihil` and the `tests/stage1/errors/tools_*` tests.

The methods whose names start with a prefix can be listed and called by name, for an MCP
server (`std:mcp.nihil`, §16): their parameters already have names, types (from the
defaults) and defaults, and the comment above them describes them.

```text
// the proposed fronts of a planogram for a month
tool_proposal(plan = 0, month = 12) { … }

>> tools("tool_");                                   // the tools, as the JSON array MCP's tools/list wants
r? = ?call_tool("tool_", name, arguments);           // calls one by name with a JSON object of arguments
```

- `tools(prefix)` is a STRING made at compile time. `prefix` is a literal text (anything else
  is a compile error). For each method whose name starts with it, in the order of the source,
  it holds an object with:
  - `name`: the method's name without the prefix;
  - `description`: the `//` lines right above the method, joined with spaces;
  - `inputSchema`: an object with one property per parameter, with `"type"` from the default
    (INT `integer`, FLOAT `number`, BOOL `boolean`, STRING `string`) and `"default"`.
  
  A parameter of another type is a compile error that names the parameter and the method.
- `call_tool(prefix, name, arguments)` gives `STRING?`. It calls the method named
  `prefix + name`, reading each parameter from the JSON object `arguments` by name (a missing
  one takes its default; a number given as text is accepted). Its result:
  - a STRING result as it is;
  - any other value `.to_json()`;
  - `""` for a command;
  - `_` when there is no such tool.
  
  A runtime error inside the method is not caught unless the call is protected
  (`?call_tool(…)`).

**Implementation.** The compiler knows every method when it reaches the end of the source:
`tools(prefix)` becomes a constant text, and `call_tool(prefix, …)` a generated method with one
branch per tool, reading its parameters with the JSON library.

## Appendix A — What changed since v1.0, and why

| Change | Reason |
|---|---|
| The counted-loop counter lives on the stack, not in `rcx` | `syscall` destroys `rcx`; a `>>` inside the loop broke the count |
| A count `<= 0` skips the block | `dec rcx; jnz` with 0 would run about 2⁶⁴ times |
| Globals in a fixed data segment, not in `rbx`/`r12`–`r15` | Methods overwrote the registers, and five are not enough |
| Two `PT_LOAD`s (code R+X, data R+W) instead of one RWX | Global addresses known in advance and non-writable code. The entry moved from `0x400078` to `0x4000B0` |
| STRING/ARRAY as a pointer with the length before the data, not a `(ptr, len)` pair | Every value fits in one register, which makes stage 0 much simpler |
| `f() {` at the top level is a declaration | v1.0 was ambiguous with "call `f` and use the result as a condition" |
| Passes | Recursion and forward calls were impossible with the return type locked at the first `:` |
| Locals never hide globals | In v1.0, `b = ...` in a method became a local or a global depending on the order of the text |
| `b? = _;` forbidden | The type would require flow analysis |
| An explicit rule for `\|\|` with an optional BOOL | It was ambiguous |
| Unary minus, bit operators, `#`, `[v ; n]`, `'c'`, `0x` | Needed to write the compiler in the language |
| `>>>` and `<<<` (raw bytes) | Without them the compiler could not read the source or write the ELF |
| `>>` adds no newline | Full control of the output, needed to generate files |
| `: expr;` at the top level sets the exit code | Programs and tests can signal a result |
| Names compared byte by byte | Hash alone let two variables collide silently |
| Runtime routines through `call`, not inline | The binary does not grow with every `>>` |
| The `close` syscall added | It was missing from the list |
| **v1.2:** comparisons below the bit operators in the precedence table | v1.1 said the bit operators were above, but the table and grammar put them below, so `a & 1 == 0` became `a & (1 == 0)` |
| **v1.3:** a parameter may have a global's name | v1.0's own example (`max(a = 0, b = 0)` with a global `b`) would be an error. A parameter is declared explicitly, so there is no ambiguity |
| **v1.3:** `even(4) {` at the top level is a call, not a declaration | The rule "`IDENT(…) {` at the top level is always a declaration" prevented using a method as a condition there |
| **v1.3:** parameter defaults are literals | The default is put into every call that omits it; arbitrary expressions would have to be evaluated in the method's context |
| **v1.3:** a method with a value and no `:` at the end is a runtime error | Returning garbage in `rax` would be silent; detecting it at compile time needs flow analysis |
| **v1.4:** an ARRAY parameter's default is a one-element literal, new on every call | A shared constant array would be changed by one call and seen by the next |
| **v1.4:** `name[…]` is an assignment only if the `]` is followed by `=` | `flags[i] { … }` is a valid conditional |
| **v1.5:** `[0 ; n]` does not fill the memory | A new heap block comes zeroed from the kernel. Without this, the 16 MiB buffers of the compiler would touch hundreds of MB |
| **v1.6:** `::`, `!>>`, `@ path`, `$ bytes`, `@ "file";` | The compiler written in the language needed to exit from inside methods, write errors to stderr, read files and build paths. All of them use symbols that were invalid before, with no keyword |
| **v1.7:** every optional is value + flag (STRING? and ARRAY? too) | A single representation keeps assignment, arguments, returns and fallback simple; null pointers are an optimization |
| **v1.7:** a T return with `: _;` becomes T? | Search methods become natural (`: i;` … `: _;`) with no type annotation |
| **v1.7:** `@?` | Reading a file that may fail without ending the program |
| **v1.8:** FLOAT as bits in `rax` | The stack machine keeps the same registers; SSE2 is used only for the arithmetic |
| **v1.8:** `.n` and `#f` as conversions | No keyword and no implicit conversion |
| **v1.8:** a prelude written in the language | Converting `double` ⇄ text exactly needs big integers; writing them in the language (not in assembly) makes them testable and reusable by the compiler |
| **v1.8:** with a provisional side, `+ - * /` give a provisional result (not INT) | With FLOAT and STRING `+`, assuming INT locked wrong return types in the first pass |
| **v1.9:** optimizations by rewriting the code already emitted, with `-O0` for the bytes of stage 0 | Keeps the generator simple (a stack machine) and keeps the equivalence with stage 0 |
| **v1.9:** saved registers learned between passes | The prologue comes before the body; using the count from the previous pass avoids saving registers for nothing |
| **v2.1:** `... ms;` waits | Animations need a fixed speed; a busy loop depends on the machine. The ellipsis reads as a pause and cannot start any valid expression |
| **v2.2:** renamed from Ket to Nihil | Latin for *nothing*: no keywords, and `_` for the missing value |
| **v4.4:** `std:time.nihil` (`now_us`, `now_ms`, `unix_time`); files kept open between reads | Programs could only time themselves through /proc/uptime, which ticks every 10 ms; and every read of a table opened the file, so one lookup by key cost four opens where C needs none |
| **v4.6:** macOS runs Linux's runtime, translated to ARM64 with each program; the translator is a Nihil program | Two runtimes (assembly and C) had to be kept doing the same; every program on the Mac went through Python |
| **v4.7:** Nihil's own assembler for the runtime (`asm/asm.nihil`, the same bytes as nasm); `from_json` checks the whole document (§26); `repeat` and `.int()` stop at the limits of INT | A change to the runtime needed nasm; a review found a size that went round in 64 bits, a number past the smallest INT and broken JSON read as a record |
| **v4.5:** a collector that marks in a child process (Linux); a lock for writes by key; tables read in parts; locals in registers by use | Pauses of a tenth of a second with large heaps; two programs writing one table by key could lose records; reading a table held the whole file and its records at once; loops ran 3 to 6 times slower than C |
| **v4.3:** automatic keys `#+id: INT(w)`: `@{}>>` numbers a record whose key is 0, with the counter in `path.nihil-seq` | Tables with a key had to compute the next number in every program (`#rows + 1`, which repeats a number after a deletion); the counter belongs to the table, and a deleted number is never given again |
| **v4.2:** the standard library: `@ "std:name.nihil";` (§16), kept inside the compiler | Libraries every program needs (configuration, JSON, MCP) were copied into each project; now they ship with the language |
| **v4.1:** inclusions compared by their normalized path (`.`, empty segments and `dir/..` removed) | A program and a server that share files reached each one by a different relative path (`api/../services/../models/x.nihil`, `database/../models/x.nihil`): the same file was included twice and its models clashed |
| **v4.0:** protected calls `?f()` and `?!`; `x.to_json()`, `MODEL.from_json(text)`, `MODEL.from_json_items(text)`; `tools("prefix")` and `call_tool("prefix", name, arguments)` | The planogram's MCP server showed what a server in Nihil lacks (docs/v4.0-mcp.md): one bad request ended the whole server, every answer was JSON assembled by hand, and every tool was declared twice (its method and its schema). A runtime error can now be a missing value, any value can be written as JSON and read back into a record, and the tools come from the methods themselves |
| **v3.12:** `c.read()`, `url.http_open(method, type, body)` | Showing an answer while it is written: a local model takes a minute for a long answer, and the screen should not stay still. The headers are read one byte at a time, so nothing of the body is taken before the program reads it |
| **v3.11:** the client side of the network: `host.connect(port)`, `url.http_get()`, `http_post`, `http_request`, `resp.http_status()` | A program can now ask other servers (an LLM served by Ollama, another planogram server) instead of only answering. The first use is an agent in Nihil that talks to a local model and calls the planogram's tools in the same process |
| **v3.10:** `req.http_header(name)`, `req.http_host_is_local()`; `respond` no longer sends `Access-Control-Allow-Origin: *`; the IDE checks the host and a secret token, refuses hidden folders as projects, control bytes in paths, and serves only `.nihil` files; a path with a zero byte inside is refused everywhere (`@`, `@?`, `@>>`, `read_at`, the directory functions, `cmd.start`); accepted connections have a read deadline of 15 s; the IDE's programs run in `build/runs`, at most 8 at a time | A security review found that any page open in the browser could use the IDE through the SSH tunnel (read keys, write and run a program) and read the planogram's data: the `*` let other sites read the answers, and nothing checked who was asking. A server for one machine now answers only to it and to its own page |
| **v3.9:** methods of a model: `MODEL.name(self = MODEL(), …) { … }`, called as `record.name(…)` | Programs organized around their data (a planogram, its shelves and items) read better when the operations live with the model. The `.` already called the library on a value; extending it to the program's own models needs no keyword and no runtime dispatch. Inheritance is left out: composition covers it and keeps the language small |
| **v3.8:** directories and processes: `path.list()`, `is_dir`, `is_file`, `make_dir`, `remove`, `size`, `read_from`; `cmd.start(out, err, dir)`, `run`; `p.write`, `close_input`, `done`, `wait`, `kill` | Tools around the language (the IDE, data collection) needed to list folders and run programs. One kernel-call routine in the runtime and the rest in the prelude keeps the assembly small and the library testable; the IDE server moved from Python to Nihil |
| **v3.7:** `contains`, `starts_with`, `ends_with` on STRING, ARRAY and MAP (the old `has`, `starts`, `ends` kept as synonyms); `upper`/`lower` change the Latin-1 letters too; the network gives `_` or `false` instead of `-errno`; faster reading: text → FLOAT by Clinger's fast path, native copies and JSON scanning, `@` of files with no size (`/proc`, pipes) | Names that other languages' libraries use, one way to report a failure across the library (optionals, not error codes), and upper case that works for Portuguese, Spanish, French and German text. Reading tables of decimals went from 14 to about 100 MB/s and JSON from 23 to 90 MB/s: the exact big-integer conversion stays for the rare numbers with more than 15 digits or a power of ten beyond 22 |
| **v3.6:** a TCP server and HTTP: `port.listen()`, `accept`, `receive`, `respond`, `http_path`, `http_param` | Serving analyses as JSON from one place, instead of one generated page per analysis. Calls on INT keep the language free of reserved words, as the library does |
| **v3.5:** JSON as text: `s.json(key)`, `json_items`, `json_keys`, `json_values`, `json_text`, `json_quote` | Importing and serving data needs JSON. Keeping each value as its text needs no dynamic type: the compiler still knows every type, and the existing `.int()`/`.float()` convert numbers |
| **v3.4:** `MAP of K to V`: `{k: v}`, `m[k]`, `m[k] = v`, `m[k] = _`, `#m`, `has`, `keys`, `values` | Grouping and adding up by key (sales by product, store and month) needed a table in memory; the one on disk syncs every write. Braces after `=` or `(` never started an expression before, and `m[k] = _` deletes as `NAME @{k} path = _` does |
| **v3.3:** queries `rows.where(cond)`, `count`, `first`, `sort(key)`, `sum(expr)` | Tables could be read but not searched or ordered by a field. Field names inside the call need no new symbol and no function values; compiling the loop in place lets the condition use the program's locals |
| **v3.2:** only the prelude methods a program reaches are compiled | With the library, every program that used FLOAT, records or `.` carried all of the prelude: `x = 1.5; >> x;` was 66 KB, now 20 KB. The passes already redo everything, so the set of methods is found like a type: from the calls of one pass, for the next |
| **v3.1:** `ARRAY + ARRAY` | Building a list of unknown length meant counting first and filling after. `+` already joined STRINGs, which are arrays of bytes |
| **v3.0:** the library `value.name(…)`, and `$` of INT, FLOAT and BOOL | Every program was rewriting the same text and array routines, and a number could not be turned into text. Names after a `.` cannot clash with the program's names, so the language still has no reserved words |
| **v2.9:** a garbage collector (conservative mark and sweep) | Programs that run for a long time or make many temporary texts grew without limit: a loop of 3 million small texts and arrays used 633 MB, now 19 MB. Conservative, so the compilers need no change and keep producing identical bytes |
| **v2.8:** `NAME @{k} path = _;` deletes by key | Completes the key-value store. The record "becomes nothing": `_` already means no value. Moving the last record into the hole keeps the table a plain file without deleted lines, which every other read would have to skip |
| **v2.7:** key fields `#name`, `NAME @{k} path`, `@{}>> path, r;` | A key-value store: finding a record by its key without reading the table. The index lives in its own file as a cache that is rebuilt when it does not match the table, so the table stays a plain text file that any write or editor may change. `#` reads as "number"; `{}` after `@` was never valid |
| **v2.6:** `@[i]>> path, r;` | A key-value store needs to change one record without rewriting the table. With fixed widths the line's position is known, so one `pwrite` replaces it. `@[i]` already meant "record i" and `>>` "write"; a command never started with `@[` before |
| **v2.5:** `NAME @[i] path`, `NAME @?[i] path`, `NAME @# path` | Reading one record should not cost reading the whole table: with fixed widths its position is known, and `pread` reads only that line. The count comes from the file's size, which also detects a file cut in the middle of a record |
| **v2.4:** data models `NAME = @[field: TYPE(width); …];`, records, `r.field`, `NAME @ path` | Programs needed structured data that matches a file layout. Fixed widths make a file a table where record *n* has a known position, which a key-value store will need; names without quotes let the compiler check every field. `@[` keeps the "file" meaning of `@`; `.` after a value was never valid before |
| **v2.3:** `@>> path, text;` and `@+>> path, text;` write files | Programs could read files but not save anything. The first step towards storing data: atomic replace (temporary + `fsync` + `rename`) so a crash never leaves half a file, and a durable append for logs. `@` already meant "file" and `>>` "write"; neither combination was valid before |
| **v2.0:** renamed from Cru to Ket, and the whole project moved to English | Ket came from Dirac's ket notation: a state written with symbols only, like a language without keywords |
| FLOAT and optionals after self-hosting | The compiler needs neither, and converting a `double` to text is a project of its own |
| Optimizations postponed | Correctness first; optimize in stage 1 |
