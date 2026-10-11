/* mkx86.c -- DERIVE the x86-64 catalog from real assembled instances, then verify it.
 *
 * WHY A GENERATOR. AArch64 could be hand-written: fixed 4-byte words, uniform field windows.
 * x86-64 cannot be, at the scale real code needs: a byte stream with an optional REX prefix,
 * a ModR/M byte whose meaning depends on the opcode, an optional SIB byte, and displacement
 * and immediate widths that change the instruction LENGTH. Hand-writing thousands of those
 * rows produces a catalog that is 95% right, and a wrong row is worse than a missing one.
 *
 * THE METHOD. Each instruction form is assembled several times with llvm-mc (which encodes
 * x86-64 natively on this device), varying the register operands, and the row is DERIVED
 * from the bytes: bits identical across all instances are the template; a run of bits whose
 * value tracks (operand >> k) is that operand's field, wherever it sits. A register operand
 * is a COMPOSITE -- 3 bits of ModR/M plus a REX bit for the r8-r15 bank -- so the search
 * tries a main run plus one high bit, which is how REX.R and REX.B get FOUND rather than
 * assumed.
 *
 * llvm-mc is used ONLY here, at catalog-build time, as ground truth -- the same role it
 * plays for the AArch64 fixtures. The shipped catalog is plain data and the runtime has no
 * assembler, no subprocess, no external dependency.
 *
 *   mkx86 emit    > ISA_X86_64.inc    derive and write the catalog
 *   mkx86 check                       per-form: assemblable or refused, with the line
 *   mkx86 probe <form>                the samplings and the bytes they assembled to
 *   mkx86 spec                        every form, one per line
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>

#include "isadb.h"
#include "arbishim.h"
#include "x86batch.h"

/* ONE batch buffer for the whole run, reused per form (see the note where it is used).
 * IT IS AN ARRAY: batch_assemble indexes it per line, and declaring it as a single struct made
 * every entry past the first write over the same bytes -- so a form's samples were all reported
 * as whatever line 0 assembled, the length filter then threw most of them away, and the emit
 * produced a third of the rows it should have. */
static Batch g_batch[XB_MAX];

/* ============================================================ registers ===== */

static const char *RN64[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                               "r8","r9","r10","r11","r12","r13","r14","r15"};
static const char *RN32[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                               "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
static const char *RN16[16] = {"ax","cx","dx","bx","sp","bp","si","di",
                               "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
/* 8-bit registers WITH a REX prefix present: spl/bpl/sil/dil for numbers 4-7. Without REX
 * those slots are ah/ch/dh/bh, which is a different instruction; the generated 8-bit rows
 * always carry a REX byte (0F B6 / 0F 94 etc. emit one for 4-7), so this is the right table. */
static const char *RN8[16]  = {"al","cl","dl","bl","spl","bpl","sil","dil",
                               "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};

static const char *RN(int w, int r) {
    if (r < 0 || r > 15) return "rax";
    switch (w) {
        case 64: return RN64[r];
        case 32: return RN32[r];
        case 16: return RN16[r];
        default: return RN8[r];
    }
}

/* ============================================================ llvm-mc ===== */

/* ---- the assembly cache ----
 * llvm-mc costs ~104ms per call on this device -- measured, and it is process startup, not work:
 * an empty file costs the same as a five-instruction one. A full emit needs several hundred
 * calls, so the cache exists to make a re-emit nearly free.
 *
 * IT IS HASH-INDEXED, NOT LINEARLY SCANNED. The first version compared the query against every
 * entry, and with a few thousand entries that is a strcmp per entry per lookup -- hundreds of
 * thousands of comparisons per emit, on top of the processes. A bucket index over the key makes
 * a hit O(1), which is what turns the second and every later emit into seconds.
 *
 * It is keyed on the exact instruction text, so a hit can only return the bytes llvm-mc produced
 * for that same text -- it cannot mask a wrong encoding. Delete the file to force a re-derive. */
#define CACHEPATH "/data/data/com.termux/files/home/binast-spike/tmp/mkx86_cache.txt"
#define CBUCKETS  65536

typedef struct { char line[128]; uint8_t by[16]; int len; int next; } CacheEnt;
static CacheEnt *g_cache;
static int      *g_bucket;      /* CBUCKETS heads, -1 empty */
static int       g_ncache, g_capcache;
static int       g_cache_dirty;

/* FNV-1a over the key. The key is the instruction text with spaces mapped to '_' so it survives
 * whitespace-delimited parsing of the cache file. */
static unsigned cache_hash(const char *k) {
    unsigned h = 2166136261u;
    for (const char *p = k; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h & (CBUCKETS - 1);
}

/* the cache file stores the key with spaces replaced, so build the key once in one place */
static void cache_key(const char *line, char *k, size_t n) {
    snprintf(k, n, "%s", line);
    for (char *p = k; *p; p++) if (*p == ' ') *p = '_';
}

static void cache_index_put(int i) {
    if (!g_bucket) return;
    unsigned h = cache_hash(g_cache[i].line);
    g_cache[i].next = g_bucket[h];
    g_bucket[h] = i;
}

static void cache_load(void) {
    g_capcache = 8192;
    g_cache  = (CacheEnt *)arb_carve_zeroed((long)g_capcache * (long)sizeof(CacheEnt));
    g_bucket = (int *)arb_carve_zeroed((long)CBUCKETS * (long)sizeof(int));
    if (!g_cache || !g_bucket) { g_capcache = 0; g_cache = NULL; g_bucket = NULL; return; }
    for (int i = 0; i < CBUCKETS; i++) g_bucket[i] = -1;
    FILE *f = fopen(CACHEPATH, "r");
    if (!f) return;
    char l[128], hex[64];
    while (g_ncache < g_capcache && fscanf(f, "%127s %63s", l, hex) == 2) {
        CacheEnt *e = &g_cache[g_ncache];
        snprintf(e->line, sizeof e->line, "%s", l);
        int n = 0;
        for (const char *p = hex; p[0] && p[1] && n < 16; p += 2) {
            unsigned v = 0;
            if (sscanf(p, "%2x", &v) != 1) break;
            e->by[n++] = (uint8_t)v;
        }
        e->len = n;
        if (n > 0) cache_index_put(g_ncache++);
    }
    fclose(f);
}
static int cache_get(const char *line, uint8_t *out, int outmax) {
    if (!g_bucket) return -1;
    char k[128];
    cache_key(line, k, sizeof k);
    unsigned h = cache_hash(k);
    for (int i = g_bucket[h]; i >= 0; i = g_cache[i].next) {
        if (!strcmp(k, g_cache[i].line)) {
            if (g_cache[i].len > outmax) return -1;
            memcpy(out, g_cache[i].by, (size_t)g_cache[i].len);
            return g_cache[i].len;
        }
    }
    return -1;
}
static void cache_put(const char *line, const uint8_t *by, int len) {
    if (!g_bucket || g_ncache >= g_capcache || len <= 0 || len > 16) return;
    CacheEnt *e = &g_cache[g_ncache];
    cache_key(line, e->line, sizeof e->line);
    memcpy(e->by, by, (size_t)len);
    e->len = len;
    cache_index_put(g_ncache++);
    g_cache_dirty = 1;
}
static void cache_save(void) {
    if (!g_cache_dirty) return;
    FILE *f = fopen(CACHEPATH, "w");
    if (!f) return;
    for (int i = 0; i < g_ncache; i++) {
        fprintf(f, "%s ", g_cache[i].line);
        for (int b = 0; b < g_cache[i].len; b++) fprintf(f, "%02x", g_cache[i].by[b]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static int asm_one_uncached(const char *line, uint8_t *out, int outmax);
static int asm_one(const char *line, uint8_t *out, int outmax) {
    int hit = cache_get(line, out, outmax);
    if (hit >= 0) return hit;
    int len = asm_one_uncached(line, out, outmax);
    if (len > 0) cache_put(line, out, len);
    return len;
}

static int asm_one_uncached(const char *line, uint8_t *out, int outmax) {
    /* mkstemp requires XXXXXX to be the LAST characters of the template, so the file it
     * creates has no extension -- and llvm-mc cannot infer the input language from a name
     * like "mkx86AB12CD", so it exits 1 with an EMPTY stderr (that is what system()==256
     * and no diagnostic looked like). The fd is created by mkstemp for safety, then the
     * same stem plus ".s" is what llvm-mc is handed. */
    char base[128], src[144], obj[160], cmd[512];
    snprintf(base, sizeof base, "%s", "/data/data/com.termux/files/home/binast-spike/tmp/mkx86XXXXXX");
    int fd = mkstemp(base);
    if (fd < 0) return -1;
    close(fd);
    snprintf(src, sizeof src, "%s.s", base);
    snprintf(obj, sizeof obj, "%s.o", base);

    FILE *f = fopen(src, "w");
    if (!f) return -1;
    fprintf(f, ".text\n%s\n", line);
    fclose(f);

    snprintf(cmd, sizeof cmd,
             "llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o %s %s 2>/dev/null",
             obj, src);
    if (system(cmd) != 0) { remove(src); remove(obj); return -1; }

    FILE *o = fopen(obj, "rb");
    if (!o) { remove(src); remove(obj); return -1; }
    fseek(o, 0, SEEK_END);
    long n = ftell(o);
    rewind(o);
    uint8_t *buf = (uint8_t *)arb_bytes((long)n);
    if (!buf || fread(buf, 1, (size_t)n, o) != (size_t)n) {
        fclose(o); remove(src); remove(obj); return -1;
    }
    fclose(o);

    int len = -1;
    if (n > 64 && !memcmp(buf, "\x7f""ELF", 4)) {
        uint64_t shoff = 0; for (int i = 7; i >= 0; i--) shoff = (shoff << 8) | buf[0x28 + i];
        uint16_t shent = (uint16_t)(buf[0x3a] | (buf[0x3b] << 8));
        uint16_t shn   = (uint16_t)(buf[0x3c] | (buf[0x3d] << 8));
        for (uint16_t i = 0; i < shn; i++) {
            uint64_t o2 = shoff + (uint64_t)i * shent;
            if (o2 + 64 > (uint64_t)n) break;
            uint64_t fl = 0; for (int k = 7; k >= 0; k--) fl = (fl << 8) | buf[o2 + 0x8 + k];
            uint64_t fo = 0; for (int k = 7; k >= 0; k--) fo = (fo << 8) | buf[o2 + 0x18 + k];
            uint64_t sz = 0; for (int k = 7; k >= 0; k--) sz = (sz << 8) | buf[o2 + 0x20 + k];
            if (!(fl & 0x4) || !sz || fo + sz > (uint64_t)n) continue;
            if ((int)sz > outmax) break;
            memcpy(out, buf + fo, (size_t)sz);
            len = (int)sz;
            break;
        }
    }
    arbit_release_carve((long)buf);
    remove(src); remove(obj);
    return len;
}

/* ============================================================ forms ===== */

/* an operand slot in a form: a register of some width, or a literal immediate */
enum { RX_R8 = 1, RX_R16, RX_R32, RX_R64, RX_IMM, RX_IMM8, RX_IMM64,
       /* A MEMORY OPERAND IS TWO ROW OPERANDS, not a new kind.
        *
        * Real compiled x86-64 is mostly loads and stores: ModR/M with mod != 11 plus a
        * displacement. The catalog had none, so it decoded 12% of clang's own output. The
        * encoding has THREE variable pieces -- the base register's 3 bits, its REX.B, and the
        * displacement -- and a row's operand slot carries one main window plus one extra bit.
        *
        * So the displacement becomes its own operand: AT&T writes `4(%rdi)`, and with the
        * displacement substituted first the text is "%s(REG%s)" -- a number, then a parenthesised
        * register. The runtime already has K_IMM for the displacement and K_REXREG for the base,
        * so nothing new is needed at run time; only the sampler and the spec text change.
        *
        * RX_MEMBASE is a base register for a memory operand: it must never be rsp or rbp (4 and
        * 5), because in mod=00 those encodings mean "SIB follows" and "disp32 follows" rather
        * than a plain base. Sampling 4 or 5 mixes three different encodings into one form. */
       RX_MEMBASE,   /* a ModR/M r/m base. rsp is IMPOSSIBLE here: rm=100 means "a SIB byte
                      * follows", so "%rsp" as a plain base is a different encoding. rbp is fine
                      * whenever a displacement is present (mod=01/10 rm=101 is rbp+disp), and
                      * every memory form here has one. */
       RX_SIBBASE,   /* a SIB base. EVERY register is legal, rsp and rbp included -- the SIB
                      * byte has its own 3-bit base field with no "escapes" in it. Using
                      * RX_MEMBASE here would have excluded rsp from a position that allows it. */
       RX_DISP8,     /* a signed byte displacement */
       RX_DISP32,    /* a signed doubleword displacement */
       /* A SIB OPERAND. `(%rdi,%rdx,4)` is ModR/M rm=100 + a SIB byte: scale, index, base.
        * All three are variable, and one row operand slot carries one main window plus one extra
        * bit, so the scale gets its own operand -- it is exactly two bits at a fixed position,
        * which is the K_IMM shape. Sampling fixes index and base and varies only the scale. */
       RX_SIBIDX,    /* an index register, rsp(4) excluded (rm=100 means "no index") */
       RX_SIBSCALE,  /* the scale field: 1,2,4,8 as 0,1,2,3 */
       /* A DIRECT BRANCH TARGET: rel8 or rel32, measured from the END of the instruction. It is
        * an immediate in everything but meaning, so it is a K_IMM field and the solver finds it
        * by watching the value track the sample's target. */
       RX_REL8,
       RX_REL32,
       /* A ONE-BYTE-OPCODE REGISTER, e.g. incq %rdx = 48 ff c2. Here the register IS the low
        * three bits of the only opcode byte, so this operand's field is inside the opcode and
        * no ModR/M byte exists at all. */
       RX_OPREG };

typedef struct { int kind; int val; } Op;

typedef struct {
    char *row, *mnem, *ops, *fmt;   /* owned copies (see spec_add) */
    int   nop;
    Op    opsv[6];   /* a SIB form has FIVE (disp, base, index, scale, reg) and the
                       * sys/system forms have six. Four was the old size and it made every
                       * SIB emit corrupt the stack: emit_row indexes m[i]/h[i]/got[i] per
                       * operand, so operand 4 wrote past the end of a 4-element array and the
                       * generator died with SIGSEGV (rc=139) after emitting 617 rows. */
} Spec;

typedef struct {
    int     n;
    uint8_t by[12][10];
    int     len[12];
    long long vals[6][12];   /* 64-bit: a movabs immediate does not fit an int */
    int     seen_high;   /* did any accepted sample use a register >= 8? A full-range sample
                          * set without one has silently lost its extended half, and the
                          * resulting row cannot encode any r8-r15 operand. */
    int     has_reg;     /* does this form have ANY register operand? "ret" and "nop" do not,
                          * so no sample can be "high" and the split check must not apply --
                          * it refused them outright with 8 valid samples in hand. */
} Samples;

/* ============================================================ derivation ===== */

typedef struct { int byte, bit, wid; } Run;

static int run_val(const Samples *S, int s, Run r) {
    int v = 0;
    for (int b = 0; b < r.wid; b++) {
        int abs = r.byte * 8 + r.bit + b;
        if ((S->by[s][abs / 8] >> (abs % 8)) & 1) v |= 1 << b;
    }
    return v;
}

/* Solve operand `op`: a main run holding the low bits, plus (for a register above 7) one
 * high bit somewhere else. Returns 1 and fills main/hi. */
/* THE WIDTH COMES FROM THE KIND, NOT FROM A SEARCH.
 *
 * The first solver tried widths ascending and returned the shortest run that matched every
 * sample. For a register that is right (3 bits). For an immediate it is WRONG: an 8-bit
 * immediate's low 3 bits are just bits of the value, so a 3-bit run matches all 8 samplings,
 * the search stops there, and the row is under-specified -- it refuses every value with a bit
 * above bit 2 set. That is why "sarq $2, %r8" decoded while "sarq $12, %r8" did not, and why
 * the rows showed "imm2"/"imm3" instead of "imm8".
 *
 * The kind already states how wide the operand is, so the width is not a question to search:
 * a register is 3 bits plus an optional REX bit, an imm8 is 8 bits, a wide immediate is 32.
 * Only the POSITION has to be found.
 */
static int width_of_kind(int kind) {
    switch (kind) {
        case RX_R8: case RX_R16: case RX_R32: case RX_R64: return 3;
        case RX_IMM8:  return 8;
        case RX_IMM:   return 32;
        /* A 64-bit immediate cannot be one run in a row whose other fields need their own
         * windows, so it is the runtime's K_SPLIT shape: 32 bits here, 32 more in the split
         * window. isa_extract/isa_encode already implement exactly that. */
        case RX_IMM64: return 32;
        /* A MEMORY DISPLACEMENT'S WIDTH IS FIXED BY THE MOD FIELD, so it is known exactly as
         * for an immediate: mod=01 is a signed byte, mod=10 a signed doubleword. Omitting these
         * made width_of_kind return 0, the solver bail, and every memory form be dropped --
         * 244 of them. */
        case RX_DISP8:  return 8;
        case RX_DISP32: return 32;
        case RX_REL8:   return 8;
        case RX_REL32:  return 32;
        case RX_SIBSCALE: return 2;
        /* a base register is 3 ModR/M (or SIB) bits plus the operand's REX bit, exactly like any
         * other register field; a SIB index is 3 bits plus REX.X; the one-byte-opcode register is
         * 3 bits plus REX.B. */
        case RX_MEMBASE: case RX_SIBBASE: case RX_SIBIDX: case RX_OPREG: return 3;
        default: return 0;
    }
}

static int solve_op(const Spec *sp, const Samples *S, int op, Run *main_out, Run *hi_out, int *use_hi) {
    int len = S->len[0];
    for (int s = 1; s < S->n; s++) if (S->len[s] != len) return 0;
    *use_hi = 0;

    int wid = width_of_kind(sp->opsv[op].kind);
    if (wid <= 0) return 0;
    unsigned mask = (wid >= 32) ? 0xFFFFFFFFu : ((1u << wid) - 1u);

    /* does the operand vary at all? a constant operand is folded into the template */
    int varies = 0;
    for (int s = 1; s < S->n; s++)
        if (S->vals[op][s] != S->vals[op][0]) varies = 1;
    if (!varies) {
        /* constant across samplings: it belongs to the base template. Report no field so the
         * caller knows it needs no window -- but still return success, because the operand is
         * accounted for. */
        main_out->byte = 0; main_out->bit = 0; main_out->wid = 0;
        return 1;
    }

    /* an 8- or 32-bit immediate sits at a byte boundary in every encoding this generator
     * produces, so try whole-byte-aligned positions first, then any position */
    for (int pass = 0; pass < 2; pass++) {
        for (int byte = 0; byte < len; byte++) {
            int bhi = (pass == 0) ? 1 : 8;
            for (int bit = 0; bit < bhi; bit++) {
                if (pass == 0 && bit != 0) continue;
                Run m = {byte, bit, wid};
                /* the run must be inside the instruction */
                if ((bit + wid + 7) / 8 + byte > len) continue;
                int exact_low = 1, hi_constant = 1;
                /* A NON-REGISTER OPERAND HAS NO HIGH BIT, SO ITS HIGH BITS ARE IRRELEVANT.
                 *
                 * `hi_constant` exists for K_REXREG: a register's bit 3 lives in a separate byte,
                 * so the solver must find that window. An immediate, a displacement or a branch
                 * target has no such bit -- only its LOW `wid` bits are in the encoding, and the
                 * comparison against `v & mask` is the whole test.
                 *
                 * Demanding a constant high half for these kinds is why every J<cc>_REL8 row was
                 * dropped: the sample values are +2, -2, +4, -4 ..., so `v >> 8` alternates between
                 * 0 (for the positives) and -1 (for the negatives, sign extension). It is never
                 * constant, exact_low still matched, and the solver fell through with no field --
                 * while `probe` (which only asks whether the samples assemble) reported OK. */
                int needs_hi = 0;
                switch (sp->opsv[op].kind) {
                    case RX_IMM: case RX_IMM8: case RX_IMM64:
                    case RX_DISP8: case RX_DISP32:
                    case RX_REL8: case RX_REL32: case RX_SIBSCALE:
                        needs_hi = 0; break;
                    default:
                        needs_hi = 1; break;
                }
                for (int s = 0; s < S->n; s++) {
                    int v = S->vals[op][s];
                    if (run_val(S, s, m) != (v & (int)mask)) exact_low = 0;
                    if (needs_hi && (v >> wid) != (S->vals[op][0] >> wid)) hi_constant = 0;
                    if (!exact_low && (!needs_hi || !hi_constant)) break;
                }
                if (exact_low && !needs_hi) hi_constant = 1;
                if (!exact_low) {
                    /* NOT a plain low-half match. For a 64-BIT IMMEDIATE that is expected: the
                     * main window holds the HIGH half, so matching it against the operand's low
                     * bits necessarily fails. Try the split before giving up on this position, or
                     * the row is dropped and movabs has no row at all -- which is exactly what
                     * happened while this branch sat after the `continue`. */
                    if (wid == 32 && sp->opsv[op].kind == RX_IMM64) {
                        for (int byte2 = 0; byte2 < len; byte2++) {
                            if (byte2 == byte) continue;
                            if ((32 + 7) / 8 + byte2 > len) continue;
                            Run h2 = {byte2, 0, 32};
                            int ok2 = 1;
                            for (int s = 0; s < S->n; s++) {
                                long long v = (long long)S->vals[op][s];
                                long long got = ((long long)run_val(S, s, m) << 32)
                                              | (long long)run_val(S, s, h2);
                                if (got != v) { ok2 = 0; break; }
                            }
                            if (!ok2) continue;
                            *main_out = m; *hi_out = h2; *use_hi = 2;
                            return 1;
                        }
                    }
                    continue;
                }
                if (hi_constant) {
                    /* The high bit is constant. If it is SET in every sample, this operand is
                     * always in the extended bank, and that set bit must be a FIELD (so the
                     * operand can also be any value whose bit 3 is set) rather than part of the
                     * template -- a template bit cannot be written by an operand, so the row
                     * would refuse every register >= 8. */
                    int hi_on = (S->vals[op][0] >> wid) & 1;
                    if (hi_on) {
                        for (int byte2 = 0; byte2 < len; byte2++) {
                            for (int bit2 = 0; bit2 < 8; bit2++) {
                                if (byte2 == byte && bit2 >= bit && bit2 < bit + wid) continue;
                                int all_set = 1;
                                for (int s = 0; s < S->n; s++)
                                    if (!((S->by[s][byte2] >> bit2) & 1)) { all_set = 0; break; }
                                if (!all_set) continue;
                                Run h = {byte2, bit2, 1};
                                *main_out = m; *hi_out = h; *use_hi = 1;
                                return 1;
                            }
                        }
                    }
                    *main_out = m; return 1;
                }
                /* THE 64-BIT IMMEDIATE: low 32 bits in the main window, high 32 in a second
                 * window (the runtime's K_SPLIT, human = (high << split_wid) | low). Only a
                 * 32-bit high half is considered, because that is the shape isa_encode
                 * implements -- guessing anything else would produce a row the encoder cannot
                 * write back. */
                if (wid == 32 && sp->opsv[op].kind == RX_IMM64) {
                    for (int byte2 = 0; byte2 < len; byte2++) {
                        Run h2 = {byte2, 0, 32};
                        if ((32 + 7) / 8 + byte2 > len) continue;
                        if (byte2 == byte) continue;
                        /* THE RUNTIME'S OWN CONVENTION: isa_extract computes
                         *     human = (enc << split_wid) | split_lo
                         * so the MAIN window (m) holds the HIGH half and the SPLIT window (h2)
                         * holds the low half. My first version had them the other way round and
                         * a decoded movabs came back with its halves transposed -- the low 4
                         * bytes containing the high half of the constant. Read the convention
                         * off the code that consumes the row rather than assuming it. */
                        int ok2 = 1;
                        for (int s = 0; s < S->n; s++) {
                            long long v = (long long)S->vals[op][s];
                            long long got = ((long long)run_val(S, s, m) << 32)
                                          | (long long)run_val(S, s, h2);
                            if (got != v) { ok2 = 0; break; }
                        }
                        if (!ok2) continue;
                        *main_out = m; *hi_out = h2; *use_hi = 2;   /* 2 = split, not REX */
                        return 1;
                    }
                }
                /* the operand needs a REX-style high bit somewhere else */
                for (int byte2 = 0; byte2 < len; byte2++) {
                    for (int bit2 = 0; bit2 < 8; bit2++) {
                        if (byte2 == byte && bit2 >= bit && bit2 < bit + wid) continue;
                        Run h = {byte2, bit2, 1};
                        int ok = 1;
                        for (int s = 0; s < S->n; s++) {
                            int v = run_val(S, s, m) | (run_val(S, s, h) << wid);
                            if (v != S->vals[op][s]) { ok = 0; break; }
                        }
                        if (!ok) continue;
                        *main_out = m; *hi_out = h; *use_hi = 1;
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/* ============================================================ spec table ===== */

static Spec g_spec[512];
static int  g_nspec;

static void spec_add(const char *row, const char *mnem, const char *ops,
                     const char *fmt, int nop, ...) {
    if (g_nspec >= 512) return;
    Spec *sp = &g_spec[g_nspec++];
    memset(sp, 0, sizeof *sp);
    /* COPY the strings: assigning the pointers made every spec alias the caller's stack
     * buffers, so the whole table showed the last iteration's names -- 114 specs that all
     * claimed to be the same cmov. */
    sp->row = strdup(row); sp->mnem = strdup(mnem);
    sp->ops = strdup(ops); sp->fmt = strdup(fmt); sp->nop = nop;
    va_list ap; va_start(ap, nop);
    for (int i = 0; i < nop; i++) sp->opsv[i].kind = va_arg(ap, int);
    va_end(ap);
}

/* fill_asm expands two markers before substituting operands:
 *   REG     -> a literal '%', which AT&T syntax requires before every register name
 *   LITPCT  -> the same, for a format that needs a '%' for another reason
 * Doing it with markers rather than "%%" at the call sites keeps the formats readable and
 * avoids the double-expansion that produced "%%rax". */
static void fill_asm(const Spec *sp, const long long *vals, char *out, size_t n) {
    char buf[256], tmp[256];
    snprintf(tmp, sizeof tmp, "%s", sp->fmt);
    { char *d = buf, *q = tmp;
      while (*q && (size_t)(d - buf) < sizeof buf - 8) {
          if (!strncmp(q, "LITPCT", 6)) { *d++ = '%'; q += 6; }
          else if (!strncmp(q, "REG", 3)) { *d++ = '%'; q += 3; }
          else *d++ = *q++;
      }
      *d = 0; }

    char result[256] = "";
    const char *p = buf;
    for (int i = 0; i < sp->nop; i++) {
        const char *pct = strstr(p, "%s");
        if (!pct) break;
        strncat(result, p, (size_t)(pct - p));
        const char *rn = "rax";
        long long v = vals[i];
        switch (sp->opsv[i].kind) {
            case RX_R64: rn = RN(64, (int)v); break;
            case RX_R32: rn = RN(32, (int)v); break;
            case RX_R16: rn = RN(16, (int)v); break;
            case RX_R8:  rn = RN(8, (int)v); break;
            case RX_MEMBASE:
            case RX_SIBBASE:
                /* A BASE REGISTER FOR A MEMORY OPERAND: the BARE NAME. The format already spells
                 * the '%' (it is written "%s(REG%s)" so that the base gets one), so contributing
                 * another marker here produced "%%rax" and llvm-mc refused every memory form. */
                rn = RN(64, (int)v);
                break;
            case RX_DISP8:
            case RX_DISP32: {
                static char db[24];
                snprintf(db, sizeof db, "%lld", v);
                rn = db;
                break;
            }
            case RX_REL8:
            case RX_REL32: {
                /* THE WHOLE TARGET EXPRESSION COMES FROM HERE, so the formats are written
                 * "jne %s" rather than "jne .+%s". A branch offset has to be spelled either
                 * ".+N" or ".-N" -- ".+-N" is a SYNTAX ERROR -- so the sign has to be part of the
                 * text, and the only place that knows the sign is here.
                 *
                 * The offset is measured from the START of the instruction (measured, not
                 * assumed: `jo .+2` encodes 0, `jo .-4` encodes -6, both consistent with
                 * enc = written_offset - length), and the caller adds the length back so that the
                 * operand value IS the encoded displacement. */
                static char rb[40];
                if (v >= 0) snprintf(rb, sizeof rb, ".+%lld", v);
                else        snprintf(rb, sizeof rb, ".-%lld", -v);
                rn = rb;
                break;
            }
            case RX_SIBSCALE: {
                /* THE OPERAND CARRIES THE RAW 2-BIT FIELD (0..3) AND THE TEXT CARRIES THE
                 * LITERAL (1,2,4,8). If the operand held the literal instead, the solver would
                 * look for a field whose bits track 1,2,4,8 -- and 8 is a 4-bit value in a 2-bit
                 * field, so it can never place it and every SIB form is refused. */
                static char sb[8];
                static const char *LIT[4] = {"1", "2", "4", "8"};
                snprintf(sb, sizeof sb, "%s", LIT[(int)(v & 3)]);
                rn = sb;
                break;
            }
            case RX_SIBIDX:
            case RX_OPREG:
                /* bare name; the format supplies the '%' */
                rn = RN(64, (int)v);
                break;
            case RX_IMM: {
                /* the sampler supplies the immediate, so the immediate bytes VARY across
                 * samples and the solver has to place them as a field. A hard-coded immediate
                 * in the format text is folded into the base template, and the row then
                 * matches only that one value. */
                static char ib[32];
                snprintf(ib, sizeof ib, "%lld", v);
                rn = ib;
                break;
            }
            case RX_IMM64: {
                /* A 64-bit immediate. Without this case the switch fell through and `rn` kept
                 * the value from the PREVIOUS operand -- so "movabsq $%s, REG%s" printed the
                 * register name in the immediate slot, llvm-mc read it as a symbol and encoded
                 * zeros, and every movabs sample had a zero upper half. The solver then found no
                 * split, and the row was emitted with a 32-bit "immediate" that could never
                 * reproduce a real movabs. Print the full 64-bit value. */
                static char ib64[40];
                snprintf(ib64, sizeof ib64, "0x%llx", (unsigned long long)v);
                rn = ib64;
                break;
            }
            case RX_IMM8: {
                /* A NARROW IMMEDIATE IS A DIFFERENT OPCODE, not a smaller number of the same
                 * one: an 8-bit immediate selects 83 /id while a 32-bit immediate selects
                 * 81 /id. The kind exists so the two are sampled as separate forms; leaving it
                 * out of this switch made every I8 form emit "addq $rax, %rdx" -- the register
                 * name in the immediate slot -- which llvm-mc then read as a symbol and encoded
                 * as zeros. The solver saw a non-varying immediate, folded it into the template,
                 * and the row only matched one value. */
                static char ib8[32];
                snprintf(ib8, sizeof ib8, "%lld", v);
                rn = ib8;
                break;
            }
        }
        strncat(result, rn, sizeof result - strlen(result) - 1);
        p = pct + 2;
    }
    strncat(result, p, sizeof result - strlen(result) - 1);
    /* EXPAND THE MARKERS IN THE RESULT TOO. Substituted operand text can itself contain a
     * marker -- a memory operand renders as "%s(REG%s)" and the base register's text carries
     * "REG" -- and since expansion only ran on the format, those arrived literally. llvm-mc then
     * read "%REGrax" as a symbol and refused every memory form, so all of them came back with
     * zero samples. */
    {
        char fin[256], *d = fin, *q = result;
        while (*q && (size_t)(d - fin) < sizeof fin - 8) {
            if (!strncmp(q, "LITPCT", 6)) { *d++ = '%'; q += 6; }
            else if (!strncmp(q, "REG", 3)) { *d++ = '%'; q += 3; }
            else *d++ = *q++;
        }
        *d = 0;
        snprintf(out, n, "%s", fin);
    }
}

/* ASSEMBLE ONE SAMPLE, MAKING A BRANCH OPERAND MEAN THE ENCODED DISPLACEMENT.
 *
 * A row's field must hold what is actually IN THE BYTES. For a branch that is
 * target - (instruction_start + length): llvm-mc subtracts the length, so "jne .+2" does not
 * store 2, it stores 0 -- and the solver, hunting for a field that tracks the operand value 2,
 * finds nothing and refuses every branch row. Two passes fix it exactly: assemble once to learn
 * the length, then re-assemble with the target moved out by that length so the encoded
 * displacement equals the operand. The width is stable across the adjustment (the sample values
 * keep each form in one width), so a single correction is enough. */
static int asm_sample(const Spec *sp, const long long *vals, uint8_t *out, int outmax) {
    char line[256];
    int rel = 0;
    for (int i = 0; i < sp->nop; i++)
        if (sp->opsv[i].kind == RX_REL8 || sp->opsv[i].kind == RX_REL32) rel = 1;
    fill_asm(sp, vals, line, sizeof line);
    int len = asm_one(line, out, outmax);
    if (len <= 0 || !rel) return len;
    long long v2[6];
    for (int i = 0; i < sp->nop; i++) {
        v2[i] = vals[i];
        int kd = sp->opsv[i].kind;
        if (kd != RX_REL8 && kd != RX_REL32) continue;
        /* THE OFFSET IS ALWAYS MEASURED FROM THE INSTRUCTION START, SO THE TEXT NEEDS D + len.
         *
         * Measured, five cases, every one consistent with enc = V - len where '.' is the start of
         * the instruction and V is the offset as written:
         *     jo .+2  at 0 -> 70 00   V=2   enc=0   = V - 2
         *     jo .-2  at 2 -> 70 fc   V=-2  enc=-4  = V - 2
         *     jo .+4  at 6 -> 70 02   V=4   enc=2   = V - 2
         *     jo .-4  at 8 -> 70 fa   V=-4  enc=-6  = V - 2
         * So asking for a displacement of D means writing V = D + len, for BOTH SIGNS.
         *
         * The first version added len only for positive D and subtracted it for negative, which
         * put every negative branch one length off -- the solver then found no field and all 32
         * J<cc>_REL8/REL32 rows were dropped with "operand 0 has no field", while their positive
         * samples assembled perfectly and made the rows look merely unbuildable. */
        v2[i] = vals[i] + len;
        if (kd == RX_REL8 && (v2[i] > 127 || v2[i] < -128)) return 0;
    }
    char line2[256];
    fill_asm(sp, v2, line2, sizeof line2);
    int len2 = asm_one(line2, out, outmax);
    if (len2 != len) return 0;
    return len2;
}

static void build_specs(void) {
    static const struct { const char *nm, *r1, *r2; int a, b; } ALU[] = {
        {"add","ADD_RM_R","ADD_R_RM",0x01,0x03}, {"or","OR_RM_R","OR_R_RM",0x09,0x0B},
        {"adc","ADC_RM_R","ADC_R_RM",0x11,0x13}, {"sbb","SBB_RM_R","SBB_R_RM",0x19,0x1B},
        {"and","AND_RM_R","AND_R_RM",0x21,0x23}, {"sub","SUB_RM_R","SUB_R_RM",0x29,0x2B},
        {"xor","XOR_RM_R","XOR_R_RM",0x31,0x33}, {"cmp","CMP_RM_R","CMP_R_RM",0x39,0x3B},
    };
    char nm[64], fmt[96];
    for (int i = 0; i < (int)(sizeof ALU / sizeof ALU[0]); i++) {
        snprintf(nm, sizeof nm, "%s_64_RR", ALU[i].r1);
        snprintf(fmt, sizeof fmt, "%sq REG%%s, REG%%s", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "r64_rm, r64_reg", fmt, 2, RX_R64, RX_R64);
        snprintf(nm, sizeof nm, "%s_64_RR", ALU[i].r2);
        spec_add(nm, ALU[i].nm, "r64_reg, r64_rm", fmt, 2, RX_R64, RX_R64);
        snprintf(nm, sizeof nm, "%s_32_RR", ALU[i].r1);
        snprintf(fmt, sizeof fmt, "%sl REG%%s, REG%%s", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "r32_rm, r32_reg", fmt, 2, RX_R32, RX_R32);
        snprintf(nm, sizeof nm, "%s_32_RR", ALU[i].r2);
        spec_add(nm, ALU[i].nm, "r32_reg, r32_rm", fmt, 2, RX_R32, RX_R32);
    }
    /* unary group F7 / FF */
    static const char *U1[] = {"not","neg","mul","imul","div","idiv"};
    for (int i = 0; i < 6; i++) {
        snprintf(nm, sizeof nm, "GRP3_%s_64", U1[i]);
        snprintf(fmt, sizeof fmt, "%sq REG%%s", U1[i]);
        spec_add(nm, U1[i], "r64", fmt, 1, RX_R64);
        snprintf(nm, sizeof nm, "GRP3_%s_32", U1[i]);
        snprintf(fmt, sizeof fmt, "%sl REG%%s", U1[i]);
        spec_add(nm, U1[i], "r32", fmt, 1, RX_R32);
    }
    spec_add("MOV_RM_R_64", "mov", "r64_rm, r64_reg", "movq REG%s, REG%s", 2, RX_R64, RX_R64);
    spec_add("MOV_R_RM_64", "mov", "r64_reg, r64_rm", "movq REG%s, REG%s", 2, RX_R64, RX_R64);
    spec_add("MOV_RM_R_32", "mov", "r32_rm, r32_reg", "movl REG%s, REG%s", 2, RX_R32, RX_R32);
    spec_add("MOV_R_RM_32", "mov", "r32_reg, r32_rm", "movl REG%s, REG%s", 2, RX_R32, RX_R32);
    spec_add("TEST_RM_R_64", "test", "r64_rm, r64_reg", "testq REG%s, REG%s", 2, RX_R64, RX_R64);
    spec_add("TEST_RM_R_32", "test", "r32_rm, r32_reg", "testl REG%s, REG%s", 2, RX_R32, RX_R32);
    /* 64-BIT: its own kind, because the runtime stores it as K_SPLIT (two 32-bit halves) and
     * sampling it as a 32-bit RX_IMM produced a row that only reproduced values below 2^32. */
    spec_add("MOV_R_IMM64", "mov", "r64, #imm64", "movabsq $%s, REG%s", 2, RX_IMM64, RX_R64);
    spec_add("MOV_R_IMM32", "mov", "r32, #imm32", "movl $%s, REG%s", 2, RX_IMM, RX_R32);
    /* the ALU immediate group: reg, imm32 (81) and reg, imm8 sign-extended (83) */
    for (int i = 0; i < (int)(sizeof ALU / sizeof ALU[0]); i++) {
        snprintf(nm, sizeof nm, "GRP1_%s_64_I32", ALU[i].nm);
        snprintf(fmt, sizeof fmt, "%sq $%%s, REG%%s", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "r64, #imm32", fmt, 2, RX_IMM, RX_R64);
        snprintf(nm, sizeof nm, "GRP1_%s_64_I8", ALU[i].nm);
        snprintf(fmt, sizeof fmt, "%sq $%%s, REG%%s", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "r64, #imm8", fmt, 2, RX_IMM8, RX_R64);
    }
    /* shifts by immediate (C1) */
    static const char *SH[] = {"rol","ror","rcl","rcr","shl","shr","shl","sar"};
    static const char *SHN[] = {"ROL","ROR","RCL","RCR","SHL","SHR","SAR","SAR2"};
    for (int i = 0; i < 8; i++) {
        if (i == 6) continue;   /* "shl $3, %rax" is ambiguous with SHL/SAR in AT&T */
        snprintf(nm, sizeof nm, "GRP2_%s_64_I8", SHN[i]);
        snprintf(fmt, sizeof fmt, "%sq $%%s, REG%%s", SH[i]);
        /* C1 /id ib: the count is ALWAYS a byte, so sample values that fit one -- sampling
         * wide values would ask llvm-mc for an encoding this opcode does not have. */
        spec_add(nm, SH[i], "r64, #imm8", fmt, 2, RX_IMM8, RX_R64);
    }
    spec_add("MOVZX_64_R8",  "movzx", "r64, r8",  "movzbq REG%s, REG%s", 2, RX_R8, RX_R64);
    /* the 32-bit destination forms: 0F B6/B7 without REX.W, which is what a compiler emits for
     * a plain byte or word load into a 32-bit register. Missing them left "movzbl %al, %ecx"
     * with no row -- and that is one of the most common instructions in compiled x86. */
    spec_add("MOVZX_32_R8",  "movzx", "r32, r8",  "movzbl REG%s, REG%s", 2, RX_R8,  RX_R32);
    spec_add("MOVZX_32_R16", "movzx", "r32, r16", "movzwl REG%s, REG%s", 2, RX_R16, RX_R32);
    spec_add("MOVSX_32_R8",  "movsx", "r32, r8",  "movsbl REG%s, REG%s", 2, RX_R8,  RX_R32);
    spec_add("MOVSX_32_R16", "movsx", "r32, r16", "movswl REG%s, REG%s", 2, RX_R16, RX_R32);
    spec_add("MOVZX_64_R16", "movzx", "r64, r16", "movzwq REG%s, REG%s", 2, RX_R16, RX_R64);
    spec_add("MOVSX_64_R8",  "movsx", "r64, r8",  "movsbq REG%s, REG%s", 2, RX_R8, RX_R64);
    spec_add("MOVSX_64_R16", "movsx", "r64, r16", "movswq REG%s, REG%s", 2, RX_R16, RX_R64);
    spec_add("MOVSX_64_R32", "movsxd","r64, r32", "movslq REG%s, REG%s", 2, RX_R32, RX_R64);
    static const char *CC[] = {"o","no","b","ae","e","ne","be","a",
                               "s","ns","p","np","l","ge","le","g"};
    static const char *CCN[] = {"O","NO","B","AE","E","NE","BE","A",
                                "S","NS","P","NP","L","GE","LE","G"};
    for (int i = 0; i < 16; i++) {
        snprintf(nm, sizeof nm, "SET%s_8", CCN[i]);
        snprintf(fmt, sizeof fmt, "set%s REG%%s", CC[i]);
        spec_add(nm, "setcc", "r8", fmt, 1, RX_R8);
        snprintf(nm, sizeof nm, "CMOV%s_64", CCN[i]);
        snprintf(fmt, sizeof fmt, "cmov%sq REG%%s, REG%%s", CC[i]);
        spec_add(nm, "cmovcc", "r64_reg, r64_rm", fmt, 2, RX_R64, RX_R64);
    }
    spec_add("LEA_64", "lea", "r64_reg, [rm]", "leaq (REG%s), REG%s", 2, RX_R64, RX_R64);

    /* ============================ MEMORY OPERANDS ============================
     * What real compiled x86-64 is actually made of. Every one of these was missing, which is
     * why the catalog decoded 12% of clang's own output while claiming coverage.
     *
     * AT&T writes a memory operand as `disp(base)`. The displacement is substituted FIRST and
     * the base second, so the format is "%s(REG%s)" -- and once substituted that is just
     * `4(REGrdi)`, which the existing substitution renders correctly. The runtime needs nothing
     * new: the displacement is a K_IMM field and the base is a K_REXREG field (three ModR/M bits
     * plus the operand's REX bit).
     *
     * Three mod shapes, and they are SEPARATE ROWS because the displacement width changes the
     * instruction LENGTH: mod=00 has no displacement, mod=01 a signed byte, mod=10 a dword.
     */
    for (int i = 0; i < (int)(sizeof ALU / sizeof ALU[0]); i++) {
        /* ALU r/m, r  with the r/m being memory -- `addl 4(%rdi), %eax` */
        snprintf(nm, sizeof nm, "%s_MEM_R_32", ALU[i].r1);
        snprintf(fmt, sizeof fmt, "%sl %%s(REG%%s), REG%%s", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "disp32, base, r32_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R32);
        snprintf(nm, sizeof nm, "%s_MEM_R_64", ALU[i].r1);
        snprintf(fmt, sizeof fmt, "%sq %%s(REG%%s), REG%%s", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "disp32, base, r64_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R64);
        /* ALU r, r/m  with memory as the SOURCE -- `addl %eax, 4(%rdi)` */
        snprintf(nm, sizeof nm, "%s_R_MEM_32", ALU[i].r2);
        snprintf(fmt, sizeof fmt, "%sl REG%%s, %%s(REG%%s)", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "r32_reg, disp32, base", fmt, 3, RX_R32, RX_DISP32, RX_MEMBASE);
        snprintf(nm, sizeof nm, "%s_R_MEM_64", ALU[i].r2);
        snprintf(fmt, sizeof fmt, "%sq REG%%s, %%s(REG%%s)", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "r64_reg, disp32, base", fmt, 3, RX_R64, RX_DISP32, RX_MEMBASE);
        /* the ALU-with-memory-destination and an immediate: `addl $1, 4(%rdi)` (81 /0) */
        snprintf(nm, sizeof nm, "%s_MEM_IMM32_32", ALU[i].r1);
        snprintf(fmt, sizeof fmt, "%sl $%%s, %%s(REG%%s)", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "imm32, disp32, base", fmt, 3, RX_IMM, RX_DISP32, RX_MEMBASE);
        snprintf(nm, sizeof nm, "%s_MEM_IMM8_32", ALU[i].r1);
        snprintf(fmt, sizeof fmt, "%sl $%%s, %%s(REG%%s)", ALU[i].nm);
        spec_add(nm, ALU[i].nm, "imm8, disp32, base", fmt, 3, RX_IMM8, RX_DISP32, RX_MEMBASE);
    }
    /* mov with memory on either side -- the single most common instruction in compiled code */
    snprintf(fmt, sizeof fmt, "movl %%s(REG%%s), REG%%s");
    spec_add("MOV_MEM_R_32", "mov", "disp32, base, r32_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R32);
    snprintf(fmt, sizeof fmt, "movq %%s(REG%%s), REG%%s");
    spec_add("MOV_MEM_R_64", "mov", "disp32, base, r64_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R64);
    snprintf(fmt, sizeof fmt, "movl REG%%s, %%s(REG%%s)");
    spec_add("MOV_R_MEM_32", "mov", "r32_reg, disp32, base", fmt, 3, RX_R32, RX_DISP32, RX_MEMBASE);
    snprintf(fmt, sizeof fmt, "movq REG%%s, %%s(REG%%s)");
    spec_add("MOV_R_MEM_64", "mov", "r64_reg, disp32, base", fmt, 3, RX_R64, RX_DISP32, RX_MEMBASE);
    /* the byte and word load/store forms compilers emit constantly */
    snprintf(fmt, sizeof fmt, "movzbl %%s(REG%%s), REG%%s");
    spec_add("MOVZX_R32_MEM8", "movzx", "disp32, base, r32_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R32);
    snprintf(fmt, sizeof fmt, "movzbq %%s(REG%%s), REG%%s");
    spec_add("MOVZX_R64_MEM8", "movzx", "disp32, base, r64_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R64);
    snprintf(fmt, sizeof fmt, "movsbl %%s(REG%%s), REG%%s");
    spec_add("MOVSX_R32_MEM8", "movsx", "disp32, base, r32_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R32);
    snprintf(fmt, sizeof fmt, "movslq %%s(REG%%s), REG%%s");
    spec_add("MOVSX_R64_MEM32", "movsxd", "disp32, base, r64_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R64);
    snprintf(fmt, sizeof fmt, "movb REG%%s, %%s(REG%%s)");
    spec_add("MOV_R8_MEM", "mov", "r8_reg, disp32, base", fmt, 3, RX_R8, RX_DISP32, RX_MEMBASE);
    snprintf(fmt, sizeof fmt, "movw REG%%s, %%s(REG%%s)");
    spec_add("MOV_R16_MEM", "mov", "r16_reg, disp32, base", fmt, 3, RX_R16, RX_DISP32, RX_MEMBASE);
    snprintf(fmt, sizeof fmt, "cmpb $%%s, %%s(REG%%s)");
    spec_add("CMP_MEM_IMM8_8", "cmp", "imm8, disp32, base", fmt, 3, RX_IMM8, RX_DISP32, RX_MEMBASE);
    /* test/cmp memory against a register */
    snprintf(fmt, sizeof fmt, "testl REG%%s, %%s(REG%%s)");
    spec_add("TEST_R_MEM_32", "test", "r32_reg, disp32, base", fmt, 3, RX_R32, RX_DISP32, RX_MEMBASE);
    snprintf(fmt, sizeof fmt, "testb REG%%s, %%s(REG%%s)");
    spec_add("TEST_R8_MEM", "test", "r8_reg, disp32, base", fmt, 3, RX_R8, RX_DISP32, RX_MEMBASE);
    snprintf(fmt, sizeof fmt, "cmpq REG%%s, %%s(REG%%s)");
    spec_add("CMP_R_MEM_64", "cmp", "r64_reg, disp32, base", fmt, 3, RX_R64, RX_DISP32, RX_MEMBASE);
    /* lea with a real base+displacement is the address-arithmetic workhorse */
    snprintf(fmt, sizeof fmt, "leaq %%s(REG%%s), REG%%s");
    spec_add("LEA_R64_MEM", "lea", "disp32, base, r64_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R64);
    /* imul with a memory source */
    snprintf(fmt, sizeof fmt, "imull %%s(REG%%s), REG%%s");
    spec_add("IMUL_R32_MEM", "imul", "disp32, base, r32_reg", fmt, 3, RX_DISP32, RX_MEMBASE, RX_R32);

    /* ======================= CONTROL TRANSFER, SIB, ONE-BYTE OPCODES =======================
     * The rest of what clang actually emitted and the catalog could not decode: every branch,
     * every indexed addressing mode, the inc/dec group, and push/pop.
     */
    /* branches, both widths. llvm-mc picks the short form when the target fits rel8 and the near
     * form otherwise, so the two are registered separately and sampled with targets that force
     * the width: 2 forces rel8, 0x100 forces rel32.
     *
     * setcc and cmovcc ALREADY EXIST ABOVE -- registering them twice would emit two rows per
     * encoding and make the catalog ambiguous (isa_decode returns the longest match, so decoding
     * would still work while the matrix cross-check reported a duplicate). */
    for (int i = 0; i < 16; i++) {
        snprintf(nm, sizeof nm, "J%s_REL8", CCN[i]);
        snprintf(fmt, sizeof fmt, "j%s %%s", CC[i]);
        spec_add(nm, "jcc", "rel8", fmt, 1, RX_REL8);
        snprintf(nm, sizeof nm, "J%s_REL32", CCN[i]);
        snprintf(fmt, sizeof fmt, "j%s %%s", CC[i]);
        spec_add(nm, "jcc", "rel32", fmt, 1, RX_REL32);
    }
    spec_add("JMP_REL8",  "jmp", "rel8",  "jmp %s", 1, RX_REL8);
    spec_add("JMP_REL32", "jmp", "rel32", "jmp %s", 1, RX_REL32);
    spec_add("CALL_REL32","call","rel32", "call %s", 1, RX_REL32);
    spec_add("RET",       "ret", "none",  "ret", 0);
    /* the one-byte inc/dec group: ff /0 and ff /1. The register is the low 3 bits of the ModR/M
     * byte, which is the K_REXREG shape -- three bits plus the operand's REX bit. */
    spec_add("INC_R64", "inc", "r64_reg", "incq REG%s", 1, RX_R64);
    spec_add("INC_R32", "inc", "r32_reg", "incl REG%s", 1, RX_R32);
    spec_add("DEC_R64", "dec", "r64_reg", "decq REG%s", 1, RX_R64);
    spec_add("DEC_R32", "dec", "r32_reg", "decl REG%s", 1, RX_R32);
    spec_add("NOT_R64","not", "r64_reg", "notq REG%s", 1, RX_R64);
    spec_add("NEG_R64","neg", "r64_reg", "negq REG%s", 1, RX_R64);
    /* push/pop: the register is packed into the single opcode byte (50+r / 58+r) */
    spec_add("PUSH_R64", "push", "r64_reg", "pushq REG%s", 1, RX_OPREG);
    spec_add("POP_R64",  "pop",  "r64_reg", "popq REG%s",  1, RX_OPREG);
    /* nop -- clang pads with it, so an alignment padding run must decode */
    spec_add("NOP", "nop", "none", "nop", 0);
    /* SIB ADDRESSING: `(%base,%index,scale)`. This is how a compiler walks an array, and it was
     * the single biggest source of gaps. The scale is written as the literal 1,2,4,8 in AT&T, so
     * the operand value is the scale ITSELF, not its 2-bit encoding -- fill_asm maps it. */
    {
        struct { const char *nm, *r1, *r2, *s64, *s32; } S[] = {
            {"ADD", "add", "addl", "addq"}, {"MOV", "mov", "movl", "movq"},
            {"SUB", "sub", "subl", "subq"}, {"CMP", "cmp", "cmpl", "cmpq"},
            {"OR",  "or",  "orl",  "orq"},  {"AND", "and", "andl", "andq"},
            {"XOR", "xor", "xorl", "xorq"}, {"LEA", "lea", "leal", "leaq"},
        };
        for (int i = 0; i < (int)(sizeof S / sizeof S[0]); i++) {
            /* r/m source (no displacement, so mod=00 and the SIB byte is mandatory) */
            snprintf(nm, sizeof nm, "%s_SIB_R_32", S[i].nm);
            snprintf(fmt, sizeof fmt, "%s (REG%%s,REG%%s,%%s), REG%%s", S[i].r2);
            spec_add(nm, S[i].r1, "base, index, scale, r32_reg", fmt, 4,
                     RX_SIBBASE, RX_SIBIDX, RX_SIBSCALE, RX_R32);
            snprintf(nm, sizeof nm, "%s_SIB_R_64", S[i].nm);
            snprintf(fmt, sizeof fmt, "%s (REG%%s,REG%%s,%%s), REG%%s", S[i].s64);
            spec_add(nm, S[i].r1, "base, index, scale, r64_reg", fmt, 4,
                     RX_SIBBASE, RX_SIBIDX, RX_SIBSCALE, RX_R64);
            /* with a displacement: mod=01, SIB still present */
            snprintf(nm, sizeof nm, "%s_SIBD_R_32", S[i].nm);
            snprintf(fmt, sizeof fmt, "%s %%s(REG%%s,REG%%s,%%s), REG%%s", S[i].r2);
            spec_add(nm, S[i].r1, "disp32, base, index, scale, r32_reg", fmt, 5,
                     RX_DISP8, RX_SIBBASE, RX_SIBIDX, RX_SIBSCALE, RX_R32);
            snprintf(nm, sizeof nm, "%s_SIBD_R_64", S[i].nm);
            snprintf(fmt, sizeof fmt, "%s %%s(REG%%s,REG%%s,%%s), REG%%s", S[i].s64);
            spec_add(nm, S[i].r1, "disp32, base, index, scale, r64_reg", fmt, 5,
                     RX_DISP8, RX_SIBBASE, RX_SIBIDX, RX_SIBSCALE, RX_R64);
        }
    }
}

/* ============================================================ sampling ===== */

/* Sample a form over a restricted register range. rlo/rhi select the bank: 0..7 (no REX) or
 * 8..15 (REX present). A 32-bit form needs one sampling in each because the REX byte changes
 * the instruction LENGTH, and a row's length is fixed. */
/* THE ONE PLACE THAT DECIDES AN OPERAND'S SAMPLE VALUE.
 *
 * There were two samplers (a full-range one and a mixed-bank one) with the operand-kind
 * dispatch written out TWICE, and they drifted: the mixed sampler learned about the new kinds
 * and the full-range one did not, then a later fix to one produced a compile error in the
 * other. Every one of the "form refused with zero samples" failures in this file traces back to
 * one of the two copies being wrong. Now both call this, so a new kind cannot be half-added.
 *
 * Returns 0 to mean "this kind has no fixed scheme -- use the caller's register walk".
 * Every value produced here obeys the requested bank [rlo, rhi], because a value outside it
 * silently changes the instruction's LENGTH (a register above 7 adds a REX byte), which turns
 * one form into two encodings and makes the row unemittable.
 */
static int sample_value(int kind, int s, int i, int rlo, int rhi, long long regval,
                        long long *out) {
    (void)i; (void)rlo; (void)rhi;
    /* THE CALLER PASSES THE REGISTER VALUE IT ALREADY COMPUTED. That value comes from the
     * per-operand phased walk (V[i][s]), which is bank-correct and distinct per operand. Recomputing
     * a register value here from `s` alone gave every operand of a sample the SAME register, so the
     * distinctness pass had to nudge them apart, the sample set collapsed, and forms were refused
     * for having too few distinct samplings. Register-like kinds now only adjust the value the
     * caller chose -- they never re-derive it. */
    long long rv = regval;

    switch (kind) {
        case RX_IMM64:
            *out = (long long)(0x0403020105060708ull
                               + (unsigned long long)s * 0x0102030405060708ull);
            return 1;
        case RX_IMM8: {
            static const int V[8] = {2, 4, 8, 16, 32, 64, 80, 96};
            *out = V[s & 7];
            return 1;
        }
        case RX_IMM:
            *out = (int)(0x01010101u * (unsigned)(s + 1));
            return 1;
        case RX_MEMBASE: case RX_SIBBASE: {
            long long bv = rv;
            if (kind == RX_MEMBASE && (bv & 7) == 4) bv = (bv & ~7) | 5;   /* rsp: not a plain base */
            /* IN A SIB BYTE WITH NO DISPLACEMENT, base=101 (rbp/r13) MEANS "NO BASE REGISTER,
             * disp32 follows" -- a different instruction with a different length. So rbp/r13 is
             * not addressable as a SIB base at mod=00. Sampling it produced "addl (%rsp,%rbp,1),
             * %edx", which llvm-mc encoded as the disp32 shape, so the no-displacement row got a
             * mix of lengths and was refused. rsp(4) HAS to be allowed here (it is a legal SIB
             * base) -- unlike in the ModR/M base position, where rm=100 means "SIB follows". */
            if (kind == RX_SIBBASE && (bv & 7) == 5) bv = (bv & ~7) | 6;
            *out = bv;
            return 1;
        }
        case RX_SIBIDX: {
            long long iv = rv;
            if ((iv & 7) == 4) iv = (iv & ~7) | 5;                        /* rsp: means "no index" */
            *out = iv;
            return 1;
        }
        case RX_SIBSCALE: {
            /* THE OPERAND CARRIES THE RAW 2-BIT FIELD, 0..3, DISTINCT PER SAMPLE.
             *
             * The value that reaches the solver is what this returns, and the solver needs the
             * eight samples to map unambiguously onto FIELD VALUES. `s & 3` gives 0,1,2,3,0,1,2,3
             * -- only four distinct values across eight samples, so two samples agree on the
             * scale, and when the rest of a sample also agrees the sample set collapses and the
             * field cannot be placed ("operand 2 has no field"). Interleaving the two low bits
             * with a slow bit keeps four values but spreads them so no two ADJACENT samples
             * repeat, which is what the solver's per-sample differencing needs. */
            *out = ((s & 1) << 1) | ((s >> 2) & 1);
            return 1;
        }
        case RX_REL8:
        case RX_REL32: {
            /* A BRANCH TARGET CANNOT BE WRITTEN AS AN OFFSET IN AT&T SYNTAX. ".+-2" is a syntax
             * error, so llvm-mc produced nothing, and the sample set for every branch row was
             * empty. The target must be an absolute address the assembler can compute a
             * displacement from: it is emitted as ".+N" for a positive N and ".-N" for a
             * negative, which is exactly an address relative to the start of the instruction. */
            static const int V8[8]  = {2, -2, 4, -4, 8, -8, 16, -16};
            static const int V32[8] = {0x100, -0x100, 0x1000, -0x1000,
                                       0x10000, -0x10000, 0x100000, -0x100000};
            *out = (kind == RX_REL8) ? V8[s & 7] : V32[s & 7];
            return 1;
        }
        case RX_OPREG:
            /* PUSH/POP: the register is the low 3 bits of the ONLY opcode byte, so the operand's
             * REX.B is a separate byte that changes the instruction LENGTH. The caller's phased
             * value is already bank-correct, so it is used as-is. */
            *out = rv;
            return 1;
        case RX_DISP8: {
            static const int V[8] = {1, -1, 4, -4, 16, -16, 64, -64};
            *out = V[s & 7];
            return 1;
        }
        case RX_DISP32: {
            static const int V[8] = {0x1234, -0x1234, 0x10000, -0x10000,
                                     0x100000, -0x100000, 0x7ffff, -0x7ffff};
            *out = V[s & 7];
            return 1;
        }
        default:
            return 0;
    }
}

static int sample_form_range(const Spec *sp, Samples *S, int rlo, int rhi) {
    memset(S, 0, sizeof *S);
    /* SET has_reg FROM THE SPEC. It means "this form has at least one operand that is a register"
     * -- an immediate, a displacement or a branch target is not. Only then can the extended bank
     * be exercised and only then does the split check below apply. */
    for (int i = 0; i < sp->nop; i++) {
        int kd = sp->opsv[i].kind;
        if (kd == RX_IMM || kd == RX_IMM8 || kd == RX_IMM64 || kd == RX_DISP8
            || kd == RX_DISP32 || kd == RX_REL8 || kd == RX_REL32 || kd == RX_SIBSCALE)
            continue;
        S->has_reg = 1;
    }
    /* A ONE-BYTE-OPCODE FORM (push/pop) MUST BE SAMPLED BANK BY BANK. Its register is inside the
     * opcode byte and every other bank's REX byte changes the LENGTH, so a full-range request can
     * never satisfy "one row, one length" -- it collects 8 low-bank samples and is refused. The
     * caller's low/high comparison then sees equal lengths and decides no split is needed, so the
     * extended rows are never emitted at all. Sampling a bank at a time here makes both shapes
     * real. */
    int opreg_form = 0;
    for (int i = 0; i < sp->nop; i++)
        if (sp->opsv[i].kind == RX_OPREG) opreg_form = 1;
    if (opreg_form && rlo == 0 && rhi == 15) rhi = 7;
    /* Values are chosen so registers 8..15 APPEAR (that is what exposes a REX bit) and so
     * no two operands hold the same value in the same sample -- llvm-mc refuses "cmovgq
     * %rax, %rax" and "leaq (%rax), %rax" as meaningless, and one refused sample used to
     * discard the entire form. */
    /* Values inside the requested bank. The bank's values are what expose (or hide) a REX
     * bit, so the sampler must not mix banks in one row. */
    /* Sample values from a rotated stride, which CANNOT collide.
     *
     * Two earlier attempts were ad-hoc formulas and both collided: v = (s + k*i) mod 16 with
     * a "if it clashes, add 5" fixup produced the SAME pair for different samples (s=2 and
     * s=4 both gave [12,15]), so the sample set silently collapsed and forms were dropped
     * for having too few distinct samplings. A collision here does not look like a bug; it
     * looks like a field that cannot be solved.
     *
     * This construction is provably distinct: sample s, operand i uses permutation index
     * (s + 2*i), and the high bank is entered for s >= 4 so both banks are visited on a full
     * range. Two samples differ unless (s + 2*i) agrees for all i, which needs 2*(i-j) = 0
     * mod 16 -- impossible here.
     */
    long long V[6][8];
    int span = rhi - rlo + 1;
    /* PERMUTATION INDEXING, not a linear walk. Within a bank the low bits of the register ARE
     * the sample-varying part, so a linear walk makes operand 0 and operand 1 step together:
     * the solver then sees one varying quantity for two operands and cannot place either
     * field (it reported operand 1 as unsolvable and the whole extended row was dropped).
     * Indexing a permutation by (s + 2*i) makes each operand's sequence distinct for every
     * sample, in either bank. */
    static const int PERM16[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    for (int i = 0; i < 6; i++)
        for (int s = 0; s < 8; s++) {
            int idx;
            if (span > 8) {
                /* FULL RANGE: each operand gets its OWN high-bank phase, so every operand
                 * position is exercised in the extended bank at least once. With a shared
                 * phase (all operands high at the same sample index) the second operand's REX
                 * bit is never seen, and the solver emits a row that encodes REX.R as 0 --
                 * which then silently fails on every instruction using r8-r15 in that
                 * position. That is exactly how LEA_64 came out able to encode REX.B but not
                 * REX.R: "leaq (%r8), %r9" worked and "leaq (%r8), %r9" with the OTHER
                 * operand extended did not.
                 *
                 * Phase: operand i is high on samples (s / 2) % 2 == i, so the two operands
                 * are never high in the same sample and each covers 4 of the 8. */
                int high = ((s / 2) % 2) == (i % 2);
                int lo = (s + 2 * i) & 7;
                idx = lo + (high ? 8 : 0);
            } else {
                idx = (s + 2 * i) & 15;
            }
            int v = PERM16[idx] % (span > 0 ? span : 1);
            V[i][s] = rlo + v;
        }
    /* BUILD ALL EIGHT SAMPLES, THEN ASSEMBLE THEM IN ONE PROCESS.
     *
     * The samples are computed first (values, clash fixes, generated text) and assembled as a
     * batch, because one llvm-mc process per sample was the entire cost of this generator: eight
     * spawns and sixteen temp files per form, thousands for a full emit. The batch is the fast
     * path; a form the batch cannot assemble falls back to one process per line, which keeps the
     * behaviour identical where it matters and costs nothing when lines are valid. */
    {
        /* FILE-SCOPE STATIC, not a per-call carve. A Batch is XB_MAX * 288 bytes (about 1.2 MB)
         * and sample_form_range is called for every form, every bank and every mixed shape, so
         * carving one per call exhausts arbit's carve log long before the catalog is finished --
         * which is exactly what "arbit_carve refuses at _CARVE_LOG_MAX" looks like from here. */
        Batch *bt = g_batch;
        char   lines[8][256];
        long long svals[8][6];
        int    nline = 0;
        if (bt) batch_reset(bt);

        for (int s = 0; s < 8; s++) {
            long long vals[6];
            for (int i = 0; i < sp->nop; i++) {
                vals[i] = V[i][s];
                long long sv;
                if (sample_value(sp->opsv[i].kind, s, i, rlo, rhi, V[i][s], &sv)) vals[i] = sv;
            }
            /* NEVER TOUCH A NON-REGISTER OPERAND. The clash rule exists so two REGISTER operands
             * are not the same register -- llvm-mc refuses a self-move as meaningless. Applying it
             * to a scale, a displacement, a branch target or an immediate rewrites a value the
             * solver is measuring: the scale field 0..3 has only four values across eight samples
             * by necessity, so the pass saw "equal" on every repeat and nudged the scale out of
             * its 2-bit field entirely. That is why every LEA_SIB/ADD_SIBD row was dropped with
             * "operand 2 has no field" while its own samples assembled perfectly. */
            for (int i = 0; i < sp->nop; i++) {
                int ki = sp->opsv[i].kind;
                if (ki != RX_R8 && ki != RX_R16 && ki != RX_R32 && ki != RX_R64
                    && ki != RX_MEMBASE && ki != RX_SIBBASE && ki != RX_SIBIDX
                    && ki != RX_OPREG) continue;
                for (int j = i + 1; j < sp->nop; j++) {
                    int kj = sp->opsv[j].kind;
                    if (kj != RX_R8 && kj != RX_R16 && kj != RX_R32 && kj != RX_R64
                        && kj != RX_MEMBASE && kj != RX_SIBBASE && kj != RX_SIBIDX
                        && kj != RX_OPREG) continue;
                    while (vals[i] == vals[j])
                        vals[j] = (vals[j] + 1 > rhi) ? rlo : vals[j] + 1;
                }
            }
            for (int i = 0; i < sp->nop; i++) {
                int kd = sp->opsv[i].kind;
                if (kd == RX_IMM || kd == RX_IMM8 || kd == RX_IMM64 || kd == RX_DISP8
                    || kd == RX_DISP32 || kd == RX_REL8 || kd == RX_REL32 || kd == RX_SIBSCALE)
                    continue;
                if (vals[i] > 7) svals[s][0] = vals[i];   /* remembered per sample below */
            }
            for (int i = 0; i < sp->nop; i++) svals[s][i] = vals[i];
            fill_asm(sp, vals, lines[s], sizeof lines[s]);
            if (bt) snprintf(bt[nline].line, sizeof bt[nline].line, "%s", lines[s]);
            nline++;
        }

        int batched = 0;
        if (bt) batched = batch_assemble(bt, nline, sp->row ? sp->row : "f");
        if (getenv("MKX86_BATCH")) fprintf(stderr, "[batch] %s n=%d got=%d\n", sp->row, nline, batched);

        for (int s = 0; s < nline; s++) {
            long long vals[6];
            uint8_t buf[32];
            int len;
            int any_high = 0;
            for (int i = 0; i < sp->nop; i++) {
                vals[i] = svals[s][i];
                int kd = sp->opsv[i].kind;
                if (kd == RX_IMM || kd == RX_IMM8 || kd == RX_IMM64 || kd == RX_DISP8
                    || kd == RX_DISP32 || kd == RX_REL8 || kd == RX_REL32 || kd == RX_SIBSCALE)
                    continue;
                if (vals[i] > 7) any_high = 1;
            }
            if (bt && bt[s].ok) {
                /* the batch resolved it; for a branch the length correction has to be applied on
                 * top, because the batch's label sits at the instruction and llvm-mc subtracts the
                 * length. A rel operand therefore needs its own single-shot pass. */
                int rel = 0;
                for (int i = 0; i < sp->nop; i++)
                    if (sp->opsv[i].kind == RX_REL8 || sp->opsv[i].kind == RX_REL32) rel = 1;
                if (!rel) {
                    len = bt[s].len;
                    memcpy(buf, bt[s].by, (size_t)(len > 0 ? len : 0));
                } else {
                    len = asm_sample(sp, vals, buf, sizeof buf);
                }
            } else {
                len = asm_sample(sp, vals, buf, sizeof buf);
                if (getenv("MKX86_BATCH")) fprintf(stderr, "[batch-miss] %s |%s|\n", sp->row, lines[s]);
            }
            if (len <= 0 || len > 10) {
                if (getenv("MKX86_DEBUG")) fprintf(stderr, "[fail] %s |%s|\n", sp->row, lines[s]);
                continue;
            }
            if (S->n && len != S->len[0]) continue;
            memcpy(S->by[S->n], buf, (size_t)len);
            S->len[S->n] = len;
            for (int i = 0; i < sp->nop; i++) S->vals[i][S->n] = vals[i];
            S->n++;
            S->seen_high |= (rhi > 7 && any_high);
        }
        (void)batched;
    }
    /* SPLIT DETECTION. A full-range request that ends up holding only low-bank samples has
     * silently dropped every extended-register sample -- and because a row's length is fixed,
     * the row that comes out cannot encode any r8-r15 operand. Requiring "at least 4 samples"
     * was not enough to notice: four low-bank samples satisfied it and the form was emitted
     * without its extended half. Report failure in that case so the caller splits by bank.
     *
     * THIS CHECK APPLIES ONLY TO FORMS THAT HAVE A REGISTER OPERAND. "ret" and "nop" have no
     * operand at all, so no sample can ever be "high", and the check refused them outright --
     * 8 perfectly good samples discarded. */
    if (rlo == 0 && rhi == 15 && !S->seen_high && S->has_reg) return 0;
    return S->n >= 4;
}
static int sample_form(const Spec *sp, Samples *S) { return sample_form_range(sp, S, 0, 15); }

/* Sample with ONE named register operand in the extended bank and the others low. That is the
 * shape where only that operand's REX bit is set, so the row template can fix the others at 0
 * and the resulting row encodes exactly those encodings. */
static int sample_form_mixed(const Spec *sp, Samples *S, int which_high);

static int sample_form_mixed(const Spec *sp, Samples *S, int which_high) {
    memset(S, 0, sizeof *S);
    for (int s = 0; s < 8; s++) {
        long long vals[6];
        for (int i = 0; i < sp->nop; i++) {
            /* ONE BANK PER OPERAND. The operand this call is varying goes to the extended bank
             * and every other register stays low -- that is what makes the REX bits come out
             * separable. Routing through sample_value with the operand's own bank is what lets a
             * SIB base or an opcode-packed register vary in the right bank, instead of the
             * arbitrary walk this used to do (which sampled "(%rsp,%rbp,1)" and got refused). */
            int rlo = (i == which_high) ? 8 : 0;
            int rhi = (i == which_high) ? 15 : 7;
            long long base_v = rlo + ((s * 3 + i) & 7);
            long long sv;
            if (sample_value(sp->opsv[i].kind, s, i, rlo, rhi, base_v, &sv)) { vals[i] = sv; continue; }
            vals[i] = base_v;
        }
        /* keep the two register operands distinct so no encoding is a no-op llvm-mc refuses */
        for (int i = 0; i < sp->nop; i++)
            for (int j = i + 1; j < sp->nop; j++) {
                int ki = sp->opsv[i].kind, kj = sp->opsv[j].kind;
                if (ki != RX_R8 && ki != RX_R16 && ki != RX_R32 && ki != RX_R64
                    && ki != RX_MEMBASE && ki != RX_SIBBASE && ki != RX_SIBIDX
                    && ki != RX_OPREG) continue;
                if (kj != RX_R8 && kj != RX_R16 && kj != RX_R32 && kj != RX_R64
                    && kj != RX_MEMBASE && kj != RX_SIBBASE && kj != RX_SIBIDX
                    && kj != RX_OPREG) continue;
                while (vals[i] == vals[j]) {
                    vals[j]++;
                    if (vals[j] > 15) vals[j] = (vals[j] & 8) ? 8 : 0;
                }
                /* rsp(4) as a SIB index means "no index register" and as a ModR/M base means "a
                 * SIB byte follows" -- a different instruction in both cases, so it must never be
                 * sampled there. The check is on the LOW THREE BITS, so it applies in either bank.
                 * The mixed sampler needs this too: without it "addl (%rbx,%rsp,2), %r9d" was
                 * sampled and llvm-mc refused it. */
                if (kj == RX_SIBIDX || kj == RX_MEMBASE) {
                    if ((vals[j] & 7) == 4) vals[j] = (vals[j] & 8) | 5;
                }
                if (ki == RX_SIBIDX || ki == RX_MEMBASE) {
                    if ((vals[i] & 7) == 4) vals[i] = (vals[i] & 8) | 5;
                }
            }
        char line[256];
        fill_asm(sp, vals, line, sizeof line);
        uint8_t buf[32];
        int len = asm_sample(sp, vals, buf, sizeof buf);
        if (len <= 0 || len > 10) continue;
        if (S->n && len != S->len[0]) continue;
        memcpy(S->by[S->n], buf, (size_t)len);
        S->len[S->n] = len;
        for (int i = 0; i < sp->nop; i++) S->vals[i][S->n] = vals[i];
        S->n++;
    }
    return S->n >= 3;
}

/* ============================================================ emit ===== */

static int emit_row(FILE *o, const Spec *sp, const Samples *S) {
    /* BUILD FIRST, WRITE ON SUCCESS. The first version streamed the row out as it went, so a
     * row whose second operand had no field was already half-written -- and half a row is a
     * row that matches words and encodes them wrong, the exact failure the drop rule exists
     * to prevent. */
    char rowbuf[1024];
    FILE *o_save = o;
    o = fmemopen(rowbuf, sizeof rowbuf, "w");
    if (!o) return 0;

    int len = S->len[0];
    Run m[6], h[6];
    int uh[6], got[6], rexw[6];
    for (int i = 0; i < sp->nop; i++) {
        memset(&m[i], 0, sizeof m[i]); memset(&h[i], 0, sizeof h[i]);
        uh[i] = got[i] = rexw[i] = 0;
        got[i] = solve_op(sp, S, i, &m[i], &h[i], &uh[i]);
    }
    /* REX.W: the bit that selects 64-bit operand size. It is constant across the samplings of
     * one form (the form IS either 64-bit or 32-bit), so it sits in the template -- but it
     * must be RECOGNISED, because a row that claims a 64-bit form with W=0 encodes wrong. */
    for (int i = 0; i < sp->nop && i < 6; i++) (void)rexw[i];

    /* Emit the template as a PLAIN initializer list padded to ISA_MAXBYTES. W(x) is a
     * fixed-arity macro for one 32-bit value; an x86 instruction can be 10+ bytes, so a
     * single W(...) cannot express it -- and silently truncating to 4 bytes would make every
     * long instruction match the wrong length. */
    /* The operands string is DERIVED, not copied from the spec. A row that says
     * "r64_rm, r64_reg" while its first field sits at ModR/M.reg describes its operands in the
     * wrong order -- anyone reading the catalog would supply them backwards and get a
     * plausible wrong instruction. Build the description from the solved positions. */
    {
        char ops[96] = "";
        for (int i = 0; i < sp->nop; i++) {
            char one[24];
            if (!got[i]) snprintf(one, sizeof one, "imm");
            else if (sp->opsv[i].kind == RX_IMM || sp->opsv[i].kind == RX_IMM8
                     || sp->opsv[i].kind == RX_IMM64) {
                /* AN IMMEDIATE IS AN IMMEDIATE WHEREVER ITS BITS SIT -- and there are THREE
                 * immediate kinds, not two. Omitting RX_IMM64 here described a 64-bit constant
                 * as "r8_reg", a register, while the encoder handled it correctly: the row
                 * encoded the right bytes and said the wrong thing about them. */
                snprintf(one, sizeof one, "imm%d", sp->opsv[i].kind == RX_IMM64 ? 64 : m[i].wid);
            } else {
                /* reg when the field is at bit 3 of its byte (ModR/M.reg), rm at bit 0 */
                const char *pos = (m[i].bit == 3 && m[i].wid == 3) ? "reg" :
                                  (m[i].bit == 0 && m[i].wid == 3) ? "rm" : "reg";
                int w = 8;
                switch (sp->opsv[i].kind) {
                    case RX_R64: w = 64; break; case RX_R32: w = 32; break;
                    case RX_R16: w = 16; break; default: w = 8; break;
                }
                snprintf(one, sizeof one, "r%d_%s", w, pos);
            }
            if (i) strncat(ops, ", ", sizeof ops - strlen(ops) - 1);
            strncat(ops, one, sizeof ops - strlen(ops) - 1);
        }
        fprintf(o, "{ \"%s\", \"%s\", \"%s\", %d, {", sp->row, sp->mnem, ops, len);
    }
    /* THE TEMPLATE IS COMPUTED AFTER THE FIELDS ARE KNOWN, and every bit a field covers is
     * CLEARED from it. Computing the base first and clearing nothing left bits of a varying
     * operand in the template whenever all samples happened to share them -- and a template bit
     * that is really part of a field makes the row refuse every value that does not repeat that
     * exact bit. That is how the movabs row ended up with 01,00,01,00 in its immediate bytes
     * and matched nothing at all.
     *
     * The rule: a template bit is one that is identical across ALL samples AND that no solved
     * field claims. */
    uint8_t field_mask[16];
    memset(field_mask, 0, sizeof field_mask);
    for (int i = 0; i < sp->nop; i++) {
        if (!got[i]) continue;
        if (m[i].wid > 0)
            for (int b = 0; b < m[i].wid; b++) {
                int abs = m[i].byte * 8 + m[i].bit + b;
                field_mask[abs / 8] |= (uint8_t)(1u << (abs % 8));
            }
        if (uh[i] == 1)
            field_mask[h[i].byte] |= (uint8_t)(1u << h[i].bit);
        if (uh[i] == 2 && m[i].wid > 0)
            for (int b = 0; b < 32; b++) {
                int abs = h[i].byte * 8 + b;
                field_mask[abs / 8] |= (uint8_t)(1u << (abs % 8));
            }
    }
    uint8_t base[16];
    for (int i = 0; i < ISA_MAXBYTES; i++) base[i] = 0;
    for (int i = 0; i < len; i++) {
        uint8_t same = 0xFF;
        for (int s = 1; s < S->n; s++) same &= (uint8_t)~(S->by[0][i] ^ S->by[s][i]);
        base[i] = (uint8_t)(S->by[0][i] & same & (uint8_t)~field_mask[i]);
    }
    for (int i = 0; i < ISA_MAXBYTES; i++)
        fprintf(o, "%s0x%02x", i ? "," : "", i < len ? base[i] : 0);
    fprintf(o, "},\n  ");
    for (int pass = 0; pass < 4; pass++) {
        fprintf(o, "{");
        for (int i = 0; i < sp->nop; i++) {
            int val = 0;
            if (got[i]) {
                switch (pass) {
                    case 0: val = m[i].byte; break;
                    case 1: val = m[i].bit;  break;
                    case 2: val = m[i].wid;  break;
                    case 3: val = 0; break;      /* vsc: none of these forms need a scale */
                }
            }
            fprintf(o, "%s%d", i ? "," : "", val);
        }
        fprintf(o, "},\n  ");
    }
    /* vkind */
    fprintf(o, "{");
    for (int i = 0; i < sp->nop; i++) {
        int k = K_REG;
        if (!got[i]) k = K_IMM;
        /* BOTH immediate kinds map to K_IMM. Testing only RX_IMM left every RX_IMM8 operand
         * declared as K_REG -- so the catalog called an 8-bit shift count a register, and the
         * decoded operands then read "imm3, r64_rm" with the immediate flagged as a register
         * position. The encoded bytes were right and the description was wrong, which is the
         * combination that misleads whoever reads the table next. */
        else if (sp->opsv[i].kind == RX_IMM || sp->opsv[i].kind == RX_IMM8) k = K_IMM;
        else if (uh[i] == 2) k = K_SPLIT;      /* a 64-bit immediate: two 32-bit halves */
        else if (uh[i]) k = K_REXREG;
        fprintf(o, "%s%d", i ? "," : "", k);
    }
    fprintf(o, "},\n  ");
    /* FIELD ORDER MATTERS: nvars comes BEFORE the per-operand windows, because that is where
     * the struct places it and every row in the tree is a plain brace initializer. Emitting the
     * windows first put the row's operand count into hbyte and zeroed the count -- every row
     * then had nvars=0 and no operands at all. */
    {
        int sb = 0, sbw = 0;
        for (int i = 0; i < sp->nop; i++)
            if (uh[i] == 2) { sb = h[i].byte; sbw = 32; }
        fprintf(o, "  %d,0,%d, %d,\n  {", sb, sbw, sp->nop);
    }
    for (int i = 0; i < sp->nop; i++) fprintf(o, "%s%d", i ? "," : "", (got[i] && uh[i]) ? h[i].byte : 0);
    fprintf(o, "},\n  {");
    /* hbit stores bit+1 so that 0 means "no window" -- a high bit genuinely at position 0
     * would otherwise be indistinguishable from "absent" and silently drop REX.B. */
    for (int i = 0; i < sp->nop; i++) fprintf(o, "%s%d", i ? "," : "", (got[i] && uh[i]) ? h[i].bit + 1 : 0);
    fprintf(o, "} },\n");
    for (int i = 0; i < sp->nop; i++)
        if (!got[i]) {
            /* A ROW WHOSE OPERANDS CANNOT BE SOLVED IS NOT EMITTED. Emitting it with a zero
             * field would produce a row that matches words and then encodes them wrong --
             * the failure mode this catalog exists to avoid. Reported instead. */
            fprintf(stderr, "  !! DROPPED %s: operand %d has no field (n=%d len=%d vals:", sp->row, i, S->n, S->len[0]);
            for (int z = 0; z < S->n; z++) {
                fprintf(stderr, " v%lld=[", S->vals[i][z]);
                for (int q = 0; q < S->len[z]; q++) fprintf(stderr, "%02x", S->by[z][q]);
                fprintf(stderr, "]");
            }
            fprintf(stderr, ")\n");
            fclose(o);
            return 0;
        }
    fclose(o);
    fputs(rowbuf, o_save);
    return 1;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "check";
    cache_load();
    atexit(cache_save);
    build_specs();

    if (!strcmp(cmd, "fmts")) {
        for (int i = 0; i < g_nspec; i++)
            printf("%-26s nop=%d fmt=|%s|\n", g_spec[i].row, g_spec[i].nop, g_spec[i].fmt);
        return 0;
    }
    if (!strcmp(cmd, "spec")) {
        for (int i = 0; i < g_nspec; i++)
            printf("%-26s %-7s %s\n", g_spec[i].row, g_spec[i].mnem, g_spec[i].ops);
        return 0;
    }
    if (!strcmp(cmd, "probelo")) {
        const char *want = argc > 2 ? argv[2] : NULL;
        for (int i = 0; i < g_nspec; i++) {
            if (want && strcmp(want, g_spec[i].row)) continue;
            Samples S;
            int ok = sample_form_range(&g_spec[i], &S, 0, 7);
            printf("%-26s %s (%d samples, len %d)\n", g_spec[i].row, ok ? "OK" : "REFUSED", S.n, S.n ? S.len[0] : 0);
            for (int s = 0; s < S.n; s++) {
                printf("    ");
                for (int b = 0; b < S.len[s]; b++) printf("%02x", S.by[s][b]);
                printf("\n");
            }
            if (want) return ok ? 0 : 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "probehi")) {
        const char *want = argc > 2 ? argv[2] : NULL;
        for (int i = 0; i < g_nspec; i++) {
            if (want && strcmp(want, g_spec[i].row)) continue;
            Samples S;
            int ok = sample_form_range(&g_spec[i], &S, 8, 15);
            printf("%-26s %s (%d samples, len %d)\n", g_spec[i].row, ok ? "OK" : "REFUSED", S.n, S.n ? S.len[0] : 0);
            for (int s = 0; s < S.n; s++) {
                printf("    ");
                for (int b = 0; b < S.len[s]; b++) printf("%02x", S.by[s][b]);
                printf("\n");
            }
            if (want) return ok ? 0 : 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "probe")) {
        const char *want = argc > 2 ? argv[2] : NULL;
        for (int i = 0; i < g_nspec; i++) {
            if (want && strcmp(want, g_spec[i].row)) continue;
            Samples S;
            int ok = sample_form(&g_spec[i], &S);
            printf("%-26s %s (%d samples, len %d)\n", g_spec[i].row,
                   ok ? "OK" : "REFUSED", S.n, S.n ? S.len[0] : 0);
            for (int s = 0; s < S.n; s++) {
                printf("    ");
                for (int b = 0; b < S.len[s]; b++) printf("%02x", S.by[s][b]);
                printf("   ");
                long long v[6]; for (int k = 0; k < g_spec[i].nop; k++) v[k] = S.vals[k][s];
                char line[256]; fill_asm(&g_spec[i], v, line, sizeof line);
                printf("%s\n", line);
            }
            if (want) return ok ? 0 : 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "check")) {
        int ok = 0, bad = 0;
        for (int i = 0; i < g_nspec; i++) {
            Samples S;
            if (sample_form(&g_spec[i], &S)) { ok++; continue; }
            bad++;
            char line[256]; long long v[6] = {0, 1, 0, 1, 0, 1};
            for (int k = 0; k < g_spec[i].nop; k++) v[k] = (k == 1) ? 1 : 0;
            fill_asm(&g_spec[i], v, line, sizeof line);
            printf("REFUSED %-24s |%s|\n", g_spec[i].row, line);
        }
        printf("--- assemblable %d, refused %d\n", ok, bad);
        return bad ? 1 : 0;
    }
    if (!strcmp(cmd, "emit")) {
        printf("/* ISA_X86_64.inc -- GENERATED by mkx86 from llvm-mc ground truth.\n"
               " *\n"
               " * Do not hand-edit. Regenerate with `mkx86 emit > ISA_X86_64.inc`, then run\n"
               " * `mkx86 check` and the fixture check. Every row was derived by assembling\n"
               " * several instances of the form and solving for the field positions, so the\n"
               " * bytes are a measurement of the ISA rather than a recollection of it.\n"
               " *\n"
               " * x86-64 is a byte stream, not a fixed word: an optional REX prefix\n"
               " * (0x40 | W<<3 | R<<2 | X<<1 | B), the opcode, then ModR/M. A register above 7\n"
               " * sets the REX bit for its operand position, so a row whose register does not\n"
               " * fit its main run uses K_REXREG: the low bits in ModR/M, the high bit in the\n"
               " * field named by the row's split window.\n"
               " *\n"
               " * Register numbers: 0..7 are the classic registers, 8..15 the r8-r15 bank.\n"
               " */\n"
               "static const IsaForm isa_forms_x86_64[] = {\n\n");
        int ok = 0;
        for (int i = 0; i < g_nspec; i++) {
            Samples S;
            /* Ask the low bank FIRST. If it encodes differently from the mixed set (different
             * length OR a different template byte), the form genuinely needs two rows -- one
             * whose registers are 0..7 and one for 8..15 -- because a row's template and length
             * are both fixed. Deciding this from "the mixed samples had differing lengths" was
             * wrong: with a high operand present in every mixed sample all lengths agree, no
             * split is taken, and the single row that comes out has REX baked into its template
             * and matches only extended encodings -- so the plain-register form finds no row. */
            /* count register operands: with two of them the mixed shape splits further, because
             * the REX byte's R and B bits are independent and a row's template fixes whichever
             * one is constant. */
            int nreg = 0;
            for (int k = 0; k < g_spec[i].nop; k++)
                if (g_spec[i].opsv[k].kind != RX_IMM && g_spec[i].opsv[k].kind != RX_IMM8) nreg++;
            (void)nreg;
            Samples LOW;
            int have_low = sample_form_range(&g_spec[i], &LOW, 0, 7);

            /* DECIDE "DOES THIS FORM SPLIT?" FROM THE SPEC, NOT FROM A SECOND SAMPLE BATCH.
             *
             * A form splits when one of its register operands is above 7, because that sets a
             * REX bit and adds a byte -- so its length differs between the banks. The old code
             * assembled the WHOLE 0..15 range (a full batch of eight llvm-mc samples) purely to
             * compare its first encoding against the low bank's, and threw that batch away in
             * every splitting case, which is most of the catalog. The spec already says which
             * operands can reach the extended bank, so the answer is known without asking.
             *
             * A form splits iff at least one operand has a register field (a plain register, a
             * memory base, a SIB base/index, or an opcode-packed register). A form with no such
             * operand (ret, nop, a branch) is one row and never splits -- and for those, LOW is
             * the whole answer. */
            int can_split = 0;
            for (int k = 0; k < g_spec[i].nop; k++) {
                int kd = g_spec[i].opsv[k].kind;
                if (kd == RX_R8 || kd == RX_R16 || kd == RX_R32 || kd == RX_R64
                    || kd == RX_MEMBASE || kd == RX_SIBBASE || kd == RX_SIBIDX
                    || kd == RX_OPREG)
                    can_split = 1;
            }
            int mixed_ok = have_low;
            int needs_split = 0;
            if (can_split) {
                /* Only a form with a register operand can differ between banks, so only those
                 * need the full-range sample. */
                Samples S15;
                mixed_ok = sample_form_range(&g_spec[i], &S15, 0, 15);
                if (have_low && mixed_ok) {
                    S = S15;
                    if (LOW.len[0] != S15.len[0]) needs_split = 1;
                    else for (int b = 0; b < LOW.len[0]; b++)
                        if (LOW.by[0][b] != S15.by[0][b]) { needs_split = 1; break; }
                } else if (mixed_ok) {
                    S = S15;
                }
            } else if (have_low) {
                S = LOW;
            }
            (void)can_split;
            if (needs_split) {
                /* FOUR SHAPES. With two register operands the REX byte's R and B bits are
                 * independent, and a row's template fixes whichever is constant -- so the
                 * encoding has four shapes, not two:
                 *     both low    no REX            01c1
                 *     op0 high    REX.R only        4401c1
                 *     op1 high    REX.B only        4501c1
                 *     both high   REX.R + REX.B     4d01c1
                 * Emitting only the both-low and both-high rows leaves every mixed case with no
                 * row at all ("cmpq %r8, %rcx" was missing for exactly this reason).
                 *
                 * THE LOW BANK IS ALREADY SAMPLED. `LOW` was collected a few lines up to decide
                 * whether a split was needed, and this path used to call sample_form_range again
                 * with 0..7 -- a second batch of eight llvm-mc samples whose result is identical
                 * to the one in hand. Reusing LOW halves the processes for every form that
                 * splits, which is most of the catalog. */
                Samples H;
                int gh = sample_form_range(&g_spec[i], &H, 8, 15);
                if (have_low && emit_row(stdout, &g_spec[i], &LOW)) ok++;
                if (gh) {
                    Spec ext = g_spec[i];
                    char nm[80];
                    snprintf(nm, sizeof nm, "%s_X8", g_spec[i].row);
                    ext.row = nm;
                    if (emit_row(stdout, &ext, &H)) ok++;
                }
                /* the mixed shapes, one per REGISTER operand position */
                for (int hi = 0; hi < g_spec[i].nop; hi++) {
                    int hk = g_spec[i].opsv[hi].kind;
                    /* only a kind that HAS a register field can be varied into the extended bank;
                     * an immediate or a displacement has no register bit to set */
                    if (hk == RX_IMM || hk == RX_IMM8 || hk == RX_IMM64
                        || hk == RX_DISP8 || hk == RX_DISP32 || hk == RX_REL8 || hk == RX_REL32
                        || hk == RX_SIBSCALE)
                        continue;
                    Samples M;
                    int gm = sample_form_mixed(&g_spec[i], &M, hi);
                    if (!gm) continue;
                    Spec ext = g_spec[i];
                    char nm[80];
                    snprintf(nm, sizeof nm, "%s_X%d", g_spec[i].row, hi);
                    ext.row = nm;
                    if (emit_row(stdout, &ext, &M)) ok++;
                }
                continue;
            }
            if (!mixed_ok) {
                /* the mixed set failed for some other reason: fall back to the low bank only,
                 * and say so rather than emitting nothing */
                Samples L, H;
                /* THE SPLIT BOUNDARY DEPENDS ON THE OPERAND WIDTH. For 32- and 64-bit
                 * operands the REX byte appears at register 8, so the banks are 0..7 / 8..15.
                 * For an 8-BIT operand it appears at 4 (spl,bpl,sil,dil need REX; al,cl,dl,bl do
                 * not), so sampling 0..7 mixes two lengths in one row and the template acquires
                 * a spurious 0x40 -- a 4-byte SETE_8 row that matches nothing at all. */
                int eightbit = 0;
                for (int k = 0; k < g_spec[i].nop; k++)
                    if (g_spec[i].opsv[k].kind == RX_R8) eightbit = 1;
                int gl = sample_form_range(&g_spec[i], &L, 0, eightbit ? 3 : 7);
                int gh = sample_form_range(&g_spec[i], &H, eightbit ? 4 : 8, 15);
                if (gl && emit_row(stdout, &g_spec[i], &L)) ok++;
                if (gh) {
                    Spec ext = g_spec[i];
                    char nm[80];
                    snprintf(nm, sizeof nm, "%s_X8", g_spec[i].row);
                    ext.row = nm;
                    if (emit_row(stdout, &ext, &H)) ok++;
                }
                continue;
            }
            if (emit_row(stdout, &g_spec[i], &S)) ok++;
        }
        /* THE EPILOGUE. It went missing once because an edit matched the same printf text
         * that emit_row used for its row output and replaced it -- so the file was written
         * without its closing brace and without the row count, and the only symptom was a
         * compile error pointing at a line in isadb.c far from the cause. */
        printf("};\n#define ISA_NFORMS_X86_64 %d\n", ok);
        fprintf(stderr, "mkx86: %d rows emitted of %d forms\n", ok, g_nspec);
        return 0;
    }
    if (!strcmp(cmd, "fixtures")) {
        /* FIXTURES FROM THE ORACLE. Each line is one sampled instance that llvm-mc encoded:
         * the row, the operand values, the bytes it produced. The fixture list therefore
         * cannot go stale against the catalog -- regenerating the catalog regenerates them --
         * and it is ground truth from an independent assembler, which is the whole point. */
        int n = 0;
        for (int i = 0; i < g_nspec; i++) {
            Samples S;
            if (!sample_form_range(&g_spec[i], &S, 0, 15)) continue;
            if (S.n < 3) continue;
            /* use the first sampling: a real, distinct operand set */
            long long v[6];
            for (int k = 0; k < g_spec[i].nop; k++) v[k] = S.vals[k][0];
            /* NAME THE ROW THAT ENCODES THIS SAMPLING. A form with extended registers splits
             * into several rows (_X8 for the extended bank, _X<k> for the mixed shapes), and a
             * fixture that names the base row for an extended-register instance fails with
             * "value 8 does not fit 3 bits" -- which reads like a broken row and is only a
             * mislabelled fixture. */
            const char *rowname = g_spec[i].row;
            char rn[80];
            int anyhi = 0;
            for (int k = 0; k < g_spec[i].nop; k++) {
                int kd = g_spec[i].opsv[k].kind;
                if (kd != RX_IMM && kd != RX_IMM8 && kd != RX_IMM64 && v[k] > 7) anyhi = 1;
            }
            if (anyhi) {
                int allhi = 1;
                for (int k = 0; k < g_spec[i].nop; k++) {
                    int kd = g_spec[i].opsv[k].kind;
                    if (kd != RX_IMM && kd != RX_IMM8 && kd != RX_IMM64 && v[k] <= 7) allhi = 0;
                }
                if (allhi) { snprintf(rn, sizeof rn, "%s_X8", g_spec[i].row); rowname = rn; }
                else {
                    /* a mixed shape: exactly one register operand is in the extended bank */
                    for (int k = 0; k < g_spec[i].nop; k++) {
                        int kd = g_spec[i].opsv[k].kind;
                        if (kd != RX_IMM && kd != RX_IMM8 && kd != RX_IMM64 && v[k] > 7) {
                            snprintf(rn, sizeof rn, "%s_X%d", g_spec[i].row, k);
                            rowname = rn;
                            break;
                        }
                    }
                }
            }
            char line[256];
            fill_asm(&g_spec[i], v, line, sizeof line);
            printf("{ \"x86_64\", \"%s\", {", rowname);
            for (int k = 0; k < g_spec[i].nop; k++) printf("%s%lld", k ? "," : "", v[k]);
            printf("}, %d, {", g_spec[i].nop);
            for (int b = 0; b < S.len[0]; b++) printf("%s0x%02x", b ? "," : "", S.by[0][b]);
            printf("}, %d, \"%s\" },\n", S.len[0], line);
            n++;
        }
        fprintf(stderr, "mkx86: %d fixtures\n", n);
        return 0;
    }
    fprintf(stderr, "usage: mkx86 emit|check|probe <form>|spec|fixtures\n");
    return 2;
}
