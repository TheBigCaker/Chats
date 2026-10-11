# binast

A raw-binary toolchain in pure C11, no dependencies. It reads, writes, and moves
machine code directly in ELF files, using a hand-written instruction lookup table —
no compiler, assembler, or linker in the loop.

**The point of it:** a user or an agent works on binary directly. A program is a page of
building blocks; read a page, change a block by name and value, write it back, run it. No
encoding knowledge required, and no toolchain in between.

    $ binastpage show tests/static.elf 0x4002c4 1
    00000000004002c4  aa1303e0  ORR_X_reg       0 31 19        # read: the block and its values

    $ binastpage set tests/static.elf 0x4002c4 MOVZ_X 0 42 0   # write: a block and new values
    block MOVZ_X  operands "Xd, #imm16, lsl #sh"
    before aa1303e0
    after  d2800540
    verified: reads back as MOVZ_X

    $ blink run tests/static.elf.paged
    run: tests/static.elf.paged exited 42                      # the program does what was written

Exactly 4 bytes differ from the original — the one word written, nothing else. The catalog
that writes the bytes is the same one verified at 100% coverage, and every write is decoded
back through it before the write is claimed. That is the whole loop: **build and shape
binary through the building-block system, with the user or agent in direct control of the
code.**

Six programs, two ways in: an interactive screen for people, and a machine-readable mode
for agents and scripts.

| program     | what it is                                                          |
|-------------|---------------------------------------------------------------------|
| `binastpage`| Binary as a page: read blocks + values, write a block, run it        |
| `binui`     | The interface. Interactive TUI, plus `tell`/`look`/`read`/`plant`/`coverage` for agents |
| `isadb`     | ISA catalog. Look up a form, encode operands to bytes, decode bytes  |
| `isamap`    | The catalog as a bit matrix, plus a tokenizer and neighbor search   |
| `blink`     | ELF tool. Inspect, disassemble, patch, move code, run                |
| `binast`    | Token stream. Split a binary into words, rebuild it losslessly       |
| `cosmetic`  | Source stripper. Proves formatting carries no semantics              |

Memory comes from `arbit` (the CCP cosmic sea) and threads from `partitricks` (raw
`clone(2)`, futex-parked) — no `malloc` family and no `pthreads`, and `make test` greps
the sources and fails if either returns.


## Using it

    binui                       # the interactive screen
    binui prog.elf              # ...with a binary already loaded

Nothing to memorise. Type to filter the instruction list, arrows to move, enter to
load a form, then type operand values and watch the bytes appear:

    1 SEARCH  movz
    2 FORMS   2 matches of 435
      MOVZ_X                 movz      Xd, #imm16, lsl #sh          4B
      MOVZ_W                 movz      Wd, #imm16, lsl #sh          4B
    3 OPERANDS  movz Xd, #imm16, lsl #sh  (2 of 3 set)
        reg            = 0
        imm            = 42
        shift<<16      = 0              <-- next
    4 BYTES  40 05 80 d2   word d2800540
         decodes back as MOVZ_X

From there you can read code out of the loaded binary by address (`r 0x4001c8 20`), ask
what a word is (`w d0000020`), see what an address already holds (`o 0x4001c8`), and
plant the encoding you built (`p 0x4001c8` -- writes a `.patched` copy, never the
original). `x` switches ISA, `h` lists everything.

### For agents, and for scripts

No terminal needed, one answer per line:

    binui tell aarch64 ADD_X_imm 0 1 42     # -> bytes 20 a8 00 91
    binui look aarch64 d2800540             # -> form MOVZ_X, op0 0, op1 42
    binui read prog.elf 4001c8 20           # addr/bytes/form/ops per instruction
    binui plant prog.elf 4001c8 aarch64 MOVZ_X 0 42 0
    binui coverage prog.elf                 # code-section words: matched/exact/data/padding
    binui forms aarch64 ADD                 # every form whose name or mnemonic matches
    binui agent                             # what this tool can do, machine-readable

`binui agent` is the discovery path: a program does not have to read this file to learn
the commands, the ISAs, or the operand kinds.

## Build

    make                     # CC=cc by default
    make CC=clang            # bring-up
    make CC=baccyn           # the port's real pipeline

`make test` is the gate — every claim below is one line of it. A failing line means
the toolchain is wrong, not the test.

`ccp_logf` comes from the port's own `cygemm/snumblits.h`; point `CYM` at that tree
if it lives somewhere else. There is no `-lm` anywhere.

## isadb — the instruction catalog

1320 AArch64 forms and 73 x86-64 forms, hand-written in `ISA_AARCH64.inc` and
`ISA_X86_64.inc`. Each row is a base word plus a list of field positions:

    { "ADD_X_reg", "add", "Xd, Xn, Xm", 4, {W(0x8B000000)},
      {0,0,2}, {0,5,0}, {5,5,5}, {0,0,0}, {0,0,0}, 0,0,0, 3 },

`W(base)` is the whole 32-bit word, written as one number so a dropped byte is
impossible. `{0,0,2}` means operand 0 lives at byte 0, bit 0, width 2 — positions are
in the byte stream, so they are readable off a hex dump.

    isadb forms mov              # what exists, matching a substring
    isadb forms ADD_X_reg        # one row: base word and field map
    isadb encode ADD_X_reg 0 1 2 # operands in the row's order -> bytes
    isadb decode d0000020        # a word as printed in a listing
    isadb decode "20 00 00 d0"   # the same word as file-order bytes
    isadb -t x86_64 decode "48 89 c8"
    isadb test                   # every row: encode, decode, re-encode, compare
    isadb reject                 # illegal words must NOT match a row
    isadb fixtures               # every row against known-good encodings
    isadb batch words.txt        # decode a whole file of words, list the misses
    isadb verify words.txt       # measure coverage over a corpus

Operand order is the row's field order, not assembly syntax. For
`MOV_RM_R {1,0}` that is (rm, reg); for `STXR_W {2,1,0}` it is (Ws, Wt, Xn).

Two checks, deliberately different:

- `test` proves each row agrees with itself. Necessary, not sufficient.
- `fixtures` holds encodings from an independent assembler. This is what catches a
  row that round-trips perfectly and still produces the wrong bytes — how `b.cond`
  (condition at bits 3:0, not 15:12), FCMP (opcode field 100010), and every
  double-precision FP instruction were wrong while `test` reported success.

## blink — the ELF tool

Address mapping goes through the executable `PT_LOAD` segment, never the section
table. A section table is advisory and can point outside the file entirely; reading
through one decodes file garbage as instructions and writes into the wrong place
while reporting success.

    blink info <elf>              # segments (authoritative), sections (flagged if bogus)
    blink symbols <elf>
    blink code <elf>              # disassemble the executable segment
    blink text <elf> <out.bin>    # raw code bytes, for comparing two builds
    blink patch <elf> <vaddr> <word> <out>
    blink relink <elf> <src> <len> <dst> <out>
    blink run <elf>

`blink info` prints section flags as W/A/X in spec bit order and marks any section
that overlaps the header, the program headers, or runs past EOF.

### relink

Moves a block of code to a new address and rewrites every PC-relative operand so the
code means the same thing:

    blink relink prog 0x4001c8 296 0x403000 prog.moved

Two things it handles that are easy to miss:

- **In-block targets travel.** A branch to somewhere inside the moved range is
  retargeted to the new location; a call out of the range keeps its absolute target.
- **ADRP is page-relative, not byte-relative.** `adrp` stores a count of 4KiB pages
  from the page containing the instruction. Treating it as a byte offset leaves every
  data reference pointing into the old page — the relocated program loads from the
  wrong address and segfaults.

The source range is left as a branch to the new location, so the old address still
works. Filling it with nops was wrong: anything entering at the old address — a call,
or the program's entry point — would fall through into whatever followed.

`relink` verifies before writing: it decodes both versions independently and requires
every PC-relative operand to still name the same destination. It refuses to write
output that fails, so a mis-relocated block is a hard error rather than a crash later.

## binast and cosmetic

    binast tokens <elf>                   # word stream
    binast rebuild <elf> <tokens> <out>   # lossless round-trip
    binast mkpatch <a> <b> <out.patch>    # byte diff between two builds
    binast patch <elf> <patch> <out>      # apply it, no toolchain involved
    binast diff <a> <b>
    binast setword <elf> <vaddr> <word> <out>

    cosmetic <in.c> <out.c>               # strip comments and formatting

## Verified state

    aarch64 fixtures             84 pass, 0 fail
    x86_64  fixtures             25 pass, 0 fail
    aarch64 self-check         1320 rows, 0 failed
    x86_64  self-check           73 rows, 0 failed
    relink                     74 instructions, 17 PC-relative operands held
    relocated binary           runs, exits 0
    full corpus, 40 binaries  3663371 of 3663456 words matched, 0 wrong encodings
                               15 inline data, 70 zero padding, 0 unmatched
                               = 100.000% of every word in every code section

Two different measurements, deliberately:

- **Small binary, every word.** `blink code tests/static.elf` reports 0 unrecognised
  non-zero words out of 6031. The only unrecognised words are `00000000`, which is
  unreachable padding inside the code segment.
- **Production binaries.** `make coverage` disassembles a 97KB encoder with an
  independent tool and measures against the 7991 words it calls instructions: all of
  them are recognised, and every one re-encodes to the identical bytes.
- **The whole corpus.** `binui coverage` walks the `SHF_EXECINSTR` sections of all 40
  binaries in `corpus/elf` — 3,663,456 words. 3,663,371 decode and re-encode
  byte-identically; 15 are bytes of an embedded string literal; 70 are zero padding;
  **none are unexplained.** Against words that are actually instructions this is
  100.000%.
- **The 15 are data, and it is checkable.** They are byte-slices of two ASCII credit
  strings the assembler embedded *inside* `.text` of the adb binary —
  `"Montgomery Multiplication for ARMv8, CRYPTOGAMS by <appro@openssl.org>"` and
  `"GHASH for ARMv8, CRYPTOGAMS by <appro@openssl.org>"`. The disassembler rejects all
  9 distinct values as `invalid instruction encoding`. `word_is_inline_data` in
  `binui.c` finds them by walking outward while bytes stay printable ASCII.
- **Correcting an earlier claim in both directions.** A draft of this file said most of
  the residual was data. Disassembling a sample showed many were real instructions —
  `orr v0.16b`, `movi v1.2d`, `aese`, `pmull`, `sha256h`, `casal`, `ldaddal`, `stg` —
  and rows were added. The opposite error then appeared: I ran the disassembler without
  its feature flags, so it rejected every crypto instruction and I read that as "this is
  not an instruction." With `-mattr=+all`, 1,405 words that had been written off as data
  turned out to be crypto, LSE-atomic, MTE and SVE instructions. Both directions are now
  settled by asking the disassembler per word with every feature enabled, never by
  assumption.
- **Exactness, not just coverage.** `isadb batch` reports a word that matched a row but
  re-encoded differently. That count is zero, and it is the number that matters: a row
  that recognises a word and then cannot reproduce it is worse than one that declines.
  Every measurement in this file is taken with that count checked.
- **Coverage denominators lie unless you pick them.** Measuring the executable
  `PT_LOAD` segment instead of the code sections inflates the total with `.rodata`,
  `.eh_frame` and relocation tables, which then all read as "unmatched" — the adb binary
  shows 101,810 of them that way and 0 the honest way. `binui coverage` iterates
  `SHF_EXECINSTR` sections for exactly this reason, and reports `inline_data` and
  `zero_padding` as their own lines so neither is ever counted as a miss.

## isamap — the catalog as a bit matrix

Each catalog row is already a matrix object: a base word plus (bit, width) windows.
`isamap` exposes the whole catalog as a `(forms x bits)` ternary matrix —
`1` fixed high, `-1` fixed low, `0` operand bit — so matching a word against every
form is one pass of

    (word XOR template) AND NOT mask == 0

which is bit arithmetic, not parsing. `isamap verify` recomputes the mask from the
raw row fields independently of the catalog's own `isa_fieldmask` and requires the two
to agree on all 1,393 rows; a matrix that agreed by construction would prove nothing.

    isamap grid aarch64             # the bit map, one line per form
    isamap stats aarch64            # density; 17,998 of 42,240 cells are operand bits
    isamap match d2800540           # which rows fire on one word
    isamap neighbors d2800540 aarch64 8   # nearest encodings by matrix distance
    isamap mtx   tmp/isa_aarch64 aarch64  # write .mtx + .names
    isamap export tmp/isa_aarch64.bin aarch64
    isamap verify aarch64
    isamap vocab tmp/exec_words.txt aarch64
    isamap agent                    # machine-readable description

Measured over the same 3,663,456-word corpus: **414,502 distinct words map onto just
812 catalog forms.** That is the tokenizer argument in one line — a word-level
vocabulary of 414k collapses to 812 symbol types plus a small operand payload, and
`neighbors` shows the relation is already in the matrix (for `movz x0,#42`, distance 0
is `MOVZ_X` and distance 1 is `MOVZ_W`/`MOVK_X`/`MOVN_X`). No vocabulary file, no
training, no embedding table.

## Where the field layouts come from

`~/ISA_MAPS/llvm_isa_tables/AArch64/*.td` states every field's bit range directly, e.g.
`AArch64InstrFormats.td`:

    class BaseAddSubEReg<...> {
      bits<5> Rd; bits<5> Rn; bits<5> Rm; bits<6> extend;
      let Inst{20-16} = Rm;
      let Inst{15-13} = extend{5-3};
      let Inst{12-10} = extend{2-0};
      let Inst{9-5}   = Rn;
      let Inst{4-0}   = Rd;
    }

Read that instead of reverse-engineering bytes: the extended-register family cost hours of
wrong rows (option bits baked into the base, a shift window that only worked for 0..7)
because the 6-bit extend field is split 3+3 across two ranges -- exactly what a single
contiguous window cannot express. `tmp/td_expand.py` parses those tables into row
skeletons in one pass (495 classes carry an explicit bit map); `tmp/td_final.py` turns the
skeletons into windows and reports which ones real corpus words confirm.


Four known characteristics, stated rather than hidden:

- 220 rows decode to a more general form than written (aliases). Round-trips are exact;
  naming picks one. `test` reports the count.
- The x86-64 catalog models the wide-imm32 forms. A real assembler canonicalises small
  immediates to `83 /digit` imm8; both are pinned in the fixtures so the difference is
  explicit.
- `isadb decode` reads a bare hex run the width of the ISA word as a numeric word
  (reversed into file order); anything else as file-order bytes. `isadb decode
  d0000020` and `isadb decode "20 00 00 d0"` are the same instruction.
- `csetm`'s operand is the *encoded* condition, which is the complement of the named
  one (`csetm eq` -> 1). A field map cannot invert a value and one alias does not
  justify a transform kind; the fixture pins the convention.

## Files

    isadb.h              catalog runtime: field kinds, encode, decode, legality
    isamap.c             the catalog as a bit matrix, tokenizer, neighbor search
    ISA_AARCH64.inc      the AArch64 table
    ISA_X86_64.inc       the x86-64 table
    isadb_fixtures.h     known-good encodings, per ISA
    relink.h             relocator: scan, retarget, verify
    elfobj.h             minimal ELF64 reader/writer
    isadb.c blink.c binast.c cosmetic.c
