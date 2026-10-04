/* =========================================================================
 *  hyperlang_primitives_vault.h
 *  =========================================================================
 *  HyperLang Sovereign Master Primitives & Fundamentals Vault:
 *
 *  1. SUB-SCALAR TRIAD (Sub-Bit Mechanics):
 *     - NanoBool  (Rank -3: 4-bit active gear)
 *     - MicroBool (Rank -2: 8-bit Newtonian gear)
 *     - MiniBool  (Rank -1: 16-bit astronomical clockwork)
 *
 *  2. FULL FLEET OF INTEGER PRIMITIVES:
 *     - NanoInt   (Rank -3: 4-bit modular/combinadic integer)
 *     - MicroInt  (Rank -2: 8-bit angular momentum coordinate)
 *     - MiniInt   (Rank -1: 16-bit continuous phase-space coordinate)
 *     - SuperInt  (Rank  1: Elastic combinadic constant-weight 70-state integer)
 *     - UltraInt  (Rank  3: 64-bit multi-scale coordinate Bulk32 * Horizon16 * Phase16)
 *
 *  3. SIX COMPOSITE OOP ACTIVE ENGINES:
 *     - BaseObject   (Rank 0: 64-Pole linear transmission shaft)
 *     - SuperObject  (Rank 1: 1D timing belt with acoustic wave propagation)
 *     - HyperObject  (Rank 2: Planetary 4-cylinder SATB harmonic engine)
 *     - UltraObject  (Rank 3: 3D toroidal Clifford lattice atom, rest mass)
 *     - MegaObject   (Rank 4: 3-speed epicyclic compound gearbox + C99 interop)
 *     - MetaObject   (Rank 5: Multi-node consortium rootbed)
 *
 *  4. COMPUTATIONAL SOVEREIGN PRIMITIVES:
 *     - SuperPtr     (Capability pointer with hardware parity self-defense)
 *     - HyperString  (3-level holographic zoomable text: L0/L1/L2)
 *     - HyperEnum    (4-track polyphonic chorded lifecycle enum)
 *     - HyperNull    (Explanatory typed void with explicit causality error codes)
 *     - MercenaryArray (Heterogeneous array with HNSW rootbed centroid routing)
 *
 *  5. FLOATING-POINT HORIZONS:
 *     - SBFP (Super-Boolean Floating Point with dynamic Radix Horizon switching)
 *     - HBFP (Hyper-Boolean Floating Point with base-16 exponent bias)
 * ========================================================================= */

#ifndef HYPERLANG_PRIMITIVES_VAULT_H
#define HYPERLANG_PRIMITIVES_VAULT_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "poly_binary_gearbox.h"
#include "hyperlang_gearbox_composite.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. NANOINT (Rank -3: 4-bit Modular & Combinadic Sub-Scalar Integer)
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    uint8_t legacy_val; /* C99 uint8_t view */
    uint8_t val : 4;    /* 4-bit unsigned [0..15] or 2's complement [-8..+7] */
    uint8_t _pad : 4;
} NanoInt;

static inline NanoInt nano_int_make(uint8_t v) {
    NanoInt ni;
    ni.val = v & 0x0F;
    ni.legacy_val = ni.val;
    ni._pad = 0;
    return ni;
}

static inline int8_t nano_int_to_signed(NanoInt ni) {
    int8_t s = (int8_t)(ni.val << 4);
    return (int8_t)(s >> 4);
}

static inline NanoInt nano_int_add(NanoInt a, NanoInt b) {
    return nano_int_make((a.val + b.val) & 0x0F);
}

/* =========================================================================
 * 2. MICROINT (Rank -2: 8-bit Angular Momentum Coordinate)
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    uint8_t legacy_val; /* C99 uint8_t view */
    int8_t  omega : 4;  /* Angular velocity [-8..+7] */
    uint8_t radius : 4; /* Radial distance [0..15] */
} MicroInt;

static inline MicroInt micro_int_make(int8_t omega, uint8_t radius) {
    MicroInt mi;
    mi.omega = omega & 0x0F;
    mi.radius = radius & 0x0F;
    mi.legacy_val = (uint8_t)(((uint8_t)(mi.omega & 0x0F) << 4) | (mi.radius & 0x0F));
    return mi;
}

static inline int16_t micro_int_momentum(MicroInt mi) {
    int8_t s_omega = (int8_t)(mi.omega << 4);
    s_omega = (int8_t)(s_omega >> 4);
    return (int16_t)(s_omega * (int16_t)mi.radius);
}

/* =========================================================================
 * 3. MINIINT (Rank -1: 16-bit Continuous Resonant Phase-Space Coordinate)
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    uint16_t legacy_val; /* C99 uint16_t view */
    uint8_t  bulk;       /* Radial magnitude [0..255] */
    uint8_t  phase;      /* Phase angle tick [0..255] across [0, 2π) */
} MiniInt;

static inline MiniInt mini_int_make(uint8_t bulk, uint8_t phase) {
    MiniInt mi;
    mi.bulk = bulk;
    mi.phase = phase;
    mi.legacy_val = (uint16_t)(((uint16_t)bulk << 8) | phase);
    return mi;
}

static inline float mini_int_project(MiniInt mi) {
    float rad = (float)mi.phase * (2.0f * (float)M_PI / 256.0f);
    return (float)mi.bulk * cosf(rad);
}

/* =========================================================================
 * 4. SUPERPTR (Rank 1: Self-Defending Capability Pointer)
 * ========================================================================= */
typedef struct {
    uintptr_t raw_address;      /* 48-bit address */
    uint8_t   radix_mode;       /* 1 = Physical RAM, 0 = Virtual Sandboxed */
    uint8_t   parity_tag;       /* Hardware self-defense parity */
} SuperPtr;

static inline SuperPtr super_ptr_make(void *ptr, uint8_t radix_mode) {
    SuperPtr sp;
    sp.raw_address = (uintptr_t)ptr;
    sp.radix_mode = radix_mode & 1;
    sp.parity_tag = (uint8_t)(__builtin_parityll(sp.raw_address) ^ sp.radix_mode);
    return sp;
}

static inline void* super_ptr_deref(const SuperPtr *sp, bool *out_trap) {
    uint8_t computed = (uint8_t)(__builtin_parityll(sp->raw_address) ^ sp->radix_mode);
    if (computed != sp->parity_tag) {
        if (out_trap) *out_trap = true; /* Trapped bit-flip / Rowhammer exploit */
        return NULL;
    }
    if (out_trap) *out_trap = false;
    return (void*)sp->raw_address;
}

/* =========================================================================
 * 5. HYPERSTRING (Rank 2: 3-Level Holographic Zoomable Text)
 * ========================================================================= */
typedef struct {
    char level0_abstract[32];   /* Ultra-compressed 5-word essence (O(1) search) */
    char level1_narrative[128]; /* 1-sentence human narrative overview */
    char level2_technical[512]; /* Full exhaustive technical telemetry */
} HyperString;

static inline HyperString hyper_string_make(const char *l0, const char *l1, const char *l2) {
    HyperString hs;
    strncpy(hs.level0_abstract, l0 ? l0 : "", 31);
    hs.level0_abstract[31] = '\0';
    strncpy(hs.level1_narrative, l1 ? l1 : "", 127);
    hs.level1_narrative[127] = '\0';
    strncpy(hs.level2_technical, l2 ? l2 : "", 511);
    hs.level2_technical[511] = '\0';
    return hs;
}

static inline const char* hyper_string_zoom(const HyperString *hs, int zoom_level) {
    if (zoom_level == 0) return hs->level0_abstract;
    if (zoom_level == 1) return hs->level1_narrative;
    return hs->level2_technical;
}

/* =========================================================================
 * 6. HYPERENUM (Rank 2: 4-Track Polyphonic Chorded Lifecycle Enum)
 * ========================================================================= */
typedef struct {
    uint8_t track_core_exec : 1; /* Track 3: 0=IDLE, 1=ACTIVE */
    uint8_t track_security  : 1; /* Track 2: 0=GUEST, 1=ADMIN */
    uint8_t track_cache     : 1; /* Track 1: 0=CLEAN, 1=DIRTY */
    uint8_t track_network   : 1; /* Track 0: 0=OFFLINE, 1=ONLINE */
    uint8_t _pad            : 4;
} HyperEnum;

static inline HyperEnum hyper_enum_make(bool active, bool admin, bool dirty, bool online) {
    HyperEnum he;
    he.track_core_exec = active ? 1 : 0;
    he.track_security  = admin ? 1 : 0;
    he.track_cache     = dirty ? 1 : 0;
    he.track_network   = online ? 1 : 0;
    he._pad = 0;
    return he;
}

static inline bool hyper_enum_is_harmonic(HyperEnum he) {
    /* Even parity across the 4 lifecycle tracks = Harmonic Operational State */
    int p = he.track_core_exec ^ he.track_security ^ he.track_cache ^ he.track_network;
    return (p == 0);
}

/* =========================================================================
 * 7. HYPERNULL (Rank 2: Explanatory Typed Void with Error Causality)
 * ========================================================================= */
typedef enum {
    HYPERNULL_NONE       = 0x0,
    HYPERNULL_PENDING    = 0x2, /* Computation deferred async */
    HYPERNULL_PERMISSION = 0x4, /* Security sandbox restriction */
    HYPERNULL_TIMEOUT    = 0x8, /* Network timeout, safe to retry */
    HYPERNULL_ASYMPTOTE  = 0xA, /* Subnormal floating-point underflow */
    HYPERNULL_PURGED_404 = 0xE  /* Resource permanently purged */
} HyperNullCode;

typedef struct {
    void         *ptr;
    HyperNullCode null_code;
    const char   *diagnostic;
} HyperNull;

static inline HyperNull hyper_null_make(HyperNullCode code, const char *diag) {
    HyperNull hn;
    hn.ptr = NULL;
    hn.null_code = code;
    hn.diagnostic = diag;
    return hn;
}

/* =========================================================================
 * 8. SUPERINT (Rank 1: Elastic Combinadic Constant-Weight Integer)
 * ========================================================================= */
/* Fixed-weight permutations: exactly 4 ones out of 8 bits => C(8,4) = 70 overflow-free states */
static const uint8_t BINOM_TABLE[9][5] = {
    {1, 0,  0,  0,  0},   /* n=0 */
    {1, 1,  0,  0,  0},   /* n=1 */
    {1, 2,  1,  0,  0},   /* n=2 */
    {1, 3,  3,  1,  0},   /* n=3 */
    {1, 4,  6,  4,  1},   /* n=4 */
    {1, 5, 10, 10,  5},   /* n=5 */
    {1, 6, 15, 20, 15},   /* n=6 */
    {1, 7, 21, 35, 35},   /* n=7 */
    {1, 8, 28, 56, 70}    /* n=8 */
};

typedef struct {
    uint8_t permutation_bits; /* 8 bits, exactly 4 ones */
    uint8_t rank_index;       /* Permutation rank [0..69] */
} SuperInt;

/* Exact Combinadic Unranker: N = sum_{i=1}^4 C(c_i, i) */
static inline SuperInt super_int_make(uint8_t rank) {
    SuperInt si;
    si.rank_index = rank % 70;
    uint8_t N = si.rank_index;
    uint8_t p = 0;
    int c_prev = 8;
    for (int i = 4; i >= 1; i--) {
        int c = c_prev - 1;
        while (c >= i - 1 && BINOM_TABLE[c][i] > N) {
            c--;
        }
        N -= BINOM_TABLE[c][i];
        p |= (uint8_t)(1U << c);
        c_prev = c;
    }
    si.permutation_bits = p;
    return si;
}

/* Exact Combinadic Ranker: maps 4-of-8 bit pattern back to rank [0..69] */
static inline uint8_t super_int_rank(uint8_t permutation_bits) {
    uint8_t r = 0;
    int k = 1;
    for (int b = 0; b < 8; b++) {
        if ((permutation_bits >> b) & 1) {
            r += BINOM_TABLE[b][k++];
            if (k > 4) break;
        }
    }
    return r;
}

static inline SuperInt super_int_add(SuperInt a, int delta) {
    int new_rank = ((int)a.rank_index + delta) % 70;
    if (new_rank < 0) new_rank += 70;
    return super_int_make((uint8_t)new_rank);
}

/* =========================================================================
 * 9. ULTRAINT (Rank 3: 64-bit Multi-Scale Toroidal Coordinate)
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    uint64_t legacy_val; /* C99 uint64_t view */
    int32_t  bulk;       /* 32-bit linear magnitude */
    uint16_t horizon;    /* 16-bit dynamic horizon multiplier */
    uint16_t phase;      /* 16-bit Clifford continuous angle */
} UltraInt;

static inline UltraInt ultra_int_make(int32_t bulk, uint16_t horizon, uint16_t phase) {
    UltraInt ui;
    ui.bulk = bulk;
    ui.horizon = horizon;
    ui.phase = phase;
    ui.legacy_val = ((uint64_t)(uint32_t)bulk << 32) | ((uint64_t)horizon << 16) | (uint64_t)phase;
    return ui;
}

static inline double ultra_int_resolve(UltraInt ui) {
    double rad = (double)ui.phase * (2.0 * M_PI / 65536.0);
    double h = (ui.horizon == 0) ? 1.0 : (double)ui.horizon;
    return ((double)ui.bulk * h) * cos(rad);
}

/* =========================================================================
 * 10. MERCENARY ARRAY (Rank 5 / Meta: Heterogeneous Rootbed with HNSW Search)
 * ========================================================================= */
#define MERCENARY_CAPACITY 8

typedef enum {
    MERCENARY_COLLIDER = 0,
    MERCENARY_SYNTH_VOICE = 1,
    MERCENARY_NEURAL_WEIGHT = 2,
    MERCENARY_DB_RECORD = 3
} MercenaryType;

typedef struct {
    char          member_id[16];
    MercenaryType class_type;
    uint16_t      harness_hex;  /* 4-digit hex behavioral profile */
    void*         payload_ptr;
} MercenaryMember;

typedef struct {
    MercenaryMember members[MERCENARY_CAPACITY];
    size_t          count;
    uint16_t        root_hub_harness; /* Elected HNSW Layer 1 hub */
} MercenaryArray;

static inline void mercenary_array_init(MercenaryArray *ma) {
    ma->count = 0;
    ma->root_hub_harness = 0;
}

static inline void mercenary_array_enlist(MercenaryArray *ma, const char *id, MercenaryType t, uint16_t hex) {
    if (ma->count >= MERCENARY_CAPACITY) return;
    MercenaryMember *m = &ma->members[ma->count++];
    strncpy(m->member_id, id, 15);
    m->member_id[15] = '\0';
    m->class_type = t;
    m->harness_hex = hex;
    ma->root_hub_harness ^= hex; /* Centroid evolution */
}

static inline int mercenary_find_closest(const MercenaryArray *ma, uint16_t query_hex) {
    int best_idx = -1;
    int min_dist = 999;
    for (size_t i = 0; i < ma->count; i++) {
        int dist = __builtin_popcount(ma->members[i].harness_hex ^ query_hex);
        if (dist < min_dist) {
            min_dist = dist;
            best_idx = (int)i;
        }
    }
    return best_idx;
}

/* =========================================================================
 * 11. FLOATING-POINT HORIZONS: SBFP & HBFP
 * ========================================================================= */
/* SBFP: Super-Boolean Radix Horizon Floating Point (Rank 1) */
typedef struct {
    uint8_t header_horizon;     /* 1 = Integral Mode, 0 = Fractional Mode */
    uint8_t depth_digits[4];    /* 4 depth precision levels */
} SBFP_Column;

static inline float sbfp_evaluate(const SBFP_Column *col) {
    float val = 0.0f;
    if (col->header_horizon == 1) {
        /* Integral Mode: Above Horizon */
        for (int j = 0; j < 4; j++) {
            val += (float)col->depth_digits[j] * (float)(1 << (3 - j));
        }
    } else {
        /* Fractional Mode: Below Horizon */
        for (int j = 0; j < 4; j++) {
            val += (float)col->depth_digits[j] * powf(2.0f, -(float)(j + 1));
        }
    }
    return val;
}

/* HBFP: Hyper-Boolean Floating Point (Rank 2) with Base-16 Exponent Bias */
typedef struct {
    uint8_t val[4]; /* 4 mantissa nibbles [0..15] */
    uint8_t exp[4]; /* 4 exponent nibbles [0..15], bias = 8 */
} HBFP;

static inline HBFP hbfp_make(const uint8_t v[4], const uint8_t e[4]) {
    HBFP h;
    for (int i = 0; i < 4; i++) {
        h.val[i] = v[i] & 0x0F;
        h.exp[i] = e[i] & 0x0F;
    }
    return h;
}

static inline double hbfp_evaluate(const HBFP *h) {
    double sum = 0.0;
    for (int i = 0; i < 4; i++) {
        int shift = (int)h->exp[i] - 8;
        sum += (double)h->val[i] * pow(16.0, (double)shift);
    }
    return sum;
}

#ifdef __cplusplus
}
#endif

#endif /* HYPERLANG_PRIMITIVES_VAULT_H */
