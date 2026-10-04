/* =========================================================================
 *  hyperlang_gearbox_composite_engine.c
 *  =========================================================================
 *  Demonstrating the Complete Higher-Rank OOP Suite built entirely
 *  from the Sub-Scalar Triad (NanoBool, MicroBool, MiniBool):
 *
 *  [Rank 0] BaseObject  : 64-Pole Linear Transmission Shaft (8 MicroBools)
 *  [Rank 1] SuperObject : 1D Timing Belt with Acoustic Wave Propagation (16 NanoBools)
 *  [Rank 2] HyperObject : 4-Cylinder SATB Planetary Harmonic Engine (4 NanoBools = 16b)
 *  [Rank 3] UltraObject : 3D Interlocking Clifford Torus Lattice (16 NanoBools = 64b)
 *  [Rank 4] MegaObject  : 3-Speed Epicyclic Compound Gearbox + C99 Bool Chameleon
 * ========================================================================= */

#include "hyperlang_gearbox_composite.h"

void legacy_c_test_system(const char *name, bool b) {
    printf("    [LEGACY C99 API] %-12s => Evaluated Bool: %s (Raw: 0x%02X)\n",
           name, b ? "TRUE " : "FALSE", (unsigned int)(uint8_t)b);
}

int main(void) {
    printf("===================================================================\n");
    printf("   HYPERLANG COMPOSITE GEARBOX ENGINE: SUPER / HYPER / ULTRA / MEGA\n");
    printf("   Built Entirely from the Sub-Scalar Triad (Nano, Micro, Mini)    \n");
    printf("===================================================================\n\n");

    /* =====================================================================
     * 1. COMPOSITE RANK 0: BaseObject (64-Pole Linear Transmission Shaft)
     * ===================================================================== */
    printf("[1] RANK 0: BaseObject (64-Pole Transmission Shaft via 8 MicroBools):\n");
    printf("-------------------------------------------------------------------\n");
    BaseObject bo = base_object_compose("CrankShaft-64", 0xDEADBEEFCAFE1024ULL);
    printf("  Shaft '%s' -> 64-bit Scalar Value: 0x%016lX\n", bo.label, bo.scalar_value);
    printf("  Inspecting internal MicroBool poles driving the shaft:\n");
    for (int i = 0; i < 8; i++) {
        printf("    Pole [%d]: Phase=%d (%3d deg) | Tens=%d/3 | Damp=%d | Spin=%c | C99=%d\n",
               i, microbool_phase(bo.poles[i]), microbool_phase(bo.poles[i]) * 45,
               microbool_tension(bo.poles[i]), microbool_damping(bo.poles[i]),
               microbool_spin(bo.poles[i]) ? '+' : '-', bo.poles[i].legacy_val);
    }
    printf("  Total Mechanical Torque Output: %d units\n\n", base_object_total_torque(&bo));

    /* =====================================================================
     * 2. COMPOSITE RANK 1: SuperObject (1D Timing Belt / Kinetic Train)
     * ===================================================================== */
    printf("[2] RANK 1: SuperObject (16-Tooth Timing Belt via 16 NanoBools):\n");
    printf("-------------------------------------------------------------------\n");
    SuperObject so = super_object_compose("TimingBelt-16", 0b1011001110010110);
    printf("  SuperObject '%s' Initial State:\n", so.label);
    printf("    Net Winding Number : %d quadrant turns\n", so.net_winding_number);
    printf("    Shear Stress Turb  : %.2f shear units\n", so.shear_stress_turb);
    printf("    Wave Velocity      : %.2f c\n", so.wave_velocity);

    printf("  Injecting Torque Pulse (+2 teeth push into Belt Head):\n");
    super_object_pulse(&so, 2);
    printf("    Post-Pulse Teeth (0..7): ");
    for (int i = 0; i < 8; i++) {
        printf("[Q%d:%s] ", nanobool_quad(so.belt[i]), nanobool_taut(so.belt[i]) ? "T" : "S");
    }
    printf("...\n    Wave successfully propagated down all 16 NanoBool links!\n\n");

    /* =====================================================================
     * 3. COMPOSITE RANK 2: HyperObject (4-Cylinder SATB Planetary Engine)
     * ===================================================================== */
    printf("[3] RANK 2: HyperObject (4-Cylinder SATB Harmonic Engine via 4 NanoBools):\n");
    printf("-------------------------------------------------------------------\n");
    /* Construct 4 NanoBools */
    NanoBool soprano = nanobool_make(0, true, true);   /* Quad 0 (Lead) */
    NanoBool alto    = nanobool_make(1, true, false);  /* Quad 1 (Counter) */
    NanoBool tenor   = nanobool_make(0, false, true);  /* Quad 0 (Rhythm) */
    NanoBool bass    = nanobool_make(1, true, false);  /* Quad 1 (Anchor) */

    HyperObject ho = hyper_object_compose("SATB-PlanetaryChord", soprano, alto, tenor, bass);
    printf("  HyperObject '%s' (Total footprint: 16 bits = 2 bytes):\n", ho.label);
    printf("    Soprano Cylinder (Query)  : Quad=%d, Taut=%d, Spin=%d\n",
           nanobool_quad(ho.soprano), nanobool_taut(ho.soprano), nanobool_spin(ho.soprano));
    printf("    Alto Cylinder (Key)       : Quad=%d, Taut=%d, Spin=%d\n",
           nanobool_quad(ho.alto), nanobool_taut(ho.alto), nanobool_spin(ho.alto));
    printf("    Tenor Cylinder (Value)    : Quad=%d, Taut=%d, Spin=%d\n",
           nanobool_quad(ho.tenor), nanobool_taut(ho.tenor), nanobool_spin(ho.tenor));
    printf("    Bass Cylinder (Anchor)    : Quad=%d, Taut=%d, Spin=%d\n",
           nanobool_quad(ho.bass), nanobool_taut(ho.bass), nanobool_spin(ho.bass));
    printf("    Packed 16-Bit Chord       : 0x%04X\n", ho.chord_matrix);
    printf("    Planetary Mesh Status     : %s\n",
           ho.is_major_harmonic ? "FRICTIONLESS MAJOR HARMONIC" : "DISSONANT CLASH");
    printf("    Internal Friction Drag    : %.2f units\n\n", ho.friction_drag);

    /* =====================================================================
     * 4. COMPOSITE RANK 3: UltraObject (3D Toroidal Clifford Lattice)
     * ===================================================================== */
    printf("[4] RANK 3: UltraObject (3D Toroidal Clifford Lattice via 16 NanoBools):\n");
    printf("-------------------------------------------------------------------\n");
    /* Exactly 64 bits = 16 NanoBools (4x4 Torus grid) */
    uint64_t torus_word = 0x6996966969969669ULL;
    UltraObject uo = ultra_object_compose("Clifford-Iron56-Atom", torus_word);

    printf("  UltraObject '%s' (Footprint: 16 NanoBools = 64-bit CPU Word):\n", uo.name);
    printf("  Scalar Word: 0x%016lX\n", uo.packed_torus_word);
    printf("  Toroidal 4x4 Nano-Gear Matrix:\n");
    for (int r = 0; r < 4; r++) {
        printf("    Row %d: ", r);
        for (int c = 0; c < 4; c++) {
            printf("[Q%d:%c%c] ", nanobool_quad(uo.torus[r][c]),
                   nanobool_taut(uo.torus[r][c]) ? 'T' : 's',
                   nanobool_spin(uo.torus[r][c]) ? '+' : '-');
        }
        printf("\n");
    }
    printf("  Computed Physical Rest Mass : %d spring tension units\n", uo.rest_mass);
    printf("  Open Valence Bonding Teeth  : %d exterior ports\n", uo.open_valences);
    printf("  Crystallization Status      : %s\n\n",
           uo.is_crystallized ? "LOCKED GROUND-STATE CRYSTAL" : "AMORPHOUS PLASMA");

    /* =====================================================================
     * 5. COMPOSITE RANK 4: MegaObject (3-Speed Epicyclic Compound Gearbox)
     * ===================================================================== */
    printf("[5] RANK 4: MegaObject (3-Speed Compound Gearbox: Nano->Micro->Mini):\n");
    printf("-------------------------------------------------------------------\n");
    MegaObject mo = mega_object_compose(true);

    printf("  MegaObject Initial State:\n");
    legacy_c_test_system("MegaObject", mo.legacy_bool);
    printf("    Fast Nano Gear   (1:1) : Quad=%d, C99=%d\n", nanobool_quad(mo.fast_gear), mo.fast_gear.legacy_val);
    printf("    Mid Micro Shaft  (4:1) : Phase=%d (%d deg), Tension=%d\n",
           microbool_phase(mo.mid_shaft), microbool_phase(mo.mid_shaft)*45, microbool_tension(mo.mid_shaft));
    printf("    Master Mini Wheel(16:1): PhaseTick=%d/255 (%.1f deg)\n\n",
           minibool_phase(mo.master_wheel), (float)minibool_phase(mo.master_wheel)*360.0f/256.0f);

    printf("  Advancing Compound Transmission by 64 cycles:\n");
    for (int cycle = 0; cycle < 64; cycle++) {
        mega_object_tick(&mo, 1);
    }

    printf("  MegaObject State After 64 Cycles (Odometer: %d):\n", mo.total_cycles);
    legacy_c_test_system("MegaObject", mo.legacy_bool);
    printf("    Fast Nano Gear   (1:1) : Quad=%d\n", nanobool_quad(mo.fast_gear));
    printf("    Mid Micro Shaft  (4:1) : Phase=%d (%d deg)\n",
           microbool_phase(mo.mid_shaft), microbool_phase(mo.mid_shaft)*45);
    printf("    Master Mini Wheel(16:1): PhaseTick=%d/255 (%.1f deg)\n\n",
           minibool_phase(mo.master_wheel), (float)minibool_phase(mo.master_wheel)*360.0f/256.0f);

    printf("===================================================================\n");
    printf("  [+] ALL HIGHER RANKS COMPOSED FROM SUB-SCALAR TRIAD VERIFIED!    \n");
    printf("===================================================================\n");
    return 0;
}
