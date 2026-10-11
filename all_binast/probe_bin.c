/* probe_bin -- pure C11 binary-stream learnability probe, CCP-stack flavored.
 *
 * No libm, no zlib, no numpy, no torch, no Python.
 *   math   : cygemm/snumblits.h  (ccp_logf -> log2, zero libc.math)
 *   threads: cygemm/partitricks.h (raw clone()/futex -- no pthread) [optional]
 *
 *   probe_bin <train.elf>... -- <test.elf>...
 *
 * Measures bits per 4-byte instruction word on binaries the model never saw.
 * Raw (no model) = 32.0 bits/word.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cygemm/snumblits.h"

#define LOG2E 1.4426950408889634f
static inline float log2f_(float x) { return ccp_logf(x) * LOG2E; }

/* ---------------- ELF .text extraction (no libelf) ---------------- */
static uint64_t rd64_(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint32_t rd32_(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16_(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t *text_words(const char *path, size_t *nwords) {
    *nwords = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz < 64) { fclose(f); return NULL; }
    uint8_t *d = malloc((size_t)sz);
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (memcmp(d, "\x7f" "ELF", 4) || d[4] != 2 || d[5] != 1) { free(d); return NULL; }
    uint64_t shoff = rd64_(d + 0x28);
    uint16_t shents = rd16_(d + 0x3a), shnum = rd16_(d + 0x3c), shstr = rd16_(d + 0x3e);
    if (shents != 64 || !shoff || shoff + (uint64_t)shnum * 64 > (uint64_t)sz) { free(d); return NULL; }
    uint64_t stroff = rd64_(d + shoff + (uint64_t)shstr * 64 + 0x18);
    uint64_t strsz  = rd64_(d + shoff + (uint64_t)shstr * 64 + 0x20);
    if (stroff + strsz > (uint64_t)sz) { free(d); return NULL; }
    const char *str = (const char *)(d + stroff);
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + (uint64_t)i * 64;
        uint32_t no = rd32_(sh);
        if (no >= strsz || strcmp(str + no, ".text")) continue;
        uint64_t off = rd64_(sh + 0x18), size = rd64_(sh + 0x20);
        if (off + size > (uint64_t)sz) break;
        size -= size % 4;
        size_t n = size / 4;
        uint32_t *w = malloc(n ? n * 4 : 4);
        for (size_t k = 0; k < n; k++) w[k] = rd32_(d + off + k * 4);
        free(d);
        *nwords = n;
        return w;
    }
    free(d);
    return NULL;
}

/* ---------------- byte-context model, orders {2,4} exact-hashed ------------- */
#define O2BITS 16            /* exact order-2 context: 65536 entries */
#define O2CTX  (1u << O2BITS)
#define O4BITS 22            /* hashed order-4 context: 4M buckets */
#define O4CTX  (1u << O4BITS)

static uint8_t  *o2c;                 /* [ctx*256] */
static uint32_t *o2t;                 /* [ctx] */
static uint32_t *o1c;                 /* [prev*256] */
static uint32_t *o1t;
static uint32_t  o0c[256], o0t;
static uint8_t  *o4c;                 /* [bucket*256] */
static uint32_t *o4t;

static uint32_t hash4(const uint8_t *b) {
    uint32_t h = 2166136261u;
    h = (h ^ b[0]) * 16777619u;
    h = (h ^ b[1]) * 16777619u;
    h = (h ^ b[2]) * 16777619u;
    h = (h ^ b[3]) * 16777619u;
    return (h ^ (h >> 15)) & (O4CTX - 1);
}

static void model_init(void) {
    o2c = calloc((size_t)O2CTX * 256, 1);
    o2t = calloc(O2CTX, sizeof(uint32_t));
    o1c = calloc(256 * 256, sizeof(uint32_t));
    o1t = calloc(256, sizeof(uint32_t));
    o4c = calloc((size_t)O4CTX * 256, 1);
    o4t = calloc(O4CTX, sizeof(uint32_t));
    if (!o2c || !o2t || !o1c || !o1t || !o4c || !o4t) { fprintf(stderr, "probe_bin: oom\n"); exit(1); }
}

static void model_train(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t x = p[i];
        if (i >= 4) {
            uint32_t b = hash4(p + i - 4);
            uint8_t *cell = &o4c[(size_t)b * 256 + x];
            if (*cell < 255) (*cell)++;
            o4t[b]++;
        }
        if (i >= 2) {
            uint32_t ctx = ((uint32_t)p[i - 2] << 8) | p[i - 1];
            uint8_t *cell = &o2c[(size_t)ctx * 256 + x];
            if (*cell < 255) (*cell)++;
            o2t[ctx]++;
        }
        if (i >= 1) { o1c[(size_t)p[i - 1] * 256 + x]++; o1t[p[i - 1]]++; }
        o0c[x]++; o0t++;
    }
}

/* order-4 hashed, backing off through order-2, order-1, order-0 */
static float prob_byte(const uint8_t *hist, size_t hlen, uint8_t x) {
    if (hlen >= 4) {
        uint32_t b = hash4(hist + hlen - 4);
        if (o4t[b] >= 4)
            return ((float)o4c[(size_t)b * 256 + x] + 0.2f) / ((float)o4t[b] + 0.2f * 256.0f);
    }
    if (hlen >= 2) {
        uint32_t ctx = ((uint32_t)hist[hlen - 2] << 8) | hist[hlen - 1];
        if (o2t[ctx] >= 4)
            return ((float)o2c[(size_t)ctx * 256 + x] + 0.2f) / ((float)o2t[ctx] + 0.2f * 256.0f);
    }
    if (hlen >= 1 && o1t[hist[hlen - 1]] >= 4)
        return ((float)o1c[(size_t)hist[hlen - 1] * 256 + x] + 0.2f) / ((float)o1t[hist[hlen - 1]] + 0.2f * 256.0f);
    return ((float)o0c[x] + 0.2f) / ((float)o0t + 0.2f * 256.0f);
}

/* bits per word, streaming (context crosses word boundaries, as in a real LM) */
static double bits_per_word(const uint32_t *words, size_t nw, int with_o4) {
    double total = 0;
    uint8_t hist[8]; size_t hl = 0;
    for (size_t i = 0; i < nw; i++) {
        uint8_t w[4];
        for (int k = 0; k < 4; k++) w[k] = (uint8_t)((words[i] >> (8 * k)) & 0xff);
        for (int k = 0; k < 4; k++) {
            float p = with_o4 ? prob_byte(hist, hl, w[k]) : prob_byte(hist, hl < 4 ? hl : 4, w[k]);
            total -= log2f_(p);
            if (hl < sizeof hist) hist[hl++] = w[k];
            else { memmove(hist, hist + 1, sizeof hist - 1); hist[sizeof hist - 1] = w[k]; }
        }
    }
    return total / (double)nw;
}

/* ---------------- word vocabulary: exact, via sort + binary search ---------- */
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static int has_u32(const uint32_t *sorted, size_t n, uint32_t v) {
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2;
        if (sorted[mid] == v) return 1;
        if (sorted[mid] < v) lo = mid + 1; else hi = mid; }
    return 0;
}

int main(int argc, char **argv) {
    int sep = -1;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--")) { sep = i; break; }
    if (sep < 1 || sep == argc - 1) { fprintf(stderr, "usage: probe_bin <train.elf>... -- <test.elf>...\n"); return 2; }

    size_t cap = 1 << 21, used = 0;
    uint8_t *train = malloc(cap);
    int ntrain = 0;
    for (int i = 1; i < sep; i++) {
        size_t nw; uint32_t *w = text_words(argv[i], &nw);
        if (!w) continue;
        ntrain++;
        size_t bytes = nw * 4;
        while (used + bytes > cap) { cap *= 2; train = realloc(train, cap); }
        memcpy(train + used, w, bytes);
        used += bytes;
        free(w);
    }
    size_t ntw = used / 4;
    printf("train: %d binaries, %.2f MB .text, %.2f M instruction words\n",
           ntrain, used / 1048576.0, ntw / 1e6);

    size_t tcap = 1 << 20, tused = 0;
    uint32_t *test = malloc(tcap * 4);
    int ntest = 0;
    for (int i = sep + 1; i < argc; i++) {
        size_t nw; uint32_t *w = text_words(argv[i], &nw);
        if (!w) continue;
        ntest++;
        if (nw > 500000) nw = 500000;      /* cap per file so the sweep stays quick */
        while (tused + nw > tcap) { tcap *= 2; test = realloc(test, tcap * 4); }
        memcpy(test + tused, w, nw * 4);
        tused += nw;
        free(w);
    }
    printf("test:  %d unseen binaries, %.2f M instruction words\n\n", ntest, tused / 1e6);

    /* exact vocabulary overlap */
    uint32_t *sorted = malloc(ntw * 4);
    memcpy(sorted, train, ntw * 4);
    qsort(sorted, ntw, 4, cmp_u32);
    size_t distinct = ntw ? 1 : 0;
    for (size_t i = 1; i < ntw; i++) if (sorted[i] != sorted[i - 1]) distinct++;
    size_t present = 0;
    for (size_t i = 0; i < tused; i++) if (has_u32(sorted, ntw, test[i])) present++;
    printf("distinct instruction words in train: %zu\n", distinct);
    printf("unseen-program test words whose exact word appears in train: %.2f%%\n\n",
           100.0 * (double)present / (double)tused);
    free(sorted);

    model_init();
    model_train(train, used);

    printf("%-42s %14s\n", "metric", "bits / 4-byte word");
    printf("%s\n", "---------------------------------------------------------------");
    printf("%-42s %14.3f\n", "raw, no model", 32.0);
    printf("%-42s %14.3f\n", "order-2 exact byte ctx, held out", bits_per_word(test, tused, 0));
    printf("%-42s %14.3f\n", "order-4 hashed + backoff, held out", bits_per_word(test, tused, 1));
    {
        size_t self_words = ntw > 500000 ? 500000 : ntw;
        printf("%-42s %14.3f\n", "order-4, SELF (training data floor)",
               bits_per_word((const uint32_t *)train, self_words, 1));
    }
    return 0;
}
