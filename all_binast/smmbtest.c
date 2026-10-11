/* smmbtest.c -- does smmb_gemm compute the firing, and smmb_outer_T the gradient into
 * the program, with the map frozen?
 *
 * The claim being tested is the architecture: the catalog is a fixed filter bank
 * (rows x bits), a program is an input image (positions x bits), firing is a matrix
 * product, and backprop goes into the PROGRAM while the map stays fixed. If smmb_gemm is
 * literally C = A @ W^T then firing IS that call, with A = program words and W = catalog
 * rows. And if smmb_outer_T is literally dA = dC @ W, then the gradient into the program
 * is that call, with W unchanged -- which is the frozen-map requirement.
 *
 * This is the real kernel, copied from cygemm/smmb/fullmetal_smmb_c.dry:1900 with only
 * the device-dispatch branches removed (cur_mode is 1 in the emitted code, so the NEON
 * blocked path is the one that ever runs). The arithmetic is untouched.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

/* the two globals the emitted kernel reads; the emitted defaults are these values */
static int  smmb_cur_mode  = 1;     /* drygon: static int cur_mode = 1; */
static int  smmb_cur_chunk = 64;    /* drygon: static int cur_chunk = 64; */
__attribute__((unused)) static int smmb_dense_threads = 1; /* no OpenMP here; single-threaded on purpose so the
                                     * test compares ARITHMETIC, not scheduling */

/* ------------------------------------------------------------------ the kernels -- */
/* C[N,No] = A[N,Ki] @ W[No,Ki]^T   -- verbatim from fullmetal_smmb_c.dry:1900 */
static void smmb_gemm(float *C, const float *A, const float *W, int N_, int No, int Ki) {
    if (smmb_cur_mode == 0 || smmb_cur_chunk > Ki || smmb_cur_chunk <= 0) {
        int block_s = 64, block_r = 64;
        for (int r0 = 0; r0 < No; r0 += block_r) {
            int rmax = r0 + block_r < No ? r0 + block_r : No;
            for (int s0 = 0; s0 < N_; s0 += block_s) {
                int smax = s0 + block_s < N_ ? s0 + block_s : N_;
                for (int s = s0; s < smax; s++) {
                    const float *as = A + (size_t)s * Ki;
                    float *cs = C + (size_t)s * No;
                    for (int rr = r0; rr < rmax; rr++) {
                        const float *wr = W + (size_t)rr * Ki;
                        float r = 0;
                        for (int k = 0; k < Ki; k++) r += as[k] * wr[k];
                        cs[rr] = r;
                    }
                }
            }
        }
        return;
    }
    /* mode=1: SMMB chunk-by-Ki stream */
    int chunk = smmb_cur_chunk;
    memset(C, 0, (size_t)N_ * No * sizeof(float));
    for (int s = 0; s < N_; s++) {
        const float *as = A + (size_t)s * Ki;
        float *cs = C + (size_t)s * No;
        for (int k0 = 0; k0 < Ki; k0 += chunk) {
            int kmax = k0 + chunk < Ki ? k0 + chunk : Ki;
            for (int rr = 0; rr < No; rr++) {
                float r = cs[rr];
                for (int k = k0; k < kmax; k++) r += as[k] * W[(size_t)rr * Ki + k];
                cs[rr] = r;
            }
        }
    }
}

/* dx[N_,Ki] += dC[N_,No] @ W[No,Ki] * sc  -- from fullmetal_smmb_c.dry:2088 */
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

/* ----------------------------------------------------------------- the fixture -- */
/* A tiny hand-checkable catalog: 3 rows x 4 bits, and 2 program words. */
#define KI 4
#define NROWS 3
#define NW 2

static int fails = 0;
static void check(const char *what, int ok) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    /* the map: row r fixes bit k to +1 if W[r][k] > 0, -1 if < 0, 0 if it does not care.
     * This is exactly the 1 / -1 / 0 ternary the .mtx exports. */
    float W[NROWS][KI] = {
        {  1,  1,  0, -1 },     /* row 0 */
        {  1, -1,  0,  0 },     /* row 1 */
        {  0,  1,  1,  1 },     /* row 2 */
    };
    /* two program words as bits: one hot-encoded 1/-1 per bit */
    float A[NW][KI] = {
        {  1,  1, -1, -1 },     /* word 0: matches row 0 exactly */
        {  1, -1, -1,  1 },     /* word 1: matches row 1 on its 2 fixed bits */
    };

    printf("== smmb_gemm IS the firing (C = A @ W^T)\n");
    float C[NW][NROWS];
    smmb_gemm(&C[0][0], &A[0][0], &W[0][0], NW, NROWS, KI);

    /* hand-computed expectation: C[s][r] = sum_k A[s][k]*W[r][k] */
    int ok = 1;
    for (int s = 0; s < NW; s++)
        for (int r = 0; r < NROWS; r++) {
            float exp = 0;
            for (int k = 0; k < KI; k++) exp += A[s][k] * W[r][k];
            if (fabsf(C[s][r] - exp) > 1e-5f) { ok = 0; printf("     C[%d][%d]=%g want %g\n", s, r, C[s][r], exp); }
        }
    check("catalog (rows x bits) x program (words x bits) = activation map", ok);

    /* the arithmetic meaning: row 0 fixes bits {0:+1,1:+1,3:-1}; word 0 is exactly that */
    check("word 0's best-scoring row is the row it IS (argmax = row 0)", C[0][0] > C[0][1] && C[0][0] > C[0][2]);
    check("word 1's best-scoring row is row 1", C[1][1] > C[1][0] && C[1][1] > C[1][2]);
    printf("     the activation map:\n");
    for (int s = 0; s < NW; s++) {
        printf("       word %d: ", s);
        for (int r = 0; r < NROWS; r++) printf("row%d=%+g ", r, C[s][r]);
        printf("\n");
    }

    printf("\n== smmb_outer_T IS the gradient into the program, map frozen\n");
    /* take a gradient on the activation map and propagate it back to the words */
    float dC[NW][NROWS] = { { 1, 0, 0 }, { 0, 1, 0 } };
    float W_before[NROWS][KI];
    memcpy(W_before, W, sizeof W);
    float dA[NW][KI];
    memset(dA, 0, sizeof dA);
    smmb_outer_T(&dA[0][0], &dC[0][0], &W[0][0], NW, NROWS, KI, 1.0f);

    ok = 1;
    for (int s = 0; s < NW; s++)
        for (int k = 0; k < KI; k++) {
            float exp = 0;
            for (int r = 0; r < NROWS; r++) exp += dC[s][r] * W[r][k];
            if (fabsf(dA[s][k] - exp) > 1e-5f) ok = 0;
        }
    check("dA = dC @ W (the transpose-multiply that backs through the gemm)", ok);
    check("the MAP is unchanged by backprop (frozen, as the architecture requires)",
          memcmp(W_before, W, sizeof W) == 0);
    printf("     dA (a gradient ON THE PROGRAM, same shape as the program):\n");
    for (int s = 0; s < NW; s++) {
        printf("       word %d: ", s);
        for (int k = 0; k < KI; k++) printf("%+g ", dA[s][k]);
        printf("\n");
    }

    printf("\n== the shapes are the model\n");
    printf("     catalog   %d rows x %d bits     W, the fixed filter bank\n", NROWS, KI);
    printf("     program   %d words x %d bits    A, the input image\n", NW, KI);
    printf("     firing    %d words x %d rows    C, the activation map\n", NW, NROWS);
    printf("     dA        %d words x %d bits    gradient on the INPUT, not on W\n", NW, KI);

    printf("\n%s\n", fails ? "SMMB KERNELS: FAILURES ABOVE" : "SMMB KERNELS: ALL CHECKS PASSED");
    return fails ? 1 : 0;
}
