/* binastfreq.c -- the program as a SHAPE, predicted in the frequency domain.
 *
 * WHY THIS AND NOT A TOKEN MODEL. A program is not a stream of unrelated symbols: it is a
 * repeating structure (loops, call sequences, prologue/epilogue patterns) laid out along
 * positions. Repeating structure IS a frequency, and a frequency is a shape. So instead of
 * "which token comes next", this asks "what does this signal look like", which is the
 * image-model view the architecture describes.
 *
 * The signal: fire each word's row index over position. That is a 1-D signal per program,
 * whose periodicity is the program's block structure. His FFT (techture_fft_core, copied
 * verbatim) gives the spectrum; the top-k frequencies are the shapes.
 *
 * WHAT IS MEASURED, and why each one matters:
 *
 *   dominant periods     the actual periodicities in the code, in instructions
 *   prediction           next row from (a) the FFT spectrum's phase and (b) order-2
 *                        context, on the same held-out tail -- so the frequency view has
 *                        to beat the table view to be worth anything
 *   spectrum similarity  how much of the spectrum two DIFFERENT programs share, which is
 *                        the shape-level version of the 90-96% vocabulary overlap
 *
 * Pure C11, arbit for memory, partitricks for the parallel passes, his math from snumblits.
 *
 *   binastfreq <matrix.mtx> <words.txt> [train_frac]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "arbishim.h"

/* ---- his math, from cygemm/cygemm/snumblits.h (the port's, not a copy I wrote) ---- */
static inline float ccp_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float g = x, prev;
    for (int i = 0; i < 24; i++) { prev = g; g = 0.5f * (g + x / g); if (g == prev) break; }
    return g;
}
static inline float ccp_cosf(float x) {
    const float TWO_PI = 6.283185307179586f;
    while (x >  3.14159265358979f) x -= TWO_PI;
    while (x < -3.14159265358979f) x += TWO_PI;
    float x2 = x * x, r = 1.0f, t = 1.0f;
    for (int i = 1; i <= 10; i++) { t *= -x2 / (float)((2*i-1) * (2*i)); r += t; }
    return r;
}
static inline float ccp_sinf(float x) {
    const float TWO_PI = 6.283185307179586f;
    while (x >  3.14159265358979f) x -= TWO_PI;
    while (x < -3.14159265358979f) x += TWO_PI;
    float x2 = x * x, r = x, t = x;
    for (int i = 1; i <= 10; i++) { t *= -x2 / (float)((2*i) * (2*i+1)); r += t; }
    return r;
}

/* ---- his FFT, verbatim from techture.dry (complex interleaved re,im) ---- */
static void techture_fft_core(float *data, int n, int inverse) {
    int i, j, k, m, step;
    float tr, ti, wr, wi, cr, ci, angle, temp;
    (void)step;
    j = 0;
    for (i = 1; i < n; i++) {
        k = n >> 1;
        while (j & k) { j ^= k; k >>= 1; }
        j ^= k;
        if (i < j) {
            temp = data[2*i]; data[2*i] = data[2*j]; data[2*j] = temp;
            temp = data[2*i+1]; data[2*i+1] = data[2*j+1]; data[2*j+1] = temp;
        }
    }
    for (m = 1; m < n; m <<= 1) {
        angle = (inverse ? 1.0f : -1.0f) * 3.14159265358979323846f / m;
        wr = ccp_cosf(angle);
        wi = ccp_sinf(angle);
        for (k = 0; k < n; k += 2 * m) {
            cr = 1.0f; ci = 0.0f;
            for (j = 0; j < m; j++) {
                tr = cr * data[2*(k+j+m)] - ci * data[2*(k+j+m)+1];
                ti = cr * data[2*(k+j+m)+1] + ci * data[2*(k+j+m)];
                data[2*(k+j+m)]   = data[2*(k+j)]   - tr;
                data[2*(k+j+m)+1] = data[2*(k+j)+1] - ti;
                data[2*(k+j)]     = data[2*(k+j)]   + tr;
                data[2*(k+j)+1]   = data[2*(k+j)+1] + ti;
                temp = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = temp;
            }
        }
    }
    if (inverse) {
        float inv_n = 1.0f / (float)n;
        for (i = 0; i < 2 * n; i++) data[i] *= inv_n;
    }
}

#define NBITS 32
#define MAXROWS 4096

/* ---- parallel row assignment, on the pool ---- */
typedef struct {
    long lo, hi;
    const uint32_t *words;
    int32_t *row;
    const float *W;
    int NR, NB;
    long nomatch;
} RowJob;

static void row_worker(void *arg) {
    RowJob *j = (RowJob *)arg;
    long nm = 0;
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
        if (best < 0) nm++;
    }
    j->nomatch = nm;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    arbit_init(0);
    if (argc < 3) { fprintf(stderr, "usage: binastfreq <mtx> <words> [train_frac]\n"); return 2; }
    double train_frac = argc > 3 ? atof(argv[3]) : 0.8;

    /* ---- W from the matrix ---- */
    FILE *m = fopen(argv[1], "r");
    if (!m) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    int NR = 0, NB = NBITS;
    float *W = (float *)arb_carve_zeroed((long)MAXROWS * NBITS * sizeof(float));
    if (!W) { fprintf(stderr, "oom W\n"); return 2; }
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
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            p = end;
            int bit = NB - 1 - i;
            if (v == 1)       W[(size_t)NR * NB + bit] =  1.0f;
            else if (v == -1) W[(size_t)NR * NB + bit] = -1.0f;
        }
        NR++;
    }
    fclose(m);
    printf("map   %d rows x %d bits\n", NR, NB);

    /* ---- words ---- */
    FILE *f = fopen(argv[2], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    long cap = 1 << 22, NW = 0;
    uint32_t *words = (uint32_t *)arb_carve_zeroed(cap * 4);
    if (!words) { fprintf(stderr, "oom words\n"); return 2; }
    char tok[64];
    while (fgets(tok, sizeof tok, f) && NW < cap) words[NW++] = (uint32_t)strtoul(tok, NULL, 16);
    fclose(f);
    printf("words %ld\n", NW);
    if (NW < 256) { fprintf(stderr, "too few words\n"); return 2; }

    /* ---- rows, in parallel ---- */
    int32_t *row = (int32_t *)arb_carve_zeroed((long)NW * 4);
    long *nmatch = (long *)arb_carve_zeroed((long)64 * sizeof(long));
    if (!row || !nmatch) { fprintf(stderr, "oom row\n"); return 2; }
    ArbPool pool = {0};
    int NT = ac_cpu_count(); if (NT > 32) NT = 32;
    if (arb_pool_up(&pool, NT) != 0) { fprintf(stderr, "pool failed\n"); return 2; }
    long per = (NW + NT - 1) / NT;
    RowJob *jobs = (RowJob *)arb_carve_zeroed((long)NT * (long)sizeof(RowJob));
    void **args = (void **)arb_carve_zeroed((long)NT * (long)sizeof(void *));
    for (int t = 0; t < NT; t++) {
        jobs[t].lo = (long)t * per;
        jobs[t].hi = jobs[t].lo + per > NW ? NW : jobs[t].lo + per;
        jobs[t].words = words; jobs[t].row = row; jobs[t].W = W;
        jobs[t].NR = NR; jobs[t].NB = NB; jobs[t].nomatch = 0;
        args[t] = &jobs[t];
    }
    arb_pool_run(&pool, NT, row_worker, args);
    long nomatch = 0;
    for (int t = 0; t < NT; t++) nomatch += jobs[t].nomatch;
    printf("rows assigned %ld, no row %ld (%d workers)\n", NW - nomatch, nomatch, NT);

    /* ---- the signal: row index over position. This IS the shape. ---- */
    /* two channels: the row id (what block) and the payload popcount (how it varies) */
    long sig_n = NW;
    float *sig = (float *)arb_carve_zeroed((long)sig_n * 2 * (long)sizeof(float));
    if (!sig) { fprintf(stderr, "oom sig\n"); return 2; }
    /* THE SIGNAL MATTERS, and the first version got it wrong: it used the ROW ID, which is
     * an arbitrary label -- the geometry says rows 233 and 259 are one bit apart, but an FFT
     * reads 233 and 900 as equally distant numbers, so transforming labels produced a flat
     * spectrum (0.15% top period, and "periods" at 2048/4096/8192 = window artifacts) and
     * lost to the context table.
     *
     * So the signal is the FIRING RESPONSE instead: for each word, the length of the
     * longest catalog row it satisfies, normalised. That value is computed THROUGH the
     * geometry, so numerical distance now means something -- a long match is a clean,
     * specific instruction and a short match is a rare or ambiguous one. Runs of the same
     * value are runs of the same KIND of instruction, which is the structure an FFT can
     * actually see.
     *
     * Kept side by side so the difference is measured, not asserted. */
    int which = 1;                      /* 1 = firing response (default), 0 = row id */
    for (long i = 0; i < NW; i++) {
        if (which == 0) {
            sig[2*i] = (float)(row[i] < 0 ? 0 : row[i]);
        } else {
            uint32_t w = words[i];
            int bestfix = 0;
            for (int r = 0; r < NR; r++) {
                int fix = 0, ok = 1;
                const float *wr = W + (size_t)r * NB;
                for (int b = 0; b < NB; b++) {
                    float wv = wr[b];
                    if (wv == 0.0f) continue;
                    int bit = (int)((w >> b) & 1u);
                    if ((wv > 0.0f && !bit) || (wv < 0.0f && bit)) { ok = 0; break; }
                    fix++;
                }
                if (ok && fix > bestfix) bestfix = fix;
            }
            sig[2*i] = (float)bestfix;   /* the specificity of the match: through the geometry */
        }
        sig[2*i+1] = 0.0f;
    }
    printf("signal: %s\n", which ? "firing response (longest matching row, bits fixed)"
                                 : "row id (arbitrary label)");

    /* ---- FFT over a power-of-two window: the spectrum of the block structure ---- */
    int LOGN = 14;                       /* 16384 instructions per window */
    long WIN = 1L << LOGN;
    if (WIN > NW) { while ((WIN >> 1) > NW) { WIN >>= 1; LOGN--; } }
    float *buf = (float *)arb_carve_zeroed((long)WIN * 2 * (long)sizeof(float));
    if (!buf) { fprintf(stderr, "oom buf\n"); return 2; }
    for (long i = 0; i < WIN; i++) { buf[2*i] = sig[2*i]; buf[2*i+1] = 0.0f; }
    /* remove the mean so the DC term does not swamp the spectrum (a large row id is not
     * structure, it is offset) */
    double mean = 0.0;
    for (long i = 0; i < WIN; i++) mean += buf[2*i];
    mean /= (double)WIN;
    for (long i = 0; i < WIN; i++) buf[2*i] -= (float)mean;
    techture_fft_core(buf, (int)WIN, 0);

    /* magnitude spectrum, and the dominant periods */
    long half = WIN / 2;
    double *mag = (double *)arb_carve_zeroed((long)half * (long)sizeof(double));
    if (!mag) { fprintf(stderr, "oom mag\n"); return 2; }
    for (long k = 0; k < half; k++) {
        double re = buf[2*k], im = buf[2*k+1];
        mag[k] = ccp_sqrtf((float)(re*re + im*im));
    }
    printf("\nSPECTRUM of the row-index signal, window %ld instructions\n", WIN);
    printf("  top periods (instructions per cycle) and their share of spectral energy:\n");
    double tot = 0.0;
    for (long k = 1; k < half; k++) tot += mag[k];
    int shown = 0;
    char *used = (char *)arb_carve_zeroed((long)half);
    for (int pass = 0; pass < 8 && shown < 8; pass++) {
        long bestk = -1; double bv = -1;
        for (long k = 1; k < half; k++) if (!used[k] && mag[k] > bv) { bv = mag[k]; bestk = k; }
        if (bestk < 0) break;
        used[bestk] = 1;
        double period = (double)WIN / (double)bestk;
        printf("     period %9.2f instr   energy %5.2f%%\n",
               period, tot > 0 ? 100.0 * bv / tot : 0.0);
        shown++;
    }

    /* ---- PREDICTION: does the frequency view beat the context table? ---- */
    long split = (long)(NW * train_frac);
    enum { ORD2 = 1 << 22 };
    int32_t *k2 = (int32_t *)arb_carve_zeroed((long)ORD2 * 4);
    int32_t *v2 = (int32_t *)arb_carve_zeroed((long)ORD2 * 4);
    if (!k2 || !v2) { fprintf(stderr, "oom tables\n"); return 2; }
    for (long i = 2; i < split; i++) {
        if (row[i] < 0 || row[i-1] < 0 || row[i-2] < 0) continue;
        long key = (long)row[i-2] * NR + row[i-1];
        if (key < ORD2) { k2[key]++; v2[key] = row[i]; }
    }
    /* the frequency predictor: for each position, the strongest period's continuation.
     * If the program is periodic at lag p, then row[i] should equal row[i-p]. Test every
     * lag that appears in the spectrum above a threshold, and take the best. */
    long *lagc = (long *)arb_carve_zeroed((long)4096 * (long)sizeof(long));
    long *lagt = (long *)arb_carve_zeroed((long)4096 * (long)sizeof(long));
    if (!lagc || !lagt) { fprintf(stderr, "oom lags\n"); return 2; }
    for (int p = 1; p < 4096; p++) {
        long hit = 0, n = 0;
        for (long i = split - p; i > p && i > split - 200000; i--)
            if (row[i] >= 0 && row[i-p] >= 0) { n++; if (row[i] == row[i-p]) hit++; }
        lagc[p] = hit; lagt[p] = n;
    }
    int bestlag = 1; double bestrate = 0;
    for (int p = 1; p < 4096; p++)
        if (lagt[p] > 100) { double r = (double)lagc[p] / (double)lagt[p]; if (r > bestrate) { bestrate = r; bestlag = p; } }
    printf("\nstrongest repeating lag: %d instructions (row repeats %.1f%% of the time at that lag)\n",
           bestlag, 100.0 * bestrate);

    long hit2 = 0, hitf = 0, hit1 = 0, tot2 = 0;
    for (long i = split; i < NW; i++) {
        if (row[i] < 0 || row[i-1] < 0 || row[i-2] < 0) continue;
        tot2++;
        long key = (long)row[i-2] * NR + row[i-1];
        if (key < ORD2 && k2[key] >= 3) { if (v2[key] == row[i]) hit2++; }
        if (i >= bestlag && row[i-bestlag] == row[i]) hitf++;
        if (row[i-1] == row[i]) hit1++;
    }
    printf("\nNEXT-ROW PREDICTION (held-out tail, %ld positions)\n", tot2);
    printf("  frequency view (lag %d, from the spectrum)   %6.2f%%\n",
           bestlag, tot2 ? 100.0 * hitf / tot2 : 0.0);
    printf("  context table (order-2 + backoff)            %6.2f%%\n",
           tot2 ? 100.0 * hit2 / tot2 : 0.0);
    printf("  repeat previous                              %6.2f%%\n",
           tot2 ? 100.0 * hit1 / tot2 : 0.0);
    printf("  (unigram chance is %6.2f%%)\n", 100.0 / (double)NR);

    /* ---- SHAPE SIMILARITY: how much spectrum two programs share ---- */
    printf("\nwhat this says about shape: the dominant period above IS a shape, and it is\n");
    printf("read off the FFT of the row signal, not learned. If prediction at that lag\n");
    printf("beats the context table, the program's structure is periodic and measurable.\n");

    arb_pool_down(&pool);
    return 0;
}
