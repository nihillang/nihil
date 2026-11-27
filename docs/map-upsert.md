# One call for m[k] = (m[k] || d) + e — design note (draft)

Status: **implemented**, after a review by Codex; the decisions taken after it are at the end. A speed change only: no program's meaning, error text or exit code changes.

## The goal

```text
by_pair[key] = (by_pair[key] || 0) + total;          // today: map_get, then map_set: two calls
units[nm] = (units[nm] || 0) + s.qty;
```

The commonest update of a map: a count or a total by key. Today it is two prelude calls, each with
its prologue: `__nihil_map_get` (or `_get_int`) and `__nihil_map_set`, the second taking the
first's slot through the map's hint (#50). One call that finds the slot once and adds in place is
what C does (`t[j].v += e`).

## 1. The form recognised

In `map_assign()` (`src1/49_maps.nihil`), after `m[K] =` with `m` a variable holding a MAP whose
values are INT, the right side is exactly

    ( m [ K' ] || D ) + E ;

- `m` the same variable;
- `K'` the same tokens as `K` (compared token by token: the same kinds and the same text);
- `D` an INT literal, or `-` and one;
- `E` any expression, parsed by `expression()` after the `+`, up to the `;`.

Anything else (another operator than `+`, a FLOAT or STRING value, `K` with a call in it, a `D`
that is not a literal, parentheses missing or around more) compiles as today.

`K` must hold **no call** (no name followed by `(`): it is evaluated once instead of twice, which
can only matter when evaluating it has an effect. Field reads, indexes, arithmetic and locals are
allowed. Its errors (an index past the end) happen at the same point: first.

## 2. Order of evaluation

Today: `m`, `K` (for the set), then the right side: `m`, `K'` (for the get), the get, then `E`, the
addition, the set. Fused: `m`, `K`, then `E`, then one call that looks `K` up and adds. So the
look-up moves after `E`.

That differs only if `E` changes the map `m` (adds or deletes keys, so that `m[K]` read before `E`
and after it differ) or has an effect that `K'` depends on. Hence a second condition: **`E` holds no
call** either. `E` then reads variables, fields and indexes and computes; it cannot change `m`. Its
errors (division by zero, an index) come before the look-up instead of after, which no program can
tell apart: neither the get nor the set can fail (they allocate, and an allocation failure ends the
program either way).

## 3. The call

    push m ; push K ; push E ; push D ; push kind ; call __nihil_map_add_int (40 bytes)

```text
// m[k] = (m[k] || d) + e; in one look-up (docs/map-upsert.md)
__nihil_map_add_int(m = 0, k = 0, e = 0, d = 0, kind = 0) {
    … the slot of k: the hint first, else __nihil_map_slot …
    used: values[j] = values[j] + e
    not:  insert k (growing as map_set does) with d + e
}
```

INT addition wraps in both forms, and `(x + a) + b` and `x + (a + b)` are equal in wrapping
arithmetic, so `E` being a sum itself (`… + a - b`) gives the same value as the left-to-right
parse.

## 4. Not covered (later, measured first)

- FLOAT values (`(m[k] || 0.0) + x`): the prelude adds INTs; a FLOAT variant needs the addition on
  bits (an intrinsic) or the compiler to emit it.
- `-`, `*`, STRING `+`.
- Calls in `K` or `E`: computed into a local first, they get the fused form.

## 5. Tests

- The bench's two forms, INT and STRING keys; `D` 0 and other values, negative; a missing key, a
  present one, a map that grows inside the update; `E` a sum, a difference, a product.
- The forms that must not fuse, with their results: a call in `K`, a call in `E`, `-`, FLOAT values,
  another map in the right side, another key, `D` a variable, extra parentheses.
- The same output with and without `-O0` (fusion off with `-O0`), x86 and `-native`, GC torture.
- Byte identity for every program without the form.

## Questions for the review

1. Is the order argument (section 2) right, with "no call in `K` or `E`" as the guard? Anything
   else in an expression with an effect (`?`, `<<` input, `@` reads, `$` of something, intrinsics)?
2. Is the token-by-token comparison of `K` and `K'` enough (the same tokens can mean different
   things only if something between them changes a name's meaning, which nothing in an expression
   does)?
3. Is the lexer's `save_lexer`/`back_lexer`/`drop_lexer` safe to look ahead over `( m [ K' ] || D ) +`
   and rewind when the form does not match (line numbers, errors)?
4. Does a map of INT values with an optional value type (`{k: 0?}` if it exists) need excluding?
5. Anything in `-native` (42_arm.nihil, owned by another session) that needs the new prelude
   routine named or treated specially (it calls prelude methods like any other)?

## Decisions after Codex's review

| Point | Decision |
| --- | --- |
| Effects without a call | "No name followed by `(`" was not enough: `@"fifo"` reads a file, `?!`, `$` and literals allocate or read state. `k` and `e` may now hold only names (not followed by `(`), INT literals, `.`, `[ ]`, `( )` and `+ - * / %`. |
| Precedence | `m[k] = (m[k] \|\| 0) + 1 << 1` is 22 today; parsing `e = 1 << 1` would give 12. `e` may hold `+` or `-` only inside parentheses (a leading `-` aside), so it is one term of the sum, compiled exactly as in the sum: the same value, and any error it has is the same one at the same place. A type error at the `+` itself is reported at that `+`, as before. |
| Values | the map's values exactly INT (BOOL and optionals are not). |
| The hint | `__nihil_map_add` takes the hint as `map_set` does for STRING keys (an occupied slot holding `k` itself, else a look-up); an INT key is looked up written out, as in `map_get_int`. A key not there yet goes through `map_set` (it may grow). |
| Lexer | look-ahead with `save_lexer`/`back_lexer`/`drop_lexer`, tokens compared by kind and source text; nothing is compiled until the form matched. |
| `-O0` | off, so `-O0` keeps its bytes. |
| Second review | Two more holes, closed: an ARRAY literal indexed (`[1][0]`) or an index after parentheses (`(a + b)[0]`) allocates, so `[` is taken only right after a name or a `]`; and looking ahead with the lexer could meet a later lexical error (a number too big) before the error compiling reports first, so the form is now recognised **in the source's bytes**, by a scan that never fails, and the lexer goes over the prefix only once it matched. `k'` must be byte for byte `k`. What allocation remains possible (`+` of two STRING names in `k`) happens once instead of twice, as other optimizations already do (`"…" + $n`): no program's output, error or exit code depends on it. |
