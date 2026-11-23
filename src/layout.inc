; =============================================================================
; layout.inc — addresses and offsets shared by the compiler (nihilc.asm)
; and the runtime embedded in the generated executables (runtime.asm). SPEC §12.3.
; =============================================================================

; ---- generated executable ------------------------------------------------------
%define BASE_CODE     0x400000
%define SIZE_HEADER   0xB0            ; ELF header (64) + 2 program headers (2 × 56)
%define BASE_DATA      0x40000000

; ---- data segment (in memory only, zeroed by the kernel) ------------------
%define OUTPUT_SIZE       (BASE_DATA + 0)        ; qword: bytes pending in the buffer
%define HEAP_POS        (BASE_DATA + 8)        ; next free byte of the heap
%define HEAP_END        (BASE_DATA + 16)       ; end of the current heap block
%define IN_POS         (BASE_DATA + 24)       ; next byte to hand out from the input buffer
%define IN_SIZE         (BASE_DATA + 32)       ; valid bytes in the input buffer
%define IN_END         (BASE_DATA + 40)       ; 1 = input has ended
; the garbage collector (runtime.asm, "heap"): its state, below OUTPUT_BUF
%define GC_STATE        (BASE_DATA + 0x40)     ; 0 = not started, 1 = ready
%define GC_HEAP_BASE    (BASE_DATA + 0x48)     ; first page of the heap (64 KiB aligned)
%define GC_HEAP_LIMIT   (BASE_DATA + 0x50)     ; end of the reserved address space
%define GC_BUMP         (BASE_DATA + 0x58)     ; first page never handed out
%define GC_DESC         (BASE_DATA + 0x60)     ; one descriptor (qword) per page
%define GC_RUNS         (BASE_DATA + 0x68)     ; runs of free pages: (first page, pages)
%define GC_NRUNS        (BASE_DATA + 0x70)
%define GC_MSTACK       (BASE_DATA + 0x78)     ; mark stack (objects still to scan)
%define GC_MSTACK_END   (BASE_DATA + 0x80)
%define GC_STACK_TOP    (BASE_DATA + 0x88)     ; top of the program's stack (0: no collection)
%define GC_DATA_END     (BASE_DATA + 0x90)     ; end of the globals
%define GC_ALLOCATED    (BASE_DATA + 0x98)     ; bytes allocated since the last collection
%define GC_THRESHOLD    (BASE_DATA + 0xA0)     ; collect when GC_ALLOCATED passes it
%define GC_LIVE         (BASE_DATA + 0xA8)     ; bytes in use after the last collection
%define GC_COUNT        (BASE_DATA + 0xB0)     ; collections so far
%define GC_DIRTY        (BASE_DATA + 0xB8)     ; empty small pages kept for reuse (page numbers)
%define GC_NDIRTY       (BASE_DATA + 0xC0)
%define GC_MINCORE      (BASE_DATA + 0xC8)     ; buffer for mincore (a byte per 4 KiB) or pagemap entries
%define GC_PAGEMAP      (BASE_DATA + 0xD0)     ; /proc/self/pagemap's fd + 1 during a collection (0: none)
; protected calls (SPEC §25): PROT_BASE is a block of 8 KiB (made at the first one): 64 levels of
; [rsp, rbp, resume, rbx, r12, r13, r14, r15], then the text of the last caught error (4000 bytes)
%define PROT_BASE       (BASE_DATA + 0xD8)
%define PROT_DEPTH      (BASE_DATA + 0xE0)     ; protections active
%define ERR_CAPTURE     (BASE_DATA + 0xE8)     ; 1: a runtime error is being written into the text
%define ERR_LEN         (BASE_DATA + 0xF0)     ; bytes of that text
%define GC_FREE         (BASE_DATA + 0x100)    ; 21 free lists, one per size class
; files kept open by rt_read_at, so a table read one record at a time is not opened each time:
; FD_CACHE_NEXT is the slot to use next (0 … 3); each of the 4 slots (FD_SLOT bytes) holds
; [fd + 1 (0: empty), st_dev, st_ino, path length, path (up to 256 bytes)]
; a collection marked by a child process (gc_fork): its pid (0: none), the map it marks in (a byte
; for every 32 bytes of the heap, shared with it) and its length, the heap's bytes when it forked,
; how the map is used (1: the child marks in it, 2: the sweep reads it) and whether pages were
; handed out after the fork (the sweep then stops before them)
%define GC_CHILD        (BASE_DATA + 0x1A8)
%define GC_MAP          (BASE_DATA + 0x1B0)
%define GC_MAP_LEN      (BASE_DATA + 0x1B8)
%define GC_SNAP         (BASE_DATA + 0x1C0)
%define GC_MAPMODE      (BASE_DATA + 0x1C8)
%define GC_TAIL         (BASE_DATA + 0x1D0)
; make … NASMFLAGS=-DGC_STATS (a diagnostic build): the times of the collection under way, in µs,
; and the blocks it marked
%define GC_STAT_T0      (BASE_DATA + 0x1D8)
%define GC_STAT_T1      (BASE_DATA + 0x1E0)
%define GC_STAT_T2      (BASE_DATA + 0x1E8)
%define GC_STAT_MARKED  (BASE_DATA + 0x1F0)
%define FD_CACHE_NEXT   (BASE_DATA + 0x1F8)
%define FD_CACHE        (BASE_DATA + 0x200)
%define FD_SLOT         288
%define FD_SLOTS        4
; a sweep in steps (gc_sweep_step): 1 while pages are left, the next one, and the last one
%define GC_SWEEPING     (BASE_DATA + 0x680)
%define GC_SWEEP_POS    (BASE_DATA + 0x688)
%define GC_SWEEP_END    (BASE_DATA + 0x690)
%define OUTPUT_BUF       (BASE_DATA + 0x1000)   ; standard output buffer
%define OUTPUT_CAP       65536
%define IN_BUF         (BASE_DATA + 0x11000)  ; standard input buffer
%define IN_CAP         65536
%define GLOBALS         (BASE_DATA + 0x21000)  ; globals: 8 bytes each

; ---- runtime jump table --------------------------------------------
; The runtime starts at BASE_CODE + SIZE_HEADER (the program's entry) with
; a 5-byte "jmp program" that nihilc patches, followed by one "jmp near
; routine" entry (5 bytes) per routine. New routines ALWAYS go at the end.
%define RT_DIV          5       ; rax = rax / rcx        (error if rcx = 0)
%define RT_MOD          10      ; rax = rax % rcx        (error if rcx = 0)
%define RT_PRINT_INT  15      ; >> rax  (INT)
%define RT_PRINT_BOOL 20      ; >> rax  (BOOL)
%define RT_PRINT_STR  25      ; >> rax  (STRING: pointer, length in [rax-8])
%define RT_EXIT         30      ; flushes the output and exits with code rdi
%define RT_ERROR         35      ; runtime error: rsi = message (0-terminated), exits with 2
%define RT_ALLOC        40      ; rcx = n → rax = new array of n zeroed elements
%define RT_ARRAY_REP    45      ; rcx = n, rdx = value → rax = [value ; n]
%define RT_ERROR_INDEX  50      ; target of the "jae" when rcx >= length of rax
%define RT_WRITE_BYTE 55      ; >>> al
%define RT_READ_BYTE      60      ; rax = next input byte, or -1 at the end
%define RT_STR_EQUAL    65      ; rax = (STRING rax == STRING rcx)
%define RT_ERROR_INT     70      ; !>> rax (INT): flushes the output and writes to stderr
%define RT_ERROR_BOOL    75      ; !>> rax (BOOL)
%define RT_ERROR_STR     80      ; !>> rax (STRING)
%define RT_READ_FILE   85      ; rax = contents of the file at path rax (STRING)
%define RT_TEXT        90      ; rax = STRING with the bytes of the ARRAY of INT rax
%define RT_PRINT_INT_OPT  95  ; >> optional: rax = value, rdx = presence (0 → "_")
%define RT_PRINT_BOOL_OPT 100
%define RT_PRINT_STR_OPT  105
%define RT_ERROR_INT_OPT     110 ; !>> opcional
%define RT_ERROR_BOOL_OPT    115
%define RT_ERROR_STR_OPT     120
%define RT_READ_FILE_OPT   125 ; like RT_READ_FILE, but returns 0 if it does not open
%define RT_CONCAT           130 ; rax = STRING rax + STRING rcx
%define RT_SLEEP            135 ; ... rax: flushes the output and waits rax milliseconds
%define RT_WRITE_FILE       140 ; @>> rax, rcx: file at path rax (STRING) gets text rcx (STRING), atomically
%define RT_APPEND_FILE      145 ; @+>> rax, rcx: appends text rcx to the file at path rax
%define RT_READ_AT          150 ; rax = path, rcx = offset, rdx = bytes → rax = STRING read there (0: no file), rdx = file size, rcx = stamp (mtime + inode)
%define RT_WRITE_AT         155 ; rax = path, rcx = text, rdx = offset: writes the text there (pwrite + fsync)
%define RT_WRITE_AT_NS      160 ; the same, without the fsync
%define RT_TRUNCATE         165 ; rax = path, rdx = size: cuts the file (ftruncate + fsync)
%define RT_TRUNCATE_NS      170 ; the same, without the fsync
%define RT_INT_TEXT         175 ; rax = INT → rax = STRING with it in decimal
%define RT_ARRAY_CONCAT     180 ; rax = ARRAY + ARRAY rcx (same type) → a new ARRAY
%define RT_NET_LISTEN       185 ; rax = port, rcx = every address? → rax = socket (or -errno)
%define RT_NET_ACCEPT       190 ; rax = socket → rax = connection (or -errno)
%define RT_FD_READ          195 ; rax = fd, rcx = most → rax = STRING (length 0 at the end)
%define RT_FD_SEND          200 ; rax = fd, rcx = text → rax = 0 or -errno
%define RT_FD_CLOSE         205 ; rax = fd
%define RT_JSON_END         215 ; rax = STRING, rcx = i → rax = the position after the JSON value at i (after spaces), or -1
%define RT_SUB              220 ; rax = STRING, rcx = a, rdx = b → rax = a new STRING with bytes a … b - 1
%define RT_PUT              225 ; rax = STRING r (new, being built), rcx = at, rdx = STRING t → t copied into r at; rax = at + #t
%define RT_REC_PARSE        230 ; rax = text, rcx = where the line starts, rdx = schema → rax = the record (ARRAY address), or -1: let the prelude do it
%define RT_LINES            235 ; rax = text, rcx = width → rax = how many lines, all of that width (-1 if one is not)
%define RT_PROTECT          245 ; rax = where to resume: a runtime error inside goes there (rax = rdx = 0)
%define RT_UNPROTECT        250 ; the protected call ended normally (keeps rax, rdx; ?! = "")
%define RT_ERR_TEXT         255 ; → rax = ?!, a new STRING
%define RT_FAIL             260 ; rax = STRING: a runtime error with that message
%define RT_NET_CONNECT      240 ; rax = IPv4 address (a.b.c.d as a << 24 | … | d), rcx = port → rax = connected socket (or -errno)
%define RT_SYSCALL          210 ; rax = ARRAY of INT [n, a1 … a6] (missing ones 0) → rax = what the kernel gave
