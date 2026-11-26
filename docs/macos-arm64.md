# Nihil on macOS (Apple silicon, ARM64) — the port

## Now: native through a translator (works)

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
optimizations read instead of the x86 bytes. With `-arm`, each operation also writes its ARM64 text
(`src1/42_arm.nihil`), on stderr after the map and a line `nihil-arm`; the translator then uses that
text for the program and translates only the runtime:

    NIHIL_ARM=1 macos/nihil prog.nihil [out]
    NIHIL_ARM=1 macos/test-native.sh          (127 programs, also with NIHIL_O0=1)
    macos/same-bytes.sh                       the x86 output, byte for byte, against a reference

The first step keeps the translator's conventions (rax → x9, the x86 stack emulated in x28), so its
runtime serves as it is; the speed is the translator's. The gains come next, operation by operation:
the native stack and more registers for locals inside methods, then a native runtime.

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
