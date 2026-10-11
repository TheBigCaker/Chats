# binast / isadb -- pure-C raw-binary toolchain

Two halves of one idea: **bytes -> building blocks -> bytes**, with no compiler,
linker or assembler in the loop after the catalog is generated.

    binast    read/write ELF: flatten, position, diff, patch, place a word
    isadb     the ISA lookup table: encode and decode instruction words

Both are pure C11 with zero dependencies. No Python anywhere (0 .py files in this
tree). No libm either -- `log2` in binast comes from `ccp_logf` in
`cygemm/snumblits.h`, so nothing links against `-lm`.

## Build

    make                 # binast, cosmetic, probe_bin (no compiler hardcoded: CC ?= cc)
    make isadb           # the ISA table runtime
    make catalog         # regenerate ISA_AARCH64.inc from llvm-mc
    make check           # lossless round-trip + compiler-free patch, on real binaries

`mkisadb` is the only thing that ever touches a system assembler, and it runs at
*catalog build* time. `isadb` itself does pure integer math: no mnemonic string is
parsed at runtime and nothing shells out. That is what makes the toolchain
portable to a box with no LLVM at all -- ship the generated `.inc` with the tool.

## Commands

    binast dump    <elf> <out.bin>            flatten code sections -> positioned blob
    binast tokens  <elf>                      positioned instruction-word stream
    binast rebuild <base.elf> <tok> <out.elf> replay tokens -> ELF (lossless)
    binast diff    <a.elf> <b.elf>            byte changes, mapped to section+offset
    binast mkpatch <a.elf> <b.elf> <p.patch>  guarded byte patches
    binast patch   <elf> <p.patch> <out.elf>  apply a patch, no compiler
    binast setword <elf> <vaddr> <word> <out> place one instruction at an address
    binast stat    <a> <b>                    canonicality % and entropy

    isadb forms   [substr]                    list the catalog
    isadb encode  <FORM> [reg|val]...         assemble one instruction
    isadb decode  <hexword>                   disassemble one instruction
    isadb test                                catalog self-check
    isadb verify  <hexwords-file>             coverage + round-trip on real code

## The catalog

Generated data, one row per *form* (mnemonic + operand skeleton):

    { name, mnemonic, operands, base, vlo[4], vwid[4], vsc[4], vkind[4], nvars }

`mkisadb` derives each field's bit position by assembling a form twice -- once
with an operand at 0, once at the smallest non-zero legal value -- and XOR-ing the
two words. One bit set means that bit is the field's lowest bit. A second probe
verifies the declared width. Nothing about the encoding is hand-copied.

Variable kinds: `K_REG`, `K_IMM`, `K_SHIFT` (UBFM alias), `K_LOGINT` (logical
immediate, ARM ARM DecodeBitMasks), `K_UBFM` (immr:imms), `K_UBFX` (lsb:width),
`K_COND` (condition code), `K_SIMM` (signed offset).

Aliases are handled by *field legality*, not by name lookup: ORR/LSL/BFM/TST/NEG/
MOV all live inside the same encoding space, so `isadb decode` picks the form whose
own field semantics accept the word (e.g. a shift form requires `imms == width-1`,
a UBFX requires `imms >= immr`). One catalog, several names per word -- exactly how
the ISA actually works.

## Measured on this box

Catalog self-check: **72/72 forms round-trip exactly** through encode -> decode ->
re-encode (18 resolve through an alias, which is correct behaviour).

Real code, 2,000,288 instruction words pulled out of 6 real binaries:

    catalog matched              1,843,633   (92.2% "catalog expressible")
    exact word round-trip        1,843,353   (99.9848% of matched)
    logical-imm grain variants         280   (mask recovered, field re-canonicalized)
    real failures                        0

The grain variants are the one documented ambiguity: the same 64-bit mask is legal
at more than one pattern length, so re-encoding may pick a different-but-equivalent
`immr:imms` than the original compiler did. The mask decodes correctly either way.

## End-to-end: an agent writes a program with no compiler

    ./isadb encode add_x_x_imm 0 0 1          ->  91000400   (add x0, x0, #1)
    ./binast setword b 0x0000079c 52803f41 tmp/b.ans
    ./b          -> 385
    ./tmp/b.ans  -> 506

`isadb` produced the word, `binast` placed it, and the binary runs with the new
result. `binast diff b tmp/b.ans` reports exactly 4 changed bytes, all inside
`.text`. That is the whole loop: search the table, pick a block, encode it, put it
in the raw binary.

Lossless read path, verified on a 41k-token third-party binary: `tokens` ->
`rebuild` reproduces the input ELF byte for byte (`cmp` clean) and the rebuilt
file executes.

## Notes and limits

- AArch64 only. The "every word starts at an aligned boundary" property that makes
  the positioned stream free of length decoding holds for AArch64/x86-64; varint
  ISAs need a length-decoding pass first.
- Fixed-size branch offsets only (the catalog covers +-1MB `b`/`b.cond` and 19-bit
  `cbz`/`cbnz`). Unconditional `bl` and ADRP/ADR page arithmetic are in the table
  where the assembler could verify them; PC-relative relocation of a *moved* block
  is not solved here and is the next real problem.
- `setword` writes one instruction at a virtual address. Cross-block control flow,
  register allocation and relocation are untouched -- those are the parts a linker
  exists for.
