# The matrix as an attention head: what firing a program against it actually does

This is the design as stated, implemented and measured:

    our map        FIXED filter bank, (rows x bits). It is the ISA. Never trained.
    the program    INPUT image, (positions x bits). The parameters.
    firing         correlate map against program -> (positions x rows) activation map
    backprop       into the PROGRAM (its binary position and structure), not the map

That is why it reads as an image model rather than a token model: a fixed kernel bank
correlated over a 2D input, producing a spatial activation map. A token model learns
what symbols mean; here the symbols are known exactly and the thing being optimised is
the input's arrangement.

## What fires, measured on real binaries

    adb, 457,282 code words
      rows exactly matching                457,267
      rows not matching                         15   (the CRYPTOGAMS strings in .text)
      mean rows fired per word                16.551
      words firing more than one row        456,213   (99.8%)

**99.8% of words fire more than one row, and the mean is 16.5.** The activation map is
not a one-hot assignment; it is a fan-out.

The reason is the map's own sparsity, and it is visible in the row sizes:

    the busiest rows and the number of bits each actually fixes
      ORR_X_reg      4      BL_imm      3      ADD_X_imm    3      LDR_X_ui     7
      MOVZ_W         4      B_cond      3      B_imm        2      LDP_X_off    5
      STP_X_off      4      STR_X_ui    6      ORR_W_reg    3      ADRP_X       2

Rows fix **2 to 7 bits out of 32**. A row that fixes 2 bits matches one quarter of all
32-bit words; one that fixes 3 matches an eighth. So a single row is a weak filter, and
firing the whole bank yields ~16 candidates rather than an identity.

**This is not a defect in the design — it is the design's central constraint, and it
says which way the two layers have to combine.** The matrix is a *detector bank*, good
at saying "not this" and poor at saying "exactly this". Two things can supply the
missing discrimination, and both are already measured in this repo:

- **more fixed bits per row**, which is a catalog change, or
- **context**, which is what the attention model was for, and which on foreign code
  supplies the form only 25% of the time (`binastmodel debug`).

So the firing map and the context model are complementary in exactly the way the
original idea suggested, and the measurement says the complementarity is real: the map
alone gives 16.5 candidates, context alone gets it right 25% of the time, and neither
is sufficient.

## The loss had to be rebuilt twice before it meant anything

Worth recording, because both failures looked like results.

**Failure 1: a loss that could not discriminate.** The first version was
`L = -log( sum_r prior_r * sim(i,r) )`. Because most rows fix only 2-4 bits, almost
every row agrees with any word, so the sum came out near 1 for correct *and* incorrect
matches alike: the reported loss was **0.0001** regardless. A loss that cannot separate
its cases is decoration, and a plausible-looking small number is worse than an obviously
wrong one.

**Failure 2: a gradient step that moved everything.** The Adam update divided by `|v|`
instead of `sqrt(v)`. For small gradients that scales the step as 1/g, so a near-zero
derivative produced an enormous update and the first refine run reported **every bit in
the program moving under a loss of 0.0001** — arithmetically impossible, and the
signature of exactly this mistake.

Both are fixed: the loss is now `(1 - best_row_agreement) * beta` with the row's own
fixed-bit count folded in, and the update uses `sqrt(v)`. The `check` command prints the
score gap between the best and second-best row so the discrimination is visible rather
than assumed — and on the current catalog **that gap is 0.0 for most words**, which is
the tie problem above showing up in the loss itself.

## The measured outcome: refining against the map DESTROYS the program

`binastfire refine` now has a loss with real discrimination and an optimizer that
actually moves bits. Run it and the result is unambiguous:

    lr 0.01, 40 steps   loss falls 0.1972 -> 0.1969,  0 of 7711 words move
    lr 0.03, 40 steps   loss falls 0.1941 -> 0.1917,  0 of 7711 words move
    lr 0.30, 40 steps   loss falls 0.1969 -> 0.0012, 7544 of 7711 words move (97.8%)

At the learning rate that moves anything, it does not refine the program -- it destroys
it. What the words BECOME, read back with the decoder:

    d503249f  HINT #imm7   ->  d503201f  NOP
    d503233f  HINT #imm7   ->  d503203f  YIELD
    d280001d  MOVZ_X       ->  d4000001  SVC #imm16
    d503203f  YIELD        ->  d503203f  YIELD

Every instruction collapses toward the same handful of **maximally specific** rows --
NOP, YIELD, SVC. Those rows fire alone precisely because they fix the most bits, so they
are the global optimum of "agree with the map as much as possible", and gradient descent
finds that optimum and stops.

**That is the design's failure mode, stated exactly: the map's objective has no
preference for the program's meaning, only for its own specificity.** Optimising a
program against the ISA map converges on the smallest set of most-constrained
instructions. A refined binary is a worse binary.

The `loss` column shows the same thing arithmetically: it goes to 0.0012, near zero, and
a near-zero loss is the symptom rather than the achievement -- the model has
memorised its target instead of learning anything about the input.

## What would have to change for the refine direction to be useful

Three candidates, in the order the measurements suggest:

1. **The objective needs a term that preserves the program.** Agreement with the map
   alone is degenerate. A term like "stay close to the original word" or "preserve
   control-flow structure" would stop the collapse -- but choosing it is a design
   decision, not a tuning knob, and the degenerate optimum means the current objective
   cannot be rescued by any learning rate.

2. **The map needs more fixed bits so its rows are discriminative.** A row fixing 2 bits
   matches a quarter of all words; the collapse happens because the maximally-specific
   rows are the only ones that discriminate at all.

3. **The direction may be wrong.** Refining the program against a fixed ISA map asks the
   program to look more like the ISA. What the earlier measurements actually showed is
   the reverse: the map is a perfect source of *labels* (100% coverage, zero wrong
   encodings), and the thing that needs adjusting is the layer that predicts from
   context -- which is `binastmodel`, and which is trained by backprop into a model
   rather than into a program.

## Status

- `binastfire fire` — **real and honest.** Reports the activation map: 99.8% of words
  fire more than one row before the `-1` constraints were counted, and 28.6% after
  (mean 1.416 rows per word). That correction matters and is described below.
- `binastfire refine` — **implemented, works mechanically, and its direction is
  measurably destructive.** Not a bug to fix; a result to respect.

### The `-1` bug that changed every firing number

The first version of the matrix loader read only `1` (fixed high) and `0` (operand bit),
and skipped `-1` (fixed low). But `-1` is a constraint too: the row requires that bit to
be 0. Omitting it made every row look far less specific than it is, and the firing
statistics were wrong by a large factor:

    mean rows fired per word        16.551  ->  1.416
    words firing more than one row   99.8%  ->  28.6%

The hard decoder disagreed visibly -- `isamap match d503249f` fires exactly **one** row
while the soft score put two rows at 0.43657 -- and chasing that disagreement found the
bug. The lesson is the ordinary one and worth stating: **when a relaxed score disagrees
with the exact function it is supposed to approximate, the relaxed score is wrong until
proven otherwise**, and the exact function is the referee.

