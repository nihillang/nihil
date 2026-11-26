# Replacing an inherited method: dispatch — design note

Status: **implemented** (SPEC §20.10 and §20.11), after two reviews by Codex. What follows is the
design as reviewed. `main` at `1405752`: models built from another
(SPEC §20.8), a child accepted where its parent is expected at the edges (§20.9), and a compile error
for a method that would replace an inherited one (`no_replacement()` in `src1/70_prescan.nihil`).
Planned as two pull requests: the model number in the block header (no dispatch), then dispatch.

## The goal

```text
PERSON.label(p = PERSON()) { : p.name; }
TEACHER.label(t = TEACHER()) { : "prof. " + t.name; }       // replaces PERSON.label
team = [PERSON() ; *];
team[] = STUDENT(…); team[] = TEACHER(…);
team : p { >> p.label() + "\n"; }                            // Ana, prof. Rui
```

A call through a PERSON-typed record runs the method of the record's **real** model: the nearest
ancestor (itself included) that declares it.

## 1. The real model lives in the block header

Every heap block is a header word, a length word, then the slots (`rt_alloc`, `src/runtime.asm`
~1640). The allocator refuses `n ≥ 2^40` and writes the whole header as `(n << 8) | 1`, so **bits
48–63 are always zero**. A record's model goes there as `m + 1` (0: no model, any other block).
No field moves, so the compiler's field offsets, the prelude's record routines, `rt_rec_parse`, JSON
and the automatic key keep their layout.

Who touches the header (Codex's inventory):

| Path | Effect on the model bits |
| --- | --- |
| allocation, small and large (`:1631`) | writes the whole header: a reused block starts with no model |
| small page preparation (`:1731`) | zeroes each free block's header |
| draining the mark stack (`:2291`) | reads n from the header: **must mask**, `shl rcx, 16; shr rcx, 24` keeps the 40 bits of n |
| marking (`:2700`), the forked child's marking (`:2368`) | sets the mark bit by OR, or marks in the shared map: the model bits survive |
| sweep of large blocks (`:2926`) and of small ones (`:2996`, `:3022`) | clears the mark bit only on survivors; zeroes dead headers; compares a whole header with zero to rebuild the free list (a free block has no model) |

`GC_STATS` and `GC_STRESS` use the same paths. `arm64/rt.c` only maps system calls; the collector is
the translated runtime. **The header keeps the block's physical size**: `rt_rec_parse` puts the
strings inside the record's block and then lowers only `[r - 8]` to the number of fields
(`:618`–`:669`), so the tag is ORed into the header, never rebuilt from the schema.

### Writing the tag

`or qword [rax - 16], imm` cannot carry `(m + 1) << 48`: a 64-bit OR takes a 32-bit sign-extended
immediate (`asm/asm.nihil:551`, `:754`), and `or [mem], reg` is not translated for the program's code
on macOS (`macos/translate.nihil:642`). The compiler emits, right after `RT_ALLOC`:

```text
mov rcx, [rax - 16]
mov rdx, (m + 1) << 48          ; mov r64, imm64
or  rcx, rdx
mov [rax - 16], rcx
```

`rcx` and `rdx` are free there (the allocation's result is in `rax`); never `rbx`, `r12`–`r15`, which
hold locals. It applies only to a block just allocated, whose model bits are zero.

## 2. Which records get a model, and from where

**Only models in a hierarchy** (with a parent or a child) are tagged: nothing dispatches on any
other, and the whole hierarchy is known before code is emitted (the prescan reads every model
first). The rule is one function used by every path, so a root gains its tag once it has a child.

| Path | Where | Model |
| --- | --- | --- |
| `NAME(…)`, `NAME()`, defaults (also inside an ARRAY default), a record global made at start | `emit_alloc_record()` (`src1/47_records.nihil`; globals: `src1/80_main.nihil:238`) | the constant above |
| `[r ; n]` | `__nihil_rec_rep` (`src1/prelude.nihil:267`) | the **static** model: copies through a PERSON are PERSONs |
| table reads: whole, by number, by key | native `rt_rec_parse`, fallback `__nihil_rec_parse` (`:316`, `:500`, `:1004`) | the model named |
| `from_json`, `from_json_items` | `__nihil_r_from_json` (`:2232`; `from_json_items` calls it, `:2275`) | the model named |

The prelude paths get the tag from the model's schema constant (`m_schema`, built in
`model_constants()`): `schema[1 + 2n] = (m + 1) << 48`, or 0 for an untagged model. One intrinsic for
the prelude, `__nihil_set_model(r, tag)`, emits the four instructions above with the tag in a
register. Only the record's block is tagged, never the outer ARRAY of rows. Growing a MAP or LIST,
`values()` and `list.array()` copy pointers, so the records keep their models.

The IDE's query programs rebuild a model from its fields, without the program's methods: dispatch
does not cross into them, and they need none.

## 3. The call

`model_method_call()` resolves `r.name(…)` with static model **S** to `k` (S's method or the nearest
inherited one) and must now pass S itself as well: the owner of `k` may be an ancestor of S, and the
models to test are S's descendants.

- If no descendant of S has an effective method other than `k`, the call stays exactly as today.
- Otherwise the receiver and the arguments are pushed as today (the receiver stays on the stack, so
  it remains reachable by the collector while the arguments allocate), and then:

```text
mov rax, [rsp + A]              ; the receiver: A = the bytes of the arguments after it
                                ; (size_param of each: 16 for an optional, 8 otherwise)
mov rcx, [rax - 16]
shr rcx, 48                     ; m + 1 (0: no model)
cmp rcx, <D1 + 1> ; je .t1      ; one test per concrete model whose effective method is not k,
cmp rcx, <D2 + 1> ; je .t2      ; grouped by target
call k ; jmp .done              ; everything else: k (also a record with no model)
.t1: call <D1's effective> ; jmp .done
.t2: call <D2's effective>
.done:
add rsp, <the arguments>        ; one cleanup for every branch; rax and rdx (presence) untouched
```

A descendant's **effective** method is the nearest ancestor (itself included) that declares `name`.
That covers a grandchild that does not replace the method. The table is computed for every concrete
descendant of S.

- `call_end` is set after the single `add rsp` cleanup, as today, so `?r.name(…)` works
  (`protected_expr()` checks `call_end == size_code`).
- `last_target` is set at every branch entry and at `.done`, for the peephole optimizer; every `call`
  is a fixup.
- Every target is recorded with `note_call()` (`call_from`/`call_to`, `src1/30_symbols.nihil:66`)
  from the calling method, in every pass, so it is compiled and its prelude dependencies are kept.
- Inline first: only direct calls and jumps, which the macOS translator handles. A shared stub
  changes where the arguments are and, ending in `jmp`, can make the translator save `x30` twice.
  Measure the code size before considering one.

## 4. Rules for a replacement

`CHILD.name` may replace an inherited `name`:

1. same number of parameters; after the first (the receiver, which is each declaration's own
   model), **each of exactly the same type**, optionals and collection types included;
2. the same return type, including optional or none. The prescan starts with returns unknown, and
   passes may infer one or promote it to an optional (`src1/60_commands.nihil:575`, `:918`), so this
   is checked **after the types settle**, before the compilation is accepted, also for replacements
   never called;
3. omitted arguments take their defaults from the declaration the static resolution found (`k`),
   since they are pushed before the dispatch. The same record seen as a PERSON and as a TEACHER may
   get different defaults; documented with an example;
4. the parameter rule is checked in the prescan, in both orders, where `no_replacement()` is today.

Inside `PERSON.label`, a call on its own receiver (`p.other()`) dispatches too, so a parent's method
can reach a child's replacement.

## 5. Calling the parent's version

No `super` yet. A replacement that needs the parent's behaviour calls a helper method **of the
parent** with another name (`PERSON.base_label`), not replaced in the hierarchy. An ordinary method
(`person_label(p = PERSON())`) does not work when that code reads the parent's private fields.

## 6. What changes for other programs

Every executable carries the whole runtime, so the collector's mask changes every program's bytes,
and the tag word changes the size of every schema constant. Byte identity with the previous compiler
does not hold here. The check is semantic: the whole suite, and the code emitted for methods
compared before and after on programs with no models built from another.

## 7. Tests

PR 1, the model number (no dispatch yet):

- every creation path tags right: constructor, default, ARRAY default, record global, `[r ; n]`
  through a parent (gets the parent's), table read native (with strings in the block) and fallback,
  by number, by key, `from_json`, `from_json_items`;
- an intrinsic that reads the model back, for the tests only;
- the collector: `make gcstress`, the four `GC_STATS`/`GC_STRESS` builds, small and large blocks that
  survive and are reused, the forked marker and the sweep in steps.

PR 2, dispatch:

- through a LIST, ARRAY and MAP of the parent; a parameter of the parent's type; a grandchild that
  inherits a replacement; a record with no model;
- arguments that allocate and have effects, optionals before and after plain arguments, returns
  optional and none, different defaults;
- a parent's method dispatching on its own receiver; declarations in both orders;
- `?p.label()` and `?{ … }` with an error inside a replacement; locals kept with and without `-O0`;
- errors: a replacement with other parameter types or count, another return type (also inferred
  late, also never called);
- macOS: `macos/test-native.sh`.

## 8. Effort

About **2 to 3 weeks** in all: PR 1 (runtime mask, schema tag, every creation path, the collector's
tests) about a week; PR 2 (the call, the late return check, the tests) one to two.
