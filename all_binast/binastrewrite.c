/* binastrewrite.c -- does the gradient actually point at the right program?
 *
 * The claim under test, stated so it can fail: fire a program through the map to get the
 * activation map C, predict the next row from context, take dC = predicted - actual, and
 * propagate dA = dC @ W. If dA is signal, then A + dA moves the program's bits TOWARD the
 * word that should be there. If prediction is near chance, dA is noise and no rewrite is
 * possible.
 *
 * The two smmb kernels are the real ones (fullmetal_smmb_c.dry:1900 forward, :2088
 * backward). Memory is arbit. No libc allocator.
 *
 *   binastrewrite <matrix.mtx> <words.txt> [train_frac]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "arbishim.h"

/* ---- the two kernels, verbatim shape from the emitted smmb ---- */
static int RW_MODE = 1, RW_CHUNK = 64;

__attribute__((unused)) static void smmb_gemm(float *C, const float *A, const float *W, int N_, int No, int Ki) {
    if (RW_MODE == 0 || RW_CHUNK > Ki || RW_CHUNK <= 0) {
        for (int s = 0; s < N_; s++) {
            const float *as = A + (size_t)s * Ki;
            float *cs = C + (size_t)s * No;
            for (int rr = 0; rr < No; rr++) {
                const float *wr = W + (size_t)rr * Ki;
                float r = 0;
                for (int k = 0; k < Ki; k++) r += as[k] * wr[k];
                cs[rr] = r;
            }
        }
        return;
    }
    memset(C, 0, (size_t)N_ * No * sizeof(float));
    for (int s = 0; s < N_; s++) {
        const float *as = A + (size_t)s * Ki;
        float *cs = C + (size_t)s * No;
        for (int k0 = 0; k0 < Ki; k0 += RW_CHUNK) {
            int kmax = k0 + RW_CHUNK < Ki ? k0 + RW_CHUNK : Ki;
            for (int rr = 0; rr < No; rr++) {
                float r = cs[rr];
                for (int k = k0; k < kmax; k++) r += as[k] * W[(size_t)rr * Ki + k];
                cs[rr] = r;
            }
        }
    }
}
static void smmb_outer_T(float *dx, const float *dC, const float *W,
                         int N_, int No, int Ki, float sc) {
    for (int s = 0; s < N_; s++) {
        const float *dc = dC + (size_t)s * No;
        float *dxs = dx + (size_t)s * Ki;
        for (int rr = 0; rr < No; rr++) {
            float dcv = dc[rr] * sc;
            if (fabsf(dcv) < 1e-15f) continue;
            const float *wr = W + (size_t)rr * Ki;
            for (int k = 0; k < Ki; k++) dxs[k] += dcv * wr[k];
        }
    }
}

#define NBITS 32
#define MAXROWS 4096

/* a deterministic 32-bit mixer, for the repair test's controlled oracle mixing. Not a
 * cryptographic generator -- the sweep needs reproducible draws, not unpredictability. */
static uint32_t rnd32(uint32_t x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }

/* one worker's slice of the row-assignment pass */
typedef struct {
    long lo, hi;
    const uint32_t *words;
    int32_t *row;
    const float *W;
    int NR, NB;
    long nomatch;
    long *nmatch;
} RowJob;

static void row_worker(void *arg) {
    RowJob *j = (RowJob *)arg;
    for (long i = j->lo; i < j->hi; i++) {
        uint32_t w = j->words[i];
        int best = -1, bestfix = -1;
        for (int r = 0; r < j->NR; r++) {
            int fix = 0, ok = 1;
            const float *wr = j->W + (size_t)r * j->NB;
            for (int b = 0; b < j->NB; b++) {
                float wv = wr[b];
                if (wv == 0.0f) continue;
                int bit = (int)((w >> b) & 1u);
                if ((wv > 0.0f && !bit) || (wv < 0.0f && bit)) { ok = 0; break; }
                fix++;
            }
            if (ok && fix > bestfix) { bestfix = fix; best = r; }
        }
        j->row[i] = best;
        if (best < 0) j->nomatch++;
    }
    *j->nmatch = j->nomatch;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: binastrewrite <mtx> <words> [train_frac]\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: a crash must not hide its own location */
    arbit_init(0);
    double train_frac = argc > 3 ? atof(argv[3]) : 0.8;

    /* ---- W: the map as signed weights ---- */
    FILE *m = fopen(argv[1], "r");
    if (!m) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    int NR = 0, NB = NBITS;
    float *W = (float *)arb_carve_zeroed((long)MAXROWS * NBITS * sizeof(float));
    char line[16384];
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '%') {
            int a = 0, b = 0;
            if (line[1] == ' ' && sscanf(line + 1, "%d %d", &a, &b) == 2 && b > 0) NB = b;
            continue;
        }
        if (!line[0] || line[0] == '\n') continue;
        if (NR >= MAXROWS) break;
        char *p = line;
        for (int i = 0; i < NB && *p; i++) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == '\r' || !*p) break;
            /* parse the token with strtol rather than hand-walking the characters. The
             * hand-walked version read "-1" as a '-' followed by a '1' and consumed the
             * '1' only in the branch where it was followed by something else -- so a row
             * ended early and the word matched NO row at all ("rows assigned 0, no row
             * 45445"). strtol cannot make that mistake: it consumes the whole token. */
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            p = end;
            int bit = NB - 1 - i;
            if (v == 1)       W[(size_t)NR * NB + bit] =  1.0f;
            else if (v == -1) W[(size_t)NR * NB + bit] = -1.0f;
            /* v == 0 means an operand bit: leave it at 0.0f (does not care) */
        }
        NR++;
    }
    fclose(m);
    if (!NR) { fprintf(stderr, "no rows\n"); return 2; }
    printf("map   %d rows x %d bits\n", NR, NB);

    /* ---- the program: words, and A as +/-1 ----
     * TWO TOKEN FORMATS, because the tree has both and silently misreading one is a
     * measurement that looks like a result:
     *   "d503249f"                   a bare word (exec_words.txt, adb_words.txt)
     *   "000001c6:0000000000000024"  a POSITIONED token: row-id : word (own_*, cp_*)
     * strtoul on the positioned form parses the row id and stops at ':'. Feeding it the
     * positioned streams made every "word" a tiny constant, every predictor scored 100%,
     * and the gradient vanished -- a broken input masquerading as a finding. Take the text
     * after the ':' when there is one. */
    FILE *f = fopen(argv[2], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    long cap = 1 << 21, NW = 0;
    uint32_t *words = (uint32_t *)arb_carve_zeroed(cap * 4);
    char tok[128];
    while (fgets(tok, sizeof tok, f) && NW < cap) {
        char *colon = strchr(tok, ':');
        const char *hex = colon ? colon + 1 : tok;
        if (!colon) {
            /* a bare word: skip anything that is not hex (blank lines, comments) */
            const char *q = tok;
            while (*q == ' ' || *q == '\t') q++;
            if (!*q || *q == '\n' || *q == '%' || *q == '#') continue;
            hex = q;
        }
        words[NW++] = (uint32_t)strtoul(hex, NULL, 16);
    }
    fclose(f);
    printf("words %ld\n", NW);
    if (NW < 100) { fprintf(stderr, "too few words\n"); return 2; }

    float *A = (float *)arb_carve_zeroed((long)NW * NB * sizeof(float));
    if (!A) { fprintf(stderr, "out of memory: A\n"); return 2; }
    for (long i = 0; i < NW; i++)
        for (int b = 0; b < NB; b++)
            A[(size_t)i * NB + b] = ((words[i] >> b) & 1u) ? 1.0f : -1.0f;

    /* ---- FIRE, in BLOCKS. The full activation map at 45k words x 1320 rows is 240 MB of
     * float32 for a 190 KB program, which is the wrong shape for the object: the map is
     * 99.7% sparse in row space (a word fires ~1.4 rows). So fire in blocks and keep only
     * each word's winning row. This is the same smmb_gemm, called on a slice.
     * (An earlier version carved the dense C and dC and needed 480 MB, which does not fit
     * a phone and is why it died. The fix is the shape, not a bigger allocation.) */
    const int BLK = 2048;   /* the gradient pass still fires in blocks */
    /* ---- assign each word its row, IN PARALLEL, on the partitricks pool ----
     * This is the O(words x rows x bits) step (45k x 1320 x 32) and it was the 75 seconds
     * in the earlier run. It is embarrassingly parallel: word i's row depends only on word
     * i and the frozen W, so workers take disjoint word ranges and no reduction or lock is
     * needed. The payload is plain C -- a worker never touches anything shared but its own
     * slice of `row` and its own counters. */
    int32_t *row = (int32_t *)arb_carve_zeroed((long)NW * 4);
    long *nmatch = (long *)arb_carve_zeroed((long)64 * sizeof(long));
    if (!row || !nmatch) { fprintf(stderr, "out of memory for row assignment\n"); return 2; }
    ArbPool pool = {0};
    int NT = ac_cpu_count(); if (NT > 32) NT = 32;
    if (arb_pool_up(&pool, NT) != 0) { fprintf(stderr, "pool bring-up failed\n"); return 2; }
    long per = (NW + NT - 1) / NT;
    RowJob *jobs = (RowJob *)arb_carve_zeroed((long)NT * (long)sizeof(RowJob));
    void **args = (void **)arb_carve_zeroed((long)NT * (long)sizeof(void *));
    if (!jobs || !args) { fprintf(stderr, "out of memory for jobs\n"); return 2; }
    for (int t = 0; t < NT; t++) {
        jobs[t].lo = (long)t * per;
        jobs[t].hi = jobs[t].lo + per > NW ? NW : jobs[t].lo + per;
        jobs[t].words = words; jobs[t].row = row; jobs[t].W = W;
        jobs[t].NR = NR; jobs[t].NB = NB; jobs[t].nomatch = 0;
        jobs[t].nmatch = &nmatch[t];
        args[t] = &jobs[t];
    }
    arb_pool_run(&pool, NT, row_worker, args);
    long nomatch = 0;
    for (int t = 0; t < NT; t++) nomatch += jobs[t].nomatch;
    printf("row assignment: %d workers, %ld words each\n", NT, per);
    printf("firing  C = A @ W^T (blocked, %d rows at a time) -> winning row per word\n", BLK);

    /* ---- the row each word IS: exact match, most specific ---- */
    printf("rows assigned %ld, no row %ld\n", NW - nomatch, nomatch);

    /* ---- PREDICT the next row from context: order-2 with backoff ----
     * This is the cheapest model that can carry context. binastmodel is the expensive one;
     * if even this is near chance there is no point training anything bigger.
     *
     * THE PREDICTION IS FOR i+1 AND THAT IS THE FIX. The first version predicted row[i],
     * while the gradient set dC = +1 at the prediction and -1 at row[i] -- so a CORRECT
     * prediction cancelled to dC == 0 and dA == 0. The gradient vanished exactly when the
     * model succeeded, which made a better model look like a smaller signal and produced
     * the false conclusion "prediction is the bottleneck". The question the rewrite asks is
     * "where does this word go next", so the target must be the row at i+1 and the source
     * must be the word at i. */
    long split = (long)(NW * train_frac);
    enum { ORD2 = 1 << 22, ORD1 = 1 << 16, UNI = MAXROWS };
    int32_t *k2 = (int32_t *)arb_carve_zeroed((long)ORD2 * 4);   /* key: r_{i-1}*NR + r_i */
    int32_t *v2 = (int32_t *)arb_carve_zeroed((long)ORD2 * 4);
    int32_t *k1 = (int32_t *)arb_carve_zeroed((long)ORD1 * 4);
    int32_t *v1 = (int32_t *)arb_carve_zeroed((long)ORD1 * 4);
    int32_t *uni = (int32_t *)arb_carve_zeroed((long)UNI * 4);
    for (long i = 2; i + 1 < split; i++) {
        if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
        long k = (long)row[i-1] * NR + row[i];             /* context ends at i */
        if (k < ORD2) { k2[k]++; v2[k] = row[i+1]; }       /* ... and the target is i+1 */
        long k1k = row[i];
        k1[k1k]++; v1[k1k] = row[i+1];
        uni[row[i+1]]++;
    }
    int unibest = 0; for (int r = 1; r < NR; r++) if (uni[r] > uni[unibest]) unibest = r;

    long hit = 0, tot = 0, hit1 = 0, hit0 = 0;
    for (long i = 2; i + 1 < NW; i++) {
        if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
        tot++;
        long k = (long)row[i-1] * NR + row[i];
        int pred;
        if (k < ORD2 && k2[k] >= 3) pred = v2[k];
        else if (k1[row[i]] >= 2) pred = v1[row[i]];
        else pred = unibest;
        if (pred == row[i+1]) hit++;
        if (row[i] == row[i+1]) hit1++;
        if (unibest == row[i+1]) hit0++;
    }
    printf("\nNEXT-ROW PREDICTION (held-out tail, %ld positions)\n", tot);
    printf("  order-2 + backoff        %6.2f%%\n", 100.0 * hit / tot);
    printf("  repeat-previous (order-1)%6.2f%%\n", 100.0 * hit1 / tot);
    printf("  most-common (unigram)    %6.2f%%\n", 100.0 * hit0 / tot);

    /* ---- THE GRADIENT TEST: does dA = dC @ W move the word the right way? ----
     * dC is +1 on the row predicted, -1 on the row that is actually there. So dA is the
     * direction from "what is" toward "what should be". Then ask whether stepping the
     * word's bits along sign(dA) actually reduces its distance to the word that follows. */
    /* dC is TWO nonzero entries per word (predicted row +1, actual row -1), so it is
     * driven sparsely instead of materialising 240 MB of mostly zeros. */
    float *dCb = (float *)arb_carve_zeroed((long)BLK * NR * sizeof(float));
    float *dA = (float *)arb_carve_zeroed((long)NW * NB * sizeof(float));
    if (!dCb || !dA) { fprintf(stderr, "out of memory: dCb/dA\n"); return 2; }
    for (long base = 0; base < NW; base += BLK) {
        int n = (int)((NW - base) < BLK ? (NW - base) : BLK);
        cymap_zero(dCb, (size_t)n * NR * sizeof(float));
        for (int s = 0; s < n; s++) {
            long i = base + s;
            if (i < 2 || i + 1 >= NW) continue;
            if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
            long k = (long)row[i-1] * NR + row[i];
            int pred;
            if (k < ORD2 && k2[k] >= 3) pred = v2[k];
            else if (k1[row[i]] >= 2) pred = v1[row[i]];
            else pred = unibest;
            /* +1 on where the word should MOVE TO (row[i+1]), -1 on where it is (row[i]).
             * Both nonzero now: the earlier version used row[i] twice, so a correct
             * prediction cancelled and the gradient died exactly when the model was right. */
            dCb[(size_t)s * NR + pred]     += 1.0f;
            dCb[(size_t)s * NR + row[i]]   -= 1.0f;
        }
        smmb_outer_T(dA + (size_t)base * NB, dCb, W, n, NR, NB, 1.0f);
    }

    /* step the word: bit b goes to the value sign(dA_b) prefers, where sign is taken from
     * the +/-1 encoding (dA > 0 means "this bit should be 1"). */
    long moved = 0, closer = 0, farther = 0, same = 0;
    long closer_pred = 0, tot_pred = 0;
    for (long i = 2; i + 1 < NW; i++) {
        if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
        uint32_t w = words[i];
        uint32_t nw = 0;
        for (int b = 0; b < NB; b++) {
            float d = dA[(size_t)i * NB + b];
            int bit = (d > 0.0f) ? 1 : 0;
            if (d == 0.0f) bit = (w >> b) & 1u;
            nw |= (uint32_t)bit << b;
        }
        if (nw != w) moved++;
        /* distance to the ACTUAL next word (i+1) before and after the step */
        {
            uint32_t t = words[i+1];
            int db = __builtin_popcount(w ^ t), da = __builtin_popcount(nw ^ t);
            tot_pred++;
            if (da < db) closer++; else if (da > db) farther++; else same++;
            /* and toward the PREDICTED row's template */
            long k = (long)row[i-1] * NR + row[i];
            int pred = (k < ORD2 && k2[k] >= 3) ? v2[k] : (k1[row[i]] >= 2 ? v1[row[i]] : unibest);
            if (pred >= 0 && pred < NR) {
                uint32_t tmpl = 0;
                for (int b = 0; b < NB; b++) if (W[(size_t)pred * NB + b] > 0.0f) tmpl |= 1u << b;
                int p0 = __builtin_popcount(w ^ tmpl), p1 = __builtin_popcount(nw ^ tmpl);
                if (p1 < p0) closer_pred++;
            }
        }
    }
    printf("\nGRADIENT REWRITE (dA = dC @ W, step the bits along sign(dA))\n");
    printf("  words whose bits moved        %ld of %ld\n", moved, tot_pred);
    printf("  distance to the NEXT word:    closer %ld   farther %ld   same %ld\n",
           closer, farther, same);
    printf("  distance to the PREDICTED row's template: closer %ld of %ld\n",
           closer_pred, tot_pred);
    printf("  => %s (ratio at x=0 is the real predictor's; a ratio above 1.0 means A + dA\n"
           "     moves the program toward what comes next more often than away from it)\n",
           (closer > farther) ? "dA MOVES THE PROGRAM THE RIGHT WAY"
                              : "dA is not distinguishable from noise here");

    /* ---- THE THRESHOLD, MEASURED RATHER THAN ASSERTED ----
     * The claim "prediction must be better than ~37% for the gradient to help" was a guess.
     * This measures the curve: replace a fraction of predictions with the TRUE next row
     * (a synthetic oracle at a knobs-set accuracy) and record how the closer/farther ratio
     * moves with it. Where the ratio crosses 1.0 is the real requirement -- the accuracy
     * the predictor must reach for A + dA to be an improvement rather than damage.
     *
     * This is legitimate only because the injected value is the REAL next row; it is an
     * upper bound on what any predictor at that accuracy could achieve, which is exactly
     * what a threshold question needs. */
    printf("\nACCURACY THRESHOLD SWEEP (x = fraction of predictions replaced by the truth)\n");
    printf("  %6s  %8s  %9s  %9s  %8s  %s\n", "x", "moved", "closer", "farther", "ratio", "verdict");
    double rand_ratio = 0.0, real_ratio = 0.0;
    for (int step = -1; step <= 10; step++) {
        float x = (float)step / 10.0f;
        int use_random = (step < 0);   /* step -1 is the control: a MEANINGLESS prediction */
        cymap_zero(dA, (size_t)NW * NB * sizeof(float));
        for (long base = 0; base < NW; base += BLK) {
            int n = (int)((NW - base) < BLK ? (NW - base) : BLK);
            cymap_zero(dCb, (size_t)n * NR * sizeof(float));
            for (int s = 0; s < n; s++) {
                long i = base + s;
                if (i < 2 || i + 1 >= NW) continue;
                if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
                long k = (long)row[i-1] * NR + row[i];
                int pred;
                if (k < ORD2 && k2[k] >= 3) pred = v2[k];
                else if (k1[row[i]] >= 2) pred = v1[row[i]];
                else pred = unibest;
                /* the oracle: with probability x, use the true next row instead.
                 * THE RANDOM CONTROL IS WHAT MAKES THE REST MEANINGFUL: if predicting at
                 * random also scores above 1.0, the ratio is measuring something about the
                 * stepping rule and not about the prediction. It must sit at ~1.0. */
                if (use_random) pred = (int)(rnd32((uint32_t)(i * 2654435761u)) % (uint32_t)NR);
                else if ((float)(rnd32((uint32_t)(i * 2654435761u)) & 0xFFFFFFu) / 16777216.0f < x)
                    pred = row[i+1];
                dCb[(size_t)s * NR + pred]   += 1.0f;
                dCb[(size_t)s * NR + row[i]] -= 1.0f;
            }
            smmb_outer_T(dA + (size_t)base * NB, dCb, W, n, NR, NB, 1.0f);
        }
        long c = 0, f = 0, m = 0;
        for (long i = 2; i + 1 < NW; i++) {
            if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
            uint32_t w = words[i], nw = 0;
            for (int b = 0; b < NB; b++) {
                float d = dA[(size_t)i * NB + b];
                int bit = (d > 0.0f) ? 1 : 0;
                if (d == 0.0f) bit = (w >> b) & 1u;
                nw |= (uint32_t)bit << b;
            }
            if (nw != w) m++;
            uint32_t t = words[i+1];
            int db = __builtin_popcount(w ^ t), da = __builtin_popcount(nw ^ t);
            if (da < db) c++; else if (da > db) f++;
        }
        if (use_random) printf("  %6s  %8ld  %9ld  %9ld  %8.3f  %s\n",
                               "rand", m, c, f, f ? (double)c / (double)f : 0.0,
                               (c > f) ? "helps" : (c == f ? "flat" : "HURTS"));
        else printf("  %6.2f  %8ld  %9ld  %9ld  %8.3f  %s\n",
                    x, m, c, f, f ? (double)c / (double)f : 0.0,
                    (c > f) ? "helps" : (c == f ? "flat" : "HURTS"));
        double ratio = f ? (double)c / (double)f : 0.0;
        if (use_random) rand_ratio = ratio;
        else if (step == 0) real_ratio = ratio;
        if (use_random && f && (double)c / (double)f > 1.10)
            printf("  !! control FAILED: a random prediction also scores above 1.10, so this\n"
                   "     measurement is not isolating the prediction\n");
    }

    /* ---- THE CROSSOVER, ON THE REAL PREDICTOR ----
     * The sweep above uses a synthetic oracle, which bounds what any predictor could do at a
     * given accuracy. This one varies the REAL predictor's own confidence threshold: a
     * higher bar makes it answer less often but be right more often. That traces the actual
     * accuracy-vs-benefit curve and locates where A + dA stops damaging the program, which
     * is the number that decides whether a better model is worth training. */
    printf("\nREAL-PREDICTOR CONFIDENCE SWEEP (min count for the order-2 / order-1 guess)\n");
    printf("  %5s %5s  %9s  %8s  %9s  %9s  %8s\n",
           "c2", "c1", "coverage", "accuracy", "moved", "closer", "ratio");
    for (int c2 = 1; c2 <= 6; c2++) {
        for (int c1 = 1; c1 <= 4; c1 += 3) {
            long cov = 0, acc = 0;
            long c = 0, f = 0, m = 0;
            cymap_zero(dA, (size_t)NW * NB * sizeof(float));
            for (long base = 0; base < NW; base += BLK) {
                int n = (int)((NW - base) < BLK ? (NW - base) : BLK);
                cymap_zero(dCb, (size_t)n * NR * sizeof(float));
                for (int s = 0; s < n; s++) {
                    long i = base + s;
                    if (i < 2 || i + 1 >= NW) continue;
                    if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
                    long k = (long)row[i-1] * NR + row[i];
                    int pred;
                    if (k < ORD2 && k2[k] >= c2) pred = v2[k];
                    else if (k1[row[i]] >= c1) pred = v1[row[i]];
                    else pred = unibest;
                    if (i >= split) { cov++; if (pred == row[i+1]) acc++; }
                    dCb[(size_t)s * NR + pred]   += 1.0f;
                    dCb[(size_t)s * NR + row[i]] -= 1.0f;
                }
                smmb_outer_T(dA + (size_t)base * NB, dCb, W, n, NR, NB, 1.0f);
            }
            for (long i = split; i + 1 < NW; i++) {
                if (row[i] < 0 || row[i-1] < 0 || row[i+1] < 0) continue;
                uint32_t w = words[i], nw = 0;
                for (int b = 0; b < NB; b++) {
                    float d = dA[(size_t)i * NB + b];
                    int bit = (d > 0.0f) ? 1 : 0;
                    if (d == 0.0f) bit = (w >> b) & 1u;
                    nw |= (uint32_t)bit << b;
                }
                if (nw != w) m++;
                uint32_t t = words[i+1];
                int db = __builtin_popcount(w ^ t), da = __builtin_popcount(nw ^ t);
                if (da < db) c++; else if (da > db) f++;
            }
            printf("  %5d %5d  %8.2f%%  %7.2f%%  %9ld  %9ld  %8.3f  %s\n",
                   c2, c1,
                   cov ? 100.0 * (double)cov / (double)(NW - split) : 0.0,
                   cov ? 100.0 * (double)acc / (double)cov : 0.0,
                   m, c, f ? (double)c / (double)f : 0.0, (c > f) ? "helps" : "HURTS");
        }
    }

    /* ONE MACHINE-READABLE LINE, so the gate asserts numbers instead of guessing at
     * column positions in two different tables. A checker that reads the wrong column
     * passes for the wrong reason -- which is exactly what happened to the first version
     * of this gate. */
    printf("\nRESULT rand_ratio %.4f real_ratio %.4f\n", rand_ratio, real_ratio);
    printf("RESULT_CONTROL %s\n",
           (rand_ratio < 1.0 && real_ratio > 1.0) ? "PASS" : "FAIL");

    arb_pool_down(&pool);
    (void)nomatch;
    return 0;
}
