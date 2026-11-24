// =============================================================================
// arm64/rt.c — the Nihil runtime for macOS on ARM64 (Apple silicon).
//
// The same contract as src/runtime.asm (SPEC §12), reached through libSystem. Generated
// code calls these functions with the standard ARM64 convention (arguments in x0–x2, x19–x28
// kept), so they are plain C; the algorithms are the x86 ones.
//
// Values: an array is [header][length][8-byte elements] and its value points to the first
// element; a STRING is the same block with its length in bytes (and a 0 after the last one).
//
// Garbage collector: conservative mark and sweep, as on x86 (runtime.asm, "heap"). Every
// 8-byte word on the stack, in the globals and in a live block that points into a used block
// keeps it. The heap is one reservation in 64 KiB pages with a descriptor each; small blocks
// (up to 32 KiB) in 21 size classes, large ones in whole pages. Differences from Linux:
//   - pages given back are remapped (mmap MAP_FIXED): macOS's MADV_DONTNEED does not zero
//     them, and new memory must be zero;
//   - mincore works on the system's pages (16 KiB on Apple silicon), not 4 KiB;
//   - the top of the stack comes from pthread_get_stackaddr_np, the globals from the two
//     symbols the compiler puts around them (_nh_globals_start, _nh_globals_end).
// =============================================================================
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/mman.h>

typedef int64_t i64;
typedef uint64_t u64;

// ---- output buffer (64 KiB: flushed when full, at the end and before reading) --------------
static unsigned char out_buf[65536];
static i64 out_size;

// ---- protected calls (SPEC §25) ---------------------------------------------------------
// A runtime error starts with err_begin and ends with err_end. With no protection active they
// write "nihil: …" and end the program with 2; inside a protected call (?f()) the text goes to
// ?! and err_end resumes after the call (nh_resume, below), with the stacks and the registers
// the translated program had there (macos/translate.py fills the frame).
typedef struct { i64 emu_sp, fp, target, r19, r20, r21, r22, r23, sp; } prot_frame;
static prot_frame prot[64];
static i64 prot_depth;
static int err_capture;
static char err_buf[4000];
static i64 err_len;
_Noreturn void nh_resume(prot_frame *f);
__asm__(
    ".text\n.p2align 2\n.globl _nh_resume\n_nh_resume:\n"
    "    ldr x16, [x0, #64]\n    mov sp, x16\n"
    "    ldr x28, [x0]\n    ldr x29, [x0, #8]\n    ldr x17, [x0, #16]\n"
    "    ldp x19, x20, [x0, #24]\n    ldp x21, x22, [x0, #40]\n    ldr x23, [x0, #56]\n"
    "    mov x9, #0\n    mov x11, #0\n    br x17\n");

static void write_all(int fd, const void *p, i64 n) {
    const char *s = p;
    if (fd == 2 && err_capture) {                     // into ?!'s text
        if (n > (i64)sizeof err_buf - err_len) n = (i64)sizeof err_buf - err_len;
        memcpy(err_buf + err_len, s, (size_t)n);
        err_len += n;
        return;
    }
    while (n > 0) {
        ssize_t w = write(fd, s, (size_t)n);
        if (w <= 0) return;
        s += w; n -= w;
    }
}

void nh_flush(void) {
    write_all(1, out_buf, out_size);
    out_size = 0;
}

void nh_put_bytes(const void *p, i64 n) {
    const unsigned char *s = p;
    while (n > 0) {
        if (out_size == (i64)sizeof out_buf) nh_flush();
        i64 room = (i64)sizeof out_buf - out_size;
        i64 k = n < room ? n : room;
        memcpy(out_buf + out_size, s, (size_t)k);
        out_size += k; s += k; n -= k;
    }
}

void nh_put_text(const char *t) { nh_put_bytes(t, ((const i64 *)t)[-1]); }

static int decimal(i64 v, char *end) {          // digits written backwards before end
    char *p = end;
    u64 u = v < 0 ? (u64)0 - (u64)v : (u64)v;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--p = '-';
    return (int)(end - p);
}

void nh_put_int(i64 v) {
    char b[24];
    int n = decimal(v, b + sizeof b);
    nh_put_bytes(b + sizeof b - n, n);
}

void nh_exit(i64 code) {
    nh_flush();
    _exit((int)code);
}

static void err_begin(void) {
    if (prot_depth > 0) { err_len = 0; err_capture = 1; return; }
    nh_flush();
    write_all(2, "nihil: ", 7);
}

static _Noreturn void err_end(void) {
    if (!err_capture) _exit(2);
    err_capture = 0;
    if (err_len > 0 && err_buf[err_len - 1] == '\n') err_len--;
    prot_depth--;
    nh_resume(&prot[prot_depth]);
}

// the translated program's RT_PROTECT: the next frame (filled by the glue)
prot_frame *nh_protect_slot(void);
void nh_unprotect(void) { prot_depth--; err_len = 0; }

// a runtime error: "nihil: <reason>" on the standard error, exit code 2 (SPEC §8.3)
void nh_error(const char *msg) {
    if (strcmp(msg, "out of memory") == 0) prot_depth = 0;   // never caught: the heap may be inconsistent
    err_begin();
    write_all(2, msg, (i64)strlen(msg));
    write_all(2, "\n", 1);
    err_end();
}

prot_frame *nh_protect_slot(void) {
    if (prot_depth >= 64) nh_error("more than 64 protected calls inside each other");
    return &prot[prot_depth++];
}

// a runtime error with a Nihil text as its message (the prelude's errors)
void nh_fail(const char *t) {
    err_begin();
    write_all(2, t, ((const i64 *)t)[-1]);
    write_all(2, "\n", 1);
    err_end();
}

// an index out of bounds: the array (its length at -8) and the index
void nh_error_index(const i64 *array, i64 index) {
    char b[96], *p = b;
    memcpy(p, "index ", 6); p += 6;
    char t[24]; int n = decimal(index, t + 24); memcpy(p, t + 24 - n, (size_t)n); p += n;
    memcpy(p, " out of bounds (size ", 21); p += 21;
    n = decimal(array[-1], t + 24); memcpy(p, t + 24 - n, (size_t)n); p += n;
    *p++ = ')'; *p = 0;
    nh_error(b);
}

// ---- heap -------------------------------------------------------------------------------
#define PAGE_SHIFT 16
#define PAGE ((i64)1 << PAGE_SHIFT)
#define SMALL_MAX 32768
#ifdef GC_STRESS                                // make gcstress: a collection every 256 KiB
#define GC_MIN ((i64)256 << 10)
#else
#define GC_MIN ((i64)16 << 20)                  // collect after at least 16 MiB of new blocks
#endif
#define GC_RESERVE ((i64)64 << 30)
#define PD_SMALL 1
#define PD_LARGE 2
#define PD_CONT 3
#define PD_DIRTY 4
#define GC_KEEP (GC_MIN >> PAGE_SHIFT)           // empty small pages kept for reuse

static const i64 class_size[21] = {32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072,
                                   4096, 6144, 8192, 12288, 16384, 24576, 32768};

static int gc_ready;
static char *heap_base, *heap_limit, *bump;
static u64 *desc;                                // one per page
static i64 (*runs)[2];                           // runs of free pages: (first page, pages)
static i64 nruns;
static i64 **mstack, **mstack_end, **mtop;        // the blocks still to scan
static i64 *dirty, ndirty;                       // empty small pages kept (page numbers)
static unsigned char *core;                      // mincore's vector
static i64 sys_page;
static char *stack_top;
static i64 allocated, threshold, live, collections;
static i64 *free_list[21];

extern i64 nh_globals_start[], nh_globals_end[];  // put around the globals by the compiler

// The x86 stack of a translated program (macos/translate.py): its own area, emulated by x28. The
// glue of each runtime call writes the current x28 here, and the collector scans from it to the
// top, so the values on that stack (and the registers the glue saved there) keep their blocks.
char *nh_emu_sp, *nh_emu_top;

void *nh_emu_stack(void) {
    i64 size = (i64)256 << 20;                       // reserved, not used: pages come when touched
    char *p = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { write(2, "nihil: out of memory\n", 21); _exit(2); }
    nh_emu_top = p + size;
    nh_emu_sp = nh_emu_top;
    return nh_emu_top;
}

i64 nh_gc_collections(void) { return collections; }
i64 nh_gc_live(void) { return live; }

static void *map_zero(i64 bytes) {
    void *p = mmap(0, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}

static void gc_init(void) {
    i64 r = GC_RESERVE;
    char *p;
    while (!(p = map_zero(r + PAGE))) {             // refused: a quarter of that
        r >>= 2;
        if (r < ((i64)64 << 20)) nh_error("out of memory");
    }
    heap_base = (char *)(((uintptr_t)p + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    bump = heap_base;
    heap_limit = heap_base + r;
    i64 pages = r >> PAGE_SHIFT;
    desc = map_zero(pages * 8);
    runs = map_zero(pages * 16);
    mstack = map_zero(r >> 2);                    // a block has at least 32 bytes: 8 per block
    dirty = map_zero(pages * 8);
    sys_page = getpagesize();
    core = map_zero(r / sys_page + 1);
    if (!desc || !runs || !mstack || !dirty || !core) nh_error("out of memory");
    mstack_end = mstack + (r >> 5);
    stack_top = pthread_get_stackaddr_np(pthread_self());
    threshold = GC_MIN;
    gc_ready = 1;
}

// pages → the first one (zeroed memory), or 0 when there is no room: first fit among the runs
// of free pages, else new pages
static char *get_pages(i64 n) {
    for (i64 i = 0; i < nruns; i++) {
        if (runs[i][1] < n) continue;
        i64 first = runs[i][0];
        if (runs[i][1] == n) { runs[i][0] = runs[nruns - 1][0]; runs[i][1] = runs[nruns - 1][1]; nruns--; }
        else { runs[i][0] += n; runs[i][1] -= n; }
        return heap_base + (first << PAGE_SHIFT);
    }
    if (bump + (n << PAGE_SHIFT) > heap_limit) return 0;
    char *p = bump;
    bump += n << PAGE_SHIFT;
    return p;
}

static void gc_collect(void);

// a new page of free blocks of class c in its list (collecting first if no page is left)
static void small_page(int c) {
    char *page;
    if (ndirty > 0) page = heap_base + (dirty[--ndirty] << PAGE_SHIFT);
    else if (!(page = get_pages(1))) {
        gc_collect();
        if (free_list[c]) return;                 // the collection freed blocks of this class
        if (!(page = get_pages(1))) nh_error("out of memory");
    }
    desc[(page - heap_base) >> PAGE_SHIFT] = ((u64)c << 8) | PD_SMALL;
    i64 size = class_size[c];
    i64 *next = 0;
    for (i64 off = (PAGE / size) * size - size; off >= 0; off -= size) {   // the list, from the end
        i64 *b = (i64 *)(page + off);
        b[0] = 0;                                  // free (a kept page still has old bytes)
        b[1] = (i64)next;
        next = b;
    }
    free_list[c] = next;
}

// n elements → a new block (its value points to the first element), all zero
i64 *nh_alloc(i64 n) {
    if (n < 0) nh_error("negative array size");
    if (n >= ((i64)1 << 40)) nh_error("array too large");
    if (!gc_ready) gc_init();
    i64 bytes = n * 8 + 16;                         // with the header and the length
    allocated += bytes;
    if (allocated > threshold) gc_collect();
    i64 *b;
    if (bytes <= SMALL_MAX) {
        int c = 0;                                  // the size class: 32, 48, 64, 96, 128, 192 …
        while (class_size[c] < bytes) c++;
        if (!free_list[c]) small_page(c);
        b = free_list[c];
        free_list[c] = (i64 *)b[1];
        memset(b + 2, 0, (size_t)n * 8);           // a reused block has old bytes
    } else {
        i64 pages = (bytes + PAGE - 1) >> PAGE_SHIFT;
        char *p = get_pages(pages);
        if (!p) { gc_collect(); if (!(p = get_pages(pages))) nh_error("out of memory"); }
        i64 first = (p - heap_base) >> PAGE_SHIFT;  // zero already: only the descriptors
        desc[first] = ((u64)pages << 8) | PD_LARGE;
        for (i64 k = 1; k < pages; k++) desc[first + k] = ((u64)k << 8) | PD_CONT;
        b = (i64 *)p;
    }
    b[0] = (n << 8) | 1;                            // used
    b[1] = n;                                       // the length the program sees
    return b + 2;
}

// [value ; n]
i64 *nh_array_rep(i64 n, i64 value) {
    i64 *a = nh_alloc(n);
    if (value) for (i64 i = 0; i < n; i++) a[i] = value;
    return a;
}

// ---- collection -------------------------------------------------------------------------
// every word in [from, to) that points into a used block marks it and puts it on the mark stack
static void scan(const i64 *from, const i64 *to) {
    i64 used = bump - heap_base;
    for (; from < to; from++) {
        u64 off = (u64)((char *)*from - heap_base);
        if (off >= (u64)used) continue;             // not in the heap (most values)
        i64 page = (i64)(off >> PAGE_SHIFT);
        u64 d = desc[page];
        i64 *b;
        switch (d & 255) {
        case PD_SMALL: {
            i64 size = class_size[d >> 8];
            i64 in = (i64)(off & (PAGE - 1));
            i64 at = in / size * size;
            if (at + size > PAGE) continue;          // the unused end of the page
            b = (i64 *)(heap_base + (page << PAGE_SHIFT) + at);
            break;
        }
        case PD_CONT: page -= (i64)(d >> 8);       // the first page of the block
        /* fall through */
        case PD_LARGE: b = (i64 *)(heap_base + (page << PAGE_SHIFT)); break;
        default: continue;
        }
        if (!(b[0] & 1) || (b[0] & 2)) continue;    // free, or already marked
        b[0] |= 2;
        if (mtop >= mstack_end) nh_error("out of memory");
        *mtop++ = b;
    }
}

// a large block, skipping the system pages that have no memory (never written: zero). A page
// macOS compressed or sent to swap is not "in core" (MINCORE_INCORE off) but it has data
// (MINCORE_PAGED_OUT and others on): it is scanned. Only a page mincore says nothing about was
// never written. (Skipping every page not in core lost the blocks a compressed page pointed to,
// under memory pressure, and the program then read freed memory.)
static void scan_resident(const i64 *from, const i64 *to) {
    char *first = (char *)((uintptr_t)from & ~(uintptr_t)(sys_page - 1));
    i64 len = ((char *)to - first + sys_page - 1) & ~(sys_page - 1);
    if (mincore(first, (size_t)len, (char *)core) != 0) { scan(from, to); return; }
    for (i64 k = 0; first + k * sys_page < (char *)to; k++) {
        if ((core[k] & 0x7F) == 0) continue;        // never written: no flag but MINCORE_ANONYMOUS (0x80)
        const i64 *a = (const i64 *)(first + k * sys_page), *z = (const i64 *)(first + (k + 1) * sys_page);
        if (a < from) a = from;
        if (z > to) z = to;
        scan(a, z);
    }
}

static void give_back(i64 first, i64 pages) {        // fresh zero pages in their place
    void *p = mmap(heap_base + (first << PAGE_SHIFT), (size_t)(pages << PAGE_SHIFT), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) nh_error("out of memory");
}

// frees what was not marked, clears the marks, rebuilds the free lists and the runs of free
// pages, and gives free pages back
static void sweep(void) {
    memset(free_list, 0, sizeof free_list);
    live = 0;
    ndirty = 0;
    i64 pages = (bump - heap_base) >> PAGE_SHIFT;
    for (i64 p = 0; p < pages;) {
        u64 d = desc[p];
        switch (d & 255) {
        case PD_LARGE: {
            i64 n = (i64)(d >> 8);
            i64 *b = (i64 *)(heap_base + (p << PAGE_SHIFT));
            if (b[0] & 2) { b[0] &= ~(i64)2; live += n << PAGE_SHIFT; }
            else for (i64 k = 0; k < n; k++) desc[p + k] = 0;
            p += n;
            break;
        }
        case PD_SMALL: {
            int c = (int)(d >> 8);
            i64 size = class_size[c];
            char *page = heap_base + (p << PAGE_SHIFT), *last = page + PAGE - size;
            int in_use = 0;
            for (char *q = page; q <= last; q += size) {
                i64 *b = (i64 *)q;
                if (b[0] & 2) { b[0] &= ~(i64)2; in_use++; } else b[0] = 0;
            }
            if (!in_use) goto empty;
            live += PAGE;
            for (char *q = page; q <= last; q += size) {
                i64 *b = (i64 *)q;
                if (b[0] == 0) { b[1] = (i64)free_list[c]; free_list[c] = b; }
            }
            p++;
            break;
        }
        case PD_DIRTY:
        empty:                                      // an empty small page: kept, up to GC_KEEP
            if (ndirty < GC_KEEP) { dirty[ndirty++] = p; desc[p] = PD_DIRTY; }
            else desc[p] = 0;
            p++;
            break;
        default:
            p++;
        }
    }
    nruns = 0;                                      // the free pages, in runs
    for (i64 p = 0; p < pages;) {
        if (desc[p] != 0) { p++; continue; }
        i64 first = p;
        while (p < pages && desc[p] == 0) p++;
        give_back(first, p - first);
        if (p == pages) bump = heap_base + (first << PAGE_SHIFT);   // at the end: never used again
        else { runs[nruns][0] = first; runs[nruns][1] = p - first; nruns++; }
    }
}

__attribute__((noinline)) static void collect_from(char *sp) {
    mtop = mstack;
    scan((const i64 *)(((uintptr_t)sp) & ~(uintptr_t)7), (const i64 *)stack_top);
    scan(nh_globals_start, nh_globals_end);
    if (nh_emu_top) scan((const i64 *)(((uintptr_t)nh_emu_sp) & ~(uintptr_t)7), (const i64 *)nh_emu_top);
    while (mtop > mstack) {                         // the blocks reached, until none is left
        i64 *b = *--mtop;
        i64 n = b[0] >> 8;
        if (n > SMALL_MAX / 8) scan_resident(b + 2, b + 2 + n);
        else scan(b + 2, b + 2 + n);
    }
    sweep();
    collections++;
    allocated = 0;
#ifdef GC_STRESS
    threshold = GC_MIN;
#else
    threshold = live > GC_MIN ? live : GC_MIN;      // the next one after as many new bytes as are in use
#endif
}

static void gc_collect(void) {
    jmp_buf regs;                                   // x19–x28 (the optimizer's registers) on the stack
    (void)setjmp(regs);
    collect_from((char *)&regs);
}

// ---- arithmetic -------------------------------------------------------------------------
// division truncated toward zero; MIN / -1 gives MIN and MIN % -1 gives 0 (no trap)
i64 nh_div(i64 a, i64 b) {
    if (b == 0) nh_error("division by zero");
    if (b == -1) return (i64)((u64)0 - (u64)a);
    return a / b;
}

i64 nh_mod(i64 a, i64 b) {
    if (b == 0) nh_error("division by zero");
    if (b == -1) return 0;
    return a % b;
}

// ---- more output ------------------------------------------------------------------------
void nh_put_bool(i64 b) { if (b) nh_put_bytes("true", 4); else nh_put_bytes("false", 5); }
void nh_put_int_opt(i64 v, i64 present) { if (present) nh_put_int(v); else nh_put_bytes("_", 1); }
void nh_put_bool_opt(i64 v, i64 present) { if (present) nh_put_bool(v); else nh_put_bytes("_", 1); }
void nh_put_text_opt(const char *t, i64 present) { if (present) nh_put_text(t); else nh_put_bytes("_", 1); }

void nh_write_byte(i64 b) {
    if (out_size == (i64)sizeof out_buf) nh_flush();
    out_buf[out_size++] = (unsigned char)b;
}

// !>>: unbuffered, to the standard error, after flushing the normal output
void nh_err_int(i64 v) { char b[24]; int n = decimal(v, b + 24); nh_flush(); write_all(2, b + 24 - n, n); }
void nh_err_bool(i64 v) { nh_flush(); if (v) write_all(2, "true", 4); else write_all(2, "false", 5); }
void nh_err_text(const char *t) { nh_flush(); write_all(2, t, ((const i64 *)t)[-1]); }
void nh_err_int_opt(i64 v, i64 present) { if (present) nh_err_int(v); else { nh_flush(); write_all(2, "_", 1); } }
void nh_err_bool_opt(i64 v, i64 present) { if (present) nh_err_bool(v); else { nh_flush(); write_all(2, "_", 1); } }
void nh_err_text_opt(const char *t, i64 present) { if (present) nh_err_text(t); else { nh_flush(); write_all(2, "_", 1); } }

// ---- input (<<, <<<): flushes what was written before waiting ----------------------------
static unsigned char in_buf[65536];
static i64 in_pos, in_size, in_end;

i64 nh_read_byte(void) {
    if (in_pos >= in_size) {
        if (in_end) return -1;
        nh_flush();
        ssize_t r = read(0, in_buf, sizeof in_buf);
        if (r <= 0) { in_end = 1; return -1; }
        in_size = r;
        in_pos = 0;
    }
    return in_buf[in_pos++];
}

// ---- texts ------------------------------------------------------------------------------
// a new STRING of n bytes, zeroed (the 0 after the last byte comes with it)
static char *new_text(i64 n) {
    i64 *a = nh_alloc((n + 8) >> 3);
    a[-1] = n;
    return (char *)a;
}

// ?! → a new text: the message of the last caught runtime error ("" after a call that worked)
char *nh_err_message(void) {
    char *r = new_text(err_len);
    memcpy(r, err_buf, (size_t)err_len);
    ((i64 *)r)[-1] = err_len;
    r[err_len] = 0;
    return r;
}

i64 nh_text_equal(const char *a, const char *b) {
    i64 n = ((const i64 *)a)[-1];
    return n == ((const i64 *)b)[-1] && memcmp(a, b, (size_t)n) == 0;
}

char *nh_concat(const char *a, const char *b) {
    i64 na = ((const i64 *)a)[-1], nb = ((const i64 *)b)[-1];
    char *r = new_text(na + nb);
    memcpy(r, a, (size_t)na);
    memcpy(r + na, b, (size_t)nb);
    return r;
}

// "…" + $n in one block
char *nh_concat_int(const char *a, i64 v) {
    char b[24];
    int n = decimal(v, b + 24);
    i64 na = ((const i64 *)a)[-1];
    char *r = new_text(na + n);
    memcpy(r, a, (size_t)na);
    memcpy(r + na, b + 24 - n, (size_t)n);
    return r;
}

// $n
char *nh_int_text(i64 v) {
    char b[24];
    int n = decimal(v, b + 24);
    char *r = new_text(n);
    memcpy(r, b + 24 - n, (size_t)n);
    return r;
}

// the STRING with the bytes of an ARRAY of INT ($array)
char *nh_text(const i64 *a) {
    i64 n = a[-1];
    char *r = new_text(n);
    for (i64 i = 0; i < n; i++) r[i] = (char)a[i];
    return r;
}

// a new STRING with the bytes a … b - 1 (the prelude has checked the bounds)
char *nh_sub(const char *s, i64 a, i64 b) {
    i64 n = b - a;
    if (n < 0) n = 0;
    char *r = new_text(n);
    memcpy(r, s + a, (size_t)n);
    return r;
}

// the bytes of t copied into r (a new STRING being built) from position at → at + #t
i64 nh_put(char *r, i64 at, const char *t) {
    i64 n = ((const i64 *)t)[-1];
    memcpy(r + at, t, (size_t)n);
    return at + n;
}

// how many lines the text has when every one has exactly that width (the last may lack its
// "\n"), or -1 when one does not
i64 nh_lines(const char *s, i64 width) {
    i64 n = ((const i64 *)s)[-1], lines = 0;
    if (width <= 0) return -1;
    for (i64 at = 0; at < n;) {
        i64 end = at + width;
        if (end > n || memchr(s + at, '\n', (size_t)width)) return -1;
        lines++;
        if (end == n) break;
        if (s[end] != '\n') return -1;
        at = end + 1;
    }
    return lines;
}

// ---- arrays -----------------------------------------------------------------------------
i64 *nh_array_concat(const i64 *a, const i64 *b) {
    i64 na = a[-1], nb = b[-1];
    i64 *r = nh_alloc(na + nb);
    memcpy(r, a, (size_t)na * 8);
    memcpy(r + na, b, (size_t)nb * 8);
    return r;
}

// ---- time -------------------------------------------------------------------------------
#include <time.h>
#include <errno.h>
void nh_sleep(i64 ms) {                            // ... ms; (≤ 0: returns at once)
    if (ms <= 0) return;
    nh_flush();
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

// ---- files ------------------------------------------------------------------------------
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>                               // rename
#define MAX_PATH 4080                            // leaves room for ".nihil-tmp" and the 0

// a Nihil text as a 0-terminated path (cut, only for messages, when it is too long). 0 when it
// does not fit or has a zero byte inside: the kernel would stop there and open another file
// than the one the program checked ("notes.txt\0.nihil" would be "notes.txt")
static int c_path(const char *t, char *out, i64 cap) {
    i64 n = ((const i64 *)t)[-1];
    int ok = n <= cap && !memchr(t, 0, (size_t)n);
    if (n > cap) n = cap;
    memcpy(out, t, (size_t)n);
    out[n] = 0;
    return ok;
}

static void put_err(const char *s) { write_all(2, s, (i64)strlen(s)); }

static _Noreturn void fail_read(const char *path) {
    err_begin();
    put_err("could not read '"); put_err(path); put_err("'\n");
    err_end();
}

static const char *reason(int e) {
    switch (e) {
    case ENOENT: return "no such file or directory";
    case EACCES: return "permission denied";
    case EEXIST: return "file exists";
    case ENOTDIR: return "a part of the path is not a directory";
    case EISDIR: return "it is a directory";
    case ENOSPC: return "no space left on the device";
    case EROFS: return "read-only file system";
    case ENAMETOOLONG: return "the path is too long";
    case EDQUOT: return "disk quota exceeded";
    case EINVAL: return "the path has a zero byte";
    }
    return 0;
}

static _Noreturn void fail_write(const char *path, int e) {
    err_begin();
    put_err("could not write '"); put_err(path); put_err("': ");
    const char *r = reason(e);
    if (r) put_err(r);
    else { char b[24]; int n = decimal(e, b + 24); put_err("error "); write_all(2, b + 24 - n, n); }
    put_err("\n");
    err_end();
}

// the data on the disk: macOS's fsync stops at the drive's cache, F_FULLFSYNC goes through it
static int full_sync(int fd) {
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
    return fsync(fd);
}

// Linux's /proc files the programs read, made from macOS's equivalents: the environment
// ("NAME=value" each followed by a 0), the seconds since boot ("12345.67 0.00\n") and a random
// UUID (36 characters and a line break). 0 for any other path.
extern char **environ;
static char *virtual_file(const char *path) {
    if (strcmp(path, "/proc/self/environ") == 0) {
        i64 n = 0;
        for (char **e = environ; *e; e++) n += (i64)strlen(*e) + 1;
        char *r = new_text(n);
        i64 at = 0;
        for (char **e = environ; *e; e++) { i64 k = (i64)strlen(*e) + 1; memcpy(r + at, *e, (size_t)k); at += k; }
        ((i64 *)r)[-1] = n; r[n] = 0;
        return r;
    }
    if (strcmp(path, "/proc/uptime") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);         // macOS: since boot, sleep included
        char b[64];
        int n = snprintf(b, sizeof b, "%lld.%02ld 0.00\n", (long long)ts.tv_sec, ts.tv_nsec / 10000000);
        char *r = new_text(n);
        memcpy(r, b, (size_t)n); ((i64 *)r)[-1] = n; r[n] = 0;
        return r;
    }
    if (strcmp(path, "/proc/sys/kernel/random/uuid") == 0) {
        unsigned char u[16];
        arc4random_buf(u, sizeof u);
        u[6] = (u[6] & 0x0F) | 0x40; u[8] = (u[8] & 0x3F) | 0x80;
        char b[40];
        int n = snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                         u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        char *r = new_text(n);
        memcpy(r, b, (size_t)n); ((i64 *)r)[-1] = n; r[n] = 0;
        return r;
    }
    return 0;
}

// @ path: the contents (a 0 after them); optional = 1 gives 0 instead of a runtime error
static char *read_file(const char *t, int optional) {
    char path[4096];
    if (!c_path(t, path, 4095)) { if (optional) return 0; fail_read(path); }
    if (strncmp(path, "/proc/", 6) == 0) {
        char *v = virtual_file(path);
        if (v) return v;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { if (optional) return 0; fail_read(path); }
    off_t size = lseek(fd, 0, SEEK_END);
    char *r;
    if (size > 0) {
        lseek(fd, 0, SEEK_SET);
        r = new_text(size);
        i64 got = 0;
        while (got < size) {
            ssize_t k = read(fd, r + got, (size_t)(size - got));
            if (k < 0 && errno == EINTR) continue;
            if (k < 0) { close(fd); if (optional) return 0; fail_read(path); }
            if (k == 0) break;                       // it shrank while being read
            got += k;
        }
        ((i64 *)r)[-1] = got;
        r[got] = 0;
    } else {                                         // no size (a pipe, /dev/stdin): read to the end
        i64 cap = 65536, got = 0;
        char *volatile buf = new_text(cap);          // volatile: a root on the stack across nh_alloc
        for (;;) {
            if (got == cap) {
                char *bigger = new_text(cap * 2);
                memcpy(bigger, buf, (size_t)got);
                buf = bigger;
                cap *= 2;
            }
            ssize_t k = read(fd, buf + got, (size_t)(cap - got));
            if (k < 0 && errno == EINTR) continue;
            if (k < 0) { close(fd); if (optional) return 0; fail_read(path); }
            if (k == 0) break;
            got += k;
        }
        r = buf;
        ((i64 *)r)[-1] = got;                         // the block keeps its size
        r[got] = 0;
    }
    close(fd);
    return r;
}

char *nh_read_file(const char *path) { return read_file(path, 0); }
char *nh_read_file_opt(const char *path) { return read_file(path, 1); }

// how a file is written: 0 replace (atomically), 1 append, 2 at an offset, 3 truncate,
// 4 at an offset without the sync, 5 truncate without the sync
static void write_file(const char *t, const char *text, int how, i64 offset) {
    char path[4096], temp[4096 + 16];
    if (!c_path(t, path, MAX_PATH)) fail_write(path, ((const i64 *)t)[-1] > MAX_PATH ? ENAMETOOLONG : EINVAL);
    const char *target = path;
    int flags;
    switch (how) {
    case 0:
        strcpy(temp, path); strcat(temp, ".nihil-tmp");
        target = temp; flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 1: flags = O_WRONLY | O_CREAT | O_APPEND; break;
    case 3: case 5: flags = O_WRONLY; break;          // it must exist
    default: flags = O_WRONLY | O_CREAT;              // in place
    }
    int fd = open(target, flags, 0644);
    if (fd < 0) fail_write(path, errno);
    int e = 0;
    if (how == 3 || how == 5) {
        if (ftruncate(fd, offset) != 0) e = errno;
    } else {
        const char *p = text;
        i64 left = ((const i64 *)text)[-1];
        while (left > 0) {
            ssize_t w = how >= 2 ? pwrite(fd, p, (size_t)left, offset) : write(fd, p, (size_t)left);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) { e = errno; break; }
            offset += w; p += w; left -= w;
        }
    }
    if (!e && how < 4 && full_sync(fd) != 0) e = errno;
    if (close(fd) != 0 && !e) e = errno;
    if (!e && how == 0 && rename(temp, path) != 0) e = errno;
    if (e) {
        if (how == 0) unlink(temp);                   // leave no debris
        fail_write(path, e);
    }
}

void nh_write_file(const char *path, const char *text) { write_file(path, text, 0, 0); }
void nh_append_file(const char *path, const char *text) { write_file(path, text, 1, 0); }
void nh_write_at(const char *path, const char *text, i64 offset) { write_file(path, text, 2, offset); }
void nh_write_at_ns(const char *path, const char *text, i64 offset) { write_file(path, text, 4, offset); }
void nh_truncate(const char *path, i64 size) { write_file(path, path, 3, size); }
void nh_truncate_ns(const char *path, i64 size) { write_file(path, path, 5, size); }

// the bytes of a file at an offset (fewer at the end, none outside it), or 0 when it does not
// open. The file's size and stamp (mtime in nanoseconds mixed with the inode: it changes when
// the file is written or replaced) are given by nh_last_size and nh_last_stamp (-1 and 0 when
// it did not open): on x86 they come back in rdx and rcx.
static i64 last_size = -1, last_stamp;
i64 nh_last_size(void) { return last_size; }
i64 nh_last_stamp(void) { return last_stamp; }

// The file is kept open for the next call (4 files, paths up to 256 bytes), as rt_read_at does on
// Linux: each call asks what is at the path now (stat), and uses the open file only while it is the
// same one (st_dev and st_ino); a file replaced or removed is opened again or forgotten.
static struct { int fd; dev_t dev; ino_t ino; size_t len; char path[257]; } kept[4];
static int kept_next;

char *nh_read_at(const char *t, i64 offset, i64 want) {
    char path[4096];
    last_size = -1; last_stamp = 0;
    if (!c_path(t, path, 4095)) return 0;
    size_t len = strlen(path);
    int slot = -1;
    for (int i = 0; i < 4; i++)
        if (kept[i].fd > 0 && kept[i].len == len && memcmp(kept[i].path, path, len) == 0) { slot = i; break; }
    struct stat st;
    int fd = -1, keep = 0;
    if (stat(path, &st) != 0) {                    // nothing there: forget a file kept for it
        if (slot >= 0) { close(kept[slot].fd - 1); kept[slot].fd = 0; }
        return 0;
    }
    if (slot >= 0 && kept[slot].dev == st.st_dev && kept[slot].ino == st.st_ino) {
        fd = kept[slot].fd - 1;                    // the same file: its fd, and the stat just made
        keep = 1;
    } else {
        if (slot >= 0) { close(kept[slot].fd - 1); kept[slot].fd = 0; }   // another file there now
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        if (fstat(fd, &st) != 0) { close(fd); return 0; }
        if (len <= 256) {
            int k = kept_next; kept_next = (kept_next + 1) & 3;
            if (kept[k].fd > 0) close(kept[k].fd - 1);
            kept[k].fd = fd + 1; kept[k].dev = st.st_dev; kept[k].ino = st.st_ino;
            kept[k].len = len; memcpy(kept[k].path, path, len); kept[k].path[len] = 0;
            keep = 1;
        }
    }
    i64 size = st.st_size;
    i64 stamp = (i64)((u64)st.st_mtimespec.tv_sec * 1000000000u + (u64)st.st_mtimespec.tv_nsec
                      + (u64)st.st_ino * 0x9E3779B97F4A7C15u);
    i64 n = 0;
    if (offset >= 0 && offset < size && want >= 0) { n = size - offset; if (n > want) n = want; }
    char *r = new_text(n);
    i64 got = 0;
    while (got < n) {
        ssize_t k = pread(fd, r + got, (size_t)(n - got), offset + got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;                           // error or end: keep what was read
        got += k;
    }
    ((i64 *)r)[-1] = got;
    r[got] = 0;
    if (!keep) close(fd);
    last_size = size;
    last_stamp = stamp;
    return r;
}

// ---- network: a TCP server (SPEC §23) ------------------------------------------------------
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

// a listening socket on the port (every address when all = 1, else only 127.0.0.1), or -errno
i64 nh_net_listen(i64 port, i64 all) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    cloexec(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = all ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 128) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

// the next connection, or -errno; what the program printed is written first, so it shows up
// while the server waits. A client that went away is an error on send, not the end of the
// program (SO_NOSIGPIPE: macOS has no MSG_NOSIGNAL).
i64 nh_net_accept(i64 server) {
    nh_flush();
    for (;;) {
        int c = accept((int)server, 0, 0);
        if (c >= 0) {
            cloexec(c);
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            // a connection that sends nothing must not hold the server (it serves one at a
            // time): a read gives up after 15 s, and the request is dropped
            struct timeval limit = {15, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
            return c;
        }
        if (errno != EINTR) return -errno;
    }
}

// a socket connected to an IPv4 address (a << 24 | b << 16 | c << 8 | d) and port, or -errno
// (the client side: the prelude finds the address and speaks HTTP)
i64 nh_net_connect(i64 address, i64 port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    cloexec(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl((uint32_t)address);
    while (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        if (errno == EINTR) continue;
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

// a new STRING with what one read gave (at most `most` bytes; length 0 at the end or on error)
char *nh_fd_read(i64 fd, i64 most) {
    if (most <= 0) most = 65536;
    char *r = new_text(most);
    ssize_t k;
    do k = read((int)fd, r, (size_t)most); while (k < 0 && errno == EINTR);
    if (k < 0) k = 0;
    ((i64 *)r)[-1] = k;                              // the block keeps its size
    r[k] = 0;
    return r;
}

// sends all of the text: 0, or -errno
i64 nh_fd_send(i64 fd, const char *text) {
    const char *p = text;
    i64 left = ((const i64 *)text)[-1];
    while (left > 0) {
        ssize_t w = send((int)fd, p, (size_t)left, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) return -errno;
        p += w; left -= w;
    }
    return 0;
}

void nh_fd_close(i64 fd) { close((int)fd); }

// ---- JSON: where a value ends (SPEC §21.2) ----------------------------------------------
// past the closing quote of the string at i (a backslash skips the next byte), or -1
static i64 skip_string(const char *s, i64 n, i64 i) {
    for (i++; i < n; i++) {
        if (s[i] == '\\') i++;
        else if (s[i] == '"') return i + 1;
    }
    return -1;
}

// the position after the JSON value that starts at i (after spaces), or -1: a string ends at
// its closing quote; an object or array where its brackets balance (strings inside skipped);
// anything else (number, true, false, null) at , } ] or a space
i64 nh_json_end(const char *s, i64 i) {
    i64 n = ((const i64 *)s)[-1];
    if (i < 0) return -1;
    while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    if (i >= n) return -1;
    char c = s[i];
    if (c == '"') return skip_string(s, n, i);
    if (c == '{' || c == '[') {
        i64 depth = 0;
        while (i < n) {
            c = s[i];
            if (c == '"') { i = skip_string(s, n, i); if (i < 0) return -1; continue; }
            i++;
            if (c == '{' || c == '[') depth++;
            else if ((c == '}' || c == ']') && --depth == 0) return i;
        }
        return -1;
    }
    i64 start = i;
    while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
    return i == start ? -1 : i;
}

// ---- tables: one record from its line (SPEC §20.7) ----------------------------------------
// text, the position where the line starts, the schema [n, type, width, …] (1 INT, 2 BOOL,
// 3 STRING, 4 FLOAT) → a new record (an ARRAY of n slots), or -1 when a field is not plain (an
// error, a FLOAT with an exponent or more than 15 digits, an INT that overflows): the prelude
// then reads that line its own way, with its messages. INT, BOOL and FLOAT drop the spaces
// around; STRING keeps the leading ones. FLOAT: Clinger's fast path, m × 10^-k with m < 10^15
// and k <= 22: one correctly rounded division, the same bits as the exact conversion.
static const double pow10_table[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                       1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

i64 nh_rec_parse(const char *text, i64 at, const i64 *schema) {
    i64 n = schema[0];
    i64 *rec = nh_alloc(n);
    for (i64 f = 0; f < n; f++) {
        i64 type = schema[1 + f * 2], width = schema[2 + f * 2];
        i64 a = at, b = at + width;
        at = b;
        if (type != 3) while (a < b && text[a] == ' ') a++;
        while (b > a && text[b - 1] == ' ') b--;
        i64 v = 0;
        if (type == 1) {                              // INT
            if (a < b) {
                int neg = 0;
                if (text[a] == '-') { neg = 1; a++; } else if (text[a] == '+') a++;
                if (a == b) return -1;               // a sign alone
                for (; a < b; a++) {
                    unsigned d = (unsigned char)text[a] - '0';
                    if (d > 9 || __builtin_mul_overflow(v, 10, &v) || __builtin_add_overflow(v, (i64)d, &v)) return -1;
                }
                if (neg) v = -v;
            }
        } else if (type == 2) {                       // BOOL
            if (b - a == 1 && text[a] == 'T') v = 1;
            else if (b - a == 1 && text[a] == 'F') v = 0;
            else if (b != a) return -1;
        } else if (type == 3) {                       // STRING
            char *s = new_text(b - a);
            memcpy(s, text + a, (size_t)(b - a));
            v = (i64)s;
        } else if (a < b) {                           // FLOAT
            int neg = 0, digits = 0, seen = 0;
            i64 m = 0, k = 0;
            if (text[a] == '-') { neg = 1; a++; } else if (text[a] == '+') a++;
            int dot = 0;
            for (; a < b; a++) {
                char c = text[a];
                if (c == '.' && !dot) { dot = 1; continue; }
                unsigned d = (unsigned char)c - '0';
                if (d > 9) return -1;                 // an exponent or something else
                seen = 1;
                if (m != 0 || d != 0) {               // a leading zero does not count
                    if (digits == 15) return -1;
                    digits++;
                    m = m * 10 + d;
                }
                if (dot) k++;
            }
            if (!seen || k > 22) return -1;
            double x = m ? (double)m / pow10_table[k] : 0.0;
            if (neg) x = -x;
            memcpy(&v, &x, 8);
        }
        rec[f] = v;
    }
    return (i64)rec;
}

// ---- the prelude's kernel calls (SPEC §24) -------------------------------------------------
// The prelude calls the Linux kernel by number ([n, a1 … a6]) for directories, files and
// processes, and reads Linux's structures. On macOS each call is made with libSystem and its
// results are written in the Linux layout, so the prelude stays the same: getdents64 records
// (length at +16, the name at +19), struct stat (the mode at +24, the size at +48), the
// flags of openat translated. The answer is what the call gives, or -errno.
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>

#define L_AT_FDCWD (-100)
static int at_dir(i64 d) { return d == L_AT_FDCWD ? AT_FDCWD : (int)d; }

static int open_flags(i64 l) {                       // Linux's open flags → macOS's
    int f = (int)(l & 3);                            // O_RDONLY, O_WRONLY, O_RDWR: the same
    if (l & 0x40) f |= O_CREAT;
    if (l & 0x80) f |= O_EXCL;
    if (l & 0x200) f |= O_TRUNC;
    if (l & 0x400) f |= O_APPEND;
    if (l & 0x800) f |= O_NONBLOCK;
    if (l & 0x10000) f |= O_DIRECTORY;
    if (l & 0x20000) f |= O_NOFOLLOW;
    if (l & 0x80000) f |= O_CLOEXEC;
    return f;
}

static i64 res(i64 r) { return r < 0 ? -errno : r; }

// getdents64 on a directory opened by openat: its entries as Linux records, as many as fit
static DIR *dirs[256];
static i64 l_getdents64(int fd, unsigned char *buf, i64 cap) {
    if (fd < 0 || fd >= 256) return -EBADF;
    if (!dirs[fd]) {
        int copy = dup(fd);
        if (copy < 0 || !(dirs[fd] = fdopendir(copy))) return -errno;
    }
    i64 used = 0;
    for (;;) {
        long pos = telldir(dirs[fd]);
        struct dirent *e = readdir(dirs[fd]);
        if (!e) break;
        i64 len = (i64)strlen(e->d_name);
        i64 rec = (19 + len + 1 + 7) & ~(i64)7;
        if (used + rec > cap) { seekdir(dirs[fd], pos); break; }
        unsigned char *r = buf + used;
        memset(r, 0, (size_t)rec);
        u64 ino = e->d_ino;
        memcpy(r, &ino, 8);                           // d_ino, d_off (left 0), d_reclen, d_type, d_name
        r[16] = (unsigned char)rec; r[17] = (unsigned char)(rec >> 8);
        r[18] = e->d_type;
        memcpy(r + 19, e->d_name, (size_t)len);
        used += rec;
    }
    return used;
}

static void l_close(int fd) {
    if (fd >= 0 && fd < 256 && dirs[fd]) { closedir(dirs[fd]); dirs[fd] = 0; }
    close(fd);
}

i64 nh_syscall(const i64 *a) {
    i64 n = a[-1], x[7] = {0};
    for (i64 i = 0; i < n && i < 7; i++) x[i] = a[i];
    switch (x[0]) {
    case 0: return res(read((int)x[1], (void *)x[2], (size_t)x[3]));
    case 1: return res(write((int)x[1], (const void *)x[2], (size_t)x[3]));
    case 3: l_close((int)x[1]); return 0;
    case 8: return res(lseek((int)x[1], x[2], (int)x[3]));
    case 13:                                          // rt_sigaction(sig, act, …): only SIG_DFL / SIG_IGN
        if (x[2]) signal((int)x[1], *(const i64 *)x[2] == 1 ? SIG_IGN : SIG_DFL);
        return 0;
    case 17: return res(pread((int)x[1], (void *)x[2], (size_t)x[3], x[4]));
    case 18: return res(pwrite((int)x[1], (const void *)x[2], (size_t)x[3], x[4]));
    case 73: return res(flock((int)x[1], (int)x[2]));    // flock: LOCK_EX 2, LOCK_UN 8 (the same values)
    case 74: return full_sync((int)x[1]) == 0 ? 0 : -errno;  // fsync, through the drive's cache as the runtime's
    case 21: return res(access((const char *)x[1], (int)x[2]));
    case 33: return res(dup2((int)x[1], (int)x[2]));
    case 57: return res(fork());
    case 59: execve((const char *)x[1], (char *const *)x[2], (char *const *)x[3]); return -errno;
    case 61: {                                        // wait4(pid, status, options, rusage)
        int st = 0;
        pid_t r = wait4((pid_t)x[1], &st, (int)x[3], 0);
        if (r < 0) return -errno;
        if (x[2]) memcpy((void *)x[2], &st, 4);
        return r;
    }
    case 62: return res(kill((pid_t)x[1], (int)x[2]));
    case 80: return res(chdir((const char *)x[1]));
    case 83: return res(mkdir((const char *)x[1], (mode_t)x[2]));
    case 217: return l_getdents64((int)x[1], (unsigned char *)x[2], x[3]);
    case 228: {                                       // clock_gettime(0 = realtime, 1 = monotonic, timespec)
        struct timespec ts;
        if (clock_gettime(x[1] == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts) != 0) return -errno;
        ((i64 *)x[2])[0] = ts.tv_sec; ((i64 *)x[2])[1] = ts.tv_nsec;
        return 0;
    }
    case 231: nh_flush(); _exit((int)x[1]);
    case 257: return res(openat(at_dir(x[1]), (const char *)x[2], open_flags(x[3]), (int)x[4]));
    case 5:                                           // fstat(fd, struct stat (Linux))
    case 262: {                                       // newfstatat(dir, path, struct stat (Linux), flags)
        struct stat st;
        unsigned char *b;
        if (x[0] == 5) {
            if (fstat((int)x[1], &st) != 0) return -errno;
            b = (unsigned char *)x[2];
        } else {
            if (fstatat(at_dir(x[1]), (const char *)x[2], &st, (x[4] & 0x100) ? AT_SYMLINK_NOFOLLOW : 0) != 0) return -errno;
            b = (unsigned char *)x[3];
        }
        memset(b, 0, 144);
        u64 dev = (u64)st.st_dev, ino = st.st_ino; uint32_t mode = st.st_mode; i64 size = st.st_size;
        i64 msec = st.st_mtimespec.tv_sec, mnsec = st.st_mtimespec.tv_nsec;
        memcpy(b + 0, &dev, 8);
        memcpy(b + 8, &ino, 8);
        memcpy(b + 24, &mode, 4);
        memcpy(b + 48, &size, 8);
        memcpy(b + 88, &msec, 8);                     // st_mtime, st_mtime_nsec
        memcpy(b + 96, &mnsec, 8);
        return 0;
    }
    case 263: return res(unlinkat(at_dir(x[1]), (const char *)x[2], (x[3] & 0x200) ? AT_REMOVEDIR : 0));
    case 293: {                                       // pipe2(fds, flags)
        int p[2];
        if (pipe(p) != 0) return -errno;
        if (x[2] & 0x80000) { cloexec(p[0]); cloexec(p[1]); }
        memcpy((void *)x[1], p, 8);
        return 0;
    }
    case 436: {                                       // close_range(first, last, 0)
        i64 last = getdtablesize() - 1;
        if (x[2] < last) last = x[2];
        for (i64 fd = x[1]; fd <= last; fd++) close((int)fd);
        return 0;
    }
    }
    return -ENOSYS;
}
