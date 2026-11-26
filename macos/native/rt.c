// macos/native/rt.c — the C runtime of programs the compiler writes whole in ARM64 (-native).
//
// A routine RT_<NAME> of src/runtime.asm is nh_<name> here: the program's registers rax, rcx, rdx,
// rsi, rdi come as the first five arguments and its stack pointer (x28) as the sixth; the result
// goes back in rax and rdx. The program's stack is of 8-byte slots, allocated here (nh_start).
//
// First milestone: the routines of small programs (output, strings, arrays, errors), with a heap
// that only grows; the collector comes next, with the contract of its roots (docs/macos-arm64.md).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

typedef int64_t I;
typedef struct { I a, d; } R;
#define ARGS I rax, I rcx, I rdx, I rsi, I rdi, I *stack
#define UNUSED (void)rax; (void)rcx; (void)rdx; (void)rsi; (void)rdi; (void)stack

static R ret(I a) { R r = { a, 0 }; return r; }

// ---- the program's stack and arguments ----
#define STACK_SIZE ((size_t)256 << 20)
static int nh_argc;
static char **nh_argv;

I nh_start(int argc, char **argv) {
    nh_argc = argc;
    nh_argv = argv;
    char *s = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s == MAP_FAILED) { perror("nihil: stack"); exit(2); }
    return (I)(s + STACK_SIZE - 64);
}

// ---- output, buffered as the x86 runtime does ----
static char out_buf[1 << 16];
static size_t out_len;

static void flush_out(void) {
    size_t done = 0;
    while (done < out_len) {
        ssize_t n = write(1, out_buf + done, out_len - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    out_len = 0;
}

static void put(const char *p, size_t n) {
    while (n > 0) {
        if (out_len == sizeof out_buf) flush_out();
        size_t k = sizeof out_buf - out_len;
        if (k > n) k = n;
        memcpy(out_buf + out_len, p, k);
        out_len += k;
        p += k;
        n -= k;
    }
}

// the decimal digits of v, ending at end → their start
static char *decimal(I v, char *end) {
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char *p = end;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--p = '-';
    return p;
}

static I len_of(I s) { return ((I *)s)[-1]; }

R nh_print_int(ARGS) { UNUSED; char b[24]; char *p = decimal(rax, b + 24); put(p, (size_t)(b + 24 - p)); return ret(0); }
R nh_print_bool(ARGS) { UNUSED; if (rax) put("true", 4); else put("false", 5); return ret(0); }
R nh_print_str(ARGS) { UNUSED; put((const char *)rax, (size_t)len_of(rax)); return ret(0); }
R nh_write_byte(ARGS) { UNUSED; char c = (char)rax; put(&c, 1); return ret(0); }

// ---- termination and errors ----
R nh_exit(ARGS) { UNUSED; flush_out(); exit((int)rdi); }

static void err_begin(void) { flush_out(); fputs("nihil: ", stderr); }
static void err_end(void) { fflush(stderr); exit(2); }

R nh_error(ARGS) { UNUSED; err_begin(); fprintf(stderr, "%s\n", (const char *)rsi); err_end(); return ret(0); }

R nh_error_index(ARGS) {
    UNUSED;
    err_begin();
    fprintf(stderr, "index %lld out of bounds (size %lld)\n", (long long)rcx, (long long)len_of(rax));
    err_end();
    return ret(0);
}

static void fail(const char *m) { err_begin(); fprintf(stderr, "%s\n", m); err_end(); }

// ---- arithmetic ----
R nh_div(ARGS) { UNUSED; if (rcx == 0) fail("division by zero"); return ret(rcx == -1 ? (I)(0 - (uint64_t)rax) : rax / rcx); }
R nh_mod(ARGS) { UNUSED; if (rcx == 0) fail("division by zero"); return ret(rcx == -1 ? 0 : rax % rcx); }

// ---- the heap: blocks of [header][length][8-byte words …]; the value points at the words ----
// (for now it only grows: the collector comes with the next milestone)
static char *heap_at, *heap_end;

static I *alloc_words(I n) {
    if (n < 0) fail("negative array size");
    if (n >= ((I)1 << 40)) fail("array too large");
    size_t bytes = ((size_t)n * 8 + 16 + 15) & ~(size_t)15;
    if (heap_at == NULL || (size_t)(heap_end - heap_at) < bytes) {
        size_t chunk = (size_t)64 << 20;
        if (chunk < bytes) chunk = bytes;
        char *c = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (c == MAP_FAILED) fail("out of memory");
        heap_at = c;
        heap_end = c + chunk;
    }
    I *b = (I *)heap_at;
    heap_at += bytes;
    b[1] = n;
    return b + 2;
}

R nh_alloc(ARGS) { UNUSED; return ret((I)alloc_words(rcx)); }

R nh_array_rep(ARGS) {
    UNUSED;
    I *a = alloc_words(rcx);
    if (rdx != 0) for (I i = 0; i < rcx; i++) a[i] = rdx;
    return ret((I)a);
}

// a new STRING of n bytes (room for its 0 at the end)
static char *new_text(I n) {
    I *w = alloc_words((n + 8) / 8);
    w[-1] = n;
    return (char *)w;
}

R nh_concat(ARGS) {
    UNUSED;
    I la = len_of(rax), lb = len_of(rcx);
    char *t = new_text(la + lb);
    memcpy(t, (char *)rax, (size_t)la);
    memcpy(t + la, (char *)rcx, (size_t)lb);
    t[la + lb] = 0;
    return ret((I)t);
}

R nh_int_text(ARGS) {
    UNUSED;
    char b[24];
    char *p = decimal(rax, b + 24);
    I n = b + 24 - p;
    char *t = new_text(n);
    memcpy(t, p, (size_t)n);
    t[n] = 0;
    return ret((I)t);
}

R nh_concat_int(ARGS) {
    UNUSED;
    char b[24];
    char *p = decimal(rcx, b + 24);
    I n = b + 24 - p, la = len_of(rax);
    char *t = new_text(la + n);
    memcpy(t, (char *)rax, (size_t)la);
    memcpy(t + la, p, (size_t)n);
    t[la + n] = 0;
    return ret((I)t);
}

R nh_str_equal(ARGS) {
    UNUSED;
    if (rax == rcx) return ret(1);
    I n = len_of(rax);
    return ret(n == len_of(rcx) && memcmp((char *)rax, (char *)rcx, (size_t)n) == 0);
}
