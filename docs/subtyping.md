# A child where its parent is expected — design note

Status: **implemented at the edges only** (SPEC §20.9), after two reviews by Codex. This note
records what was decided, what was left out on purpose, and why.

## The rule

A record of model C is accepted where a record of model P is expected, when C descends from P
(`C = @[P; …]`, SPEC §20.8), **in exactly these places**:

1. an explicit argument for a parameter: `show(p = PERSON())` takes `show(student)`;
2. an element stored into an ARRAY of P: `people[i] = student;`
3. an element added to or replaced in a LIST of P: `team[] = student;`, `team[i] = student;`
4. a value stored into a MAP whose values are P: `by_id[k] = student;`

Each place has its expected type fixed by something else: a parameter's type comes from its default
(read before any code), and storing into a collection never changes the collection's type. So
accepting a child there **never feeds the type inference**.

For optional parameters: a C into `P?` is present; a `C?` into `P?` keeps its presence; a `C?` into a
plain P is refused, as today (unwrap it with `||`).

## What stays an error

| Case | Why it stays out |
| --- | --- |
| `x = PERSON(); x = STUDENT();` (any order) | a local's type is inferred; accepting a child there needs the inference below |
| `[PERSON(…), STUDENT(…)]` | an ARRAY literal's type is inferred from its elements |
| a method returning `PERSON(…)` and `STUDENT(…)` | its return type is inferred |
| an ARRAY, LIST or MAP of C where one of P is expected | the callee could store a P into it: collections are invariant |
| a P where a C is expected | a P lacks C's fields |
| `==` between records | records are not compared, with or without models from another |

An explicit edge covers most of these: `as_person(p = PERSON()) { : p; }` turns any descendant into a
PERSON-typed reference to the same record.

## The same record, and copies by the static model

Passing a child as a parent passes **the same record**: a change through the PERSON reference shows in
the STUDENT. What builds or writes data takes the model of the expression (the static model):

- `[p ; n]` through a PERSON gives PERSON records (copies of the parent's fields);
- `$p`, `to_json` and the table writes (`@>>`, `@+>>`, `@[i]>>`, `@{}>>`) write the PERSON part;
- reading (`PERSON @ path`, `from_json`) gives the model named.

A STUDENT is written whole through a STUDENT-typed expression. One table per model stays a rule of
use: the path does not say which model a table holds. With an automatic key, writing a STUDENT
through a PERSON reference to a PERSON table numbers the STUDENT itself (the key field is shared);
writing it then to a STUDENT table keeps that number.

## Why not the whole inference (left for later)

Letting locals, returns and literals take a child needs the compiler to tell a type it knows for now
from a type fixed by the first assignment in the text. Codex showed that the passes today fix types
from the first one **known**, not the first one **written**, so a simple rule is not deterministic:

```text
f() {
    x = later();   // unknown in this pass
    x = C();       // x takes C for now …
    x = P();       // … and this would fail, although x's first assignment turns out to be a P
    : x;
}
later() { : P(); }
```

Fixing it means changing how `compile()` decides that types have settled (`unknowns`, `discovered`,
`altered`, `final_pass` in `src1/80_main.nihil`), how a method's return type is published between
passes, and how joins treat unknown parts. It also changes how existing programs are inferred (a
recursive method's return type, for one), and the number of passes. It was estimated at 3 to 5
weeks, after a prototype. The edges give the main uses now (a function on PERSON, a mixed team)
without touching any of it.

## How "no program changes" was checked

Without a model built from another, `record_subtype` accepts only equal models, so the edges compile
exactly as before. Checked by compiling the same 170 sources with the compiler at `301324e` and with
this one, optimized and with `-O0`, and comparing the executables byte for byte: tests, examples, the
IDE's server, the assembler, the macOS translator, the compiler itself and a real application.
All were identical. The other 124 sources (the error tests, and one program that already failed)
fail with the same message under both compilers.

## Implementation

- `record_subtype(c, p)` (`src1/45_optionals.nihil`): plain records only, equal or `descends(c, p)`.
- `convert_argument(from, to)`: what `convert()` accepts, plus the child cases above; used only by
  `call_method()` for explicit arguments. `convert()` itself is unchanged, since it also serves
  assignments to optionals.
- `element_accepts(el, v)`: used by the ARRAY element store, the LIST append and replace, and the
  MAP value store.
- A method call on a child (`student.age(…)`) already passes the receiver without a type check
  (SPEC §20.8); that path is unchanged.
