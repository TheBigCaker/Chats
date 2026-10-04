/* =========================================================================
 *  hyperlang_primitives_vault_engine.c
 *  =========================================================================
 *  Empirical Verification of the Complete Sovereign Fleet of HyperLang Primitives:
 *  - Sub-Scalar Integers: NanoInt, MicroInt, MiniInt, UltraInt
 *  - Combinadic Bijection: SuperInt (70 unique patterns, bijection rank/unrank)
 *  - Pointer Defense: SuperPtr (Parity hardware self-defense)
 *  - Holographic Text: HyperString (3-scale zoomable text)
 *  - Multi-Track Lifecycle: HyperEnum (4-track lifecycle harmony)
 *  - Explanatory Void: HyperNull (Typed error causality)
 *  - Heterogeneous Rootbed: MercenaryArray (HNSW Hamming routing)
 *  - Dual Radix Floating-Point: SBFP & HBFP
 * ========================================================================= */

#include "hyperlang_primitives_vault.h"

int main(void) {
    printf("===================================================================\n");
    printf("   HYPERLANG MASTER PRIMITIVES & FUNDAMENTALS VAULT ENGINE         \n");
    printf("   Exhaustive Verification of the Sovereign Fleet Across All Ranks \n");
    printf("===================================================================\n\n");

    /* 1. Sub-Scalar Integers (NanoInt, MicroInt, MiniInt, UltraInt) */
    printf("[1] Sub-Scalar & Toroidal Integer Primitives:\n");
    NanoInt ni = nano_int_make(14); /* 0b1110 -> 14 unsigned, -2 signed */
    NanoInt ni2 = nano_int_make(3);
    NanoInt ni_sum = nano_int_add(ni, ni2); /* (14 + 3) & 0xF = 1 */
    printf("  • NanoInt (Rank -3, 4b): val=%u, signed=%d, (14 + 3 mod 16) = %u\n",
           ni.val, nano_int_to_signed(ni), ni_sum.val);
    assert(ni_sum.val == 1);

    MicroInt mi = micro_int_make(-3, 10); /* omega = -3, radius = 10 */
    int16_t momentum = micro_int_momentum(mi);
    printf("  • MicroInt (Rank -2, 8b): omega=%d, radius=%u => Momentum (L=r*w) = %d\n",
           mi.omega, mi.radius, momentum);
    assert(momentum == -30);

    MiniInt mii = mini_int_make(100, 64); /* 64/256 = 90 degrees -> cos(90 deg) = 0 */
    float proj = mini_int_project(mii);
    printf("  • MiniInt (Rank -1, 16b): bulk=%u, phase=%u (90 deg) => Projection = %.4f\n",
           mii.bulk, mii.phase, proj);
    assert(fabsf(proj) < 1e-4f);

    UltraInt ui = ultra_int_make(500, 4, 0); /* 500 * 4 * cos(0) = 2000.0 */
    double resolved = ultra_int_resolve(ui);
    printf("  • UltraInt (Rank 3, 64b): bulk=%d, horizon=%u, phase=%u => Resolved = %.2f\n\n",
           ui.bulk, ui.horizon, ui.phase, resolved);
    assert(fabs(resolved - 2000.0) < 1e-4);

    /* 2. SuperInt Combinadic Invariant & Bijection Verification */
    printf("[2] SuperInt (Rank 1: Elastic Combinadic 70-State Integer):\n");
    uint8_t seen[256] = {0};
    int unique_count = 0;
    for (uint8_t r = 0; r < 70; r++) {
        SuperInt s = super_int_make(r);
        int pop = __builtin_popcount(s.permutation_bits);
        assert(pop == 4); /* Invariant: strictly 4 bits set */
        
        uint8_t unranked = super_int_rank(s.permutation_bits);
        assert(unranked == r); /* Invariant: exact bijection */
        
        if (!seen[s.permutation_bits]) {
            seen[s.permutation_bits] = 1;
            unique_count++;
        }
    }
    printf("  • Verified Combinadic Bijection: Exactly %d / 70 Unique Weight-4 Patterns!\n", unique_count);
    assert(unique_count == 70);

    SuperInt si_step = super_int_make(68);
    SuperInt si_next = super_int_add(si_step, 5); /* (68 + 5) mod 70 = 3 */
    printf("  • Circular Step: Rank %d (0x%02X) + 5 -> Rank %d (0x%02X) [Zero Overflow!]\n\n",
           si_step.rank_index, si_step.permutation_bits, si_next.rank_index, si_next.permutation_bits);
    assert(si_next.rank_index == 3);

    /* 3. SuperPtr Hardware Self-Defense */
    printf("[3] SuperPtr (Capability Self-Defending Pointer):\n");
    int secret_data = 1337;
    SuperPtr sp = super_ptr_make(&secret_data, 1);
    bool trapped = false;
    int *read_ptr = (int*)super_ptr_deref(&sp, &trapped);
    printf("  • Valid Pointer -> Addr: %p, Mode: %d, Parity: %d => Deref: %d (Trapped: %s)\n",
           (void*)sp.raw_address, sp.radix_mode, sp.parity_tag, *read_ptr, trapped ? "YES" : "NO");
    assert(!trapped && *read_ptr == 1337);

    SuperPtr tampered = sp;
    tampered.raw_address ^= 0x10; /* Injected bit-flip */
    void *abort_ptr = super_ptr_deref(&tampered, &trapped);
    printf("  • Rowhammer Exploit Injected -> Trapped: %s (Pointer Aborted: %s)\n\n",
           trapped ? "YES [ATTACK BLOCKED]" : "NO", abort_ptr == NULL ? "YES" : "NO");
    assert(trapped && abort_ptr == NULL);

    /* 4. HyperString Holographic Zoom */
    printf("[4] HyperString (3-Level Holographic Zoomable Text):\n");
    HyperString hs = hyper_string_make(
        "REVROS: KNOT SYNCHRONIZED",
        "The RevRos node completed an autonomous state transition via Sntnl.",
        "Buffer seq=2, magic=0x52050001, Clifford seed=0x6996966969969669, rest mass=24 amu, gear teeth=16."
    );
    printf("  • Zoom Level 0 (Abstract 5-Word) : \"%s\"\n", hyper_string_zoom(&hs, 0));
    printf("  • Zoom Level 1 (Human Narrative) : \"%s\"\n", hyper_string_zoom(&hs, 1));
    printf("  • Zoom Level 2 (Full Telemetry)  : \"%s\"\n\n", hyper_string_zoom(&hs, 2));

    /* 5. HyperEnum Multi-Track Chord */
    printf("[5] HyperEnum (4-Track Lifecycle Enum):\n");
    HyperEnum he_harm = hyper_enum_make(true, true, false, false); /* 1+1+0+0 = 2 (Even) */
    HyperEnum he_diss = hyper_enum_make(true, true, true, false);  /* 1+1+1+0 = 3 (Odd) */
    printf("  • State [1,1,0,0] -> %s\n", hyper_enum_is_harmonic(he_harm) ? "HARMONIC OPERATIONAL" : "DISSONANT RACE");
    printf("  • State [1,1,1,0] -> %s\n\n", hyper_enum_is_harmonic(he_diss) ? "HARMONIC OPERATIONAL" : "DISSONANT RACE");
    assert(hyper_enum_is_harmonic(he_harm));
    assert(!hyper_enum_is_harmonic(he_diss));

    /* 6. HyperNull Causality */
    printf("[6] HyperNull (Explanatory Typed Void):\n");
    HyperNull hn1 = hyper_null_make(HYPERNULL_PERMISSION, "Sandbox SELinux policy disallows raw socket creation");
    HyperNull hn2 = hyper_null_make(HYPERNULL_TIMEOUT, "Peer Sntnl node did not acknowledge within 1 breath interval");
    printf("  • Void 1 -> Code: 0x%X [%s]\n", hn1.null_code, hn1.diagnostic);
    printf("  • Void 2 -> Code: 0x%X [%s]\n\n", hn2.null_code, hn2.diagnostic);

    /* 7. MercenaryArray Heterogeneous Rootbed */
    printf("[7] MercenaryArray (Heterogeneous Collection with HNSW Routing):\n");
    MercenaryArray ma;
    mercenary_array_init(&ma);
    mercenary_array_enlist(&ma, "Collider_Box", MERCENARY_COLLIDER,     0xD6A1);
    mercenary_array_enlist(&ma, "Synth_Voice",  MERCENARY_SYNTH_VOICE,  0x5A5A);
    mercenary_array_enlist(&ma, "Neural_Weight",MERCENARY_NEURAL_WEIGHT,0x295E);
    mercenary_array_enlist(&ma, "DB_Record_01", MERCENARY_DB_RECORD,    0xD6A0);

    int match = mercenary_find_closest(&ma, 0xD6A2);
    printf("  • Query 0xD6A2 -> Closest Free-Agent: [%d] '%s' (Hex: 0x%04X, Popcount Dist: %d)\n\n",
           match, ma.members[match].member_id, ma.members[match].harness_hex,
           __builtin_popcount(ma.members[match].harness_hex ^ 0xD6A2));
    assert(match == 3); /* 0xD6A0 is Hamming distance 1 (0xD6A2 ^ 0xD6A0 = 0x0002) */

    /* 8. Floating-Point Horizons: SBFP & HBFP */
    printf("[8] Floating-Point Horizons (SBFP & HBFP):\n");
    SBFP_Column col_int  = { .header_horizon = 1, .depth_digits = {1, 0, 1, 1} }; /* 8+2+1 = 11.0 */
    SBFP_Column col_frac = { .header_horizon = 0, .depth_digits = {1, 0, 1, 1} }; /* 0.5+0.125+0.0625 = 0.6875 */
    printf("  • SBFP Integral Mode   (H=1): [1,0,1,1] => %.4f\n", sbfp_evaluate(&col_int));
    printf("  • SBFP Fractional Mode (H=0): [1,0,1,1] => %.4f\n", sbfp_evaluate(&col_frac));
    assert(fabsf(sbfp_evaluate(&col_int) - 11.0f) < 1e-4f);
    assert(fabsf(sbfp_evaluate(&col_frac) - 0.6875f) < 1e-4f);

    uint8_t vals[4] = {3, 1, 8, 4};
    uint8_t exps[4] = {8, 9, 7, 8}; /* 3*16^0 + 1*16^1 + 8*16^-1 + 4*16^0 = 3 + 16 + 0.5 + 4 = 23.5 */
    HBFP hbfp = hbfp_make(vals, exps);
    double hbfp_res = hbfp_evaluate(&hbfp);
    printf("  • HBFP Base-16 Evaluation: => %.4f (Expected 23.5000)\n\n", hbfp_res);
    assert(fabs(hbfp_res - 23.5) < 1e-4);

    printf("===================================================================\n");
    printf("  [+] ALL SOVEREIGN PRIMITIVES FULLY VALIDATED & 100%% PASSING!   \n");
    printf("===================================================================\n");
    return 0;
}
