# Changelog

What each version of the language added, and why. Appendix A of
[SPEC.md](SPEC.md) keeps the same history together with the design decisions made
before v2.0.

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
