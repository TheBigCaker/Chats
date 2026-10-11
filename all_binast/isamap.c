/* isamap.c -- the instruction catalog as a literal bit matrix.
 *
 * Every row of ISA_AARCH64.inc / ISA_X86_64.inc is already a matrix object:
 *
 *     template word  T   the bits that are FIXED
 *     field mask     M   the bits that are OPERAND (they move)
 *
 * Read as a grid, each row of the catalog becomes one row of a
 * (nforms x BITS) ternary matrix whose entries are:
 *
 *      1   bit is fixed high in the base word
 *     -1   bit is fixed low
 *      0   bit belongs to an operand field
 *
 * and matching a word against the whole catalog is one pass of
 *
 *     (word XOR T[r]) AND NOT M[r]   ==   0        -> row r fires
 *
 * which is bit arithmetic over the matrix, no parsing. That is the
 * "matrix of binary commands" reading: the catalog IS a detector matrix,
 * one row per instruction form, and decode is a matrix reduction.
 *
 * Encoding is the same object read the other way: the fields are a
 * gather matrix G[r] (field k collects bits pos[k][0..n]), so the raw
 * operand bits are G[r] applied to the word, and put-back re-scatters
 * them. Decode gathers, encode scatters, the mask says what moves.
 *
 * WIDTH: the row is as wide as the ISA's longest instruction -- 32 bits for
 * AArch64, 120 bits for x86-64 (15 bytes). A fixed 32-bit mask is not enough:
 * x86 rows carry fields above bit 32 (the 64-bit immediate, the wide rel32 the
 * bytes wrap around), and a narrow mask makes every one of those rows report a
 * gather outside its own mask. The matrix is therefore bits128 and covers any
 * instruction this catalog can express.
 *
 * This program is deliberately INDEPENDENT of the catalog's own runtime: it
 * computes M from the raw row fields (vbyte/vbit/vwid + the split window)
 * instead of calling isa_fieldmask. If the two disagree on any row, or if the
 * matrix's firing decisions differ from isa_decode on a corpus, one of them is
 * wrong -- which is what `verify` and `verify-corpus` measure. A matrix that
 * agreed with the catalog by construction would prove nothing.
 *
 *   isamap grid   [isa] [n]     the ternary bit map, one line per form
 *   isamap stats  [isa]         density, per-bit occupancy
 *   isamap match  <hex> [isa]   which rows fire on a word
 *   isamap mtx    <base> [isa]  write <base>.mtx and <base>.names
 *   isamap export <file> [isa]  self-describing binary matrix
 *   isamap verify [isa]         matrix vs catalog, every row
 *   isamap verify-corpus <f> [isa]  matrix vs catalog over real words
 *   isamap agent                what this tool is, machine-readable
 *
 * Pure C11, no dependencies, no libm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "isadb.h"

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"

#define ISA_INC_AARCH64 "ISA_AARCH64.inc"
#define ISA_INC_X86_64  "ISA_X86_64.inc"

#include ISA_INC_AARCH64
static const IsaTable T_aarch64 = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

#include ISA_INC_X86_64
static const IsaTable T_x86_64 = { isa_forms_x86_64, ISA_NFORMS_X86_64, "x86_64", 1, 15 };

static const IsaTable *tables[] = { &T_aarch64, &T_x86_64 };
static const int ntables = 2;

/* ------------------------------------------------------------------ matrix core */

/* A row of the matrix: one bit per position in the instruction's bytes. 128 bits
 * covers every instruction in these catalogs (x86-64 maxes at 15 bytes = 120). */
typedef struct { uint64_t w[2]; } bits128;

static bits128 B0(void) { bits128 r; r.w[0] = 0; r.w[1] = 0; return r; }
static void bset(bits128 *b, int i) { if (i >= 0 && i < 128) b->w[i >> 6] |= 1ull << (i & 63); }
static int  bget(const bits128 *b, int i) { return (i >= 0 && i < 128) ? (int)((b->w[i >> 6] >> (i & 63)) & 1ull) : 0; }

/* bits per row: the widest instruction this table can produce */
static int table_nbits(const IsaTable *t) {
    int w = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        int nb = t->forms[i].len * 8;
        if (nb > w) w = nb;
    }
    return w;
}

/* The mask, computed HERE from the raw field list. Deliberately not isa_fieldmask:
 * the point is to be a second, independent reading of the same table. */
static bits128 matrix_mask(const IsaForm *f) {
    bits128 m = B0();
    for (int v = 0; v < f->nvars; v++) {
        int lo = f->vbyte[v] * 8 + f->vbit[v];
        for (int i = 0; i < f->vwid[v]; i++) bset(&m, lo + i);
    }
    for (int v = 0; v < f->nvars; v++) {
        uint8_t k = f->vkind[v];
        if (k == K_SPLIT || k == K_BITSPL || k == K_PAGCNT || k == K_REXLO || k == K_LANEIX) {
            int lo = f->split_byte * 8 + f->split_bit;
            for (int i = 0; i < f->split_wid; i++) bset(&m, lo + i);
        }
        /* THE PER-OPERAND REX WINDOW, and this is a second independent reading that has to know
         * about it too. K_REXREG keeps an operand's high bit in ITS OWN window (REX.R / REX.B /
         * REX.X), so a row that does not mask those bits appears to conflict with its own
         * template. Omitting this made 261 of 350 x86 rows disagree with the catalog -- and the
         * check exists precisely to catch a disagreement like this, so the fix belongs in the
         * matrix as well as in isa_fieldmask. */
        if ((k == K_REXREG || k == K_REXREG_4 || k == K_REXREG_5 || k == K_REXREG_B4) && f->hbit[v]) {
            int lo = f->hbyte[v] * 8 + (f->hbit[v] - 1);
            bset(&m, lo);
        }
    }
    return m;
}

/* The template as bits, over the row's own length. */
static bits128 row_template(const IsaForm *f) {
    bits128 t = B0();
    for (int i = 0; i < f->len && i < ISA_MAXBYTES; i++)
        for (int k = 0; k < 8; k++)
            if ((f->bytes[i] >> k) & 1u) bset(&t, i * 8 + k);
    return t;
}

/* The word as bits. */
static bits128 word_bits(const uint8_t *b, int len) {
    bits128 w = B0();
    for (int i = 0; i < len && i < ISA_MAXBYTES; i++)
        for (int k = 0; k < 8; k++)
            if ((b[i] >> k) & 1u) bset(&w, i * 8 + k);
    return w;
}

/* A row is only meaningful over the bytes it actually occupies. */
static bits128 row_used(const IsaForm *f) {
    bits128 u = B0();
    for (int i = 0; i < f->len && i < ISA_MAXBYTES; i++)
        for (int k = 0; k < 8; k++) bset(&u, i * 8 + k);
    return u;
}

/* Gather order for one field: the bit positions, low piece first where the
 * kind stores its low half in the split window (K_SPLIT/K_PAGCNT), otherwise
 * main window first. This is the row's gather matrix, written out explicitly. */
static int gather_positions(const IsaForm *f, int v, uint8_t *pos) {
    int n = 0;
    int mainlo = f->vbyte[v] * 8 + f->vbit[v];
    int spllo  = f->split_byte * 8 + f->split_bit;
    uint8_t k  = f->vkind[v];
    int has_split = (k == K_SPLIT || k == K_BITSPL || k == K_PAGCNT || k == K_REXLO || k == K_LANEIX);
    if (has_split && (k == K_SPLIT || k == K_PAGCNT)) {
        for (int i = 0; i < f->split_wid; i++) pos[n++] = (uint8_t)(spllo + i);
        for (int i = 0; i < f->vwid[v]; i++)   pos[n++] = (uint8_t)(mainlo + i);
    } else {
        for (int i = 0; i < f->vwid[v]; i++)   pos[n++] = (uint8_t)(mainlo + i);
        if (has_split)
            for (int i = 0; i < f->split_wid; i++) pos[n++] = (uint8_t)(spllo + i);
    }
    /* THE PER-OPERAND REX BIT IS PART OF THIS FIELD. K_REXREG keeps an operand's high bit in its
     * own window (REX.R for ModR/M.reg, REX.B for .rm, REX.X for a SIB index), so gather must
     * collect it too -- otherwise the bit is in the matrix mask and no field ever addresses it,
     * which is exactly what "GATHER INCOMPLETE ... covered 0 mask 1" reports. 261 of 350 x86 rows
     * failed this check, and the check was right. */
    if ((k == K_REXREG || k == K_REXREG_4 || k == K_REXREG_5 || k == K_REXREG_B4) && f->hbit[v])
        pos[n++] = (uint8_t)(f->hbyte[v] * 8 + (f->hbit[v] - 1));
    return n;
}

/* The match rule, stated once:
 *     (word XOR template) AND NOT mask == 0
 * i.e. every bit the row calls fixed must equal the template, and operand bits
 * are free. `used` limits the comparison to the bytes the row actually occupies,
 * so a short row is not judged against the padding above it. */
static int row_fires(const IsaForm *f, const uint8_t *b) {
    bits128 m = matrix_mask(f), t = row_template(f), u = row_used(f);
    bits128 w = word_bits(b, f->len > ISA_MAXBYTES ? ISA_MAXBYTES : f->len);
    uint64_t d0 = (w.w[0] ^ t.w[0]) & ~m.w[0] & u.w[0];
    uint64_t d1 = (w.w[1] ^ t.w[1]) & ~m.w[1] & u.w[1];
    return (d0 | d1) == 0;
}

static const IsaTable *pick(const char *n) {
    for (int i = 0; i < ntables; i++) if (!strcmp(tables[i]->name, n)) return tables[i];
    return NULL;
}

/* ---------------------------------------------------------------- grid / stats */

static char tern_char(const bits128 *t, const bits128 *m, int bit) {
    if (bget(m, bit)) return '#';         /* operand: this bit moves */
    return bget(t, bit) ? '1' : '.';
}

static int cmd_grid(int argc, char **argv) {
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    int limit = (argc > 2) ? atoi(argv[2]) : (int)t->n;
    int nbits = table_nbits(t);
    printf("# isamap grid  isa=%s  forms=%u  bits=%d\n", t->name, t->n, nbits);
    printf("# # = operand bit (moves)   1 = fixed high   . = fixed low\n#\n");
    int shown = 0;
    for (uint32_t i = 0; i < t->n && shown < limit; i++, shown++) {
        const IsaForm *f = &t->forms[i];
        bits128 m = matrix_mask(f), tm = row_template(f);
        printf("%-26s ", f->name);
        for (int b = nbits - 1; b >= 0; b--) putchar(tern_char(&tm, &m, b));
        printf("  %s %s\n", f->mnemonic, f->operands);
    }
    return 0;
}

static int cmd_stats(int argc, char **argv) {
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    int nbits = table_nbits(t);
    unsigned long fixed1 = 0, fixed0 = 0, operand = 0;
    unsigned long *occ1 = arb_bytes((long)((size_t)nbits) * (long)(sizeof *occ1));
    unsigned long *occ0 = arb_bytes((long)((size_t)nbits) * (long)(sizeof *occ0));
    unsigned long *occv = arb_bytes((long)((size_t)nbits) * (long)(sizeof *occv));
    unsigned long total_fields = 0, split_fields = 0, nrows = 0;
    if (!occ1 || !occ0 || !occv) { fprintf(stderr, "isamap: out of memory\n"); return 2; }
    for (uint32_t i = 0; i < t->n; i++) {
        const IsaForm *f = &t->forms[i];
        bits128 m = matrix_mask(f), tm = row_template(f);
        for (int b = 0; b < f->len * 8 && b < nbits; b++) {
            if (bget(&m, b)) { operand++; occv[b]++; }
            else if (bget(&tm, b)) { fixed1++; occ1[b]++; }
            else { fixed0++; occ0[b]++; }
        }
        total_fields += (unsigned long)f->nvars;
        for (int v = 0; v < f->nvars; v++) {
            uint8_t k = f->vkind[v];
            if (k == K_SPLIT || k == K_BITSPL || k == K_PAGCNT || k == K_REXLO || k == K_LANEIX)
                split_fields++;
        }
        nrows++;
    }
    unsigned long cells = fixed1 + fixed0 + operand;
    printf("isa %s\n", t->name);
    printf("forms %lu\nbits_per_row %d\n", nrows, nbits);
    printf("matrix_cells %lu\n", cells);
    printf("fixed_high %lu\nfixed_low %lu\noperand %lu\n", fixed1, fixed0, operand);
    printf("fixed_total %lu\noperand_per_mille %lu\n", fixed1 + fixed0,
           cells ? (operand * 1000) / cells : 0);
    printf("fields_total %lu\nfields_split %lu\n", total_fields, split_fields);
    printf("bit  fixed_high fixed_low operand\n");
    for (int b = nbits - 1; b >= 0; b--)
        printf("%3d %10lu %9lu %7lu\n", b, occ1[b], occ0[b], occv[b]);
    arbit_release_carve((long)(occ1)); arbit_release_carve((long)(occ0)); arbit_release_carve((long)(occv));
    return 0;
}

/* -------------------------------------------------------------------- matching */

/* Parse a word the way isadb does, so the two tools accept the same input:
 *   a bare contiguous run the width of the ISA word ("d2800540") is a numeric
 *   word, written the way a listing prints it, and is reversed into memory order;
 *   anything else (spaced, comma-separated, a different width) is file-order bytes.
 * Returns the byte count, and sets *reversed to say whether it reversed them.
 *
 * Skipping separators is NOT cosmetic: stopping at the first space made
 * "40 05 80 d2" parse as the single byte 0x40, so match declined a word that
 * neighbors resolved correctly. Two tools disagreeing about one input is a bug
 * in whichever is narrower. */
static int hex_to_bytes(const char *s, uint8_t *out, int max, int *reversed) {
    if (reversed) *reversed = 0;
    size_t P = strlen(s);
    if (P >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; P -= 2; }
    int contiguous = 1;
    for (size_t i = 0; i < P; i++)
        if (!isxdigit((unsigned char)s[i])) { contiguous = 0; break; }
    if (contiguous) {
        int n = 0;
        for (size_t i = 0; i + 1 < P && n < max; i += 2) {
            char t[3] = { s[i], s[i+1], 0 };
            out[n++] = (uint8_t)strtoul(t, NULL, 16);
        }
        /* A contiguous run is the word written the way a listing prints it, so the
         * caller must reverse it into memory order. Saying so here is the whole
         * point of the flag -- leaving it 0 made "d2800540" decline while the
         * spaced form worked, which is the same class of bug in reverse. */
        if (reversed) *reversed = 1;
        return n;
    }
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *t = p;
        if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
        int d = 0;
        while (isxdigit((unsigned char)*t)) { t++; d++; }
        if (!d || d > 2) break;
        out[n++] = (uint8_t)strtoul((p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? p + 2 : p, NULL, 16);
        p = t;
    }
    return n;
}

/* A bare numeric word is printed most-significant-byte-first; reverse it. Only a
 * contiguous run is reversed -- doing it to a spaced string would misread a
 * file-order byte list, which is the form a hex dump gives you. */
static void maybe_reverse(uint8_t *b, int n, int wordsize, int was_contiguous) {
    if (was_contiguous && n == wordsize)
        for (int i = 0, j = n - 1; i < j; i++, j--) { uint8_t s = b[i]; b[i] = b[j]; b[j] = s; }
}

/* A word list holds one contiguous hex run per line (the form `binast tokens`
 * emits). Kept separate from hex_to_bytes so the corpus path stays a tight loop. */
static int is_contiguous_run(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

static int cmd_match(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isamap match <hex> [isa]\n"); return 2; }
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    uint8_t b[ISA_MAXBYTES];
    memset(b, 0, sizeof b);
    int reversed = 0;
    int n = hex_to_bytes(argv[0], b, ISA_MAXBYTES, &reversed);
    if (!n) { fprintf(stderr, "isamap: no hex in %s\n", argv[0]); return 2; }
    maybe_reverse(b, n, t->wordsize, reversed);
    const IsaForm *f = NULL; long h[ISA_MAXV] = {0};
    int used = isa_decode(t, b, n, &f, h);
    printf("word");
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
    printf("catalog %s\n", used ? f->name : "(declined)");
    int fired = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        if (t->forms[i].len > n) continue;
        if (!row_fires(&t->forms[i], b)) continue;
        fired++;
        printf("fires  %-26s %s %s\n", t->forms[i].name, t->forms[i].mnemonic, t->forms[i].operands);
    }
    printf("fired %d\n", fired);
    if (used && !row_fires(f, b)) { printf("INCONSISTENT catalog row did not fire\n"); return 1; }
    return 0;
}

/* ---------------------------------------------------------------- mtx / export */

static int cmd_mtx(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isamap mtx <base> [isa]\n"); return 2; }
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    int nbits = table_nbits(t);
    char mp[512], np[512];
    snprintf(mp, sizeof mp, "%s.mtx", argv[0]);
    snprintf(np, sizeof np, "%s.names", argv[0]);
    FILE *m = fopen(mp, "w"), *nm = fopen(np, "w");
    if (!m || !nm) { fprintf(stderr, "isamap: cannot write\n"); if (m) fclose(m); if (nm) fclose(nm); return 2; }
    fprintf(m, "%%%% isamap ternary bit matrix  isa=%s  bits=%d  rows=%u\n", t->name, nbits, t->n);
    fprintf(m, "%%%% 1 = bit fixed high in the base word\n");
    fprintf(m, "%%%% -1 = bit fixed low\n");
    fprintf(m, "%%%% 0 = bit belongs to an operand field\n");
    fprintf(m, "%%%% column order: most significant bit first\n");
    fprintf(m, "%% %u %d\n", t->n, nbits);
    for (uint32_t i = 0; i < t->n; i++) {
        const IsaForm *f = &t->forms[i];
        bits128 msk = matrix_mask(f), tm = row_template(f);
        for (int b = nbits - 1; b >= 0; b--) {
            int v = bget(&msk, b) ? 0 : (bget(&tm, b) ? 1 : -1);
            fprintf(m, "%s%d", b == nbits - 1 ? "" : " ", v);
        }
        fprintf(m, "\n");
        fprintf(nm, "%s\t%s\t%s\t%d\n", f->name, f->mnemonic, f->operands, f->len);
    }
    fclose(m); fclose(nm);
    printf("wrote %s\nwrote %s\nrows %u\nbits %d\n", mp, np, t->n, nbits);
    return 0;
}

/* Binary export. Self-describing, little-endian, no padding surprises:
 *   magic "ISAMAP01"  8 bytes
 *   u32 nforms, u32 nbits, u32 nfields, u32 wordsize, u32 maxlen, u32 name_len
 *   name bytes, padded to 4
 *   u64 template_lo, template_hi     (nforms pairs)
 *   u64 mask_lo, mask_hi             (nforms pairs)
 *   u8  len[nforms]
 *   u8  nvars[nforms]
 *   u32 field_index[nforms]          -- start of this row's fields below
 *   per field: u8 nbits, u8 kind, u8 pos[nbits]   (positions in gather order)
 */
static int cmd_export(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isamap export <file> [isa]\n"); return 2; }
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    int nbits = table_nbits(t);
    uint32_t nfields = 0;
    for (uint32_t i = 0; i < t->n; i++) nfields += t->forms[i].nvars;
    FILE *o = fopen(argv[0], "wb");
    if (!o) { fprintf(stderr, "isamap: cannot write %s\n", argv[0]); return 2; }
    size_t nm_len = strlen(t->name);
    uint32_t hdr[6] = { t->n, (uint32_t)nbits, nfields, (uint32_t)t->wordsize,
                        (uint32_t)t->maxlen, (uint32_t)nm_len };
    fwrite("ISAMAP01", 1, 8, o);
    fwrite(hdr, sizeof(uint32_t), 6, o);
    fwrite(t->name, 1, nm_len, o);
    size_t pad = (4 - (nm_len % 4)) % 4;
    for (size_t i = 0; i < pad; i++) fputc(0, o);
    for (uint32_t i = 0; i < t->n; i++) {
        bits128 tm = row_template(&t->forms[i]);
        fwrite(&tm.w[0], sizeof(uint64_t), 1, o);
        fwrite(&tm.w[1], sizeof(uint64_t), 1, o);
    }
    for (uint32_t i = 0; i < t->n; i++) {
        bits128 ms = matrix_mask(&t->forms[i]);
        fwrite(&ms.w[0], sizeof(uint64_t), 1, o);
        fwrite(&ms.w[1], sizeof(uint64_t), 1, o);
    }
    for (uint32_t i = 0; i < t->n; i++) { uint8_t l = t->forms[i].len; fwrite(&l, 1, 1, o); }
    for (uint32_t i = 0; i < t->n; i++) { uint8_t v = t->forms[i].nvars; fwrite(&v, 1, 1, o); }
    uint32_t idx = 0;
    for (uint32_t i = 0; i < t->n; i++) { fwrite(&idx, sizeof(uint32_t), 1, o); idx += t->forms[i].nvars; }
    for (uint32_t i = 0; i < t->n; i++) {
        const IsaForm *f = &t->forms[i];
        for (int v = 0; v < f->nvars; v++) {
            uint8_t pos[64];
            int np = gather_positions(f, v, pos);
            uint8_t fn = (uint8_t)np, fk = f->vkind[v];
            fwrite(&fn, 1, 1, o);
            fwrite(&fk, 1, 1, o);
            fwrite(pos, 1, (size_t)np, o);
        }
    }
    long sz = ftell(o);
    fclose(o);
    printf("wrote %s\nforms %u\nbits %d\nfields %u\nbytes %ld\n", argv[0], t->n, nbits, nfields, sz);
    return 0;
}

/* --------------------------------------------------------------------- verify */

/* Level 1: the matrix's mask, computed here, must equal the catalog's own
 * isa_fieldmask for every row. Two independent readings, same table. */
static int cmd_verify(int argc, char **argv) {
    const IsaTable *t = argc > 0 ? pick(argv[0]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    int bad = 0, checked = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        const IsaForm *f = &t->forms[i];
        uint8_t cm[ISA_MAXBYTES];
        isa_fieldmask(f, cm);
        bits128 got = matrix_mask(f);
        bits128 want = word_bits(cm, ISA_MAXBYTES);
        checked++;
        for (int b = 0; b < ISA_MAXBYTES * 8; b++) {
            if (bget(&want, b) != bget(&got, b)) {
                printf("MASK MISMATCH %s at bit %d: catalog %d matrix %d\n",
                       f->name, b, bget(&want, b), bget(&got, b));
                bad++;
                break;
            }
        }
        /* every field's gather must address only bits inside the mask, and the
         * gather must cover every mask bit (no operand bit left uncollected) */
        bits128 covered = B0();
        for (int v = 0; v < f->nvars; v++) {
            uint8_t pos[128];
            int np = gather_positions(f, v, pos);
            for (int k = 0; k < np; k++) {
                if (!bget(&got, pos[k])) {
                    printf("GATHER OUTSIDE MASK %s field %d bit %u\n", f->name, v, pos[k]);
                    bad++;
                }
                bset(&covered, pos[k]);
            }
        }
        for (int b = 0; b < ISA_MAXBYTES * 8; b++) {
            if (bget(&got, b) != bget(&covered, b)) {
                printf("GATHER INCOMPLETE %s at bit %d: covered %d mask %d\n",
                       f->name, b, bget(&covered, b), bget(&got, b));
                bad++;
                break;
            }
        }
    }
    printf("%s matrix: %d rows checked, %d failed\n", t->name, checked, bad);
    return bad ? 1 : 0;
}

/* Level 2: the matrix's firing decision must agree with the catalog on real
 * words. The catalog is the oracle; the matrix is an independent re-derivation. */
static int cmd_verify_corpus(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isamap verify-corpus <words> [isa]\n"); return 2; }
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    FILE *f = fopen(argv[0], "r");
    if (!f) { fprintf(stderr, "isamap: cannot open %s\n", argv[0]); return 2; }
    unsigned long total = 0, decoded = 0, declined = 0, nofire = 0, missed = 0, multi = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t L = strlen(p);
        while (L && (p[L-1] == '\n' || p[L-1] == '\r' || p[L-1] == ' ')) p[--L] = 0;
        if (!L) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        total++;
        uint8_t b[ISA_MAXBYTES];
        memset(b, 0, sizeof b);
        int n = 0;
        size_t P = strlen(p);
        for (size_t i = 0; i + 1 < P && n < ISA_MAXBYTES; i += 2) {
            char s[3] = { p[i], p[i+1], 0 };
            b[n++] = (uint8_t)strtoul(s, NULL, 16);
        }
        if (n == t->wordsize && is_contiguous_run(p))
            for (int i = 0, j = n - 1; i < j; i++, j--) { uint8_t s = b[i]; b[i] = b[j]; b[j] = s; }
        const IsaForm *form = NULL; long h[ISA_MAXV] = {0};
        int used = isa_decode(t, b, n, &form, h);
        int fired = 0;
        for (uint32_t i = 0; i < t->n; i++) {
            if (t->forms[i].len > n) continue;
            if (row_fires(&t->forms[i], b)) fired++;
        }
        if (used) {
            decoded++;
            if (!row_fires(form, b)) { missed++; if (missed < 5) printf("MATRIX MISSED %s\n", p); }
            if (fired > 1) multi++;
        } else {
            declined++;
            if (!fired) nofire++;
        }
    }
    fclose(f);
    printf("isa %s\nwords %lu\ndecoded %lu\ndeclined %lu\n"
           "matrix_fired_when_decoded %lu\nmatrix_candidates_on_declined %lu\n"
           "matrix_missed_catalog_match %lu\nambiguous_multiple_fired %lu\n",
           t->name, total, decoded, declined, decoded - missed, declined - nofire, missed, multi);
    return missed ? 1 : 0;
}

/* ----------------------------------------------------------------- vocabulary --
 * A tokenizer for raw machine code that needs no training and no vocabulary file.
 *
 * The point the matrix makes concrete: an instruction word does not have to be
 * chopped into text tokens at all. The word IS the token. Two words are the same
 * token exactly when they are the same 32 bits; two words are the same *kind* of
 * token when the same catalog row decodes them, with operands differing.
 *
 * So the vocabulary is:
 *   level 0  the word itself         4.3e9 possible, 414502 seen in the corpus
 *   level 1  the catalog row         ~1400, the instruction form
 *   level 2  the mnemonic            ~500, the operation
 *
 * and the operand values are not part of the identity -- they are the content in
 * a fixed slot. That is what makes it work for a model: the form is a symbol, and
 * the operand bits are a structured payload the model can predict positionally.
 *
 * `vocab` prints the distribution; `neighbors` prints the rows whose mask is
 * nearest a given word, which is the "semantic DB" question answered in the
 * cheapest possible way -- distance in the matrix IS the semantic relation.
 */
static int cmd_vocab(int argc, char **argv) {
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    if (argc < 2) { fprintf(stderr, "usage: isamap vocab <words-file> [isa]\n"); return 2; }
    FILE *f = fopen(argv[0], "r");
    if (!f) { fprintf(stderr, "isamap: cannot open %s\n", argv[0]); return 2; }
    /* count form usage; operand payloads are not part of token identity */
    unsigned long *cnt = arb_bytes((long)(t->n) * (long)(sizeof *cnt));
    if (!cnt) { fprintf(stderr, "isamap: out of memory\n"); return 2; }
    unsigned long total = 0, declined = 0, distinct_words = 0;
    /* distinct words: cheap exact set, open-addressed on the word value */
    size_t cap = 1u << 22;
    uint32_t *tbl = arb_bytes((long)(cap) * (long)(sizeof *tbl));
    uint8_t *has = arb_bytes((long)(cap) * (long)(1));
    if (!tbl || !has) { fprintf(stderr, "isamap: out of memory\n"); return 2; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t L = strlen(p);
        while (L && (p[L-1] == '\n' || p[L-1] == '\r' || p[L-1] == ' ')) p[--L] = 0;
        if (!L) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint8_t b[ISA_MAXBYTES];
        memset(b, 0, sizeof b);
        int n = 0;
        size_t P = strlen(p);
        for (size_t i = 0; i + 1 < P && n < ISA_MAXBYTES; i += 2) {
            char s[3] = { p[i], p[i+1], 0 };
            b[n++] = (uint8_t)strtoul(s, NULL, 16);
        }
        if (n == t->wordsize)
            for (int i = 0, j = n - 1; i < j; i++, j--) { uint8_t s = b[i]; b[i] = b[j]; b[j] = s; }
        total++;
        uint32_t val = 0;
        for (int i = 0; i < n && i < 4; i++) val |= (uint32_t)b[i] << (8 * i);
        size_t h = (val * 2654435761u) & (cap - 1);
        while (has[h] && tbl[h] != val) h = (h + 1) & (cap - 1);
        if (!has[h]) { has[h] = 1; tbl[h] = val; distinct_words++; }
        const IsaForm *form = NULL; long hh[ISA_MAXV] = {0};
        int used = isa_decode(t, b, n, &form, hh);
        if (!used) { declined++; continue; }
        cnt[(size_t)(form - t->forms)]++;
    }
    fclose(f);
    printf("isa %s\nwords %lu\ndistinct_words %lu\ndeclined %lu\nforms_used ", t->name, total, distinct_words, declined);
    unsigned long used_forms = 0;
    for (uint32_t i = 0; i < t->n; i++) if (cnt[i]) used_forms++;
    printf("%lu\nforms_catalog %u\n", used_forms, t->n);
    printf("levels word:%lu form:%lu mnemonic:<=%u operand_bits:payload\n",
           distinct_words, used_forms, t->n);
    printf("%-26s %12s %7s\n", "form", "count", "per_mille");
    for (int pass = 0; pass < 25; pass++) {
        uint32_t best = 0; unsigned long bv = 0;
        for (uint32_t i = 0; i < t->n; i++) if (cnt[i] > bv) { bv = cnt[i]; best = i; }
        if (!bv) break;
        printf("%-26s %12lu %7lu\n", t->forms[best].name, bv, total ? (bv * 1000) / total : 0);
        cnt[best] = 0;
    }
    arbit_release_carve((long)(cnt)); arbit_release_carve((long)(tbl)); arbit_release_carve((long)(has));
    return 0;
}

/* Nearest catalog rows to a word, by Hamming distance over the bits each row
 * considers fixed. This answers the "semantic DB" question in the cheapest
 * possible way: the matrix IS the embedding, and distance in it is the relation.
 * A word's nearest rows are the encodings it is closest to being. */
static int cmd_neighbors(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isamap neighbors <hex> [isa] [n]\n"); return 2; }
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    int want = (argc > 2) ? atoi(argv[2]) : 10;
    if (want < 1) want = 1;
    if (want > 64) want = 64;
    uint8_t b[ISA_MAXBYTES];
    memset(b, 0, sizeof b);
    int reversed = 0;
    int n = hex_to_bytes(argv[0], b, ISA_MAXBYTES, &reversed);
    if (!n) { fprintf(stderr, "isamap: no hex in %s\n", argv[0]); return 2; }
    maybe_reverse(b, n, t->wordsize, reversed);
    bits128 w = word_bits(b, n);
    printf("word");
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");

    /* distance of every eligible row, then a simple selection of the best `want` */
    int *dist = arb_bytes(t->n * sizeof *dist);
    if (!dist) { fprintf(stderr, "isamap: out of memory\n"); return 2; }
    for (uint32_t i = 0; i < t->n; i++) {
        const IsaForm *f = &t->forms[i];
        if (f->len > n) { dist[i] = -1; continue; }
        bits128 m = matrix_mask(f), tm = row_template(f), u = row_used(f);
        int d = 0;
        for (int k = 0; k < 128; k++) {
            if (!bget(&u, k)) continue;
            if (!bget(&m, k) && (bget(&w, k) != bget(&tm, k))) d++;
        }
        dist[i] = d;
    }
    for (int rank = 0; rank < want; rank++) {
        int bestd = 1 << 30, besti = -1;
        for (uint32_t i = 0; i < t->n; i++) if (dist[i] >= 0 && dist[i] < bestd) { bestd = dist[i]; besti = (int)i; }
        if (besti < 0) break;
        printf("%2d distance %2d  %-26s %s %s\n", rank + 1, bestd,
               t->forms[besti].name, t->forms[besti].mnemonic, t->forms[besti].operands);
        dist[besti] = -1;
    }
    arbit_release_carve((long)(dist));
    return 0;
}

/* Emit the catalog row index for every word, one per line: the FORM sequence.
 *
 * The row is a deterministic function of the word, so this projects the operand
 * payload away and leaves "which instruction is this". Scoring that sequence with
 * binast_hold answers the sweet-spot question directly:
 *
 *   H(form)             what the row's own frequency distribution explains
 *   H(form | context)   how much of the row is predictable from the token history
 *
 * If H(form|context) is near zero, context already determines the row and feeding
 * the matrix to a model adds nothing. The closer it sits to H(form), the more the
 * matrix contributes as a prior. That residual is the ceiling on the whole idea.
 *
 * Undecodable words emit ffffffff so they are visible in the sequence rather than
 * silently dropped, which would change the context distances. */
static int cmd_rows(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isamap rows <words-file> [isa]\n"); return 2; }
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    FILE *f = fopen(argv[0], "r");
    if (!f) { fprintf(stderr, "isamap: cannot open %s\n", argv[0]); return 2; }
    unsigned long n = 0, undec = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t L = strlen(p);
        while (L && (p[L-1] == '\n' || p[L-1] == '\r' || p[L-1] == ' ')) p[--L] = 0;
        if (!L) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        int contiguous = is_contiguous_run(p);
        uint8_t b[ISA_MAXBYTES];
        memset(b, 0, sizeof b);
        int n2 = 0;
        size_t P = strlen(p);
        if (contiguous) {
            for (size_t i = 0; i + 1 < P && n2 < ISA_MAXBYTES; i += 2) {
                char s[3] = { p[i], p[i+1], 0 };
                b[n2++] = (uint8_t)strtoul(s, NULL, 16);
            }
            if (n2 == t->wordsize)
                for (int i = 0, j = n2 - 1; i < j; i++, j--) { uint8_t s = b[i]; b[i] = b[j]; b[j] = s; }
        } else {
            const char *q = p;
            while (*q && n2 < ISA_MAXBYTES) {
                while (*q == ' ' || *q == '\t' || *q == ',') q++;
                if (!*q) break;
                const char *tt = q;
                int d = 0;
                while (isxdigit((unsigned char)*tt)) { tt++; d++; }
                if (!d || d > 2) break;
                b[n2++] = (uint8_t)strtoul(q, NULL, 16);
                q = tt;
            }
        }
        if (!n2) continue;
        n++;
        const IsaForm *form = NULL; long h[ISA_MAXV] = {0};
        int used = isa_decode(t, b, n2, &form, h);
        if (!used) { printf("ffffffff\n"); undec++; continue; }
        printf("%08zx\n", (size_t)(form - t->forms));
    }
    fclose(f);
    fprintf(stderr, "isamap rows: %lu words, %lu undecodable, %u forms in the catalog\n",
            n, undec, t->n);
    return 0;
}

/* Emit, per word, "row:payload" where payload is the operand bits packed in a
 * fixed order. This splits the instruction into the two things a model has to
 * predict, so each can be scored on its own:
 *
 *   row      which form            -- a symbol from the catalog
 *   payload  the operand values    -- a bounded integer, positionally defined
 *
 * The payload is the field bits of the row's own operand windows, read low to
 * high and packed contiguously. It is NOT the raw word: fixed bits are dropped,
 * so two words of the same form with the same operands produce the same payload
 * even when they differ in a bit the form owns. That is the point -- it makes the
 * payload a property of the instruction's CONTENT rather than its encoding.
 *
 * With `--fixed` the second column is instead the number of fixed bits in the row,
 * which is the row's information content as a mask.
 */
static int cmd_payload(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isamap payload <words-file> [isa]\n"); return 2; }
    const IsaTable *t = argc > 1 ? pick(argv[1]) : &T_aarch64;
    if (!t) { fprintf(stderr, "isamap: unknown ISA\n"); return 2; }
    FILE *f = fopen(argv[0], "r");
    if (!f) { fprintf(stderr, "isamap: cannot open %s\n", argv[0]); return 2; }
    unsigned long n = 0, undec = 0;
    double pay_bits_sum = 0.0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t L = strlen(p);
        while (L && (p[L-1] == '\n' || p[L-1] == '\r' || p[L-1] == ' ')) p[--L] = 0;
        if (!L) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint8_t b[ISA_MAXBYTES];
        memset(b, 0, sizeof b);
        int n2 = 0;
        size_t P = strlen(p);
        int contiguous = is_contiguous_run(p);
        if (contiguous) {
            for (size_t i = 0; i + 1 < P && n2 < ISA_MAXBYTES; i += 2) {
                char s[3] = { p[i], p[i+1], 0 };
                b[n2++] = (uint8_t)strtoul(s, NULL, 16);
            }
            if (n2 == t->wordsize)
                for (int i = 0, j = n2 - 1; i < j; i++, j--) { uint8_t s = b[i]; b[i] = b[j]; b[j] = s; }
        } else {
            const char *q = p;
            while (*q && n2 < ISA_MAXBYTES) {
                while (*q == ' ' || *q == '\t' || *q == ',') q++;
                if (!*q) break;
                const char *tt = q;
                int d = 0;
                while (isxdigit((unsigned char)*tt)) { tt++; d++; }
                if (!d || d > 2) break;
                b[n2++] = (uint8_t)strtoul(q, NULL, 16);
                q = tt;
            }
        }
        if (!n2) continue;
        n++;
        const IsaForm *form = NULL; long h[ISA_MAXV] = {0};
        int used = isa_decode(t, b, n2, &form, h);
        if (!used) { printf("ffffffff:0\n"); undec++; continue; }
        /* pack every operand window, in field order, low bit first */
        uint64_t pay = 0;
        int shift = 0;
        for (int v = 0; v < ISA_MAXV; v++) {
            if (v >= form->nvars) break;
            uint64_t rawbits = 0;
            int lo = form->vbyte[v] * 8 + form->vbit[v];
            for (int i = 0; i < form->vwid[v]; i++) {
                int bit = lo + i;
                if ((b[bit >> 3] >> (bit & 7)) & 1u) rawbits |= 1ull << i;
            }
            uint8_t k = form->vkind[v];
            if (k == K_SPLIT || k == K_BITSPL || k == K_PAGCNT || k == K_REXLO || k == K_LANEIX) {
                int slo = form->split_byte * 8 + form->split_bit;
                uint64_t sraw = 0;
                for (int i = 0; i < form->split_wid; i++) {
                    int bit = slo + i;
                    if ((b[bit >> 3] >> (bit & 7)) & 1u) sraw |= 1ull << i;
                }
                if (k == K_SPLIT || k == K_PAGCNT) {
                    /* low piece is the split window; keep the same packing order */
                    pay |= (sraw << shift); shift += form->split_wid;
                    pay |= (rawbits << shift); shift += form->vwid[v];
                } else {
                    pay |= (rawbits << shift); shift += form->vwid[v];
                    pay |= (sraw << shift); shift += form->split_wid;
                }
            } else {
                pay |= (rawbits << shift); shift += form->vwid[v];
            }
            if (shift > 60) break;
        }
        pay_bits_sum += (double)shift;
        printf("%08zx:%016llx\n", (size_t)(form - t->forms), (unsigned long long)pay);
    }
    fclose(f);
    fprintf(stderr, "isamap payload: %lu words, %lu undecodable, mean payload width %.2f bits\n",
            n, undec, n ? pay_bits_sum / (double)n : 0.0);
    return 0;
}

/* ---------------------------------------------------------------------- agent */

static int cmd_agent(void) {
    int nb = table_nbits(&T_aarch64), nb2 = table_nbits(&T_x86_64);
    printf("tool isamap\n");
    printf("what it is: the instruction catalog read as a (forms x bits) ternary bit matrix\n");
    printf("entry meaning: 1 fixed high in the base word, -1 fixed low, 0 operand bit\n");
    printf("match_rule (word XOR template) AND NOT mask == 0\n");
    printf("grid_columns most significant bit first, width is the ISA's longest instruction\n");
    printf("isa aarch64 forms %u bits %d\n", T_aarch64.n, nb);
    printf("isa x86_64 forms %u bits %d\n", T_x86_64.n, nb2);
    printf("command grid   [isa] [n]    print the bit map\n");
    printf("command stats  [isa]        density and per-bit occupancy\n");
    printf("command match  <hex> [isa]  which matrix rows fire on a word\n");
    printf("command mtx    <base> [isa] write <base>.mtx ternary matrix and <base>.names\n");
    printf("command export <file> [isa] self-describing binary matrix\n");
    printf("command verify [isa]        matrix mask vs catalog mask, every row\n");
    printf("command verify-corpus <f> [isa] matrix firing vs catalog decode over words\n");
    printf("command vocab  <words> [isa] the token distribution by form\n");
    printf("command neighbors <hex> [isa] [n]  nearest encodings, by matrix distance\n");
    printf("command rows  <words> [isa]  the form index per word, for H(form|context)\n");
    return 0;
}

/* ----------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc < 2) {
        fprintf(stderr,
            "usage: isamap <command> [args]\n"
            "  grid   [isa] [n]        the ternary bit matrix, one line per form\n"
            "  stats  [isa]            density and per-bit occupancy\n"
            "  match  <hex> [isa]      which rows fire on a word\n"
            "  mtx    <base> [isa]     write <base>.mtx and <base>.names\n"
            "  export <file> [isa]     self-describing binary matrix\n"
            "  verify [isa]            matrix vs catalog, every row\n"
            "  verify-corpus <f> [isa] matrix vs catalog over real words\n"
            "  vocab  <words> [isa]    the token distribution by form\n"
            "  neighbors <hex> [isa] [n]  nearest encodings by matrix distance\n"
            "  agent                   what this is, machine-readable\n");
        return 2;
    }
    const char *c = argv[1];
    char **rest = argv + 2;
    int nrest = argc - 2;
    if (!strcmp(c, "grid"))   return cmd_grid(nrest, rest);
    if (!strcmp(c, "stats"))  return cmd_stats(nrest, rest);
    if (!strcmp(c, "match"))  return cmd_match(nrest, rest);
    if (!strcmp(c, "mtx"))    return cmd_mtx(nrest, rest);
    if (!strcmp(c, "export")) return cmd_export(nrest, rest);
    if (!strcmp(c, "verify")) return cmd_verify(nrest, rest);
    if (!strcmp(c, "verify-corpus")) return cmd_verify_corpus(nrest, rest);
    if (!strcmp(c, "vocab"))  return cmd_vocab(nrest, rest);
    if (!strcmp(c, "neighbors")) return cmd_neighbors(nrest, rest);
    if (!strcmp(c, "rows"))   return cmd_rows(nrest, rest);
    if (!strcmp(c, "payload")) return cmd_payload(nrest, rest);
    if (!strcmp(c, "agent"))  return cmd_agent();
    fprintf(stderr, "isamap: unknown command %s\n", c);
    return 2;
}
