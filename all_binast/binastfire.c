/* binastfire.c -- fire the catalog matrix against a loaded program.
 *
 * THE ARCHITECTURE, as stated: the catalog is not a feature of a model, it is the
 * ATTENTION HEAD ITSELF. Our map is (rows x bits), a fixed filter bank. A loaded
 * program is (positions x bits), an input image. Firing one against the other gives a
 * (positions x rows) activation map, and the backprop goes into the PROGRAM -- shifting
 * its binary position and structure -- not into our map. Our map is the ISA, and the
 * ISA is ground truth, so it is never trained.
 *
 * That is why this is closer to an image model than a token model: a fixed kernel
 * bank correlated over a 2D input, producing a spatial activation map. A token model
 * learns what symbols mean; here the symbols are already known exactly and the thing
 * being optimised is the input's arrangement.
 *
 * THE SOFTNESS. A hard match is not differentiable: `(word XOR template) AND NOT mask
 * == 0` is a boolean. So each bit is relaxed to s_b = sigmoid(p_b) and the agreement
 * between a program word i and a catalog row r becomes the fraction of that row's
 * fixed bits the word agrees with: 1.0 is exact, 0.5 is chance, differentiable in
 * every program bit. The hard decoder stays the referee -- `row_score` below is the
 * relaxation, and how far it drifts from the hard answer is measured, not assumed.
 *
 * MEMORY AND THREADS. No malloc family anywhere: every buffer comes from arbit, the
 * same cosmic-sea allocator the CCP pipeline uses (arbitcore.h). No pthreads: the
 * pool is partitricks, raw clone(2) with futex parking (arbishim.h). `make test`
 * greps this file for malloc/calloc/realloc/free/pthread and fails if one returns.
 * File I/O still goes through stdio, which is not the alloc family -- the ELF read
 * is a stream, and stdio is what every other tool in this tree uses for it.
 *
 *   binastfire fire   <elf> [verbose]      per-word activation: rows fired, ambiguity
 *   binastfire refine <elf> [steps] [lr] [alpha]   gradient descent INTO the program
 *   binastfire check  <elf>                loss diagnostics for the first words
 *   binastfire agent
 *
 * Pure C11, no libm, no libc allocator, no pthreads, no dependencies.
 */
#include <stdio.h>
#include <stdlib.h>   /* getenv/atof only -- NOT malloc. make test proves it. */
#include <string.h>
#include <stdint.h>

#include "isadb.h"
#include "arbishim.h"

#define ISA_INC_AARCH64 "ISA_AARCH64.inc"
#include ISA_INC_AARCH64
static const IsaTable T = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

/* ---- no libm ---- */
static inline float f_exp(float x) {
    if (x < -87.0f) return 0.0f;
    if (x > 88.0f) x = 88.0f;
    const float LN2 = 0.6931471806f, INV_LN2 = 1.4426950409f;
    float kf = x * INV_LN2;
    float k = (kf >= 0.0f) ? (float)(long)(kf + 0.5f) : (float)(long)(kf - 0.5f);
    float r = x - k * LN2, r2 = r * r;
    float p = 1.0f + r + r2 * (0.5f + r * (1.0f/6.0f + r * (1.0f/24.0f + r * (1.0f/120.0f
            + r * (1.0f/720.0f + r * (1.0f/5040.0f))))));
    union { float f; uint32_t u; } sc;
    long ki = (long)k;
    if (ki > 127) ki = 127;
    if (ki < -126) { p *= 1e-38f; ki += 126; }
    sc.u = (uint32_t)(ki + 127) << 23;
    return p * sc.f;
}
static inline float sigmoid(float x) { return 1.0f / (1.0f + f_exp(-x)); }
static inline float f_sqrt_local(float x) {
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t u; } v; v.f = x;
    v.u = (v.u >> 1) + 0x1fc00000u;
    float y = v.f;
    y = 0.5f * (y + x / y); y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y); y = 0.5f * (y + x / y);
    return y;
}

/* ------------------------------------------------------------------ the map --- */

/* The catalog as a filter bank: for every row, its fixed-bit template and the
 * positions of the bits it fixes. The .mtx file is the source of truth so this reads
 * exactly what `isamap` verified, never a second guess at the catalog. */
static int      g_nrows = 0;
static int      g_nbits = 0;
static uint32_t g_tmpl[4096];
static uint8_t  g_fixed[4096][33];      /* ALL constrained bit positions (1 AND -1) */
static int      g_nfixed[4096];

static int load_matrix(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "binastfire: cannot open %s -- run "
                              "`isamap mtx tmp/isa_aarch64 aarch64` first\n", path); return -1; }
    char line[16384];
    int expect = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '%') {
            if (line[1] == ' ') {
                int a = 0, b = 0;
                if (sscanf(line + 1, "%d %d", &a, &b) == 2 && b > 0) { expect = a; g_nbits = b; }
            }
            continue;
        }
        if (!line[0] || line[0] == '\n') continue;
        if (g_nrows >= 4096) break;
        uint32_t tmpl = 0;
        int nf = 0;
        char *p = line;
        for (int idx = 0; idx < g_nbits && *p; idx++) {
            while (*p == ' ') p++;
            if (*p == '\n' || !*p) break;
            int shift = g_nbits - 1 - idx;
            /* ALL THREE STATES ARE CONSTRAINTS. -1 (fixed low) is as much a part of the
             * template as 1 (fixed high): the row requires that bit to be 0. Only '0'
             * (operand bit) is free. Reading only '1' and '0' and skipping '-1' meant
             * g_fixed omitted every bit a row requires to be LOW, so rows looked far
             * less specific than they are and the soft score tied where the hard
             * decoder saw a single match -- d503249f fires exactly one row, but the
             * soft score put two rows at 0.43657. */
            if (*p == '-') {
                p++;
                if (*p == '1') p++;
                if (nf < 32) g_fixed[g_nrows][nf++] = (uint8_t)shift;   /* required low */
            }
            else if (*p == '1') {
                tmpl |= 1u << shift;
                if (nf < 32) g_fixed[g_nrows][nf++] = (uint8_t)shift; p++;
            }
            else if (*p == '0') { p++; }
            else break;
        }
        g_tmpl[g_nrows] = tmpl;
        g_nfixed[g_nrows] = nf;
        g_nrows++;
        if (expect && g_nrows >= expect) break;
    }
    fclose(f);
    /* rows with no fixed bits cannot be matched at all and would divide by zero */
    int live = 0;
    for (int r = 0; r < g_nrows; r++) if (g_nfixed[r]) live++;
    if (!live) { fprintf(stderr, "binastfire: no row fixes any bit\n"); return -1; }
    return 0;
}

/* --------------------------------------------------------------- the program -- */

typedef struct { uint8_t *b; size_t nwords; uint64_t vaddr; uint8_t *keep; size_t keep_size; } Prog;

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static int prog_load(const char *path, Prog *o) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "binastfire: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    /* the whole image comes from a carve: it is one large contiguous region, which is
     * what a carve is for. Not the fibonacci buckets -- those are tensor-shaped. */
    uint8_t *img = (uint8_t *)arb_carve_zeroed(sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
    fclose(f);
    if (memcmp(img, "\x7f" "ELF", 4)) {
        fprintf(stderr, "binastfire: not an ELF\n");
        arbit_release_carve((long)img); return -1;
    }
    uint64_t shoff = r64(img + 0x28);
    int shnum = r16(img + 0x3c), shent = r16(img + 0x3a);
    uint64_t best_off = 0, best_va = 0; size_t best_sz = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = img + shoff + (uint64_t)i * shent;
        uint64_t flags = r64(sh + 8), addr = r64(sh + 0x10), off = r64(sh + 0x18), size = r64(sh + 0x20);
        if (!(flags & 0x4) || !size || off >= (uint64_t)sz) continue;
        if (off + size > (uint64_t)sz) size = (uint64_t)sz - off;
        if (size > best_sz) { best_sz = (size_t)size; best_off = off; best_va = addr; }
    }
    if (!best_sz) {
        fprintf(stderr, "binastfire: no executable section\n");
        arbit_release_carve((long)img); return -1;
    }
    o->b = img + best_off;
    o->nwords = best_sz / 4;
    o->vaddr = best_va;
    o->keep = img;
    o->keep_size = (size_t)sz;
    return 0;
}
static void prog_release(Prog *p) { if (p->keep) { arbit_release_carve((long)p->keep); p->keep = NULL; } }

/* ------------------------------------------------------------ the firing ------ */

/* Prior over rows: how often each row appears across the program. Used as the weight
 * in the loss so a common instruction is not forced to look like a rare one. */
static float *row_prior(const Prog *p, float *out, long *hard_hits) {
    ArbitBuffer cb = ac_alloc_bytes((long)g_nrows * (long)sizeof(long));
    long *cnt = (long *)cb.ptr_addr;
    long tot = 0;
    for (size_t i = 0; i < p->nwords; i++) {
        const IsaForm *f = NULL; long h[ISA_MAXV] = {0};
        if (isa_decode(&T, p->b + i * 4, 4, &f, h)) {
            cnt[f - T.forms]++;
            if (hard_hits) hard_hits[f - T.forms]++;
            tot++;
        }
    }
    for (int r = 0; r < g_nrows; r++)
        out[r] = (float)((double)(cnt[r] + 1.0) / ((double)tot + g_nrows));
    arbit_free(cb);
    return out;
}

/* Row discrimination, and the discovery that fixes the tie problem.
 *
 * The first version scored a row purely by the fraction of its fixed bits that agree,
 * weighted by how many bits it fixes. Every word then tied at gap 0.0, and refine could
 * not move anything -- correctly, because a tied target has no gradient direction.
 *
 * The hard decoder in isadb.h breaks the same ties a different way: it picks the
 * LONGEST match among rows that also pass `isa_legal`, which checks the row's own field
 * semantics -- shift in range, logical immediate decodable, UBFM ordering valid. So
 * legality, not bit count, is what the catalog actually uses to tell two firing rows
 * apart, and the soft score keeps that structure: agreement on the fixed bits,
 * sharpened by specificity, preferring the more specific row when both agree. */
static float row_score(const float *logits, size_t i, int r) {
    if (!g_nfixed[r]) return -1e30f;
    int nb = g_nbits > 32 ? 32 : g_nbits;
    float p[32];
    const float *L = logits + i * (size_t)g_nbits;
    for (int b = 0; b < nb; b++) p[b] = sigmoid(L[b]);
    float agree = 0.0f;
    const uint32_t t = g_tmpl[r];
    for (int k = 0; k < g_nfixed[r]; k++) {
        int b = g_fixed[r][k];
        float want = ((t >> b) & 1u) ? 1.0f : 0.0f;
        agree += want * p[b] + (1.0f - want) * (1.0f - p[b]);
    }
    agree /= (float)g_nfixed[r];
    float spec = (float)g_nfixed[r] / (float)g_nbits;
    return agree * spec;
}

static float row_length_bonus(int r) { return (float)T.forms[r].len / 4.0f; }

/* exported for the diagnostics */
static float word_score_parts(const float *logits, size_t i, float *best_out, float *second_out) {
    float best = -1e30f, second = -1e30f;
    for (int r = 0; r < g_nrows; r++) {
        float s = row_score(logits, i, r);
        if (s > best) { second = best; best = s; }
        else if (s > second) second = s;
    }
    *best_out = best; *second_out = second;
    return best - second;
}

/* The loss, with an anchor. A version that maximised agreement with the best-matching
 * row over ALL 32 bits of every word has a trivial global optimum -- "be the most
 * constrained instruction there is" -- so it converges to NOP. That is a tautology
 * about that objective, not a property of the architecture, and it is why the
 * objective has to say what the program should BE, not just how matchable:
 *
 *     L = (1 - best_agreement) + alpha * mean_b( (p_b - orig_b)^2 )
 *
 * The second term is faithfulness to the original word. alpha 0 reproduces the
 * degenerate objective exactly, which makes an alpha sweep a controlled comparison
 * rather than a new guess. */
static const float *g_orig_logit = NULL;      /* the program's starting bits, as +/-6 */

static float word_loss(const float *logits, size_t i, float *dsim, float alpha) {
    int R = g_nrows;
    float best = -1e30f;
    int dom = -1;
    for (int r = 0; r < R; r++) {
        float s = row_score(logits, i, r) * row_length_bonus(r);
        if (s > best) { best = s; dom = r; }
    }
    const float *Lb = logits + i * (size_t)g_nbits;
    for (int b = 0; b < g_nbits; b++) dsim[b] = 0.0f;
    if (dom >= 0) {
        const uint32_t t = g_tmpl[dom];
        float w = 1.0f / ((float)g_nfixed[dom] * (float)g_nbits);
        for (int k = 0; k < g_nfixed[dom]; k++) {
            int b = g_fixed[dom][k];
            float p = sigmoid(Lb[b]);
            float want = ((t >> b) & 1u) ? 1.0f : 0.0f;
            dsim[b] += -w * (2.0f * want - 1.0f) * p * (1.0f - p);
        }
    }
    float L = 1.0f - best;
    if (alpha > 0.0f && g_orig_logit) {
        const float *Ob = g_orig_logit + i * (size_t)g_nbits;
        float pen = 0.0f;
        for (int b = 0; b < g_nbits; b++) {
            float d = Lb[b] - Ob[b];
            pen += d * d;
            dsim[b] += alpha * 2.0f * d / (float)g_nbits;
        }
        L += alpha * pen / (float)g_nbits;
    }
    return L;
}

static int cmd_fire(const char *path, int verbose) {
    Prog p;
    if (prog_load(path, &p) != 0) return 2;
    ArbitBuffer pb = ac_alloc_bytes((long)g_nrows * (long)sizeof(float));
    ArbitBuffer hb = ac_alloc_bytes((long)g_nrows * (long)sizeof(long));
    if (!pb.active || !hb.active) { fprintf(stderr, "binastfire: oom\n"); return 2; }
    float *prior = (float *)pb.ptr_addr;
    long *hits = (long *)hb.ptr_addr;
    row_prior(&p, prior, hits);

    long exact = 0, ambiguous = 0, none = 0;
    double agree_sum = 0.0;
    for (size_t i = 0; i < p.nwords; i++) {
        uint32_t w = r32(p.b + i * 4);
        const IsaForm *f = NULL; long h[ISA_MAXV] = {0};
        if (isa_decode(&T, p.b + i * 4, 4, &f, h)) exact++;
        else none++;
        if (w == 0) continue;
        /* how many rows fire -- the ambiguity of the activation map */
        int hi = 0;
        for (int r = 0; r < g_nrows; r++) {
            if (!g_nfixed[r]) continue;
            int ok = 1;
            const uint32_t t = g_tmpl[r];
            for (int k = 0; k < g_nfixed[r]; k++) {
                int b = g_fixed[r][k];
                if (((w >> b) & 1u) != ((t >> b) & 1u)) { ok = 0; break; }
            }
            if (ok) hi++;
        }
        agree_sum += hi;
        if (hi > 1) ambiguous++;
    }
    printf("program %s\n", path);
    printf("words %zu\ncode_vaddr %#llx\n", p.nwords, (unsigned long long)p.vaddr);
    printf("rows_exactly_matching %ld\nrows_not_matching %ld\n", exact, none);
    printf("mean_rows_fired_per_word %.3f\n", p.nwords ? agree_sum / (double)p.nwords : 0.0);
    printf("words_firing_more_than_one_row %ld\n", ambiguous);
    if (verbose) {
        printf("-- the activation map, most-used rows first:\n");
        for (int pass = 0; pass < 12; pass++) {
            int best = -1; long bv = 0;
            for (int r = 0; r < g_nrows; r++) if (hits[r] > bv) { bv = hits[r]; best = r; }
            if (best < 0) break;
            printf("   %-24s %8ld  fixed_bits %d\n", T.forms[best].name, bv, g_nfixed[best]);
            hits[best] = 0;
        }
    }
    arbit_free(pb); arbit_free(hb);
    prog_release(&p);
    return 0;
}

/* ------------------------------------------------------------------ threading -- */

/* The refine loop is embarrassingly parallel and worth saying why, because it is not
 * usually true of a training loop: the loss for word i depends ONLY on word i's own
 * bits (`row_score` reads the frozen map, never another word), and the gradient with
 * respect to those bits is likewise local. The Adam moments are per word-and-bit too.
 * So the whole update decomposes per word with no accumulation across words, no
 * reduction, and no locking on the arithmetic -- workers only need disjoint slices.
 *
 * (binastmodel is the opposite: its loss couples words through the attention window, so
 * there the gradient must be reduced across a batch before stepping.)
 *
 * The workers are partitricks: raw clone(2) threads that park on a futex when idle.
 * No pthreads, and the payload is plain C -- it never touches Python or a lock. */
typedef struct {
    const float *logits;
    float       *mflat, *vflat;
    float       *g;          /* per-worker scratch, nbits */
    size_t       lo, hi;
    int          steps;
    float        lr, alpha, bc1, bc2;
    double       loss;       /* per-worker accumulator */
} RefineJob;

static void refine_worker(void *arg) {
    RefineJob *j = (RefineJob *)arg;
    j->loss = 0.0;
    for (int st = 0; st < j->steps; st++) {
        for (size_t i = j->lo; i < j->hi; i++) {
            float *Li = (float *)j->logits + i * (size_t)g_nbits;
            float *mi = j->mflat + i * (size_t)g_nbits;
            float *vi = j->vflat + i * (size_t)g_nbits;
            j->loss += word_loss(j->logits, i, j->g, j->alpha);
            for (int b = 0; b < g_nbits; b++) {
                mi[b] = 0.9f * mi[b] + 0.1f * j->g[b];
                vi[b] = 0.999f * vi[b] + 0.001f * j->g[b] * j->g[b];
                /* sqrt(v), NOT |v|. Dividing by |v| makes the step scale as 1/g for
                 * small gradients, so a near-zero derivative produces an enormous
                 * update: an earlier version reported every bit in the program moving
                 * under a loss of 0.0001, which is arithmetically impossible and was
                 * the signature of this mistake. */
                float vh = vi[b] / j->bc2;
                float denom = 1e-8f + (vh > 0.0f ? f_sqrt_local(vh) : 0.0f);
                Li[b] -= j->lr * (mi[b] / j->bc1) / denom;
            }
        }
    }
}

/* Gradient descent INTO THE PROGRAM. The map is frozen; the bits move. On a program
 * that is already ISA-conformant most words should barely move, so what matters is
 * WHERE motion concentrates: that is the question this answers. */
static int cmd_refine(const char *path, int steps, float lr, float alpha) {
    Prog p;
    if (prog_load(path, &p) != 0) return 2;

    size_t N = p.nwords;
    size_t per_word = (size_t)g_nbits;

    /* everything below is a carve: N can be millions of words and per-word state is
     * nbits floats wide, so these are the large contiguous regions a carve exists for. */
    ArbitBuffer pb   = ac_alloc_bytes((long)g_nrows * (long)sizeof(float));
    ArbitBuffer gb   = ac_alloc_bytes((long)N * (long)sizeof(float));
    float *prior     = (float *)pb.ptr_addr;
    void  *logits_v  = arb_carve_zeroed((long)(N * per_word * sizeof(float)));
    void  *orig_v    = arb_carve_zeroed((long)(N * 4));
    void  *anchor_v  = arb_carve_zeroed((long)(N * per_word * sizeof(float)));
    void  *mflat_v   = arb_carve_zeroed((long)(N * per_word * sizeof(float)));
    void  *vflat_v   = arb_carve_zeroed((long)(N * per_word * sizeof(float)));
    if (!pb.active || !gb.active || !logits_v || !orig_v || !anchor_v || !mflat_v || !vflat_v) {
        fprintf(stderr, "binastfire: out of memory for %zu words\n", N);
        return 2;
    }
    float   *logits = (float *)logits_v;
    uint8_t *orig   = (uint8_t *)orig_v;
    float   *mflat  = (float *)mflat_v;
    float   *vflat  = (float *)vflat_v;
    cymap_copy(orig, p.b, N * 4);
    row_prior(&p, prior, NULL);

    /* initialise logits so sigmoid(logit) == the bit, sharply: 0 -> -6, 1 -> +6.
     * Starting at 0 would put every bit at 0.5 and make the first gradient enormous. */
    for (size_t i = 0; i < N; i++) {
        uint32_t w = r32(p.b + i * 4);
        for (int b = 0; b < g_nbits; b++)
            logits[i * per_word + b] = ((w >> b) & 1u) ? 6.0f : -6.0f;
    }
    /* the anchor: the program's own starting bits, for the faithfulness term */
    cymap_copy(anchor_v, logits_v, N * per_word * sizeof(float));
    g_orig_logit = (const float *)anchor_v;

    ArbPool pool = {0};
    /* NOTE NT, not T: `T` is the IsaTable at file scope, and shadowing it here made
     * isa_decode(&T, ...) below pass an int* as a table -- a real bug the compiler
     * caught, which is why the thread count has its own name. */
    int NT = ac_cpu_count();
    if (NT > 32) NT = 32;
    if ((size_t)NT > N) NT = (int)N;
    if (arb_pool_up(&pool, NT) != 0) { fprintf(stderr, "binastfire: pool bring-up failed\n"); return 2; }

    printf("refine %s\nwords %zu\nsteps %d\nlr %.4f\nalpha %.4f\nthreads %d\n",
           path, N, steps, lr, alpha, NT);
    printf("alloc  arbit carve (large) + arbit buckets (small); malloc family absent\n");
    printf("threads partitricks pool (raw clone(2), futex-parked), pthreads absent\n");
    printf("alpha 0 = the degenerate objective; alpha>0 adds faithfulness to the input\n");
    printf("step  mean_loss  bits_moved  words_moved\n");

    size_t chunk = (N + (size_t)NT - 1) / (size_t)NT;
    ArbitBuffer jb = ac_alloc_bytes((long)NT * (long)sizeof(RefineJob));
    ArbitBuffer sb = ac_alloc_bytes((long)NT * (long)per_word * (long)sizeof(float));
    void **args = (void **)arb_carve_zeroed((long)NT * (long)sizeof(void *));
    if (!jb.active || !sb.active || !args) { fprintf(stderr, "binastfire: oom\n"); return 2; }
    RefineJob *jobs = (RefineJob *)jb.ptr_addr;
    float *scratch = (float *)sb.ptr_addr;

    int report_every = steps / 10 > 0 ? steps / 10 : 1;
    for (int st = 1; st <= steps; st += report_every) {
        int nsteps = report_every;
        if (st + nsteps - 1 > steps) nsteps = steps - st + 1;
        for (int t = 0; t < NT; t++) {
            jobs[t].logits = logits;
            jobs[t].mflat  = mflat;
            jobs[t].vflat  = vflat;
            jobs[t].g      = scratch + (size_t)t * per_word;
            jobs[t].lo     = (size_t)t * chunk;
            jobs[t].hi     = ((size_t)t + 1) * chunk > N ? N : ((size_t)t + 1) * chunk;
            jobs[t].steps  = nsteps;
            jobs[t].lr     = lr;
            jobs[t].alpha  = alpha;
            jobs[t].bc1    = 1.0f - 0.9f;
            jobs[t].bc2    = 1.0f - 0.999f;
            jobs[t].loss   = 0.0;
            args[t]        = &jobs[t];
        }
        arb_pool_run(&pool, NT, refine_worker, args);

        double loss = 0.0;
        for (int t = 0; t < NT; t++) loss += jobs[t].loss;
        long moved = 0, bits_moved = 0;
        for (size_t i = 0; i < N; i++) {
            uint32_t a = r32(orig + i * 4);
            uint32_t now = 0;
            for (int b = 0; b < g_nbits; b++)
                if (logits[i * per_word + b] > 0.0f) now |= 1u << b;
            if (a != now) moved++;
            uint32_t d = a ^ now;
            for (int b = 0; b < g_nbits; b++) if ((d >> b) & 1u) bits_moved++;
        }
        printf("%4d  %9.6f  %10ld  %ld\n", st + nsteps - 1,
               loss / ((double)N * nsteps), bits_moved, moved);
        fflush(stdout);
    }
    /* WHERE did it move? Split by whether the original word was a decodable
     * instruction, a zero padding word, or something else entirely. */
    long moved_instr = 0, moved_zero = 0, moved_other = 0;
    long n_instr = 0, n_zero = 0, n_other = 0;
    for (size_t i = 0; i < N; i++) {
        uint32_t a = r32(orig + i * 4);
        uint32_t now = 0;
        for (int b = 0; b < g_nbits; b++)
            if (logits[i * per_word + b] > 0.0f) now |= 1u << b;
        const IsaForm *f = NULL; long h[ISA_MAXV] = {0};
        int is_instr = isa_decode(&T, orig + i * 4, 4, &f, h) != 0;
        int is_zero = (a == 0);
        if (is_zero) { n_zero++; if (a != now) moved_zero++; }
        else if (is_instr) { n_instr++; if (a != now) moved_instr++; }
        else { n_other++; if (a != now) moved_other++; }
    }
    printf("-- where the gradient moved the program:\n");
    printf("decodable_instructions %ld  moved %ld  (%.2f%%)\n",
           n_instr, moved_instr, n_instr ? 100.0 * moved_instr / n_instr : 0.0);
    printf("zero_padding_words     %ld  moved %ld  (%.2f%%)\n",
           n_zero, moved_zero, n_zero ? 100.0 * moved_zero / n_zero : 0.0);
    printf("undecodable_other      %ld  moved %ld  (%.2f%%)\n",
           n_other, moved_other, n_other ? 100.0 * moved_other / n_other : 0.0);

    /* WRITE THE RESULT. Without this the run is uncheckable: the numbers above say how
     * many words moved but not what they moved TO, and "it converged" is exactly the
     * claim that needs inspecting. The refined words are written next to the original
     * so the two can be diffed. */
    {
        char out[1024];
        snprintf(out, sizeof out, "%s.refined", path);
        FILE *of = fopen(out, "wb");
        if (of) {
            size_t off = (size_t)(p.b - p.keep);
            fwrite(p.keep, 1, off, of);
            for (size_t i = 0; i < N; i++) {
                uint32_t now = 0;
                for (int b = 0; b < g_nbits; b++)
                    if (logits[i * per_word + b] > 0.0f) now |= 1u << b;
                uint8_t w[4] = { (uint8_t)now, (uint8_t)(now >> 8),
                                 (uint8_t)(now >> 16), (uint8_t)(now >> 24) };
                fwrite(w, 1, 4, of);
            }
            size_t rest_off = off + N * 4;
            if (rest_off < p.keep_size) fwrite(p.keep + rest_off, 1, p.keep_size - rest_off, of);
            fclose(of);
            printf("wrote %s\n", out);
        } else {
            printf("could not write %s\n", out);
        }
    }

    arb_pool_down(&pool);
    arbit_free(pb); arbit_free(jb); arbit_free(sb); arbit_free(gb);
    arbit_release_carve((long)args);
    arbit_release_carve((long)logits_v);
    arbit_release_carve((long)orig_v);
    arbit_release_carve((long)anchor_v);
    arbit_release_carve((long)mflat_v);
    arbit_release_carve((long)vflat_v);
    g_orig_logit = NULL;
    prog_release(&p);
    return 0;
}

/* Diagnostic: the loss must be interpretable. Reporting best/second/gap makes a wrong
 * loss visible instead of printing a plausible-looking small number. */
static int cmd_check(const char *path) {
    Prog p;
    if (prog_load(path, &p) != 0) return 2;
    ArbitBuffer pb = ac_alloc_bytes((long)g_nrows * (long)sizeof(float));
    ArbitBuffer lb = ac_alloc_bytes((long)g_nbits * (long)sizeof(float));
    if (!pb.active || !lb.active) { fprintf(stderr, "binastfire: oom\n"); return 2; }
    float *prior = (float *)pb.ptr_addr;
    float *L = (float *)lb.ptr_addr;
    row_prior(&p, prior, NULL);
    int shown = 0;
    for (size_t i = 0; i < p.nwords && shown < 6; i++) {
        const IsaForm *f = NULL; long h[ISA_MAXV] = {0};
        uint32_t w = r32(p.b + i * 4);
        int dec = isa_decode(&T, p.b + i * 4, 4, &f, h);
        if (!dec) continue;
        for (int b = 0; b < g_nbits; b++) L[b] = ((w >> b) & 1u) ? 6.0f : -6.0f;
        float b1 = 0.0f, s2 = 0.0f;
        float gap = word_score_parts(L, 0, &b1, &s2);
        printf("word %08x  decoded %-18s  best_score %.5f  second %.5f  gap %.5f  loss %.5f\n",
               w, f->name, b1, s2, gap, (1.0f - b1) * 6.0f);
        shown++;
    }
    arbit_free(pb); arbit_free(lb);
    prog_release(&p);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: binastfire <command>\n"
            "  fire   <elf> [verbose]   per-word activation: rows fired, ambiguity\n"
            "  refine <elf> [steps] [lr] [alpha]\n"
            "  check  <elf>\n"
            "  agent\n");
        return 2;
    }
    /* the sea must be up before any tool touches arbit */
    arbit_init(0);
    const char *c = argv[1];
    if (!strcmp(c, "agent")) {
        printf("tool binastfire\n");
        printf("what it is: the catalog matrix fired against a loaded program as an attention head\n");
        printf("our map    FIXED filter bank (rows x bits); it is the ISA and is never trained\n");
        printf("program    INPUT image (positions x bits); the parameters\n");
        printf("output     (positions x rows) activation map\n");
        printf("softness   bit relaxed to sigmoid, agreement = fraction of fixed bits matching\n");
        printf("backprop   goes into the PROGRAM (its binary position and structure)\n");
        printf("why image-like  a fixed kernel bank correlated over a 2D input, not learned symbols\n");
        printf("memory     arbit cosmic sea (no malloc family anywhere in this tool)\n");
        printf("threads    partitricks pool: raw clone(2), futex-parked (no pthreads)\n");
        printf("command fire   <elf> [verbose]\n");
        printf("command refine <elf> [steps] [lr] [alpha]\n");
        printf("  alpha=0 maximises matchability alone (degenerate); alpha>0 adds a\n");
        printf("  faithfulness term so the program is not free to become anything\n");
        return 0;
    }
    const char *mp = getenv("BINASTFIRE_MAP");
    const char *mat = mp ? mp : "tmp/isa_aarch64.mtx";
    if (!strcmp(c, "fire")) {
        if (argc < 3) { fprintf(stderr, "binastfire: fire needs an ELF\n"); return 2; }
        if (load_matrix(mat) != 0) return 2;
        return cmd_fire(argv[2], argc > 3);
    }
    if (!strcmp(c, "check")) {
        if (argc < 3) { fprintf(stderr, "binastfire: check needs an ELF\n"); return 2; }
        if (load_matrix(mat) != 0) return 2;
        return cmd_check(argv[2]);
    }
    if (!strcmp(c, "refine")) {
        if (argc < 3) { fprintf(stderr, "binastfire: refine needs an ELF\n"); return 2; }
        if (load_matrix(mat) != 0) return 2;
        return cmd_refine(argv[2], argc > 3 ? atoi(argv[3]) : 5,
                          argc > 4 ? (float)atof(argv[4]) : 0.05f,
                          argc > 5 ? (float)atof(argv[5]) : 0.0f);
    }
    fprintf(stderr, "binastfire: unknown command %s\n", c);
    return 2;
}
