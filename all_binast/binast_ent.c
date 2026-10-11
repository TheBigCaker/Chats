/* binast_ent.c -- entropy of raw instruction streams, in pure C11.
 *
 * READ THIS BEFORE USING `cond` OR `cross`. Both use the naive single-stream
 * estimator, which on this data produces a spectacular and meaningless number
 * (0.207 bits/word at k=4). With enough context distinctness every context occurs
 * exactly once, its distribution is a single observation, and its conditional
 * entropy is zero by construction. That is memorisation, not a model.
 *
 * Both commands now print that warning in their own output and report
 * `tokens_per_distinct_context`, which is the ratio that decides whether the
 * estimate means anything. They are kept so the failure can be reproduced.
 *
 * For the honest measurement use **binast_hold**: count on a train half, score a
 * held-out half, absolute discounting with interpolation.
 *
 * `stats` is sound as it stands: H(W) over one stream is a direct measurement of
 * that stream's own distribution, with no conditioning to get wrong.
 *
 *   binast_ent stats    <words-file>          H(W), distinct, redundancy
 *   binast_ent cond     <words-file> [kmax]   NAIVE, self-labelled as such
 *   binast_ent cross    <a> <b> [k]           NAIVE, self-labelled as such
 *   binast_ent agent
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
#include "arbishim.h"   /* ccp_logf: log without libm */

#define LOG2E 1.4426950408889634f
static inline float log2f_(float x) { return ccp_logf(x) * LOG2E; }

/* ---------------------------------------------------------------- word stream */

typedef struct {
    uint32_t *w;      /* the words, in file order */
    size_t    n;
    uint32_t  distinct;
} Stream;

static void stream_free(Stream *s) { arbit_release_carve((long)(s->w)); s->w = NULL; s->n = 0; }

static int stream_load(const char *path, Stream *out) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "binast_ent: cannot open %s\n", path); return -1; }
    size_t cap = 1 << 16, n = 0;
    uint32_t *w = arb_bytes(cap * sizeof *w);
    if (!w) { fclose(f); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t L = strlen(p);
        while (L && (p[L-1] == '\n' || p[L-1] == '\r' || p[L-1] == ' ')) p[--L] = 0;
        if (!L) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        /* a word list is one contiguous hex run per line, word value or bytes;
         * for entropy only the identity matters, so take it as printed */
        uint32_t v = 0;
        int d = 0;
        for (; *p && d < 8; p++, d++) {
            char c = *p;
            int x = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (x < 0) break;
            v = (v << 4) | (uint32_t)x;
        }
        if (!d) continue;
        if (n == cap) { cap *= 2; uint32_t *t = arb_bytes(cap * sizeof *w); if (t) cymap_copy(t, w, (size_t)(cap * sizeof *w / 2));; if (!t) { arbit_release_carve((long)(w)); fclose(f); return -1; } w = t; }
        w[n++] = v;
    }
    fclose(f);
    out->w = w; out->n = n;
    return 0;
}

/* distinct count via an open-addressed set sized to the next power of two */
static uint32_t count_distinct(const uint32_t *w, size_t n) {
    size_t cap = 1;
    while (cap < n * 2 + 16) cap <<= 1;
    uint32_t *tbl = arb_bytes((long)(cap) * (long)(sizeof *tbl));
    uint8_t  *has = arb_bytes((long)(cap) * (long)(1));
    if (!tbl || !has) { arbit_release_carve((long)(tbl)); arbit_release_carve((long)(has)); return 0; }
    uint32_t d = 0;
    for (size_t i = 0; i < n; i++) {
        size_t h = (w[i] * 2654435761u) & (cap - 1);
        while (has[h] && tbl[h] != w[i]) h = (h + 1) & (cap - 1);
        if (!has[h]) { has[h] = 1; tbl[h] = w[i]; d++; }
    }
    arbit_release_carve((long)(tbl)); arbit_release_carve((long)(has));
    return d;
}

/* -------------------------------------------------------------- entropy math */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* H of the word distribution, in bits */
static float entropy_of(const uint32_t *w, size_t n) {
    if (!n) return 0.0f;
    uint32_t *c = arb_bytes(n * sizeof *c);
    if (!c) return 0.0f;
    memcpy(c, w, n * sizeof *c);
    qsort(c, n, sizeof *c, cmp_u32);
    float h = 0.0f;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && c[j] == c[i]) j++;
        float p = (float)(j - i) / (float)n;
        h -= p * log2f_(p);
        i = j;
    }
    arbit_release_carve((long)(c));
    return h;
}

/* Conditional entropy H(W | context of k previous words), by counting.
 *
 * The context key is a hash of the k preceding words. A flat table would need
 * distinct^k entries, so the count table is a bounded open-addressed map with
 * each bucket holding the key, the word, and a count; collisions beyond the
 * table's capacity are counted as misses rather than silently merged. Capacity
 * is reported so a saturated table is visible instead of looking like high
 * entropy.
 */
typedef struct { uint32_t key, word, cnt; } CtxEnt;
typedef struct {
    CtxEnt  *e;
    size_t   cap;
    size_t   used;
    size_t   overflow;   /* entries dropped because the table was full */
} CtxTable;

static void ct_init(CtxTable *t, size_t cap) {
    t->cap = cap; t->used = 0; t->overflow = 0;
    t->e = arb_bytes((long)(cap) * (long)(sizeof *t->e));
}

static uint32_t hash_ctx(const uint32_t *w, int k) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < k; i++) { h ^= w[i]; h *= 16777619u; }
    h ^= (uint32_t)k;
    h *= 16777619u;
    return h ? h : 1u;
}

static void ct_add(CtxTable *t, uint32_t key, uint32_t word) {
    if (!t->e) return;
    size_t h = (key * 2654435761u + word * 40503u) & (t->cap - 1);
    for (;;) {
        if (t->e[h].cnt == 0) {
            if (t->used >= t->cap * 3 / 4) { t->overflow++; return; }
            t->e[h].key = key; t->e[h].word = word; t->e[h].cnt = 1;
            t->used++;
            return;
        }
        if (t->e[h].key == key && t->e[h].word == word) { t->e[h].cnt++; return; }
        h = (h + 1) & (t->cap - 1);
    }
}

/* sort (key, word) pairs so contexts group together */
static int cmp_ctxt(const void *a, const void *b) {
    const CtxEnt *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->word != y->word) return x->word < y->word ? -1 : 1;
    return 0;
}

/* conditional entropy: sum over contexts of H(words | that context) */
static float ct_entropy(CtxTable *t) {
    if (!t->e) return 0.0f;
    size_t n = 0;
    for (size_t i = 0; i < t->cap; i++) if (t->e[i].cnt) n++;
    if (!n) return 0.0f;
    CtxEnt *v = arb_bytes(n * sizeof *v);
    if (!v) return 0.0f;
    size_t k = 0;
    for (size_t i = 0; i < t->cap; i++) if (t->e[i].cnt) v[k++] = t->e[i];
    /* qsort, not insertion sort: with a real corpus this array is ~10^5 entries
     * and an O(n^2) sort here simply never returns. */
    qsort(v, n, sizeof *v, cmp_ctxt);
    double H = 0.0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        uint64_t tot = 0;
        while (j < n && v[j].key == v[i].key) { tot += v[j].cnt; j++; }
        for (size_t m = i; m < j; m++) {
            double p = (double)v[m].cnt / (double)tot;
            H += ((double)tot) * -p * log2f_((float)p);
        }
        i = j;
    }
    arbit_release_carve((long)(v));
    /* normalise by the number of tokens the table actually holds */
    uint64_t cnt_total = 0;
    for (size_t m = 0; m < t->cap; m++) if (t->e[m].cnt) cnt_total += t->e[m].cnt;
    return cnt_total ? (float)(H / (double)cnt_total) : 0.0f;
}

/* ------------------------------------------------------------------- commands */

static int cmd_stats(const char *path) {
    Stream s = {0};
    if (stream_load(path, &s) != 0) return 2;
    s.distinct = count_distinct(s.w, s.n);
    float H = entropy_of(s.w, s.n);
    /* the byte-level floor for comparison: entropy over 4 bytes treated as one
     * symbol is the same measurement, so report bits/byte too */
    printf("stream %s\n", path);
    printf("words %zu\n", s.n);
    printf("distinct_words %u\n", s.distinct);
    printf("H_word_bits %.4f\n", H);
    printf("H_bits_per_byte %.4f\n", H / 4.0f);
    printf("uniform_over_distinct_bits %.4f\n", s.distinct ? log2f_((float)s.distinct) : 0.0f);
    printf("redundancy_per_mille %.1f\n",
           s.distinct ? (1.0f - H / log2f_((float)s.distinct)) * 1000.0f : 0.0f);
    stream_free(&s);
    return 0;
}

/* The naive single-stream conditional entropy. THIS NUMBER IS NOT A RESULT.
 *
 * It computes H(W | k previous words) by counting over ONE stream, so with enough
 * context distinctness every context occurs exactly once, its "distribution" is a
 * single observation, and its entropy is zero by construction. On adb it reports
 * 0.207 bits/word at k=4, which would be a spectacular result and is pure
 * memorisation of the training stream.
 *
 * It is kept because the failure is instructive and worth being able to reproduce
 * -- but it says so, loudly, in its own output, and points at the estimator that
 * is honest (binast_hold, held-out scoring with absolute discounting). A tool that
 * can print a misleading number must print the warning with it. */
static int cmd_cond(const char *path, int kmax) {
    Stream s = {0};
    if (stream_load(path, &s) != 0) return 2;
    float H0 = entropy_of(s.w, s.n);
    printf("stream %s\nwords %zu\n", path, s.n);
    printf("!! NAIVE ESTIMATOR -- NOT A RESULT. Counted over one stream, so a\n");
    printf("!! context seen once has a one-sample distribution and scores 0 bits.\n");
    printf("!! Use `binast_hold` (held-out, absolute discounting) for anything real.\n");
    printf("H0_bits %.4f\n", H0);
    /* a bounded table: 1<<24 entries = 16M*12B = 192MB, enough for these corpora
     * at short context and honest about overflow via the reported count */
    size_t cap = 1u << 24;
    for (int k = 1; k <= kmax; k++) {
        CtxTable t; ct_init(&t, cap);
        for (size_t i = (size_t)k; i < s.n; i++)
            ct_add(&t, hash_ctx(s.w + i - k, k), s.w[i]);
        float Hk = ct_entropy(&t);
        /* tokens per distinct context: at or below ~1.0 the estimate is meaningless */
        double per_ctx = t.used ? (double)s.n / (double)t.used : 0.0;
        printf("k %d\nHk_bits %.4f\ncontext_gain_bits %.4f\ntable_entries %zu\ntable_overflow %zu\n"
               "tokens_per_distinct_context %.3f\nestimate_trustworthy %s\n",
               k, Hk, H0 - Hk, t.used, t.overflow, per_ctx,
               per_ctx >= 5.0 ? "yes" : "NO -- one observation per context");
        arbit_release_carve((long)(t.e));
    }
    stream_free(&s);
    return 0;
}

static int cmd_cross(const char *pa, const char *pb, int k) {
    Stream a = {0}, b = {0};
    if (stream_load(pa, &a) != 0) return 2;
    if (stream_load(pb, &b) != 0) { stream_free(&a); return 2; }
    float H0 = entropy_of(a.w, a.n);
    size_t cap = 1u << 24;
    /* model built on stream B, measured on stream A */
    CtxTable tb; ct_init(&tb, cap);
    for (size_t i = (size_t)k; i < b.n; i++) ct_add(&tb, hash_ctx(b.w + i - k, k), b.w[i]);
    /* and the same for A modelled on itself, for the contrast */
    CtxTable ta; ct_init(&ta, cap);
    for (size_t i = (size_t)k; i < a.n; i++) ct_add(&ta, hash_ctx(a.w + i - k, k), a.w[i]);
    float Hself = ct_entropy(&ta);
    float Hcross = ct_entropy(&tb);
    printf("stream_a %s\nstream_b %s\nk %d\n", pa, pb, k);
    printf("H0_bits %.4f\n", H0);
    printf("H_self_bits %.4f\nself_gain_bits %.4f\n", Hself, H0 - Hself);
    printf("H_cross_bits %.4f\ncross_gain_bits %.4f\n", Hcross, H0 - Hcross);
    printf("cross_fraction_of_self %.4f\n", (H0 - Hself) != 0.0f ? (H0 - Hcross) / (H0 - Hself) : 0.0f);
    printf("!! same NAIVE estimator as `cond` -- not a result; see binast_hold cross\n");
    printf("a_entries %zu\na_overflow %zu\nb_entries %zu\nb_overflow %zu\n",
           ta.used, ta.overflow, tb.used, tb.overflow);
    arbit_release_carve((long)(ta.e)); arbit_release_carve((long)(tb.e));
    stream_free(&a); stream_free(&b);
    return 0;
}

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc < 2) {
        fprintf(stderr,
            "usage: binast_ent <command>\n"
            "  stats  <words-file>\n"
            "  cond   <words-file> [kmax]\n"
            "  cross  <file-a> <file-b> [k]\n"
            "  agent\n");
        return 2;
    }
    if (!strcmp(argv[1], "stats")) return cmd_stats(argv[2]);
    if (!strcmp(argv[1], "cond"))  return cmd_cond(argv[2], argc > 3 ? atoi(argv[3]) : 4);
    if (!strcmp(argv[1], "cross")) return cmd_cross(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 2);
    if (!strcmp(argv[1], "agent")) {
        printf("tool binast_ent\n");
        printf("what it is: entropy of raw instruction streams, in pure C11, no libm\n");
        printf("sound_command stats  -- H(W) over one stream, a direct measurement\n");
        printf("naive_commands cond cross  -- single-stream conditional entropy; these\n");
        printf("  reported 0.207 bits/word on adb, which is memorisation, not prediction.\n");
        printf("  They self-label now, but use `binast_hold` for anything that matters.\n");
        printf("honest_tool binast_hold -- held-out scoring, absolute discounting\n");
        printf("log2 source: ccp_logf (port snumblits), no libm anywhere\n");
        return 0;
    }
    fprintf(stderr, "binast_ent: unknown command %s\n", argv[1]);
    return 2;
}
