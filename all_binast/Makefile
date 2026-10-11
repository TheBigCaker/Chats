# binast -- pure-C raw-binary toolchain.
#
# CC is a variable, never hardcoded:
#   make CC=clang     (bring-up on this box)
#   make CC=baccyn    (the port's real pipeline; drygon is NOT used here)
#
# No -lm anywhere: every float helper comes from cygemm/snumblits.h. A link error on
# log2 means a libm call snuck back in -- fix the call site, do not add the flag.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
# The catalogs are brace initializers written before the per-operand REX windows existed.
# C zero-initializes the absent fields, which is exactly the intent, so the warning is noise
# -- but it fires 13,200 times, once per row, and hides real ones.
CATFLAGS = -Wno-missing-field-initializers
# NOT named PREFIX: Termux exports PREFIX=/data/data/com.termux/files/usr, so a
# variable with that name is silently overridden and install lands in the system
# bin instead of the documented location. ?= does not protect against the environment.
BINDIR  ?= $(HOME)/binast

# ccp_logf lives in the port's cygemm tree. Point CYM at it rather than keeping a
# copy here: one source of truth, and no stale duplicate to drift.
CYM     ?= $(HOME)/CCP-C-Cython-Python
CFLAGS  += -I$(CYM)

BIN   = binast isadb blink cosmetic binui isamap binast_ent binast_hold binast_norm binastmodel binastfire binastpage arbittest smmbtest binastrewrite binastfreq binastcure curetest binastbuild binastconv convtest x86verify x86walk x86cov x86td x86fixt
# every hand-written C source in the tree, for the allocator/threading greps. Kept as a
# list rather than a wildcard so a NEW file is a deliberate addition to the check.
SRC   = binast.c isadb.c blink.c cosmetic.c binui.c isamap.c binast_ent.c binast_hold.c \
        binast_norm.c binastmodel.c binastfire.c binastpage.c arbittest.c smmbtest.c \
        binastrewrite.c binastfreq.c binastcure.c curetest.c binastbuild.c binastconv.c convtest.c \
        x86verify.c mkx86.c x86walk.c x86td.c x86fixt.c \
        arbishim.h arbitcore.h elfobj.h relink.h lopimm.h isadb.h

all: $(BIN)

# ---- the x86-64 fixtures -------------------------------------------------------------------------
# THE FIXTURES ARE GENERATED, NOT WRITTEN. Each one is an encoding llvm-mc produced, paired with the
# row the catalog currently chooses for it -- so the check stays meaningful when the catalog changes,
# which a hand-written list of row names cannot do (all 116 of the old ones named rows that no longer
# existed once the catalog stopped being 73 hand-written rows).
fixtures: x86fixt
	./x86fixt > isadb_fixtures_x86.h
	@echo "   isadb_fixtures_x86.h regenerated from llvm-mc"

# THE x86-64 CATALOG IS GENERATED FROM THE LOCAL .td ISA MAP, NOT MAINTAINED BY HAND.
# `x86td` reads ~/ISA_MAPS/llvm_isa_tables/X86/*.td and derives every row: the Format gives the
# ModR/M layout, the class gives the prefix, the opcode is literal. Regenerating it is a build step,
# so the catalog cannot drift from the tables it claims to come from.
catalog: x86td
	./x86td emit > ISA_X86_64.inc
	@echo "   ISA_X86_64.inc regenerated from the .td tables"

x86fixt: x86fixt.c isadb.h ISA_X86_64.inc
	$(CC) $(CFLAGS) -o $@ $<

x86td: x86td.c isadb.h
	$(CC) $(CFLAGS) -o $@ $<

binast:  binast.c $(CYM)/cygemm/snumblits.h
	$(CC) $(CFLAGS) -o $@ binast.c

isadb:   isadb.c isadb.h isadb_fixtures.h isadb_fixtures_x86.h ISA_AARCH64.inc ISA_X86_64.inc lopimm.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ isadb.c

blink:   blink.c isadb.h relink.h elfobj.h ISA_AARCH64.inc
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ blink.c

cosmetic: cosmetic.c
	$(CC) $(CFLAGS) -o $@ cosmetic.c

binui:   binui.c isadb.h ISA_AARCH64.inc ISA_X86_64.inc
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ binui.c

isamap:  isamap.c isadb.h ISA_AARCH64.inc ISA_X86_64.inc lopimm.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ isamap.c

binast_ent: binast_ent.c $(CYM)/cygemm/snumblits.h
	$(CC) $(CFLAGS) -o $@ binast_ent.c

binast_hold: binast_hold.c $(CYM)/cygemm/snumblits.h
	$(CC) $(CFLAGS) -o $@ binast_hold.c

binast_norm: binast_norm.c isadb.h relink.h ISA_AARCH64.inc lopimm.h
	$(CC) $(CFLAGS) -I. -o $@ binast_norm.c $(CATFLAGS)

binastmodel: binastmodel.c
	$(CC) $(CFLAGS) $(CATFLAGS) -o $@ binastmodel.c

binastfire: binastfire.c isadb.h ISA_AARCH64.inc lopimm.h arbitcore.h arbishim.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ binastfire.c

# arbit + partitricks, exercised on their own before any tool leans on them.
# A header that is only ever compiled as part of something else is a header whose
# failures show up as a wrong number in a model. This runs them directly.
arbittest: arbittest.c arbitcore.h arbishim.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ arbittest.c

# Does smmb_gemm compute the FIRING, and smmb_outer_T the frozen-map gradient? Tested
# against a hand-computable catalog before any real map is put through it.
smmbtest: smmbtest.c
	$(CC) $(CFLAGS) $(CATFLAGS) -o $@ smmbtest.c

# Does the gradient rewrite work? Fires a program, predicts the next block, takes
# dA = dC @ W, and asks whether stepping the bits along dA moves toward the right word.
binastrewrite: binastrewrite.c arbitcore.h arbishim.h
	$(CC) $(CFLAGS) -I. -o $@ binastrewrite.c

# The program as a waveform: his FFT over the firing signal. Tests whether structure is
# periodic. It is not -- see the commit -- and having the negative result in the tree stops
# the frequency framing being re-proposed as if untested.
binastfreq: binastfreq.c arbitcore.h arbishim.h
	$(CC) $(CFLAGS) -I. -o $@ binastfreq.c

# repair: find damage and put it back. curetest injects KNOWN damage and demands
# byte-exact recovery, because a repairer shown only damage it caused proves nothing.
binastcure: binastcure.c isadb.h ISA_AARCH64.inc lopimm.h arbitcore.h arbishim.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ binastcure.c
curetest: curetest.c arbitcore.h arbishim.h
	$(CC) $(CFLAGS) -I. -o $@ curetest.c

# THE LAYOUT ENGINE: build a runnable program from catalog blocks. Depends on elfobj.h and
# relink.h, and that dependency is load-bearing -- without them in the rule `make` reports
# success while rebuilding nothing, which silently tested every fix against a stale binary.
binastbuild: binastbuild.c isadb.h ISA_AARCH64.inc lopimm.h elfobj.h relink.h arbitcore.h arbishim.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ binastbuild.c

# CONVERT: disassemble an ELF into editable blocks. Depends on ISA_AARCH64.inc (the rows)
# and isadb.h, and the dependency is load-bearing -- without it make reports success while
# rebuilding nothing.
binastconv: binastconv.c isadb.h ISA_AARCH64.inc arbitcore.h arbishim.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ binastconv.c

convtest: convtest.c arbitcore.h arbishim.h
	$(CC) $(CFLAGS) -I. -o $@ convtest.c

# x86-64: the catalog is GENERATED from llvm-mc ground truth, and this checks it against
# llvm-mc again. Both depend on ISA_X86_64.inc, and that dependency is load-bearing: without
# it a regenerated catalog is never recompiled into the checker.
# x86walk had NO RULE, so `make x86walk` silently did nothing and the binary kept an old
# catalog baked in -- it reported the same 12% coverage before and after the catalog grew from
# 366 to 608 rows, which is what gave the staleness away. The table dependency is load-bearing.
x86walk:  x86walk.c isadb.h ISA_X86_64.inc
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ x86walk.c

x86verify: x86verify.c isadb.h ISA_X86_64.inc
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ x86verify.c

mkx86: mkx86.c isadb.h x86batch.h
	$(CC) $(CFLAGS) -I. -o $@ mkx86.c

# x86td derives the catalog from the LLVM TableGen tables directly -- no subprocess per sample.
x86td: x86td.c isadb.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ x86td.c

# binary as a readable/writable page of building blocks: the direct interaction surface.
binastpage: binastpage.c isadb.h ISA_AARCH64.inc lopimm.h arbitcore.h arbishim.h
	$(CC) $(CFLAGS) $(CATFLAGS) -I. -o $@ binastpage.c

# ---------------------------------------------------------------- verification
# The gate. `test` is the deliverable, not a convenience: every claim in README.md is
# one of these lines, and a failing line means the toolchain is wrong, not the test.
test: all
	@mkdir -p tmp
	@echo "== smmb GEMM is the firing, and its backward is the frozen-map gradient"
	@./smmbtest | tail -3
	@./smmbtest > /dev/null || { echo "   FAIL: smmb kernel check failed"; exit 1; }
	@echo
	@echo "== the allocator and the thread pool work, on this device"
	@./arbittest || exit 1
	@echo
	@echo "== no libc allocation, no pthreads, in ANY built binary"
	@for b in $(BIN); do \
	   s=$$(nm -u $$b 2>/dev/null | grep -E '\\b(malloc|calloc|realloc|free|posix_memalign|strdup)$$' || true); \
	   [ -z "$$s" ] || { echo "   FAIL: $$b references the libc allocator:"; echo "$$s"; exit 1; }; \
	done
	@for b in $(BIN); do \
	   s=$$(nm -u $$b 2>/dev/null | grep -E 'pthread_' || true); \
	   [ -z "$$s" ] || { echo "   FAIL: $$b calls pthreads:"; echo "$$s"; exit 1; }; \
	done
	@echo "   no libc allocator, no pthread_* (arbit + partitricks in every program)"
	@if grep -nE '\b(malloc|calloc|realloc)[[:space:]]*\(' $(SRC) ; then \
	   echo "   FAIL: an alloc-family call is still present"; exit 1; \
	fi
	@if grep -nE '^[[:space:]]*#[[:space:]]*define[[:space:]]+(malloc|calloc|realloc|free)' $(SRC) ; then \
	   echo "   FAIL: an alloc-family macro is present"; exit 1; \
	fi
	@echo "   no malloc/calloc/realloc/free call or #define -- only arbit"
	@echo
	@echo "== x86-64: decode a real assembled program, every instruction"
	@mkdir -p tmp
	@llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o tmp/x86p.o tests/x86prog.s 2>/dev/null \
	   && ld.lld -o tmp/x86p.elf tmp/x86p.o 2>/dev/null \
	   || { echo "   FAIL: could not assemble tests/x86prog.s"; exit 1; }
	@./x86walk tmp/x86p.elf > tmp/x86walk.txt 2>&1 || true
	@tail -1 tmp/x86walk.txt | grep -q "100% covered" \
	   || { echo "   FAIL: the catalog did not decode the whole program:"; cat tmp/x86walk.txt; exit 1; }
	@tail -1 tmp/x86walk.txt
	@echo
	@echo "== x86-64: the generated catalog, checked against llvm-mc"
	@./x86verify \
	   || { echo "   FAIL: x86-64 catalog disagrees with llvm-mc"; exit 1; }
	@echo
	@echo "== ISA catalogs, checked against known-good encodings (ground truth)"
	@./isadb fixtures            || exit 1
	@./isadb -t x86_64 fixtures  || exit 1
	@echo
	@echo "== every row encodes, decodes and re-encodes to the same bytes"
	@./isadb test                || exit 1
	@./isadb -t x86_64 test      || exit 1
	@./isadb reject             || exit 1
	@./isadb -t x86_64 reject   || exit 1
	@echo
	@echo "== the catalog read as a bit matrix agrees with the catalog itself"
	@./isamap verify aarch64     || exit 1
	@./isamap verify x86_64      || exit 1
	@echo
	@echo "== relocation-aware tokenization: canonical form loses nothing"
	@./binast_norm verify tests/static.elf || exit 1
	@echo
	@echo "== decode a real compiled binary, then round-trip it"
	@./blink code tests/static.elf > tmp/dec.txt
	@tot=$$(wc -l < tmp/dec.txt); \
	 cov=$$(grep -vc 'no catalog form' tmp/dec.txt); \
	 nz=$$(grep 'no catalog form' tmp/dec.txt | awk '{print $$2}' | grep -vc '^00000000'); \
	 printf '   %s words, %s recognised, %s unrecognised non-zero\n' "$$tot" "$$cov" "$$nz"; \
	 [ "$$nz" = "0" ] || { echo "   FAIL: real instructions are not covered"; exit 1; }
	@echo
	@echo "== relink: move code and keep every PC-relative operand correct"
	@./blink relink tests/static.elf 0x4001c8 296 0x403000 tmp/rt.elf > tmp/relink.txt \
	   || { cat tmp/relink.txt; exit 1; }
	@cat tmp/relink.txt
	@cmp -l tests/static.elf tmp/rt.elf | \
	   awk '{o=$$1-1; if(!((o>=456&&o<752)||(o>=12288&&o<12584))) c++} END{ if(c) { print "   FAIL: "c" bytes changed outside the moved blocks"; exit 1 } print "   only the two relocated blocks differ" }'
	@echo
	@echo "== the relocated program still runs"
	@./blink run tmp/rt.elf > tmp/run.txt 2>&1; rc=$$?; cat tmp/run.txt; \
	 grep -q 'exited 0' tmp/run.txt || { echo "   FAIL: relocated binary did not exit 0"; exit 1; }
	@echo
	@echo "== agent interface answers without a terminal"
	@./binui tell aarch64 ADD_X_imm 0 1 42 | grep -q "bytes 20 a8 00 91" || { echo "   FAIL: tell gave the wrong encoding"; exit 1; }
	@./binui look aarch64 d2800540 | grep -q "form MOVZ_X" || { echo "   FAIL: look did not identify the word"; exit 1; }
	@./binui coverage tests/static.elf | grep -q "unmatched_nonzero 0" || { echo "   FAIL: coverage found unrecognised real instructions"; exit 1; }
	@./binui agent | grep -q "isa aarch64 forms" || { echo "   FAIL: manifest unreadable"; exit 1; }
	@echo "   tell / look / coverage / agent all answer"
	@echo
	@echo "== plant an instruction into a real binary, read it back, run it"
	@rm -f tmp/plant.elf
	@./blink patch tests/static.elf 0x4001c8 d2800540 tmp/plant.elf > /dev/null
	@./binui read tmp/plant.elf 4001c8 1 | grep -q "form MOVZ_X" || { echo "   FAIL: planted word not at the address"; exit 1; }
	@./blink run tmp/plant.elf | grep -q "exited 0" || { echo "   FAIL: modified binary did not run"; exit 1; }
	@echo "   planted word reads back at the address, binary still runs"
	@echo
	@echo "== layout: BUILD a runnable program from blocks, then run it"
	@rm -f tmp/hello.elf tmp/branch.elf
	@./binastbuild tests/hello.bld tmp/hello.elf 0x400000 | grep -E 'resolved|encoded|entry'
	@chmod +x tmp/hello.elf
	@out=$$(./tmp/hello.elf); [ "$$out" = "hi" ] \
	   || { echo "   FAIL: built program printed '$$out', expected 'hi'"; exit 1; }
	@echo "   built program ran and printed 'hi' (and exited 0)"
	@./binastbuild tests/branch.bld tmp/branch.elf 0x400000 > /dev/null
	@chmod +x tmp/branch.elf
	@out=$$(./tmp/branch.elf); [ "$$out" = "ok" ] \
	   || { echo "   FAIL: branch program printed '$$out', expected 'ok'"; exit 1; }
	@echo "   branch program ran and printed 'ok' (forward branch resolved)"
	@rm -f tmp/hello.elf tmp/branch.elf
	@echo
	@echo "== rewrite: A + dA through the real kernels, with a RANDOM PREDICTION control"
	@mkdir -p tmp
	@test -f tmp/isa_aarch64.mtx || ./isamap mtx tmp/isa_aarch64 aarch64 > /dev/null
	@test -f tmp/own_code.txt || ./binui words ./isadb > tmp/own_code.txt
	@# NOT `blink code <elf> --hex`: that dumps the whole file including the ELF header and
	@# strings, and a whole-file stream makes repeat-previous score ABOVE order-2 -- a
	@# symptom that reads like a finding. `binui words` takes only SHF_EXECINSTR sections.
	@./binastrewrite tmp/isa_aarch64.mtx tmp/own_code.txt 0.8 > tmp/rewrite.txt 2>&1 || \
	   { echo "   FAIL: binastrewrite did not run"; cat tmp/rewrite.txt; exit 1; }
	@grep -q "rand" tmp/rewrite.txt || { echo "   FAIL: the random control did not run"; exit 1; }
	@grep -q "RESULT_CONTROL PASS" tmp/rewrite.txt || { \
	   echo "   FAIL: the random-prediction control did not pass -- either a random"; \
	   echo "   prediction scored >= 1.0 (the ratio is an artifact of the stepping rule,"; \
	   echo "   not of the prediction), or the real predictor scored <= 1.0"; \
	   grep "RESULT" tmp/rewrite.txt; exit 1; }
	@grep "^RESULT " tmp/rewrite.txt | awk '{ printf "   random prediction ratio: %s (must be < 1.0)\n   real prediction ratio:   %s (must be > 1.0)\n", $$3, $$5 }'
	@echo "   control passed: a random prediction hurts, the real predictor helps"
	@echo
	@echo "== convert: disassemble a program to blocks, rebuild it, compare at ADDRESSES"
	@./convtest ./binastcure tmp \
	   || { echo "   FAIL: convert round trip is not byte-exact"; exit 1; }
	@echo
	@echo "== round-trip a REAL dynamic binary through its OWN container"
	@echo "   (convert -> blocks -> rebuild into the original image: must be byte-identical"
	@echo "    AND still run -- the code is right, the dynamic container is preserved)"
	@mkdir -p tmp/ct
	@test -f tmp/ct/good || { cp /data/data/com.termux/files/usr/bin/true tmp/ct/good; chmod +x tmp/ct/good; }
	@./binastconv tmp/ct/good tmp/ct/rt.bld > /dev/null \
	   || { echo "   FAIL: convert"; exit 1; }
	@./binastbuild tmp/ct/rt.bld tmp/ct/rt.out --into tmp/ct/good > /dev/null \
	   || { echo "   FAIL: rebuild --into"; exit 1; }
	@cmp -s tmp/ct/good tmp/ct/rt.out \
	   || { echo "   FAIL: the rebuilt binary is not byte-identical to the original"; exit 1; }
	@echo "   byte-identical to the original after a full convert + rebuild"
	@mkdir -p tmp/ct/run && cp tmp/ct/rt.out tmp/ct/run/true && chmod +x tmp/ct/run/true
	@out=$$(./tmp/ct/run/true --version 2>&1 | head -1); \
	 case "$$out" in *coreutils*) echo "   and it still runs: $$out";; \
	   *) echo "   FAIL: rebuilt binary printed '$$out'"; exit 1;; esac
	@echo
	@echo "== relocate: the SAME source at two bases must still run (this is what makes"
	@echo "   it a source and not a listing -- a raw displacement would break)"
	@./binastbuild tests/movable.bld tmp/reloc_a.elf 0x400000 > /dev/null \
	   || { echo "   FAIL: build at 0x400000"; exit 1; }
	@./binastbuild tests/movable.bld tmp/reloc_b.elf 0x900000 > /dev/null \
	   || { echo "   FAIL: build at 0x900000"; exit 1; }
	@chmod +x tmp/reloc_a.elf tmp/reloc_b.elf
	@a=$$(./tmp/reloc_a.elf); b=$$(./tmp/reloc_b.elf); \
	 [ "$$a" = "reloc" ] && [ "$$b" = "reloc" ] \
	   || { echo "   FAIL: relocated program printed '$$a' / '$$b', expected 'reloc'"; exit 1; }
	@echo "   both bases ran and printed 'reloc': the branch followed the label"
	@rm -f tmp/reloc_a.elf tmp/reloc_b.elf
	@echo
	@echo "== repair: inject KNOWN damage, demand byte-exact recovery"
	@rm -f fix0.elf fix1.elf fix2.elf dmg0.elf dmg1.elf dmg2.elf
	@./curetest tests/static.elf . | tee tmp/cure.txt | grep -E 'VERDICT|of 3 repairs'
	@grep -q '3 of 3 repairs byte-exact' tmp/cure.txt \
	   || { echo "   FAIL: not every repair recovered the original bytes"; exit 1; }
	@rm -f fix0.elf fix1.elf fix2.elf dmg0.elf dmg1.elf dmg2.elf
	@echo
	@echo "== page read/write: name a block, give it values, the program does it"
	@rm -f tests/static.elf.paged
	@./binastpage show tests/static.elf 0x4002c4 1 | grep -q "ORR_X_reg" \
	   || { echo "   FAIL: the page reader did not identify the block"; exit 1; }
	@echo "   read:  0x4002c4 is ORR_X_reg 0 31 19"
	@./binastpage set tests/static.elf 0x4002c4 MOVZ_X 0 42 0 | grep -q "reads back as MOVZ_X" \
	   || { echo "   FAIL: the write did not verify through the catalog"; exit 1; }
	@echo "   write: set it to MOVZ_X 0 42 0, decoded back as MOVZ_X"
	@./blink run tests/static.elf.paged | grep -q "exited 42" \
	   || { echo "   FAIL: the edited binary did not exit 42"; exit 1; }
	@echo "   run:   the edited program exits 42 (the value that was written)"
	@n=$$(cmp -l tests/static.elf tests/static.elf.paged | wc -l); \
	 [ "$$n" = "4" ] || { echo "   FAIL: $$n bytes changed, expected exactly 4 (one word)"; exit 1; }
	@echo "   bytes: exactly 4 differ -- the one word written, nothing else"
	@rm -f tests/static.elf.paged
	@echo
	@echo "== section header trust check reports what it cannot verify"
	@./blink info tests/static.elf | grep -q 'cannot be trusted' \
	   || { echo "   FAIL: bogus section table not flagged"; exit 1; }
	@echo "   bogus .text header correctly flagged"
	@echo
	@echo "ALL CHECKS PASSED"

# Lossless token round-trip and compiler-free patching, on a hand-built binary.
roundtrip: all
	@mkdir -p tmp
	@echo "== token round-trip"
	@./binast tokens tests/static.elf > tmp/tok
	@./binast rebuild tests/static.elf tmp/tok tmp/rebuilt
	@cmp tmp/rebuilt tests/static.elf && echo "   byte-identical OK"
	@echo "== compiler-free patch (b -> b2: one immediate changed)"
	@./binast mkpatch b b2 tmp/p.patch > /dev/null
	@./binast patch b tmp/p.patch tmp/b2.rebuilt
	@cmp tmp/b2.rebuilt b2 && echo "   byte patch == compiler output OK"
	@echo "== minification is semantics-free (compare code, not container)"
	@./cosmetic a.c tmp/a.fmt.c
	@$(CC) -O2 -Wl,--build-id=none -o tmp/a.full a.c
	@$(CC) -O2 -Wl,--build-id=none -o tmp/a.fmt tmp/a.fmt.c
	@./blink code tmp/a.full | tail -n +2 | cut -d' ' -f3- > tmp/s1
	@./blink code tmp/a.fmt  | tail -n +2 | cut -d' ' -f3- > tmp/s2
	@cmp tmp/s1 tmp/s2 \
	   && echo "   $$(wc -l < tmp/s1) instructions identical from a $$(wc -c < a.c)-byte and a $$(wc -c < tmp/a.fmt.c)-byte source OK"

# Coverage against a real production binary's instructions, as judged by an
# independent disassembler. This is the honest denominator: a code segment contains
# literal pools and padding, so counting every word understates coverage badly.
coverage: all
	@mkdir -p tmp
	@b=$${BIN_UNDER_TEST:-corpus/elf/000_SvtAv1EncApp}; \
	 if [ ! -f "$$b" ]; then echo "coverage: no corpus binary (run make corpus first)"; exit 0; fi; \
	 llvm-objdump -d "$$b" 2>/dev/null | awk '/^\s+[0-9a-f]+:/ && $$2 ~ /^[0-9a-f]{8}$$/ {print $$2}' > tmp/real.txt; \
	 ./isadb batch tmp/real.txt > tmp/gap.txt 2> tmp/cov.log; \
	 cat tmp/cov.log; \
	 t=$$(wc -l < tmp/real.txt); c=$$(awk 'NR==FNR{a[$$0];next} !($$0 in a)' tmp/gap.txt tmp/real.txt | wc -l); \
	 awk -v c=$$c -v t=$$t 'BEGIN{printf "coverage of real instructions: %.2f%%\n", 100*c/t}'

# A corpus of real instruction words, for measuring coverage beyond one binary.
corpus:
	@mkdir -p corpus/elf
	@i=0; for f in $${SYSDIR:-$$PREFIX}/bin/*; do [ -f "$$f" ] || continue; \
	  head -c4 "$$f" | od -An -tx1 | grep -q '7f 45 4c 46' || continue; \
	  cp "$$f" corpus/elf/$$(printf '%03d' $$i)_$$(basename $$f) 2>/dev/null && i=$$((i+1)); \
	  [ $$i -ge 60 ] && break; \
	done; echo "corpus: $$i binaries -> corpus/elf"

bench: all corpus
	@mkdir -p tmp
	@echo "== catalog vs every instruction word in the corpus"
	@: > tmp/words.txt
	@for b in corpus/elf/*; do ./blink code "$$b" 2>/dev/null | tail -n +2 | awk '{print $$2}'; done > tmp/words.txt
	@./isadb verify tmp/words.txt

# ---------------------------------------------------------------- install
install: all
	@mkdir -p $(BINDIR)/bin
	@cp $(BIN) $(BINDIR)/bin/
	@echo "installed: $(BINDIR)/bin ($(words $(BIN)) programs: $(BIN))"

clean:
	rm -f $(BIN)
	rm -rf tmp

distclean: clean
	rm -rf corpus

.PHONY: all test roundtrip corpus bench install clean distclean
