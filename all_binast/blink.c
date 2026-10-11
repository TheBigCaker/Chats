/* blink -- "blink" the binary: read raw binary, patch or relocate it, write ELF.
 *
 * The front end for the linker: everything here is bytes in, bytes out. Commands
 *
 *   blink info    <elf>                       sections, entry, dynamic symbols
 *   blink symbols <elf>                       symbol table with addresses
 *   blink code    <elf> [--hex]               the code block as catalog tokens
 *   blink patch   <elf> <vaddr> <word> <out>  place one instruction
 *   blink addcode <elf> <src.asm> <out>       assemble a catalog block and APPEND it
 *   blink relink  <elf> <vaddr> <len> <dst> <out>   move a block, fix PC-relative
 *   blink run     <elf>                       exec it and report the exit status
 *
 * Pure C11, no dependencies. Uses isadb.h for encode/decode and relink.h for the
 * relocation pass. `addcode` reads a text block of "<FORM> <ops...>" lines, so an
 * agent composes a routine by naming catalog forms -- no assembler, no compiler.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "isadb.h"

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"
#include "relink.h"

#include "ISA_AARCH64.inc"
static const IsaTable T = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

#define MAXSEC 64
typedef struct { char name[32]; uint64_t addr, off, size, flags; uint32_t type; } Sec;

typedef struct {
    uint8_t *raw; size_t n;
    uint64_t entry, phoff, shoff;
    int phnum, phentsize;
    Sec sec[MAXSEC]; int nsec;
} Elf;

static void die(const char *m) { fprintf(stderr, "blink: %s\n", m); exit(1); }

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff); }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff); }

static void elf_open(Elf *e, const char *path) {
    memset(e, 0, sizeof *e);
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open ELF");
    fseek(f, 0, SEEK_END); e->n = (size_t)ftell(f); rewind(f);
    e->raw = arb_bytes(e->n);
    if (fread(e->raw, 1, e->n, f) != e->n) die("short read");
    fclose(f);
    if (e->n < 64 || memcmp(e->raw, "\x7f" "ELF", 4)) die("not an ELF");
    e->entry = rd64(e->raw + 0x18);
    e->phnum = rd16(e->raw + 0x38);
    e->phentsize = rd16(e->raw + 0x36);
    e->phoff = rd64(e->raw + 0x20);
    e->shoff = rd64(e->raw + 0x28);
    uint16_t shents = rd16(e->raw + 0x3a), shnum = rd16(e->raw + 0x3c), shstr = rd16(e->raw + 0x3e);
    if (shents != 64) die("unexpected section header size");
    const uint8_t *strsh = e->raw + e->shoff + (uint64_t)shstr * 64;
    uint64_t stroff = rd64(strsh + 0x18);
    const char *str = (const char *)(e->raw + stroff);
    for (int i = 0; i < shnum && e->nsec < MAXSEC; i++) {
        const uint8_t *sh = e->raw + e->shoff + (uint64_t)i * 64;
        uint32_t no = rd32(sh);
        Sec *s = &e->sec[e->nsec];
        snprintf(s->name, sizeof s->name, "%s", str + no);
        s->type = rd32(sh + 0x04);
        s->flags = rd64(sh + 0x08);
        s->addr = rd64(sh + 0x10);
        s->off = rd64(sh + 0x18);
        s->size = rd64(sh + 0x20);
        e->nsec++;
    }
}

/* The segment that holds executable code: the first PT_LOAD with the X flag.
 * Section headers cannot be trusted for this -- a linker-produced ELF can carry a
 * section table that points outside the file, and reading there decodes file garbage
 * as instructions. The program headers are what the loader itself uses. */
static int code_region(const Elf *e, uint64_t *off, uint64_t *va, size_t *len) {
    for (int i = 0; i < e->phnum; i++) {
        const uint8_t *ph = e->raw + e->phoff + (uint64_t)i * e->phentsize;
        if (rd32(ph) != 1) continue;             /* PT_LOAD */
        uint32_t flags = rd32(ph + 4);
        if (!(flags & 1)) continue;              /* PF_X */
        uint64_t o = rd64(ph + 8), a = rd64(ph + 16), fs = rd64(ph + 32);
        if (!fs) continue;
        if (o + fs > e->n) {                     /* clamp: never read past EOF */
            if (o >= e->n) continue;
            fs = e->n - o;
        }
        *off = o; *va = a; *len = (size_t)fs;
        return 0;
    }
    return -1;
}

static const Sec *sec_at(const Elf *e, uint64_t va) {
    for (int i = 0; i < e->nsec; i++) {
        const Sec *s = &e->sec[i];
        if (s->size && s->addr <= va && va < s->addr + s->size) return s;
    }
    return NULL;
}

static const Sec *sec_named(const Elf *e, const char *n) {
    for (int i = 0; i < e->nsec; i++) if (!strcmp(e->sec[i].name, n)) return &e->sec[i];
    return NULL;
}

static int to_file_off(const Elf *e, uint64_t va, uint64_t *fo) {
    const Sec *s = sec_at(e, va);
    if (!s) return -1;
    *fo = s->off + (va - s->addr);
    return 0;
}

/* ---- commands ---- */

/* Section flags are three bits, printed in the order the ELF spec names them:
 * bit 0 W (writable), bit 1 A (allocated in memory at load), bit 2 X (executable).
 * Decoding them in any other order produces a table that looks plausible and means
 * nothing: .text would read "-AX" only by luck. */
static void sec_flags_str(uint64_t f, char out[4]) {
    out[0] = (f & 1) ? 'W' : '-';
    out[1] = (f & 2) ? 'A' : '-';
    out[2] = (f & 4) ? 'X' : '-';
    out[3] = 0;
}

/* Why a section header cannot be believed. A section table is advisory: a hand-made
 * or stripped ELF can carry one whose offsets run past the end of the file or that
 * overlap each other and the header itself. Printing such a row without saying so
 * invites reading data at an address that holds something else. */
static const char *sec_problem(const Elf *e, const Sec *s) {
    if (s->type == 8) return NULL;                      /* SHT_NOBITS has no bytes */
    if (s->size == 0) return NULL;
    /* SHT_NULL / empty name is the required first entry */
    if (s->name[0] == '\0' && s->size == 0) return NULL;
    if (s->off >= e->n) return "offset past end of file";
    if (s->off + s->size > e->n) return "runs past end of file";
    if (s->off < 64) return "overlaps the ELF header";
    if (s->off < e->phoff + (uint64_t)e->phnum * e->phentsize) return "overlaps the program headers";
    if (s->off < e->shoff + 4096 && s->off + s->size > e->shoff) return "overlaps the section header table";
    return NULL;
}

static int cmd_info(const char *path) {
    Elf e; elf_open(&e, path);
    printf("entry   %#llx\ntype    %s\nphoff   %#llx  (%d x %d bytes)\nshoff   %#llx\ntotal   %zu bytes\n",
           (unsigned long long)e.entry,
           rd16(e.raw + 0x10) == 3 ? "DYN (shared object / PIE)" : rd16(e.raw + 0x10) == 2 ? "EXEC" : "other",
           (unsigned long long)e.phoff, e.phnum, e.phentsize, (unsigned long long)e.shoff, e.n);

    /* Segments first: these are what the loader obeys, and what every other command
     * here maps addresses through. */
    printf("\nPROGRAM HEADERS (authoritative)\n");
    printf("  %-16s %-10s %-14s %-14s %-10s %-10s %s\n",
           "type", "flags", "offset", "vaddr", "filesz", "memsz", "note");
    static const char *ptname[] = { "NULL", "LOAD", "DYNAMIC", "INTERP", "NOTE", "SHLIB", "PHDR", "TLS",
                                    "GNU_EH_FRAME", "GNU_STACK", "GNU_RELRO" };
    for (int i = 0; i < e.phnum; i++) {
        const uint8_t *ph = e.raw + e.phoff + (uint64_t)i * e.phentsize;
        uint32_t type = rd32(ph), fl = rd32(ph + 4);
        uint64_t off = rd64(ph + 8), va = rd64(ph + 16), fsz = rd64(ph + 32), msz = rd64(ph + 40);
        char flg[4] = { (fl & 4) ? 'R' : '-', (fl & 2) ? 'W' : '-', (fl & 1) ? 'X' : '-', 0 };
        printf("  %-16s %-10s %#014llx %#014llx %-10llu %-10llu %s\n",
               type < 11 ? ptname[type] : "?",
               flg, (unsigned long long)off, (unsigned long long)va,
               (unsigned long long)fsz, (unsigned long long)msz,
               (off + fsz > e.n) ? "(clamped to EOF when read)" : "");
    }

    printf("\nSECTION HEADERS (advisory)\n");
    printf("  %-20s %-9s %-14s %-10s %10s  %s\n", "name", "flags", "addr", "offset", "size", "note");
    int suspect = 0, shown = 0;
    for (int i = 0; i < e.nsec; i++) {
        const Sec *s = &e.sec[i];
        if (s->name[0] == '\0') continue;               /* the mandatory null entry */
        shown++;
        char fl[4]; sec_flags_str(s->flags, fl);
        const char *prob = sec_problem(&e, s);
        if (prob) suspect++;
        printf("  %-20s %-9s %#014llx %#010llx %10llu  %s%s%s\n", s->name, fl,
               (unsigned long long)s->addr, (unsigned long long)s->off, (unsigned long long)s->size,
               prob ? "UNTRUSTWORTHY: " : "", prob ? prob : "",
               (s->type == 8) ? " (NOBITS)" : "");
    }
    printf("\nflags: first letter W=write, second A=alloc at load, third X=execute\n");
    if (suspect)
        printf("%d of %d section(s) cannot be trusted; use the program headers above to map addresses.\n",
               suspect, shown);
    else
        printf("section table is consistent with the file.\n");
    arbit_release_carve((long)(e.raw));
    return 0;
}

static int cmd_symbols(const char *path) {
    Elf e; elf_open(&e, path);
    const Sec *sym = sec_named(&e, ".symtab"), *dyn = sec_named(&e, ".dynsym");
    const Sec *str = sec_named(&e, ".strtab");
    printf("%-34s %-18s %-8s %s\n", "name", "value", "size", "shndx");
    const Sec *syms[2] = { sym, dyn };
    for (int k = 0; k < 2; k++) {
        if (!syms[k]) continue;
        for (uint64_t off = 0; off + 24 <= syms[k]->size; off += 24) {
            const uint8_t *p = e.raw + syms[k]->off + off;
            uint32_t nameoff = rd32(p);
            uint64_t value = rd64(p + 8), size = rd64(p + 16);
            uint16_t ndx = rd16(p + 6);
            const char *table = str ? (const char *)(e.raw + str->off) : "";
            const char *nm = (str && nameoff < str->size) ? table + nameoff : "";
            if (!*nm && !value) continue;
            printf("%-34s %#018llx %-8llu %u\n", *nm ? nm : "<anon>",
                   (unsigned long long)value, (unsigned long long)size, ndx);
        }
    }
    arbit_release_carve((long)(e.raw));
    return 0;
}

static int cmd_code(const char *path, int hex) {
    Elf e; elf_open(&e, path);
    uint64_t coff = 0, cva = 0; size_t len = 0;
    if (code_region(&e, &coff, &cva, &len) != 0) die("no executable PT_LOAD segment");
    if (!len) die("empty code segment");
    const uint8_t *code = e.raw + coff;
    if (hex) {
        for (size_t i = 0; i < len; i += 4) {
            uint32_t w = rd32(code + i);
            printf("%08x\n", w);
        }
        arbit_release_carve((long)(e.raw));
        return 0;
    }
    printf("# code at %#llx (%zu bytes, from the executable PT_LOAD)\n", (unsigned long long)cva, len);
    size_t off = 0;
    while (off < len) {
        const IsaForm *f = NULL; long human[ISA_MAXV] = {0, 0, 0, 0};
        int used = isa_decode(&T, code + off, (int)(len - off), &f, human);
        if (!used) {
            printf("%#010llx %08x  ?? (no catalog form)\n", (unsigned long long)(cva + off), rd32(code + off));
            off += 4;
            continue;
        }
        printf("%#010llx %08x  %-16s", (unsigned long long)(cva + off), rd32(code + off), f->name);
        for (int v = 0; v < f->nvars; v++) printf(" %ld", human[v]);
        printf("\n");
        off += (size_t)used;
    }
    arbit_release_carve((long)(e.raw));
    return 0;
}

static int cmd_patch(const char *path, const char *va_s, const char *word_s, const char *out) {
    Elf e; elf_open(&e, path);
    uint64_t va = strtoull(va_s, NULL, 16);
    uint64_t fo = 0;
    if (to_file_off(&e, va, &fo) != 0) die("no section contains that address");
    uint8_t *img = arb_bytes(e.n); memcpy(img, e.raw, e.n);
    const IsaForm *f = NULL; long human[ISA_MAXV] = {0, 0, 0, 0};
    uint8_t b[ISA_MAXBYTES];
    uint32_t w = (uint32_t)strtoul(word_s, NULL, 16);
    wr32(b, w);
    int used = isa_decode(&T, b, 4, &f, human);
    printf("patch  %#llx  %08x -> %08x", (unsigned long long)va, rd32(e.raw + fo), w);
    if (used) printf("   (%s)\n", f->name); else printf("   (word not in catalog)\n");
    for (int i = 0; i < 4; i++) img[fo + i] = (uint8_t)((w >> (8 * i)) & 0xff);
    FILE *o = fopen(out, "wb"); fwrite(img, 1, e.n, o); fclose(o);
    chmod(out, 0755);
    arbit_release_carve((long)(img)); arbit_release_carve((long)(e.raw));
    return 0;
}

/* Extend .text with `blen` bytes: grow the section header and slide every section
 * that sits after .text in the file up by blen. Only the header table needs
 * fixups; program headers are left alone because .text's file and virtual
 * addresses stay put and nothing else moves in address space. */
static int elf_append_text(Elf *e, int shidx, const uint8_t *blob, int blen, uint8_t **img_out) {
    Sec *t = &e->sec[shidx];
    /* only shift sections that live after .text in the FILE */
    uint64_t t_end = t->off + t->size;
    uint8_t *img = arb_bytes(e->n + (size_t)blen);
    memcpy(img, e->raw, (size_t)t_end);
    memcpy(img + t_end, blob, (size_t)blen);
    uint64_t oldtail = t_end, newtail = t_end + (uint64_t)blen;

    /* copy the shifted tail and record each section's new offset */
    uint64_t delta = (uint64_t)blen;
    uint64_t tail_len = e->n - oldtail;
    memcpy(img + newtail, e->raw + oldtail, (size_t)tail_len);
    for (int i = 0; i < e->nsec; i++) {
        Sec *s = &e->sec[i];
        if (i == shidx || s->type == 8 || !s->off) continue;
        if (s->off >= t_end) {
            s->off += delta;
            uint8_t *sh = img + e->shoff + (uint64_t)i * 64;
            wr64(sh + 0x18, s->off);
        }
    }
    /* grow .text in the header */
    t->size += (uint64_t)blen;
    {
        uint8_t *sh = img + e->shoff + (uint64_t)shidx * 64;
        wr64(sh + 0x20, t->size);
    }
    /* the section header table moved too */
    e->shoff += delta;
    /* update the program header that covers the tail, if any: p_offset/p_filesz
     * must follow the shift for anything mapping past .text */
    uint16_t phnum = rd16(img + 0x38);
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = img + e->phoff + (uint64_t)i * 56;
        uint64_t poff = rd64(ph + 8), pvaddr = rd64(ph + 16), pfsz = rd64(ph + 32);
        if (poff + pfsz > oldtail && pfsz) { /* segment reaches past .text: relocate the tail part */
            /* simplest correct move for a PT_LOAD that contains both .text and the
             * tail: keep it, but extend filesz by blen so the inserted bytes are
             * mapped, and shift its data accordingly (already shifted above). */
            wr64(ph + 32, pfsz + delta);
            wr64(ph + 40, rd64(ph + 40) + delta);
        }
        (void)pvaddr;
    }
    *img_out = img;
    return 0;
}

/* Assemble a text block of catalog forms, append it to .text, and extend the
 * section. This is how an agent adds a routine without a compiler. */
static int cmd_addcode(const char *path, const char *srcfile, const char *out) {
    Elf e; elf_open(&e, path);
    const Sec *t = sec_named(&e, ".text");
    if (!t) die("no .text");
    FILE *f = fopen(srcfile, "r");
    if (!f) die("cannot open source block");
    char line[512];
    uint8_t blob[65536]; int blen = 0;
    int ninsn = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char form[64]; int k = 0;
        while (*p && !isspace((unsigned char)*p) && k < 63) form[k++] = *p++;
        form[k] = 0;
        const IsaForm *fm = isadb_find(&T, form);
        if (!fm) { fprintf(stderr, "blink: unknown form '%s'\n", form); fclose(f); return 1; }
        long human[ISA_MAXV] = {0, 0, 0, 0};
        for (int v = 0; v < fm->nvars; v++) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            char tok[64]; int j = 0;
            while (*p && !isspace((unsigned char)*p) && j < 63) tok[j++] = *p++;
            tok[j] = 0;
            /* symbolic: allow "here" for the current offset */
            if (!strcmp(tok, "here")) human[v] = blen;
            else human[v] = strtol(tok, NULL, 0);
        }
        uint8_t enc[ISA_MAXBYTES]; char err[200];
        if (isa_encode(fm, human, enc, err, sizeof err) != 0) {
            fprintf(stderr, "blink: %s\n", err); fclose(f); return 1;
        }
        memcpy(blob + blen, enc, fm->len);
        blen += fm->len;
        ninsn++;
    }
    fclose(f);

    /* locate .text's section index */
    int shidx = -1;
    for (int i = 0; i < e.nsec; i++) if (!strcmp(e.sec[i].name, ".text")) { shidx = i; break; }
    if (shidx < 0) die("no .text section header");

    uint8_t *img = NULL;
    uint64_t at_va = t->addr + t->size;
    if (elf_append_text(&e, shidx, blob, blen, &img) != 0) { fprintf(stderr, "blink: append failed\n"); return 1; }

    /* fix the program header: e_entry and the shoff live in the ELF header, which
     * elf_append_text updated in the image; patch the header copy too */
    wr64(img + 0x28, e.shoff);
    FILE *o = fopen(out, "wb");
    fwrite(img, 1, e.n + (size_t)blen, o);
    fclose(o);
    chmod(out, 0755);

    printf("addcode: %d instruction(s), %d bytes appended at %#llx (.text+%llu)\n",
           ninsn, blen, (unsigned long long)at_va, (unsigned long long)t->size);
    printf("         wrote %s\n", out);
    arbit_release_carve((long)(img));
    arbit_release_carve((long)(e.raw));
    return 0;
}

/* The linker command: move `len` bytes from `src` to `dst`, rewriting every
 * PC-relative operand so the code means the same thing at its new address. */
/* Map a virtual address to a file offset through the executable segment. The section
 * table is not usable for this: a linker-produced ELF can carry section headers that
 * point outside the file, and writing through them plants the block in the wrong
 * place while still reporting success. */
static int cmd_relink(const char *path, const char *src_s, const char *len_s,
                      const char *dst_s, const char *out) {
    Elf e; elf_open(&e, path);
    uint64_t seg_off = 0, seg_va = 0; size_t seg_len = 0;
    if (code_region(&e, &seg_off, &seg_va, &seg_len) != 0) die("no executable segment");
    uint64_t src = strtoull(src_s, NULL, 16);
    long len = strtol(len_s, NULL, 0);
    uint64_t dst = strtoull(dst_s, NULL, 16);
    if (len <= 0 || (uint64_t)len > seg_len) die("bad length");
    if (src < seg_va || src + (uint64_t)len > seg_va + seg_len) die("source range outside the code segment");
    if (dst < seg_va || dst + (uint64_t)len > seg_va + seg_len) die("destination range outside the code segment");
    uint64_t fo = seg_off + (src - seg_va);
    const uint8_t *code = e.raw + fo;

    uint8_t moved[65536];
    int moved_len = 0;
    if (relink_move(&T, code, (int)len, src, dst, moved, &moved_len) != 0) {
        fprintf(stderr, "blink: relink failed\n");
        return 1;
    }
    if (relink_verify(&T, code, (int)len, src, moved, dst) != 0) {
        fprintf(stderr, "blink: relink verification failed -- refusing to write output\n");
        return 1;
    }
    /* Plant the moved block at dst and replace the source with a single branch to it.
     * Filling the source with nops was wrong: anything that enters the block at its
     * old address -- a call, or the entry point itself -- would fall through the
     * nops into whatever follows and crash. A branch keeps the old address working. */
    uint8_t *img = arb_bytes(e.n); memcpy(img, e.raw, e.n);
    uint64_t dfo = seg_off + (dst - seg_va);
    memcpy(img + dfo, moved, (size_t)moved_len);
    if (len < 4) die("block too small to hold a branch");
    {
        const IsaForm *b = isadb_find(&T, "B_imm");
        if (!b) die("no branch form in the catalog");
        long ops[ISA_MAXV] = { (long)((int64_t)dst - (int64_t)src), 0, 0, 0 };
        uint8_t bb[ISA_MAXBYTES]; char err[200];
        if (isa_encode(b, ops, bb, err, sizeof err) != 0) {
            fprintf(stderr, "blink: source address %#llx cannot reach %#llx: %s\n",
                    (unsigned long long)src, (unsigned long long)dst, err);
            arbit_release_carve((long)(img)); arbit_release_carve((long)(e.raw)); return 1;
        }
        memcpy(img + fo, bb, b->len);
        const IsaForm *nop = isadb_find(&T, "NOP");
        for (long i = b->len; i + 4 <= len; i += 4) {
            uint8_t nb[ISA_MAXBYTES];
            long z[ISA_MAXV] = {0, 0, 0, 0};
            if (nop && isa_encode(nop, z, nb, err, sizeof err) == 0)
                memcpy(img + fo + i, nb, nop->len);
        }
    }
    FILE *o = fopen(out, "wb"); fwrite(img, 1, e.n, o); fclose(o);
    chmod(out, 0755);
    printf("relink: moved %ld bytes %#llx -> %#llx, source redirected with a branch\n",
           len, (unsigned long long)src, (unsigned long long)dst);
    printf("        wrote %s\n", out);
    arbit_release_carve((long)(img)); arbit_release_carve((long)(e.raw));
    return 0;
}

/* Dump the raw bytes of the executable segment to a file. This is how two builds are
 * compared for "same code": the whole-file comparison is useless because the compiler
 * stamps a build id and a .comment section that carry the source path and flags. */
static int cmd_text(const char *path, const char *out) {
    Elf e; elf_open(&e, path);
    uint64_t off = 0, va = 0; size_t len = 0;
    if (code_region(&e, &off, &va, &len) != 0) die("no executable segment");
    FILE *o = fopen(out, "wb");
    if (!o) die("cannot write output");
    fwrite(e.raw + off, 1, len, o);
    fclose(o);
    printf("text: %zu bytes of code (%#llx..%#llx) -> %s\n",
           len, (unsigned long long)va, (unsigned long long)(va + len), out);
    arbit_release_carve((long)(e.raw));
    return 0;
}

/* Run the produced binary and report how it exited -- the only proof that a
 * relocated program still means what it meant. */
static int cmd_run(const char *path) {
    pid_t p = fork();
    if (p == 0) {
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFEXITED(st)) printf("run: %s exited %d\n", path, WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) printf("run: %s killed by signal %d\n", path, WTERMSIG(st));
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static int usage(void) {
    fprintf(stderr,
        "usage: blink <command>\n"
        "  info    <elf>\n"
        "  symbols <elf>\n"
        "  code    <elf> [--hex]\n"
        "  text    <elf> <out.bin>        raw bytes of the executable segment\n"
        "  patch   <elf> <vaddr> <word> <out>\n"
        "  addcode <elf> <src.block> <out>\n"
        "  relink  <elf> <src> <len> <dst> <out>\n"
        "  run     <elf>\n");
    return 2;
}

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "info")    && argc == 3) return cmd_info(argv[2]);
    if (!strcmp(c, "symbols") && argc == 3) return cmd_symbols(argv[2]);
    if (!strcmp(c, "code")    && argc >= 3) return cmd_code(argv[2], argc > 3 && !strcmp(argv[3], "--hex"));
    if (!strcmp(c, "patch")   && argc == 6) return cmd_patch(argv[2], argv[3], argv[4], argv[5]);
    if (!strcmp(c, "addcode") && argc == 5) return cmd_addcode(argv[2], argv[3], argv[4]);
    if (!strcmp(c, "relink")  && argc == 7) return cmd_relink(argv[2], argv[3], argv[4], argv[5], argv[6]);
    if (!strcmp(c, "text")    && argc == 4) return cmd_text(argv[2], argv[3]);
    if (!strcmp(c, "run")     && argc == 3) return cmd_run(argv[2]);
    return usage();
}
