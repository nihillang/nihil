// macos/native/rt.c — what is left in C of the runtime of programs the compiler writes whole in
// ARM64 (-native): the output, the errors and the protected calls, and the program's start. The rest
// is Nihil: src1/native.nihil (texts, files, the network, the system's calls, the heap and its
// collector), which reaches this file through the functions it exports (nh_flush_out, nh_fail_bytes,
// nh_regs, nh_globals_start …).
//
// A routine RT_<NAME> of src/runtime.asm still here is nh_<name>, with its behaviour: the program's
// registers rax, rcx, rdx, rsi, rdi come as the first five arguments and its stack pointer (x28) as
// the sixth; the result goes back in rax and rdx. Their adapters are in the program's text
// (42_arm.nihil, native_end).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

typedef int64_t I;
typedef uint64_t U;
typedef struct { I a, d; } R;
#define ARGS I rax, I rcx, I rdx, I rsi, I rdi, I *stack
#define UNUSED (void)rax; (void)rcx; (void)rdx; (void)rsi; (void)rdi; (void)stack

I nh_linux_syscall(I n0, I a1, I a2, I a3, I a4, I a5, I a6);       // arm64/rt.c
#define sys nh_linux_syscall

static R ret(I a) { R r = { a, 0 }; return r; }
static R ret2(I a, I d) { R r = { a, d }; return r; }

extern I nh_globals[];                       // the program's (its text: native_end)
extern I nh_globals_size;
I nh_sp;                                     // x28 at the last call of the runtime (the adapters)
I nh_ret_rcx;                                // read_at's third result
static I base_sp;                            // sp in the program's code (protected calls restore it)
void nh_resume(I *record, I sp) __attribute__((noreturn));        // the program's text

// ---- the program's stack ----
#define STACK_SIZE ((size_t)256 << 20)
static char *stack_top;

I nh_start(int argc, char **argv, I sp) {
    (void)argc; (void)argv;
    base_sp = sp;
    char *s = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s == MAP_FAILED) { write(2, "nihil: out of memory\n", 21); _exit(2); }
    stack_top = s + STACK_SIZE - 64;
    return (I)stack_top;
}

// ---- output, buffered ----
static char out_buf[65536];
static size_t out_len;

static void write_all(int fd, const char *p, I n) {
    while (n > 0) {
        I k = sys(1, fd, (I)p, n, 0, 0, 0);
        if (k == -4) continue;
        if (k <= 0) return;
        p += k;
        n -= k;
    }
}

static void flush_out(void) { write_all(1, out_buf, (I)out_len); out_len = 0; }

static void put(const char *p, I n) {
    while (n > 0) {
        if (out_len == sizeof out_buf) flush_out();
        I k = (I)(sizeof out_buf - out_len);
        if (k > n) k = n;
        memcpy(out_buf + out_len, p, (size_t)k);
        out_len += (size_t)k;
        p += k;
        n -= k;
    }
}

// the decimal digits of v, ending at end → their start
static char *decimal(I v, char *end) {
    U u = v < 0 ? (U)0 - (U)v : (U)v;
    char *p = end;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--p = '-';
    return p;
}

static I len_of(I s) { return ((I *)s)[-1]; }

// ---- errors, and the protected calls (SPEC §25) ----
#define PROT_MAX 64
static I prot[PROT_MAX + 1][11];             // x28, x29, where to resume, x19 … x26
static I prot_depth;
static int err_capture;
static char err_buf[4000];
static I err_len;

// what goes to stderr: written, or kept for ?! inside a protected call
static void err_write(const char *p, I n) {
    if (err_capture) {
        if (n > (I)sizeof err_buf - err_len) n = (I)sizeof err_buf - err_len;
        memcpy(err_buf + err_len, p, (size_t)n);
        err_len += n;
        return;
    }
    write_all(2, p, n);
}
static void err_str(const char *s) { err_write(s, (I)strlen(s)); }
static void err_num(I v) { char b[24]; char *p = decimal(v, b + 24); err_write(p, b + 24 - p); }

static void err_begin(void) {
    if (prot_depth > 0) { err_len = 0; err_capture = 1; return; }
    flush_out();
    err_str("nihil: ");
}

static __attribute__((noreturn)) void err_end(void) {
    if (!err_capture) _exit(2);
    err_capture = 0;
    if (err_len > 0 && err_buf[err_len - 1] == '\n') err_len--;
    prot_depth--;
    nh_resume(prot[prot_depth], base_sp);
}

static __attribute__((noreturn)) void fail(const char *m) { err_begin(); err_str(m); err_str("\n"); err_end(); }

// regs: x28, x29, x19 … x26 as the protected call has them (its adapter)
R nh_protect(I resume, I *regs) {
    if (prot_depth >= PROT_MAX) fail("more than 64 protected calls inside each other");
    I *p = prot[prot_depth++];
    p[0] = regs[0]; p[1] = regs[1]; p[2] = resume;
    for (int i = 0; i < 8; i++) p[3 + i] = regs[2 + i];
    return ret(resume);
}

R nh_unprotect(ARGS) { UNUSED; prot_depth--; err_len = 0; return ret2(rax, rdx); }

// for the routines written in Nihil (src1/native.nihil), until output and errors are Nihil too:
// the output written so far, and an error (its text, then a line break) as the runtime reports one
void nh_flush_out(void) { flush_out(); }
void nh_fail_bytes(const char *p, I n) { err_begin(); err_write(p, n); err_str("\n"); err_end(); }

R nh_div(ARGS) { UNUSED; if (rcx == 0) fail("division by zero"); return ret(rcx == -1 ? (I)(0 - (U)rax) : rax / rcx); }
R nh_mod(ARGS) { UNUSED; if (rcx == 0) fail("division by zero"); return ret(rcx == -1 ? 0 : rax % rcx); }

R nh_print_int(ARGS) { UNUSED; char b[24]; char *p = decimal(rax, b + 24); put(p, b + 24 - p); return ret(0); }
R nh_print_bool(ARGS) { UNUSED; if (rax) put("true", 4); else put("false", 5); return ret(0); }
R nh_print_str(ARGS) { UNUSED; put((const char *)rax, len_of(rax)); return ret(0); }
R nh_print_int_opt(ARGS) { if (!rdx) { put("_", 1); return ret(0); } return nh_print_int(rax, rcx, rdx, rsi, rdi, stack); }
R nh_print_bool_opt(ARGS) { if (!rdx) { put("_", 1); return ret(0); } return nh_print_bool(rax, rcx, rdx, rsi, rdi, stack); }
R nh_print_str_opt(ARGS) { if (!rdx) { put("_", 1); return ret(0); } return nh_print_str(rax, rcx, rdx, rsi, rdi, stack); }
R nh_write_byte(ARGS) { UNUSED; char c = (char)rax; put(&c, 1); return ret(0); }

R nh_error_int(ARGS) { UNUSED; flush_out(); err_num(rax); return ret(0); }
R nh_error_bool(ARGS) { UNUSED; flush_out(); if (rax) err_write("true", 4); else err_write("false", 5); return ret(0); }
R nh_error_str(ARGS) { UNUSED; flush_out(); err_write((const char *)rax, len_of(rax)); return ret(0); }
R nh_error_int_opt(ARGS) { if (!rdx) { flush_out(); err_write("_", 1); return ret(0); } return nh_error_int(rax, rcx, rdx, rsi, rdi, stack); }
R nh_error_bool_opt(ARGS) { if (!rdx) { flush_out(); err_write("_", 1); return ret(0); } return nh_error_bool(rax, rcx, rdx, rsi, rdi, stack); }
R nh_error_str_opt(ARGS) { if (!rdx) { flush_out(); err_write("_", 1); return ret(0); } return nh_error_str(rax, rcx, rdx, rsi, rdi, stack); }

R nh_error(ARGS) {
    UNUSED;
    if (strcmp((const char *)rsi, "out of memory") == 0) prot_depth = 0;   // never caught: the heap may be inconsistent
    err_begin();
    err_str((const char *)rsi);
    err_str("\n");
    err_end();
}

R nh_fail(ARGS) { UNUSED; err_begin(); err_write((const char *)rax, len_of(rax)); err_str("\n"); err_end(); }

R nh_error_index(ARGS) {
    UNUSED;
    I n = len_of(rax);
    err_begin();
    err_str("index "); err_num(rcx); err_str(" out of bounds (size "); err_num(n); err_str(")\n");
    err_end();
}

// ---- for the collector written in Nihil: its roots ----
void nh_regs(I *b) {                         // x19 … x28 as the caller has them
    __asm__ volatile("stp x19, x20, [%0]\n\tstp x21, x22, [%0, #16]\n\tstp x23, x24, [%0, #32]\n\t"
                     "stp x25, x26, [%0, #48]\n\tstp x27, x28, [%0, #64]" : : "r"(b) : "memory");
}
I nh_globals_start(void) { return (I)nh_globals; }
I nh_globals_bytes(void) { return nh_globals_size; }
I nh_stack_top(void) { return (I)stack_top; }
I nh_prot_start(void) { return (I)prot; }
I nh_prot_end(void) { return (I)prot[prot_depth]; }
// the text of the last error caught by ?! (err_text, in Nihil)
I nh_err_len(void) { return err_len; }
I nh_err_buf(void) { return (I)err_buf; }
