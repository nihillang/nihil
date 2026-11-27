# A model's methods inside a query — design note (draft)

Status: **implemented** (SPEC §21.1), after a review by Codex; the decisions taken after it are at the end. Builds on queries (SPEC §21.1), methods of a model (§20.6), private
fields (§20.7) and dispatch (§20.10).

## The goal

```text
ACCOUNT = @[#id: INT(4); owner: STRING(10); _cents: INT(10)];
ACCOUNT.balance(a = ACCOUNT()) { : a._cents; }
ACCOUNT.fee(a = ACCOUNT(), cents = 0) { ... : 0; }
CHECKING = @[ACCOUNT; overdraft: INT(10)];
CHECKING.fee(c = CHECKING(), cents = 0) { : 150; }

cs = CHECKING @ "checking.dat";
cs.where(balance() > 1000)                 // today: "method 'balance' does not exist"
cs.sort(balance())
cs.sum(balance() - fee(100))
accounts.array().where(fee(100) > 100)     // dispatched: each record's real model's fee
```

Today a query's expression sees the current element only through its field names, so the rules a
model keeps in its methods (and the private fields behind them) cannot be used to filter, sort or
add up. The workaround is a loop by hand.

## 1. The rule

Inside `rows.where(…)`, `count`, `first`, `sort` and `sum`, a name followed by `(` that is a
method of the element's model (its own or the nearest inherited, as `r.name(…)` resolves it) is
that method called on the current element: `balance()` is `element.balance()`, `fee(100)` is
`element.fee(100)`. The element is the first argument; the others are written as usual and may use
the fields (`fee(qty)`).

It is the rule fields already follow: inside a query, the element's field names stand for its
fields. A method name of the model stands for its method.

## 2. A program method with the same name

A program method `balance(…)` and a model method `ACCOUNT.balance` may both exist. Today, inside a
query, `balance(x)` calls the program's. Letting the model's win would change the meaning of such
a program silently. So, inside a query over that model, the name is a **compile error** when both
exist: "balance is a method of ACCOUNT and of the program: inside a query over ACCOUNT, rename one".
No program that compiles today changes meaning; the ones that would are refused.

(A field and a method of one name cannot be confused: a field is never followed by `(`.)

## 3. Compilation

In `primary_name()` (`src1/50_expressions.nihil:578`), after `query_field()`, when `q_model >= 0`
and the next token is `(`:

```text
k = the model method for (q_model, name), up the parents       // as model_method_call finds it
k >= 0 && a program method of that name exists { error (section 2) }
k >= 0 {
    q_load(0, q_elem)                                          // the element in rax
    : model_method_call(model_type(q_model), name)             // push, dispatch, call: as r.name(…)
}
```

`model_method_call()` already pushes the receiver, parses the arguments, builds the dispatch on
the record's real model and records the calls (`note_call`), so nothing about the call is new.
Inside the query, the loop's hidden locals are callee-saved registers or frame slots
(`q_slot()`), and the expression may already call program methods: the FLOAT sum already falls
back from `xmm8`/`xmm9` to memory when the expression calls anything (`n_calls == calls0`).

## 4. Private fields

Unchanged: the query's expression still cannot name `_cents` outside ACCOUNT's methods, but
`balance()` reads it inside ACCOUNT's method, which is the point. Inside a method of ACCOUNT,
`xs.where(_cents > a._cents)` already compiles.

## 5. What stays out

- An explicit name for the element (`cs.where(c: c.balance() > 1000)`, or `it`): a new syntax for
  what section 1 gives with the existing rule.
- Indexes on other fields, `group`/`min`/`max` over records.

## 6. Tests

- `where`, `count`, `first`, `sort`, `sum` with a method; an INT, FLOAT, STRING and BOOL return;
  a method with more arguments, one taking a field (`fee(qty)`).
- An inherited method; a replaced one dispatched per element through a LIST of the parent (a
  CHECKING and a SAVINGS); an abstract one implemented below.
- A method that allocates (a STRING built per element) under `make gcstress` and
  `NIHIL_GC_TORTURE=1`, with `where` and `sort` (whose result arrays are alive during the loop).
- A method that runs a query of its own (nested queries); a FLOAT `sum` calling a method;
  `-O0` and optimized; macOS native.
- A method raising an error inside `?rows.where(…)`.
- Errors: a method of the program and of the model with the same name; a method that does not
  exist on the model (the message lists the model's methods); a method returning nothing in
  `where`; `_cents` in the expression still refused.
- Byte identity: every source that compiles today compiles to the same bytes.

## 7. Effort

About **1 to 2 days**: the change is one branch in `primary_name()` and the error of section 2;
the rest is tests.

## Questions for the review

1. Is section 2 the right rule, or should the model's method simply win inside a query over that
   model (as fields already hide program names)?
2. Is calling a method inside the loop safe for the hidden locals and the results being built:
   `where`'s result array (allocated before the loop, its length set after) and `sort`'s keys,
   if the method allocates and the collector runs? Are they reachable roots while the loop runs?
3. Anything in the native ARM64 backend (`src1/42_arm.nihil`) or the macOS translator that assumes
   a query's expression makes no calls to model methods (dispatch inside a loop)?
4. Does `model_method_call()` need anything from its caller (token position, `lib_k`) that
   `primary_name()` would not have?
5. Is the element's static model the right one for the dispatch (an ARRAY of ACCOUNT holding
   CHECKINGs dispatches per element), and the right one for section 2's check?

## Decisions after Codex's review

| Point | Decision |
| --- | --- |
| Other meanings of `name(…)` | Not only program methods: `primary_name()` also gives a name followed by `(` to a model's constructor, the intrinsics, `tools()` and `call_tool()`. Any of these with the method's name is the compile error of section 2. A variable (global or local) with the name is not: it cannot be called, so `balance()` never meant it, and a program with a global `total` and a method `RULE.total` keeps working. |
| Compatibility | Not every program that compiles today keeps compiling: one with a program method and a model method of the same name, calling the program's inside a query over that model, is refused (never silently changed). Every other program compiles to the same bytes; the corpus is checked for both. |
| GC | Safe as it is: allocations are zeroed and their header counts every slot, so `where`'s result and `sort`'s keys are traced while the loop runs (the note's premise of uninitialised slots was wrong). |
| FLOAT `sum` | Codex found that a FLOAT sum calling a method gave 0.0 inside a method with a free register (the sum loaded over the value). Fixed on its own, before this: PR #46. |
| Dispatch | Inside the loop, for every element: the header test and the chain of compares, one direct call. Its cost grows with the number of replacing descendants. |
| Unknown name | Inside a query, a name followed by `(` that is neither the model's method nor anything else gets its own error: the model's methods and fields are listed. |
| LIST queries | Already work (`list.where(…)` copies to an ARRAY): removed from section 5. |
