# Profiling: where a program spends its time

```sh
nihil profile generate.nihil          # builds it with -profile, runs it, then reports
```

The program runs as `nihil run` would run it: same arguments, input and output, same exit
code. Every 10 ms of CPU time the process is interrupted and notes where it was. When it ends,
it writes those samples to `nihil-profile.PID` (PID is its process id) in the current
directory. The report then goes to the standard error:

```text
profile: 564 samples, 5.64 s of CPU (a sample every 10 ms)

where the CPU was (self):
 16.8%  0.95 s  __nihil_map_slot
 12.9%  0.73 s  gc_small_page
 11.0%  0.62 s  gc_sweep_step
  7.8%  0.44 s  rt_alloc
  6.2%  0.35 s  rt_concat
   …

the runtime's time, by the method that called it:
 20.0%  1.13 s  SLOT.json
  5.9%  0.33 s  PLANOGRAM.kanban
   …

in the method or in what it called (total):
 96.3%  5.43 s  main
 67.0%  3.78 s  PLANOGRAM.page_json
   …
```

- **self**: the method, or the runtime routine, that was running. Prelude methods start with
  `__nihil_` (maps, JSON, FLOAT text). `rt_` and `gc_` routines are the runtime: allocation,
  the collector (`gc_`), joining texts (`rt_concat`) and similar work.
- **the runtime's time, by the method that called it**: each sample spent in the runtime is
  charged to the first method of the program on the stack. In this report, `SLOT.json` makes
  a fifth of the program's time go to allocating and collecting.
- **total**: the samples in which the method was anywhere on the stack, counted once per
  sample. A method that calls itself counts once.

Samples count CPU time, user and system together, so time spent waiting (sleeping, reading the
network) does not show. A program that runs for less than 10 ms of CPU has no samples.

## By hand

`-profile` before the path is the compiler's option (`NIHIL_PROFILE=1 ./nihilc1.sh` in the
repository). The executable it makes runs as usual and writes the profile when it ends; it
says where on stderr (`nihil: profile in nihil-profile.12345`). `tools/profile.nihil` reads
the path of the profile on its standard input:

```sh
printf '%s\n' '-profile generate.nihil' | build/nihilc1 > gen && chmod +x gen
./gen
./nihilc1.sh tools/profile.nihil build/profile-report
ls nihil-profile.* | build/profile-report
```

The executable writes its profile when it ends normally, with `::`, or with a runtime
error. A program killed by a signal writes none.

## How it works

- **The compiler** (`-profile`) adds a call at the start of the program that hands the runtime
  a table: one line `start end name` (decimal addresses) for each runtime routine, each method
  compiled and the top level. The runtime's routines come from its plain labels, which
  `make runtime` writes into `src1/runtime_bin.nihil` (`runtime_sym_names`, `runtime_sym_ofs`).
- **The runtime** (`rt_prof_start`) maps a 64 MiB buffer, installs a `SIGPROF` handler
  (`SA_RESTART`, so system calls resume) and starts `ITIMER_PROF` at 10 ms. Each sample is
  the interrupted address, then return addresses, then 0:
  - inside the runtime, whose routines make no stack frame, the words between the stack pointer
    and the frame that lie in the code right after a `call rel32`, up to the first one in the
    program;
  - then the chain of frames (`[rbp]`, `[rbp + 8]`), kept between the stack pointer and the top
    of the stack.
- **At the exit** (`rt_exit`, and a runtime error), `rt_prof_write` stops the timer and writes
  the table, `--\n` and the samples.
- **The collector's marking child** is made by `fork`, which does not inherit the timer, so it is
  not sampled. Programs started with `cmd.start` are not sampled either.

The cost is under 1% (the planogram's `generate`: 5.86 → 5.91 s, the same output and memory).

## Limits

- **Linux x86_64 only, for now.** On macOS the translated program runs without samples, and
  `nihil profile` says so.
- An address is named by the routine or method around it. Code the compiler writes inline
  (a query's loop, `rows.where(…)`) counts for the method that holds it.
- Up to 48 return addresses per sample: in deeper recursion, the outermost callers are left out.
