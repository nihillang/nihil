# Abstract methods — design note (draft)

Status: **implemented** (SPEC §20.11), after a review by Codex; the decisions taken after it are at the end. Builds on dispatch (PR #37, SPEC §20.10): a child
may replace an inherited method, and a call through the parent runs the record's real model's.

## The goal

```text
SHAPE = @[name: STRING(12)];
SHAPE.area(s = SHAPE()) { ... : 0.0; }          // abstract: no body; returns a FLOAT
SHAPE.describe(s = SHAPE()) { : s.name + ": " + $s.area(); }

CIRCLE = @[SHAPE; r: FLOAT(8)];
CIRCLE.area(c = CIRCLE()) { : 3.14159 * c.r * c.r; }
SQUARE = @[SHAPE; side: FLOAT(8)];
SQUARE.area(q = SQUARE()) { : q.side * q.side; }

shapes = [SHAPE() ; *];                          // only gives the type
shapes[] = CIRCLE("c", 1.0);
shapes[] = SQUARE("q", 2.0);
shapes : s { >> s.describe() + "\n"; }
SHAPE("x");                                      // compile error: SHAPE is abstract (area)
```

## 1. Syntax: `{ ... }` and `{ ... : literal; }`

A model's method whose body is `...` alone is **abstract**. Its return type is written the way a
parameter's type is, by an example: `{ ... : 0.0; }` returns a FLOAT, `{ ... : ""; }` a STRING,
`{ ... }` nothing (optional and ARRAY returns are question 1). Codex asked for the return type in the
contract: inferring it from the implementations ties the parent to whatever children the program
has and adds cycles to the inference.

Today `...` is only the pause, `... ms;`, which needs an expression: both `{ ... }` and
`{ ... : 0.0; }` are compile errors now ("expected an expression, found '}'" / "found ':'"), so no
program changes meaning. Only a model's method may be abstract.

## 2. What an abstract method compiles to

The method itself is an ordinary method whose body raises a runtime error and, for the type,
returns its example:

```text
SHAPE.area(s = SHAPE()) { ?! = "SHAPE.area is abstract: a model below SHAPE implements it"; : 0.0; }
```

So a call that reaches it (a record with no implementing model, see section 4) is a catchable runtime
error, and nothing in the dispatch changes: it calls the abstract method as it would call any
inherited one. Its parameters and its return type are checked against its replacements by the rules
of §20.10 (`same_parameters` in the prescan, `check_replacements` after the passes).

## 3. Abstract and concrete models

A model is **abstract** when it has an abstract method that neither it nor an ancestor below the
declaring one implements: a model that declares one, and every descendant that does not implement
all the abstract methods it inherits. The others are **concrete**. Computed once, after the prescan
(every model and method is known then).

Making a record of an abstract model is a compile error, with the methods it lacks:

| Form | Today it… | With an abstract model |
| --- | --- | --- |
| `SHAPE("x")` | allocates and fills | **error**: "SHAPE is abstract: it does not implement area" |
| `SHAPE @ path`, `@[i]`, `@{k}`, `SHAPE.from_json`, `from_json_items` | make SHAPEs from data | **error** |
| a parameter `s = SHAPE()` | its default gives the type; an omitted argument allocates a new SHAPE (`emit_default`) | the type only: **the argument is required**, omitting it is an error |
| `[SHAPE() ; *]`, `[SHAPE() ; 0]` | the element is compiled (allocated) before `;` is seen | **no record made**: the literal only gives the type |
| `{"": SHAPE() ; 8}` | the same, for a MAP's values | **no record made** |
| `[SHAPE() ; n]`, n not a literal 0 | n copies | **error** |
| a record global that starts as `SHAPE()` | allocated at the start | see question 3 |
| `[r ; n]` through a SHAPE reference | n copies of the SHAPE part | **error**: the copies would be SHAPEs |

The "no record made" forms need the compiler to know, before compiling `SHAPE()`, that it is in a
type-only position: `[NAME() ;` followed by `*` or `0 ]`, and `{key : NAME() ;`. A lookahead in the
ARRAY and MAP literals, or a flag set by them around the element.

## 4. Calls

`s.area()` through a SHAPE dispatches as in §20.10: each concrete descendant gets its effective
method. The default branch (the static model's own, here SHAPE.area) is the abstract one and raises:
it can only be reached by a record whose model is not a concrete descendant, which section 3 makes
impossible except through question 3.

A call on a concrete model's record that resolves statically to an abstract method cannot happen: a
concrete model implements every abstract method above it.

## 5. What stays out

- **Interfaces** (a model with no fields, several per model): `@[]` is refused today, and one parent
  per model is what the field prefix allows. Codex advised leaving them out.
- `super`: unchanged (§20.10).
- An abstract method's default values: its parameters keep their defaults (the replacements are
  called with the static method's, §20.10).

## 6. Tests

- The example above; a grandchild implementing what its parent left abstract; a child that stays
  abstract (does not implement) and its own child that does; an abstract method called inside a
  concrete one of the parent (`describe`), dispatching to the child's.
- `?s.area()` on a record that reaches the abstract method (question 3) gives the runtime error.
- Errors: constructing an abstract model (with the missing methods listed), a table read and
  `from_json` of one, `[SHAPE() ; 3]`, an omitted argument for a SHAPE parameter, a replacement of an
  abstract method with another return type than its example, `{ ... }` outside a model's method.
- `[SHAPE() ; *]`, `[SHAPE() ; 0]` and `{k: SHAPE() ; n}` compile and make no record.

## 7. Effort

The method itself is a rewrite in the parser (section 2). The rest is the abstract/concrete
computation, the checks at the creation forms, and the type-only literals: about **3 to 5 days**.

## Questions for the review

1. Is `{ ... : literal; }` a good way to give the return type, and does it parse unambiguously
   (`: _;` for an optional return? `: [0];` for an ARRAY? a record `: SHAPE();` would construct)?
2. Is compiling the abstract method as an ordinary method that raises sound with the dispatch,
   `prune_prelude`, `check_replacements` and the optimizer?
3. A record global whose type is an abstract model starts as `NAME()` (`src1/80_main.nihil:238`):
   allow it as an internal placeholder (reading it before assigning would reach the abstract
   method's error), or refuse such globals?
4. Is the creation-form table complete? Other places that make a record of a given model?
5. Is the type-only detection for `[NAME() ; *]`, `[NAME() ; 0]` and `{k: NAME() ; n}` safe in the
   parser (the element is compiled before `;` today, `src1/50_expressions.nihil:672`)?

## Decisions after Codex's review

| Point | Decision |
| --- | --- |
| The return type | an example read as a parameter's default is (a literal, `NAME()`, `[x]`), never compiled, so `: SHAPE();` makes nothing; optional returns are left out |
| A child making an inherited method abstract | allowed: abstract by the nearest model declaring the method, at or above |
| A global of an abstract model | a compile error, decided after the passes (not by where the global is kept) |
| Type-only forms | only those that never made elements: `[NAME() ; *]` (and `*n`) and `{k: NAME() ; n}`, recognised by their tokens before the element is compiled; `[NAME() ; 0]` is an error, with `[NAME() ; *]` named instead |
| Defaults | `s = SHAPE()` and `xs = [SHAPE()]` give the type; an omitted argument is an error at the call |
| Refused makings | `NAME(…)`, every table read but `@#`, `from_json`, `from_json_items`, `[r ; n]` (n = 0 too) |
| Interfaces, `super` | out |
