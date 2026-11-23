# Nihil — stage 0 (assembly) and stage 1 (Nihil)
NASM := $(shell command -v nasm 2>/dev/null || echo $(HOME)/.local/bin/nasm)
NASMFLAGS := -w+all -w-reloc-rel-dword -w-reloc-abs-qword -I src/ -I build/
SOURCES1 := $(sort $(wildcard src1/*.nihil))
comma := ,
# stage 0 needs nasm and ld; without them, stage 1 starts from the compiler of the latest release
STAGE0 := $(and $(wildcard $(NASM)),$(shell command -v ld 2>/dev/null))

ifneq ($(STAGE0),)
all: build/nihilc build/nihilc1
else
all: build/nihilc1
endif

# ---- stage 0 ----
build/nihilc: build/nihilc.o
	ld -o $@ $<

build/nihilc.o: src/nihilc.asm src/commands.inc src/layout.inc build/runtime.bin
	$(NASM) -f elf64 -g -F dwarf $(NASMFLAGS) -o $@ src/nihilc.asm

build/runtime.bin: src/runtime.asm src/layout.inc
	@mkdir -p build
	$(NASM) -f bin $(NASMFLAGS) -o $@ src/runtime.asm

# ---- stage 1: the compiler written in Nihil ----
# the runtime's bytes, which stage 1 copies into every executable, are kept in the repository
# (src1/runtime_bin.nihil), so stage 1 builds without nasm; after a change to src/runtime.asm,
# `make runtime` writes them again, and `make runtime-check` (CI) checks they are up to date
RUNTIME_TEXT = { printf '// generated from src/runtime.asm by `make runtime` (nasm) — do not edit; `make runtime-check` compares\nruntime_bin = ['; \
	  od -An -v -tu1 build/runtime.bin | tr -s ' \n' '  ' | sed 's/^ //; s/ $$//; s/ /, /g'; printf '\n];\n'; }

runtime: build/runtime.bin
	$(RUNTIME_TEXT) > src1/runtime_bin.nihil

runtime-check: build/runtime.bin
	$(RUNTIME_TEXT) > build/runtime_bin.nihil
	@cmp build/runtime_bin.nihil src1/runtime_bin.nihil || { echo "src1/runtime_bin.nihil is not src/runtime.asm's: make runtime" >&2; exit 1; }

# the prelude (bigint + float_text + prelude) becomes a STRING inside nihilc1
PRELUDE := src1/bigint.nihil src1/float_text.nihil src1/prelude.nihil
build/prelude_text.nihil: $(PRELUDE)
	@mkdir -p build
	{ printf '// generated from $(PRELUDE) — do not edit\nPRELUDE_TEXT = "'; \
	  cat $(PRELUDE) | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | awk '{ printf "%s\\n", $$0 }'; \
	  printf '";\n'; } > $@

# the standard library (std/*.nihil, SPEC §16): each file becomes a STRING inside nihilc1,
# included by programs as @ "std:name.nihil";
STD := $(sort $(wildcard std/*.nihil))
build/std_text.nihil: $(STD)
	@mkdir -p build
	{ printf '// generated from std/*.nihil — do not edit\nSTD_LIST = "%s";\nstd_text(name = "") {\n' "$(subst $(eval) ,$(comma) ,$(notdir $(STD)))"; \
	  for f in $(STD); do \
	    printf '    name == "%s" { : "' "$$(basename $$f)"; \
	    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' $$f | awk '{ printf "%s\\n", $$0 }'; \
	    printf '"; }\n'; \
	  done; \
	  printf '    : "";\n}\n'; } > $@

STAGE1_INPUTS := build/prelude_text.nihil build/std_text.nihil $(SOURCES1)

ifneq ($(STAGE0),)
build/nihilc1: build/nihilc $(STAGE1_INPUTS)
	build/nihilc src1/nihilc1.nihil $@
else
build/nihilc1: build/nihilc1-seed
	cp $< $@
endif

# from a seed: the latest release's compiler (dist/seed.sh) compiles src1, and that compiler
# compiles it again without optimizations, which gives the bytes stage 0 gives (see fixedpoint)
build/seed/nihilc1:
	dist/seed.sh build/seed

build/nihilc1-seed: build/seed/nihilc1 $(STAGE1_INPUTS)
	printf '%s\n' src1/nihilc1.nihil | build/seed/nihilc1 > build/seed/nihilc1-new
	chmod 755 build/seed/nihilc1-new
	printf '%s\n' '-O0 src1/nihilc1.nihil' | build/seed/nihilc1-new > $@.new
	chmod 755 $@.new && mv $@.new $@

# stage 1 from the seed is the same compiler as stage 1 from stage 0
seedcheck: build/nihilc build/nihilc1-seed
	build/nihilc src1/nihilc1.nihil build/nihilc1-stage0
	cmp build/nihilc1-stage0 build/nihilc1-seed
	@echo "the seed gives the same stage 1 as stage 0"

# ---- tests ----
test: build/nihilc
	@./tests/run.sh

test1: build/nihilc1
	@NIHILC=./nihilc1.sh DIRS="tests tests/stage1" ./tests/run.sh

# the whole suite through nihilc1 without optimizations
test1-O0: build/nihilc1
	@NIHIL_O0=1 NIHILC=./nihilc1.sh DIRS="tests tests/stage1" ./tests/run.sh

# every test program compiled by both stages must give identical bytes
compare: build/nihilc build/nihilc1
	@./tests/compare.sh

# fixed point:
#  1. without optimizations, nihilc1 compiles itself into the binary nihilc produced
#  2. optimized: nihilc2 (nihilc1 by itself) = nihilc3 (nihilc2 by itself)
#  3. nihilc2 (optimized) without optimizations produces exactly nihilc1 again
fixedpoint: build/nihilc1
	NIHIL_O0=1 ./nihilc1.sh src1/nihilc1.nihil build/nihilc1-O0
	cmp build/nihilc1 build/nihilc1-O0
	./nihilc1.sh src1/nihilc1.nihil build/nihilc2
	NIHILC1=build/nihilc2 ./nihilc1.sh src1/nihilc1.nihil build/nihilc3
	cmp build/nihilc2 build/nihilc3
	NIHIL_O0=1 NIHILC1=build/nihilc2 ./nihilc1.sh src1/nihilc1.nihil build/nihilc1-O0b
	cmp build/nihilc1 build/nihilc1-O0b
	@echo "fixed point: nihilc1 = nihilc1 -O0 ($$(stat -c %s build/nihilc1) bytes); nihilc2 = nihilc3 ($$(stat -c %s build/nihilc2) bytes, optimized)"

# FLOAT ⇄ text against Python: every number reads back the same, is the shortest, and parsing is exact
floatcheck: build/nihilc
	build/nihilc tests/float/conversion_check.nihil build/conversion_check
	python3 tests/float/check.py build/conversion_check 100000

# the collector under pressure: a copy whose runtime collects every 256 KiB,
# whatever is in use, runs the stage 1 suite, the fixed point and floatcheck
gcstress:
	rm -rf build/stress && mkdir -p build/stress
	cp -r src src1 std tests Makefile nihilc1.sh build/stress/
	$(MAKE) -C build/stress NASMFLAGS="$(NASMFLAGS) -DGC_STRESS" runtime
	$(MAKE) -C build/stress NASMFLAGS="$(NASMFLAGS) -DGC_STRESS" test1 fixedpoint floatcheck

# the IDE server, written in Nihil (SPEC §24): ./build/ide-server, then http://127.0.0.1:8730
ide: build/ide-server

build/ide-server: build/nihilc1 ide/server.nihil
	./nihilc1.sh ide/server.nihil $@.new && mv $@.new $@     # the running server keeps its copy

clean:
	rm -rf build

.PHONY: all runtime runtime-check seedcheck test test1 test1-O0 compare fixedpoint floatcheck gcstress ide clean
