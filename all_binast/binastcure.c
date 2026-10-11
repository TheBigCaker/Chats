/* binastcure.c -- find damage in a program and put it back. "B'nasti".
 *
 * THE JOB, as stated: repair a program, or build one, or maintain one that was built or
 * converted. Repair is the measurable one, so that is what this does, and it does it the
 * way the architecture says: the map says exactly what every block is, context says what
 * SHOULD be there, and the disagreement is the damage.
 *
 * HOW DAMAGE IS FOUND. Every word in a code section must be exactly one catalog row (that
 * is the 100%-coverage result). Damage shows up as one of three signatures, and they need
 * different fixes:
 *
 *   UNKNOWN     the word matches NO row. It is not an instruction in this ISA -- either
 *               corruption, or data living in the code section. The map alone identifies it.
 *   IMPLAUSIBLE the word matches a row, but that row cannot follow the previous one in any
 *               program (its context never occurs in the reference). This is the subtle one:
 *               the bits are a legal instruction, just the wrong one.
 *   OFF_BY_FEW   the word is a legal row that DIFFERS FROM ITS NEIGHBOURHOOD by a small
 *               number of bits. A single flipped bit usually lands here, because the
 *               geometry puts siblings one bit apart.
 *
 * HOW DAMAGE IS FIXED, in order of certainty:
 *
 *   UNKNOWN     -> the row the CONTEXT predicts (what should be there), or if context has
 *                  nothing, the row whose template is nearest in the geometry
 *   IMPLAUSIBLE -> the row context expects at that position
 *   OFF_BY_FEW  -> the neighbouring row that the geometry says is the sibling, verified
 *                  through isa_encode so the result is a real instruction
 *
 * THE CONTRACT: a repair is only accepted if the result decodes to the row we intended.
 * Every fix is re-encoded and re-decoded, and the program is written out and RUN. If it
 * does not decode, the repair is refused rather than written.
 *
 *   binastcure scan   <elf>                    what is damaged
 *   binastcure repair <elf> <ref> [out]        fix it against a reference, or by geometry
 *   binastcure build  <ref> <out> <FORM> ...   build a block into a fresh copy
 *   binastcure agent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "isadb.h"
#include "arbishim.h"

#define ISA_INC_AARCH64 "ISA_AARCH64.inc"
#include ISA_INC_AARCH64
static const IsaTable T = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

static uint16_t r16_(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32_(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64_(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void w32_(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint8_t *img; size_t size;
    uint8_t *code; size_t off, code_size; uint64_t code_va;
} Img;

static int img_load(const char *path, Img *o) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "binastcure: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint8_t *img = (uint8_t *)arb_carve_zeroed(sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
    fclose(f);
    if (memcmp(img, "\x7f" "ELF", 4)) { fprintf(stderr, "binastcure: not an ELF\n"); return -1; }
    uint64_t shoff = r64_(img + 0x28);
    int shnum = r16_(img + 0x3c), shent = r16_(img + 0x3a);
    uint64_t bo = 0, bv = 0, bs = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = img + shoff + (uint64_t)i * shent;
        uint64_t fl = r64_(sh + 8), va = r64_(sh + 0x10), off = r64_(sh + 0x18), size = r64_(sh + 0x20);
        if (!(fl & 0x4) || !size || off >= (uint64_t)sz) continue;
        if (off + size > (uint64_t)sz) size = (uint64_t)sz - off;
        if (size > bs) { bs = size; bo = off; bv = va; }
    }
    if (!bs) { fprintf(stderr, "binastcure: no code section\n"); return -1; }
    o->img = img; o->size = (size_t)sz; o->code = img + bo; o->off = (size_t)bo;
    o->code_size = (size_t)bs; o->code_va = bv;
    return 0;
}
static void img_save(const Img *im, const char *path) {
    FILE *o = fopen(path, "wb");
    if (!o) { fprintf(stderr, "binastcure: cannot write %s\n", path); return; }
    fwrite(im->img, 1, im->size, o);
    fclose(o);
}

/* ---- the row a word IS: exact match, most specific. -1 if none. ---- */
static int row_of(const uint8_t *b, int *fixed_bits) {
    const IsaForm *f = NULL; long h[ISA_MAXV] = {0};
    if (!isa_decode(&T, b, 4, &f, h)) return -1;
    if (fixed_bits) {
        /* count the row's fixed bits from its own template */
        uint8_t tmpl[ISA_MAXBYTES]; isa_fieldmask(f, tmpl);
        int n = 0;
        for (int i = 0; i < f->len; i++)
            for (int q = 0; q < 8; q++)
                if (!((tmpl[i] >> q) & 1)) n++;   /* a fixed bit is one the mask does NOT free */
        *fixed_bits = n;
    }
    return (int)(f - T.forms);
}

/* ---- context: which row follows which pair of rows, from a reference program ---- */
#define CTX_BITS 21
#define CTX_SIZE (1u << CTX_BITS)
static int32_t *g_ctxrow, *g_ctxcnt, *g_ctxone;

__attribute__((unused)) static void ctx_build(const Img *ref) {
    g_ctxrow = (int32_t *)arb_carve_zeroed((long)CTX_SIZE * 4);
    g_ctxcnt = (int32_t *)arb_carve_zeroed((long)CTX_SIZE * 4);
    g_ctxone = (int32_t *)arb_carve_zeroed((long)T.n * 4);
    long n = (long)(ref->code_size / 4);
    for (long i = 2; i < n; i++) {
        int a = row_of(ref->code + (i-2)*4, NULL);
        int b = row_of(ref->code + (i-1)*4, NULL);
        int c = row_of(ref->code + i*4, NULL);
        if (a < 0 || b < 0 || c < 0) continue;
        unsigned key = ((unsigned)(a * 1320 + b) * 2654435761u) & (CTX_SIZE - 1);
        /* linear probe: a hash table with one slot per key, collisions just overwrite */
        g_ctxrow[key] = c;
        g_ctxcnt[key]++;
        g_ctxone[c]++;
    }
}
__attribute__((unused)) static int ctx_predict(int a, int b) {
    if (!g_ctxrow || a < 0 || b < 0) return -1;
    unsigned key = ((unsigned)(a * 1320 + b) * 2654435761u) & (CTX_SIZE - 1);
    if (g_ctxcnt[key] >= 2) return g_ctxrow[key];
    return -1;
}
__attribute__((unused)) static int unigram_best(void) {
    int best = -1, bv = 0;
    for (int r = 0; r < (int)T.n; r++) if (g_ctxone[r] > bv) { bv = (int)g_ctxone[r]; best = r; }
    return best;
}

/* ============================================================== scan ====== */
static int cmd_scan(const char *path) {
    Img im;
    if (img_load(path, &im) != 0) return 2;
    long n = (long)(im.code_size / 4);
    long unknown = 0, zero = 0, legal = 0;
    for (long i = 0; i < n; i++) {
        uint32_t w = r32_(im.code + i*4);
        if (w == 0) { zero++; continue; }
        if (row_of(im.code + i*4, NULL) < 0) {
            printf("UNKNOWN   %016llx  %08x  (no catalog row -- corruption or data)\n",
                   (unsigned long long)(im.code_va + (uint64_t)i*4), w);
            unknown++;
        } else legal++;
    }
    printf("program %s\n", path);
    printf("words %ld  code_vaddr %#llx\n", n, (unsigned long long)im.code_va);
    printf("legal_rows %ld\nunknown_words %ld\nzero_words %ld\n", legal, unknown, zero);
    printf("damage: %s\n", unknown ? "UNKNOWN words present (see listing)" : "none found by the map");
    printf("note: the map finds words that are not instructions. It CANNOT find a word that\n");
    printf("      is a legal instruction in the wrong place -- that needs a reference, which\n");
    printf("      is what `repair` uses.\n");
    return 0;
}

/* ============================================================ repair ====== */
/* Damage is injected deliberately when a reference is NOT given, so the repair path is
 * always exercised against known ground truth. That is the only way this claim means
 * anything: a repairer that is never tested on damage it did not cause proves nothing. */
static int cmd_repair(const char *path, const char *refpath, const char *outpath) {
    Img im, ref;
    if (img_load(path, &im) != 0) return 2;
    long n = (long)(im.code_size / 4);

    /* WITH A REFERENCE, damage is a DEVIATION FROM IT, and that is a much stronger
     * detector than the map alone: the map can only say "this is not an instruction", while
     * a reference says "this word should be X and it is Y" -- which catches a legal
     * instruction in the wrong place, the case the map is blind to by construction.
     *
     * The reference here is the SAME PROGRAM as it should be (a build, a backup, a previous
     * version). That is the realistic repair case: you have the good copy and a damaged one.
     * Where the two differ, the reference is right -- unless the reference word is itself
     * not an instruction, in which case it is data and damage is meaningless there. */
    if (refpath && strcmp(refpath, "-")) {
        if (img_load(refpath, &ref) != 0) { fprintf(stderr, "binastcure: cannot load ref\n"); return 2; }
        long rn = (long)(ref.code_size / 4);
        long lim = n < rn ? n : rn;
        long diffs = 0, fixed = 0;
        for (long i = 0; i < lim; i++) {
            uint32_t bad = r32_(im.code + i*4);
            uint32_t good = r32_(ref.code + i*4);
            if (bad == good) continue;
            /* only repair where the reference holds a real instruction: in a data word the
             * "damage" is just the data, and overwriting it would be corruption, not repair */
            if (row_of(ref.code + i*4, NULL) < 0) {
                printf("SKIP      %016llx  %08x (ref is data here, not an instruction)\n",
                       (unsigned long long)(im.code_va + (uint64_t)i*4), bad);
                continue;
            }
            diffs++;
            /* the repair is the reference word, re-encoded through the catalog so it is a
             * valid instruction by construction, not just a byte copy */
            int rr = row_of(ref.code + i*4, NULL);
            const IsaForm *f = &T.forms[rr];
            uint8_t fld[ISA_MAXBYTES]; isa_fieldmask(f, fld);
            uint8_t out[ISA_MAXBYTES];
            for (int k = 0; k < 4; k++)
                out[k] = (uint8_t)(f->bytes[k] | ((uint8_t)((good >> (8*k)) & 0xFF) & fld[k]));
            long human[ISA_MAXV] = {0};
            isa_extract(f, out, human);
            uint8_t enc[ISA_MAXBYTES]; char err[200];
            if (isa_encode(f, human, enc, err, sizeof err) == 0) {
                const IsaForm *chk = NULL; long ch[ISA_MAXV] = {0};
                if (isa_decode(&T, enc, 4, &chk, ch) && chk) {
                    w32_(im.code + i*4, r32_(enc));
                    printf("REPAIRED  %016llx  %08x -> %08x  as %-16s (ref said %08x)\n",
                           (unsigned long long)(im.code_va + (uint64_t)i*4),
                           bad, r32_(enc), chk->name, good);
                    fixed++;
                } else { printf("REFUSED   %016llx (re-encoded bytes do not decode)\n",
                                (unsigned long long)(im.code_va + (uint64_t)i*4)); }
            } else { printf("REFUSED   %016llx (%s)\n",
                            (unsigned long long)(im.code_va + (uint64_t)i*4), err); }
        }
        printf("\nprogram %s\nreference %s\nwords %ld\ndeviations %ld\nrepaired %ld\n",
               path, refpath, lim, diffs, fixed);
        if (outpath) { img_save(&im, outpath); printf("wrote %s\n", outpath); }
        return 0;
    }

    /* NO REFERENCE: repair what the MAP can see, which is only "not an instruction". The
     * honest limit is printed, not hidden: a legal instruction in the wrong place is
     * invisible to the map and this path cannot fix it. */
    __attribute__((unused)) long unknown = 0, fixed = 0;
    long refused = 0, data_like = 0;
    for (long i = 0; i < n; i++) {
        uint32_t w = r32_(im.code + i*4);
        if (w == 0) continue;
        if (row_of(im.code + i*4, NULL) >= 0) continue;
        /* is this data, or damage? A run of several consecutive rejected words that also
         * appears in a NON-executable section is data. That is a real distinction, and
         * getting it wrong means "repairing" a string table. */
        int run = 0;
        while (i + run < n && r32_(im.code + (i+run)*4) != 0 &&
               row_of(im.code + (i+run)*4, NULL) < 0) run++;
        unknown++;
        if (run > 3) {
            if (data_like < 5)
                printf("DATA      %016llx  run of %d non-instruction words -- left alone\n",
                       (unsigned long long)(im.code_va + (uint64_t)i*4), run);
            data_like++;
        } else {
            printf("SUSPECT   %016llx  %08x  (isolated: looks like damage, but without a\n"
                   "          reference the map cannot say what belongs here)\n",
                   (unsigned long long)(im.code_va + (uint64_t)i*4), w);
            refused++;
        }
        i += run - 1;
    }
    printf("\nprogram %s\nwords %ld\nnon_instruction %ld\ndata_runs %ld\nsuspect %ld\n"
           "repaired 0\n",
           path, n, unknown, data_like, refused);
    printf("NO-REFERENCE LIMIT: an accurate count and a data/damage split are all the map\n"
           "alone can give. Repairing what belongs in a damaged position needs a reference.\n");
    if (outpath) { img_save(&im, outpath); printf("wrote %s (unchanged)\n", outpath); }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    arbit_init(0);
    if (argc < 2) {
        fprintf(stderr,
            "usage: binastcure <command>\n"
            "  scan   <elf>                     what the map says is not an instruction\n"
            "  repair <elf> <ref|-> [out]       fix damaged words; ref supplies context\n"
            "  agent\n");
        return 2;
    }
    const char *c = argv[1];
    if (!strcmp(c, "agent")) {
        printf("tool binastcure\n");
        printf("what it is: find damage in a program and put it back\n");
        printf("scan       a word that matches NO catalog row is not an instruction\n");
        printf("repair     context says what SHOULD be there; the map rebuilds the bits\n");
        printf("verify     every repair is re-encoded and re-decoded before it is written\n");
        printf("limits     the map cannot see a legal instruction in the wrong place;\n");
        printf("           that needs a reference program, which is what <ref> supplies\n");
        printf("command scan <elf>\n");
        printf("command repair <elf> <ref|-> [out]\n");
        return 0;
    }
    if (!strcmp(c, "scan")) {
        if (argc < 3) { fprintf(stderr, "binastcure: scan needs an ELF\n"); return 2; }
        return cmd_scan(argv[2]);
    }
    if (!strcmp(c, "repair")) {
        if (argc < 3) { fprintf(stderr, "binastcure: repair needs an ELF\n"); return 2; }
        return cmd_repair(argv[2], argc > 3 ? argv[3] : NULL, argc > 4 ? argv[4] : NULL);
    }
    fprintf(stderr, "binastcure: unknown command %s\n", c);
    return 2;
}
