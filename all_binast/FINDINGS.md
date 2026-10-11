# binast -- measured findings

Goal under test: "all code has to make it to raw binary at some point; an AI
could write binary directly; a script parses/positions it like an assembler;
maybe even train a transformer on pure binary."

Everything below is from this box (Termux/aarch64, clang 21.1.8), same-directory
builds, so paths in the binary are identical. Nothing here is estimated.

## 1. The redundancy is real, and it is bigger than the source

Two C files, same program. `a.c` is the human version (descriptive names,
comments, blank lines); `b.c` is the minified version.

    source bytes:                a.c 397     b.c 132
    .text bytes:                      360         360
    stripped ELF:                    4144        4144
    flattened code blob (.text+.rodata, 416 B): byte-identical, 100%
    one semantic edit (10 -> 11) in b.c: 2 bytes changed in .text, 0 elsewhere in code
    identifier-rename-only edit in b.c:  0 bytes changed in .text

Read that again: the 265-byte source difference between `a.c` and `b.c` is
information that provably does not exist in the program. The binary is the
canonical object; source is one of many spellings of it.

Compile-level proof, not just a blob comparison: `a.c` with comments stripped
(`cosmetic`) compiled in the same directory gives **byte-identical .text,
.rodata and .data**. The only byte that moved was the GNU build-id (hash of the
whole file), which is a build artifact -- `-Wl,--build-id=none` removes it.

Corpus scale: ~/CCP-C-Cython-Python holds ~100 MB of C in 400 files. A large
fraction of those bytes are comments and indentation that compiles away. A
binary corpus is strictly smaller per unit of real content.

## 2. What is actually left in the binary is compressed, not redundant

    flattened blob 416 B: H = 5.43 bits/byte, 105 distinct byte values
    whole stripped ELF 4144 B -> gzip -9 -> ~354 B

So the "raw binary is bloated" intuition is backwards. Post-link, the padding,
symbols and headers are the fat; the code itself is at 5.4/8 bits per byte and
gzips only 1.2:1. A byte-level LM trained on it is not going to win by modelling
a low-entropy stream -- there is no low-entropy stream left to exploit beyond
structure like `mov`-heavy prologues.

## 3. Real .text is a clean fixed-width token stream

`binast tokens` emits a positioned stream with no length decoding needed:

    # binast-tokens v1 arch=aarch64 wordsize=4 endian=le
    # SEC .text addr=0x648 size=360 kind=CODE
    0x00000648 d503249f
    0x0000064c d280001d
    ...

Round-trip is lossless, and verified: `binast rebuild base.elf tokens out.elf`
reconstructs the ELF exactly (`cmp` clean, and the rebuilt file executes).
So the trainable form and the artifact are the same object -- no encoder/
decoder pair to keep in sync, which is where most "binary DSL" projects die.

## 4. Compiler output already has the property you want for agent editing

`binast mkpatch b b2` produced 214 byte edits for a one-token source change.
Patch those bytes into the old binary with `binast patch` and the result is
**byte-identical to the compiler's own output** (`cmp` clean) and prints the new
answer (506 vs 385). No compiler, no linker, no toolchain in the loop.

Cost side of the same measurement: a one-character semantic edit in C moved 214
bytes in the ELF, because the build-id hashes the file and .strtab/.symtab shift
by one byte each. That is the whole argument for (a) `--build-id=none`, and
(b) keeping sections position-independent with padding so a small edit stays
small. An agent that cannot afford to rebuild a 5 MB binary for a one-line
change needs this; the compiler makes it possible, it just does not do it by
default.

## 5. Is instruction-word stream a trainable target? -- YES, but context must span a word

Pure C11 probe, `probe_bin.c` + `probe_bin` (no libm: log2 comes from
`ccp_logf` in cygemm/snumblits.h; no zlib; no numpy; no Python). Corpora are the
system's own binaries, split BY PROGRAM -- train on 45, test on 15 the model has
never seen. Metric: bits per 4-byte instruction word; raw = 32.0. 13 seconds.

    train: 45 binaries, 14.15 MB .text, 3.71 M instruction words
    test:  15 unseen binaries, 2.23 M instruction words
    distinct instruction words in train: 419,628
    test words whose exact word appears in train: 87.38%

    metric                                     bits / 4-byte word
    raw, no model                                      32.000
    order-2 exact byte context, held out               32.615
    order-4 hashed + backoff, held out                 17.425
    order-4, SELF (training-data floor)                13.710

Reading:

- Raw binary is learnable: 17.4 bits/word held out is 1.84x compression on
  programs the model has never seen, with 87% of test words present in the train
  vocabulary. A model trained on instruction words is not chasing noise.
- The order-2 result is the useful negative. Two bytes of context is not enough,
  because an AArch64 instruction is 4 bytes: a byte model with a 2-byte horizon
  cannot condition on a whole previous instruction and lands at 32.6 bits/word,
  i.e. slightly WORSE than transmitting raw bits. Context must span at least one
  full word before any of the structure becomes exploitable. Anyone training on
  binary bytes should treat this as a fixed hyperparameter, not a tuning knob.
- Self vs held-out gap (13.7 -> 17.4) is the memorization margin: the model is
  generalizing, not recalling.
- For scale: the static unigram byte entropy measured in section 2 was 5.43
  bits/byte = 21.7 bits/word for the same kind of stream. The order-4 model's
  17.4 is below that, so the win comes from cross-word structure, not from byte
  frequencies.

What this does NOT show: that a model emitting instruction words produces correct
programs. Learnability is a compression question. Program synthesis needs a
verifier in the loop (run the result, compare behaviour) -- the measurement above
says the training signal exists, not that the search will find a working program.

## 6. Verdict on the four ideas

1. "All code has to reach raw binary at some point" -- true, and stronger than
   you'd expect: the binary is the canonical form. Comments, blank lines and
   identifier names provably do not reach it (100% identical flattened blobs
   from two very differently written sources; a 397-byte file and its 132-byte
   minification compile to the same code bytes).
2. "An AI could write binary like an assembler script positions it" -- the
   positioning half is done and verified here: `binast tokens` / `rebuild` is a
   lossless positioned form with no encoder/decoder pair to desync, and
   `mkpatch`/`patch` edits a real binary with no compiler in the loop.
3. "Drastic cut in the code-creation process" -- only for small, local edits.
   Measured cost: a one-character semantic change moved 214 ELF bytes (build-id
   hash plus .strtab/.symtab shift), so the win depends entirely on killing the
   build-id (`-Wl,--build-id=none`) and keeping edit distances short. Section
   relocation, register allocation and relocations are not solved by any of this.
4. "Train a transformer on pure binary" -- supported, with the word-sized
   context requirement above and one caveat: the target should be the positioned
   token stream, not a raw byte soup, because the positioning is what keeps the
   context aligned to instruction boundaries.

## What the spike does not settle

- Whether an agent writes better code as instruction words than as C. Byte-level
  learnability (measured above) is not the same question as editability at
  plan/refactor scale. An attention model over words emits locally plausible
  instructions and has no type checker to catch the global mistakes; the cheap
  defence is to keep a verifier in the loop (execute the result, compare
  behaviour) rather than to trust the model.
- AArch64 only. Every statement about fixed-width words and positioned tokens
  was tested for AArch64; x86-64/varint ISAs break the "each word starts at a
  4-byte boundary" assumption and need a length-decoding pass first.
- Section-level patching only. Cross-function control flow, relocation and
  register allocation are untouched -- real instruction-level editing still
  needs the register/relocation facts that a linked binary throws away (debug
  info or a rebuild from object files recovers them).
