/* binast_norm.c -- relocation-aware tokenization: the build-invariance layer.
 *
 * THE PROBLEM. A word stream read straight out of a binary is not a property of
 * the program; it is a property of the program AND its build. A branch stores a
 * DISTANCE, so the same source line assembles to different words depending on
 * where the target landed and where the instruction sits. ADRP stores a count of
 * 4KiB pages from the instruction's own page, so it changes with the load address
 * as well. Feed those raw as tokens and a model learns build layout -- addresses
 * that mean nothing -- as if it were content.
 *
 * THE FIX, stated exactly. For every instruction whose row has a PC-relative
 * operand (a branch offset, or ADRP's page count):
 *
 *   resolved_target  = pc + displacement            (or the page form for ADRP)
 *   canonical_word   = word with the PC-relative field's bits CLEARED
 *
 * Clearing is the whole trick, and it is exact rather than approximate: the only
 * bits that vary with layout are precisely the bits of that one field, so removing
 * them yields a token that is invariant under ANY relocation -- a block move, a
 * rebase, ASLR -- while every other bit of the instruction is untouched. The
 * displacement is not thrown away; it moves to a payload channel as a resolved
 * TARGET, which is the part that carries meaning.
 *
 * So one instruction becomes:
 *
 *   canonical_word   the form, with the position-dependent field removed
 *   target_class     what the operand pointed at (same section / a symbol / other)
 *   target           the resolved absolute destination
 *
 * and reconstruction is arithmetic, not a lookup: displacement = target - pc, so
 * the exact original word is recoverable. `verify` proves that on every
 * PC-relative instruction in a real binary, and `drift` proves the invariance
 * experimentally by moving a block and counting how many tokens change.
 *
 * THE EXPERIMENT (`drift`). Take a block, relocate it with the relink machinery
 * (which already retargets PC-relative operands and verifies them), then compare
 * the block's own tokens before and after. Raw words are expected to change
 * wherever an operand pointed outside the block or across a page. Canonical words
 * must change NOWHERE. That is the measurement of how much of the token
 * distribution was build noise.
 *
 *   binast_norm stats  <elf>              PC-relative count, classes, symbol coverage
 *   binast_norm stream <elf> [--raw]      the token stream (canonical by default)
 *   binast_norm verify <elf>              canonical + target reconstructs every word
 *   binast_norm drift  <elf> <off> <len> <dst>   relocation invariance, measured
 *   binast_norm agent
 *
 * Pure C11, no dependencies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "isadb.h"
#include "relink.h"

#define ISA_INC_AARCH64 "ISA_AARCH64.inc"
#include ISA_INC_AARCH64

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"
static const IsaTable T = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

#define MAXSEC  64
#define MAXSYM  200000

typedef struct { char name[40]; uint64_t addr, off, size; uint64_t flags, type; } Sec;
typedef struct { uint64_t value, size; uint32_t nameoff; uint8_t info; } Sym;

static uint8_t *g_img = NULL;
static size_t   g_n = 0;
static Sec      g_sec[MAXSEC];
static int      g_nsec = 0;
static Sym     *g_sym = NULL;
static int      g_nsym = 0;
static char    *g_str = NULL;
static size_t   g_strn = 0;

static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int cmp_sym(const void *a, const void *b) {
    uint64_t x = ((const Sym *)a)->value, y = ((const Sym *)b)->value;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int elf_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "binast_norm: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 64) { fclose(f); fprintf(stderr, "binast_norm: too small\n"); return -1; }
    g_img = arb_bytes((size_t)sz);
    if (!g_img || fread(g_img, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
    fclose(f);
    g_n = (size_t)sz;
    if (memcmp(g_img, "\x7f" "ELF", 4)) { fprintf(stderr, "binast_norm: not an ELF\n"); return -1; }

    uint64_t shoff = r64(g_img + 0x28);
    int shnum = r16(g_img + 0x3c), shent = r16(g_img + 0x3a);
    int shstrndx = r16(g_img + 0x3e);
    const char *shstr = NULL;
    if (shstrndx > 0 && shstrndx < shnum) {
        const uint8_t *sh = g_img + shoff + (uint64_t)shstrndx * shent;
        shstr = (const char *)(g_img + r64(sh + 0x18));
    }
    g_nsec = 0;
    for (int i = 0; i < shnum && g_nsec < MAXSEC; i++) {
        const uint8_t *sh = g_img + shoff + (uint64_t)i * shent;
        uint32_t nameoff = r32(sh);
        uint64_t type = r32(sh + 4), flags = r64(sh + 8);
        uint64_t addr = r64(sh + 0x10), off = r64(sh + 0x18), size = r64(sh + 0x20);
        if (!size || off >= g_n) continue;
        if (off + size > g_n) size = g_n - off;
        Sec *s = &g_sec[g_nsec++];
        s->addr = addr; s->off = off; s->size = size; s->flags = flags; s->type = type;
        s->name[0] = 0;
        if (shstr && nameoff) snprintf(s->name, sizeof s->name, "%s", shstr + nameoff);

        /* symbol tables: SHT_SYMTAB (2) and SHT_DYNSYM (11) */
        if (type == 2 || type == 11) {
            uint64_t link = r32(sh + 0x28);
            if (link < (uint64_t)shnum) {
                const uint8_t *strsh = g_img + shoff + link * shent;
                g_str = (char *)(g_img + r64(strsh + 0x18));
                g_strn = (size_t)r64(strsh + 0x20);
            }
            int cnt = (int)(size / 24);
            if (!g_sym) {
                g_sym = arb_bytes((size_t)(cnt > 0 ? cnt : 1) * sizeof *g_sym);
                if (g_sym) {
                    for (int k = 0; k < cnt; k++) {
                        const uint8_t *e = g_img + off + (uint64_t)k * 24;
                        g_sym[g_nsym].nameoff = r32(e);
                        g_sym[g_nsym].info    = e[4];
                        g_sym[g_nsym].value   = r64(e + 8);
                        g_sym[g_nsym].size    = r64(e + 0x10);
                        g_nsym++;
                    }
                }
            }
        }
    }
    if (g_sym && g_nsym) qsort(g_sym, (size_t)g_nsym, sizeof *g_sym, cmp_sym);
    return 0;
}

/* ---- the PC-relative operand model ---- */

/* The bits that vary with layout, for a PC-relative operand. */
static uint64_t prel_mask(const IsaForm *f, int v) {
    uint64_t m = 0;
    int lo = f->vbyte[v] * 8 + f->vbit[v];
    for (int i = 0; i < f->vwid[v]; i++) m |= 1ull << (lo + i);
    if (f->vkind[v] == K_PAGCNT) {
        int slo = f->split_byte * 8 + f->split_bit;
        for (int i = 0; i < f->split_wid; i++) m |= 1ull << (slo + i);
    }
    return m;
}

static uint32_t word_of(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static void put_word(uint8_t *b, uint32_t w) {
    b[0] = (uint8_t)w; b[1] = (uint8_t)(w >> 8); b[2] = (uint8_t)(w >> 16); b[3] = (uint8_t)(w >> 24);
}

/* Resolved destination of a PC-relative operand. */
static uint64_t prel_target(const IsaForm *f, int v, long human, uint64_t pc) {
    if (f->vkind[v] == K_PAGCNT)
        return (pc & ~(uint64_t)0xfff) + (uint64_t)human * 4096u;
    return pc + (uint64_t)human;
}

/* Is this address inside this section? */
static int in_sec(const Sec *s, uint64_t a) { return a >= s->addr && a < s->addr + s->size; }

/* The section containing an address, or NULL. */
static const Sec *sec_of(uint64_t a) {
    for (int i = 0; i < g_nsec; i++) if (in_sec(&g_sec[i], a)) return &g_sec[i];
    return NULL;
}

/* Nearest preceding symbol at or before `a`, within a sane delta. */
static const Sym *sym_at(uint64_t a, uint64_t window) {
    if (!g_sym || !g_nsym) return NULL;
    int lo = 0, hi = g_nsym - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_sym[mid].value <= a) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return NULL;
    /* a zero-value symbol is not an anchor */
    if (g_sym[best].value == 0) return NULL;
    if (a - g_sym[best].value > window) return NULL;
    return &g_sym[best];
}

static const char *sym_name(const Sym *s) {
    if (!s || !g_str || s->nameoff >= g_strn) return "";
    return g_str + s->nameoff;
}

/* Target classes. Kept few and stable: this is a vocabulary a model sees. */
#define TC_NONE      0   /* not PC-relative at all */
#define TC_SAME_SEC  1   /* lands in the same code section */
#define TC_SYMBOL    2   /* resolves to a named symbol in this binary */
#define TC_EXTERNAL  3   /* elsewhere in the image, no symbol */
#define TC_OUTSIDE   4   /* outside every section (PLT-like, or unresolved) */

typedef struct {
    uint32_t raw;
    uint64_t target;
    int      klass;
    const IsaForm *form;
    int      prel_idx;
    long     human[ISA_MAXV];
} Tok;

/* Decode one instruction into a token. Returns bytes consumed, 0 if undecodable. */
static int tokenize_one(const uint8_t *code, int avail, uint64_t pc, Tok *t) {
    const IsaForm *f = NULL;
    long human[ISA_MAXV] = {0,0,0,0};
    int used = isa_decode(&T, code, avail, &f, human);
    if (!used) return 0;
    memset(t, 0, sizeof *t);
    t->raw = word_of(code);
    t->form = f;
    t->prel_idx = relink_prel_operand(f);
    memcpy(t->human, human, sizeof human);
    for (int v = 0; v < ISA_MAXV; v++) t->human[v] = human[v];
    if (t->prel_idx >= 0) {
        t->target = prel_target(f, t->prel_idx, human[t->prel_idx], pc);
        const Sec *sc = sec_of(t->target);
        if (!sc) t->klass = TC_OUTSIDE;
        else if (sc->flags & 0x4) t->klass = (sc == sec_of(pc)) ? TC_SAME_SEC : TC_SYMBOL;
        else t->klass = TC_EXTERNAL;
        /* a symbol at the target is the stronger statement */
        if (sym_at(t->target, 1u << 20)) t->klass = TC_SYMBOL;
    } else {
        t->klass = TC_NONE;
    }
    return used;
}

/* The canonical word: the raw word with the layout-dependent field cleared. */
static uint32_t canonical_word(const Tok *t) {
    if (t->prel_idx < 0) return t->raw;
    uint64_t m = prel_mask(t->form, t->prel_idx);
    return (uint32_t)(((uint64_t)t->raw) & ~m);
}

/* ------------------------------------------------------------------- commands */

static int code_sections(int *idx, int max) {
    int n = 0;
    for (int i = 0; i < g_nsec && n < max; i++)
        if ((g_sec[i].flags & 0x4) && (g_sec[i].type == 1)) idx[n++] = i;
    return n;
}

static int cmd_stats(const char *path) {
    if (elf_load(path) != 0) return 2;
    int idx[MAXSEC];
    int nsec = code_sections(idx, MAXSEC);
    unsigned long words = 0, prel = 0, undec = 0;
    unsigned long klass[8] = {0};
    unsigned long distinct_raw = 0, distinct_canon = 0;
    /* distinct sets via sorted arrays (bounded by word count) */
    size_t cap = 1 << 20;
    uint32_t *raws = arb_bytes(cap * sizeof *raws);
    uint32_t *cans = arb_bytes(cap * sizeof *cans);
    if (!raws || !cans) { arbit_release_carve((long)(raws)); arbit_release_carve((long)(cans)); return 2; }
    size_t nraw = 0, ncan = 0;
    unsigned long sym_resolved = 0;

    for (int s = 0; s < nsec; s++) {
        const Sec *sc = &g_sec[idx[s]];
        const uint8_t *code = g_img + sc->off;
        int len = (int)sc->size;
        for (int off = 0; off + 4 <= len; off += 4) {
            Tok t;
            int used = tokenize_one(code + off, len - off, sc->addr + (uint64_t)off, &t);
            words++;
            if (!used) { undec++; continue; }
            if (t.prel_idx >= 0) {
                prel++;
                if (t.klass < 8) klass[t.klass]++;
                if (sym_at(t.target, 1u << 20)) sym_resolved++;
            }
            if (nraw < cap) raws[nraw++] = t.raw;
            if (ncan < cap) cans[ncan++] = canonical_word(&t);
        }
    }
    /* distinct counts */
    uint32_t *tmp = arb_bytes((nraw ? nraw : 1) * sizeof *tmp);
    if (tmp) {
        memcpy(tmp, raws, nraw * sizeof *tmp);
        qsort(tmp, nraw, sizeof *tmp, (int (*)(const void *, const void *))cmp_sym);
        for (size_t i = 0; i < nraw; i++) if (!i || tmp[i] != tmp[i-1]) distinct_raw++;
        memcpy(tmp, cans, ncan * sizeof *tmp);
        qsort(tmp, ncan, sizeof *tmp, (int (*)(const void *, const void *))cmp_sym);
        for (size_t i = 0; i < ncan; i++) if (!i || tmp[i] != tmp[i-1]) distinct_canon++;
        arbit_release_carve((long)(tmp));
    }
    printf("file %s\ncode_sections %d\n", path, nsec);
    printf("words %lu\nundecodable %lu\n", words, undec);
    printf("pc_relative %lu\npc_relative_per_mille %lu\n",
           prel, words ? (prel * 1000) / words : 0);
    printf("class_same_section %lu\nclass_symbol %lu\nclass_external %lu\nclass_outside %lu\n",
           klass[TC_SAME_SEC], klass[TC_SYMBOL], klass[TC_EXTERNAL], klass[TC_OUTSIDE]);
    printf("targets_resolving_to_a_symbol %lu\n", sym_resolved);
    printf("symbols_loaded %d\n", g_nsym);
    printf("distinct_raw_words %lu\ndistinct_canonical_words %lu\n", distinct_raw, distinct_canon);
    /* a few resolved targets by name, so the class counts are inspectable */
    printf("-- sample resolved targets:\n");
    int shown = 0;
    for (int s = 0; s < nsec && shown < 8; s++) {
        const Sec *sc = &g_sec[idx[s]];
        const uint8_t *code = g_img + sc->off;
        int len = (int)sc->size;
        for (int off = 0; off + 4 <= len && shown < 8; off += 4) {
            Tok t;
            if (!tokenize_one(code + off, len - off, sc->addr + (uint64_t)off, &t)) continue;
            if (t.prel_idx < 0) continue;
            const Sym *sy = sym_at(t.target, 1u << 20);
            if (!sy) continue;
            printf("   %-18s +%-6d -> %s+%llu\n", t.form->name, off,
                   sym_name(sy), (unsigned long long)(t.target - sy->value));
            shown++;
        }
    }
    if (!shown) printf("   (no target resolved to a named symbol)\n");
    arbit_release_carve((long)(raws)); arbit_release_carve((long)(cans));
    return 0;
}

static int cmd_stream(const char *path, int raw) {
    if (elf_load(path) != 0) return 2;
    int idx[MAXSEC];
    int nsec = code_sections(idx, MAXSEC);
    for (int s = 0; s < nsec; s++) {
        const Sec *sc = &g_sec[idx[s]];
        const uint8_t *code = g_img + sc->off;
        int len = (int)sc->size;
        for (int off = 0; off + 4 <= len; off += 4) {
            Tok t;
            int used = tokenize_one(code + off, len - off, sc->addr + (uint64_t)off, &t);
            if (!used) continue;
            if (raw) printf("%08x\n", t.raw);
            else     printf("%08x\n", canonical_word(&t));
        }
    }
    return 0;
}

/* The invariant: canonical word + resolved target reconstructs the exact original
 * word, for every PC-relative instruction. Reconstruction is arithmetic --
 * displacement = target - pc -- so this proves the canonical form loses nothing. */
static int cmd_verify(const char *path) {
    if (elf_load(path) != 0) return 2;
    int idx[MAXSEC];
    int nsec = code_sections(idx, MAXSEC);
    unsigned long checked = 0, bad = 0, total = 0;
    for (int s = 0; s < nsec; s++) {
        const Sec *sc = &g_sec[idx[s]];
        const uint8_t *code = g_img + sc->off;
        int len = (int)sc->size;
        for (int off = 0; off + 4 <= len; off += 4) {
            uint64_t pc = sc->addr + (uint64_t)off;
            Tok t;
            int used = tokenize_one(code + off, len - off, pc, &t);
            if (!used) continue;
            total++;
            if (t.prel_idx < 0) continue;
            checked++;
            /* rebuild from the canonical word plus the resolved target */
            uint32_t cw = canonical_word(&t);
            long disp;
            if (t.form->vkind[t.prel_idx] == K_PAGCNT)
                disp = (long)((int64_t)t.target - (int64_t)(pc & ~(uint64_t)0xfff)) / 4096;
            else
                disp = (long)((int64_t)t.target - (int64_t)pc);
            long human2[ISA_MAXV];
            memcpy(human2, t.human, sizeof human2);
            human2[t.prel_idx] = disp;
            uint8_t enc[ISA_MAXBYTES];
            char err[200];
            /* start from the canonical word so only the cleared field is refilled */
            put_word(enc, cw);
            if (isa_encode(t.form, human2, enc, err, sizeof err) != 0) {
                if (bad < 5) printf("REBUILD FAILED %s at %#llx: %s\n", t.form->name,
                                    (unsigned long long)pc, err);
                bad++;
                continue;
            }
            if (word_of(enc) != t.raw) {
                if (bad < 5) printf("MISMATCH %s at %#llx: original %08x rebuilt %08x\n",
                                    t.form->name, (unsigned long long)pc, t.raw, word_of(enc));
                bad++;
            }
        }
    }
    printf("file %s\nwords %lu\npc_relative_checked %lu\nreconstruction_failures %lu\n",
           path, total, checked, bad);
    if (!bad) printf("every PC-relative word reconstructs exactly from its canonical form\n");
    return bad ? 1 : 0;
}

/* The experiment: relocate a block, then count how many of the block's own tokens
 * change. Raw tokens carry build layout, so they are expected to drift. Canonical
 * tokens must not move at all. This is the measured cost of NOT normalizing. */
static int cmd_drift(const char *path, uint64_t off, int len, uint64_t dst) {
    if (elf_load(path) != 0) return 2;
    int idx[MAXSEC];
    int nsec = code_sections(idx, MAXSEC);
    int which = -1;
    for (int s = 0; s < nsec; s++)
        if (off >= g_sec[idx[s]].addr && off + (uint64_t)len <= g_sec[idx[s]].addr + g_sec[idx[s]].size)
            which = idx[s];
    if (which < 0) { fprintf(stderr, "binast_norm: that range is not inside one code section\n"); return 2; }
    const Sec *sc = &g_sec[which];
    int fileoff = (int)(sc->off + (off - sc->addr));
    const uint8_t *code = g_img + fileoff;

    /* relocate with the relink machinery, which retargets and verifies */
    uint8_t *moved = arb_bytes((size_t)len);
    if (!moved) return 2;
    int outlen = 0;
    if (relink_move(&T, code, len, off, dst, moved, &outlen) != 0) {
        fprintf(stderr, "binast_norm: relink_move failed\n");
        arbit_release_carve((long)(moved));
        return 2;
    }
    if (relink_verify(&T, code, len, off, moved, dst) != 0) {
        fprintf(stderr, "binast_norm: relink_verify failed -- refusing to measure on it\n");
        arbit_release_carve((long)(moved));
        return 2;
    }

    unsigned long n = 0, raw_drift = 0, can_drift = 0, prel_here = 0;
    for (int o = 0; o + 4 <= len; o += 4) {
        Tok a, b;
        int ua = tokenize_one(code + o, len - o, off + (uint64_t)o, &a);
        int ub = tokenize_one(moved + o, len - o, dst + (uint64_t)o, &b);
        if (!ua || !ub) continue;
        n++;
        if (a.prel_idx >= 0) prel_here++;
        if (a.raw != b.raw) raw_drift++;
        if (canonical_word(&a) != canonical_word(&b)) {
            can_drift++;
            if (can_drift <= 5)
                printf("CANONICAL DRIFT at +%d: %08x -> %08x (%s)\n",
                       o, canonical_word(&a), canonical_word(&b), a.form->name);
        }
    }
    printf("file %s\nblock %#llx..%#llx moved to %#llx\n",
           path, (unsigned long long)off, (unsigned long long)(off + (uint64_t)len),
           (unsigned long long)dst);
    printf("instructions %lu\npc_relative_in_block %lu\n", n, prel_here);
    printf("raw_token_drift %lu\nraw_drift_per_mille %lu\n",
           raw_drift, n ? (raw_drift * 1000) / n : 0);
    printf("canonical_token_drift %lu\n", can_drift);
    printf("build_noise_removed %s\n", can_drift == 0 ? "yes -- canonical stream is relocation-invariant" : "NO");
    arbit_release_carve((long)(moved));
    return can_drift ? 1 : 0;
}

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc < 2) {
        fprintf(stderr,
            "usage: binast_norm <command>\n"
            "  stats  <elf>                     PC-relative count, classes, symbol coverage\n"
            "  stream <elf> [--raw]             token stream (canonical by default)\n"
            "  verify <elf>                     canonical + target reconstructs every word\n"
            "  drift  <elf> <off> <len> <dst>   relocation invariance, measured\n"
            "  agent\n");
        return 2;
    }
    const char *c = argv[1];
    if (!strcmp(c, "agent")) {
        printf("tool binast_norm\n");
        printf("what it is: relocation-aware tokenization; the build-invariance layer\n");
        printf("problem: a branch stores a distance and adrp a page count, so the same\n");
        printf("  source assembles to different words in different builds; raw tokens\n");
        printf("  teach a model that layout is content\n");
        printf("canonical_word: the word with the PC-relative field's bits cleared\n");
        printf("payload: resolved target + target class (same-section/symbol/external/outside)\n");
        printf("invariant: displacement = target - pc, so the exact word is recoverable\n");
        printf("command stats  <elf>\n");
        printf("command stream <elf> [--raw]\n");
        printf("command verify <elf>    reconstruction on every PC-relative instruction\n");
        printf("command drift  <elf> <off> <len> <dst>   tokens before/after a real move\n");
        return 0;
    }
    if (!strcmp(c, "stats") && argc > 2) return cmd_stats(argv[2]);
    if (!strcmp(c, "stream") && argc > 2) return cmd_stream(argv[2], argc > 3 && !strcmp(argv[3], "--raw"));
    if (!strcmp(c, "verify") && argc > 2) return cmd_verify(argv[2]);
    if (!strcmp(c, "drift") && argc > 5)
        return cmd_drift(argv[2], strtoull(argv[3], NULL, 0), atoi(argv[4]), strtoull(argv[5], NULL, 0));
    fprintf(stderr, "binast_norm: bad command or missing args\n");
    return 2;
}
