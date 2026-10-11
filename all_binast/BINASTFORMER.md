# BINASTFORMER.md

## What this is

The catalog is a matrix. `isamap` already exposes it that way: **1,320 rows x 32 bit
columns**, every cell ternary — `1` the row requires high, `-1` it requires low, `0` it
does not care. That is the map, and it is verified against the catalog it came from.

Two things were then noticed, and they are the same thing:

1. Read down a column and a row is a **vector of weights**. Not metaphorically — it is a
   fixed vector of +1/0/-1 values over 32 dimensions, one per bit. Fire a word against
   the 1,320 rows and you get an agreement score per row: a dot product between the word
   vector and each row vector. That is a linear layer, with the map as its weights, and
   the weights are never trained because the ISA is ground truth.

2. Every word is also a **number**. `d2800540` is one 32-bit integer, and it is the same
   integer whether you call it a token or a value.

So the vocabulary and the weight matrix are the same object seen from two directions. The
rows are what a word is matched against; the words are what the rows match. There is no
separate embedding table to learn, because the symbol space is already the 32-bit space
the weights live in.

**That is the binastformer, and that is its vocabulary.**

## Why this is not a normal transformer

A normal transformer has two things the map already is:

    vocabulary        learned embedding table, D x V, trained
    attention         learned Q/K/V projections, trained

Here the "vocabulary" is a row index into a hand-written table and the "attention" is a
dot product against fixed ISA patterns. Neither is trained, because neither is unknown —
an instruction's meaning is defined by the ISA, not discovered from data. The map is a
frozen, exact, infinite-precision prior that a learned model would spend its entire
training budget trying and failing to recover.

What is left to learn is the one thing the ISA does not specify: **which block comes
next, and what its operands are.** That is a real and large problem, and it is the only
problem here.

## The numbers that already exist

Everything below is measured, in this tree, and reproducible from `make test` plus the
tools named:

    catalog size          1,320 rows x 32 bits, 0 rows failed self-check
    corpus covered        3,663,441 words, 0 unmatched, 0 wrong encodings
    rows that fire        mean 1.416 per word; 28.6% of words fire more than one
    the tie               busiest rows fix only 2-7 bits (B_imm 2, ADRP_X 2, LDR_X_ui 7)
    H(form)               5.7245 bits over 812 forms used
    H(form | context)     gains +0.9282 / +1.2961 / +1.5410 / +1.5880 at orders 1-4
    order sweep           crossover at order 4; 5 and 6 overfit
    mean payload          20.99 bits per instruction
    canonical tokens      order-3 held-out 7.2144 vs raw order-2 8.4494
    PC-relative words     17.8% (81,995 of 458,642 in one binary)

## The two directions, and where each one honestly stands

**Firing the map at a program** (`binastfire fire`) is real and works: 457,267 of 457,282
words in the adb binary match a row exactly, and the activation map over
(positions x rows) is computed. This is the map used as a filter bank, image-model style,
exactly as the architecture describes.

**Refining a program against the map** (`binastfire refine`) does NOT work, and the
reason is not a bug to fix. The objective "maximise agreement with the best-matching row"
has a trivial global optimum — be the most-constrained instruction that exists — so
gradient descent finds it and collapses words onto NOP/YIELD/UDF. That is a tautology
about that objective. Any loss that says "be as matchable as possible" degenerates the
same way, so discrimination cannot come from the map alone.

Where discrimination has to come from, measured rather than guessed:

    map alone            ~1.4-16 candidate rows per word (coarse; it is a filter, not a decision)
    context alone        gets the form right ~25% on foreign code
    both together        complementary -- neither is sufficient, which is the ground for fusing them

## The fusion this points at

The map supplies **exact labels** — it can say "this word IS MOVZ_X, not something like
it" with zero ambiguity, which no learned model can do. The context model supplies
**which block comes next**, which the map cannot know. So the alignment has to be
one-way:

    train the context model TOWARD the map     (the map is the exact teacher)
    never train the map toward the context     (a softmax never produces exact bits)

That is the design the numbers support, and it is what `binastmodel` already does: pure-C
causal self-attention over canonical tokens, gradients verified, and — once its learning
rate was allowed to decay rather than held fixed — beating the best n-gram on both slices
it has been trained on:

    200k slice, converged       held-out row 4.6358  vs order-1 n-gram 5.3716   (-0.74)
    full corpus, epoch 3        held-out row 3.7911  vs order-3 n-gram 3.9017   (-0.11)

The full-corpus run is the one worth being careful about. It was reported earlier as a
LOSS to the trigram (4.0418 vs 3.9017) and that report was wrong: the run had a fixed
learning rate of 2e-3, the loss was flat across its last epochs while the n-gram kept
winning, and a rate that cannot come down makes a model circle the optimum at full step
size. With cosine decay to 10% on the same data, same split, same 3 epochs:

    unigram baseline            row 5.4513
    order-3 n-gram              3.9017   <- the bar
    binastmodel epoch 1         4.0541
    binastmodel epoch 2         3.8795   <- already past the trigram
    binastmodel epoch 3         3.7911   and still descending (drops 0.175, 0.088)
    gain vs unigram             row +1.6602

So the model is ahead at 3 epochs and has not converged — the 200k run needed 18. The
honest statement is "beating the trigram and still falling", not "converged to X".

## Which program to load first, measured

Scanning binast's own eight binaries (code sections only, `SHF_EXECINSTR`):

    tokens           45,445
    unique words     19,434          <- a 2.3 uses-per-token ratio: too sparse to learn
    declined             519 (0.4%)  strings and jump tables, not code
    forms used           344   of 1320

and against the 40-binary external corpus:

    binast's own code     344 forms
    external corpus        92 forms
    catalog              1320 forms

**binast's own code uses 3.7x more of the ISA than the whole external corpus**, because it
does binary parsing -- shifts, byte extraction, bit masks, integer work -- while the corpus
is dominated by a handful of load/store/branch patterns. The corpus was thin; this code is
rich. That is the measurement that says which program should be loaded first, and it also
says the token decision is open: **19,434 whole words is unusably sparse; 344 forms is
dense.** The row-plus-payload split already built is the evidence for the latter, but the
choice is not made here.

## The geometry: measured, and it is real

If the map is a geometry rather than an alphabet, then distance in it should mean
similarity in meaning. Tested over 1,320 rows, 8 independent samples of 400 rows each
(chance level 6.4%):

    metric                          family kin   stdev   chance
    plain Hamming (1D)                 46.8%    1.7      6.4%
    operands-only (the other axis)      5.8%      -      6.4%
    2D: layout then opcode             53.6%    1.5      6.4%
    entropy/MI-weighted (1D)           41.7%    1.7      6.4%
    exact form, any metric              0.0%      -      0.1%

"Family kin" = the single nearest row shares the mnemonic family, e.g. `USHR_V_16B_2 ->
USHR_V_4S_2` at distance 1, `CSET_GE -> CSET_LT` at distance 1, `MOVI_V_2S_lsl16 ->
MOVI_V_2S_lsl24` at distance 1. The ISA is built so that **neighbouring bit patterns are
sibling instructions**, and that is measurable without any training.

Three findings worth stating precisely:

1. **Treating it as a shape beats treating it as a number.** The same rows scored as a 2D
   object (operand layout as one axis, opcode geometry as the other) reach 53.6% while the
   flat 1D Hamming reaches 46.8%. The gap is about 8 standard errors, so it is real. The
   two axes are not redundant: operand layout ALONE is near-useless at 5.8%, and it is only
   as a second axis that it contributes.

2. **The information-theoretic bit weighting made it WORSE** (41.7% vs 46.8%). Worth
   recording because the intuition pointed the other way: weighting bits by their mutual
   information with the family over-sensitises the metric to exactly the bits that
   distinguish siblings within a family, so it splits the neighbourhood apart. Plain
   counting of differing fixed bits is the better geometry.

3. **Geometry localises, it does not identify. Exact form: 0.0%, every metric.** Distance
   never picks the single right row. What it does instead:

       mean neighbourhood size         6.2 tied rows at the winning distance
       the winning neighbourhood
         contains a same-family row    82.3%

   So the answer is IN the neighbourhood 82% of the time, and reading the neighbourhood
   rather than the top-1 is the correct operation. This corrects an earlier framing in this
   document: the tie at ~1.4-16 rows was described as a flaw that broke `refine`. It is not
   a flaw. **It is the locality.** Firing does not fail to decide; it hands over the
   neighbouring strata, which is the useful object.

That is the shape of the model this points at: not "predict the token", but **predict where
in the geometry the next instruction lands, and read the neighbourhood there.** A token
model's space is arbitrary (`cat` and `chat` are unrelated integers and the model must
learn that they are near); this space is already organised, so similarity does not have to
be learned -- only trajectory through it.

## The axes available

    axis 1   the 32 bits within a word           (the shape)
    axis 2   which row / where in the geometry    (the position)
    axis 3   position in the program              (the context)

A program is already a 2D field (N words x 32 bits) and firing produces a third
(N words x 1320 rows), so the objects here are images, not streams. That is not an analogy:
`binastfire` computes a (positions x rows) activation map, and attention weights in a
transformer are the same kind of object (positions x positions).

**The map is the vocabulary and the weights at once, and it is exact and frozen; the only
thing left to learn is what comes next.**

## What an external program adds: measured

If the binastformer is the map + a program + the tokens, then loading an external program
is supposed to add a layer. The question that decides whether that layer is NEW information
or mostly a repetition was measured by firing three programs against the same map and
comparing their form distributions:

    A                    B                    shared   |A|  |B|  B mass covered
    adb                  binast_own             318   688  369       96.1%
    000_SvtAv1EncApp     binast_own             181   211  369       89.8%
    000_SvtAv1EncApp     adb                    188   211  688       91.4%

"B mass covered" = the share of B's instruction weight that lands on forms A also uses, so
it is weighted by how often each form actually appears, not by how many forms exist.

**89.8% - 96.1%.** Loading a second program covers almost all of the first program's
instruction mass. And the top forms are nearly identical lists:

    binast_own            adb                  000_SvtAv1EncApp
    ADD_X_imm    3404     ORR_X_reg    43729    LDR_X_ui        776
    B_cond       2985     BL_imm       37159    ORR_X_reg       752
    ORR_X_reg    2574     LDR_X_ui     33481    ADD_X_imm       674
    LDR_X_ui     2212     ADD_X_imm    32890    BL_imm          605
    MOVZ_W       1820     MOVZ_W       20951    ADRP_X          551

So: **a new program is a new ARRANGEMENT of a largely-known vocabulary, not a new
vocabulary.** The skeleton is shared; what differs is the ORDER and the operand values.

That answers the layer question concretely:

- The **vocabulary layer does not grow.** A second program adds the remaining 10.2% of form
  types (mostly rare ones) and nothing more. Worth stating plainly so a "new layer" is not
  assumed to mean "new knowledge".
- The **arrangement layer is entirely new.** Every program has its own sequence, and that
  is where all of the novel information is. This is why the earlier transfer experiment was
  directional: form frequencies transfer (96% overlap), structure does not follow from them.
- Therefore the external program should be fed in **as A (the input image), using the same
  W**, and NOT as a new W. Firing it fills in the same (positions x rows) activation map —
  the shapes are the same shapes.

One consequence worth naming: since the vocabulary barely grows, an external program is
mostly a test of whether the model can READ an unfamiliar arrangement, not whether it can
learn new symbols. That is the harder and more useful question, and it is now measurable
against a baseline instead of being assumed.

**The map is the vocabulary and the weights at once, and it is exact and frozen; the only
thing left to learn is what comes next.**
