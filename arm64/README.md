# arm64/ — Nihil on macOS (Apple silicon)

- rt.c: what the runtime needs from macOS. The runtime itself is Linux's, src/runtime.asm,
  translated to ARM64 with each program (macos/translate_rt.py); rt.c only makes its Linux
  system calls with macOS's (nh_linux_syscall: numbers, flags, layouts and errno values, mmap
  and madvise, mincore's 16 KiB pages, sockets, /proc's files) and gives the emulated x86 stack
  (nh_emu_stack). See docs/macos-arm64.md.
- fib.s: phase 0 of the port, what an ARM64 backend would have emitted for a small program (by
  hand); the port became a translator instead.

The runtime is tested by the programs of tests/ and tests/stage1/ on the Mac (macos/test-native.sh,
macos/dev.sh), with GC_STRESS too. Until v4.5, rt.c was a second runtime in C, with tests of its
own (rt_test.c, gc_test.c, file_test.c, net_test.c, out_test.c, sys_test.c), removed with it.
