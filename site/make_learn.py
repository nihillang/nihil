import html, os, subprocess, sys
# site/make_learn.py [NIHIL]: runs every program of examples/learn and writes site/learn.html with
# each one and the output it printed, so the page never shows an example that does not work.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
L = os.path.join(ROOT, "examples", "learn"); NIHIL = sys.argv[1] if len(sys.argv) > 1 else "nihil"; OUT = os.path.join(ROOT, "site", "learn.html")
chapters = [
 ("01_hello", "Hello", "Install Nihil (see the <a href=\"index.html#install\">home page</a>), save this as <code>hello.nihil</code> and run <code>nihil run hello.nihil</code>. <code>&gt;&gt;</code> writes to the standard output and adds no line break of its own. <code>nihil hello.nihil</code> compiles it into a native executable, <code>./hello</code>, instead."),
 ("02_values", "Values and types", "There are no declarations: a variable is created by its first assignment, and its type comes from the value and stays. The four basic types are INT, FLOAT, BOOL and STRING. Nothing converts implicitly: <code>$</code> makes text, <code>.n</code> turns an INT into a FLOAT, <code>#f</code> a FLOAT into an INT (and <code>#</code> of a STRING or an ARRAY is its length)."),
 ("03_control", "Conditions and loops", "There are no keywords: the expression before a <code>{</code> decides what the block does. A BOOL makes it an if, and <code>} || … {</code> continues as else if or else. An INT repeats the block that many times. A <code>*</code> before a BOOL makes it a while loop."),
 ("04_methods", "Methods", "A method is a name, parameters and a block. Every parameter has a default, which gives its type, so calls may leave arguments out. <code>:</code> returns a value, and the type of the result is worked out from what is returned."),
 ("05_arrays", "Arrays, text and the library", "An ARRAY holds values of one type; <code>[0 ; n]</code> makes <code>n</code> copies. The library is called on a value after a <code>.</code>, so its names never take one from your program. <code>$</code> of an ARRAY of INT makes text from its bytes, which is why <code>show</code> builds the text itself."),
 ("06_optionals", "Optionals: the value that is not there", "<code>_</code> is the absence of a value. A variable created with a <code>?</code> may hold it; an ordinary one never does. The only way to take the value out is <code>||</code> with a fallback, so a missing value can never be used by accident."),
 ("07_maps", "Maps", "A MAP holds values by key, in memory. Reading a key gives an optional, because the key may not be there; assigning <code>_</code> deletes it."),
 ("08_files", "Files and input", "<code>@</code> reads a whole file, <code>@&gt;&gt;</code> replaces it atomically and <code>@+&gt;&gt;</code> appends to it. <code>@?</code> gives <code>_</code> instead of an error when the file is not there. <code>&lt;&lt;</code> reads a line from the input into a variable of any basic type. (The output below is with <code>Ana</code> typed as the input.)"),
 ("09_tables", "Tables on disk", "A data model describes a record of fixed width; <code>#</code> marks its key. A table is a plain text file, one record per line, with a hash index on disk for reading, writing and deleting by key. Queries like <code>where</code>, <code>sort</code> and <code>sum</code> take an expression of the fields."),
 ("10_std", "The standard library", "Libraries that ship with the compiler are included with <code>@ \"std:name.nihil\";</code>: <code>std:env.nihil</code> (configuration from <code>.env</code> and the environment), <code>std:json.nihil</code> (writing JSON) and <code>std:mcp.nihil</code> (an MCP server for AI agents). Reading JSON is built into the library: a JSON value stays text, and each call takes a part of it."),
 ("11_globals", "Globals and locals: the one rule to remember", "Variables assigned at the top level are <b>globals</b>, and a name that belongs to a global always means that global, even inside a method: there, <code>total = …</code> changes the global instead of creating a local. Give locals names no global uses. Only a parameter may share a global's name, and then it hides the global inside its method."),
 ("12_auto_keys", "Automatic keys", "A key marked <code>#+</code> is numbered by the table: write a record with key 0 by key (<code>@{}&gt;&gt;</code>) and it gets the next number, which the record then holds. The counter is kept next to the table, so a deleted number is never given again and a second run goes on from where the first stopped. In the IDE, <code>orders.dat</code> then appears under Database, where its records can be filtered, added, changed and deleted."),
]
toc = []; body = []
for slug, title, intro in chapters:
    src = open(os.path.join(L, slug + ".nihil")).read().rstrip("\n")
    for junk in ("products.dat", "products.dat.nihil-idx", "notes.txt", "orders.dat", "orders.dat.nihil-idx", "orders.dat.nihil-seq"):
        try: os.remove(os.path.join(L, junk))
        except FileNotFoundError: pass
    r = subprocess.run([NIHIL, "run", slug + ".nihil"], cwd=L, input="Ana\n", capture_output=True, text=True)
    if r.returncode != 0: sys.exit(f"{slug} failed: {r.stderr}")
    anchor = slug[3:]
    toc.append(f'<li><a href="#{anchor}">{title}</a></li>')
    body.append(f'<h2 id="{anchor}">{len(toc)}. {title}</h2>\n<p>{intro}</p>\n<pre class="nihil">{html.escape(src)}</pre>\n<p class="out">Output:</p>\n<pre class="output">{html.escape(r.stdout.rstrip(chr(10)))}</pre>')
page = f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Learn Nihil</title>
<meta name="description" content="Nihil in twelve short chapters: values, control without keywords, methods, arrays, optionals, maps, files, tables on disk, automatic keys and the standard library.">
<link rel="icon" type="image/png" href="icon-64.png">
<link rel="stylesheet" href="style.css">
</head>
<body>
<nav class="top"><div class="wrap">
  <a class="brand" href="index.html"><picture><source media="(prefers-color-scheme: dark)" srcset="icon-dark-64.png"><img src="icon-64.png" alt=""></picture>Nihil</a>
  <a class="here" href="learn.html">Learn</a>
  <a href="library.html">Library</a>
  <a href="https://github.com/nihillang">GitHub</a>
</div></nav>
<main class="doc"><div class="wrap">
<h1>Learn Nihil</h1>
<p>Twelve short chapters. Every example is a complete program, and the output under it is what it prints.
The full reference is <a href="https://github.com/nihillang/nihil/blob/main/SPEC.md">SPEC.md</a>.</p>
<ol class="toc">
{chr(10).join(toc)}
</ol>
{chr(10).join(body)}
<h2 id="next">Next</h2>
<p>The <a href="library.html">library reference</a> lists every call on texts, arrays, tables, JSON and maps.
Questions: <a href="mailto:contact@nihil-lang.dev">contact@nihil-lang.dev</a>.</p>
</div></main>
<footer><div class="wrap row"><span>Nihil · MIT License</span><span><a href="index.html">nihil-lang.dev</a></span></div></footer>
<script src="nihil.js"></script>
</body>
</html>
'''
open(OUT, "w").write(page)
print("ok", len(chapters), "chapters")
