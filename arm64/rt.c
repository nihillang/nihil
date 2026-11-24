// =============================================================================
// arm64/rt.c — what the Nihil runtime needs from macOS on ARM64 (Apple silicon).
//
// The runtime is Linux's own, src/runtime.asm, translated to ARM64 with the program
// (macos/translate_rt.py): the collector, texts, files, network and output are the same code on
// both systems. What differs is the kernel: each Linux system call of the runtime (and of the
// prelude's __nihil_syscall, which goes through the runtime) reaches nh_linux_syscall, which
// makes it with macOS's calls, with Linux's numbers, flags, layouts and errno values on both
// sides. Where macOS differs:
//   - MADV_DONTNEED does not zero the pages: they are mapped again (mmap MAP_FIXED);
//   - mincore works on the system's pages (16 KiB on Apple silicon), not 4 KiB; a page never
//     written reports only MINCORE_ANONYMOUS;
//   - sockaddr has a length byte, SOL_SOCKET and its options other numbers, and there is no
//     MSG_NOSIGNAL (SO_NOSIGPIPE instead);
//   - /proc does not exist: /proc/self/environ, /proc/uptime and /proc/sys/kernel/random/uuid are
//     made from macOS's equivalents; the stack's top and the data's end, which Linux's runtime
//     finds in /proc and in the ELF header, the translated start gives it (PLAT_STACK_TOP, …).
// =============================================================================
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef int64_t i64;
typedef uint64_t u64;

#define L_AT_FDCWD (-100)
extern char **environ;

// ---- the x86 stack, emulated by x28: its own area (the translated code's start asks for it) ----
void *nh_emu_stack(void) {
    i64 size = (i64)256 << 20;                       // reserved, not used: pages come when touched
    char *p = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { write(2, "nihil: out of memory\n", 21); _exit(2); }
    return p + size;
}

static int full_sync(int fd) {
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
    return fsync(fd);
}

static void cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

// the contents of one of those files in malloc'd memory and their length (environ's 0s are its
// separators), or 0
static char *virtual_file_bytes(const char *path, size_t *len) {
    if (strcmp(path, "/proc/self/environ") == 0) {
        size_t n = 0;
        for (char **e = environ; *e; e++) n += strlen(*e) + 1;
        char *r = malloc(n + 1);
        size_t at = 0;
        for (char **e = environ; *e; e++) { size_t k = strlen(*e) + 1; memcpy(r + at, *e, k); at += k; }
        r[n] = 0;
        *len = n;
        return r;
    }
    char *t = 0;
    if (strcmp(path, "/proc/uptime") == 0 || strcmp(path, "/proc/sys/kernel/random/uuid") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        char b[64];
        if (path[6] == 'u') {
            snprintf(b, sizeof b, "%lld.%02ld 0.00\n", (long long)ts.tv_sec, ts.tv_nsec / 10000000);
        } else {
            unsigned char u[16];
            arc4random_buf(u, sizeof u);
            u[6] = (u[6] & 0x0F) | 0x40; u[8] = (u[8] & 0x3F) | 0x80;
            snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        }
        t = strdup(b);
        *len = strlen(t);
    }
    return t;
}

// the same files for the translated runtime, which reads them through openat: a file of its own,
// already removed, with the contents, read from the start (0 when the path is not one of them)
static int virtual_fd(const char *path) {
    size_t n = 0;
    char *v = virtual_file_bytes(path, &n);
    if (!v) return -1;
    char name[] = "/tmp/nihil-proc-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) { free(v); return -1; }
    unlink(name);
    size_t at = 0;
    while (at < n) { ssize_t k = write(fd, v + at, n - at); if (k <= 0) break; at += (size_t)k; }
    free(v);
    lseek(fd, 0, SEEK_SET);
    cloexec(fd);
    return fd;
}

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

// macOS's errno → Linux's, for the ones whose numbers differ (the runtime and the prelude speak
// Linux's: their messages and checks use those numbers)
static i64 linux_errno(int e) {
    switch (e) {
    case EAGAIN: return 11;          case EDEADLK: return 35;      case ENAMETOOLONG: return 36;
    case ENOLCK: return 37;          case ENOSYS: return 38;       case ENOTEMPTY: return 39;
    case ELOOP: return 40;           case ENOMSG: return 42;       case EIDRM: return 43;
    case ENOSTR: return 60;          case ENODATA: return 61;      case ETIME: return 62;
    case ENOSR: return 63;           case ENOLINK: return 67;      case EPROTO: return 71;
    case EMULTIHOP: return 72;       case EBADMSG: return 74;      case EOVERFLOW: return 75;
    case EILSEQ: return 84;          case EUSERS: return 87;       case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89;    case EMSGSIZE: return 90;     case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92;     case EPROTONOSUPPORT: return 93; case ESOCKTNOSUPPORT: return 94;
    case EOPNOTSUPP: return 95;      case EPFNOSUPPORT: return 96; case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;      case EADDRNOTAVAIL: return 99; case ENETDOWN: return 100;
    case ENETUNREACH: return 101;    case ENETRESET: return 102;   case ECONNABORTED: return 103;
    case ECONNRESET: return 104;     case ENOBUFS: return 105;     case EISCONN: return 106;
    case ENOTCONN: return 107;       case ESHUTDOWN: return 108;   case ETOOMANYREFS: return 109;
    case ETIMEDOUT: return 110;      case ECONNREFUSED: return 111; case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;   case EALREADY: return 114;    case EINPROGRESS: return 115;
    case ESTALE: return 116;         case EDQUOT: return 122;      case ECANCELED: return 125;
    case EOWNERDEAD: return 130;     case ENOTRECOVERABLE: return 131;
    default: return e;               // 1 … 34: the same numbers
    }
}

static i64 res(i64 r) { return r < 0 ? -linux_errno(errno) : r; }

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

// Linux's sockaddr_in (u16 family, u16 port, u32 address) → macOS's (u8 length, u8 family, …)
static struct sockaddr_in mac_sockaddr(const unsigned char *l) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    memcpy(&a.sin_port, l + 2, 2);
    memcpy(&a.sin_addr, l + 4, 4);
    return a;
}

// a new socket or connection: closed on exec when asked, and a peer that went away is an error
// on send, not SIGPIPE (macOS has no MSG_NOSIGNAL)
static i64 own_socket(int fd, i64 flags) {
    if (fd < 0) return -linux_errno(errno);
    if (flags & 0x80000) cloexec(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return fd;
}

// one Linux system call (its number and arguments as the kernel takes them) made with macOS's:
// the prelude's __nihil_syscall and every syscall of the translated runtime come here
i64 nh_linux_syscall(i64 n0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    i64 x[7] = {n0, a1, a2, a3, a4, a5, a6};
    switch (x[0]) {
    case 9: {                                         // mmap: the flags' numbers differ
        int f = (int)(x[4] & 3);                       // MAP_SHARED, MAP_PRIVATE
        if (x[4] & 0x10) f |= MAP_FIXED;
        if (x[4] & 0x20) f |= MAP_ANON;
        if (x[4] & 0x4000) f |= MAP_NORESERVE;
        void *p = mmap((void *)x[1], (size_t)x[2], (int)x[3], f, (int)x[5], (off_t)x[6]);
        return p == MAP_FAILED ? -linux_errno(errno) : (i64)p;
    }
    case 11: return res(munmap((void *)x[1], (size_t)x[2]));
    case 27: {                                        // mincore: Linux's 4 KiB pages from macOS's (16 KiB)
        i64 pg = getpagesize(), len = x[2], start = x[1];
        i64 first = start & ~(pg - 1);
        i64 sys = (start + len - first + pg - 1) / pg;
        unsigned char *v = malloc((size_t)sys);
        if (!v) return -12;
        if (mincore((void *)first, (size_t)(sys * pg), (char *)v) != 0) { free(v); return -linux_errno(errno); }
        i64 pages = (len + 4095) / 4096;
        // any state (in memory, paged out, compressed …) counts as there, but a page never written
        // reports only MINCORE_ANONYMOUS: nothing to scan there, and reading it would make it real
        for (i64 i = 0; i < pages; i++)
            ((unsigned char *)x[3])[i] = (v[(start + i * 4096 - first) / pg] & ~MINCORE_ANONYMOUS) != 0;
        free(v);
        return 0;
    }
    case 28:                                          // madvise
        if (x[3] == 4) {                              // MADV_DONTNEED: Linux zeroes the pages, macOS does not:
            void *p = mmap((void *)x[1], (size_t)x[2], PROT_READ | PROT_WRITE,   // fresh pages there
                           MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            return p == MAP_FAILED ? -linux_errno(errno) : 0;
        }
        return 0;                                     // MADV_HUGEPAGE and the others: only advice
    case 35: return res(nanosleep((const struct timespec *)x[1], (struct timespec *)x[2]));
    case 41: return own_socket(socket((int)x[1], (int)(x[2] & 0xF), (int)x[3]), x[2]);
    case 42: {
        struct sockaddr_in a = mac_sockaddr((const unsigned char *)x[2]);
        return res(connect((int)x[1], (struct sockaddr *)&a, sizeof a));
    }
    case 43: case 288: {                              // accept, accept4(fd, addr, len, flags)
        int c;
        do { c = accept((int)x[1], 0, 0); } while (c < 0 && errno == EINTR);
        return own_socket(c, x[0] == 288 ? x[4] : 0);
    }
    case 44: return res(send((int)x[1], (const void *)x[2], (size_t)x[3], 0));   // sendto, no address
    case 45: return res(recv((int)x[1], (void *)x[2], (size_t)x[3], 0));         // recvfrom, no address
    case 49: {
        struct sockaddr_in a = mac_sockaddr((const unsigned char *)x[2]);
        return res(bind((int)x[1], (struct sockaddr *)&a, sizeof a));
    }
    case 50: return res(listen((int)x[1], (int)x[2]));
    case 54: {                                        // setsockopt: SOL_SOCKET and its options have other numbers
        int level = (int)x[2], opt = (int)x[3];
        if (x[2] == 1) {
            level = SOL_SOCKET;
            switch (x[3]) {
            case 2: opt = SO_REUSEADDR; break;
            case 9: opt = SO_KEEPALIVE; break;
            case 20: opt = SO_RCVTIMEO; break;
            case 21: opt = SO_SNDTIMEO; break;
            default: return 0;
            }
        }
        return res(setsockopt((int)x[1], level, opt, (const void *)x[4], (socklen_t)x[5]));
    }
    case 60: _exit((int)x[1]);
    case 77: return res(ftruncate((int)x[1], (off_t)x[2]));
    case 82: return res(rename((const char *)x[1], (const char *)x[2]));
    case 87: return res(unlink((const char *)x[1]));
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
    case 231: _exit((int)x[1]);
    case 257:
        if (strncmp((const char *)x[2], "/proc/", 6) == 0) {   // the Linux files the programs read
            int fd = virtual_fd((const char *)x[2]);
            return fd >= 0 ? fd : -2;                 // ENOENT: maps, pagemap … (the runtime has others)
        }
        return res(openat(at_dir(x[1]), (const char *)x[2], open_flags(x[3]), (int)x[4]));
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
