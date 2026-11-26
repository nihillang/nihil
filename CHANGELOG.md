# Changelog

What each version of the language added, and why. Appendix A of
[SPEC.md](SPEC.md) keeps the same history together with the design decisions made
before v2.0.

## v4.11.0

The five pillars of object orientation, without a keyword: private fields, a model built from another, a child where
its parent is expected, replacing methods with dispatch by the record's real model, and abstract methods. Each step
was reviewed by Codex before it was written, and no program that compiled before changes meaning.

- abstract methods (SPEC §20.11): `SHAPE.area(s = SHAPE()) { ... : 0.0; }` declares the method and its return type by an example (`{ ... }`: nothing) and leaves the body to the models below. A model that does not implement one, or makes an inherited one abstract again, is abstract: no record of it is made (constructor, table reads, JSON, copies, a default), while `[SHAPE() ; *]` and `{k: SHAPE() ; n}` still give the type, and a global cannot hold one. Both forms were compile errors before, so no program changes meaning
  - *Why:* The fifth pillar of object orientation, abstraction: a parent that says what its children must do. The return type is written, not inferred from the children (Codex's review: it would tie the parent to whatever children a program has), and the method compiles to one that raises, so dispatch needs nothing new
- a child may replace an inherited method (SPEC §20.10): `TEACHER.label` replaces `PERSON.label`, and a call through a PERSON runs the record's real model's, the nearest one declaring it (a grandchild gets its parent's), also inside the parent's own methods. A replacement takes the same parameters after the first and returns the same type; defaults come from the method the static type finds. A call no descendant replaces is the plain call it was
  - *Why:* The polymorphism of the pillars of object orientation: a mixed team in one LIST of PERSON, each one labelled its own way. The record's model is in its header (previous change), so the call compares it and calls directly: no table of methods, and the macOS translator takes it as it is
- inside: a record of a model in a hierarchy carries its model in its block's header (SPEC §20.12), set by every way of making a record (constructors, defaults, record globals, `[r ; n]`, the table readers, `from_json`); nothing a program sees changes yet
  - *Why:* The first half of replacing inherited methods (docs/dispatch.md): a call through a parent needs the record's real model. The header's top 16 bits were always zero, so no field moves, and the collector only masks them where it reads a block's size
- a child where its parent is expected, at four places: an explicit argument for a parameter of the parent's type (optional ones too), and an element stored into an ARRAY, LIST or MAP of the parent (SPEC §20.9). It is the same record; copies, `$r`, JSON and table writes through the parent take the parent's fields. Variables, returns and literals still need equal models, and a collection of a child is never taken for one of its parent. Every source without a model built from another compiles to the same bytes as before (170 checked, optimized and `-O0`)
  - *Why:* A function on a PERSON and a mixed team (students and teachers in one LIST of PERSON) are the uses of a parent type, and these places take a type the compiler already knows, so they change no inference. Letting variables, returns and literals take a child needs the inference to tell a provisional type from a fixed one: a larger change, left for later (docs/subtyping.md), after two reviews by Codex
- a model can start from another: `CHILD = @[PARENT; field: …];` has the parent's fields first (in its order, with its key) and then its own, and the parent's methods work on it (SPEC §20.8). A child has a table of its own, and the IDE reads it with the whole line. For now a child does not replace an inherited method and is not accepted where its parent is expected; a parent's private fields stay private to the parent
  - *Why:* Step 2 of the way to the pillars of object orientation, cut down after a review by Codex: models that share fields repeated them by hand. Reusing the fields and methods needs no new symbol and changes no program; letting a child stand for its parent needs rules for copies, tables and JSON first, and replacing methods needs dispatch
- private fields: a field whose name starts with `_` is private to its model, and only the model's own methods read it or change it (SPEC §20.7); anywhere else, `r._field`, `r._field = v;` and a query naming it are compile errors. Creating a record, tables and JSON still go through every field. No field in the repository or in the programs we know of started with `_`, so no program changes meaning
  - *Why:* The first of four steps towards the pillars of object orientation (encapsulation; then inheritance of fields, dispatch and abstract methods): a model's methods could keep its data consistent, but nothing stopped the rest of the program from changing a field behind them. `_` is a name the language already allows, so this needs no keyword and no new symbol

## v4.10.0

A profiler and faster FLOAT text, from measuring a real program (a planogram generator) on its Linux server;
protected blocks, try/catch without a keyword; and a short reference for the IDE's coding agents.

- `nihil profile PROGRAM.nihil [ARG…]`: runs the program as `nihil run` does, sampled every 10 ms of CPU time, then reports where the time went: by method and runtime routine, the runtime's time by the method that called it, and each method with what it called (docs/profiling.md). Behind it, the compiler's `-profile` option and a `SIGPROF` sampler in the runtime, which writes `nihil-profile.PID` at the exit; `tools/profile.nihil` reads it. Linux x86_64 for now. `make profilecheck` (CI) checks it
  - *Why:* Measuring where a real program spent its time took gdb, a hand-made sampler and the assembler's listing (`perf` was blocked on the machine). The cost is under 1% (the planogram's `generate`: 5.86 → 5.91 s), so a program can be measured as it runs
- `?{ … } || { … }`: a protected block (SPEC §25.1), as `?f()` protects a call. A runtime error anywhere inside the block goes to the block after `||`, with `?!` holding its text; what the block changed before the error stays changed. `: value;` inside it leaves the protection too
  - *Why:* Protecting a few commands took a method written only to be called with `?`. The symbols are the ones the language already has: `?` for "protected" and `||` for "otherwise", so no program changes meaning (`{ … } : { … }` was considered, but `: {…}` already returns a MAP after a plain block)
- FLOAT → text takes its fast way, with exact integers, for numbers with up to 17 decimals (it was up to 3) from about 10^-5 to 2^53: ratios, percentages and measures no longer go through the general algorithm and its big integers. The text is the same (`make floatcheck`: 501,117 cases against Python and against the general algorithm, now with 4 to 17 decimals among them)
  - *Why:* A profile of a real program (a planogram generator writing JSON) showed 12% of its time in the general algorithm; it runs 8% faster (6.39 → 5.89 s), with the same output byte for byte
- `NIHIL-AGENTS.md`: a short reference for coding agents (about 1,800 tokens against the SPEC's 27,600): a table from C, Go and Python to Nihil, the ten traps a model trained on other languages falls into, the core syntax in two examples, and pointers to the SPEC's sections. The IDE puts it in its agents' instructions (Claude Code's system prompt, Codex's first message) and points to the SPEC only for details; without the file it gives the old instructions. It ships in `share/nihil`, and `tests/stage1/agents_reference_*` keep its examples and claims true
  - *Why:* The IDE told its agents to read the SPEC before writing code. Measured on the same repair tasks, a small model given the SPEC spent 2.6 times the tokens; the short reference as a file still cost it an extra turn, while the same text in the instructions halved its tokens against the SPEC, cut its reasoning below having no reference at all, and kept it to the protocol. Larger models were unaffected either way

## v4.9.1

Three fixes in the runtime, found while researching threads for Nihil and measuring the planogram on its Linux server.

- `@>>` writes its temporary as `path.nihil-tmp.PID` (this process's id) instead of `path.nihil-tmp`: two programs replacing the same file no longer write into one temporary. The longest path `@>>` accepts goes from 4080 to 4064 bytes
  - *Why:* With one shared name, one program renamed the other's half-written temporary, and the other then failed with "could not write: no such file or directory" (two programs writing one file 1,500 times each: one of them failed before, both finish now)
- Writing to the standard output and errors, reading the standard input and reading a file try again when the kernel interrupts the call (`EINTR`), as the network and the table reads already did
  - *Why:* An interrupted write stopped silently and an interrupted read looked like the end of the input; a prerequisite for anything that delivers signals to the program
- The heap no longer asks the kernel for huge pages (`MADV_HUGEPAGE`)
  - *Why:* Where the kernel's defrag setting is `madvise`, a common default, a machine short of free memory compacted memory on the spot for each 2 MiB page: the planogram's `generate` took 14 to 68 s instead of 12 to 14 s, and its server started in 3 to 6 s instead of 1.5 s (2 CPUs, 2 GB free of 31 GB). Where the setting is `always`, the kernel still gives huge pages

## v4.9.0

- LIST, an ARRAY that grows (SPEC §28): `[x ; *]` is an empty list of x's type (`[x ; *n]` with room for n), `v[] = x;` adds at the end, and `v[i]`, `v[i] = x`, `#v` and `xs : x, i { … }` work as on an ARRAY. A list is a reference with a shared header, as a MAP is: every name of a list sees the same items however much it grows. `v.array()` copies the items to an ARRAY, every other ARRAY method works on such a copy, and `to_json` writes lists anywhere. Adding n items costs O(n) copies in all: 1,000,000 appends take 0.01 s
  - *Why:* Programs that collected items either copied the whole array for each one (`x = x + [y]`, 37 times in the IDE's server and the assembler) or grew it by hand with a counter beside it

## v4.8.0

- `xs : x, i { … }`: each element of an ARRAY (each byte of a STRING) and, if wanted, its index (SPEC §7.6). The collection is evaluated once and its size taken on entry; the names after the `:` are new locals of the block, as a method's parameters are new names (one that exists is a compile error), so a loop never changes a global by accident
  - *Why:* Every big program repeated the same loop by hand: `i = 0; * i < #xs { x = xs[i]; … i = i + 1; }`
- `?! = text;` raises a runtime error with that text (SPEC §25): a protected call `?f()` catches it like any other and `?!` holds the text; outside any protection the program ends with `nihil: text` and exit code 2
  - *Why:* A program that had to give up on a step made an error on purpose (an index past an array) and kept the reason somewhere else

## v4.7.1

Eleven fixes in the prelude, found in a review by Codex, each confirmed by running it and its fix reviewed again:

- `<< n` into an INT: a digit after `-9223372036854775808` gave `0` (v4.7.0's fix of `.int()` had not reached the input); it is the invalid-input error
- a table path with a NUL byte: the fast reader opened the path up to the NUL (`"VERSION\0x"` read `VERSION`); it is refused
- automatic keys: a counter behind the table with a free number gave a key below the largest; `.nihil-seq` now keeps the table's size and stamp, and while they match the counter is used without reading the table, else the largest key + 1 is found once (SPEC §20.5)
- `json_keys()` changed a global variable called `keys`
- `s.find(t, from)` with `from` near the largest INT gave an index error; it gives `_`
- `rows.sort(key)` by a FLOAT: `0.0` and `-0.0` changed order; the sort is stable for them
- `s.float()`: leading zeros used up the 768 places for digits (`"0" × 768 + "1"` read as `0.0`)
- `MODEL.from_json` took a `\u` surrogate without its pair and made invalid UTF-8; it is refused, and the lenient `s.json_text()` gives U+FFFD for it (SPEC §26)
- `http_host_is_local()` refused `[::1]`, which SPEC §23 names; `[::1]` with any port is local, `[::1].evil` is not
- `cmd.start()` / `cmd.run()` of a program that cannot start gave a process that ended with 127 (`["/"].run()` was `127`); it is `_`, told by a pipe that `execve` closes
- `find`, `contains`, `split` and `replace` compared at every position: `"a" × 200,000` searched for `"a" × 100,000 + "b"` took 10 s; a needle over 8 bytes uses Knuth–Morris–Pratt, linear
  - *Why:* Wrong results at the edges, a table that could open another file or repeat a key, and a search that could take seconds

## v4.7.0

- the runtime is assembled by Nihil's own assembler: `asm/asm.nihil` turns `src/runtime.asm` into `src1/runtime_bin.nihil` with the same bytes as nasm (the runtime and its `GC_STRESS` / `GC_STATS` builds, compared byte by byte), in about 40 ms; `make runtime` uses it, so a change to the runtime no longer needs nasm (only stage 0, the root of the bootstrap, does). It follows nasm's choices of encoding (short jumps and 8-bit values when they fit, a label's value always relocatable) and refuses with a message what it does not encode exactly. `make asm-check` (CI) compares it with nasm on the runtime and on `asm/test.asm` (186 more forms), and checks that the 38 cases of `asm/refuse.asm` are refused
  - *Why:* A change to the runtime needed nasm, the last tool from outside past stage 0; the language now assembles its own runtime
- three safety fixes, found in a review of the compiler by Codex and each confirmed by running it:
  - `s.repeat(n)`: `#s * n` past the largest INT went round to a small size and the copies ran past the block (`"AB".repeat(2^63 - 1)` wrote about 1 GB outside it); it is now a runtime error, `text too large`. `join`, `replace` and the text of a table check their sizes the same way
  - `s.int()`: a digit after `-9223372036854775808` gave `0`; it gives `_`
  - `MODEL.from_json` took a cut or broken document as a record (`{"qty": 5` gave 5); the whole text is now checked by JSON's grammar (RFC 8259, UTF-8 strings), and anything else is a runtime error (SPEC §26). `s.json(key)` stays lenient
  - *Why:* A language that says it is safe must not write outside a block, nor read a broken document as data
- the IDE's chat can talk to **Codex** (OpenAI's Codex CLI) as well as Claude: each tab chooses its agent. Codex has its own folder in the IDE (`~/.codex-ide`, as Claude Code has `~/.claude-ide`), signs in with a ChatGPT plan or an API key (`/login` in a Codex tab), and its commands, edits and tokens show in the panel as Claude's do. The tab's mode chooses its sandbox: ask and plan read only, accept edits and auto write in the project
  - *Why:* Some people work with Codex, and the IDE should not choose for them
- the IDE's chat takes a message while Claude works: it joins the same run, as in Claude Code's terminal (at once when Claude only waits for work in the background, else after the tool that runs); ■ Stop puts a message not yet taken back in the box (by Matheus Mônaco do Carmo)
  - *Why:* Send was disabled for as long as a background command ran
- go to definition in the IDE: ⌘-click (Ctrl-click), F12 or the right-click menu goes to where a name is declared (a method, a model's method, a model, a global, a local of the method), in the file and the files it includes; `std:` libraries open read only, from `share/nihil/std`, which the package now carries
  - *Why:* Finding a method meant searching the files by hand
- the IDE's server closes a run's input when the run ends, and never writes to an input once closed: a run stopped before its input was closed lost one descriptor
- the queries over records on the Mac measure the same as in v4.5.0 (7.3 ms for `count` + `sum` + `where` over 1,000,000 records, 50 times); the "10–50% slower" of v4.6.0 came from timing only the first query, while the collector that marks in a child process was at work
  - *Why:* v4.6.0's note said it was to be looked at

## v4.6.0

- macOS runs the same runtime as Linux: `src/runtime.asm`, which every executable carries, is translated to ARM64 with the program, and `arm64/rt.c` only turns Linux's system calls into macOS's (1,389 → 356 lines). The Mac gets the collector that marks in a child process, the lock for writes by key, the compact records and the tables read in parts. Measured on the Mac against v4.5.0: making records 21% faster, reading a table 8% faster, 13% less memory; queries over records 10–50% slower (the records' new layout), to be looked at
  - *Why:* Two runtimes, one in assembly and one in C, had to do the same thing: each improvement was made twice, or the Mac stayed behind (it still had the collector that stops the program)
- the Mac's translator is a Nihil program (`macos/translate.nihil`), in place of the two Python scripts; it writes the same assembly, byte for byte. The package carries it built, so Nihil on macOS needs only the Xcode Command Line Tools (clang, objdump), not python3. The first one is built by the previous release's `nihil` (`macos/seed.sh`), and every new one must translate itself into the same assembly (`macos/translator.sh`, and the release's macOS job)
  - *Why:* The compiler, its library and the IDE's server were in Nihil, but every program on the Mac went through Python
- `site/make_learn.nihil` writes the Learn page (each chapter's program run, with what it printed), in place of a Python script; `.gitattributes` leaves the benchmarks, the tests' helpers and the site out of GitHub's language bar
  - *Why:* The repository showed C and Python as if the language were written in them

## v4.5.0

- the garbage collector no longer stops the program for a whole collection on Linux: from 64 MB of heap, a child process marks while the program goes on, and the sweep runs in steps as the program allocates (SPEC §12.4). Pauses went from 120–150 ms to about 1 ms with a million records in memory; the heap asks for huge pages. On the Mac the collector is the one before
  - *Why:* A program holding hundreds of MB stopped for a tenth of a second at each collection: a server saw it as slow answers
- writes by key go through the table and the index kept open, and two programs writing the same table by key now wait for each other: each holds a lock on `<path>.nihil-lock` from the lookup to the last write (SPEC §20.5). Reads take no lock
  - *Why:* Four programs adding records to one table at once kept 300 of 1,200; with the lock all 1,200 are there
- a lookup by key allocates nothing on the way: 2.2 → 1.6 µs (C, checking the table as Nihil does: 2.0 µs). Lists given to the kernel are built on the stack, texts are compared 8 bytes at a time, and the index's slots are read 16 at a time
  - *Why:* Every lookup made four to six blocks of memory, which the collector then had to take back
- tables use less memory and are read faster: a record read from a table is one block with its texts inside, and the file is read in parts of about 1 MB, so it is never whole in memory (1,000,000 records: 149 → 105 MB at the peak, 125 → 103 ms; records with a STRING(200): 82 → 25 MB)
  - *Why:* At the peak, a read held the whole file and every record, with a block for each text
- the compiler keeps more in registers: the locals most used in loops get them (not the first five declared), the top level's variables that no method uses become its own locals, `x = x + y` changes the register in place, a simple index needs no stack, and a query's loop and its FLOAT sum live in registers. Counting to 100 million at the top level: 155 → 49 ms; summing an array: 176 → 88 ms. Loops start at an address multiple of 16
  - *Why:* Every variable lived in memory and every operation went through the stack, 3 to 6 times C on simple loops
- `"…" + $n` is made in one block, and `$n` turns an INT into text by multiplication instead of division (`"item " + $(i)`, a million times: 95 → 33 ms)
  - *Why:* Building texts with numbers was two thirds of the time of making records in memory
- `bench/compare`: `CHECK=1` makes the C and Go programs check the table's path before each operation, as Nihil does, for a fair comparison
  - *Why:* Without it they trust that no other program replaces the table, which Nihil does not

## v4.4.0

- `std:time.nihil`: `now_us()` and `now_ms()`, microseconds and milliseconds on the monotonic clock, to time something; `unix_time()`, seconds since 1970 by the system clock
  - *Why:* A program could only read /proc/uptime, which ticks every 10 ms: too coarse to time anything short
- tables are faster, closer to C and Go (bench/compare, Linux, 1 000 000 records): writing a table from 1.3 s to 0.76 s (C: 0.90 s), a lookup by key from 15 to 3 µs (C: 1.5 µs), a record by its number from 3 to 1.3 µs, building the index from 480 to 360 ms
  - the runtime keeps up to four files open between reads, and checks with a stat of the path that each is still the file there: a table replaced by a rename (an editor, another program) is opened again, a removed one forgotten (SPEC §20.5)
  - the key operations keep the table and its index open themselves: a lookup is one stat and two reads
  - a FLOAT with at most 3 decimals (a price, a measure, a whole number) is turned into text with exact integer arithmetic instead of big integers, 8 times faster; the text is the same, checked on 200,000 values against the general algorithm and against Python
  - building the index hashes each key straight from its line and no longer works a key out again at each collision
  - *Why:* Measured against C and Go doing the same work, Nihil was 2 to 7 times slower on tables, mostly from opening files on every read and from turning prices into text with big integers
- `make runtime` and `make runtime-check` work on macOS (the runtime's bytes are written with `tr`, not `xargs`)
  - *Why:* macOS's xargs breaks the line, so the bytes did not match the ones made on Linux

## v4.3.3

- the IDE's Claude panel shows a tab's earlier messages when the IDE is opened again: they are read from the file Claude Code keeps of the conversation (the latest 1.5 MB, without the tools' results), so it costs no tokens, and the next message goes on with the same conversation
  - *Why:* A tab kept its conversation but showed nothing of it after the IDE was reopened, so it looked lost
- the bottom panel (Terminal, Problems, Assembly, Table) can be minimized to its bar of tabs with ▾ or Ctrl+J / Cmd+J, and comes back when something has to be shown there
  - *Why:* The editor could not take the whole height of the window
- the browser no longer offers to translate the IDE (`translate="no"`), the scroll bars take the IDE's dark colors, and the panel's Activity starts closed (opened, it stays open)
  - *Why:* A translator changes code and names on screen; the system's light scroll bars and an open Activity took attention and room from the code and the chat

## v4.3.2

- lookups by key are faster: a program keeps in memory the last index it found to match its table, and reads the record once (on the Mac, `ITEM @{k} path` went from 70 to 40 µs; SPEC §20.5)
  - *Why:* Every lookup re-read the index's header and read the record twice: about seven file operations where two are needed
- the IDE's header fits any window: the path shrinks first, then the least used buttons fold into a ⋯ menu (Assembly, optimize, New folder, Open folder…, New), then the project's name shrinks
  - *Why:* Below about 1150 px (1230 px in the app) the buttons ran past the window and the page scrolled sideways
- Learn: chapter 12, automatic keys, and chapter 13, modeling data (the model as the schema, relations by key, joins by lookup, changing a model, what is not there yet)
  - *Why:* Nothing explained how Nihil models data, and the automatic keys of v4.3.0 had no example
- `bench/`: how fast tables are, for 10 000 to 1 000 000 records, and the page [Performance](https://nihil-lang.dev/performance.html) with the numbers on Linux and macOS
  - *Why:* The costs were described in SPEC §20 but never measured

## v4.3.1

- `install.sh` runs inside `main()`, called on its last line, so a download cut short runs nothing; and it refuses a `NIHIL_HOME` that is empty, `/`, your home directory, or an existing directory that is not a Nihil installation
  - *Why:* The installer replaces `NIHIL_HOME` as a whole: `NIHIL_HOME=~/projects` would have wiped that directory, and a partial script could run half of it
- the packages of every release (and of `nightly`) carry a build provenance attestation from GitHub Actions: `gh attestation verify nihil-linux-x86_64.tar.gz --repo nihillang/nihil`
  - *Why:* The `.sha256` comes from the same place as the package: it catches a corrupt download, not a replaced one
- the IDE: the Claude panel and the side bar stop where the code (editor and terminal) would get narrower than 360px; a narrower window shrinks them, the Claude panel first, and the widths chosen come back when it is wide again
  - *Why:* The Claude panel could be dragged until the editor and the terminal were drawn over it, and a narrower window made the page scroll sideways

## v4.3.0

- automatic keys: `#+id: INT(w)` in a model makes `@{}>>` give a record whose key is 0 the next number (1, 2, 3, …), kept in `path.nihil-seq`; a deleted number is never given again, and a key written by hand past the counter moves it (SPEC §20.5)
  - *Why:* Tables with a key had to compute the next number in every program (`#rows + 1`, which repeats a number after a deletion); the counter belongs to the table
- the IDE's Database area: a table can be filtered and sorted with a Nihil query (`where(…)`, `sort(…)`), and its records inserted, changed (double click on a cell) and deleted, through the language's own table operations; an automatic key is left blank on insert
  - *Why:* A table could only be looked at: changing one record, or finding it, meant writing a program
- the IDE's own right-click menus: in the editor, the terminal, the file tree (open, run, rename, duplicate, delete, copy path, new file or folder) and the Database area; a selection can be taken to Claude (right-click or Cmd+L)
  - *Why:* The browser's menu offered nothing about the IDE's files and tables
- `Nihil IDE.app`: no title bar (the IDE's header is the top of the window) and one menu
  - *Why:* The window looked like a browser's, with a bar and menus that did nothing for the IDE
- `nihil ide` starts the IDE's server in the background and gives the terminal back (log in `~/Nihil/.nihil-ide/server.log`); `nihil ide stop` stops it, `nihil ide --foreground` keeps it in the terminal
  - *Why:* `nihil ide` held the terminal for as long as the IDE was open
- building from source on Linux needs only `make` and `curl`: the runtime is kept assembled in `src1/runtime_bin.nihil`, and without `nasm` or `ld`, stage 1 starts from the latest release's compiler, which gives the same bytes as stage 0 (`make seedcheck`)
  - *Why:* Building Nihil needed an assembler, though only the frozen stage 0 and the runtime are in assembly
- GitHub builds the packages on every push (kept with the run in Actions) and the pre-release `nightly` from `main`: `curl -fsSL https://nihil-lang.dev/install.sh | NIHIL_VERSION=nightly sh`
  - *Why:* Trying a change before a release meant building it by hand

## v4.2.4

- Linux: the installer adds a "Nihil IDE" menu entry with the Nihil icon; `nihil ide` opens the IDE in a browser profile of its own (window class `NihilIDE`), so the taskbar and Alt+Tab show the Nihil icon, and closing the window stops the server
  - *Why:* On Linux the IDE's window showed the browser's icon and needed a terminal

## v4.2.3

- the IDE's icons are made small (64 colors, no 1024-pixel size): the macOS package goes from 880 KB to about 400 KB
  - *Why:* The icons were most of the package's size

## v4.2.2

- macOS: `Nihil IDE.app`, the IDE in a native window with the Nihil icon; the installer puts a copy in `~/Applications` (Launchpad, Spotlight), and the app starts the IDE's server itself when it is not running
  - *Why:* The app window of v4.2.1 was a browser's: it showed the browser's icon in the Dock and in Cmd+Tab, and it needed a terminal
- the IDE's page can be installed as an app from the browser (a web manifest and icons)
  - *Why:* On Linux too the IDE gets a window and an icon of its own

## v4.2.1

- `nihil ide`: the IDE comes with the packages and opens in an app window; its workspace is `~/Nihil`, with the examples
  - *Why:* An installed Nihil had only the compiler: the IDE ran only from a clone of the repository
- the `nihil` command (`nihil PROGRAM.nihil`, `nihil run`, `nihil version`), packages for Linux x86_64 and macOS arm64, and `install.sh`
  - *Why:* A Mac could not install Nihil without building it on Linux first

## v4.2.0

- the standard library: `@ "std:name.nihil";` (§16), kept inside the compiler
  - *Why:* Libraries every program needs (configuration, JSON, MCP) were copied into each project; now they ship with the language

## v4.1.0

- inclusions compared by their normalized path (`.`, empty segments and `dir/..` removed)
  - *Why:* A program and a server that share files reached each one by a different relative path (`api/../services/../models/x.nihil`, `database/../models/x.nihil`): the same file was included twice and its models clashed

## v4.0.0

- protected calls `?f()` and `?!`; `x.to_json()`, `MODEL.from_json(text)`, `MODEL.from_json_items(text)`; `tools("prefix")` and `call_tool("prefix", name, arguments)`
  - *Why:* The planogram's MCP server showed what a server in Nihil lacks (docs/v4.0-mcp.md): one bad request ended the whole server, every answer was JSON assembled by hand, and every tool was declared twice (its method and its schema). A runtime error can now be a missing value, any value can be written as JSON and read back into a record, and the tools come from the methods themselves

## v3.12.0

- `c.read()`, `url.http_open(method, type, body)`
  - *Why:* Showing an answer while it is written: a local model takes a minute for a long answer, and the screen should not stay still. The headers are read one byte at a time, so nothing of the body is taken before the program reads it

## v3.11.0

- the client side of the network: `host.connect(port)`, `url.http_get()`, `http_post`, `http_request`, `resp.http_status()`
  - *Why:* A program can now ask other servers (an LLM served by Ollama, another planogram server) instead of only answering. The first use is an agent in Nihil that talks to a local model and calls the planogram's tools in the same process

## v3.10.0

- `req.http_header(name)`, `req.http_host_is_local()`; `respond` no longer sends `Access-Control-Allow-Origin: *`; the IDE checks the host and a secret token, refuses hidden folders as projects, control bytes in paths, and serves only `.nihil` files; a path with a zero byte inside is refused everywhere (`@`, `@?`, `@>>`, `read_at`, the directory functions, `cmd.start`); accepted connections have a read deadline of 15 s; the IDE's programs run in `build/runs`, at most 8 at a time
  - *Why:* A security review found that any page open in the browser could use the IDE through the SSH tunnel (read keys, write and run a program) and read the planogram's data: the `*` let other sites read the answers, and nothing checked who was asking. A server for one machine now answers only to it and to its own page

## v3.9.0

- methods of a model: `MODEL.name(self = MODEL(), …) { … }`, called as `record.name(…)`
  - *Why:* Programs organized around their data (a planogram, its shelves and items) read better when the operations live with the model. The `.` already called the library on a value; extending it to the program's own models needs no keyword and no runtime dispatch. Inheritance is left out: composition covers it and keeps the language small

## v3.8.0

- directories and processes: `path.list()`, `is_dir`, `is_file`, `make_dir`, `remove`, `size`, `read_from`; `cmd.start(out, err, dir)`, `run`; `p.write`, `close_input`, `done`, `wait`, `kill`
  - *Why:* Tools around the language (the IDE, data collection) needed to list folders and run programs. One kernel-call routine in the runtime and the rest in the prelude keeps the assembly small and the library testable; the IDE server moved from Python to Nihil

## v3.7.0

- `contains`, `starts_with`, `ends_with` on STRING, ARRAY and MAP (the old `has`, `starts`, `ends` kept as synonyms); `upper`/`lower` change the Latin-1 letters too; the network gives `_` or `false` instead of `-errno`; faster reading: text → FLOAT by Clinger's fast path, native copies and JSON scanning, `@` of files with no size (`/proc`, pipes)
  - *Why:* Names that other languages' libraries use, one way to report a failure across the library (optionals, not error codes), and upper case that works for Portuguese, Spanish, French and German text. Reading tables of decimals went from 14 to about 100 MB/s and JSON from 23 to 90 MB/s: the exact big-integer conversion stays for the rare numbers with more than 15 digits or a power of ten beyond 22

## v3.6.0

- a TCP server and HTTP: `port.listen()`, `accept`, `receive`, `respond`, `http_path`, `http_param`
  - *Why:* Serving analyses as JSON from one place, instead of one generated page per analysis. Calls on INT keep the language free of reserved words, as the library does

## v3.5.0

- JSON as text: `s.json(key)`, `json_items`, `json_keys`, `json_values`, `json_text`, `json_quote`
  - *Why:* Importing and serving data needs JSON. Keeping each value as its text needs no dynamic type: the compiler still knows every type, and the existing `.int()`/`.float()` convert numbers

## v3.4.0

- `MAP of K to V`: `{k: v}`, `m[k]`, `m[k] = v`, `m[k] = _`, `#m`, `has`, `keys`, `values`
  - *Why:* Grouping and adding up by key (sales by product, store and month) needed a table in memory; the one on disk syncs every write. Braces after `=` or `(` never started an expression before, and `m[k] = _` deletes as `NAME @{k} path = _` does

## v3.3.0

- queries `rows.where(cond)`, `count`, `first`, `sort(key)`, `sum(expr)`
  - *Why:* Tables could be read but not searched or ordered by a field. Field names inside the call need no new symbol and no function values; compiling the loop in place lets the condition use the program's locals

## v3.2.0

- only the prelude methods a program reaches are compiled
  - *Why:* With the library, every program that used FLOAT, records or `.` carried all of the prelude: `x = 1.5; >> x;` was 66 KB, now 20 KB. The passes already redo everything, so the set of methods is found like a type: from the calls of one pass, for the next

## v3.1.0

- `ARRAY + ARRAY`
  - *Why:* Building a list of unknown length meant counting first and filling after. `+` already joined STRINGs, which are arrays of bytes

## v3.0.0

- the library `value.name(…)`, and `$` of INT, FLOAT and BOOL
  - *Why:* Every program was rewriting the same text and array routines, and a number could not be turned into text. Names after a `.` cannot clash with the program's names, so the language still has no reserved words

## v2.9.0

- a garbage collector (conservative mark and sweep)
  - *Why:* Programs that run for a long time or make many temporary texts grew without limit: a loop of 3 million small texts and arrays used 633 MB, now 19 MB. Conservative, so the compilers need no change and keep producing identical bytes

## v2.8.0

- `NAME @{k} path = _;` deletes by key
  - *Why:* Completes the key-value store. The record "becomes nothing": `_` already means no value. Moving the last record into the hole keeps the table a plain file without deleted lines, which every other read would have to skip

## v2.7.0

- key fields `#name`, `NAME @{k} path`, `@{}>> path, r;`
  - *Why:* A key-value store: finding a record by its key without reading the table. The index lives in its own file as a cache that is rebuilt when it does not match the table, so the table stays a plain text file that any write or editor may change. `#` reads as "number"; `{}` after `@` was never valid

## v2.6.0

- `@[i]>> path, r;`
  - *Why:* A key-value store needs to change one record without rewriting the table. With fixed widths the line's position is known, so one `pwrite` replaces it. `@[i]` already meant "record i" and `>>` "write"; a command never started with `@[` before

## v2.5.0

- `NAME @[i] path`, `NAME @?[i] path`, `NAME @# path`
  - *Why:* Reading one record should not cost reading the whole table: with fixed widths its position is known, and `pread` reads only that line. The count comes from the file's size, which also detects a file cut in the middle of a record

## v2.4.0

- data models `NAME = @[field: TYPE(width); …];`, records, `r.field`, `NAME @ path`
  - *Why:* Programs needed structured data that matches a file layout. Fixed widths make a file a table where record *n* has a known position, which a key-value store will need; names without quotes let the compiler check every field. `@[` keeps the "file" meaning of `@`; `.` after a value was never valid before

## v2.3.0

- `@>> path, text;` and `@+>> path, text;` write files
  - *Why:* Programs could read files but not save anything. The first step towards storing data: atomic replace (temporary + `fsync` + `rename`) so a crash never leaves half a file, and a durable append for logs. `@` already meant "file" and `>>` "write"; neither combination was valid before

## v2.2.0

- renamed from Ket to Nihil
  - *Why:* Latin for *nothing*: no keywords, and `_` for the missing value

## v2.1.0

- `... ms;` waits
  - *Why:* Animations need a fixed speed; a busy loop depends on the machine. The ellipsis reads as a pause and cannot start any valid expression

## v2.0.0

- renamed from Cru to Ket, and the whole project moved to English
  - *Why:* Ket came from Dirac's ket notation: a state written with symbols only, like a language without keywords

## v1.9.0

- optimizations by rewriting the code already emitted, with `-O0` for the bytes of stage 0
  - *Why:* Keeps the generator simple (a stack machine) and keeps the equivalence with stage 0
- saved registers learned between passes
  - *Why:* The prologue comes before the body; using the count from the previous pass avoids saving registers for nothing

## v1.8.0

- FLOAT as bits in `rax`
  - *Why:* The stack machine keeps the same registers; SSE2 is used only for the arithmetic
- `.n` and `#f` as conversions
  - *Why:* No keyword and no implicit conversion
- a prelude written in the language
  - *Why:* Converting `double` ⇄ text exactly needs big integers; writing them in the language (not in assembly) makes them testable and reusable by the compiler
- with a provisional side, `+ - * /` give a provisional result (not INT)
  - *Why:* With FLOAT and STRING `+`, assuming INT locked wrong return types in the first pass

## v1.7.0

- every optional is value + flag (STRING? and ARRAY? too)
  - *Why:* A single representation keeps assignment, arguments, returns and fallback simple; null pointers are an optimization
- a T return with `: _;` becomes T?
  - *Why:* Search methods become natural (`: i;` … `: _;`) with no type annotation
- `@?`
  - *Why:* Reading a file that may fail without ending the program

## v1.6.0

- `::`, `!>>`, `@ path`, `$ bytes`, `@ "file";`
  - *Why:* The compiler written in the language needed to exit from inside methods, write errors to stderr, read files and build paths. All of them use symbols that were invalid before, with no keyword

## v1.5.0

- `[0 ; n]` does not fill the memory
  - *Why:* A new heap block comes zeroed from the kernel. Without this, the 16 MiB buffers of the compiler would touch hundreds of MB

## v1.4.0

- an ARRAY parameter's default is a one-element literal, new on every call
  - *Why:* A shared constant array would be changed by one call and seen by the next
- `name[…]` is an assignment only if the `]` is followed by `=`
  - *Why:* `flags[i] { … }` is a valid conditional

## v1.3.0

- a parameter may have a global's name
  - *Why:* v1.0's own example (`max(a = 0, b = 0)` with a global `b`) would be an error. A parameter is declared explicitly, so there is no ambiguity
- `even(4) {` at the top level is a call, not a declaration
  - *Why:* The rule "`IDENT(…) {` at the top level is always a declaration" prevented using a method as a condition there
- parameter defaults are literals
  - *Why:* The default is put into every call that omits it; arbitrary expressions would have to be evaluated in the method's context
- a method with a value and no `:` at the end is a runtime error
  - *Why:* Returning garbage in `rax` would be silent; detecting it at compile time needs flow analysis

## v1.2.0

- comparisons below the bit operators in the precedence table
  - *Why:* v1.1 said the bit operators were above, but the table and grammar put them below, so `a & 1 == 0` became `a & (1 == 0)`
