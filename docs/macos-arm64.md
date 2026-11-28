# Nihil on macOS (Apple silicon, ARM64) — the port

## Since v4.13: the compiler writes ARM64 itself (-native), by default

`nihil` on a Mac compiles every program with `-native` (section "-native" below): the compiler
writes the whole program as ARM64 assembly, its runtime included (written in Nihil,
`src1/native.nihil`), and clang assembles and links it with the system's library alone. No
translator, no `arm64/rt.c`. The package's compiler is built the same way, and it compiles itself
to the same assembly (fixed point, in the release workflow). Its programs, and the package's own,
run on macOS 11 and later (`-mmacosx-version-min=11.0`; without it clang stamps the build machine's
version).

    macos/nihil-native prog.nihil [out]   what `nihil` does (./nihilc1.sh too, in the repository)
    macos/test-native-c.sh                tests/ and tests/stage1/ this way (NIHIL_O0=1, NIHIL_GC_TORTURE=1)

`NIHIL_TRANSLATE=1 nihil …` keeps the way before v4.13, the translator of the next section, in
v4.13 and v4.14, in case a program needs it (tell us). In v4.15 the translator and `arm64/rt.c`
leave the package.

## Before v4.13 (NIHIL_TRANSLATE=1): native through a translator

Instead of a second backend in the compiler (the plan below), the x86-64 code the compiler
already emits is **translated** to ARM64 assembly. The compiler emits a closed set of about 180
instruction forms (a stack machine, no indirect jumps), so the translation is instruction by
instruction, and the compiler itself is translated the same way: there is a native compiler.

    macos/nihil prog.nihil [out]     a native Mach-O executable (NIHIL_O0=1: no optimizations)
    ./nihilc1.sh prog.nihil [out]    the same on macOS (on Linux, unchanged)
    macos/test-native.sh             tests/ and tests/stage1/ natively (101 programs)
    macos/test.sh                    the same, compiled by the x86 compiler in a Linux VM

- `nihilc1 -map` writes `nihil-map RUNTIME_END CODE_END SIZE` on stderr: where the program's
  code starts (after the header and the runtime) and where its constants start. The output
  is the same with and without it.
- The translator, `macos/translate.nihil` (a Nihil program; until v4.5 it was Python),
  disassembles `[RUNTIME_END, CODE_END)` (clang + objdump) and writes
  `.s`: rax rcx rdx rsi rdi → x9–x13; rbx r12–r15 → x19–x23 (kept by C); rbp → x29; the x86
  stack is emulated exactly by x28 in its own area (`nh_emu_stack`); globals → a symbol through
  x27; constants are copied after the code. Calls into the runtime's jump table become calls into
  the translated runtime (below).
  cmp/test/ucomisd map to cmp/tst/fcmp, with the conditions mapped for each.
- **The runtime is Linux's** (`src/runtime.asm`, which the executable carries after its header),
  translated by the same program with each instruction's exact meaning (it is hand-written,
  not the compiler's closed set): r8–r10 → x24–x26, r11 → x7; 32-bit operations on w registers,
  8/16-bit writes with bfi; absolute data addresses (and immediates that are such addresses)
  relative to x27, the runtime's slots and buffers in `_nh_data_start` just below the globals;
  flags set in ARM only where a later instruction reads them (a carry read after an addition, or
  any flag read across a call, is refused); each routine has a label for jumps and one for calls
  (which pushes the return address on the x86 stack); `rep movs/stos`, `repne scas`, `rep cmps` →
  loops; `div` only after `xor edx, edx`, `idiv` only after `cqo`. Every `syscall` goes to
  `nh_linux_syscall` in `arm64/rt.c`, which makes it with macOS's: mmap flags, MADV_DONTNEED as a
  fresh mapping (macOS does not zero), mincore from 16 KiB pages to 4 KiB, sockets (sockaddr,
  SOL_SOCKET options, SO_NOSIGPIPE), errno numbers, and `/proc/self/environ`, `/proc/uptime`,
  `/proc/sys/kernel/random/uuid` as files of their own. The start sets PLAT_STACK_TOP (the
  emulated stack) and PLAT_DATA_END, which Linux finds in /proc/self/maps and the ELF header.
  `arm64/rt.c` is only that layer and the emulated stack (until v4.5 it was a second runtime, in C).
- The translator is built by `macos/translator.sh` (`build/macos/translate`): the native compiler
  emits its x86-64 code, the previous translator translates it, and the new one must translate
  itself into the same assembly. The first one is a seed (`macos/seed.sh`): `macos/translate.nihil`
  compiled by a Nihil already installed or by the latest release's package. It reads three lines on
  its standard input: the program (`.x86`), the map and the `.s` to write.
- `build/macos/nihilc1-native` (the translated compiler) compiles `src1/nihilc1.nihil` to
  the same bytes as the x86 compiler, and the compiler it builds does the same (fixed point).
- The IDE server (`ide/server.nihil`) builds native programs on macOS and its assembly view
  shows ARM64.
- Collector: a large block is scanned only on pages mincore reports anything about. On macOS a
  page never written reports only MINCORE_ANONYMOUS (0x80); a compressed or swapped page is
  not "in core" but has other flags and must be scanned (skipping it freed live blocks under
  memory pressure). The Linux runtime (`src/runtime.asm`) has the same risk with swap: mincore
  says 0 for a swapped page there.

Bootstrapping a new Mac: `build/macos/nihilc1-native` comes from translating the output of
`printf -- "-map src1/nihilc1.nihil\n" | build/nihilc1` (Linux), once; after that the Mac
rebuilds its compiler by itself (`macos/nihil src1/nihilc1.nihil build/macos/nihilc1-native`).

## Next: the compiler writes ARM64 itself (branch arm64-backend)

Every instruction the compiler emits is an operation (`src1/40_emit.nihil`: `op_load`, `op_push`,
`op_ld`, `op_jcc` … about 60 kinds, `OP_*` in `src1/00_data.nihil`), noted in a ring that the
optimizations read instead of the x86 bytes. Jumps name labels (`label_new`, `label_bind`,
`label_here`), not positions in the code. With `-arm`, all of a pass's operations are kept, with
where each label was bound, and after the last pass `src1/42_arm.nihil` writes them as ARM64 text,
on stderr after the map and a line `nihil-arm`; the translator then uses that text for the program
and translates only the runtime:

    NIHIL_ARM=1 macos/nihil prog.nihil [out]
    NIHIL_ARM=1 macos/test-native.sh          (127 programs, also with NIHIL_O0=1)
    macos/same-bytes.sh                       the x86 output, byte for byte, against a reference

The first step keeps the translator's conventions (rax → x9, the x86 stack emulated in x28), so its
runtime serves as it is; the speed is the translator's. The gains come next, operation by operation:
the native stack and more registers for locals inside methods, then a native runtime.

### -native: no translator, and a runtime in Nihil

`-native` writes the whole program as ARM64 assembly on stdout (`_main`, the program, its runtime,
the constants, the globals); clang links it with the system's library (libSystem) alone: no x86
code, no translator, no C of ours. The program's stack stays a stack of 8-byte slots in x28 (the
code builds arrays on it, with 8-byte elements), mapped by `_main`; `sp` is only the system's.

    macos/nihil-native prog.nihil [out]
    macos/test-native-c.sh                    the battery (NIHIL_O0=1, NIHIL_GC_TORTURE=1)

The runtime is Nihil: `src1/native.nihil`, embedded in the compiler like the prelude (`make
build/native_text.nihil`) and put before it in every `-native` program. A routine `RT_<NAME>` is the
method `__nihil_rt_<name>`: the registers it reads (rax, rcx, rdx, rsi, rdi, in that order) are its
arguments; a second result (read_at's size and stamp, unprotect's rdx) comes back through globals;
a routine reached by a jump (an index out of bounds, a division by zero) through a label `Lrt<n>`
that pushes them. Its locals are named `__…`: a program's global of the same name would be used
otherwise. It reaches the system with intrinsics the compiler knows only with `-native`:
- `__nihil_c("name", a1 … a8)`, `__nihil_c32` (an `int` result) and `__nihil_cvar("name", F, …)`
  (F fixed arguments, the others on C's stack: `openat`'s mode, `fcntl`'s) call a function of
  libSystem; `__nihil_peek`, `__nihil_peekb` and `__nihil_poke8` read and write memory;
- the prelude speaks Linux's system calls (it is the same on both systems): `__nihil_rt_sys` makes
  them with macOS's functions, with Linux's numbers, flags, layouts (stat, dirent, sockaddr) and
  errno values, and the files of /proc programs read (environ, uptime, a random uuid).

All of src/runtime.asm's routines are there, with their messages. The tests pass (optimized, `-O0`,
and with the collector every 64 KiB), the compiler built this way gives the same output (its fixed
point), and the planogram server serves the same pages. Written in Nihil, the hottest routines
were slower than in C at first (reading a table of a million records: 60 → 120 ms); the code the
compiler writes then learned registers for arguments and more of them for locals (below), and the
routines their own fast ways: 86–93 ms now. What is left is mostly `rec_parse`, whose loop has more
values than registers.

The collector (in `src1/native.nihil`) is conservative mark and sweep, like the x86 runtime's. Its
memory is its own (mmap, calloc): it never makes a block of the heap it collects. Its contract with
the code the compiler writes:
- it runs only inside a call of the runtime (alloc): every call of the runtime is a safepoint;
- roots: the globals, the program's stack (from x28 to its top: the methods keep there the
  registers they save, and the calls the rcx, rsi, rdi that stay live), the registers x19 … x28
  (`_nh_regs`), and the records of the protected calls;
- any word that points into an allocated block keeps it (a record's texts are inside its block);
- a large block is scanned only where it was written (`mincore`): a big array of zeros costs nothing.

How it works:
- what is in use lives in a map, not in the blocks: a byte for each 32 bytes of the heap. A mark
  fills a second map with the blocks it reaches; then the two swap. There is no sweep: each size
  class hands out the next block whose byte is clear, in the pages the mark left with room, and a
  page with nothing marked goes back to the system without being read;
- from 64 MB live on, a child process (fork) marks, as in the x86 runtime, while the program goes
  on; the program marks in the map each block it hands out meanwhile, and waits for the child only
  after making as many bytes as were live. Since nothing writes the heap's pages but the program,
  the fork costs only the pages the program itself writes (copy on write);
- an allocation of a small block is done inside `__nihil_rt_alloc` when its class's page has room
  and no collection is due (the class from a table; the block zeroed 8 bytes at a time);
- the C collector asked for each block ahead of reading it (prefetch); in the Nihil one it made the
  child's mark slower (22 → 38 ms), at the push or 16 blocks ahead, and it is not used
  (`__nihil_prefetch` is there). Marking with several threads was tried and was slower: a big array
  is one block.

`NIHIL_GC_STATS=1` shows each collection (and the program's own peak at its end);
`NIHIL_GC_TORTURE=1` collects every 64 KiB and forks from 1 MB live (the battery and the
compiler's fixed point pass so, the compiler with about 100 forks).

Measured on an M4 (`CHECK=1 SIZES=1000000 sh bench/compare/run.sh macos/nihil-native`, three runs),
against the translator's path: making the records 70–112 ms (60–80), reading the table 61–67 ms
(74), the first key lookup, which builds the index, 72–78 ms (72–79); peak memory 352 MB (325). The
program stops for at most 3 ms in a forked collection. The compiler compiles itself in 0.25 s (0.43
s with the collector before this one).

The code (`src1/42_arm.nihil`, `-native` optimized) keeps x86's operations but not their copies:
- liveness of rax, rcx, rdx, rsi, rdi and the flags for every operation (backwards over the jumps;
  each runtime routine reads only its own arguments);
- a constant, or a local in a register, loaded into rax or rcx waits where it is: the operations
  that read it read it there (`cmp x24, #1000`, `ldr x9, [x25, #8]`), and it is copied only where
  it is still live (a label, a jump's target, an operation not followed);
- a comparison whose 0 or 1 only a jump looks at becomes the jump (`cmp ; b.lt`, `cbz`); a result
  goes straight into the local it is stored in; push … pop of a value waiting is no memory; a 0 is
  stored from `xzr`;
- division and remainder are `sdiv`/`msub` (by a power of 2, shifts), with x86's results (INT_MIN /
  -1, the remainder's sign) and the runtime's error for a division by zero.

Calls and methods:
- a method call reads no register and leaves none (liveness); a method's start and end are one
  `stp`/`ldp` of x29 and x30; an empty frame costs nothing;
- a method whose arguments take 8 slots or fewer gets them in x1 … x8: the caller pushes nothing and
  drops nothing; a leaf (it calls nothing) whose frame does not escape keeps its parameters there,
  another method pushes them at its start, where its body reads them;
- a slot no call crosses while it is live (from its first operation to its last, widened to the
  loops around it) may live in x1 … x8, with no save; the others in x19 … x26 (saved by a method);
  the program's own code keeps to x19 … x26 (it calls its globals' start, placed at its end);
- a constant of more than one instruction used in a loop gets a register of x19 … x26 still free,
  loaded once;
- a jump on which rax is known to be 0 or not, to the join of `&&` or `||`, goes on past the join's
  own test (`Lt<n>`); a join no jump reaches any more is no label, so `d < 0 || d > 9` is
  `cmp ; b.lt ; cmp ; b.le`;
- a value computed and pushed in the middle of an expression (`a[i] * b[i]`) waits in a free
  register of x0 … x7, not on the stack, when nothing before its pop calls C or a method;
- each method has a local symbol `_m_<name>_<k>`, so profilers and backtraces name it
  (`xctrace record --template 'Time Profiler'`).

The compiler's own arrays for `-arm`/`-native` (operations, labels, the text) start small and
double: a collection reads only what is there (`mincore` of 1.6 GB of untouched arrays took 25 ms a
collection). Compiling itself natively: 0.22 → 0.14 s, with no collection at all.

Measured (best of 3, M4; C with `cc -O2`): a double loop of `+`, `^` and `%`, 0.37 → 0.154 s (C
0.142); Collatz up to 3 000 000, 1.45 → 0.69 s (C 0.32); a dot product in a method called 100 000
times, 0.083 → 0.056 s (C without vectorizing 0.052). A sieve and `fib(35)` do not change: a BOOL
array takes 8 bytes an element, and `fib` is about 7 cycles a call already (clang transforms the
recursion). The compiler compiles itself in 0.157 s (its own profile: the symbol lookup, a linear
search, is the largest part).

## The original plan (a second backend)

## The target

- The compiler writes **ARM64 assembly text** (`.s`); `clang -arch arm64` assembles it, links
  it with libSystem and signs it (ad-hoc), which macOS requires. No Mach-O writer, no code
  signing and no ARM64 encoder of our own at first; direct emission can come later.
- The kernel is reached through **libSystem** (`write`, `read`, `mmap`, `open`, `socket`,
  `posix_spawn`, `exit` …): macOS does not keep its system call numbers stable.
- Code is position independent (`adrp`/`add` for globals and texts): macOS does not load
  programs at a fixed address, so the x86 absolute addressing (`[disp32]`) has no equivalent.
- Registers: x86 `rax` → `x0` (the value), `rdx` → `x1` (an optional's presence), `rbp` →
  `x29` (the frame). The stack stays 16-byte aligned, so a push takes 16 bytes
  (`str x0, [sp, #-16]!`). The optimizer's registers (`FLAG_REG`) go to x19–x28.
- Texts and arrays keep their layout: the length in the 8 bytes before the data.

## Phases

0. **Done: the path works.** `arm64/runtime.s` (output buffer, numbers, texts, exit) and
   `arm64/fib.s` (what the compiler will emit for a small program, by hand, in its
   stack-machine style). `clang -arch arm64 -o fib runtime.s fib.s` gives a signed Mach-O that
   prints the right numbers; fib(35) and the first 31 in 0.12 s on an M4.
1. **The runtime in ARM64** (on the Mac, now): every routine of `src/runtime.asm` (2,600 lines)
   — the heap and arrays (`mmap`), the conservative garbage collector (stack and registers),
   texts, input (`<<`, `<<<`), files and tables (`open`, `pread`, `pwrite`, `fstat`), runtime
   errors, the native helpers (record parsing, JSON value end, copies), time, network
   (`socket`, `bind`, `accept` …) and processes (`posix_spawn`, `waitpid`, `kill`). Tested
   with hand-written callers, as phase 0.
2. **A backend layer in the compiler** (needs Linux x86_64 to build): the 410 places that emit
   x86 bytes (`40_emit`, `47_records`, `48_library`, `49_maps`, `50_expressions`,
   `60_commands`, `80_main` …) go through named operations (load a local, push, pop, compare,
   call a runtime routine …). The x86 output must stay byte for byte the same: `make compare`
   and `make fixedpoint` prove it at every step. Then an ARM64 implementation of the same
   operations that writes text.
3. **Cross-compiling**: `nihilc1 --target=macos-arm64 prog.nihil` on Linux writes `prog.s`;
   on the Mac, `clang` and the test battery (`tests/`, the same expected outputs).
4. **Self-hosting**: `nihilc1` compiled for macOS, compiling on the Mac (it calls `clang`), and
   the fixed point there (the Mac compiler compiles itself to the same text).
5. **The optimizer** on ARM64 (registers x19–x28; `-O` and `-O0` give the same results).
6. **The tools**: `make` targets for the Mac, the IDE server and the MCP server on the Mac.

Stage 0 (`src/nihilc.asm`, NASM) stays as it is: it only builds stage 1 on Linux, and the
first macOS compiler comes from stage 1 (phase 3).
