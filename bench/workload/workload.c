// bench/workload/workload.c — the work of workload.nihil in C (cc -O2): N sales made up, written to a
// table (atomically, synced) and read back, added up in hash tables (each sale priced by one of four
// rules, through a function pointer), each store's best products ranked, the result written as JSON.
// The same bytes as the Nihil and Go versions (run.sh compares them).
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

enum { STORES = 20, PRODUCTS = 2000, TOP = 20 };

static int64_t seed = 12345;
static int64_t rnd(int64_t n) { seed = (seed * 1103515245 + 12345) % 2147483648LL; return seed / 16 % n; }

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int64_t T;
static void phase(const char *name) { int64_t n = now_us(); fprintf(stderr, "%s %lld\n", name, (long long)((n - T) / 1000)); T = n; }

static void die(const char *what) { perror(what); exit(2); }

// text that grows
typedef struct { char *p; size_t n, cap; } buf;
static void put(buf *b, const char *s, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); if (!b->p) die("realloc"); }
    memcpy(b->p + b->n, s, n); b->n += n;
}
static void puts_(buf *b, const char *s) { put(b, s, strlen(s)); }
static void puti(buf *b, int64_t v) { char t[24]; int n = snprintf(t, sizeof t, "%lld", (long long)v); put(b, t, (size_t)n); }

// a file replaced atomically: a temporary, synced, renamed (as Nihil's @>>)
static void write_file(const char *path, const char *p, size_t n) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die(tmp);
    size_t done = 0;
    while (done < n) { ssize_t w = write(fd, p + done, n - done); if (w <= 0) die("write"); done += (size_t)w; }
    if (fsync(fd) != 0) die("fsync");
    close(fd);
    if (rename(tmp, path) != 0) die("rename");
}

// INT → INT hash table: open addressing, linear probing, at most 2/3 full (as Nihil's MAP)
typedef struct { int64_t *keys, *vals; char *used; size_t cap, n; } imap;
static void imap_init(imap *m, size_t cap) { m->cap = cap; m->n = 0; m->keys = calloc(cap, 8); m->vals = calloc(cap, 8); m->used = calloc(cap, 1); }
static size_t ihash(int64_t k, size_t cap) { uint64_t h = (uint64_t)k * 0x9E3779B97F4A7C15ULL; return (size_t)(h ^ (h >> 29)) & (cap - 1); }
static void imap_grow(imap *m);
static int64_t *imap_slot(imap *m, int64_t k, int add) {
    size_t i = ihash(k, m->cap);
    while (m->used[i]) { if (m->keys[i] == k) return &m->vals[i]; i = (i + 1) & (m->cap - 1); }
    if (!add) return NULL;
    if ((m->n + 1) * 3 > m->cap * 2) { imap_grow(m); return imap_slot(m, k, 1); }
    m->used[i] = 1; m->keys[i] = k; m->vals[i] = 0; m->n++;
    return &m->vals[i];
}
static void imap_grow(imap *m) {
    imap o = *m;
    imap_init(m, o.cap * 2);
    for (size_t i = 0; i < o.cap; i++) if (o.used[i]) *imap_slot(m, o.keys[i], 1) = o.vals[i];
    free(o.keys); free(o.vals); free(o.used);
}

// STRING → INT hash table, FNV-1a
typedef struct { const char **keys; int64_t *vals; size_t cap, n; } smap;
static void smap_init(smap *m, size_t cap) { m->cap = cap; m->n = 0; m->keys = calloc(cap, sizeof *m->keys); m->vals = calloc(cap, 8); }
static size_t shash(const char *s, size_t cap) { uint64_t h = 14695981039346656037ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return (size_t)h & (cap - 1); }
static void smap_grow(smap *m);
static int64_t *smap_slot(smap *m, const char *k, int add) {
    size_t i = shash(k, m->cap);
    while (m->keys[i]) { if (strcmp(m->keys[i], k) == 0) return &m->vals[i]; i = (i + 1) & (m->cap - 1); }
    if (!add) return NULL;
    if ((m->n + 1) * 3 > m->cap * 2) { smap_grow(m); return smap_slot(m, k, 1); }
    m->keys[i] = k; m->vals[i] = 0; m->n++;
    return &m->vals[i];
}
static void smap_grow(smap *m) {
    smap o = *m;
    smap_init(m, o.cap * 2);
    for (size_t i = 0; i < o.cap; i++) if (o.keys[i]) *smap_slot(m, o.keys[i], 1) = o.vals[i];
    free(o.keys); free(o.vals);
}

// the price rules, through a function pointer
static int64_t regular(int64_t q, int64_t p) { return q * p; }
static int64_t bulk(int64_t q, int64_t p) { return q >= 5 ? q * p * 90 / 100 : q * p; }
static int64_t promo(int64_t q, int64_t p) { return (q - q / 3) * p; }
static int64_t fee(int64_t q, int64_t p) { return q * p + q * 15; }
static int64_t (*const rules[4])(int64_t, int64_t) = { regular, bulk, promo, fee };

typedef struct { int store, product, month, qty, price; } sale;
typedef struct { int64_t product, revenue; } item;
static int by_revenue(const void *a, const void *b) {
    const item *x = a, *y = b;
    if (x->revenue != y->revenue) return x->revenue > y->revenue ? -1 : 1;
    return x->product < y->product ? -1 : x->product > y->product;
}

// an INT field of a line: spaces, then digits (fixed width, right-aligned)
static int field(const char *s, int w) { int v = 0; for (int i = 0; i < w; i++) if (s[i] != ' ') v = v * 10 + (s[i] - '0'); return v; }

int main(void) {
    const char *e = getenv("N");
    int64_t N = e ? atoll(e) : 100000;
    const char *DIR = getenv("DIR") ? getenv("DIR") : "/tmp/nihil-workload";
    char FILE_[4096], OUT[4096];
    snprintf(FILE_, sizeof FILE_, "%s/sales.dat", DIR);
    snprintf(OUT, sizeof OUT, "%s/out.json", DIR);
    T = now_us();

    // 1. generate
    static int64_t price[PRODUCTS + 1];
    static char names[PRODUCTS + 1][8];
    for (int p = 1; p <= PRODUCTS; p++) { price[p] = 100 + rnd(9900); snprintf(names[p], 8, "P%05d", p); }
    sale *sales = malloc((size_t)N * sizeof *sales);
    for (int64_t i = 0; i < N; i++) {
        int pr = (int)(1 + rnd(PRODUCTS)), st = (int)(1 + rnd(STORES)), mo = (int)(1 + rnd(12)), q = (int)(1 + rnd(10));
        sales[i] = (sale){ st, pr, mo, q, (int)price[pr] };
    }
    phase("generate");

    // 2. table: 20 characters a line, written atomically, read back
    buf t = {0};
    for (int64_t i = 0; i < N; i++) {
        char line[32];
        int n = snprintf(line, sizeof line, "%3d%5d%2d%2d%8d\n", sales[i].store, sales[i].product, sales[i].month, sales[i].qty, sales[i].price);
        put(&t, line, (size_t)n);
    }
    write_file(FILE_, t.p, t.n);
    free(t.p);
    FILE *f = fopen(FILE_, "rb");
    if (!f) die(FILE_);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    if (fread(text, 1, (size_t)size, f) != (size_t)size) die("read");
    fclose(f);
    int64_t nrows = size / 21;
    sale *rows = malloc((size_t)nrows * sizeof *rows);
    for (int64_t i = 0; i < nrows; i++) {
        const char *l = text + i * 21;
        rows[i] = (sale){ field(l, 3), field(l + 3, 5), field(l + 8, 2), field(l + 10, 2), field(l + 12, 8) };
    }
    free(text);
    phase("table");

    // 3. aggregate
    imap by_pair; imap_init(&by_pair, 65536);
    int64_t by_month[13] = {0};
    smap units; smap_init(&units, 4096);
    int64_t grand = 0;
    for (int64_t i = 0; i < nrows; i++) {
        sale *s = &rows[i];
        int64_t total = rules[s->product % 4](s->qty, s->price);
        *imap_slot(&by_pair, (int64_t)s->store * 100000 + s->product, 1) += total;
        by_month[s->month] += total;
        *smap_slot(&units, names[s->product], 1) += s->qty;
        grand += total;
    }
    phase("aggregate");

    // 4. rank
    static item tops[STORES * TOP];
    int firsts[STORES + 2];
    int ntops = 0;
    item *items = malloc(PRODUCTS * sizeof *items);
    for (int st = 1; st <= STORES; st++) {
        int n = 0;
        for (int p = 1; p <= PRODUCTS; p++) {
            int64_t *v = imap_slot(&by_pair, (int64_t)st * 100000 + p, 0);
            if (v) items[n++] = (item){ p, *v };
        }
        qsort(items, (size_t)n, sizeof *items, by_revenue);
        firsts[st] = ntops;
        for (int j = 0; j < TOP && j < n; j++) tops[ntops++] = items[j];
    }
    firsts[STORES + 1] = ntops;
    phase("rank");

    // 5. JSON
    buf j = {0};
    puts_(&j, "{\"stores\": [");
    for (int st = 1; st <= STORES; st++) {
        if (st > 1) puts_(&j, ", ");
        puts_(&j, "{\"store\": "); puti(&j, st); puts_(&j, ", \"top\": [");
        for (int k = firsts[st]; k < firsts[st + 1]; k++) {
            if (k > firsts[st]) puts_(&j, ", ");
            puts_(&j, "{\"product\": "); puti(&j, tops[k].product);
            puts_(&j, ", \"name\": \""); puts_(&j, names[tops[k].product]);
            puts_(&j, "\", \"revenue\": "); puti(&j, tops[k].revenue); puts_(&j, "}");
        }
        puts_(&j, "]}");
    }
    puts_(&j, "], \"pairs\": [");
    int64_t npairs = 0;
    for (int st = 1; st <= STORES; st++)
        for (int p = 1; p <= PRODUCTS; p++) {
            int64_t *v = imap_slot(&by_pair, (int64_t)st * 100000 + p, 0);
            if (!v) continue;
            if (npairs > 0) puts_(&j, ", ");
            puts_(&j, "{\"s\": "); puti(&j, st); puts_(&j, ", \"p\": "); puti(&j, p);
            puts_(&j, ", \"r\": "); puti(&j, *v); puts_(&j, "}");
            npairs++;
        }
    puts_(&j, "], \"months\": [");
    for (int m = 1; m <= 12; m++) { if (m > 1) puts_(&j, ", "); puti(&j, by_month[m]); }
    puts_(&j, "], \"units\": {");
    for (int p = 1; p <= PRODUCTS; p++) {
        if (p > 1) puts_(&j, ", ");
        int64_t *u = smap_slot(&units, names[p], 0);
        puts_(&j, "\""); puts_(&j, names[p]); puts_(&j, "\": "); puti(&j, u ? *u : 0);
    }
    puts_(&j, "}}\n");
    write_file(OUT, j.p, j.n);
    phase("json");

    printf("sales %lld, revenue %lld, pairs %lld, json %zu bytes\n", (long long)N, (long long)grand, (long long)npairs, j.n);
    return 0;
}
