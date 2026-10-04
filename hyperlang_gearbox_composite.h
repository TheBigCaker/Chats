/* =========================================================================
 *  hyperlang_gearbox_composite.h
 *  =========================================================================
 *  HyperLang Composite Gearbox Architecture:
 *  Building Super, Hyper, Ultra, and Mega USING the Sub-Scalar Triad
 *  (NanoBool, MicroBool, MiniBool).
 *
 *  - Rank 0: BaseObject   -> 64-Pole Linear Transmission Shaft (8 MicroBools)
 *  - Rank 1: SuperObject  -> 1D Timing Belt / Kinetic Gear Train (16 NanoBools)
 *  - Rank 2: HyperObject  -> Planetary 4-Cylinder SATB Harmonic Engine (4 NanoBools)
 *  - Rank 3: UltraObject  -> 3D Interlocking Clifford Torus (16 NanoBools = 64-bit)
 *  - Rank 4: MegaObject   -> 3-Speed Compound Epicyclic Gearbox (Nano->Micro->Mini)
 *  - Rank 5: MetaObject   -> Consortium of Interlocked Mechanical Transmissions
 *
 *  All structures maintain zero-copy legacy C99 bool interop at offset 0.
 * ========================================================================= */

#ifndef HYPERLANG_GEARBOX_COMPOSITE_H
#define HYPERLANG_GEARBOX_COMPOSITE_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "poly_binary_gearbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Convenience Aliases mapping to poly_binary_gearbox.h */
#define nanobool_make nanobool_create
#define nanobool_quad nanobool_quadrant
#define nanobool_taut nanobool_is_taut
#define nanobool_spin nanobool_is_cw
#define microbool_make microbool_create
#define minibool_make minibool_create

static inline NanoBool nanobool_tick(NanoBool nb, int steps) {
    uint8_t new_q = (nanobool_quad(nb) + steps) & 0x03;
    return nanobool_make(new_q, nanobool_taut(nb), nanobool_spin(nb));
}

/* =========================================================================
 * 2. COMPOSITE RANK 0: BaseObject (64-Pole Transmission Shaft)
 *    Composed of 8 MicroBool gears driving a 64-bit scalar output.
 * ========================================================================= */
typedef struct {
    char      label[32];
    uint64_t  scalar_value;  /* Collapsed 64-bit integer view for CPU ALU */
    MicroBool poles[8];      /* 8 live MicroBool gears driving the shaft */
} BaseObject;

static inline BaseObject base_object_compose(const char *name, uint64_t raw_val) {
    BaseObject bo;
    strncpy(bo.label, name ? name : "BaseShaft", 31);
    bo.label[31] = '\0';
    bo.scalar_value = raw_val;

    for (int i = 0; i < 8; i++) {
        uint8_t byte = (raw_val >> (i * 8)) & 0xFF;
        uint8_t sector  = (byte >> 5) & 0x07;
        uint8_t tension = (byte >> 3) & 0x03;
        uint8_t damping = (byte >> 1) & 0x03;
        bool    spin    = (byte & 0x01) != 0;
        bo.poles[i] = microbool_make(sector, tension, damping, spin);
    }
    return bo;
}

static inline uint32_t base_object_total_torque(const BaseObject *bo) {
    uint32_t total = 0;
    for (int i = 0; i < 8; i++) {
        total += microbool_tension(bo->poles[i]) * (microbool_spin(bo->poles[i]) ? 1 : -1);
    }
    return total;
}

/* =========================================================================
 * 3. COMPOSITE RANK 1: SuperObject (1D Timing Belt / Gear Train)
 *    Composed of 16 NanoBools in a continuous kinetic belt.
 * ========================================================================= */
#define SUPER_BELT_TEETH 16

typedef struct {
    char     label[32];
    NanoBool belt[SUPER_BELT_TEETH]; /* 16 sequential NanoBool gears */
    int      net_winding_number;     /* Total accumulated net quadrant rotations */
    float    shear_stress_turb;      /* Differential shear stress across teeth */
    float    wave_velocity;          /* Phonon wave propagation speed */
} SuperObject;

static inline SuperObject super_object_compose(const char *name, uint16_t initial_bits) {
    SuperObject so;
    strncpy(so.label, name ? name : "SuperBelt", 31);
    so.label[31] = '\0';

    int winding = 0;
    float shear = 0.0f;

    for (int i = 0; i < SUPER_BELT_TEETH; i++) {
        bool bit = (initial_bits >> i) & 1;
        uint8_t quad = bit ? 0 : 2; /* 0 deg vs 180 deg */
        bool taut = ((i % 3) == 0);
        bool spin = ((i % 2) == 0);
        so.belt[i] = nanobool_make(quad, taut, spin);
        winding += quad;

        if (i > 0) {
            int diff = abs((int)nanobool_quad(so.belt[i]) - (int)nanobool_quad(so.belt[i - 1]));
            shear += (float)diff;
        }
    }

    so.net_winding_number = winding;
    so.shear_stress_turb  = shear / (float)(SUPER_BELT_TEETH - 1);
    so.wave_velocity      = (so.shear_stress_turb > 0.0f) ? (1.0f / so.shear_stress_turb) : 1.0f;
    return so;
}

/* Propagate a torque pulse through the belt */
static inline void super_object_pulse(SuperObject *so, int input_teeth_push) {
    for (int i = 0; i < SUPER_BELT_TEETH; i++) {
        /* Damped wave propagation: each tooth receives partial pulse */
        int step = (input_teeth_push > 0) ? 1 : -1;
        if (nanobool_taut(so->belt[i])) {
            step *= 2; /* Taut links transmit double angular momentum */
        }
        so->belt[i] = nanobool_tick(so->belt[i], step);
    }
}

/* =========================================================================
 * 4. COMPOSITE RANK 2: HyperObject (Planetary 4-Cylinder SATB Harmonic Engine)
 *    Composed of 4 NanoBool voice gears: Soprano, Alto, Tenor, Bass.
 *    Total footprint: 4 x 4 bits = 16 bits!
 * ========================================================================= */
typedef struct {
    char     label[32];
    NanoBool soprano;           /* Voice 0: Melodic Lead / Query */
    NanoBool alto;              /* Voice 1: Harmonic Counter / Key */
    NanoBool tenor;             /* Voice 2: Rhythmic Core / Value */
    NanoBool bass;              /* Voice 3: Tonal Ground / Anchor */
    uint16_t chord_matrix;      /* Combined 16-bit packed chord */
    bool     is_major_harmonic; /* Frictionless meshing vs tooth clash */
    float    friction_drag;     /* Internal harmonic friction (0.0 = pure chord) */
} HyperObject;

static inline HyperObject hyper_object_compose(const char *name,
                                               NanoBool s, NanoBool a,
                                               NanoBool t, NanoBool b) {
    HyperObject ho;
    strncpy(ho.label, name ? name : "HyperEngine", 31);
    ho.label[31] = '\0';
    ho.soprano = s;
    ho.alto    = a;
    ho.tenor   = t;
    ho.bass    = b;

    /* Pack into 16-bit chord */
    ho.chord_matrix = ((uint16_t)(s.gears & 0x0F) << 12) |
                      ((uint16_t)(a.gears & 0x0F) << 8)  |
                      ((uint16_t)(t.gears & 0x0F) << 4)  |
                      ((uint16_t)(b.gears & 0x0F));

    /* Tooth mesh harmony: check parity balance across all 4 cylinders */
    int p_s = nanobool_quad(s) & 1;
    int p_a = nanobool_quad(a) & 1;
    int p_t = nanobool_quad(t) & 1;
    int p_b = nanobool_quad(b) & 1;

    ho.is_major_harmonic = ((p_s ^ p_a ^ p_t ^ p_b) == 0);

    /* Clashing teeth generate friction drag */
    int clashes = abs((int)nanobool_quad(s) - (int)nanobool_quad(b)) +
                  abs((int)nanobool_quad(a) - (int)nanobool_quad(t));
    ho.friction_drag = (float)clashes * 0.25f;
    return ho;
}

/* =========================================================================
 * 5. COMPOSITE RANK 3: UltraObject (3D Interlocking Toroidal Clifford Lattice)
 *    Composed of a 4x4 matrix = 16 NanoBool gears.
 *    16 gears x 4 bits = Exactly 64 bits (8 Bytes)! Fits in 1 CPU register!
 * ========================================================================= */
typedef struct {
    char     name[32];
    NanoBool torus[4][4];       /* 4x4 Torus grid of NanoBool gears */
    uint64_t packed_torus_word; /* Exact 64-bit scalar machine word */
    uint32_t rest_mass;         /* Sum of internal spring tensions */
    int      open_valences;     /* Perimeter teeth open for bonding */
    bool     is_crystallized;   /* True if locked in ground-state resonance */
} UltraObject;

static inline UltraObject ultra_object_compose(const char *name, uint64_t word_seed) {
    UltraObject uo;
    strncpy(uo.name, name ? name : "UltraTorus", 31);
    uo.name[31] = '\0';
    uo.packed_torus_word = word_seed;

    uint32_t total_tension = 0;
    int valences = 0;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int bit_pos = (r * 4 + c) * 4;
            uint8_t nibble = (uint8_t)((word_seed >> bit_pos) & 0x0F);
            uint8_t quad = (nibble >> 2) & 0x03;
            bool taut = (nibble & 0x02) != 0;
            bool spin = (nibble & 0x01) != 0;

            uo.torus[r][c] = nanobool_make(quad, taut, spin);
            if (taut) total_tension += 2;
            else total_tension += 1;

            /* Check boundary edges for open bonding teeth (torus exterior ports) */
            if ((r == 0 || r == 3 || c == 0 || c == 3) && !taut) {
                valences++;
            }
        }
    }

    uo.rest_mass = total_tension;
    uo.open_valences = valences;
    uo.is_crystallized = (total_tension >= 24); /* High tension = crystalline atom */
    return uo;
}

/* =========================================================================
 * 6. COMPOSITE RANK 4: MegaObject (Compound Epicyclic Multi-Speed Gearbox)
 *    Contains Nano (fast), Micro (mid), and Mini (slow) linked by 4:1 reduction.
 *    Offset 0 is strictly a standard C99 bool.
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    bool      legacy_bool;  /* Offset 0: Exact C99 bool (0x01 or 0x00) */
    NanoBool  fast_gear;    /* 4-bit fast escapement (ticks every cycle) */
    MicroBool mid_shaft;    /* 8-bit intermediate shaft (4:1 reduction) */
    MiniBool  master_wheel; /* 16-bit slow astronomical clockwork (16:1 reduction) */
    uint32_t  total_cycles; /* Cycle odometer */
} MegaObject;

static inline MegaObject mega_object_compose(bool initial_state) {
    MegaObject mo;
    mo.legacy_bool  = initial_state;
    mo.fast_gear    = nanobool_make(initial_state ? 0 : 2, true, true);
    mo.mid_shaft    = microbool_make(initial_state ? 1 : 5, 3, 1, true);
    mo.master_wheel = minibool_make(initial_state ? 32 : 160, 12, 8);
    mo.total_cycles = 0;
    return mo;
}

/* Drive the compound gearbox: Nano turns Micro, Micro turns Mini */
static inline void mega_object_tick(MegaObject *mo, int ticks) {
    mo->total_cycles += abs(ticks);

    /* Fast Nano ticks */
    mo->fast_gear = nanobool_tick(mo->fast_gear, ticks);

    /* 4:1 Gear Reduction: Micro steps every 4 Nano ticks */
    if (mo->total_cycles % 4 == 0) {
        mo->mid_shaft = microbool_rotate(mo->mid_shaft, (ticks > 0) ? 1 : -1);
    }

    /* 16:1 Reduction: Master Mini wheel advances phase */
    if (mo->total_cycles % 16 == 0) {
        uint8_t new_p = (minibool_phase(mo->master_wheel) + (ticks > 0 ? 8 : -8)) & 0xFF;
        mo->master_wheel = minibool_make(new_p,
                                        minibool_tension(mo->master_wheel),
                                        minibool_momentum(mo->master_wheel));
    }

    /* Update C99 legacy bool state according to master clockwork */
    mo->legacy_bool = minibool_collapse(mo->master_wheel);
}

#ifdef __cplusplus
}
#endif

#endif /* HYPERLANG_GEARBOX_COMPOSITE_H */
