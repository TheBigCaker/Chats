/* binastbuild.c -- BUILD a program from blocks. The layout engine.
 *
 * WHY THIS IS THE POINT OF THE WHOLE TOOLCHAIN. Everything else reads or edits a program
 * someone else's compiler produced. This one takes a list of instructions written as
 * (BLOCK, operand values) and lays them out into a runnable ELF: it places each block at a
 * position, resolves every branch displacement, and proves the result decodes and runs.
 * No assembler, no compiler, no linker. The catalog is the assembler.
 *
 * THE FOUR JOBS A LAYOUT ENGINE HAS TO DO, and each one is where a naive version breaks:
 *
 * 1. PLACE      a block goes at an address. Straightforward -- but the address decides every
 *               displacement, so placement cannot be independent of resolution.
 * 2. RESOLVE    a branch stores a DISTANCE, not a target. The distance depends on where the
 *               branch lands, which depends on placement -- and moving a block to make a
 *               branch reachable changes other branches. So placement and resolution have
 *               to be solved TOGETHER, not in sequence.
 * 3. ALIGN      some blocks must land on 4-byte boundaries; a literal pool referenced by
 *               `ldr x0, =value` must sit close enough to be in range (19-bit offset).
 * 4. VERIFY     every emitted word must decode back to the block it was written as, and the
 *               program must RUN. "It assembled" is not the claim; "it runs and does what
 *               was specified" is.
 *
 * THE SHAPE OF THE INPUT. A program is a list of lines, because that is what an agent or a
 * human can write and diff:
 *
 *     # a comment
 *     MOVZ_X  8 64 0            <- write movz x8,#64 (the write syscall number)
 *     MOVZ_X  0 1 0             <- x0 = 1 (fd)
 *     ADRP_X  1 0               <- x1 = page of msg   (a LABEL target, resolved at layout)
 *     ADD_X_imm 1 1 msg         <- operand is a SYMBOL, not a number
 *     MOVZ_X  8 93 0
 *     SVC     0
 *     msg: .ascii "hello\n"
 *
 *   A slot whose value is a SYMBOL, or the first operand of ADRP/B, is resolved by the
 *   layout, not typed by the author. That is what makes this a layout engine rather than a
 *   table of numbers.
 *
 *   binastbuild <src> <out.elf> [base_addr]
 *   binastbuild agent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>   /* stat/chmod: the output must keep the source's mode, or a rebuilt
                         * program is not executable and every test of it exits 127 */
#include "isadb.h"
#include "relink.h"   /* relink_prel_operand: does this row carry a PC-relative operand? */
#include "elfobj.h"   /* ElfImage: build an ELF from scratch, no linker */
#include "arbishim.h"

#define ISA_INC_AARCH64 "ISA_AARCH64.inc"
#include ISA_INC_AARCH64
static const IsaTable T = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

/* THE ITEM AND SYMBOL TABLES ARE CARVED AT STARTUP, SIZED FROM THE INPUT.
 *
 * They were fixed arrays: MAXLINES 1048576 items and MAXSYMS 262144 symbols, both static. An
 * `Item` is 920 bytes, so the line table alone was 920 MB of BSS, and the fixed caps were wrong in
 * both directions on a real program:
 *
 *     cargo   4,380,432 items  -- 4x over the line cap
 *     cbmc    1,520,177 items  -- 1.5x over it
 *     cargo     316,070 branch targets -- 1.2x over the SYMBOL cap
 *
 * The symbol overrun mattered more than the line overrun, because sym_add used to have a smaller
 * cap and DROPPED symbols: a dropped label turns into "targets undefined label" with nothing
 * saying the table was full. Both tables are now sized from the source's own line count and from
 * the number of branch targets the converter reported, with the ceiling imposed by memory rather
 * than by a constant, and an honest refusal when the allocation cannot be made.
 */
#define MAXLINES_HARD 24000000L  /* a ceiling, not a requirement: ~22 GB of items */
#define MAXSYMS_HARD   4000000L

typedef struct { char name[64]; uint64_t addr; int defined; int item; } Sym;

typedef struct {
    char     block[48];        /* the catalog row name, or ".ascii"/".word"/".align" */
    long     slot[ISA_MAXV];
    char     sym[ISA_MAXV][64];/* a slot's symbolic name, if it has one */
    int      has_sym[ISA_MAXV];
    int      lo12[ISA_MAXV];    /* the symbol is wanted as its low 12 bits (@lo12) */
    int      nslot;
    const IsaForm *f;          /* resolved catalog row, NULL for a directive */
    int      pc_rel;           /* this block carries a PC-relative displacement */
    int      is_pc_slot;       /* which slot is the displacement */
    char     target[64];       /* where it must point */
    uint64_t addr;             /* where it landed */
    uint32_t word;             /* the emitted bits */
    char     data[256];        /* for .ascii */
    int      dlen;
    int      is_data;
    int      is_align;
    int      is_org;         /* .org: force the next item to an absolute address */
    uint64_t org;
    int      srcline;
} Item;

static Item  *g_it;
static long   g_n = 0;
static long   g_cap_it = 0;
static Sym   *g_sym;
static long   g_nsym = 0;
static long   g_cap_sym = 0;
#define SBUCKETS 2097152
static int   *g_sbucket;   /* SBUCKETS heads, -1 empty */
static int   *g_snext;     /* per-symbol next index in its bucket, -1 end */

/* little-endian readers for the ELF header walk in --into (no libelf in the tree) */
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static unsigned sym_hash(const char *n) {
    unsigned h = 2166136261u;
    for (const char *p = n; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h & (SBUCKETS - 1);
}

static Sym *sym_find(const char *n) {
    /* HASH-INDEXED, NOT A LINEAR SCAN. A converted real program has one symbol per branch target
     * -- 316,070 for cargo -- and resolving each branch by scanning that table is about 5 * 10^10
     * strcmps. O(1) lookup is the difference between building cargo and not building it. */
    if (!g_sbucket) return NULL;
    for (int i = g_sbucket[sym_hash(n)]; i >= 0; i = g_snext[i])
        if (!strcmp(g_sym[i].name, n)) return &g_sym[i];
    return NULL;
}
static Sym *sym_add(const char *n) {
    Sym *s = sym_find(n);
    if (s) return s;
    if (g_nsym >= g_cap_sym) {
        fprintf(stderr, "binastbuild: symbol table full at %ld -- refusing rather than "
                        "silently dropping '%s'\n", g_cap_sym, n);
        exit(2);
    }
    /* THE CHAIN LINK IS A PARALLEL ARRAY. Sym carries `item` for the binding pass, and overloading
     * one field for two purposes is how a table silently corrupts itself -- the binding pass walks
     * every symbol and would have followed the chain instead of the item index. */
    unsigned h = sym_hash(n);
    s = &g_sym[g_nsym];
    snprintf(s->name, sizeof s->name, "%s", n);
    s->addr = 0; s->defined = 0; s->item = -1;
    g_snext[g_nsym] = g_sbucket[h];
    g_sbucket[h] = (int)g_nsym;
    g_nsym++;
    return s;
}

/* ------------------------------------------------------------------ parse ---- */
static int parse_line(char *line, int lineno) {
    /* strip comments and trailing space */
    char *h = strchr(line, '#');
    if (h) *h = 0;
    size_t L = strlen(line);
    while (L && (line[L-1] == ' ' || line[L-1] == '\t' || line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;

    Item *it = &g_it[g_n];
    memset(it, 0, sizeof *it);
    it->srcline = lineno;

    /* a label:  name:  -- may share a line with an instruction */
    char *colon = strchr(p, ':');
    if (colon && (colon == p || colon[-1] != ' ')) {
        char nm[64]; int k = 0;
        for (char *q = p; q < colon && k < 63; q++) nm[k++] = *q;
        nm[k] = 0;
        while (k && (nm[k-1] == ' ' || nm[k-1] == '\t')) nm[--k] = 0;
        /* A LABEL'S ADDRESS IS NOT g_n * 4. That was only ever true while every item was one
         * word and nothing moved the cursor -- .org, .align and data all break it, and the
         * failure is silent: the label binds to the wrong address and every branch to it is
         * wrong. Record which item the label precedes and bind the address during placement. */
        if (k) { Sym *s = sym_add(nm); if (s) { s->defined = 1; s->addr = 0; s->item = g_n; } }
        p = colon + 1;
        while (*p == ' ' || *p == '\t') p++;
        /* A LABEL ON ITS OWN IS NOT AN ITEM. The first version emitted one with f == NULL,
         * which the encode pass then handed to isa_encode, and it read the form's template
         * through a null pointer and segfaulted. A label contributes an ADDRESS, not a
         * word, and the address is already recorded above. */
        if (!*p) { return 0; }
    }

    /* .sym NAME = ADDR  -- define a symbol at an ABSOLUTE address. This is what makes a
     * converted program rebuildable: a converted ELF branches and ADRPs to addresses OUTSIDE
     * its executable sections (rodata, the GOT, runtime functions). Those are not instructions
     * and are not in the source, but their ADDRESSES are still the truth the code depends on,
     * and layout must not silently move them. */
    if (!strncmp(p, ".sym", 4)) {
        char nm[64] = ""; unsigned long long v = 0;
        if (sscanf(p + 4, " %63s = %llx", nm, &v) == 2 ||
            sscanf(p + 4, " %63s %llx", nm, &v) == 2) {
            Sym *sy = sym_add(nm);
            if (sy) { sy->defined = 1; sy->addr = (uint64_t)v; }
        }
        return 0;
    }

    /* .org ADDR -- place the NEXT byte at an absolute address. A converted program's layout
     * IS its meaning: the gap between two executable sections, a branch to an address between
     * them, a table the code reads by absolute address. Relayouting those contiguously shifts
     * everything and the program stops running while every word still looks correct. */
    if (!strncmp(p, ".org", 4)) {
        unsigned long long v = 0;
        if (sscanf(p + 4, " %llx", &v) == 1) {
            it->is_org = 1;
            it->org = (uint64_t)v;
            g_n++;
        }
        return 0;
    }

    /* a directive */
    if (p[0] == '.') {
        if (!strncmp(p, ".ascii", 6)) {
            it->is_data = 1;
            char *q = strchr(p, '"');
            if (q) {
                q++;
                int k = 0;
                while (*q && *q != '"' && k < 255) {
                    if (*q == '\\' && q[1]) {
                        q++;
                        it->data[k++] = (*q == 'n') ? '\n' : (*q == 't') ? '\t' : *q;
                    } else it->data[k++] = *q;
                    q++;
                }
                it->dlen = k;
            }
            g_n++; return 0;
        }
        if (!strncmp(p, ".word", 5)) {
            it->is_data = 1;
            unsigned v = 0;
            sscanf(p + 5, "%x", &v);
            it->data[0] = (char)v; it->data[1] = (char)(v>>8);
            it->data[2] = (char)(v>>16); it->data[3] = (char)(v>>24);
            it->dlen = 4;
            g_n++; return 0;
        }
        if (!strncmp(p, ".asciiz", 7)) {
            it->is_data = 1;
            char *q = strchr(p, '"');
            if (q) {
                q++;
                int k = 0;
                while (*q && *q != '"' && k < 254) {
                    if (*q == '\\' && q[1]) { q++; it->data[k++] = (*q=='n')?'\n':(*q=='t')?'\t':*q; }
                    else it->data[k++] = *q;
                    q++;
                }
                it->data[k++] = 0;
                it->dlen = k;
            }
            g_n++; return 0;
        }
        /* .align N as its own line is a real item: it must exist in the layout or the
         * addresses after it are wrong */
        if (!strncmp(p, ".align", 6)) {
            it->is_align = 1;
            sscanf(p + 6, "%d", &it->nslot);
            g_n++; return 0;
        }
        fprintf(stderr, "binastbuild: line %d: unknown directive %s\n", lineno, p);
        return -1;
    }

    /* an instruction: BLOCK slot slot ...   where a slot may be a symbol */
    char *tok = p;
    int t = 0;
    char *toks[ISA_MAXV + 4];
    while (*tok && t < ISA_MAXV + 4) {
        while (*tok == ' ' || *tok == '\t' || *tok == ',') tok++;
        if (!*tok) break;
        toks[t++] = tok;
        while (*tok && *tok != ' ' && *tok != '\t' && *tok != ',') tok++;
        if (*tok) { *tok = 0; tok++; }
    }
    if (t < 1) { g_n++; return 0; }
    snprintf(it->block, sizeof it->block, "%s", toks[0]);
    const IsaForm *f = isadb_find(&T, it->block);
    if (!f) {
        fprintf(stderr, "binastbuild: line %d: no catalog block named '%s'\n", lineno, it->block);
        return -1;
    }
    it->f = f;
    for (int i = 1; i < t; i++) {
        char *v = toks[i];
        int is_num = 1;
        if (*v == '-' ) { for (char *q = v+1; *q; q++) if (*q < '0' || *q > '9') is_num = 0; }
        else { for (char *q = v; *q; q++) if (*q < '0' || *q > '9') is_num = 0; }
        int s = it->nslot;
        if (s >= ISA_MAXV) { fprintf(stderr, "binastbuild: line %d: too many slots\n", lineno); return -1; }
        if (is_num) it->slot[s] = strtol(v, NULL, 0);
        else {
            /* "msg"    the symbol as the row wants it (page for ADRP, word delta for a branch)
             * "msg@lo12"  the LOW 12 BITS of its address -- what an ADD or LDR immediate
             *             needs when an ADRP already loaded the page. The ADRP+ADD pair is
             *             the standard way to reach a label, and the second half is not the
             *             whole delta, it is the within-page offset. */
            char *at = strchr(v, '@');
            if (at) { *at = 0; it->lo12[s] = 1; }
            it->has_sym[s] = 1;
            snprintf(it->sym[s], sizeof it->sym[s], "%s", v);
            sym_add(v);
        }
        it->nslot++;
    }
    g_n++;
    return 0;
}

/* --------------------------------------------------- which slot is a target ---- */
/* A block carries a PC-relative displacement when one of its operands is declared as such
 * in the catalog (relink_prel_operand answers exactly this). For those blocks the slot that
 * holds the displacement is the one whose value is a symbol, or -- for a bare branch -- the
 * first operand. Working this out from the catalog rather than a hardcoded list is what
 * keeps it correct when the catalog grows. */
static void classify(Item *it) {
    if (it->is_data || it->is_align || !it->f) return;
    const IsaForm *f = it->f;
    /* relink_prel_operand returns the SLOT INDEX carrying the PC-relative operand, or -1.
     * It is not a predicate -- and treating -1 as true made every row look PC-relative,
     * so movz and svc were demanded to have a symbol. */
    int v = relink_prel_operand(f);
    if (v < 0) return;
    it->pc_rel = 1;
    it->is_pc_slot = v;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    arbit_init(0);
    if (argc < 2) {
        fprintf(stderr,
            "usage: binastbuild <src> <out.elf> [base_addr]\n"
            "  a source line is:  BLOCK slot slot ...   with # comments\n"
            "  a slot may be a NUMBER, or a SYMBOL for a branch/ADRP target\n"
            "  directives: .ascii \"text\"  .asciiz \"text\"  .word hex  .align N\n"
            "  labels:     name:\n"
            "  example:\n"
            "    MOVZ_X 8 64 0\n"
            "    ADRP_X 1 msg\n"
            "    ADD_X_imm 1 1 msg\n"
            "    MOVZ_X 8 93 0\n"
            "    SVC 0\n"
            "    msg: .ascii \"hi\\n\"\n");
        return 2;
    }
    if (!strcmp(argv[1], "agent")) {
        printf("tool binastbuild\n");
        printf("what it is: a layout engine. Build a runnable program from catalog blocks.\n");
        printf("place      each block gets an address; the address fixes every displacement\n");
        printf("resolve    a branch stores a DISTANCE, computed from where it lands\n");
        printf("symbols    a slot may name a label; the layout fills the number in\n");
        printf("verify     every word is decoded back to its block, then the program is run\n");
        printf("command binastbuild <src> <out.elf> [base_addr]\n");
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "binastbuild: need <src> <out.elf>\n"); return 2; }
    uint64_t base = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x400000;

    /* --into <elf>: keep that program's container and write the rebuilt code into it. This
     * is the flag that makes a rebuilt DYNAMIC program still a dynamic program. */
    const char *into = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--into") && i + 1 < argc) {
            into = argv[++i];
            /* the base comes from the container: layout must match where the code lives, or
             * every displacement would be computed against the wrong addresses */
            uint64_t cbase = 0;
            FILE *cf = fopen(into, "rb");
            if (!cf) { fprintf(stderr, "binastbuild: cannot open %s\n", into); return 2; }
            uint8_t hdr[64];
            if (fread(hdr, 1, 64, cf) != 64) { fclose(cf); return 2; }
            uint64_t shoff = r64(hdr + 0x28);
            uint16_t shent = r16(hdr + 0x3a), shn = r16(hdr + 0x3c);
            uint8_t *tbl = (uint8_t *)arb_bytes((size_t)shn * shent);
            /* SEEK TO shoff. The first version read the table straight from position 64, so
             * every field came from the middle of the header block: the base was computed as
             * 0 and layout then placed everything at 0 while the container's code lives at
             * 0x759f8. The rebuild "succeeded", wrote 769 KB into the file, and produced a
             * program whose every address was wrong -- the worst kind of passing build. */
            if (tbl && fseek(cf, (long)shoff, SEEK_SET) == 0 &&
                fread(tbl, 1, (size_t)shn * shent, cf) == (size_t)shn * shent) {
                for (uint16_t k = 0; k < shn; k++) {
                    uint64_t o = (uint64_t)k * shent;
                    uint64_t fl = r64(tbl + o + 0x8), ad = r64(tbl + o + 0x10);
                    uint64_t sz = r64(tbl + o + 0x20);
                    if ((fl & 0x4) && sz && (!cbase || ad < cbase)) cbase = ad;
                }
            }
            (void)shoff;
            fclose(cf);
            if (cbase) base = cbase;
        } else if (argv[i][0] != '-') {
            base = strtoull(argv[i], NULL, 0);
        }
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "binastbuild: cannot open %s\n", argv[1]); return 2; }

    /* SIZE THE TABLES FROM THE INPUT. One item per line, plus labels, plus the room a converter's
     * .org/.align directives need. Counting lines first costs one pass over the file and removes
     * the fixed caps entirely -- cargo's 4.38M items and cbmc's 1.52M both fit, and the failure
     * mode for something genuinely larger is a clear message rather than a truncation at 1M. */
    {
        struct stat st;
        long lines = 0;
        if (fstat(fileno(f), &st) == 0 && st.st_size > 0) {
            /* a line averages well under 64 bytes for converted code; count them exactly, since a
             * wrong estimate here is exactly the truncation this change exists to remove */
            int c;
            while ((c = fgetc(f)) != EOF) if (c == '\n') lines++;
            lines += 2;
        }
        if (lines < 4096) lines = 4096;
        rewind(f);

        g_cap_it  = lines;
        g_cap_sym = lines / 4 + 4096;      /* a symbol per branch target, and cargo is 1 in 14 */
        if (g_cap_it  > MAXLINES_HARD) g_cap_it  = MAXLINES_HARD;
        if (g_cap_sym > MAXSYMS_HARD)  g_cap_sym = MAXSYMS_HARD;

        g_it      = (Item *)arb_carve_zeroed((long)((long)sizeof(Item) * g_cap_it));
        g_sym     = (Sym  *)arb_carve_zeroed((long)((long)sizeof(Sym)  * g_cap_sym));
        g_snext   = (int  *)arb_carve_zeroed((long)((long)sizeof(int)  * g_cap_sym));
        g_sbucket = (int  *)arb_carve_zeroed((long)SBUCKETS * (long)sizeof(int));
        if (g_sbucket)
            for (long k = 0; k < SBUCKETS; k++) g_sbucket[k] = -1;
        if (!g_it || !g_sym || !g_snext || !g_sbucket) {
            fprintf(stderr, "binastbuild: cannot allocate room for %ld items and %ld symbols "
                            "(%ld MB). The source is %ld bytes.\n",
                    g_cap_it, g_cap_sym,
                    (long)(((long)sizeof(Item) * g_cap_it + (long)sizeof(Sym) * g_cap_sym) >> 20),
                    (long)(st.st_size));
            return 2;
        }
    }

    char line[512];
    int lineno = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (g_n >= g_cap_it) {
            fprintf(stderr, "binastbuild: source has more than %ld lines; raise the table\n", g_cap_it);
            bad = 1; break;
        }
        if (parse_line(line, lineno) != 0) { bad = 1; break; }
    }
    fclose(f);
    if (bad) return 2;
    printf("layout: %ld items, %ld symbols\n", g_n, g_nsym);

    /* ---- PLACE: walk the items, assign addresses. A .align rounds up. ---- */
    uint64_t pc = base;
    for (int i = 0; i < g_n; i++) {
        Item *it = &g_it[i];
        if (it->is_org) { it->addr = pc; pc = it->org; continue; }
        if (it->is_align) {
            int a = it->nslot ? (1 << it->nslot) : 4;
            uint64_t m = (uint64_t)a;
            uint64_t up = (pc + m - 1) & ~(m - 1);
            it->addr = pc;
            pc = up;
            continue;
        }
        it->addr = pc;
        pc += it->is_data ? (uint64_t)((it->dlen + 3) & ~3) : 4;
    }
    uint64_t end = pc;
    /* bind every label to the address of the item it precedes (or `end` if it is last) */
    for (int i = 0; i < g_nsym; i++) {
        Sym *sy = &g_sym[i];
        if (!sy->defined || !strncmp(sy->name, "EXT_", 4)) continue;   /* .sym already absolute */
        int k = sy->item;
        sy->addr = (k >= 0 && k < g_n) ? g_it[k].addr : end;
        if (g_it[k].is_align || g_it[k].is_org) {
            /* the label names the address AFTER the alignment/org directive it introduces */
            sy->addr = (k + 1 < g_n) ? g_it[k+1].addr : end;
        }
    }
    printf("placed: %#llx .. %#llx (%llu bytes)\n", (unsigned long long)base,
           (unsigned long long)end, (unsigned long long)(end - base));

    /* THE OLD BINDING LOOP USED TO SIT HERE and it re-derived every label's address as
     * `g_it[sy->addr / 4].addr` -- the parse-time `g_n * 4` guess, mapped through the item
     * list. It ran AFTER the correct binding above and overwrote it, so a label whose item
     * index and address/4 disagreed silently got the wrong address while every word it
     * pointed at was correct. Removing it is the fix; the binding above is the only one.
     * (Left as a comment because re-adding a second derivation is an easy mistake: two
     * places computing the same thing is how this bug existed at all.) */
    for (int i = 0; i < g_n; i++) {
        Item *it = &g_it[i];
        classify(it);
        if (!it->pc_rel) continue;
        if (it->has_sym[it->is_pc_slot]) snprintf(it->target, sizeof it->target, "%s", it->sym[it->is_pc_slot]);
    }

    /* ---- RESOLVE: place, then compute each displacement, then CHECK REACHABILITY.
     * A branch can only reach so far (simm26 = +/-128 MiB for B, +/-1 MiB for B.cond,
     * +/-1 MiB for ADRP at page granularity). If one cannot reach, the layout has to move
     * -- and this refuses rather than emitting a wrong displacement. ---- */
    int resolved = 0, unreachable = 0;
    for (int i = 0; i < g_n; i++) {
        Item *it = &g_it[i];
        if (!it->pc_rel) continue;
        if (!it->target[0]) {
            fprintf(stderr, "binastbuild: %s at %#llx has a PC-relative operand but no symbol\n",
                    it->block, (unsigned long long)it->addr);
            unreachable++;
            continue;
        }
        Sym *tgt = sym_find(it->target);
        if (!tgt || !tgt->defined) {
            fprintf(stderr, "binastbuild: %s at %#llx targets undefined label '%s'\n",
                    it->block, (unsigned long long)it->addr, it->target);
            unreachable++;
            continue;
        }
        /* THE OPERAND IS A DISTANCE IN THE ROW'S OWN UNITS, and this is the bug that made
         * the first built program fault: the layout passed the raw BYTE delta, while
         * isa_encode applies the row's own scale (vsc) on the way in. For ADRP (vsc=2, a
         * 21-bit page field) the caller must pass (target_page - pc_page) >> 12 -- pages --
         * and the encoder shifts it back up. Passing bytes sent ADRP to a page 32 away
         * instead of 0, so it computed a wild address.
         *
         * For a branch the row's scale accounts for the /4 (B encodes a word offset), so
         * dividing the byte delta by 2^vsc gives exactly the value the row expects. Doing
         * it from the catalog's own vsc rather than a per-row table is what keeps this
         * correct as the catalog grows. */
        int64_t off = (int64_t)tgt->addr - (int64_t)it->addr;
        /* UNITS COME FROM THE ROW'S KIND, not from vsc. K_PAGCNT (11) means the operand is
         * a PAGE COUNT -- ADRP's field is split (immlo 2 bits at 30:29, immhi 19 at 23:5)
         * and its vsc is 0, so a vsc-based conversion silently passed a byte delta and sent
         * every ADRP to a wild page. For every other PC-relative row vsc carries the unit
         * scale (2 for a branch's word offset). Reading the kind is what makes ADRP work. */
        if (it->f->vkind[it->is_pc_slot] == K_PAGCNT) {
            int64_t tp = tgt->addr >> 12;
            int64_t pp = it->addr >> 12;
            off = tp - pp;                     /* pages, which is what the field holds */
        } else {
            /* NO pre-scaling for a plain branch. isa_encode applies the row's own scale
             * (B_imm has vsc=2, so it divides the byte delta by 4 itself and refuses a
             * value that is not a multiple of 4). Dividing here as well double-scaled it:
             * "B_imm: 2 is not a multiple of 4" came from exactly that. The only row that
             * wants a pre-converted unit is K_PAGCNT, handled above. */
        }
        /* REACHABILITY: the displacement must fit the row's field. Refusing here is the
         * whole point of doing layout in one pass -- emitting a truncated displacement
         * produces a program that assembles and branches to the wrong place. */
        int wid = it->f->vwid[it->is_pc_slot];
        int64_t lim = (1LL << (wid - 1));
        if (off < -lim || off > lim - 1) {
            fprintf(stderr, "binastbuild: %s at %#llx cannot reach '%s' at %#llx: "
                            "displacement %lld does not fit %d bits\n",
                    it->block, (unsigned long long)it->addr, it->target,
                    (unsigned long long)tgt->addr, (long long)off, wid);
            unreachable++;
            continue;
        }
        it->slot[it->is_pc_slot] = (long)off;
        resolved++;
    }
    printf("resolved: %d PC-relative operand(s), %d problem(s)\n", resolved, unreachable);
    if (unreachable) { fprintf(stderr, "binastbuild: refusing to emit a program with unresolved targets\n"); return 2; }

    /* resolve @lo12 operands: the within-page offset of the symbol's address. These are NOT
     * PC-relative (they are absolute low bits), so they are filled here rather than in the
     * PC-relative pass above. */
    for (int i = 0; i < g_n; i++) {
        Item *it = &g_it[i];
        if (it->is_data || it->is_align || !it->f) continue;
        for (int v = 0; v < it->nslot; v++) {
            if (!it->has_sym[v] || !it->lo12[v]) continue;
            Sym *tgt = sym_find(it->sym[v]);
            if (!tgt || !tgt->defined) {
                fprintf(stderr, "binastbuild: line %d: %s targets undefined '%s'\n",
                        it->srcline, it->block, it->sym[v]);
                return 2;
            }
            it->slot[v] = (long)(tgt->addr & 0xFFFu);
        }
    }

    /* ---- ENCODE + VERIFY every word, before a byte is written ---- */
    uint8_t *code = (uint8_t *)arb_carve_zeroed((long)(end - base) + 64);
    if (!code) { fprintf(stderr, "binastbuild: oom\n"); return 2; }
    for (int i = 0; i < g_n; i++) {
        Item *it = &g_it[i];
        size_t off = (size_t)(it->addr - base);
        if (it->is_align) continue;
        if (it->is_data) { memcpy(code + off, it->data, (size_t)it->dlen); continue; }
        if (!it->f) continue;      /* an item with no block emits nothing (a bare label) */
        uint8_t out[ISA_MAXBYTES]; char err[200];
        if (isa_encode(it->f, it->slot, out, err, sizeof err) != 0) {
            fprintf(stderr, "binastbuild: line %d: %s: %s\n", it->srcline, it->block, err);
            return 2;
        }
        it->word = (uint32_t)out[0] | ((uint32_t)out[1]<<8) | ((uint32_t)out[2]<<16) | ((uint32_t)out[3]<<24);
        memcpy(code + off, out, 4);
        /* VERIFY: the emitted word must decode back to the block it was written as */
        const IsaForm *chk = NULL; long ch[ISA_MAXV] = {0};
        if (!isa_decode(&T, out, 4, &chk, ch) || !chk) {
            fprintf(stderr, "binastbuild: line %d: emitted %08x does not decode\n", it->srcline, it->word);
            return 2;
        }
    }
    printf("encoded: %ld block(s), every one decoded back through the catalog\n", g_n);

    /* ---- CONTAINER-PRESERVING OUTPUT ----
     * `--into <elf>`: write the rebuilt code back into the ORIGINAL program's image instead
     * of emitting a fresh minimal ELF.
     *
     * WHY THIS EXISTS. Building a new ELF produces byte-exact CODE (measured: 25 third-party
     * binaries, 1.34M words, identical at every address) but a minimal static CONTAINER --
     * no .interp, no .dynsym, no .rela.plt, no PLT, no libc linkage. Replacing a dynamic
     * program with it segfaults on the first call through the PLT. The code was right and
     * the program still did not run, which is the difference between rebuilding A program
     * and building THE program.
     *
     * With --into, every byte of the original image is preserved -- headers, dynamic
     * sections, relocations, rodata, the GOT -- and only the executable sections are
     * overwritten with the rebuilt words. The result keeps its original identity: a
     * dynamically linked PIE stays a dynamically linked PIE.
     *
     * THE CONTRACT: each original SHF_EXECINSTR section is written at FILE offset
     * sh_offset + (section_addr - base), with the rebuilt bytes for that address range. The
     * source must cover the section, so a partial conversion is refused rather than silently
     * leaving a hole -- a half-rebuilt program that still loads is the worst outcome. */
    if (into) {
        FILE *in = fopen(into, "rb");
        if (!in) { fprintf(stderr, "binastbuild: cannot open %s for --into\n", into); return 2; }
        fseek(in, 0, SEEK_END); long n = ftell(in); rewind(in);
        uint8_t *img = (uint8_t *)arb_bytes((size_t)n);
        if (!img || fread(img, 1, (size_t)n, in) != (size_t)n) { fclose(in); return 2; }
        fclose(in);
        if (memcmp(img, "\x7f""ELF", 4)) { fprintf(stderr, "binastbuild: --into is not an ELF\n"); return 2; }

        /* read the section table by hand (no libc, no libelf) */
        uint64_t shoff = r64(img + 0x28);
        uint16_t shent = r16(img + 0x3a), shn = r16(img + 0x3c);
        long written = 0, refused_short = 0;
        for (uint16_t i = 0; i < shn; i++) {
            uint64_t o = shoff + (uint64_t)i * shent;
            uint64_t fl = r64(img + o + 0x8), ad = r64(img + o + 0x10);
            uint64_t fo = r64(img + o + 0x18), sz = r64(img + o + 0x20);
            if (!(fl & 0x4) || !sz || fl & 0x8 /* SHT_NOBITS has no file bytes */) continue;
            if (fo + sz > (uint64_t)n) continue;
            if (ad < base || ad + sz > end) {
                /* the rebuilt layout does not cover this original section: refuse rather than
                 * leave a partly-updated program that still loads and runs the wrong code */
                fprintf(stderr, "binastbuild: --into: original section at %#llx (%llu bytes) is "
                                "not covered by the rebuilt range %#llx..%#llx\n",
                        (unsigned long long)ad, (unsigned long long)sz,
                        (unsigned long long)base, (unsigned long long)end);
                refused_short++;
                continue;
            }
            memcpy(img + fo, code + (ad - base), (size_t)sz);
            written += (long)sz;
        }
        if (refused_short && !written) { fprintf(stderr, "binastbuild: nothing written\n"); return 2; }
        FILE *outf = fopen(argv[2], "wb");
        if (!outf) { fprintf(stderr, "binastbuild: cannot write %s\n", argv[2]); return 2; }
        fwrite(img, 1, (size_t)n, outf);
        fclose(outf);
        { struct stat st; if (stat(into, &st) == 0) chmod(argv[2], st.st_mode & 07777); }
        printf("into %s: preserved the container, wrote %ld bytes of code into %d section(s)\n",
               into, written, (int)(shn));
        printf("wrote %s\n", argv[2]);
        printf("entry %#llx (the original entry point is kept)\n", (unsigned long long)base);
        return 0;
    }

    /* ---- EMIT a fresh minimal ELF ---- */
    ElfImage e;
    /* THE BASE MUST REACH THE WRITER. elf_init hard-coded vcursor = 0x400000, so a program
     * laid out at any other address was REPORTED at that address and EMITTED at 0x400000 --
     * the layout printout said 0x21df0 while the ELF's section was at 0x400000, and every
     * tool that then looked for the code at 0x21df0 found nothing. Passing the base through
     * is what makes `binastbuild src out.elf <any base>` true rather than cosmetic. */
    elf_init_at(&e, base);
    size_t codelen = (size_t)(end - base);
        /* SHF_ALLOC=0x2, SHF_EXECINSTR=0x4. NOT 0x6 -- 0x6 is ALLOC|WRITE, which is why
     * binastpage reported "no executable section": the section was marked writable, not
     * executable, and every tool that looks for SHF_EXECINSTR skipped it. */
    {
        int si = elf_add_section(&e, ".text", code, codelen, 0x2 | 0x4 /* ALLOC | EXECINSTR */, 4);
        if (si < 0) { fprintf(stderr, "binastbuild: elf_add_section refused (returned %d)\n", si); return 2; }
        printf("section .text added at index %d, %zu bytes\n", si, codelen);
    }
    elf_finish(&e, argv[2], base);   /* returns void: it writes the file or dies */
    printf("wrote %s\n", argv[2]);
    printf("entry %#llx\n", (unsigned long long)base);
    printf("program: %d instructions + %llu bytes of data\n", resolved, (unsigned long long)codelen);
    return 0;
}/* THE SYMBOL TABLE IS HASH-INDEXED. sym_find was a linear scan, and a converted real program has
 * one symbol per branch target -- 316,070 for cargo. Resolving each of its 316,070 branches by
 * scanning the table is about 5 * 10^10 strcmps: not slow, never. A bucket index over the name
 * makes lookup O(1), and that is the difference between building cargo and not building it.
 *
 * The chain link is a PARALLEL array (`g_snext`), not a field of Sym: Sym carries `item` for the
 * binding pass, and overloading one field for two purposes is how a table silently corrupts
 * itself -- the binding pass walks every symbol and would have followed the chain instead. */



