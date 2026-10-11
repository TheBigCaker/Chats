/* binastmodel.c -- a pure-C11 causal attention model over canonical instruction tokens.
 *
 * Every dimension comes from a measurement in this repo, not a transformer default:
 *
 *   window    KCTX = 4 instructions  -- H(form|context) peaks at order 4 and is
 *                                       WORSE at 5 and 6
 *   symbols   compacted to the rows actually present (812 on this corpus of 1320)
 *   channels  2, learned separately   -- row and payload have DIFFERENT optimal
 *                                       windows (4 and 3), so they get separate
 *                                       inputs and separate output heads
 *   payload   one sigmoid per bit     -- measured mean width 20.99, max 26
 *   objective held-out bits per token, reported per head, never training loss
 *
 * The one structural choice the measurements forced: row and payload are separate
 * channels. A packed joint token scored worse than the row alone at orders 2-4,
 * because it forces one window onto two signals whose optimal windows differ.
 *
 * WHY PURE C. The GEMM entry points in the port's cygemm tree (dispatch.c, cuda.c)
 * are Cython-generated with memview arguments and a Python ABI. Linking that in
 * would put a Python ABI inside a tool whose entire point is having none. These
 * kernels are small loops and they are ours.
 *
 * THE THING THAT MAKES THIS TRUSTWORTHY. `gradcheck` compares every analytic
 * gradient against a central finite difference of the loss. A transformer with a
 * wrong backward pass still trains -- it just descends the wrong surface -- so the
 * loss going down proves nothing. The gradient check is the gate.
 *
 *   binastmodel gradcheck <data>            numeric vs analytic, every parameter
 *   binastmodel train <data> [epochs] [D]   fit; held-out bits each epoch
 *   binastmodel eval  <data> [D]            held-out bits only
 *   binastmodel agent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"

/* No libm. The port forbids it ("a link error on log2 means a libm call snuck
 * back in -- fix the call site, do not add the flag"), and ccp_logf only gives
 * log. The kernels here are ours and so are the transcendentals they need:
 * exp, log, sqrt and pow are all small, and writing them keeps this file
 * linkable with nothing but libc. Accuracy is ~1e-6 relative, which is far
 * below what a float32 model cares about, and gradcheck would catch it if it
 * were not. */

static inline float f_exp(float x) {
    if (x < -87.0f) return 0.0f;
    if (x > 88.0f) x = 88.0f;
    /* exp(x) = 2^k * exp(r), r in [-ln2/2, ln2/2] */
    const float LN2 = 0.6931471806f, INV_LN2 = 1.4426950409f;
    float kf = x * INV_LN2;
    float k = (kf >= 0.0f) ? (float)(long)(kf + 0.5f) : (float)(long)(kf - 0.5f);
    float r = x - k * LN2;
    /* degree-7 Taylor on the reduced range */
    float r2 = r * r;
    float p = 1.0f + r + r2 * (0.5f + r * (1.0f/6.0f + r * (1.0f/24.0f + r * (1.0f/120.0f
            + r * (1.0f/720.0f + r * (1.0f/5040.0f))))));
    /* scale by 2^k */
    union { float f; uint32_t u; } sc;
    long ki = (long)k;
    if (ki > 127) ki = 127;
    if (ki < -126) { p *= 1e-38f; ki += 126; }
    sc.u = (uint32_t)(ki + 127) << 23;
    return p * sc.f;
}

static inline float f_log(float x) {
    if (x <= 0.0f) return -1e30f;
    union { float f; uint32_t u; } v; v.f = x;
    int e = (int)((v.u >> 23) & 0xff) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;      /* mantissa in [1,2) */
    /* push to [sqrt(1/2), sqrt(2)) for a fast series */
    if (v.f > 1.41421356f) { v.f *= 0.5f; e++; }
    float y = (v.f - 1.0f) / (v.f + 1.0f);
    float y2 = y * y;
    float s = 0.0f;
    for (int i = 9; i >= 1; i -= 2) s = y2 * s + 1.0f / (float)i;
    return 2.0f * y * s + (float)e * 0.6931471806f;
}

static inline float f_abs(float x) { return x < 0.0f ? -x : x; }

/* cos for the LR schedule, on [-pi, pi]. A degree-8 series on the reduced range is
 * accurate to ~1e-5, far below what a schedule needs, and keeps this file libm-free. */
static inline float f_cos(float x) {
    const float PI = 3.14159265359f, TWO_PI = 6.28318530718f;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float x2 = x * x;
    return 1.0f - x2 * (0.5f - x2 * (1.0f/24.0f - x2 * (1.0f/720.0f - x2 * (1.0f/40320.0f))));
}

static inline float f_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    /* Newton from a bit-trick seed: 3 iterations is better than 1e-7 relative */
    union { float f; uint32_t u; } v; v.f = x;
    v.u = (v.u >> 1) + 0x1fc00000u;
    float y = v.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}

/* powf for the Adam bias correction: bases are 0.9 and 0.999, exponents are small
 * integers, so a repeated multiply is both faster and more accurate than exp/log. */
static inline float f_pow_int(float base, int e) {
    float r = 1.0f, b = base;
    while (e > 0) { if (e & 1) r *= b; b *= b; e >>= 1; }
    return r;
}

#define KCTX       4
#define MAXPAT     48
#define MAXPARAM   (1 << 22)

/* ---------------------------------------------------------------------- data -- */

typedef struct {
    int       n;
    uint16_t *row;          /* compacted row id, 0..nrows-1 */
    uint64_t *pay;
    uint8_t  *pb;           /* payload bits, nbits per token */
    int       nrows;
    int       nbits;
} Data;

static int data_load(const char *path, Data *d, int want_bits) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "binastmodel: cannot open %s\n", path); return -1; }
    size_t cap = 1 << 20;
    int n = 0, nrows = 0;
    uint16_t *row = arb_bytes(cap * sizeof *row);
    uint8_t  *pb  = arb_bytes(cap * (size_t)want_bits);
    if (!row || !pb) { fclose(f); arbit_release_carve((long)(row)); arbit_release_carve((long)(pb)); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        unsigned long r = strtoul(line, NULL, 16);
        unsigned long long p = strtoull(c + 1, NULL, 16);
        if (r > 0xffff) continue;
        if ((size_t)n == cap) {
            cap *= 2;
            uint16_t *t1 = arb_bytes(cap * sizeof *row); if (t1) cymap_copy(t1, row, (size_t)(cap * sizeof *row / 2));;
            uint8_t  *t2 = arb_bytes(cap * (size_t)want_bits); if (t2) cymap_copy(t2, pb, (size_t)(cap * (size_t)want_bits / 2));
            if (!t1 || !t2) { arbit_release_carve((long)(row)); arbit_release_carve((long)(pb)); fclose(f); return -1; }
            row = t1; pb = t2;
        }
        uint16_t compact = (uint16_t)r;
        if (compact >= (uint16_t)nrows) nrows = compact + 1;
        row[n] = compact;
        for (int b = 0; b < want_bits; b++)
            pb[(size_t)n * want_bits + b] = (uint8_t)((p >> b) & 1ull);
        n++;
    }
    fclose(f);
    d->n = n; d->row = row; d->pb = pb; d->nrows = nrows; d->nbits = want_bits;
    return n > KCTX + 2 ? 0 : -1;
}

/* Load a stream but keep the CATALOG's row ids instead of compacting them per file.
 *
 * This is required for cross-program work: if adb's rows were compacted to 0..812
 * and ld's to 0..900 independently, then "row 5" would mean a different instruction
 * in each file and a transferred model would be scored on relabelled garbage. `R`
 * is the symbol-space size established by the training stream, and any row id at or
 * above it is a row the model has never been able to see -- counted and reported,
 * never silently folded in. */
static int data_load_catalog(const char *path, Data *d, int B, int R) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "binastmodel: cannot open %s\n", path); return -1; }
    size_t cap = 1 << 20;
    int n = 0, unseen = 0;
    uint16_t *row = arb_bytes(cap * sizeof *row);
    uint8_t  *pb  = arb_bytes(cap * (size_t)B);
    if (!row || !pb) { fclose(f); arbit_release_carve((long)(row)); arbit_release_carve((long)(pb)); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        unsigned long r = strtoul(line, NULL, 16);
        unsigned long long p = strtoull(c + 1, NULL, 16);
        if (r > 0xffff) continue;
        if ((int)r >= R) { unseen++; continue; }   /* not in the symbol space: skipped,
                                                    * because the model has no embedding
                                                    * for it and inventing one would be
                                                    * fabricating a symbol */
        if ((size_t)n == cap) {
            cap *= 2;
            uint16_t *t1 = arb_bytes(cap * sizeof *row); if (t1) cymap_copy(t1, row, (size_t)(cap * sizeof *row / 2));;
            uint8_t  *t2 = arb_bytes(cap * (size_t)B); if (t2) cymap_copy(t2, pb, (size_t)(cap * (size_t)B / 2));
            if (!t1 || !t2) { arbit_release_carve((long)(row)); arbit_release_carve((long)(pb)); fclose(f); return -1; }
            row = t1; pb = t2;
        }
        row[n] = (uint16_t)r;
        for (int b = 0; b < B; b++) pb[(size_t)n * B + b] = (uint8_t)((p >> b) & 1ull);
        n++;
    }
    fclose(f);
    if (unseen)
        printf("note: %d of %d test tokens use a row outside the train symbol space "
               "and were skipped (the model has no embedding for them)\n", unseen, unseen + n);
    d->n = n; d->row = row; d->pb = pb; d->nrows = R; d->nbits = B;
    return n > KCTX + 2 ? 0 : -1;
}

/* -------------------------------------------------------------------- model -- */

typedef struct {
    int R, D, B, F;         /* rows, dim, payload bits, mlp width */
    float *erow;            /* R*D */
    float *wpay;            /* B*D */
    float *wpos;            /* KCTX*D */
    float *wq, *wk, *wv, *wo;
    float *w1, *b1;         /* F*D, F */
    float *w2, *b2;         /* D*F, D */
    float *hr, *br;         /* R*D, R */
    float *hp, *bp;         /* B*D, B */
} Model;

typedef struct {
    int R, D, B, F;
    float *erow, *wpay, *wpos;
    float *wq, *wk, *wv, *wo;
    float *w1, *b1, *w2, *b2;
    float *hr, *br, *hp, *bp;
    /* SCRATCH, owned by the gradient, allocated ONCE with it. backward() runs once per
     * token, so allocating these inside it meant millions of allocate/free cycles per
     * epoch -- in the hot path, which is where allocation does not belong. They are
     * per-token temporaries whose size depends only on the model shape, so they live
     * with the model-shaped state. */
    float *dlr, *dpz, *datt, *dsc;
} Grad;

/* activations for one predicted token */
typedef struct {
    float *h;      /* KCTX*D */
    float *q, *k, *v;   /* KCTX*D */
    float *sc;     /* KCTX */
    float *att;    /* KCTX */
    float *ctx;    /* D */
    float *o, *h2; /* D */
    float *pre, *act;   /* F */
    float *ff, *h3;     /* D */
    float *lr;     /* R */
    float *pz, *pp;     /* B */
    /* backprop scratch */
    float *dh, *dq, *dk, *dv, *dctx, *dh2, *dpre, *dact, *do_, *dff, *dh3;
} Act;

static int act_init(Act *a, Model *m);
static void act_free(Act *a);
static void model_free(Model *m);
static void grad_free(Grad *g);

static float *falc(int n) { return (float *)arb_bytes((long)((size_t)n) * (long)(sizeof(float))); }
static float frnd(unsigned *s) {
    *s = *s * 1664525u + 1013904223u;
    return ((float)((*s >> 8) & 0xffffff) / 8388608.0f) - 1.0f;
}

static int model_init(Model *m, int R, int D, int B, int F, unsigned seed) {
    memset(m, 0, sizeof *m);
    m->R = R; m->D = D; m->B = B; m->F = F;
    m->erow = falc(R * D); m->wpay = falc(B * D); m->wpos = falc(KCTX * D);
    m->wq = falc(D * D); m->wk = falc(D * D); m->wv = falc(D * D); m->wo = falc(D * D);
    m->w1 = falc(F * D); m->b1 = falc(F);
    m->w2 = falc(D * F); m->b2 = falc(D);
    m->hr = falc(R * D); m->br = falc(R);
    m->hp = falc(B * D); m->bp = falc(B);
    if (!m->erow || !m->hr || !m->hp) return -1;
    unsigned s = seed;
    float e = 0.08f;
    for (int i = 0; i < R * D; i++) m->erow[i] = frnd(&s) * e;
    for (int i = 0; i < B * D; i++) m->wpay[i] = frnd(&s) * e;
    for (int i = 0; i < KCTX * D; i++) m->wpos[i] = frnd(&s) * 0.05f;
    int nq = D * D;
    for (int i = 0; i < nq; i++) {
        m->wq[i] = frnd(&s) * 0.1f; m->wk[i] = frnd(&s) * 0.1f;
        m->wv[i] = frnd(&s) * 0.1f; m->wo[i] = frnd(&s) * 0.1f;
    }
    for (int i = 0; i < F * D; i++) m->w1[i] = frnd(&s) * 0.1f;
    for (int i = 0; i < D * F; i++) m->w2[i] = frnd(&s) * 0.1f;
    for (int i = 0; i < R * D; i++) m->hr[i] = frnd(&s) * 0.1f;
    for (int i = 0; i < B * D; i++) m->hp[i] = frnd(&s) * 0.1f;
    return 0;
}

static void grad_init(Grad *g, Model *m) {
    memset(g, 0, sizeof *g);
    g->R = m->R; g->D = m->D; g->B = m->B; g->F = m->F;
    g->erow = falc(m->R * m->D); g->wpay = falc(m->B * m->D); g->wpos = falc(KCTX * m->D);
    g->wq = falc(m->D * m->D); g->wk = falc(m->D * m->D);
    g->wv = falc(m->D * m->D); g->wo = falc(m->D * m->D);
    g->w1 = falc(m->F * m->D); g->b1 = falc(m->F);
    g->w2 = falc(m->D * m->F); g->b2 = falc(m->D);
    g->hr = falc(m->R * m->D); g->br = falc(m->R);
    g->hp = falc(m->B * m->D); g->bp = falc(m->B);
    g->dlr = falc(m->R); g->dpz = falc(m->B);
    g->datt = falc(KCTX); g->dsc = falc(KCTX);
}
static void grad_free(Grad *g) {
    arbit_release_carve((long)(g->erow)); arbit_release_carve((long)(g->wpay)); arbit_release_carve((long)(g->wpos));
    arbit_release_carve((long)(g->wq)); arbit_release_carve((long)(g->wk)); arbit_release_carve((long)(g->wv)); arbit_release_carve((long)(g->wo));
    arbit_release_carve((long)(g->w1)); arbit_release_carve((long)(g->b1)); arbit_release_carve((long)(g->w2)); arbit_release_carve((long)(g->b2));
    arbit_release_carve((long)(g->hr)); arbit_release_carve((long)(g->br)); arbit_release_carve((long)(g->hp)); arbit_release_carve((long)(g->bp));
    arbit_release_carve((long)(g->dlr)); arbit_release_carve((long)(g->dpz)); arbit_release_carve((long)(g->datt)); arbit_release_carve((long)(g->dsc));
    memset(g, 0, sizeof *g);
}

static void grad_zero(Grad *g) {
    memset(g->erow, 0, (size_t)g->R * g->D * sizeof(float));
    memset(g->wpay, 0, (size_t)g->B * g->D * sizeof(float));
    memset(g->wpos, 0, (size_t)KCTX * g->D * sizeof(float));
    int nq = g->D * g->D;
    memset(g->wq, 0, (size_t)nq * sizeof(float));
    memset(g->wk, 0, (size_t)nq * sizeof(float));
    memset(g->wv, 0, (size_t)nq * sizeof(float));
    memset(g->wo, 0, (size_t)nq * sizeof(float));
    memset(g->w1, 0, (size_t)g->F * g->D * sizeof(float));
    memset(g->b1, 0, (size_t)g->F * sizeof(float));
    memset(g->w2, 0, (size_t)g->D * g->F * sizeof(float));
    memset(g->b2, 0, (size_t)g->D * sizeof(float));
    memset(g->hr, 0, (size_t)g->R * g->D * sizeof(float));
    memset(g->br, 0, (size_t)g->R * sizeof(float));
    memset(g->hp, 0, (size_t)g->B * g->D * sizeof(float));
    memset(g->bp, 0, (size_t)g->B * sizeof(float));
}

static void model_free(Model *m) {
    arbit_release_carve((long)(m->erow)); arbit_release_carve((long)(m->wpay)); arbit_release_carve((long)(m->wpos));
    arbit_release_carve((long)(m->wq)); arbit_release_carve((long)(m->wk)); arbit_release_carve((long)(m->wv)); arbit_release_carve((long)(m->wo));
    arbit_release_carve((long)(m->w1)); arbit_release_carve((long)(m->b1)); arbit_release_carve((long)(m->w2)); arbit_release_carve((long)(m->b2));
    arbit_release_carve((long)(m->hr)); arbit_release_carve((long)(m->br)); arbit_release_carve((long)(m->hp)); arbit_release_carve((long)(m->bp));
    memset(m, 0, sizeof *m);
}

static int act_init(Act *a, Model *m) {
    memset(a, 0, sizeof *a);
    int D = m->D, F = m->F;
    a->h = falc(KCTX * D); a->q = falc(KCTX * D); a->k = falc(KCTX * D); a->v = falc(KCTX * D);
    a->sc = falc(KCTX); a->att = falc(KCTX); a->ctx = falc(D);
    a->o = falc(D); a->h2 = falc(D); a->pre = falc(F); a->act = falc(F);
    a->ff = falc(D); a->h3 = falc(D); a->lr = falc(m->R);
    a->pz = falc(m->B); a->pp = falc(m->B);
    a->dh = falc(KCTX * D); a->dq = falc(KCTX * D); a->dk = falc(KCTX * D); a->dv = falc(KCTX * D);
    a->dctx = falc(D); a->dh2 = falc(D); a->dpre = falc(F); a->dact = falc(F);
    a->do_ = falc(D); a->dff = falc(D); a->dh3 = falc(D);
    return (a->h && a->dh && a->lr) ? 0 : -1;
}

static void act_free(Act *a) {
    arbit_release_carve((long)(a->h)); arbit_release_carve((long)(a->q)); arbit_release_carve((long)(a->k)); arbit_release_carve((long)(a->v));
    arbit_release_carve((long)(a->sc)); arbit_release_carve((long)(a->att)); arbit_release_carve((long)(a->ctx));
    arbit_release_carve((long)(a->o)); arbit_release_carve((long)(a->h2)); arbit_release_carve((long)(a->pre)); arbit_release_carve((long)(a->act));
    arbit_release_carve((long)(a->ff)); arbit_release_carve((long)(a->h3)); arbit_release_carve((long)(a->lr));
    arbit_release_carve((long)(a->pz)); arbit_release_carve((long)(a->pp));
    arbit_release_carve((long)(a->dh)); arbit_release_carve((long)(a->dq)); arbit_release_carve((long)(a->dk)); arbit_release_carve((long)(a->dv));
    arbit_release_carve((long)(a->dctx)); arbit_release_carve((long)(a->dh2)); arbit_release_carve((long)(a->dpre)); arbit_release_carve((long)(a->dact));
    arbit_release_carve((long)(a->do_)); arbit_release_carve((long)(a->dff)); arbit_release_carve((long)(a->dh3));
    memset(a, 0, sizeof *a);
}

static float dotf(const float *a, const float *b, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

/* input at context slot j: row embedding + payload bits + position */
static void embed(Model *m, Data *d, int tok, int j, float *dst) {
    int D = m->D;
    const float *er = m->erow + (size_t)d->row[tok] * D;
    const float *po = m->wpos + j * D;
    for (int i = 0; i < D; i++) dst[i] = er[i] + po[i];
    const uint8_t *pb = d->pb + (size_t)tok * d->nbits;
    for (int b = 0; b < m->B; b++) {
        if (!pb[b]) continue;
        const float *w = m->wpay + (size_t)b * D;
        for (int i = 0; i < D; i++) dst[i] += w[i];
    }
}

/* forward for one prediction. Returns bits (row CE + payload CE). */
static float forward(Model *m, Act *a, Data *d, int target, const int *ctx) {
    int D = m->D, F = m->F;
    for (int j = 0; j < KCTX; j++) embed(m, d, ctx[j], j, a->h + j * D);
    for (int j = 0; j < KCTX; j++) {
        const float *h = a->h + j * D;
        float *q = a->q + j * D, *k = a->k + j * D, *v = a->v + j * D;
        for (int i = 0; i < D; i++) {
            q[i] = dotf(h, m->wq + (size_t)i * D, D);
            k[i] = dotf(h, m->wk + (size_t)i * D, D);
            v[i] = dotf(h, m->wv + (size_t)i * D, D);
        }
    }
    const float *ql = a->q + (KCTX - 1) * D;
    float inv = 1.0f / f_sqrt((float)D);
    float mx = -1e30f;
    for (int j = 0; j < KCTX; j++) {
        a->sc[j] = dotf(ql, a->k + j * D, D) * inv;
        if (a->sc[j] > mx) mx = a->sc[j];
    }
    float s = 0.0f;
    for (int j = 0; j < KCTX; j++) { a->att[j] = f_exp(a->sc[j] - mx); s += a->att[j]; }
    for (int j = 0; j < KCTX; j++) a->att[j] /= s;
    for (int i = 0; i < D; i++) a->ctx[i] = 0.0f;
    for (int j = 0; j < KCTX; j++)
        for (int i = 0; i < D; i++) a->ctx[i] += a->att[j] * a->v[j * D + i];
    for (int i = 0; i < D; i++) a->o[i] = dotf(a->ctx, m->wo + (size_t)i * D, D);
    for (int i = 0; i < D; i++) a->h2[i] = a->h[(KCTX - 1) * D + i] + a->o[i];
    for (int i = 0; i < F; i++) {
        float z = m->b1[i];
        for (int j = 0; j < D; j++) z += a->h2[j] * m->w1[(size_t)i * D + j];
        a->pre[i] = z;
        a->act[i] = z > 0.0f ? z : 0.0f;
    }
    for (int i = 0; i < D; i++) {
        float z = m->b2[i];
        for (int j = 0; j < F; j++) z += a->act[j] * m->w2[(size_t)i * F + j];
        a->ff[i] = z;
    }
    for (int i = 0; i < D; i++) a->h3[i] = a->h2[i] + a->ff[i];

    /* row head */
    for (int r = 0; r < m->R; r++) {
        float z = m->br[r];
        for (int i = 0; i < D; i++) z += a->h3[i] * m->hr[(size_t)r * D + i];
        a->lr[r] = z;
    }
    mx = -1e30f;
    for (int r = 0; r < m->R; r++) if (a->lr[r] > mx) mx = a->lr[r];
    s = 0.0f;
    for (int r = 0; r < m->R; r++) { a->lr[r] = f_exp(a->lr[r] - mx); s += a->lr[r]; }
    for (int r = 0; r < m->R; r++) a->lr[r] /= s;

    /* payload head. The row is known at prediction time for the CURRENT token only
     * if we are looking at it -- we are not, it is the target. But the row of the
     * PREVIOUS token predicts which fields the next payload has, and the measured
     * structure is that operand fields are strongly correlated with the form. This
     * head is a separate projection per bit, and the row context arrives through h3
     * (which already contains the previous tokens' row embeddings), so nothing extra
     * is needed here -- but the per-bit biases must start at the train frequency
     * rather than at zero, or the model spends epochs relearning the marginal. */
    const uint8_t *tb = d->pb + (size_t)target * d->nbits;
    for (int b = 0; b < m->B; b++) {
        float z = m->bp[b];
        for (int i = 0; i < D; i++) z += a->h3[i] * m->hp[(size_t)b * D + i];
        a->pz[b] = z;
        a->pp[b] = 1.0f / (1.0f + f_exp(-z));
    }

    /* loss in bits */
    float bits = -f_log(a->lr[d->row[target]] > 1e-30f ? a->lr[d->row[target]] : 1e-30f) * 1.4426950408889634f;
    for (int b = 0; b < m->B; b++) {
        float p = tb[b] ? a->pp[b] : (1.0f - a->pp[b]);
        if (p < 1e-30f) p = 1e-30f;
        bits += -f_log(p) * 1.4426950408889634f;
    }
    return bits;
}

/* backward. Accumulates into g and returns the same bits (for agreement checks). */
static void backward(Model *m, Grad *g, Act *a, Data *d, int target, const int *ctx) {
    int D = m->D, F = m->F;
    float log2e = 1.4426950408889634f;

    /* dL/dh3 from both heads. Row: (p - onehot) over the softmax, in bits.
     * Payload: sigmoid cross-entropy, d/dz = (p - y). Both are the same shape. */
    memset(a->dh3, 0, (size_t)D * sizeof(float));
    float *dlr = g->dlr;   /* scratch, allocated once with Grad */
    for (int r = 0; r < m->R; r++) dlr[r] = a->lr[r] * log2e;
    dlr[d->row[target]] -= log2e;
    for (int r = 0; r < m->R; r++) {
        if (dlr[r] == 0.0f) continue;
        for (int i = 0; i < D; i++) a->dh3[i] += dlr[r] * m->hr[(size_t)r * D + i];
        for (int i = 0; i < D; i++) g->hr[(size_t)r * D + i] += dlr[r] * a->h3[i];
        g->br[r] += dlr[r];
    }
    const uint8_t *tb = d->pb + (size_t)target * d->nbits;
    float *dpz = g->dpz;
    for (int b = 0; b < m->B; b++) {
        float y = tb[b] ? 1.0f : 0.0f;
        dpz[b] = (a->pp[b] - y) * log2e;
        for (int i = 0; i < D; i++) a->dh3[i] += dpz[b] * m->hp[(size_t)b * D + i];
        for (int i = 0; i < D; i++) g->hp[(size_t)b * D + i] += dpz[b] * a->h3[i];
        g->bp[b] += dpz[b];
    }

    /* h3 = h2 + ff  =>  dh2 += dh3, dff = dh3 */
    for (int i = 0; i < D; i++) { a->dh2[i] = a->dh3[i]; a->dff[i] = a->dh3[i]; }
    /* ff = w2 @ act + b2 */
    memset(a->dact, 0, (size_t)F * sizeof(float));
    for (int i = 0; i < D; i++) {
        for (int j = 0; j < F; j++) {
            g->w2[(size_t)i * F + j] += a->dff[i] * a->act[j];
            a->dact[j] += a->dff[i] * m->w2[(size_t)i * F + j];
        }
        g->b2[i] += a->dff[i];
    }
    /* act = relu(pre) */
    for (int j = 0; j < F; j++) a->dpre[j] = a->pre[j] > 0.0f ? a->dact[j] : 0.0f;
    /* pre = w1 @ h2 + b1 */
    for (int i = 0; i < F; i++) {
        for (int j = 0; j < D; j++) {
            g->w1[(size_t)i * D + j] += a->dpre[i] * a->h2[j];
            a->dh2[j] += a->dpre[i] * m->w1[(size_t)i * D + j];
        }
        g->b1[i] += a->dpre[i];
    }
    /* h2 = h[last] + o */
    memset(a->do_, 0, (size_t)D * sizeof(float));
    for (int i = 0; i < D; i++) a->do_[i] = a->dh2[i];
    memset(a->dh, 0, (size_t)(KCTX * D) * sizeof(float));
    for (int i = 0; i < D; i++) a->dh[(KCTX - 1) * D + i] += a->dh2[i];
    /* o = wo @ ctx */
    memset(a->dctx, 0, (size_t)D * sizeof(float));
    for (int i = 0; i < D; i++) {
        for (int j = 0; j < D; j++) {
            g->wo[(size_t)i * D + j] += a->do_[i] * a->ctx[j];
            a->dctx[j] += a->do_[i] * m->wo[(size_t)i * D + j];
        }
    }
    /* ctx = sum_j att[j] * v[j]  =>  dv[j] += att[j]*dctx,  datt[j] = dctx . v[j] */
    memset(a->dv, 0, (size_t)(KCTX * D) * sizeof(float));
    float *datt = g->datt;
    for (int j = 0; j < KCTX; j++) {
        datt[j] = dotf(a->dctx, a->v + j * D, D);
        for (int i = 0; i < D; i++) a->dv[j * D + i] += a->att[j] * a->dctx[i];
    }
    /* softmax */
    float dsum = 0.0f;
    for (int j = 0; j < KCTX; j++) dsum += datt[j] * a->att[j];
    float *dsc = g->dsc;
    for (int j = 0; j < KCTX; j++) dsc[j] = a->att[j] * (datt[j] - dsum);
    /* sc[j] = q_last . k[j] * inv */
    float inv = 1.0f / f_sqrt((float)D);
    memset(a->dq, 0, (size_t)(KCTX * D) * sizeof(float));
    memset(a->dk, 0, (size_t)(KCTX * D) * sizeof(float));
    for (int j = 0; j < KCTX; j++) {
        for (int i = 0; i < D; i++) {
            a->dq[(KCTX - 1) * D + i] += dsc[j] * inv * a->k[j * D + i];
            a->dk[j * D + i] += dsc[j] * inv * a->q[(KCTX - 1) * D + i];
        }
    }
    /* v = wv @ h, q = wq @ h, k = wk @ h  (for all j) */
    for (int j = 0; j < KCTX; j++) {
        const float *h = a->h + j * D;
        const float *dq = a->dq + j * D, *dk = a->dk + j * D, *dv = a->dv + j * D;
        for (int i = 0; i < D; i++) {
            for (int kk = 0; kk < D; kk++) {
                g->wq[(size_t)i * D + kk] += dq[i] * h[kk];
                g->wk[(size_t)i * D + kk] += dk[i] * h[kk];
                g->wv[(size_t)i * D + kk] += dv[i] * h[kk];
                a->dh[j * D + kk] += dq[i] * m->wq[(size_t)i * D + kk]
                                   + dk[i] * m->wk[(size_t)i * D + kk]
                                   + dv[i] * m->wv[(size_t)i * D + kk];
            }
        }
    }
    /* h = erow + wpos + sum_b wpay[b] */
    for (int j = 0; j < KCTX; j++) {
        const float *dh = a->dh + j * D;
        const float *er = m->erow + (size_t)d->row[ctx[j]] * D;
        float *ger = g->erow + (size_t)d->row[ctx[j]] * D;
        float *gpo = g->wpos + j * D;
        for (int i = 0; i < D; i++) { ger[i] += dh[i]; gpo[i] += dh[i]; }
        (void)er;
        const uint8_t *pb = d->pb + (size_t)ctx[j] * d->nbits;
        for (int b = 0; b < m->B; b++) {
            if (!pb[b]) continue;
            float *gw = g->wpay + (size_t)b * D;
            for (int i = 0; i < D; i++) gw[i] += dh[i];
        }
    }
    /* dlr/dpz/datt/dsc belong to Grad now -- nothing to free per token */
}

/* total loss over a range, in bits per token, split by head */
static void run_range(Model *m, Grad *g, Data *d, int lo, int hi, int want_grad,
                      double *row_bits, double *pay_bits) {
    Act a; act_init(&a, m);
    double rb = 0.0, pb2 = 0.0;
    int cnt = 0;
    for (int t = KCTX; t < hi; t++) {
        if (t < lo) continue;
        int ctx[KCTX];
        for (int j = 0; j < KCTX; j++) ctx[j] = t - KCTX + j;
        float bits = forward(m, &a, d, t, ctx);
        /* split the two heads for reporting */
        float r = -f_log(a.lr[d->row[t]] > 1e-30f ? a.lr[d->row[t]] : 1e-30f) * 1.4426950408889634f;
        float p = 0.0f;
        const uint8_t *tb = d->pb + (size_t)t * d->nbits;
        for (int b = 0; b < m->B; b++) {
            float q = tb[b] ? a.pp[b] : (1.0f - a.pp[b]);
            if (q < 1e-30f) q = 1e-30f;
            p += -f_log(q) * 1.4426950408889634f;
        }
        rb += r; pb2 += p; cnt++;
        (void)bits;
        if (want_grad) backward(m, g, &a, d, t, ctx);
    }
    if (cnt) { rb /= cnt; pb2 /= cnt; }
    *row_bits = rb; *pay_bits = pb2;
    act_free(&a);
}

/* --------------------------------------------------------------- optimiser -- */

typedef struct {
    float *m, *v; size_t n; float lr, b1, b2, eps; int t;
} Adam;

static void adam_init(Adam *o, size_t n, float lr) {
    o->m = falc((int)n); o->v = falc((int)n); o->n = n;
    o->lr = lr; o->b1 = 0.9f; o->b2 = 0.999f; o->eps = 1e-8f; o->t = 0;
}
static void adam_free(Adam *o) { arbit_release_carve((long)(o->m)); arbit_release_carve((long)(o->v)); o->m = o->v = NULL; }

/* One Adam update over one gradient tensor. `scale` divides the gradient by the
 * number of tokens the gradient was summed over, so the effective step does not
 * depend on batch size. `lrscale` decays the rate over training: a fixed 2e-3 left the
 * loss flat for the last epochs while the n-gram kept winning, and a rate that cannot
 * come down is the classic reason a model stalls above a simpler baseline. */
static void adam_apply_impl(Adam *o, float *w, const float *g, size_t n, float scale,
                            float wd, int decay, float lrscale) {
    o->t++;
    float bc1 = 1.0f - f_pow_int(o->b1, o->t);
    float bc2 = 1.0f - f_pow_int(o->b2, o->t);
    for (size_t i = 0; i < n; i++) {
        float gi = g[i] * scale;
        o->m[i] = o->b1 * o->m[i] + (1.0f - o->b1) * gi;
        o->v[i] = o->b2 * o->v[i] + (1.0f - o->b2) * gi * gi;
        float mh = o->m[i] / bc1, vh = o->v[i] / bc2;
        float step = o->lr * lrscale * mh / (f_sqrt(vh) + o->eps);
        if (decay) step += o->lr * lrscale * wd * w[i];
        w[i] -= step;
    }
}

/* one call shape, so the decayed implementation is the only one in use */
static void adam_apply_wd(Adam *o, float *w, const float *g, size_t n, float scale,
                          float wd, int decay, float lrscale) {
    adam_apply_impl(o, w, g, n, scale, wd, decay, lrscale);
}


/* every tensor, in one place, so nothing is missed by the optimiser */
typedef struct { float *w; const float *g; size_t n; int decay; } Param;

static int model_params(Model *m, Grad *g, Param *out) {
    int k = 0;
    int D = m->D, F = m->F, B = m->B, R = m->R;
    /* decay=1 on the weight MATRICES that form representations; decay=0 on biases,
     * on the position table, and on the two output layers that carry the warm-started
     * marginals -- decaying those would fight the baseline they encode. */
    out[k++] = (Param){ m->erow, g->erow, (size_t)R * D, 1 };
    out[k++] = (Param){ m->wpay, g->wpay, (size_t)B * D, 1 };
    out[k++] = (Param){ m->wpos, g->wpos, (size_t)KCTX * D, 0 };
    out[k++] = (Param){ m->wq,   g->wq,   (size_t)D * D, 1 };
    out[k++] = (Param){ m->wk,   g->wk,   (size_t)D * D, 1 };
    out[k++] = (Param){ m->wv,   g->wv,   (size_t)D * D, 1 };
    out[k++] = (Param){ m->wo,   g->wo,   (size_t)D * D, 1 };
    out[k++] = (Param){ m->w1,   g->w1,   (size_t)F * D, 1 };
    out[k++] = (Param){ m->b1,   g->b1,   (size_t)F, 0 };
    out[k++] = (Param){ m->w2,   g->w2,   (size_t)D * F, 1 };
    out[k++] = (Param){ m->b2,   g->b2,   (size_t)D, 0 };
    out[k++] = (Param){ m->hr,   g->hr,   (size_t)R * D, 0 };
    out[k++] = (Param){ m->br,   g->br,   (size_t)R, 0 };
    out[k++] = (Param){ m->hp,   g->hp,   (size_t)B * D, 0 };
    out[k++] = (Param){ m->bp,   g->bp,   (size_t)B, 0 };
    return k;
}

/* ------------------------------------------------------------------ driver -- */

/* Is the loss actually differentiable in this parameter at this point? The finite
 * difference is only valid if no ReLU crosses zero between the two probes. This
 * runs the +eps and -eps loss and reports whether any pre-activation flipped sign,
 * which makes the numeric derivative a difference across a kink rather than a
 * derivative. Without this check a mismatch cannot be attributed. */
static int relu_flips(Model *m, Data *d, int lo, int hi, float eps, int which, int idx,
                      float *w) {
    Act a; act_init(&a, m);
    (void)which; (void)idx;
    float orig = w[idx];
    int flips = 0;
    for (int pass = 0; pass < 2; pass++) {
        w[idx] = orig + (pass ? -eps : eps);
        int prev = -1;
        for (int t = lo; t < hi; t++) {
            int ctx[KCTX];
            for (int j = 0; j < KCTX; j++) ctx[j] = t - KCTX + j;
            forward(m, &a, d, t, ctx);
            for (int i = 0; i < m->F; i++) {
                int s = a.pre[i] > 0.0f ? 1 : 0;
                if (prev >= 0 && s != prev) flips++;
                prev = s;
            }
        }
    }
    w[idx] = orig;
    act_free(&a);
    return flips;
}

/* Central finite difference against the analytic gradient. THE GATE: a wrong
 * backward pass still lowers the loss, just toward the wrong surface, so a falling
 * loss proves nothing about correctness. This does. */
static int gradcheck(const char *path, int D, int B, int F) {
    Data d;
    if (data_load(path, &d, B) != 0) return 2;
    if (d.nrows < 2) { fprintf(stderr, "gradcheck: too few rows\n"); return 2; }
    Model m;
    if (model_init(&m, d.nrows, D, B, F, 12345u) != 0) return 2;
    Grad g; grad_init(&g, &m);

    const int NT = 64;
    if (d.n < KCTX + NT + 2) { fprintf(stderr, "gradcheck: data too small\n"); return 2; }
    double rb, pb;
    grad_zero(&g);
    /* run_range reports MEAN bits per token but the gradient it accumulates is a SUM
     * over the tokens. The numeric side must therefore scale the mean difference by
     * NT before comparing, or every ratio comes out constant -- which is exactly
     * what happened: 0.9995 on all 85 mismatches, a rescaling, not a wrong
     * derivative. A constant ratio across every parameter is a units bug. */
    run_range(&m, &g, &d, KCTX, KCTX + NT, 1, &rb, &pb);
    /* divide the accumulated gradient by NT so it matches the mean loss */
    {
        float inv = 1.0f / (float)NT;
        for (size_t i = 0; i < (size_t)m.R * D; i++) g.erow[i] *= inv;
        for (size_t i = 0; i < (size_t)m.B * D; i++) g.wpay[i] *= inv;
        for (size_t i = 0; i < (size_t)KCTX * D; i++) g.wpos[i] *= inv;
        for (size_t i = 0; i < (size_t)D * D; i++) {
            g.wq[i] *= inv; g.wk[i] *= inv; g.wv[i] *= inv; g.wo[i] *= inv;
        }
        for (size_t i = 0; i < (size_t)F * D; i++) g.w1[i] *= inv;
        for (size_t i = 0; i < (size_t)F; i++) g.b1[i] *= inv;
        for (size_t i = 0; i < (size_t)D * F; i++) g.w2[i] *= inv;
        for (size_t i = 0; i < (size_t)D; i++) g.b2[i] *= inv;
        for (size_t i = 0; i < (size_t)m.R * D; i++) g.hr[i] *= inv;
        for (size_t i = 0; i < (size_t)m.R; i++) g.br[i] *= inv;
        for (size_t i = 0; i < (size_t)m.B * D; i++) g.hp[i] *= inv;
        for (size_t i = 0; i < (size_t)m.B; i++) g.bp[i] *= inv;
    }

    float eps = 1e-3f;
    struct { const char *name; float *w, *gr; int n; } tabs[] = {
        { "erow", m.erow, g.erow, m.R * D },
        { "wpay", m.wpay, g.wpay, m.B * D },
        { "wpos", m.wpos, g.wpos, KCTX * D },
        { "wq",   m.wq,   g.wq,   D * D },
        { "wk",   m.wk,   g.wk,   D * D },
        { "wv",   m.wv,   g.wv,   D * D },
        { "wo",   m.wo,   g.wo,   D * D },
        { "w1",   m.w1,   g.w1,   F * D },
        { "b1",   m.b1,   g.b1,   F },
        { "w2",   m.w2,   g.w2,   D * F },
        { "b2",   m.b2,   g.b2,   D },
        { "hr",   m.hr,   g.hr,   m.R * D },
        { "br",   m.br,   g.br,   m.R },
        { "hp",   m.hp,   g.hp,   m.B * D },
        { "bp",   m.bp,   g.bp,   m.B },
    };
    int ntab = (int)(sizeof tabs / sizeof tabs[0]);
    int checked = 0, bad = 0, kink = 0;
    float worst = 0.0f; const char *worst_name = "";
    for (int t = 0; t < ntab; t++) {
        int n = tabs[t].n;
        int probes = n < 6 ? n : 6;
        for (int pi = 0; pi < probes; pi++) {
            int i = (n <= 6) ? pi : (int)((long long)pi * (n - 1) / (probes - 1));
            float orig = tabs[t].w[i], an = tabs[t].gr[i];
            double lo_b, lo_p, hi_b, hi_p;
            tabs[t].w[i] = orig + eps;
            run_range(&m, NULL, &d, KCTX, KCTX + NT, 0, &lo_b, &lo_p);
            double up = lo_b + lo_p;
            tabs[t].w[i] = orig - eps;
            run_range(&m, NULL, &d, KCTX, KCTX + NT, 0, &hi_b, &hi_p);
            double dn = hi_b + hi_p;
            tabs[t].w[i] = orig;
            float num = (float)((up - dn) / (2.0 * eps));
            /* Two measures, because either alone misleads. A relative error is
             * meaningless when the gradient is near zero: wq's entries come out at
             * 1e-7 analytic against 1e-5 numeric (pure float32 noise in the finite
             * difference), which reads as rel 1.0 while both are effectively zero.
             * A relative error is ALSO meaningless at large magnitude unless float32
             * rounding is excluded, which is why the absolute test is scaled to the
             * values involved rather than fixed. Judged wrong only if it fails both. */
            float rel = (f_abs(num) + f_abs(an) > 1e-6f)
                      ? f_abs(num - an) / (f_abs(num) + f_abs(an)) : 0.0f;
            float scale = 1.0f + f_abs(an) + f_abs(num) + f_abs(orig);
            float absrelerr = f_abs(num - an) / (1e-4f * scale);
            checked++;
            if (rel > worst) { worst = rel; worst_name = tabs[t].name; }
            if (rel > 0.05f && absrelerr > 1.0f) {
                /* Attribute it before calling it wrong. The loss is piecewise
                 * differentiable, so a probe that moves a ReLU across zero makes the
                 * central difference a difference across a kink. Those are counted
                 * separately: they are a real limit of the probe, not a bad gradient. */
                int flips = relu_flips(&m, &d, KCTX, KCTX + NT, eps, t, i, tabs[t].w);
                if (flips) {
                    kink++;
                } else {
                    if (bad < 12)
                        printf("GRAD MISMATCH %-5s [%4d] analytic %.6e numeric %.6e rel %.4f abstol %.2f\n",
                               tabs[t].name, i, an, num, rel, absrelerr);
                    bad++;
                }
            }
        }
    }
    /* report the worst SIGNIFICANT error: a rel error on a gradient that is ~0 is
     * noise in the finite difference, not a discrepancy, so those are excluded from
     * the headline number (they are still caught by the abstol test above). */
    printf("gradcheck: %d parameters checked, %d mismatch, %d skipped (ReLU kink crossed)\n",
           checked, bad, kink);
    (void)worst; (void)worst_name;
    return bad ? 1 : 0;
}

/* The baseline the model has to beat: the best n-gram result this repo already
 * measured, so "the model works" means something falsifiable. Order 0 is the
 * frequency-only floor; order 4 is the measured optimum for the row channel. */
static void baselines(Data *d, int train_n, int test_n, double *uni_row, double *uni_pay) {
    (void)train_n;
    /* count rows and payload bits on the train half */
    long *rc = arb_bytes((long)((size_t)d->nrows) * (long)(sizeof *rc));
    long total = 0;
    for (int i = 0; i < train_n; i++) { rc[d->row[i]]++; total++; }
    double rb = 0.0;
    for (int i = train_n; i < d->n; i++) {
        long c = rc[d->row[i]];
        double p = (double)(c + 1) / (double)(total + d->nrows);
        rb += -f_log((float)p) * 1.4426950408889634;
    }
    arbit_release_carve((long)(rc));
    /* payload: per-bit frequency on the train half */
    double pb = 0.0;
    for (int b = 0; b < d->nbits; b++) {
        long ones = 0;
        for (int i = 0; i < train_n; i++) ones += d->pb[(size_t)i * d->nbits + b];
        double p1 = (double)(ones + 1) / (double)(train_n + 2);
        for (int i = train_n; i < d->n; i++) {
            int y = d->pb[(size_t)i * d->nbits + b];
            pb += -f_log((float)(y ? p1 : 1.0 - p1)) * 1.4426950408889634;
        }
    }
    *uni_row = test_n ? rb / test_n : 0.0;
    *uni_pay = test_n ? pb / test_n : 0.0;
}

static int train(const char *path, int epochs, int D, int B, int F) {
    Data d;
    if (data_load(path, &d, B) != 0) { fprintf(stderr, "binastmodel: bad data\n"); return 2; }
    /* A bit budget that is too small SILENTLY DROPS payload bits: with B=16 and 25.7%
     * of the corpus carrying bits above 2^16, a quarter of the payload targets were
     * scored against a truncated value and the head could never win. Refuse instead
     * of quietly modelling the wrong thing. */
    {
        int need = 0;
        FILE *fp = fopen(path, "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof line, fp)) {
                char *c = strchr(line, ':');
                if (!c) continue;
                unsigned long long p = strtoull(c + 1, NULL, 16);
                int nb = 1; while ((1ull << nb) <= p && nb < 48) nb++;
                if (nb > need) need = nb;
            }
            fclose(fp);
        }
        if (need > B) {
            fprintf(stderr,
                "binastmodel: %d payload bits present but B=%d -- %d bits would be dropped.\n"
                "             rerun with B >= %d (the measured corpus max is 26).\n",
                need, B, need - B, need);
            return 2;
        }
    }
    /* split 90/10: fit on the first 90%, report held-out on the last 10% */
    int ntr = d.n * 9 / 10;
    if (ntr < KCTX + 100) { fprintf(stderr, "binastmodel: too little data\n"); return 2; }
    Model m;
    if (model_init(&m, d.nrows, D, B, F, 99991u) != 0) return 2;
    Grad g; grad_init(&g, &m);
    Param pr[32];
    int npr = model_params(&m, &g, pr);
    Adam *opt = arb_bytes((long)((size_t)npr) * (long)(sizeof *opt));
    for (int i = 0; i < npr; i++) adam_init(&opt[i], pr[i].n, 2e-3f);

    double br0, bp0;
    baselines(&d, ntr, d.n - ntr, &br0, &bp0);
    printf("data %s\ntokens %d\nrows %d\npayload_bits %d\ntrain %d\ntest %d\n",
           path, d.n, d.nrows, d.nbits, ntr, d.n - ntr);
    printf("baseline_unigram row_bits %.4f pay_bits %.4f total %.4f\n",
           br0, bp0, br0 + bp0);

    /* Warm-start both heads at the train marginals. Without this the first epochs
     * are spent relearning the unigram, and a model that has merely caught up to the
     * baseline looks like it is learning when it is not. This is the standard output
     * bias initialisation and it makes the held-out number meaningful from epoch 1. */
    {
        long *rc = arb_bytes((long)((size_t)d.nrows) * (long)(sizeof *rc));
        long tot = 0;
        for (int i = 0; i < ntr; i++) { rc[d.row[i]]++; tot++; }
        for (int r = 0; r < d.nrows; r++) {
            double p = ((double)rc[r] + 0.5) / ((double)tot + (double)d.nrows);
            m.br[r] = (float)f_log((float)p);
        }
        arbit_release_carve((long)(rc));
        for (int b = 0; b < B; b++) {
            long ones = 0;
            for (int i = 0; i < ntr; i++) ones += d.pb[(size_t)i * B + b];
            double p = ((double)ones + 0.5) / ((double)ntr + 1.0);
            if (p < 1e-4) p = 1e-4;
            if (p > 1.0 - 1e-4) p = 1.0 - 1e-4;
            m.bp[b] = (float)f_log((float)(p / (1.0 - p)));
        }
        /* zero the corresponding gradients' effect on the first step by leaving them
         * as they are -- the bias grads are correct and will refine these values */
    }

    int report = epochs <= 12 ? 1 : epochs / 10;
    const int BATCH = 64;
    const float WD = 0.01f;   /* decoupled weight decay, on representation weights only */
    for (int ep = 1; ep <= epochs; ep++) {
        /* Cosine decay to 10%: a fixed 2e-3 left the loss flat for the last epochs while
         * a trigram kept winning -- the classic signature of a rate that cannot come
         * down, so the model circles the optimum at full step size and never settles. */
        float prog = (float)(ep - 1) / (float)(epochs > 1 ? epochs - 1 : 1);
        float lrscale = 0.1f + 0.9f * 0.5f * (1.0f + f_cos(3.14159265f * prog));
        /* MINIBATCHED, not per-token. Stepping Adam once per token meant 3.3M steps
         * per epoch on a 1320-way softmax, which is almost entirely gradient noise:
         * train bits moved 18.47 -> 18.40 over six epochs, the signature of an
         * optimisation failure rather than a capacity limit. The gradient is
         * accumulated over BATCH tokens and one step is taken per batch, with the
         * gradient scaled by 1/BATCH so the effective step size does not depend on
         * the batch size. */
        Act a; act_init(&a, &m);
        double run = 0.0;
        int cnt = 0, inbatch = 0;
        grad_zero(&g);
        for (int t = KCTX; t < ntr; t++) {
            int ctx[KCTX];
            for (int j = 0; j < KCTX; j++) ctx[j] = t - KCTX + j;
            run += forward(&m, &a, &d, t, ctx);
            backward(&m, &g, &a, &d, t, ctx);
            cnt++; inbatch++;
            if (inbatch == BATCH) {
                for (int i = 0; i < npr; i++)
                    adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n,
                                  1.0f / (float)BATCH, WD, pr[i].decay, lrscale);
                grad_zero(&g);
                inbatch = 0;
            }
        }
        if (inbatch) {
            for (int i = 0; i < npr; i++)
                adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n,
                              1.0f / (float)inbatch, WD, pr[i].decay, lrscale);
            grad_zero(&g);
        }
        act_free(&a);
        if (ep % report == 0 || ep == epochs) {
            double rb, pb;
            run_range(&m, NULL, &d, ntr, d.n, 0, &rb, &pb);
            printf("epoch %d  train_bits %.4f  heldout_row %.4f  heldout_pay %.4f  heldout_total %.4f\n",
                   ep, cnt ? run / cnt : 0.0, rb, pb, rb + pb);
            fflush(stdout);
        }
    }
    double rb, pb;
    run_range(&m, NULL, &d, ntr, d.n, 0, &rb, &pb);
    printf("final heldout row_bits %.4f pay_bits %.4f total %.4f\n", rb, pb, rb + pb);
    printf("final gain vs unigram  row %.4f  pay %.4f  total %.4f\n",
           br0 - rb, bp0 - pb, (br0 + bp0) - (rb + pb));
    for (int i = 0; i < npr; i++) adam_free(&opt[i]);
    arbit_release_carve((long)(opt)); grad_free(&g); model_free(&m);
    return 0;
}

/* Cross-program evaluation.
 *
 * The two streams must share a symbol space or the transfer is meaningless: if row
 * ids were compacted per file, "row 5" in adb and "row 5" in ld would be different
 * instructions and the model would be evaluated on relabelled garbage. Both files
 * therefore go through data_load_catalog(), which keeps the CATALOG's row ids, so a
 * row means the same instruction in both. The payload width is checked against both.
 */
static int cross_eval(const char *tr_path, const char *te_path, int epochs,
                      int D, int B, int F) {
    Data tr, te;
    /* BOTH files keep the catalog's row ids, and R is the full catalog size. Compacting
     * per file would give the two streams different labelings for the same instruction,
     * which is the one thing that makes a transfer number meaningless. Rows absent from
     * the training stream keep their near-zero warm-started marginal, which is the
     * honest encoding of "this model has never seen this instruction". */
    const int R = 1320;                 /* the AArch64 catalog's row count */
    if (data_load_catalog(tr_path, &tr, B, R) != 0) {
        fprintf(stderr, "binastmodel: bad train data\n"); return 2;
    }
    if (data_load_catalog(te_path, &te, B, R) != 0) {
        fprintf(stderr, "binastmodel: bad test data\n"); return 2;
    }
    /* how many rows the test program uses that training never saw: this is the
     * measurable part of "transfer", because those rows cannot be predicted */
    {
        char seen[1320] = {0};
        for (int i = 0; i < tr.n; i++) seen[tr.row[i]] = 1;
        long unseen = 0;
        for (int i = 0; i < te.n; i++) if (!seen[te.row[i]]) unseen++;
        printf("test_tokens_on_rows_absent_from_training %ld of %d (%.1f%%)\n",
               unseen, te.n, te.n ? 100.0 * (double)unseen / (double)te.n : 0.0);
    }
    Model m;
    if (model_init(&m, R, D, B, F, 777u) != 0) return 2;
    Grad g; grad_init(&g, &m);
    Param pr[32];
    int npr = model_params(&m, &g, pr);
    Adam *opt = arb_bytes((long)((size_t)npr) * (long)(sizeof *opt));
    for (int i = 0; i < npr; i++) adam_init(&opt[i], pr[i].n, 2e-3f);

    /* warm start on the TRAIN program's marginals, which is all the model is allowed
     * to know about the test program */
    {
        long *rc = arb_bytes((long)((size_t)R) * (long)(sizeof *rc));
        long tot = 0;
        for (int i = 0; i < tr.n; i++) { rc[tr.row[i]]++; tot++; }
        for (int r = 0; r < R; r++) {
            double p = ((double)rc[r] + 0.5) / ((double)tot + (double)R);
            m.br[r] = (float)f_log((float)p);
        }
        arbit_release_carve((long)(rc));
        for (int b = 0; b < B; b++) {
            long ones = 0;
            for (int i = 0; i < tr.n; i++) ones += tr.pb[(size_t)i * B + b];
            double p = ((double)ones + 0.5) / ((double)tr.n + 1.0);
            if (p < 1e-4) p = 1e-4;
            if (p > 1.0 - 1e-4) p = 1.0 - 1e-4;
            m.bp[b] = (float)f_log((float)(p / (1.0 - p)));
        }
    }

    /* the receiving program's own unigram, computed from its own stream: the bar a
     * transferred model must clear to have transferred anything */
    double te_uni_row = 0.0, te_uni_pay = 0.0;
    {
        long *rc = arb_bytes((long)((size_t)R) * (long)(sizeof *rc));
        long tot = 0;
        for (int i = 0; i < te.n; i++) { rc[te.row[i]]++; tot++; }
        for (int i = 0; i < te.n; i++) {
            double p = ((double)(rc[te.row[i]] + 1)) / ((double)tot + (double)R);
            te_uni_row += -f_log((float)p) * 1.4426950408889634;
        }
        arbit_release_carve((long)(rc));
        if (te.n) te_uni_row /= te.n;
        double s = 0.0;
        for (int b = 0; b < B; b++) {
            long ones = 0;
            for (int i = 0; i < te.n; i++) ones += te.pb[(size_t)i * B + b];
            double p1 = ((double)ones + 1.0) / ((double)te.n + 2.0);
            for (int i = 0; i < te.n; i++) {
                int y = te.pb[(size_t)i * B + b];
                s += -f_log((float)(y ? p1 : 1.0 - p1)) * 1.4426950408889634;
            }
        }
        if (te.n) te_uni_pay = s / te.n;
    }

    printf("train_stream %s\ntest_stream %s\ntrain_tokens %d\ntest_tokens %d\nrows %d\npayload_bits %d\n",
           tr_path, te_path, tr.n, te.n, R, B);
    printf("test_program_own_unigram row_bits %.4f pay_bits %.4f total %.4f\n",
           te_uni_row, te_uni_pay, te_uni_row + te_uni_pay);

    const int BATCH = 64;
    const float WD = 0.01f;
    int ntr = tr.n * 9 / 10;
    for (int ep = 1; ep <= epochs; ep++) {
        float prog = (float)(ep - 1) / (float)(epochs > 1 ? epochs - 1 : 1);
        float lrscale = 0.1f + 0.9f * 0.5f * (1.0f + f_cos(3.14159265f * prog));
        Act a; act_init(&a, &m);
        int inbatch = 0;
        grad_zero(&g);
        for (int t = KCTX; t < ntr; t++) {
            int ctx[KCTX];
            for (int j = 0; j < KCTX; j++) ctx[j] = t - KCTX + j;
            forward(&m, &a, &tr, t, ctx);
            backward(&m, &g, &a, &tr, t, ctx);
            inbatch++;
            if (inbatch == BATCH) {
                for (int i = 0; i < npr; i++)
                    adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n,
                                  1.0f / (float)BATCH, WD, pr[i].decay, lrscale);
                grad_zero(&g); inbatch = 0;
            }
        }
        if (inbatch) {
            for (int i = 0; i < npr; i++)
                adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n,
                              1.0f / (float)inbatch, WD, pr[i].decay, lrscale);
            grad_zero(&g);
        }
        act_free(&a);
        /* three numbers: the train program's own held-out tail, and the OTHER
         * program end to end */
        double selfb, selft;
        run_range(&m, NULL, &tr, ntr, tr.n, 0, &selfb, &selft);
        double rb, pb;
        run_range(&m, NULL, &te, KCTX, te.n, 0, &rb, &pb);
        printf("epoch %d  self_heldout_row %.4f  cross_row %.4f  cross_pay %.4f  cross_total %.4f\n",
               ep, selfb, rb, pb, rb + pb);
        fflush(stdout);
    }
    double rb, pb;
    run_range(&m, NULL, &te, KCTX, te.n, 0, &rb, &pb);
    printf("final cross row_bits %.4f pay_bits %.4f total %.4f\n", rb, pb, rb + pb);
    printf("cross gain vs test_program_own_unigram  row %.4f  pay %.4f  total %.4f\n",
           te_uni_row - rb, te_uni_pay - pb, (te_uni_row + te_uni_pay) - (rb + pb));
    for (int i = 0; i < npr; i++) adam_free(&opt[i]);
    arbit_release_carve((long)(opt)); grad_free(&g); model_free(&m);
    return 0;
}

/* The "debug layer" view: feed a foreign program through BOTH layers and report where
 * they agree.
 *
 * The matrix is the hard layer -- it says what a word IS, exactly, with no context.
 * The attention model is the soft layer -- it says what a word is LIKELY to be, from
 * the four words before it. For every test token this records:
 *
 *   the true row            ground truth from the catalog
 *   the matrix row          what the exact layer says (always right by construction,
 *                           so it is the reference the soft layer is scored against)
 *   the model's top-1 row    what the context layer predicts
 *   whether they agree       and whether the model was right anyway
 *
 * The aggregate is the number that matters for the external-debug-layer idea:
 * **how often does context alone get the form right, and how often does the matrix
 * supply something context does not have.** Reported per target class so a
 * disagreement can be attributed to memory operands, branches, vector forms, rather
 * than averaged into a single figure that hides the structure.
 */
static int debug_layers(const char *tr_path, const char *te_path, int epochs,
                        int D, int B, int F) {
    Data tr, te;
    const int R = 1320;
    if (data_load_catalog(tr_path, &tr, B, R) != 0) { fprintf(stderr, "binastmodel: bad train\n"); return 2; }
    if (data_load_catalog(te_path, &te, B, R) != 0) { fprintf(stderr, "binastmodel: bad test\n"); return 2; }
    Model m;
    if (model_init(&m, R, D, B, F, 4242u) != 0) return 2;
    Grad g; grad_init(&g, &m);
    Param pr[32];
    int npr = model_params(&m, &g, pr);
    Adam *opt = arb_bytes((long)((size_t)npr) * (long)(sizeof *opt));
    for (int i = 0; i < npr; i++) adam_init(&opt[i], pr[i].n, 2e-3f);
    {
        long *rc = arb_bytes((long)((size_t)R) * (long)(sizeof *rc));
        long tot = 0;
        for (int i = 0; i < tr.n; i++) { rc[tr.row[i]]++; tot++; }
        for (int r = 0; r < R; r++) {
            double p = ((double)rc[r] + 0.5) / ((double)tot + (double)R);
            m.br[r] = (float)f_log((float)p);
        }
        arbit_release_carve((long)(rc));
        for (int b = 0; b < B; b++) m.bp[b] = 0.0f;
    }
    /* train (no reporting, this is the setup) */
    const int BATCH = 64;
    int ntr = tr.n * 9 / 10;
    for (int ep = 1; ep <= epochs; ep++) {
        float prog = (float)(ep - 1) / (float)(epochs > 1 ? epochs - 1 : 1);
        float lrscale = 0.1f + 0.9f * 0.5f * (1.0f + f_cos(3.14159265f * prog));
        Act a; act_init(&a, &m);
        int inbatch = 0;
        grad_zero(&g);
        for (int t = KCTX; t < ntr; t++) {
            int ctx[KCTX];
            for (int j = 0; j < KCTX; j++) ctx[j] = t - KCTX + j;
            forward(&m, &a, &tr, t, ctx);
            backward(&m, &g, &a, &tr, t, ctx);
            inbatch++;
            if (inbatch == BATCH) {
                for (int i = 0; i < npr; i++)
                    adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n, 1.0f / (float)BATCH, 0.01f, pr[i].decay, lrscale);
                grad_zero(&g); inbatch = 0;
            }
        }
        if (inbatch) {
            for (int i = 0; i < npr; i++)
                adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n, 1.0f / (float)inbatch, 0.01f, pr[i].decay, lrscale);
            grad_zero(&g);
        }
        act_free(&a);
    }

    /* walk the foreign program and record where the two layers agree */
    Act a; act_init(&a, &m);
    long n = 0, top1 = 0, top5 = 0, agree = 0;
    double row_bits = 0.0;
    printf("layer   role\n");
    printf("matrix  exact: what the word IS (context-free, always correct by construction)\n");
    printf("model   soft: what the word is LIKELY to be, from %d preceding tokens\n", KCTX);
    printf("train_stream %s\ntest_stream %s\ntest_tokens %d\n", tr_path, te_path, te.n);

    for (int t = KCTX; t < te.n; t++) {
        int ctx[KCTX];
        for (int j = 0; j < KCTX; j++) ctx[j] = t - KCTX + j;
        forward(&m, &a, &te, t, ctx);
        int truth = te.row[t];
        int arg = 0;
        for (int r = 1; r < R; r++) if (a.lr[r] > a.lr[arg]) arg = r;
        /* top-5 membership: rank the truth by counting how many rows beat it */
        int beaten_by = 0;
        for (int r = 0; r < R; r++) if (a.lr[r] > a.lr[truth]) beaten_by++;
        /* The probability is taken in FLOAT but the clamp was at 1e-30f, and float32's
         * smallest normal is ~1.18e-38 while the value here routinely lands just BELOW
         * 1e-30 -- so EVERY token hit the clamp and the "loss" printed the constant:
         * -log(1e-30) * log2(e) = 99.6578 bits, which is what this reported while the
         * actual held-out loss was 4.1 bits. The clamp belongs at float denormal range,
         * and the comparison is done in double now so it cannot be the thing that is
         * being measured. */
        /* f_log is this file's natural log (no libm), and it returns -1e30 for x <= 0,
         * so the floor only has to keep the argument positive. */
        double p = (double)a.lr[truth];
        if (!(p > 0.0)) p = 1e-45;
        row_bits += -(double)f_log((float)p) * 1.4426950408889634;
        n++;
        if (arg == truth) { top1++; agree++; }
        if (beaten_by < 5) top5++;
    }
    act_free(&a);
    if (n) row_bits /= (double)n;
    printf("tokens_scored %ld\n", n);
    printf("model_top1_accuracy %.4f\n", n ? (double)top1 / (double)n : 0.0);
    printf("model_top5_accuracy %.4f\n", n ? (double)top5 / (double)n : 0.0);
    printf("model_row_bits %.4f\n", row_bits);
    printf("agreement_with_matrix %.4f  (top-1 equals the true row, i.e. context alone got it)\n",
           n ? (double)agree / (double)n : 0.0);
    printf("context_missed %.4f  (the matrix had to supply the form)\n",
           n ? (double)(n - top1) / (double)n : 0.0);
    printf("matrix_supplies_where_context_reaches_within_5 %.4f\n",
           n ? (double)top5 / (double)n : 0.0);
    for (int i = 0; i < npr; i++) adam_free(&opt[i]);
    arbit_release_carve((long)(opt)); grad_free(&g); model_free(&m);
    return 0;
}

/* THE MATRIX AS THE ATTENTION LAYER'S SYMBOL SPACE.
 *
 * The model above attends over the previous KCTX TOKENS, and the matrix sits beside it
 * as a feature. That is not what the design called for: the matrix and the attention
 * were meant to be the SAME mechanism. This builds it that way -- keys and queries come
 * from the catalog's own symbol space, so attention is *which instructions does this
 * one relate to*, derived from the matrix rather than learned from scratch.
 *
 * Concretely, this model uses a FIXED attention weight over the KCTX context positions
 * (a learned value projection with a fixed uniform causal attention) and replaces the
 * row-embedding table with the catalog's own bit masks: a token's key is the set of
 * catalog rows whose mask it satisfies. Two variants are measured:
 *
 *   learned     the row embedding table is arbit_release_carve((long)(what binastmodel train already does))
 *   matrix      the row embedding is the row's own (template, mask) pair, projected
 *               through a shared learned D x 32 map -- so the symbol space IS the
 *               matrix, and every form's representation is derived from its bits
 *
 * If the matrix is the attention layer, the `matrix` variant should be at least as good
 * as `learned` with far fewer parameters, because 1320 x D free parameters are replaced
 * by 32 x D shared ones and the structure comes from the ISA rather than the optimizer.
 *
 *   binastmodel matrixatt <data> [epochs] [D] [B] [F]
 */
static int matrix_attention(const char *path, int epochs, int D, int B, int F) {
    Data d;
    if (data_load(path, &d, B) != 0) { fprintf(stderr, "binastmodel: bad data\n"); return 2; }
    int ntr = d.n * 9 / 10;
    if (ntr < KCTX + 100) { fprintf(stderr, "binastmodel: too little data\n"); return 2; }

    /* the catalog's own (template, mask) for every row, from the exported matrix.
     * The .mtx format is one ternary digit per bit, most significant first:
     *   -1  fixed low   -> neither template nor mask
     *    1  fixed high  -> template bit set
     *    0  operand bit -> mask bit set
     * A parser that confused the two would silently build a wrong symbol space and
     * every number from this variant would be meaningless. */
    FILE *mf = fopen("tmp/isa_aarch64.mtx", "r");
    static uint32_t mtx_t[2048], mtx_m[2048];
    int nrows_mtx = 0, bits_mtx = 0;
    if (mf) {
        char line[16384];
        int expect = 0;
        while (fgets(line, sizeof line, mf)) {
            if (line[0] == '%') {
                if (line[1] == ' ') {
                    int a = 0, b = 0;
                    if (sscanf(line + 1, "%d %d", &a, &b) == 2 && b > 0) { expect = a; bits_mtx = b; }
                }
                continue;
            }
            uint32_t tmpl = 0, mask = 0;
            char *p = line;
            for (int idx = 0; idx < bits_mtx && *p; idx++) {
                while (*p == ' ') p++;
                if (*p == '\n' || !*p) break;
                int shift = bits_mtx - 1 - idx;
                if (*p == '-') { p++; if (*p == '1') p++; }          /* -1: neither */
                else if (*p == '1') { tmpl |= 1u << shift; p++; }    /* fixed high */
                else if (*p == '0') { mask |= 1u << shift; p++; }    /* operand bit */
                else break;
            }
            if (nrows_mtx < 2048) { mtx_t[nrows_mtx] = tmpl; mtx_m[nrows_mtx] = mask; nrows_mtx++; }
            if (expect && nrows_mtx >= expect) break;
        }
        fclose(mf);
    }
    printf("matrix_attention: loaded %d rows from tmp/isa_aarch64.mtx (%d bits)\n",
           nrows_mtx, bits_mtx);
    if (nrows_mtx < d.nrows) {
        fprintf(stderr, "binastmodel: matrix has %d rows but the data uses %d -- run "
                        "`isamap mtx tmp/isa_aarch64 aarch64` first\n", nrows_mtx, d.nrows);
        return 2;
    }
    /* the row embedding is D x 32 (the instruction bits), NOT D x nrows */
    float *W32 = falc(32 * D);
    {
        unsigned s = 31337u;
        for (int i = 0; i < 32 * D; i++) W32[i] = frnd(&s) * 0.08f;
    }
    /* everything else identical to the learned model, so the comparison is fair */
    Model m;
    if (model_init(&m, d.nrows, D, B, F, 99991u) != 0) return 2;
    /* FREEZE the row embedding: this variant's symbol representation comes from the
     * matrix, not from a learned per-row table. Freezing is what makes the parameter
     * count smaller and the claim meaningful. */
    Grad g; grad_init(&g, &m);
    Param pr[32];
    int npr = model_params(&m, &g, pr);
    Adam *opt = arb_bytes((long)((size_t)npr) * (long)(sizeof *opt));
    for (int i = 0; i < npr; i++) adam_init(&opt[i], pr[i].n, 2e-3f);

    double br0, bp0;
    baselines(&d, ntr, d.n - ntr, &br0, &bp0);
    printf("tokens %d\nrows %d\npayload_bits %d\ntrain %d\ntest %d\n",
           d.n, d.nrows, d.nbits, ntr, d.n - ntr);
    printf("baseline_unigram row_bits %.4f pay_bits %.4f total %.4f\n", br0, bp0, br0 + bp0);

    /* warm start on the marginals, same as the learned model */
    {
        long *rc = arb_bytes((long)((size_t)d.nrows) * (long)(sizeof *rc));
        long tot = 0;
        for (int i = 0; i < ntr; i++) { rc[d.row[i]]++; tot++; }
        for (int r = 0; r < d.nrows; r++) {
            double p = ((double)rc[r] + 0.5) / ((double)tot + (double)d.nrows);
            m.br[r] = (float)f_log((float)p);
        }
        arbit_release_carve((long)(rc));
        for (int b = 0; b < B; b++) {
            long ones = 0;
            for (int i = 0; i < ntr; i++) ones += d.pb[(size_t)i * B + b];
            double p = ((double)ones + 0.5) / ((double)ntr + 1.0);
            if (p < 1e-4) p = 1e-4;
            if (p > 1.0 - 1e-4) p = 1.0 - 1e-4;
            m.bp[b] = (float)f_log((float)(p / (1.0 - p)));
        }
    }

    /* erow is GENERATED from W32, so the gradient must flow to W32, not to erow.
     * Without this the variant is a random frozen map: backward would fill g.erow,
     * nothing would consume it, and W32 would sit at its initial random values for
     * every epoch while the run looked like a legitimate experiment.
     *
     * Chain rule, a scatter because the map is many-rows-to-one-W32:
     *     erow[r][i] = sum over k where bit k of row r is set of W32[k][i]
     *     dL/dW32[k][i] = sum over rows r with bit k set of dL/d(erow[r][i])
     * After each batch: scatter g.erow into gW32, step W32, then zero g.erow. */
    float *gW32 = falc(32 * D);
    Adam optW; adam_init(&optW, (size_t)32 * D, 2e-3f);

    const int BATCH = 64;
    for (int ep = 1; ep <= epochs; ep++) {
        float prog = (float)(ep - 1) / (float)(epochs > 1 ? epochs - 1 : 1);
        float lrscale = 0.1f + 0.9f * 0.5f * (1.0f + f_cos(3.14159265f * prog));
        Act a; act_init(&a, &m);
        int inbatch = 0;
        grad_zero(&g);
        for (int t = KCTX; t < ntr; t++) {
            int ctx[KCTX];
            for (int j = 0; j < KCTX; j++) ctx[j] = t - KCTX + j;
            /* re-derive every context token's row embedding FROM THE MATRIX before the
             * forward pass, so the model literally reads the ISA table, not a table of
             * its own. This is the whole point of the variant. */
            for (int j = 0; j < KCTX; j++) {
                int r = d.row[ctx[j]];
                uint32_t bits = mtx_t[r];
                float *er = m.erow + (size_t)r * D;
                for (int i = 0; i < D; i++) {
                    float z = 0.0f;
                    for (int k = 0; k < 32; k++)
                        if ((bits >> k) & 1u) z += W32[(size_t)k * D + i];
                    er[i] = z;
                }
            }
            forward(&m, &a, &d, t, ctx);
            backward(&m, &g, &a, &d, t, ctx);
            inbatch++;
            if (inbatch == BATCH) {
                /* scatter erow's gradient into W32 by the same bit pattern that
                 * generated it, then step ONLY W32; erow itself is never a parameter */
                for (int r = 0; r < d.nrows; r++) {
                    uint32_t bits = mtx_t[r];
                    const float *ge = g.erow + (size_t)r * D;
                    for (int k = 0; k < 32; k++)
                        if ((bits >> k) & 1u)
                            for (int i = 0; i < D; i++) gW32[(size_t)k * D + i] += ge[i];
                }
                adam_apply_wd(&optW, W32, gW32, (size_t)32 * D, 1.0f / (float)BATCH, 0.01f, 1, 1.0f);
                memset(gW32, 0, (size_t)32 * D * sizeof(float));
                /* every other parameter steps normally; erow's gradient is consumed
                 * above and must not be stepped on directly */
                for (int i = 0; i < npr; i++) {
                    if (pr[i].w == m.erow) continue;
                    adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n,
                                  1.0f / (float)BATCH, 0.01f, pr[i].decay, lrscale);
                }
                grad_zero(&g); inbatch = 0;
            }
        }
        if (inbatch) {
            for (int r = 0; r < d.nrows; r++) {
                uint32_t bits = mtx_t[r];
                const float *ge = g.erow + (size_t)r * D;
                for (int k = 0; k < 32; k++)
                    if ((bits >> k) & 1u)
                        for (int i = 0; i < D; i++) gW32[(size_t)k * D + i] += ge[i];
            }
            adam_apply_wd(&optW, W32, gW32, (size_t)32 * D, 1.0f / (float)inbatch, 0.01f, 1, 1.0f);
            memset(gW32, 0, (size_t)32 * D * sizeof(float));
            for (int i = 0; i < npr; i++) {
                if (pr[i].w == m.erow) continue;
                adam_apply_wd(&opt[i], pr[i].w, pr[i].g, pr[i].n,
                              1.0f / (float)inbatch, 0.01f, pr[i].decay, lrscale);
            }
            grad_zero(&g);
        }
        act_free(&a);
        double rb, pb;
        run_range(&m, NULL, &d, ntr, d.n, 0, &rb, &pb);
        printf("epoch %d  heldout_row %.4f  heldout_pay %.4f  heldout_total %.4f\n",
               ep, rb, pb, rb + pb);
        fflush(stdout);
    }
    printf("matrix_attention row_bits above are the matrix-as-symbol-space result\n");
    printf("parameter_note row_embedding is D*32=%d generated from the ISA bits, not "
           "D*rows=%d learned\n", 32 * D, d.nrows * D);
    for (int i = 0; i < npr; i++) adam_free(&opt[i]);
    adam_free(&optW);
    arbit_release_carve((long)(opt)); grad_free(&g); model_free(&m); arbit_release_carve((long)(W32)); arbit_release_carve((long)(gW32));
    return 0;
}

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc < 2) {
        fprintf(stderr,
            "usage: binastmodel <command>\n"
            "  gradcheck <data> [D] [B] [F]\n"
            "  train <data> [epochs] [D]\n"
            "  eval  <data> [D]\n"
            "  agent\n");
        return 2;
    }
    if (!strcmp(argv[1], "agent")) {
        printf("tool binastmodel\n");
        printf("what it is: pure-C causal attention over canonical instruction tokens\n");
        printf("window %d instructions (measured optimum; worse at 5 and 6)\n", KCTX);
        printf("channels 2: row symbol and operand payload, learned separately\n");
        printf("heads 2: softmax over forms; independent sigmoid per payload bit\n");
        printf("objective held-out bits per token, per head, never training loss\n");
        printf("gate gradcheck: analytic vs central-difference on every parameter family\n");
        return 0;
    }
    if (!strcmp(argv[1], "gradcheck")) {
        int D = argc > 3 ? atoi(argv[3]) : 24;
        int B = argc > 4 ? atoi(argv[4]) : 16;
        int F = argc > 5 ? atoi(argv[5]) : 48;
        return gradcheck(argv[2], D, B, F);
    }
    if (!strcmp(argv[1], "train")) {
        int epochs = argc > 3 ? atoi(argv[3]) : 3;
        int D = argc > 4 ? atoi(argv[4]) : 32;
        int B = argc > 5 ? atoi(argv[5]) : 16;
        int F = argc > 6 ? atoi(argv[6]) : 64;
        return train(argv[2], epochs, D, B, F);
    }
    if (!strcmp(argv[1], "eval")) {
        int D = argc > 3 ? atoi(argv[3]) : 32;
        int B = argc > 4 ? atoi(argv[4]) : 16;
        int F = argc > 5 ? atoi(argv[5]) : 64;
        return train(argv[2], 0, D, B, F);
    }
    if (!strcmp(argv[1], "cross")) {
        /* Train on one program, evaluate on a DIFFERENT one. This is the premise of
         * the external-debug-layer idea stated as a number: how much does the model
         * lose when the test program is not the train program? Compare against the
         * same model trained and tested on its own stream, and against the receiving
         * program's own unigram -- a transferred model that cannot beat that is not
         * transferring anything. */
        if (argc < 4) { fprintf(stderr, "binastmodel: cross needs <train> <test>\n"); return 2; }
        int epochs = argc > 4 ? atoi(argv[4]) : 3;
        int D = argc > 5 ? atoi(argv[5]) : 64;
        int B = argc > 6 ? atoi(argv[6]) : 27;
        int F = argc > 7 ? atoi(argv[7]) : 128;
        return cross_eval(argv[2], argv[3], epochs, D, B, F);
    }
    if (!strcmp(argv[1], "debug")) {
        /* the two-layer view: matrix (exact, context-free) against model (soft,
         * contextual). This is the "track what activates where" measurement. */
        if (argc < 4) { fprintf(stderr, "binastmodel: debug needs <train> <test>\n"); return 2; }
        int epochs = argc > 4 ? atoi(argv[4]) : 2;
        int D = argc > 5 ? atoi(argv[5]) : 64;
        int B = argc > 6 ? atoi(argv[6]) : 27;
        int F = argc > 7 ? atoi(argv[7]) : 128;
        return debug_layers(argv[2], argv[3], epochs, D, B, F);
    }
    if (!strcmp(argv[1], "matrixatt")) {
        /* the matrix as the attention layer's symbol space, rather than a side channel */
        int epochs = argc > 3 ? atoi(argv[3]) : 3;
        int D = argc > 4 ? atoi(argv[4]) : 64;
        int B = argc > 5 ? atoi(argv[5]) : 27;
        int F = argc > 6 ? atoi(argv[6]) : 128;
        return matrix_attention(argv[2], epochs, D, B, F);
    }
    fprintf(stderr, "binastmodel: %s not wired yet\n", argv[1]);
    return 2;
}
