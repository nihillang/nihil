// macos/native/rt.c — the C runtime of programs the compiler writes whole in ARM64 (-native).
//
// A routine RT_<NAME> of src/runtime.asm is nh_<name> here, with its behaviour: the program's
// registers rax, rcx, rdx, rsi, rdi come as the first five arguments and its stack pointer (x28) as
// the sixth; the result goes back in rax and rdx (read_at also gives rcx, through nh_ret_rcx).
// Three adapters in the program's text differ (42_arm.nihil, native_end): alloc keeps rcx, rsi
// and rdi (src/runtime.asm's contract), protect passes the registers it saves, read_at sets rcx.
// The system calls go through nh_linux_syscall (arm64/rt.c): Linux's numbers, flags and errno
// values, as the prelude and these routines speak them.
//
// The collector: conservative mark and sweep, as the x86 runtime's (without its concurrent mark).
//   - a block is [header][length][8-byte words …]; a value points at the words; the header holds
//     the block's state (allocated, marked); its size is its class (or its pages, for a large one);
//   - a word anywhere that points into an allocated block (its middle too: a record's texts are
//     inside the record's block) keeps it alive;
//   - the roots: the globals (_nh_globals), the program's stack (from x28, saved by each adapter in
//     nh_sp, to its top), C's stack from the collection up to nh_start (where the adapters keep x29,
//     x30 and alloc's rcx, rsi, rdi, and where C keeps x19 … x23, callee-saved, or they are still in
//     the registers: setjmp writes them there), and the registers kept by protected calls;
//   - it runs only inside alloc, so only at a call of the runtime: a safepoint.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

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
static char *c_stack_top;
void nh_resume(I *record, I sp) __attribute__((noreturn));        // the program's text

// ---- the program's stack ----
#define STACK_SIZE ((size_t)256 << 20)
static char *stack_top;

I nh_start(int argc, char **argv, I sp) {
    (void)argc; (void)argv;
    char here;
    c_stack_top = &here;
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

// ---- the heap ----
#define PAGE_SHIFT 16
#define PAGE ((I)1 << PAGE_SHIFT)
#define ARENA ((I)64 << 30)
#define NPAGES (ARENA >> PAGE_SHIFT)
#define SMALL_MAX 32768
#define KEEP_PAGES 256                       // empty small pages kept at a collection: 16 MiB
#define H_USED 1
#define H_MARK 2
enum { P_FREE, P_SMALL, P_LARGE, P_CONT };

static const I class_size[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072,
                                4096, 6144, 8192, 12288, 16384, 24576, 32768 };
#define NCLASS 21
static char *arena;
static uint8_t *pkind, *pclass;
static uint32_t *pspan;                      // a large block: its pages (head) or its head (the others)
static I high;                               // pages in use or used before: [0, high)
static I *free_list[NCLASS];                 // free blocks of each class (linked by their first word)
static I *free_pages;                        // single free pages below high
static I n_free_pages;
static I allocated, threshold = (I)16 << 20, live, live_large;

static void heap_init(void) {
    arena = mmap(NULL, (size_t)ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    pkind = calloc((size_t)NPAGES, 1);
    pclass = calloc((size_t)NPAGES, 1);
    pspan = calloc((size_t)NPAGES, 4);
    free_pages = malloc((size_t)NPAGES * sizeof(I));
    if (arena == MAP_FAILED || !pkind || !pclass || !pspan || !free_pages) { write(2, "nihil: out of memory\n", 21); _exit(2); }
}

// the pages p … p + n - 1 back to the system: zero when used again
static void pages_release(I p, I n) {
    if (mmap(arena + p * PAGE, (size_t)(n * PAGE), PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
        write(2, "nihil: out of memory\n", 21);
        _exit(2);
    }
    for (I i = 0; i < n; i++) pkind[p + i] = P_FREE;
}

static __attribute__((noreturn)) void fail(const char *m);

// n contiguous free pages (zeroed) → the first
static I pages_get(I n) {
    if (n == 1) {
        while (n_free_pages > 0) {
            I p = free_pages[--n_free_pages];
            if (pkind[p] == P_FREE) return p;
        }
    } else if (n_free_pages >= n) {          // a run among the free pages
        I run = 0;
        for (I p = 0; p < high; p++) {
            run = pkind[p] == P_FREE ? run + 1 : 0;
            if (run == n) return p - n + 1;
        }
    }
    if (high + n > NPAGES) fail("out of memory");
    I p = high;
    high += n;
    return p;
}

static void collect(void);

// a block for n words → the value (its words, zeroed; its length n). lazy: the caller may write
// only part of a large one (a big array of zeros): its pages count when written, not now
static I *alloc_words_l(I n, int lazy) {
    if (n < 0) fail("negative array size");
    if (n >= ((I)1 << 40)) fail("array too large");
    if (!arena) { heap_init(); if (getenv("NIHIL_GC_TORTURE")) threshold = 65536; }
    I bytes = n * 8 + 16;
    allocated += lazy && bytes > SMALL_MAX ? PAGE : bytes;
    if (allocated > threshold) collect();
    I *b;
    if (bytes <= SMALL_MAX) {
        int c = 0;
        while (class_size[c] < bytes) c++;
        if (!free_list[c]) {                 // a new page of that class
            I p = pages_get(1);
            pkind[p] = P_SMALL;
            pclass[p] = (uint8_t)c;
            I s = class_size[c], k = PAGE / s;
            char *start = arena + p * PAGE;
            for (I i = k - 1; i >= 0; i--) {
                I *f = (I *)(start + i * s);
                f[0] = 0;
                f[2] = (I)free_list[c];
                free_list[c] = f;
            }
        }
        b = free_list[c];
        free_list[c] = (I *)b[2];
        memset(b, 0, (size_t)class_size[c]);
    } else {
        I np = (bytes + PAGE - 1) / PAGE;
        I p = pages_get(np);
        pkind[p] = P_LARGE;
        pspan[p] = (uint32_t)np;
        for (I i = 1; i < np; i++) { pkind[p + i] = P_CONT; pspan[p + i] = (uint32_t)p; }
        b = (I *)(arena + p * PAGE);
    }
    b[0] = H_USED;
    b[1] = n;
    return b + 2;
}

static I *alloc_words(I n) { return alloc_words_l(n, 0); }

// ---- the collection ----
static I **mark_stack;
static I mark_n, mark_cap;

// the block w points into, if it is allocated → its header, else 0
static I *block_of(U w) {
    if (w < (U)arena || w >= (U)(arena + high * PAGE)) return 0;
    I off = (I)(w - (U)arena), p = off >> PAGE_SHIFT;
    I *b;
    switch (pkind[p]) {
    case P_SMALL: {
        I s = class_size[pclass[p]], i = (off & (PAGE - 1)) / s;
        if ((i + 1) * s > PAGE) return 0;    // the page's tail, after its last block
        b = (I *)(arena + p * PAGE + i * s);
        break;
    }
    case P_CONT: p = pspan[p]; /* fallthrough */
    case P_LARGE: b = (I *)(arena + p * PAGE); break;
    default: return 0;
    }
    return (b[0] & H_USED) ? b : 0;
}

static void mark_word(U w) {
    I *b = block_of(w);
    if (!b || (b[0] & H_MARK)) return;
    b[0] |= H_MARK;
    if (mark_n == mark_cap) {
        mark_cap = mark_cap ? mark_cap * 2 : 65536;
        mark_stack = realloc(mark_stack, (size_t)mark_cap * sizeof(I *));
        if (!mark_stack) { write(2, "nihil: out of memory\n", 21); _exit(2); }
    }
    mark_stack[mark_n++] = b;
}

static void mark_range(const char *from, const char *to) {
    for (const char *p = (const char *)(((U)from + 7) & ~(U)7); p + 8 <= to; p += 8) mark_word(*(const U *)p);
}

static I block_bytes(I *b) {
    I p = ((char *)b - arena) >> PAGE_SHIFT;
    return pkind[p] == P_SMALL ? class_size[pclass[p]] : (I)pspan[p] * PAGE;
}

// a block's words; of a large one, only the system pages ever written (mincore): the others are
// zero, and reading them would make them real (a big array mostly unused costs nothing)
static void mark_block(I *b) {
    I bytes = block_bytes(b);
    if (bytes <= SMALL_MAX) { mark_range((const char *)(b + 2), (const char *)b + bytes); return; }
    static I sys_page;
    static unsigned char vec[PAGE * 4];
    if (!sys_page) sys_page = getpagesize();
    char *start = (char *)b, *end = start + bytes;
    for (char *chunk = start; chunk < end; ) {
        I n = (end - chunk) / sys_page;
        if (n > (I)sizeof vec) n = (I)sizeof vec;
        if (mincore(chunk, (size_t)(n * sys_page), (char *)vec) != 0) {
            live_large += n * sys_page;
            mark_range(chunk == start ? (const char *)(b + 2) : chunk, chunk + n * sys_page);
        } else {
            for (I i = 0; i < n; i++) {
                if ((vec[i] & ~MINCORE_ANONYMOUS) == 0) continue;
                live_large += sys_page;
                char *from = chunk + i * sys_page;
                mark_range(from == start ? (const char *)(b + 2) : from, from + sys_page);
            }
        }
        chunk += n * sys_page;
    }
}

static __attribute__((noinline)) void mark_roots(void) {
    jmp_buf regs;
    if (setjmp(regs)) return;                // x19 … x28 into C's stack, scanned with it
    mark_range((const char *)__builtin_frame_address(0), c_stack_top);
    mark_range((const char *)regs, (const char *)regs + sizeof regs);
    mark_range((const char *)nh_globals, (const char *)nh_globals + nh_globals_size);
    if (nh_sp) mark_range((const char *)nh_sp, stack_top);
    mark_range((const char *)prot, (const char *)prot[prot_depth]);
}

static I now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (I)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void collect(void) {
    I t0 = now_us();
    live_large = 0;
    mark_roots();
    while (mark_n > 0) {
        I *b = mark_stack[--mark_n];
        mark_block(b);
    }
    // the sweep: the unmarked blocks are free; a small page with none in use goes back to the system
    for (int c = 0; c < NCLASS; c++) free_list[c] = 0;
    n_free_pages = 0;
    live = 0;
    I kept = 0;
    for (I p = 0; p < high; p++) {
        if (pkind[p] == P_SMALL) {
            int c = pclass[p];
            I s = class_size[c], k = PAGE / s, used = 0;
            char *start = arena + p * PAGE;
            I *first = free_list[c];
            for (I i = k - 1; i >= 0; i--) {
                I *b = (I *)(start + i * s);
                if (b[0] & H_MARK) { b[0] = H_USED; used++; continue; }
                b[0] = 0;
                b[2] = (I)free_list[c];
                free_list[c] = b;
            }
            if (used == 0 && kept >= KEEP_PAGES) {   // empty: back to the system, past the first 16 MiB kept
                free_list[c] = first;
                pages_release(p, 1);
                free_pages[n_free_pages++] = p;
            } else if (used == 0) {
                kept++;                      // kept, its blocks free in its class (written again when used)
            } else {
                live += used * s;
            }
        } else if (pkind[p] == P_LARGE) {
            I *b = (I *)(arena + p * PAGE), np = pspan[p];
            if (b[0] & H_MARK) {
                b[0] = H_USED;
            } else {
                pages_release(p, np);
                for (I i = 0; i < np; i++) free_pages[n_free_pages++] = p + i;
            }
            p += np - 1;
        } else if (pkind[p] == P_FREE) {
            free_pages[n_free_pages++] = p;
        }
    }
    live += live_large;                      // (a large block: its pages written)
    allocated = 0;
    threshold = live > ((I)16 << 20) ? live : ((I)16 << 20);
    static int torture = -1;                 // NIHIL_GC_TORTURE=1: a collection every 64 KiB (tests)
    if (torture < 0) torture = getenv("NIHIL_GC_TORTURE") != NULL;
    if (torture) threshold = 65536;
    if (getenv("NIHIL_GC_STATS")) {
        char m[160];
        int k = snprintf(m, sizeof m, "gc: %lld us, live %lld KiB, heap %lld KiB, next after %lld KiB\n", (long long)(now_us() - t0),
                         (long long)(live >> 10), (long long)((high - n_free_pages) * PAGE >> 10), (long long)(threshold >> 10));
        write(2, m, (size_t)k);
    }
}

// a new STRING of n bytes (room for its 0 at the end, zeroed)
static char *new_text(I n) {
    if (n < 0) n = 0;
    I *w = alloc_words((n + 8) / 8);
    w[-1] = n;
    return (char *)w;
}

// ---- the routines ----
R nh_alloc(ARGS) { UNUSED; return ret((I)alloc_words_l(rcx, 1)); }

R nh_array_rep(ARGS) {
    UNUSED;
    I *a = alloc_words_l(rcx, rdx == 0);
    if (rdx != 0) for (I i = 0; i < rcx; i++) a[i] = rdx;
    return ret((I)a);
}

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

static char in_buf[65536];
static I in_pos, in_size, in_end;

R nh_read_byte(ARGS) {
    UNUSED;
    if (in_pos < in_size) return ret((unsigned char)in_buf[in_pos++]);
    if (in_end) return ret(-1);
    flush_out();
    I n;
    do { n = sys(0, 0, (I)in_buf, sizeof in_buf, 0, 0, 0); } while (n == -4);
    if (n <= 0) { in_end = 1; return ret(-1); }
    in_size = n;
    in_pos = 1;
    return ret((unsigned char)in_buf[0]);
}

R nh_exit(ARGS) { UNUSED; flush_out(); _exit((int)rdi); }

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

R nh_err_text(ARGS) {
    UNUSED;
    char *t = new_text(err_len);
    memcpy(t, err_buf, (size_t)err_len);
    return ret((I)t);
}

// ---- texts and arrays ----
R nh_concat(ARGS) {
    UNUSED;
    I la = len_of(rax), lb = len_of(rcx);
    char *t = new_text(la + lb);
    memcpy(t, (char *)rax, (size_t)la);
    memcpy(t + la, (char *)rcx, (size_t)lb);
    return ret((I)t);
}

R nh_int_text(ARGS) {
    UNUSED;
    char b[24];
    char *p = decimal(rax, b + 24);
    char *t = new_text(b + 24 - p);
    memcpy(t, p, (size_t)(b + 24 - p));
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
    return ret((I)t);
}

R nh_str_equal(ARGS) {
    UNUSED;
    if (rax == rcx) return ret(1);
    I n = len_of(rax);
    return ret(n == len_of(rcx) && memcmp((char *)rax, (char *)rcx, (size_t)n) == 0);
}

R nh_text(ARGS) {
    UNUSED;
    I n = len_of(rax);
    char *t = new_text(n);
    for (I i = 0; i < n; i++) t[i] = (char)((I *)rax)[i];
    return ret((I)t);
}

R nh_sub(ARGS) {
    UNUSED;
    I n = rdx - rcx;
    if (n < 0) n = 0;
    char *t = new_text(n);
    memcpy(t, (char *)rax + rcx, (size_t)n);
    return ret((I)t);
}

R nh_put(ARGS) {
    UNUSED;
    I n = len_of(rdx);
    memmove((char *)rax + rcx, (char *)rdx, (size_t)n);
    return ret(rcx + n);
}

R nh_array_concat(ARGS) {
    UNUSED;
    I na = len_of(rax), nb = len_of(rcx);
    I *a = alloc_words(na + nb);
    memcpy(a, (I *)rax, (size_t)na * 8);
    memcpy(a + na, (I *)rcx, (size_t)nb * 8);
    return ret((I)a);
}

R nh_syscall(ARGS) {
    UNUSED;
    I *a = (I *)rax, n = len_of(rax), x[7] = { 0 };
    for (I i = 0; i < n && i < 7; i++) x[i] = a[i];
    return ret(sys(x[0], x[1], x[2], x[3], x[4], x[5], x[6]));
}

static I json_skip_string(const unsigned char *s, I n, I i) {
    i++;
    while (i < n) {
        unsigned char c = s[i++];
        if (c == '"') return i;
        if (c == 92) i++;
    }
    return -1;
}

R nh_json_end(ARGS) {
    UNUSED;
    const unsigned char *s = (const unsigned char *)rax;
    I n = len_of(rax), i = rcx;
    if (i < 0) return ret(-1);
    while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    if (i >= n) return ret(-1);
    if (s[i] == '"') return ret(json_skip_string(s, n, i));
    if (s[i] == '{' || s[i] == '[') {
        I depth = 0;
        while (i < n) {
            unsigned char c = s[i];
            if (c == '"') { i = json_skip_string(s, n, i); if (i < 0) return ret(-1); continue; }
            i++;
            if (c == '{' || c == '[') depth++;
            else if ((c == '}' || c == ']') && --depth == 0) return ret(i);
        }
        return ret(-1);
    }
    I start = i;
    while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
    return ret(i == start ? -1 : i);
}

R nh_lines(ARGS) {
    UNUSED;
    const char *s = (const char *)rax;
    I n = len_of(rax), w = rcx;
    if (w <= 0) return ret(-1);
    if (n == 0) return ret(0);
    I whole = n / (w + 1), rest = n % (w + 1), lines = whole;
    if (rest) { if (rest != w) return ret(-1); lines++; }
    for (I k = 0; k < whole; k++) if (s[k * (w + 1) + w] != '\n') return ret(-1);
    I breaks = 0;
    for (I i = 0; i < n; i++) breaks += s[i] == '\n';
    return ret(breaks == whole ? lines : -1);
}

static const double pow10_table[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12,
                                      1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

R nh_rec_parse(ARGS) {
    UNUSED;
    I *schema = (I *)rdx, nf = schema[0], at = rcx, words = nf;
    for (I i = 0, a = at; i < nf; i++) {                     // the size: the slots and the texts
        const char *t = (const char *)rax;
        I w = schema[2 + 2 * i];
        if (schema[1 + 2 * i] == 3) {
            I e = a + w;
            while (e > a && t[e - 1] == ' ') e--;
            words += (e - a + 16) / 8;
        }
        a += w;
    }
    I *rec = alloc_words(words);
    const char *t = (const char *)rax;
    rec[-1] = nf;
    char *next = (char *)(rec + nf);
    for (I i = 0; i < nf; i++) {
        I type = schema[1 + 2 * i], a = at, b = at + schema[2 + 2 * i];
        at = b;
        if (type != 3) while (a < b && t[a] == ' ') a++;
        while (b > a && t[b - 1] == ' ') b--;
        I v = 0;
        if (type == 1) {                                      // INT
            if (a < b) {
                int neg = 0;
                if (t[a] == '-') { neg = 1; a++; } else if (t[a] == '+') a++;
                if (a == b) return ret(-1);
                for (; a < b; a++) {
                    unsigned d = (unsigned)(unsigned char)t[a] - '0';
                    if (d > 9 || __builtin_mul_overflow(v, 10, &v) || __builtin_add_overflow(v, (I)d, &v)) return ret(-1);
                }
                if (neg) v = -v;
            }
        } else if (type == 2) {                               // BOOL
            if (b - a == 1) {
                if (t[a] == 'T') v = 1; else if (t[a] != 'F') return ret(-1);
            } else if (b != a) return ret(-1);
        } else if (type == 3) {                               // STRING, in the record's block
            I n = b - a;
            *(I *)next = n;
            memcpy(next + 8, t + a, (size_t)n);
            v = (I)(next + 8);
            next += 8 + ((n + 8) / 8) * 8;
        } else if (a < b) {                                   // FLOAT: Clinger's fast path
            int neg = 0, seen = 0, dot = 0;
            if (t[a] == '-') { neg = 1; a++; } else if (t[a] == '+') a++;
            I m = 0, digits = 0, pw = 0;
            for (; a < b; a++) {
                char c = t[a];
                if (c == '.' && !dot) { dot = 1; continue; }
                unsigned d = (unsigned)(unsigned char)c - '0';
                if (d > 9) return ret(-1);
                seen = 1;
                if (m != 0 || d != 0) { if (digits >= 15) return ret(-1); digits++; m = m * 10 + d; }
                if (dot) pw--;
            }
            if (!seen || pw < -22) return ret(-1);
            double x = m == 0 ? 0.0 : (double)m / pow10_table[-pw];
            memcpy(&v, &x, 8);
            if (neg) v |= (I)((U)1 << 63);
        }
        rec[i] = v;
    }
    return ret((I)rec);
}

// ---- files ----
// the path (a STRING) 0-terminated in buf (cap bytes): 0 if it is too long or has a zero byte
static int c_path(I s, char *buf, I cap, I *len) {
    I n = len_of(s);
    *len = n;
    I k = n < cap - 1 ? n : cap - 1;
    memcpy(buf, (char *)s, (size_t)k);
    buf[k] = 0;
    return n < cap && memchr((char *)s, 0, (size_t)n) == NULL;
}

static __attribute__((noreturn)) void read_failed(const char *p) {
    err_begin(); err_str("could not read '"); err_str(p); err_str("'\n"); err_end();
}

static R read_file(I path, int optional) {
    char p[4096];
    I n, fd = -1;
    if (c_path(path, p, sizeof p, &n)) fd = sys(257, -100, (I)p, 0, 0, 0, 0);
    if (fd < 0) { if (optional) return ret(0); read_failed(p); }
    I size = sys(8, fd, 0, 2, 0, 0, 0), cap = size, got = 0;
    int stream = size <= 0;
    if (stream) cap = 65536; else sys(8, fd, 0, 0, 0, 0, 0);
    char *t = new_text(cap);
    for (;;) {
        if (got == cap) {
            if (!stream) break;
            char *u = new_text(cap * 2);
            memcpy(u, t, (size_t)got);
            t = u;
            cap *= 2;
        }
        I k = sys(0, fd, (I)(t + got), cap - got, 0, 0, 0);
        if (k == -4) continue;
        if (k < 0) { sys(3, fd, 0, 0, 0, 0, 0); if (optional) return ret(0); read_failed(p); }
        if (k == 0) break;
        got += k;
    }
    ((I *)t)[-1] = got;
    t[got] = 0;
    sys(3, fd, 0, 0, 0, 0, 0);
    return ret((I)t);
}

R nh_read_file(ARGS) { UNUSED; return read_file(rax, 0); }
R nh_read_file_opt(ARGS) { UNUSED; return read_file(rax, 1); }

static const char *reason(I e) {
    switch (e) {
    case 2: return "no such file or directory";       case 13: return "permission denied";
    case 17: return "file exists";                     case 20: return "a part of the path is not a directory";
    case 21: return "it is a directory";               case 28: return "no space left on the device";
    case 30: return "read-only file system";           case 36: return "the path is too long";
    case 122: return "disk quota exceeded";            case 255: return "the path has a zero byte";
    }
    return 0;
}

// flag: 0 replace (atomically), 1 append, 2 at an offset, 3 truncate, 4 at an offset without
// fsync, 5 truncate without fsync
#define WF_MAX_PATH 4064
static R write_file(I path, I text, I offset, int flag) {
    char p[4096], tmp[4200];
    I n = len_of(path), e = 0, fd = -1, r = 0;
    I k = n < WF_MAX_PATH ? n : WF_MAX_PATH;
    memcpy(p, (char *)path, (size_t)k);
    p[k] = 0;
    if (memchr((char *)path, 0, (size_t)k)) { e = 255; goto report; }
    if (n > WF_MAX_PATH) { e = 36; goto report; }
    const char *target = p;
    I oflags;
    if (flag == 0) {
        snprintf(tmp, sizeof tmp, "%s.nihil-tmp.%lld", p, (long long)sys(39, 0, 0, 0, 0, 0, 0));
        target = tmp;
        oflags = 0x241;
    } else if (flag == 1) oflags = 0x441;
    else if (flag == 3 || flag == 5) oflags = 1;
    else oflags = 0x41;
    fd = sys(257, -100, (I)target, oflags, 420, 0, 0);
    if (fd < 0) { e = -fd; goto report; }
    if (flag == 3 || flag == 5) {
        r = sys(77, fd, offset, 0, 0, 0, 0);
    } else {
        const char *s = (const char *)text;
        I left = len_of(text);
        while (left > 0) {
            r = flag >= 2 ? sys(18, fd, (I)s, left, offset, 0, 0) : sys(1, fd, (I)s, left, 0, 0, 0);
            if (r == -4) continue;
            if (r < 0) break;
            offset += r; s += r; left -= r;
            r = 0;
        }
    }
    if (r >= 0 && flag < 4) r = sys(74, fd, 0, 0, 0, 0, 0);
    if (r < 0) { sys(3, fd, 0, 0, 0, 0, 0); if (flag == 0) sys(87, (I)tmp, 0, 0, 0, 0, 0); e = -r; goto report; }
    r = sys(3, fd, 0, 0, 0, 0, 0);
    if (r >= 0 && flag == 0) r = sys(82, (I)tmp, (I)p, 0, 0, 0, 0);
    if (r < 0) { if (flag == 0) sys(87, (I)tmp, 0, 0, 0, 0, 0); e = -r; goto report; }
    return ret(0);
report:
    err_begin();
    err_str("could not write '"); err_str(p); err_str("': ");
    const char *why = reason(e);
    if (why) err_str(why); else { err_str("error "); err_num(e); }
    err_str("\n");
    err_end();
}

R nh_write_file(ARGS) { UNUSED; return write_file(rax, rcx, 0, 0); }
R nh_append_file(ARGS) { UNUSED; return write_file(rax, rcx, 0, 1); }
R nh_write_at(ARGS) { UNUSED; return write_file(rax, rcx, rdx, 2); }
R nh_write_at_ns(ARGS) { UNUSED; return write_file(rax, rcx, rdx, 4); }
R nh_truncate(ARGS) { UNUSED; return write_file(rax, rax, rdx, 3); }
R nh_truncate_ns(ARGS) { UNUSED; return write_file(rax, rax, rdx, 5); }

// read_at keeps up to 4 files open, while the path names the same file (st_dev, st_ino)
#define FD_SLOTS 4
static struct { I fd1, dev, ino, len; char path[257]; } fd_cache[FD_SLOTS];
static I fd_next;

R nh_read_at(ARGS) {
    UNUSED;
    char p[4096];
    I n, st[18];
    I offset = rcx, want = rdx, fd = -1;
    nh_ret_rcx = 0;
    if (!c_path(rax, p, sizeof p, &n)) return ret2(0, -1);
    int keep = 0, slot = -1;
    I r = sys(262, -100, (I)p, (I)st, 0, 0, 0);
    for (int i = 0; i < FD_SLOTS; i++)
        if (fd_cache[i].fd1 && fd_cache[i].len == n && memcmp(fd_cache[i].path, p, (size_t)n) == 0) { slot = i; break; }
    if (r < 0) {
        if (slot >= 0) { sys(3, fd_cache[slot].fd1 - 1, 0, 0, 0, 0, 0); fd_cache[slot].fd1 = 0; }
        return ret2(0, -1);
    }
    if (slot >= 0 && fd_cache[slot].dev == st[0] && fd_cache[slot].ino == st[1]) {
        fd = fd_cache[slot].fd1 - 1;
        keep = 1;
    } else {
        if (slot >= 0) { sys(3, fd_cache[slot].fd1 - 1, 0, 0, 0, 0, 0); fd_cache[slot].fd1 = 0; }
        fd = sys(257, -100, (I)p, 0x80000, 0, 0, 0);
        if (fd < 0) return ret2(0, -1);
        if (sys(5, fd, (I)st, 0, 0, 0, 0) < 0) { sys(3, fd, 0, 0, 0, 0, 0); return ret2(0, -1); }
        if (n <= 256) {
            I s = fd_next;
            fd_next = (fd_next + 1) & (FD_SLOTS - 1);
            if (fd_cache[s].fd1) sys(3, fd_cache[s].fd1 - 1, 0, 0, 0, 0, 0);
            fd_cache[s].fd1 = fd + 1;
            fd_cache[s].dev = st[0];
            fd_cache[s].ino = st[1];
            fd_cache[s].len = n;
            memcpy(fd_cache[s].path, p, (size_t)n);
            keep = 1;
        }
    }
    I size = st[6];
    I stamp = (I)((U)st[11] * 1000000000u + (U)st[12] + 0x9E3779B97F4A7C15ull * (U)st[1]);
    I count = 0;
    if (offset >= 0 && offset < size) {
        count = size - offset;
        if (want < 0) count = 0;
        else if (count > want) count = want;
    }
    char *t = new_text(count);
    I got = 0;
    while (got < count) {
        I k = sys(17, fd, (I)(t + got), count - got, offset + got, 0, 0);
        if (k == -4) continue;
        if (k <= 0) break;
        got += k;
    }
    ((I *)t)[-1] = got;
    t[got] = 0;
    if (!keep) sys(3, fd, 0, 0, 0, 0, 0);
    nh_ret_rcx = stamp;
    return ret2((I)t, size);
}

R nh_sleep(ARGS) {
    UNUSED;
    if (rax <= 0) return ret(0);
    flush_out();
    I ts[2] = { rax / 1000, (rax % 1000) * 1000000 };
    while (sys(35, (I)ts, (I)ts, 0, 0, 0, 0) == -4) {}
    return ret(0);
}

// ---- network ----
static I sockaddr_of(unsigned char *a, I addr, I port) {
    memset(a, 0, 16);
    a[0] = 2;                                                 // AF_INET (Linux's layout)
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    a[4] = (unsigned char)(addr >> 24); a[5] = (unsigned char)(addr >> 16);
    a[6] = (unsigned char)(addr >> 8); a[7] = (unsigned char)addr;
    return (I)a;
}

R nh_net_listen(ARGS) {
    UNUSED;
    I fd = sys(41, 2, 1 | 0x80000, 0, 0, 0, 0);
    if (fd < 0) return ret(fd);
    int one = 1;
    sys(54, fd, 1, 2, (I)&one, 4, 0);
    unsigned char a[16];
    I r = sys(49, fd, sockaddr_of(a, rcx ? 0 : 0x7F000001, rax), 16, 0, 0, 0);
    if (r >= 0) r = sys(50, fd, 128, 0, 0, 0, 0);
    if (r < 0) { sys(3, fd, 0, 0, 0, 0, 0); return ret(r); }
    return ret(fd);
}

R nh_net_connect(ARGS) {
    UNUSED;
    I fd = sys(41, 2, 1 | 0x80000, 0, 0, 0, 0);
    if (fd < 0) return ret(fd);
    unsigned char a[16];
    I r;
    do { r = sys(42, fd, sockaddr_of(a, rax, rcx), 16, 0, 0, 0); } while (r == -4);
    if (r < 0) { sys(3, fd, 0, 0, 0, 0, 0); return ret(r); }
    return ret(fd);
}

R nh_net_accept(ARGS) {
    UNUSED;
    flush_out();
    I c;
    do { c = sys(288, rax, 0, 0, 0x80000, 0, 0); } while (c == -4);
    if (c >= 0) { I tv[2] = { 15, 0 }; sys(54, c, 1, 20, (I)tv, 16, 0); }
    return ret(c);
}

R nh_fd_read(ARGS) {
    UNUSED;
    I most = rcx > 0 ? rcx : 65536;
    char *t = new_text(most);
    I k;
    do { k = sys(0, rax, (I)t, most, 0, 0, 0); } while (k == -4);
    if (k < 0) k = 0;
    ((I *)t)[-1] = k;
    t[k] = 0;
    return ret((I)t);
}

R nh_fd_send(ARGS) {
    UNUSED;
    const char *s = (const char *)rcx;
    I left = len_of(rcx);
    while (left > 0) {
        I k = sys(44, rax, (I)s, left, 0x4000, 0, 0);
        if (k == -4) continue;
        if (k < 0) return ret(k);
        s += k; left -= k;
    }
    return ret(0);
}

R nh_fd_close(ARGS) { UNUSED; sys(3, rax, 0, 0, 0, 0, 0); return ret(0); }
