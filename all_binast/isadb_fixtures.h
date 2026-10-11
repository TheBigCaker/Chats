/* isadb_fixtures.h -- ground truth: (form, operands, expected bytes).
 *
 * The self-check in isadb.c proves a row round-trips through MY encoder, which is
 * not the same as proving it matches the ISA. These fixtures are the missing check:
 * known-good encodings, so a wrong base word or a wrong field position fails loudly
 * instead of quietly agreeing with itself.
 *
 * Every byte string below is the real encoding of the stated instruction, taken
 * from an independent assembler and checked by hand against the field layout. Keep
 * the operands in the form the catalog takes -- branch immediates are BYTE offsets.
 */
#ifndef ISADB_FIXTURES_H
#define ISADB_FIXTURES_H

typedef struct {
    const char *isa;
    const char *form;
    long        ops[4];
    int         nops;
    uint8_t     bytes[16];
    int         len;
    const char *text;      /* the instruction this must equal, for the failure message */
} Fix;

static const Fix fixtures[] = {
/* ---- AArch64 ---- */
{ "aarch64", "ADD_X_reg",     {8,9,10},        3, {0x28,0x01,0x0a,0x8b}, 4, "add x8, x9, x10" },
{ "aarch64", "ADD_W_reg",     {8,9,10},        3, {0x28,0x01,0x0a,0x0b}, 4, "add w8, w9, w10" },
{ "aarch64", "SUB_X_reg",     {4,5,6},         3, {0xa4,0x00,0x06,0xcb}, 4, "sub x4, x5, x6" },
{ "aarch64", "MUL_X",         {8,9,10},        3, {0x28,0x7d,0x0a,0x9b}, 4, "mul x8, x9, x10" },
{ "aarch64", "UDIV_X",        {8,9,10},        3, {0x28,0x09,0xca,0x9a}, 4, "udiv x8, x9, x10" },
{ "aarch64", "ADD_X_imm",     {8,9,100},       3, {0x28,0x91,0x01,0x91}, 4, "add x8, x9, #100" },
{ "aarch64", "SUB_X_imm",     {8,9,16},        3, {0x28,0x41,0x00,0xd1}, 4, "sub x8, x9, #16" },
{ "aarch64", "SUBS_X_imm",    {8,9,1},         3, {0x28,0x05,0x00,0xf1}, 4, "subs x8, x9, #1" },
{ "aarch64", "MOVZ_X",        {8,4660,0},      3, {0x88,0x46,0x82,0xd2}, 4, "movz x8, #4660" },
{ "aarch64", "MOVZ_W",        {0,1,0},         3, {0x20,0x00,0x80,0x52}, 4, "movz w0, #1" },
{ "aarch64", "MOVK_X",        {0,0x1234,16},   3, {0x80,0x46,0xa2,0xf2}, 4, "movk x0, #0x1234, lsl 16" },
{ "aarch64", "AND_X_imm",     {8,9,255},       3, {0x28,0x1d,0x40,0x92}, 4, "and x8, x9, #255" },
{ "aarch64", "ORR_X_imm",     {8,9,255},       3, {0x28,0x1d,0x40,0xb2}, 4, "orr x8, x9, #255" },
{ "aarch64", "EOR_X_imm",     {8,9,255},       3, {0x28,0x1d,0x40,0xd2}, 4, "eor x8, x9, #255" },
{ "aarch64", "AND_W_imm",     {8,9,0x0f0f0f0f}, 3, {0x28,0xcd,0x00,0x12}, 4, "and w8, w9, #0xf0f0f0f" },
{ "aarch64", "LDR_X_ui",      {0,1,16},        3, {0x20,0x08,0x40,0xf9}, 4, "ldr x0, [x1, #16]" },
{ "aarch64", "STR_X_ui",      {0,1,16},        3, {0x20,0x08,0x00,0xf9}, 4, "str x0, [x1, #16]" },
{ "aarch64", "LDR_W_ui",      {0,1,4},         3, {0x20,0x04,0x40,0xb9}, 4, "ldr w0, [x1, #4]" },
{ "aarch64", "LDRB_W_ui",     {0,1,1},         3, {0x20,0x04,0x40,0x39}, 4, "ldrb w0, [x1, #1]" },
{ "aarch64", "LDUR_X",        {0,1,-8},        3, {0x20,0x80,0x5f,0xf8}, 4, "ldur x0, [x1, #-8]" },
{ "aarch64", "STP_X_pre",     {29,30,31,-16},  4, {0xfd,0x7b,0xbf,0xa9}, 4, "stp x29, x30, [sp, #-16]!" },
{ "aarch64", "LDP_X_post",    {29,30,31,16},   4, {0xfd,0x7b,0xc1,0xa8}, 4, "ldp x29, x30, [sp], #16" },
{ "aarch64", "STP_X_off",     {0,1,2,16},      4, {0x40,0x04,0x01,0xa9}, 4, "stp x0, x1, [x2, #16]" },
{ "aarch64", "B_imm",         {256},           1, {0x40,0x00,0x00,0x14}, 4, "b #256" },
{ "aarch64", "BL_imm",        {16},            1, {0x04,0x00,0x00,0x94}, 4, "bl #16" },
{ "aarch64", "CBZ_X",         {0,8},           2, {0x40,0x00,0x00,0xb4}, 4, "cbz x0, #8" },
{ "aarch64", "CBNZ_X",        {1,8},           2, {0x41,0x00,0x00,0xb5}, 4, "cbnz x1, #8" },
{ "aarch64", "CSEL_X",        {8,9,10,0},      4, {0x28,0x01,0x8a,0x9a}, 4, "csel x8, x9, x10, eq" },
{ "aarch64", "CSEL_W",        {8,9,10,1},      4, {0x28,0x11,0x8a,0x1a}, 4, "csel w8, w9, w10, ne" },
{ "aarch64", "RET_X30",       {},              0, {0xc0,0x03,0x5f,0xd6}, 4, "ret" },
{ "aarch64", "NOP",           {},              0, {0x1f,0x20,0x03,0xd5}, 4, "nop" },
{ "aarch64", "SVC_0",         {},              0, {0x01,0x00,0x00,0xd4}, 4, "svc #0" },
{ "aarch64", "FADD_D",        {0,1,2},         3, {0x20,0x28,0x62,0x1e}, 4, "fadd d0, d1, d2" },
{ "aarch64", "FMOV_X_D",      {0,0},           2, {0x00,0x00,0x66,0x9e}, 4, "fmov x0, d0" },
{ "aarch64", "ADR_X",         {0,16},          2, {0x80,0x00,0x00,0x10}, 4, "adr x0, #16" },
{ "aarch64", "TST_X_reg",     {0,1},           2, {0x1f,0x00,0x01,0xea}, 4, "tst x0, x1" },
{ "aarch64", "MOV_X0_X1",     {},              0, {0xe0,0x03,0x01,0xaa}, 4, "mov x0, x1" },
{ "aarch64", "LDR_X_post_X8", {},              0, {0xe8,0x07,0x41,0xf8}, 4, "ldr x8, [sp], #16" },
/* Two-register FP: same shape as the three-register ops but a different opcode
 * field, so they must be pinned separately or a "fix" to one breaks the other. */
{ "aarch64", "FNEG_D",        {0,0},           2, {0x00,0x40,0x61,0x1e}, 4, "fneg d0, d0" },
{ "aarch64", "FNEG_S",        {0,0},           2, {0x00,0x40,0x21,0x1e}, 4, "fneg s0, s0" },
{ "aarch64", "FSQRT_D",       {0,0},           2, {0x00,0xc0,0x61,0x1e}, 4, "fsqrt d0, d0" },
{ "aarch64", "FABS_D",        {0,0},           2, {0x00,0xc0,0x60,0x1e}, 4, "fabs d0, d0" },
{ "aarch64", "FCVT_D_S",      {0,0},           2, {0x00,0x40,0x62,0x1e}, 4, "fcvt d0, s0" },
{ "aarch64", "FCVT_S_D",      {0,0},           2, {0x00,0xc0,0x22,0x1e}, 4, "fcvt s0, d0" },
{ "aarch64", "FADD_D",        {0,0,0},         3, {0x00,0x28,0x60,0x1e}, 4, "fadd d0, d0, d0" },
{ "aarch64", "FADD_S",        {0,0,0},         3, {0x00,0x28,0x20,0x1e}, 4, "fadd s0, s0, s0" },
{ "aarch64", "FMOV_D_D",      {0,2},           2, {0x40,0x40,0x60,0x1e}, 4, "fmov d0, d2" },
{ "aarch64", "FCMP_D",        {0,1},           2, {0x00,0x20,0x61,0x1e}, 4, "fcmp d0, d1" },
{ "aarch64", "FCMPZ_D",       {0},             1, {0x08,0x20,0x60,0x1e}, 4, "fcmp d0, #0.0" },
{ "aarch64", "FRINTA_D",      {0,0},           2, {0x00,0x40,0x66,0x1e}, 4, "frinta d0, d0" },
{ "aarch64", "ADRP_X",        {0,1},           2, {0x00,0x00,0x00,0xb0}, 4, "adrp x0, #4096 (page 1)" },
{ "aarch64", "LDXR_W",        {1,0},           2, {0x01,0x7c,0x5f,0x88}, 4, "ldxr w1, [x0]" },
{ "aarch64", "LDXR_X",        {1,0},           2, {0x01,0x7c,0x5f,0xc8}, 4, "ldxr x1, [x0]" },
{ "aarch64", "LDAR_W",        {1,0},           2, {0x01,0xfc,0xdf,0x88}, 4, "ldar w1, [x0]" },
{ "aarch64", "STLR_W",        {1,0},           2, {0x01,0xfc,0x9f,0x88}, 4, "stlr w1, [x0]" },
{ "aarch64", "STXR_W",        {2,1,0},         3, {0x01,0x7c,0x02,0x88}, 4, "stxr w2, w1, [x0]" },
{ "aarch64", "ADD_X_reg_lsl", {0,1,2,3},       4, {0x20,0x0c,0x02,0x8b}, 4, "add x0, x1, x2, lsl #3" },
{ "aarch64", "ADD_X_ext_uxtw",{0,1,2,0},       4, {0x20,0x40,0x22,0x8b}, 4, "add x0, x1, w2, uxtw" },
{ "aarch64", "LDR_X_ro_s3",   {0,1,2},         3, {0x20,0x78,0x62,0xf8}, 4, "ldr x0, [x1, x2, lsl #3]" },
{ "aarch64", "ADD_W_reg_lsl", {0,0,0,1},       4, {0x00,0x04,0x00,0x0b}, 4, "add w0, w0, w0, lsl #1" },
{ "aarch64", "ADD_W_reg_lsr", {0,0,0,1},       4, {0x00,0x04,0x40,0x0b}, 4, "add w0, w0, w0, lsr #1" },
{ "aarch64", "SUB_W_reg_lsl", {0,0,0,1},       4, {0x00,0x04,0x00,0x4b}, 4, "sub w0, w0, w0, lsl #1" },
/* Corpus-driven scalar additions: every one of these was measured missing on a real
 * production binary before it was added, and its bytes come from an assembler. */
{ "aarch64", "TBZ_X",      {0,3,16},        3, {0x80,0x00,0x18,0x36}, 4, "tbz x0, #3, #16" },
{ "aarch64", "TBNZ_X",     {1,5,16},        3, {0x81,0x00,0x28,0x37}, 4, "tbnz x1, #5, #16" },
{ "aarch64", "MRS",        {0,0x5a10},      2, {0x00,0x42,0x3b,0xd5}, 4, "mrs x0, nzcv" },
{ "aarch64", "UMULL_X",    {0,1,2},         3, {0x20,0x7c,0xa2,0x9b}, 4, "umull x0, w1, w2" },
{ "aarch64", "UMADDL_X",   {0,1,2,3},       4, {0x20,0x0c,0xa2,0x9b}, 4, "umaddl x0, w1, w2, x3" },
{ "aarch64", "UMULH_X",    {0,1,2},         3, {0x20,0x7c,0xc2,0x9b}, 4, "umulh x0, x1, x2" },
{ "aarch64", "CCMP_X",     {0,1,0,0},       4, {0x00,0x00,0x41,0xfa}, 4, "ccmp x0, x1, #0, eq" },
{ "aarch64", "CSEL_X",     {0,1,2,0},       4, {0x20,0x00,0x82,0x9a}, 4, "csel x0, x1, x2, eq" },
{ "aarch64", "CSETM_X",    {0,1},           2, {0xe0,0x13,0x9f,0xda}, 4, "csetm x0, eq" },
{ "aarch64", "LSR_X",      {0,1,3},         3, {0x20,0xfc,0x43,0xd3}, 4, "lsr x0, x1, #3" },
{ "aarch64", "LSL_X",      {0,1,3},         3, {0x20,0xf0,0x7d,0xd3}, 4, "lsl x0, x1, #3" },
{ "aarch64", "ASR_X",      {0,1,3},         3, {0x20,0xfc,0x43,0x93}, 4, "asr x0, x1, #3" },
{ "aarch64", "FCSEL_D",    {0,1,2,0},       4, {0x20,0x0c,0x62,0x1e}, 4, "fcsel d0, d1, d2, eq" },
{ "aarch64", "LDRSH_X_ui", {0,1,2},         3, {0x20,0x04,0x80,0x79}, 4, "ldrsh x0, [x1, #2]" },
{ "aarch64", "LDRSB_X_ui", {0,1,1},         3, {0x20,0x04,0x80,0x39}, 4, "ldrsb x0, [x1, #1]" },
{ "aarch64", "LDURSW_X",   {0,1,-4},        3, {0x20,0xc0,0x9f,0xb8}, 4, "ldursw x0, [x1, #-4]" },
{ "aarch64", "MVN_X",      {0,1},           2, {0xe0,0x03,0x21,0xaa}, 4, "mvn x0, x1" },
{ "aarch64", "UCVTF_D_X",  {0,1},           2, {0x20,0x00,0x63,0x9e}, 4, "ucvtf d0, x1" },
{ "aarch64", "FCVTZU_W_D", {0,1},           2, {0x20,0x00,0x79,0x1e}, 4, "fcvtzu w0, d1" },
{ "aarch64", "FCVTAS_X_D", {0,0},           2, {0x00,0x00,0x64,0x9e}, 4, "fcvtas x0, d0" },
{ "aarch64", "HINT",       {5,0},           2, {0xbf,0x20,0x03,0xd5}, 4, "hint #5" },
{ "aarch64", "SMULL_X",    {0,1,2},         3, {0x20,0x7c,0x22,0x9b}, 4, "smull x0, w1, w2" },


/* ---- x86-64 ----
 * THE x86-64 FIXTURES ARE NOT HERE. They live in isadb_fixtures_x86.h, GENERATED from llvm-mc by
 * `x86fixt` (`make fixtures`), because a hand-written list cannot survive the catalog changing
 * underneath it. When the x86-64 catalog stopped being 73 hand-written rows and became the 24,000
 * derived from the local .td map, every fixture in this file named a row that no longer existed --
 * 116 of 116 "failed" while the catalog was correct on all of them. A fixture must be an encoding
 * an independent assembler produced, paired with the row the catalog CURRENTLY chooses for it.
 */
};

#define NFIXTURES (sizeof fixtures / sizeof fixtures[0])

#endif /* ISADB_FIXTURES_H */
