// bench/compare/tables.c — the work of bench/tables.nihil, in C, for comparison: the same records
// (the same pseudo-random sequence), the same fixed-width text table, the same index on disk
// (FNV-1a of the key's text, linear probing, 4-byte slots after a 16-byte header) and the same
// syncs: a whole table is written to a temporary file, synced and renamed; a change in place
// syncs the table, an addition and a deletion the table and the index. It keeps its two files
// open, as a C program would, where Nihil opens them for each operation.
//
//   cc -O2 -o tables bench/compare/tables.c && N=100000 DIR=/tmp/b ./tables
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define W 49                    // the width of a record, without its "\n"
typedef struct { long code; char name[21]; long category; double price; long qty; int active; } Item;

static long N, OPS, SYNCED;
static char FILE_[1024], IDX[1024], TMP[1024];
static unsigned long long seed = 12345;
static long rnd(long n) { seed = (seed * 1103515245ULL + 12345ULL) % 2147483648ULL; return (long)(seed / 16 % (unsigned long long)n); }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double T0;
static void start(void) { T0 = now_ms(); }
static void done(const char *what, long count) {
    double ms = now_ms() - T0;
    if (count > 0) printf("%s: %.1f ms (%ld, %.2f µs each)\n", what, ms, count, ms * 1000.0 / count);
    else printf("%s: %.1f ms\n", what, ms);
}
static void die(const char *what) { fprintf(stderr, "%s: %s\n", what, strerror(errno)); exit(1); }

// the data on the disk: macOS's fsync stops at the drive's cache (F_FULLFSYNC goes through it, as Nihil does)
static void sync_fd(int fd) {
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) return;
#endif
    if (fsync(fd) != 0) die("fsync");
}

// a FLOAT as Nihil writes it here: the fewest decimals (at least one) that read back the same
static int float_text(double v, char *out) {
    for (int d = 1; d <= 17; d++) {
        int n = snprintf(out, 32, "%.*f", d, v);
        if (strtod(out, NULL) == v) return n;
    }
    return snprintf(out, 32, "%.17g", v);
}

static void line_of(const Item *r, char *b) {          // W bytes, no "\n"
    char f[40];
    int n = float_text(r->price, f);
    int k = snprintf(b, W + 1, "%8ld%-20s%4ld%*s%s%6ld%c", r->code, r->name, r->category, 10 - n, "", f, r->qty, r->active ? 'T' : 'F');
    if (k != W) { fprintf(stderr, "bad width %d\n", k); exit(1); }
}

static long int_at(const char *s, int a, int b) {
    long v = 0; int neg = 0;
    while (a < b && s[a] == ' ') a++;
    if (a < b && s[a] == '-') { neg = 1; a++; }
    for (; a < b && s[a] >= '0' && s[a] <= '9'; a++) v = v * 10 + (s[a] - '0');
    return neg ? -v : v;
}
static void parse(const char *s, Item *r) {
    r->code = int_at(s, 0, 8);
    int e = 28; while (e > 8 && s[e - 1] == ' ') e--;
    memcpy(r->name, s + 8, e - 8); r->name[e - 8] = 0;
    r->category = int_at(s, 28, 32);
    char f[11]; memcpy(f, s + 32, 10); f[10] = 0; r->price = strtod(f, NULL);
    r->qty = int_at(s, 42, 48);
    r->active = s[48] == 'T';
}

static uint64_t hash(const char *s, int n) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
    return h;
}
static int key_text(long k, char *out) { return sprintf(out, "%ld", k); }

static int tfd, ifd;            // the table and its index, kept open
static int CHECK;               // CHECK=1: before each operation, a stat of the path, as Nihil does
static dev_t tdev;
static ino_t tino;
static long cap;                // the index's capacity
static long nrec;               // records in the table

static uint32_t slot(long j) {
    uint32_t v; if (pread(ifd, &v, 4, 16 + j * 4) != 4) die("pread slot"); return v;
}
static void set_slot(long j, uint32_t v, int sync) {
    if (pwrite(ifd, &v, 4, 16 + j * 4) != 4) die("pwrite slot");
    if (sync) sync_fd(ifd);
}
static void header(void) {
    int64_t h[2] = { nrec * (W + 1), 0 };
    if (pwrite(ifd, h, 16, 0) != 16) die("pwrite header");
}
static long key_of_line(long i) {
    char b[W]; if (pread(tfd, b, W, i * (W + 1)) != W) die("pread record"); return int_at(b, 0, 8);
}

// writes a whole file atomically: a temporary file, synced, renamed over the path
static void write_atomic(const char *path, const char *data, size_t n) {
    snprintf(TMP, sizeof TMP, "%s.tmp", path);
    int fd = open(TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) die("open tmp");
    for (size_t o = 0; o < n;) { ssize_t k = write(fd, data + o, n - o); if (k <= 0) die("write"); o += k; }
    sync_fd(fd); close(fd);
    if (rename(TMP, path) != 0) die("rename");
}

static void build_index(void) {
    struct stat st; if (fstat(tfd, &st) != 0) die("fstat");
    nrec = st.st_size / (W + 1);
    char *text = malloc(st.st_size + 1);
    if (pread(tfd, text, st.st_size, 0) != st.st_size) die("pread table");
    cap = 16; while (cap < 2 * (nrec + 1)) cap *= 2;
    uint32_t *slots = calloc(cap + 4, 4);
    int64_t *h = (int64_t *)slots; h[0] = st.st_size; h[1] = 0;
    uint32_t *s = slots + 4;
    char k[24];
    for (long i = 0; i < nrec; i++) {
        int n = key_text(int_at(text, i * (W + 1), i * (W + 1) + 8), k);
        long j = hash(k, n) & (cap - 1);
        while (s[j]) j = (j + 1) & (cap - 1);
        s[j] = i + 1;
    }
    write_atomic(IDX, (char *)slots, (cap + 4) * 4);
    free(slots); free(text);
    if (ifd >= 0) close(ifd);
    ifd = open(IDX, O_RDWR); if (ifd < 0) die("open idx");
}

static void build_index(void);
// CHECK=1: is the table at the path still the file kept open? If another program replaced it,
// open it again and rebuild the index (the check Nihil makes before each operation)
static void check_table(void) {
    if (!CHECK) return;
    struct stat st;
    if (stat(FILE_, &st) != 0) die("stat");
    if (st.st_dev == tdev && st.st_ino == tino) return;
    close(tfd);
    tfd = open(FILE_, O_RDWR); if (tfd < 0) die("open table");
    tdev = st.st_dev; tino = st.st_ino;
    build_index();
}

// where key k is: its record number (or -1), and its slot (or the empty one where it would go)
static long find(long k, long *jout, Item *r) {
    check_table();
    char kt[24]; int n = key_text(k, kt);
    long j = hash(kt, n) & (cap - 1);
    for (long left = cap; left > 0; left--) {
        uint32_t v = slot(j);
        if (!v) { *jout = j; return -1; }
        char b[W]; if (pread(tfd, b, W, (long)(v - 1) * (W + 1)) != W) die("pread record");
        if (int_at(b, 0, 8) == k) { if (r) parse(b, r); *jout = j; return v - 1; }
        j = (j + 1) & (cap - 1);
    }
    *jout = j; return -1;
}

static void put(const Item *r) {                // @{}>> path, r
    long j, i = find(r->code, &j, NULL);
    char b[W + 1]; line_of(r, b); b[W] = '\n';
    if (i >= 0) {
        if (pwrite(tfd, b, W + 1, i * (W + 1)) != W + 1) die("pwrite");
        sync_fd(tfd);
    } else {
        if (pwrite(tfd, b, W + 1, nrec * (W + 1)) != W + 1) die("pwrite");
        sync_fd(tfd);
        nrec++;
        if (nrec * 2 > cap) { build_index(); return; }
        set_slot(j, nrec, 1);
    }
    header();
}

static void delete_key(long k) {                // NAME @{k} path = _
    long j, i = find(k, &j, NULL);
    if (i < 0) { fprintf(stderr, "no key %ld\n", k); exit(1); }
    long last = nrec - 1, mask = cap - 1;
    if (i != last) {
        char b[W + 1]; if (pread(tfd, b, W + 1, last * (W + 1)) != W + 1) die("pread last");
        long moved = int_at(b, 0, 8);
        if (pwrite(tfd, b, W + 1, i * (W + 1)) != W + 1) die("pwrite");
        sync_fd(tfd);
        if (ftruncate(tfd, last * (W + 1)) != 0) die("ftruncate");
        char kt[24]; int n = key_text(moved, kt);
        long m = hash(kt, n) & mask;
        while (slot(m) != (uint32_t)(last + 1)) m = (m + 1) & mask;
        set_slot(m, i + 1, 0);
    } else {
        if (ftruncate(tfd, last * (W + 1)) != 0) die("ftruncate");
        sync_fd(tfd);
    }
    nrec--;
    // the deleted key's slot is emptied, moving back each slot after it whose home is not between
    long kk = (j + 1) & mask;
    uint32_t v = slot(kk);
    while (v) {
        char kt[24]; int n = key_text(key_of_line(v - 1), kt);
        long h = hash(kt, n) & mask;
        int stay = j < kk ? (h > j && h <= kk) : (h > j || h <= kk);
        if (!stay) { set_slot(j, v, 0); j = kk; }
        kk = (kk + 1) & mask;
        v = slot(kk);
    }
    set_slot(j, 0, 1);
    header();
}

static Item *SORT_ROWS;
static int by_price_desc(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    double px = SORT_ROWS[x].price, py = SORT_ROWS[y].price;
    if (px > py) return -1;
    if (px < py) return 1;
    return x < y ? -1 : x > y;        // stable: the earlier record first
}

int main(void) {
    N = getenv("N") ? atol(getenv("N")) : 100000;
    OPS = getenv("OPS") ? atol(getenv("OPS")) : 20000;
    SYNCED = getenv("SYNCED") ? atol(getenv("SYNCED")) : 300;
    CHECK = getenv("CHECK") && atoi(getenv("CHECK")) == 1;
    const char *dir = getenv("DIR") ? getenv("DIR") : "/tmp/nihil-bench";
    mkdir(dir, 0755);
    snprintf(FILE_, sizeof FILE_, "%s/items-c.dat", dir);
    snprintf(IDX, sizeof IDX, "%s.idx", FILE_);
    unlink(FILE_); unlink(IDX); ifd = -1;
    printf("N = %ld records of %d bytes\n", N, W + 1);

    start();
    Item *rows = malloc(N * sizeof(Item));
    for (long i = 0; i < N; i++) {
        long p = rnd(100000);
        rows[i].code = i + 1;
        snprintf(rows[i].name, sizeof rows[i].name, "item %ld", i + 1);
        rows[i].category = rnd(50);
        rows[i].price = p / 100.0;
        rows[i].qty = rnd(1000);
        rows[i].active = rnd(10) > 1;
    }
    done("make the records in memory", N);

    start();
    char *text = malloc((size_t)N * (W + 1) + 1);
    for (long i = 0; i < N; i++) { line_of(&rows[i], text + i * (W + 1)); text[i * (W + 1) + W] = '\n'; }
    write_atomic(FILE_, text, (size_t)N * (W + 1));
    free(text);
    done("write the table (atomic)", N);
    struct stat st; stat(FILE_, &st);
    printf("  file: %lld KB\n", (long long)st.st_size / 1024);

    start();
    int fd = open(FILE_, O_RDONLY); fstat(fd, &st);
    char *all = malloc(st.st_size + 1);
    for (off_t o = 0; o < st.st_size;) { ssize_t k = read(fd, all + o, st.st_size - o); if (k <= 0) die("read"); o += k; }
    close(fd);
    long n = st.st_size / (W + 1);
    Item *table = malloc(n * sizeof(Item));
    for (long i = 0; i < n; i++) parse(all + i * (W + 1), &table[i]);
    free(all);
    done("read the whole table", n);

    start();
    long found = 0; for (long i = 0; i < n; i++) if (table[i].active && table[i].price < 200.0) found++;
    char what[80]; snprintf(what, sizeof what, "where(active && price < 200.0): %ld found", found);
    done(what, n);

    start();
    long c7 = 0; for (long i = 0; i < n; i++) if (table[i].category == 7) c7++;
    snprintf(what, sizeof what, "count(category == 7): %ld", c7);
    done(what, n);

    start();
    double total = 0; for (long i = 0; i < n; i++) total += table[i].price * (double)table[i].qty;
    done("sum(price * qty)", n);

    start();
    long *order = malloc(n * sizeof(long)); for (long i = 0; i < n; i++) order[i] = i;
    SORT_ROWS = table; qsort(order, n, sizeof(long), by_price_desc);
    done("sort(-price)", n);

    start();
    tfd = open(FILE_, O_RDWR); if (tfd < 0) die("open table");
    { struct stat st; fstat(tfd, &st); tdev = st.st_dev; tino = st.st_ino; }
    build_index();
    long j; Item r; find(1, &j, &r);
    done("first key lookup (reads the table, builds the index)", 0);
    stat(IDX, &st); printf("  index: %lld KB\n", (long long)st.st_size / 1024);

    start();
    for (long k = 0; k < OPS; k++) if (find(rnd(N) + 1, &j, &r) < 0) { fprintf(stderr, "lost\n"); return 1; }
    done("lookup by key", OPS);

    start();
    for (long k = 0; k < OPS; k++) { char b[W]; check_table(); if (pread(tfd, b, W, rnd(N) * (W + 1)) != W) die("pread"); parse(b, &r); }
    done("read record i", OPS);

    start();
    for (long k = 0; k < SYNCED; k++) { find(rnd(N) + 1, &j, &r); r.qty++; put(&r); }
    done("change by key (lookup + write in place, 1 fsync)", SYNCED);

    start();
    for (long k = 0; k < SYNCED; k++) { Item a = { N + 1 + k, "new", 1, 1.0, 1, 1 }; put(&a); }
    done("add by key (2 fsync)", SYNCED);

    start();
    for (long k = 0; k < SYNCED; k++) delete_key(N + 1 + k);
    done("delete by key (2 fsync)", SYNCED);

    close(tfd); close(ifd);
    if (!getenv("KEEP")) { unlink(FILE_); unlink(IDX); }    // KEEP=1: the table stays, to compare
    if (total < 0) printf("%f\n", total);       // keeps the sum from being optimized away
    return 0;
}
