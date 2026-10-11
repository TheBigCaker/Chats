/* binastconv.c -- CONVERT a program into blocks you can edit, rebuild and run.
 *
 * THE FOURTH VERB. "B'nasti can repair a program, or build one, or maintain one that has
 * been built or converted." The repairer and the layout engine exist. This is convert:
 * take ANY aarch64 ELF, disassemble its executable sections THROUGH THE CATALOG, and write
 * a .bld source that binastbuild can rebuild into a runnable program. Not a listing -- a
 * source. Round trip is the test: convert, rebuild, run, and compare the bytes.
 *
 * TWO THINGS MAKE IT A SOURCE RATHER THAN A LISTING:
 *
 * 1. DATAFLOW, NOT PRETTY PRINTING. A branch encodes a displacement, and a displacement is
 *    only meaningful relative to the address the code was linked at. Emitting
 *    "B_imm -37" produces a file that rebuilds to a program branching somewhere wrong the
 *    moment anything moves. So the converter walks the branches, turns each target into a
 *    LABEL, and emits "B_imm lab_4000a4". A label survives re-layout; a displacement does
 *    not. This is the difference between a disassembly and a relocatable source.
 *
 * 2. DATA IS LABELLED, NOT GUESSED. A word with no catalog row is not an instruction --
 *    it is a literal pool, a jump table, or padding. Those become labelled .word bytes at
 *    the exact address they occupied, so the layout is preserved instead of being
 *    "cleaned up" into something that no longer runs.
 *
 *   binastconv <elf> <out.bld> [--base 0xADDR] [--data]     (--data: emit literal runs)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "arbishim.h"
#include "isadb.h"
#include "ISA_AARCH64.inc"

/* The catalog, built here the way isadb.c builds it. Each translation unit that needs the
 * table constructs it from the same .inc -- there is one source of rows. */
static const IsaTable T_aarch64 = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

/* ---- the image and its executable sections ---- */
typedef struct { uint64_t off, size, addr; } Sec;

static uint8_t *g_img;
static size_t   g_img_n;
static Sec      g_sec[32];
static int      g_nsec;
static uint64_t g_base;
static uint64_t g_lo, g_hi;      /* the address range covered by executable sections */

static uint32_t r32_(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64_(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint16_t r16_(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- load: mirror binui's section logic (SHF_EXECINSTR only) ---- */
static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "binastconv: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n < 64) { fclose(f); fprintf(stderr, "binastconv: %s too small\n", path); return -1; }
    g_img = (uint8_t *)arb_bytes((size_t)n);
    if (!g_img || fread(g_img, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); fprintf(stderr, "binastconv: short read\n"); return -1;
    }
    g_img_n = (size_t)n;
    fclose(f);
    if (memcmp(g_img, "\x7f""ELF", 4)) { fprintf(stderr, "binastconv: not an ELF\n"); return -1; }

    uint64_t shoff = r64_(g_img + 0x28);
    uint16_t shentsize = r16_(g_img + 0x3a);
    uint16_t shnum = r16_(g_img + 0x3c);
    uint16_t shstrndx = r16_(g_img + 0x3e);
    if (shstrndx >= shnum || !shoff) { fprintf(stderr, "binastconv: no section table\n"); return -1; }
    uint64_t stroff = r64_(g_img + shoff + (uint64_t)shstrndx * shentsize + 0x18);

    for (uint16_t i = 0; i < shnum && g_nsec < 32; i++) {
        uint64_t o = shoff + (uint64_t)i * shentsize;
        uint32_t nm = (uint32_t)r32_(g_img + o);
        uint64_t fl = r64_(g_img + o + 0x8);
        uint64_t ad = r64_(g_img + o + 0x10);
        uint64_t of = r64_(g_img + o + 0x18);
        uint64_t sz = r64_(g_img + o + 0x20);
        if (!(fl & 0x4)) continue;    /* SHF_EXECINSTR */
        if (!sz || of + sz > g_img_n) continue;
        g_sec[g_nsec].off = of; g_sec[g_nsec].size = sz; g_sec[g_nsec].addr = ad;
        const char *sname = (const char *)(g_img + stroff + nm);
        (void)sname;
        g_nsec++;
    }
    if (!g_nsec) { fprintf(stderr, "binastconv: no executable section\n"); return -1; }
    g_lo = g_sec[0].addr; g_hi = g_sec[0].addr + g_sec[0].size;
    for (int i = 1; i < g_nsec; i++) {
        if (g_sec[i].addr < g_lo) g_lo = g_sec[i].addr;
        if (g_sec[i].addr + g_sec[i].size > g_hi) g_hi = g_sec[i].addr + g_sec[i].size;
    }
    return 0;
}

/* ---- the decoded stream: one entry per word address ---- */
typedef struct {
    uint64_t addr;
    uint32_t word;
    const IsaForm *f;       /* NULL = no catalog row: data */
    long     ops[ISA_MAXV];
    int      is_pc;         /* which operand slot is PC-relative, -1 = none */
} Instr;

static uint64_t g_ninstr;
static Instr   *g_ins;

static int pc_slot_of(const IsaForm *f) {
    for (int v = 0; v < (int)f->nvars; v++) {
        int k = f->vkind[v];
        /* the kinds that hold a displacement. K_PAGCNT (adrp) and the plain branch kinds;
         * a register or an immediate is not a target. */
        if (k == K_PAGCNT) return v;
    }
    /* a branch: the mnemonic starts with b, cb, tb -- and the row's operand is signed */
    /* a branch: the mnemonic starts with b/cb/tb AND the first operand is a signed
     * immediate. Both conditions matter -- "bic" starts with 'b' and is not a branch. */
    if (f->mnemonic && (f->mnemonic[0] == 'b' || !strncmp(f->mnemonic, "cb", 2)
                        || !strncmp(f->mnemonic, "tb", 2))
        && f->nvars >= 1 && (f->vkind[0] == K_SIMM)) return 0;
    return -1;
}

static void disassemble(void) {
    /* count first */
    uint64_t total = 0;
    for (int s = 0; s < g_nsec; s++) total += g_sec[s].size / 4;
    g_ins = (Instr *)arb_carve_zeroed((long)(total + 8) * (long)sizeof(Instr));
    if (!g_ins) { fprintf(stderr, "binastconv: out of memory for %llu words\n",
                            (unsigned long long)total); exit(2); }
    for (int s = 0; s < g_nsec; s++) {
        uint64_t nw = g_sec[s].size / 4;
        for (uint64_t i = 0; i < nw; i++) {
            const uint8_t *p = g_img + g_sec[s].off + i * 4;
            Instr *it = &g_ins[g_ninstr];
            it->addr = g_sec[s].addr + i * 4;
            it->word = r32_(p);
            it->is_pc = -1;
            const IsaForm *f = NULL; long ops[ISA_MAXV] = {0};
            if (isa_decode(&T_aarch64, p, 4, &f, ops) && f) {
                it->f = f;
                for (int v = 0; v < ISA_MAXV; v++) it->ops[v] = ops[v];
                it->is_pc = pc_slot_of(f);
            }
            g_ninstr++;
        }
    }
}

/* ---- symbol/label names, derived from the address so they are stable and readable ---- */
static void label_for(uint64_t addr, char *out, size_t n) {
    snprintf(out, n, "L_%llx", (unsigned long long)addr);
}

/* which addresses are branch targets -> they need labels. Collected BEFORE emitting, because
 * a forward branch must name a label that appears later in the file. */
static uint64_t *g_target;      /* internal: gets a label in the source */
static uint64_t  g_ntarget;
static uint64_t *g_ext;         /* external: gets a .sym at its absolute address */
static uint64_t  g_next;

static uint64_t target_of(const Instr *it) {
    if (it->is_pc < 0) return 0;
    long disp = it->ops[it->is_pc];
    /* K_PAGCNT (adrp) holds a page count; every other PC-relative row holds the value the
     * encoder wants, and the encoder scales by the row's own vsc. To recover a BYTE address
     * the layout must undo that scale, which is why isa_encode refuses a non-multiple. */
    /* isa_extract ALREADY scales: K_SIMM returns signext(enc,width) << vsc, and the default
     * kind returns enc << vsc. So the decoded operand is in BYTES already -- shifting by vsc
     * again here put every branch 3 words off on a binary whose most common branch is +1
     * (1 << 2 = 4 becomes 4 << 2 = 16 -- off by 12 bytes, exactly the observed error).
     *
     * K_PAGCNT is the one exception: adrp's field is a page count, so the byte address is
     * the page-aligned pc plus disp pages. */
    if (it->f->vkind[it->is_pc] == K_PAGCNT)
        return ((it->addr >> 12) << 12) + ((uint64_t)(int64_t)disp << 12);
    return (uint64_t)((int64_t)it->addr + (int64_t)disp);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void collect_targets(void) {
    g_target = (uint64_t *)arb_carve_zeroed((long)(g_ninstr + 8) * 8);
    g_ext    = (uint64_t *)arb_carve_zeroed((long)(g_ninstr + 8) * 8);
    if (!g_target || !g_ext) { fprintf(stderr, "binastconv: out of memory for targets\n"); exit(2); }
    for (uint64_t i = 0; i < g_ninstr; i++) {
        if (g_ins[i].is_pc < 0) continue;
        uint64_t t = target_of(&g_ins[i]);
        /* only a target INSIDE the code range is a label. A branch outside is a call into a
         * runtime we are not converting, and inventing a label for it would be a lie. */
        if (t >= g_lo && t < g_hi) g_target[g_ntarget++] = t;
        else g_ext[g_next++] = t;
    }
    qsort(g_ext, g_next, 8, cmp_u64);
    uint64_t ke = 0;
    for (uint64_t i = 0; i < g_next; i++)
        if (!ke || g_ext[i] != g_ext[ke-1]) g_ext[ke++] = g_ext[i];
    g_next = ke;
    qsort(g_target, g_ntarget, 8, cmp_u64);
    /* dedupe */
    uint64_t k = 0;
    for (uint64_t i = 0; i < g_ntarget; i++)
        if (!k || g_target[i] != g_target[k-1]) g_target[k++] = g_target[i];
    g_ntarget = k;
}

static int is_target(uint64_t a) {
    long lo = 0, hi = (long)g_ntarget - 1;
    while (lo <= hi) {
        long m = (lo + hi) / 2;
        if (g_target[m] == a) return 1;
        if (g_target[m] < a) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

/* ---- emit one instruction as "FORM op op op" ---- */
static void emit_instr(FILE *o, Instr *it) {
    char lab[40];
    if (is_target(it->addr)) { label_for(it->addr, lab, sizeof lab); fprintf(o, "%s:\n", lab); }
    fprintf(o, "    %-18s", it->f->name);
    for (uint32_t v = 0; v < it->f->nvars; v++) {
        if ((int)v == it->is_pc) {
            uint64_t t = target_of(it);
            if (t >= g_lo && t < g_hi) {
                label_for(t, lab, sizeof lab);
                fprintf(o, " %s", lab);           /* a label survives re-layout; a delta does not */
            } else {
                fprintf(o, " EXT_%llx", (unsigned long long)t);   /* outside the code: a .sym */
            }
        } else {
            fprintf(o, " %lld", (long long)it->ops[v]);
        }
    }
    if (it->f->nvars == 0) fprintf(o, " ");
    fprintf(o, "   # %08x\n", it->word);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: binastconv <elf> <out.bld> [--base 0xADDR] [--data]\n"
                        "  converts an aarch64 ELF into editable blocks that binastbuild can rebuild\n");
        return 2;
    }
    arbit_init(0);
    const char *path = argv[1], *out = argv[2];
    int want_base = 0, want_data = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--base") && i + 1 < argc) { g_base = (uint64_t)strtoull(argv[++i], NULL, 0); want_base = 1; }
        else if (!strcmp(argv[i], "--nodata")) want_data = 0;
    }
    if (load(path) != 0) return 2;
    if (!want_base) g_base = g_lo;
    printf("source    %s\n", path);
    printf("code      %d executable section(s), %llu bytes, %#llx .. %#llx\n",
           g_nsec, (unsigned long long)(g_hi - g_lo),
           (unsigned long long)g_lo, (unsigned long long)g_hi);
    printf("base      %#llx\n", (unsigned long long)g_base);

    disassemble();
    collect_targets();
    long ndata = 0;
    for (uint64_t i = 0; i < g_ninstr; i++) if (!g_ins[i].f) ndata++;
    printf("decoded   %llu words: %llu instructions, %ld data (no catalog row)\n",
           (unsigned long long)g_ninstr, (unsigned long long)(g_ninstr - (uint64_t)ndata), ndata);
    printf("branches  %llu distinct targets became labels\n", (unsigned long long)g_ntarget);

    FILE *o = fopen(out, "w");
    if (!o) { fprintf(stderr, "binastconv: cannot write %s\n", out); return 2; }
    fprintf(o, "# converted from %s by binastconv\n", path);
    fprintf(o, "# %llu words, %llu branch targets as labels. Rebuild: binastbuild %s out.elf %#llx\n",
            (unsigned long long)g_ninstr, (unsigned long long)g_ntarget, out,
            (unsigned long long)g_base);
    fprintf(o, "#\n# A branch operand here is a LABEL, not a displacement: a displacement only\n"
               "# means anything relative to the address this code was linked at, so it would\n"
               "# silently break the moment anything moved. That is the whole difference\n"
               "# between a listing and a source.\n");
    if (g_next) {
        fprintf(o, "#\n# EXTERNAL TARGETS: addresses this code reaches that are NOT in these\n"
                   "# sections -- rodata, the GOT, runtime functions. They are recorded as\n"
                   "# absolute symbols so layout cannot silently move them.\n");
        for (uint64_t i = 0; i < g_next; i++)
            fprintf(o, ".sym EXT_%llx = %llx\n", (unsigned long long)g_ext[i],
                    (unsigned long long)g_ext[i]);
    }
    fprintf(o, "\n");

    long emitted = 0, skipped_data = 0;
    int next_sec = 0;
    for (uint64_t i = 0; i < g_ninstr; i++) {
        Instr *it = &g_ins[i];
        /* a gap between executable sections is PART OF THE LAYOUT: a branch or an ADRP can
         * target an address inside that gap. Relayouting contiguously would shift everything
         * and the rebuilt program would not run while every word still looked correct. */
        if (next_sec < g_nsec && it->addr >= g_sec[next_sec].addr + g_sec[next_sec].size) {
            while (next_sec < g_nsec && it->addr >= g_sec[next_sec].addr + g_sec[next_sec].size) next_sec++;
            if (next_sec < g_nsec)
                fprintf(o, "    .org %llx        # next executable section\n",
                        (unsigned long long)g_sec[next_sec].addr);
        }
        if (!it->f) {
            if (!want_data) { skipped_data++; continue; }
            /* DATA IS LABELLED, NOT GUESSED. Keep the exact bytes at the exact address. */
            if (is_target(it->addr)) { char lab[40]; label_for(it->addr, lab, sizeof lab); fprintf(o, "%s:\n", lab); }
            fprintf(o, "    .word %08x        # not an instruction: literal or jump table\n", it->word);
            emitted++;
            continue;
        }
        emit_instr(o, it);
        emitted++;
    }
    fclose(o);
    printf("written   %s: %ld items", out, emitted);
    if (skipped_data) printf(" (%ld data words skipped)", skipped_data);
    printf("\n");
    return 0;
}
