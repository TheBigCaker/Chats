/* binast_hold.c -- held-out cross-entropy of instruction streams: the honest measurement.
 *
 * Two artifacts had to be removed before this could report anything real, and both
 * are worth recording because each one looked like a result.
 *
 * ARTIFACT 1 -- single-stream conditional entropy. Computing H(W | k previous words)
 * over ONE stream gave 0.207 bits/word at k=4 on adb. The count table held 400,800
 * distinct (context, word) pairs against 458,642 tokens: essentially one observation
 * per context, so every context's "distribution" was a single sample and its entropy
 * was zero by construction. That is memorisation, not prediction.
 *
 * ARTIFACT 2 -- add-one smoothing over a large vocabulary. Scoring a train/test
 * split with the textbook (c+1)/(T+V) formula made EVERY order worse than the
 * unigram (gains of -0.32 to -0.62 bits), because V is ~124,000: a context seen
 * once with a perfectly consistent follower scored 15.9 bits while the unigram
 * scored 8.4. The smoothing constant swamped the signal, and the sign of the
 * result was decided by arithmetic rather than by the data.
 *
 * The estimator here is ABSOLUTE DISCOUNTING with interpolation, the standard
 * treatment for sparse high-cardinality contexts:
 *
 *   p_k(w | h) = max(count(h,w) - D, 0) / T(h)  +  (D * N1+(h) / T(h)) * p_{k-1}(w | h')
 *
 *   T(h)    total count seen after context h
 *   N1+(h)  number of DISTINCT words seen after h -- the escape mass
 *   D       discount, 0.75, the usual value
 *   p_{k-1} the interpolated lower order, ending at the unigram
 *
 * This cannot do worse than the lower order by construction (the escape term is
 * proportional to it), so a negative gain now means something real.
 *
 * Scoring is on the held-out half, so table size cannot buy a better number:
 *
 *   H_test = -(1/N_test) * sum log2 P(word_i | context_i, train only)
 *
 *   binast_hold <words-file> [order] [train-percent]
 *   binast_hold cross <train-file> <test-file> [order]
 *   binast_hold agent
 *
 * Pure C11, no libm (log2 via ccp_logf from the port's snumblits).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cygemm/snumblits.h"

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"

#define LOG2E 1.4426950408889634f
static inline float log2f_(float x) { return ccp_logf(x) * LOG2E; }

#define DISCOUNT 0.75

/* ------------------------------------------------------------------- streams */

/* Tokens are 64-bit. A 32-bit word fits, and so does a packed operand payload
 * (measured mean width 20.99 bits, but the packing is not bounded to 32) or an
 * x86-64 word. The parser used to cap at 8 hex digits, which silently truncated
 * every 16-digit payload to its leading zeros and made a whole measurement read
 * 0.0000 bits -- a token type that cannot hold the token is a bug, not a limit. */
typedef struct { uint64_t *w; size_t n; } Stream;

static void stream_done(Stream *s) { arbit_release_carve((long)(s->w)); s->w = NULL; s->n = 0; }

static int sload(const char *path, Stream *o) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "binast_hold: cannot open %s\n", path); return -1; }
    size_t cap = 1 << 16, n = 0;
    uint64_t *w = arb_bytes(cap * sizeof *w);
    if (!w) { fclose(f); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t L = strlen(p);
        while (L && (p[L-1] == '\n' || p[L-1] == '\r' || p[L-1] == ' ')) p[--L] = 0;
        if (!L) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint64_t v = 0; int d = 0;
        for (; *p && d < 16; p++, d++) {
            char c = *p;
            int x = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (x < 0) break;
            v = (v << 4) | (uint64_t)x;
        }
        if (!d) continue;
        if (n == cap) { cap *= 2; uint64_t *t = arb_bytes(cap * sizeof *w); if (t) cymap_copy(t, w, (size_t)(cap * sizeof *w / 2));; if (!t) { arbit_release_carve((long)(w)); fclose(f); return -1; } w = t; }
        w[n++] = v;
    }
    fclose(f);
    o->w = w; o->n = n;
    return 0;
}

/* ------------------------------------------------------------------- counting */

typedef struct { uint64_t word; uint32_t key, cnt; } Pair;
typedef struct { uint32_t key, tot, n1; }    Ctx;

typedef struct { Pair *e; size_t cap, used, overflow; } PairTab;
typedef struct { Ctx  *e; size_t cap, used; }           CtxTab;

static void pt_init(PairTab *t, size_t cap) {
    t->cap = cap; t->used = 0; t->overflow = 0;
    t->e = arb_bytes((long)(cap) * (long)(sizeof *t->e));
}
static void ct_init(CtxTab *t, size_t cap) {
    t->cap = cap; t->used = 0;
    t->e = arb_bytes((long)(cap) * (long)(sizeof *t->e));
}

static uint32_t hash_ctx(const uint64_t *w, int k) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < k; i++) {
        uint64_t x = w[i];
        h ^= (uint32_t)x;         h *= 16777619u;
        h ^= (uint32_t)(x >> 32); h *= 16777619u;
    }
    h ^= (uint32_t)(k * 2654435761u);
    return h ? h : 1u;
}

/* returns 1 if this (key,word) pair is newly inserted, 0 if it existed */
static int pt_add(PairTab *t, uint32_t key, uint64_t word) {
    if (!t->e) return 0;
    size_t h = ((size_t)key * 2654435761u + (size_t)word * 2246822519u) & (t->cap - 1);
    for (;;) {
        if (t->e[h].cnt == 0) {
            if (t->used >= t->cap * 3 / 4) { t->overflow++; return 0; }
            t->e[h].key = key; t->e[h].word = word; t->e[h].cnt = 1;
            t->used++;
            return 1;
        }
        if (t->e[h].key == key && t->e[h].word == word) { t->e[h].cnt++; return 0; }
        h = (h + 1) & (t->cap - 1);
    }
}

static uint32_t pt_get(const PairTab *t, uint32_t key, uint64_t word) {
    if (!t->e) return 0;
    size_t h = ((size_t)key * 2654435761u + (size_t)word * 2246822519u) & (t->cap - 1);
    for (;;) {
        if (t->e[h].cnt == 0) return 0;
        if (t->e[h].key == key && t->e[h].word == word) return t->e[h].cnt;
        h = (h + 1) & (t->cap - 1);
    }
}

static void ct_bump(CtxTab *t, uint32_t key, int new_follower) {
    if (!t->e) return;
    size_t h = (key * 2654435761u) & (t->cap - 1);
    for (;;) {
        if (t->e[h].tot == 0 && t->e[h].n1 == 0) {
            if (t->used >= t->cap * 3 / 4) return;
            t->e[h].key = key; t->e[h].tot = 1; t->e[h].n1 = new_follower ? 1 : 0;
            t->used++;
            return;
        }
        if (t->e[h].key == key) {
            t->e[h].tot++;
            if (new_follower) t->e[h].n1++;
            return;
        }
        h = (h + 1) & (t->cap - 1);
    }
}

static int ct_find(const CtxTab *t, uint32_t key, uint32_t *tot, uint32_t *n1) {
    if (!t->e) return 0;
    size_t h = (key * 2654435761u) & (t->cap - 1);
    for (;;) {
        if (t->e[h].tot == 0 && t->e[h].n1 == 0) return 0;
        if (t->e[h].key == key) { *tot = t->e[h].tot; *n1 = t->e[h].n1; return 1; }
        h = (h + 1) & (t->cap - 1);
    }
}

/* ------------------------------------------------------------------- the model */

#define MAXORDER 6

typedef struct {
    int      order;
    PairTab  pair[MAXORDER + 1];   /* pair[k] = (context hash, word) -> count */
    CtxTab   ctx[MAXORDER + 1];    /* ctx[k]  = context hash -> (total, distinct followers) */
    PairTab  uni;                  /* unigram counts, key unused */
    CtxTab   uni_tot;              /* single entry: (total tokens, distinct words) */
    uint64_t vocab;
    uint64_t tokens;
} Model;

static void model_free(Model *m) {
    for (int k = 1; k <= m->order; k++) { arbit_release_carve((long)(m->pair[k].e)); arbit_release_carve((long)(m->ctx[k].e)); }
    arbit_release_carve((long)(m->uni.e));
    arbit_release_carve((long)(m->uni_tot.e));
}

static void model_build(Model *m, const uint64_t *w, size_t n, int order, size_t cap) {
    memset(m, 0, sizeof *m);
    m->order = order;
    pt_init(&m->uni, cap);
    ct_init(&m->uni_tot, 64);
    for (size_t i = 0; i < n; i++) {
        int isnew = pt_add(&m->uni, 0, w[i]);
        ct_bump(&m->uni_tot, 0, isnew);
    }
    m->vocab = m->uni.used;
    m->tokens = n;
    for (int k = 1; k <= order; k++) {
        pt_init(&m->pair[k], cap);
        ct_init(&m->ctx[k], cap);
        for (size_t i = (size_t)k; i < n; i++) {
            uint32_t key = hash_ctx(w + i - k, k);
            int isnew = pt_add(&m->pair[k], key, w[i]);
            ct_bump(&m->ctx[k], key, isnew);
        }
    }
}

/* Unigram probability, discounted the same way so all orders are comparable. */
static double model_p0(const Model *m, uint64_t word) {
    double V = (double)m->vocab;
    double c = (double)pt_get(&m->uni, 0, word);
    double N = (double)m->tokens;
    double discount = c > DISCOUNT ? DISCOUNT : c;
    return (c - discount) / N + (DISCOUNT * V / N) * (1.0 / V);
}

/* Interpolated probability of `word` after `hist[0..k-1]`, absolute discounting.
 * Recurses down the orders; the escape term is proportional to the lower order, so
 * the estimate cannot be worse than a shorter context by construction. */
static double model_p(const Model *m, const uint64_t *hist, int k, uint64_t word) {
    if (k < 1) return model_p0(m, word);
    int kk = k > m->order ? m->order : k;
    uint32_t key = hash_ctx(hist + (k - kk), kk);
    uint32_t tot = 0, n1 = 0;
    double lower = model_p(m, hist, kk - 1, word);
    if (!ct_find(&m->ctx[kk], key, &tot, &n1) || !tot)
        return lower;                       /* context never seen: pure backoff */
    double c = (double)pt_get(&m->pair[kk], key, word);
    double discount = c > DISCOUNT ? DISCOUNT : c;
    double p_ml = (c - discount) / (double)tot;
    double escape = (DISCOUNT * (double)n1) / (double)tot;
    return p_ml + escape * lower;
}

/* Score the test half: mean cross-entropy in bits per word. */
static double model_score(const Model *m, const uint64_t *w, size_t n, int k, double *sum_bits) {
    int kmax = k > m->order ? m->order : k;
    if ((size_t)kmax >= n) { *sum_bits = 0.0; return 0.0; }
    double bits = 0.0;
    size_t count = 0;
    for (size_t i = (size_t)kmax; i < n; i++) {
        double p = model_p(m, w + i - kmax, kmax, w[i]);
        bits += -log2f_((float)p);
        count++;
    }
    *sum_bits = bits;
    return count ? bits / (double)count : 0.0;
}

/* -------------------------------------------------------------------- commands */

static int cmd_hold(const char *path, int order, int train_pct) {
    Stream s = {0};
    if (sload(path, &s) != 0) return 2;
    size_t split = s.n * (size_t)train_pct / 100;
    if (split < 64 || s.n - split < 64) { fprintf(stderr, "binast_hold: stream too small\n"); stream_done(&s); return 2; }
    const uint64_t *train = s.w, *test = s.w + split;
    size_t ntr = split, nte = s.n - split;

    printf("stream %s\n", path);
    printf("words %zu\ntrain_words %zu\ntest_words %zu\ntrain_percent %d\n",
           s.n, ntr, nte, train_pct);

    size_t cap = 1u << 22;
    Model m0; model_build(&m0, train, ntr, order, cap);
    double b0 = 0.0;
    double H0 = model_score(&m0, test, nte, 0, &b0);
    printf("order 0 (unigram)  cross_entropy_bits %.4f\n", H0);
    model_free(&m0);

    for (int k = 1; k <= order; k++) {
        Model m; model_build(&m, train, ntr, k, cap);
        double sb = 0.0;
        double H = model_score(&m, test, nte, k, &sb);
        size_t ov = 0;
        for (int j = 1; j <= k; j++) ov += m.pair[j].overflow;
        printf("order %d  cross_entropy_bits %.4f  gain_bits %.4f  pairs %zu  overflow %zu\n",
               k, H, H0 - H, m.pair[k].used, ov);
        model_free(&m);
    }
    stream_done(&s);
    return 0;
}

static int cmd_cross(const char *ptr_file, const char *pte_file, int order) {
    Stream tr = {0}, te = {0};
    if (sload(ptr_file, &tr) != 0) return 2;
    if (sload(pte_file, &te) != 0) { stream_done(&tr); return 2; }
    printf("train_stream %s\ntest_stream %s\n", ptr_file, pte_file);
    printf("train_words %zu\ntest_words %zu\n", tr.n, te.n);
    size_t cap = 1u << 22;
    Model m0; model_build(&m0, tr.w, tr.n, order, cap);
    double b0 = 0.0;
    double H0 = model_score(&m0, te.w, te.n, 0, &b0);
    printf("order 0  cross_entropy_bits %.4f\n", H0);
    model_free(&m0);
    for (int k = 1; k <= order; k++) {
        Model m; model_build(&m, tr.w, tr.n, k, cap);
        double sb = 0.0;
        double H = model_score(&m, te.w, te.n, k, &sb);
        printf("order %d  cross_entropy_bits %.4f  gain_bits %.4f\n", k, H, H0 - H);
        model_free(&m);
    }
    stream_done(&tr); stream_done(&te);
    return 0;
}

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc < 2) {
        fprintf(stderr,
            "usage: binast_hold <words-file> [order] [train-percent]\n"
            "       binast_hold cross <train-file> <test-file> [order]\n"
            "       binast_hold agent\n");
        return 2;
    }
    if (!strcmp(argv[1], "agent")) {
        printf("tool binast_hold\n");
        printf("what it is: held-out cross-entropy of instruction word streams\n");
        printf("estimator: absolute discounting with interpolation, discount 0.75\n");
        printf("why: single-stream conditional entropy is memorisation, not prediction;\n");
        printf("     and add-one smoothing over a 124k vocabulary invents negative gains\n");
        printf("output: cross_entropy_bits per word at orders 0..N, and the gain vs unigram\n");
        return 0;
    }
    if (!strcmp(argv[1], "cross")) {
        if (argc < 4) { fprintf(stderr, "binast_hold: cross needs two files\n"); return 2; }
        return cmd_cross(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 4);
    }
    return cmd_hold(argv[1], argc > 2 ? atoi(argv[2]) : 4, argc > 3 ? atoi(argv[3]) : 50);
}
