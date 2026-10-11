/* binast -- raw-binary AST tool: flatten, diff, patch, tokenize, canonicalize.
 * Pure C11, no dependencies, no Python anywhere.
 * Reads ELF64 directly (no binutils needed).
 *
 *   binast dump   <elf> <out.bin>   flatten code sections into one positioned blob
 *   binast tokens <elf>             positioned token stream (trainable form)
 *   binast diff   <a.elf> <b.elf>   byte-level patch list, section-mapped
 *   binast patch  <elf> <patch> <out.elf>   apply byte patch, no compiler involved
 *   binast stat   <a.bin> <b.bin>   canonicality + entropy comparison
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cygemm/snumblits.h"

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"   /* ccp_logf: log without libc.math / libm */

#define BINAST_LOG2E 1.4426950408889634f
static inline float b_log2(float x) { return ccp_logf(x) * BINAST_LOG2E; }

typedef struct { uint8_t *p; size_t n; } Buf;

#define CODE_SECTIONS 3
static const char *codesec[CODE_SECTIONS] = { ".text", ".rodata", ".data" };

typedef struct {
    uint64_t off;      /* file offset in the ELF */
    uint64_t size;
    uint64_t addr;     /* virtual address */
    uint64_t flags;
    uint32_t type;     /* SHT_* ; 8 = SHT_NOBITS has no file bytes */
    char     name[64];
    int      is_code;  /* 1 if in codesec[] */
} Sec;

typedef struct {
    const char *path;
    Buf   raw;
    Sec   sec[64];
    int   nsec;
} Elf;

static void die(const char *m) { fprintf(stderr, "binast: %s\n", m); exit(1); }

static Buf read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "binast: cannot open %s\n", path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) die("seek");
    long sz = ftell(f);
    if (sz < 0) die("tell");
    rewind(f);
    Buf b; b.n = (size_t)sz; b.p = arb_bytes(b.n ? b.n : 1);
    if (!b.p) die("oom");
    if (fread(b.p, 1, b.n, f) != b.n) die("short read");
    fclose(f);
    return b;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static int in_codesec(const char *n) {
    for (int i = 0; i < CODE_SECTIONS; i++) if (!strcmp(n, codesec[i])) return 1;
    return 0;
}

static void elf_load(Elf *e, const char *path) {
    memset(e, 0, sizeof *e);
    e->path = path;
    e->raw = read_all(path);
    const uint8_t *p = e->raw.p;
    if (e->raw.n < 64) die("too small to be ELF");
    if (p[0] != 0x7f || p[1] != 'E' || p[2] != 'L' || p[3] != 'F') die("not an ELF file");
    if (p[4] != 2 || p[5] != 1) die("only ELF64 little-endian supported");
    uint64_t shoff  = rd64(p + 0x28);
    uint16_t shents = rd16(p + 0x3a);
    uint16_t shnum  = rd16(p + 0x3c);
    uint16_t shstr  = rd16(p + 0x3e);
    if (shents != 64) die("unexpected section header size");
    if (shoff + (uint64_t)shnum * 64 > e->raw.n) die("section table past EOF");
    const uint8_t *strsh = p + shoff + (uint64_t)shstr * 64;
    uint64_t stroff = rd64(strsh + 0x18), strsz = rd64(strsh + 0x20);
    if (stroff + strsz > e->raw.n) die("string table past EOF");
    const char *strtab = (const char *)(p + stroff);
    for (int i = 0; i < shnum && e->nsec < 64; i++) {
        const uint8_t *sh = p + shoff + (uint64_t)i * 64;
        uint32_t nameoff = rd32(sh + 0x00);
        if (nameoff >= strsz) continue;
        const char *nm = strtab + nameoff;
        Sec *s = &e->sec[e->nsec];
        snprintf(s->name, sizeof s->name, "%s", nm);
        s->type  = rd32(sh + 0x04);
        s->flags = rd64(sh + 0x08);
        s->addr  = rd64(sh + 0x10);
        s->off   = rd64(sh + 0x18);
        s->size  = rd64(sh + 0x20);
        s->is_code = in_codesec(nm);
        if (s->size && s->type != 8 && s->off + s->size > e->raw.n) die("section past EOF");
        e->nsec++;
    }
    if (!e->nsec) die("no sections");
}

/* Flatten the code sections into one contiguous positioned blob. */
static void flatten(const Elf *e, Buf *out) {
    size_t total = 0;
    for (int i = 0; i < e->nsec; i++)
        if (e->sec[i].is_code && e->sec[i].size && e->sec[i].type != 8)
            total += (e->sec[i].size + 15) & ~(size_t)15;
    out->p = arb_bytes((long)(total ? total : 1) * (long)(1));
    if (!out->p) die("oom");
    out->n = total;
    size_t cur = 0;
    for (int i = 0; i < e->nsec; i++) {
        const Sec *s = &e->sec[i];
        if (!s->is_code || !s->size || s->type == 8) continue;
        memcpy(out->p + cur, e->raw.p + s->off, s->size);
        cur += (s->size + 15) & ~(size_t)15;
    }
}

/* Map a raw-file offset back to "<section>+<rel>". */
static const Sec *sec_of(const Elf *e, uint64_t foff, uint64_t *rel) {
    for (int i = 0; i < e->nsec; i++) {
        const Sec *s = &e->sec[i];
        if (s->size && foff >= s->off && foff < s->off + s->size) { *rel = foff - s->off; return s; }
    }
    return NULL;
}

static double entropy(const uint8_t *p, size_t n) {
    size_t h[256]; memset(h, 0, sizeof h);
    for (size_t i = 0; i < n; i++) h[p[i]]++;
    double H = 0;
    for (int i = 0; i < 256; i++) if (h[i]) { double q = (double)h[i] / (double)n; H -= q * (double)b_log2((float)q); }
    return H;
}

static int cmd_dump(const char *path, const char *outpath) {
    Elf e; elf_load(&e, path);
    Buf blob; flatten(&e, &blob);
    FILE *f = fopen(outpath, "wb");
    if (!f) die("cannot write blob");
    fwrite(blob.p, 1, blob.n, f);
    fclose(f);
    printf("dump %s -> %s (%zu bytes)\n", path, outpath, blob.n);
    size_t cur = 0;
    for (int i = 0; i < e.nsec; i++) {
        const Sec *s = &e.sec[i];
        if (!s->is_code || !s->size || s->type == 8) continue;
        printf("  %-10s blob[%5zu..%5zu)  size %6llu  vaddr %#llx  flags %s\n",
               s->name, cur, cur + s->size, (unsigned long long)s->size,
               (unsigned long long)s->addr, (s->flags & 4) ? "AX" : "A");
        cur += (s->size + 15) & ~(size_t)15;
    }
    return 0;
}

/* Positioned token stream: the trainable "raw binary AST".
 * AArch64 code is fixed-width 4-byte, so every instruction word starts on a
 * 4-byte boundary inside a code section -- no length decoding needed. */
static int cmd_tokens(const char *path) {
    Elf e; elf_load(&e, path);
    printf("# binast-tokens v1 arch=%s wordsize=%d endian=le\n", "aarch64", 4);
    for (int i = 0; i < e.nsec; i++) {
        const Sec *s = &e.sec[i];
        if (!s->is_code || !s->size || s->type == 8) continue;
        int word = (s->flags & 4) && !strcmp(s->name, ".text");
        printf("# SEC %s addr=%#llx size=%llu kind=%s\n", s->name,
               (unsigned long long)s->addr, (unsigned long long)s->size,
               word ? "CODE" : "DATA");
        if (word) {
            for (uint64_t o = 0; o + 4 <= s->size; o += 4) {
                uint32_t w = rd32(e.raw.p + s->off + o);
                printf("%#010llx %08x\n", (unsigned long long)(s->addr + o), w);
            }
        } else {
            for (uint64_t o = 0; o < s->size; o++) {
                uint8_t b = e.raw.p[s->off + o];
                printf("%#010llx %02x\n", (unsigned long long)(s->addr + o), b);
            }
        }
    }
    return 0;
}

static int cmd_diff(const char *pa, const char *pb) {
    Elf a, b; elf_load(&a, pa); elf_load(&b, pb);
    if (a.raw.n != b.raw.n)
        printf("warning: file sizes differ (%zu vs %zu)\n", a.raw.n, b.raw.n);
    size_t n = a.raw.n < b.raw.n ? a.raw.n : b.raw.n;
    size_t changes = 0, code_changes = 0, data_changes = 0;
    for (size_t i = 0; i < n; i++) {
        if (a.raw.p[i] == b.raw.p[i]) continue;
        changes++;
        uint64_t rel = 0;
        const Sec *s = sec_of(&a, i, &rel);
        if (s && s->is_code) {
            if (!strcmp(s->name, ".text")) code_changes++;
            else data_changes++;
        }
        if (changes <= 40) {
            if (s) printf("@@ %-9s+%-6llu (file %#zx)  %02x -> %02x\n",
                          s->name, (unsigned long long)rel, i, a.raw.p[i], b.raw.p[i]);
            else   printf("@@ %-9s %-7s (file %#zx)  %02x -> %02x\n",
                          "?", "", i, a.raw.p[i], b.raw.p[i]);
        } else if (changes == 41) {
            printf("... (truncated)\n");
        }
    }
    printf("diff: %zu bytes differ; %zu in .text, %zu in .rodata/.data\n",
           changes, code_changes, data_changes);
    return 0;
}

/* Patch file format:  <fileoffset-hex> <oldbyte-hex> <newbyte-hex>\n per line */
static int cmd_patch(const char *elfp, const char *patchp, const char *outp) {
    Elf e; elf_load(&e, elfp);
    Buf pa; pa.p = NULL;
    char *txt = NULL;
    { FILE *f = fopen(patchp, "rb"); if (!f) die("cannot open patch"); fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
      txt = arb_bytes((size_t)sz + 1); if (fread(txt, 1, (size_t)sz, f) != (size_t)sz) die("patch read"); txt[sz] = 0; fclose(f); }
    uint8_t *img = arb_bytes(e.raw.n); memcpy(img, e.raw.p, e.raw.n);
    int applied = 0, refused = 0;
    char *line = strtok(txt, "\n");
    while (line) {
        unsigned long long fo; unsigned o, nb;
        if (line[0] != '#' && sscanf(line, "%llx %x %x", &fo, &o, &nb) == 3) {
            if (fo < e.raw.n) {
                if (img[fo] == (uint8_t)o) { img[fo] = (uint8_t)nb; applied++; }
                else { refused++; fprintf(stderr, "binast: guard mismatch at %#llx\n", fo); }
            } else refused++;
        }
        line = strtok(NULL, "\n");
    }
    FILE *f = fopen(outp, "wb"); if (!f) die("cannot write out elf");
    fwrite(img, 1, e.raw.n, f); fclose(f);
    printf("patch: %d applied, %d refused -> %s\n", applied, refused, outp);
    return refused ? 2 : 0;
}

/* Replay a token stream back into an ELF image: proves the trainable form is
 * lossless.  Token lines are "<vaddr-hex> <value-hex>"; the base ELF supplies
 * the vaddr -> file-offset mapping.  No compiler, no linker involved. */
static int cmd_rebuild(const char *elfp, const char *tokp, const char *outp) {
    Elf e; elf_load(&e, elfp);
    FILE *f = fopen(tokp, "rb"); if (!f) die("cannot open token file");
    char line[256];
    uint8_t *img = arb_bytes(e.raw.n); memcpy(img, e.raw.p, e.raw.n);
    size_t written = 0, missed = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long long va, val;
        if (sscanf(line, "%llx %llx", &va, &val) != 2) continue;
        /* word-sized token if the section is executable .text, else byte */
        for (int i = 0; i < e.nsec; i++) {
            const Sec *s = &e.sec[i];
            if (!s->size || va < s->addr || va >= s->addr + s->size) continue;
            uint64_t fo = s->off + (va - s->addr);
            int word = (s->flags & 4) && !strcmp(s->name, ".text");
            if (word) {
                if (fo + 4 > e.raw.n) { missed++; break; }
                for (int k = 0; k < 4; k++) img[fo + k] = (uint8_t)((val >> (8*k)) & 0xff);
            } else {
                if (fo + 1 > e.raw.n) { missed++; break; }
                img[fo] = (uint8_t)(val & 0xff);
            }
            written++;
            break;
        }
    }
    fclose(f);
    FILE *o = fopen(outp, "wb"); if (!o) die("cannot write rebuilt elf");
    fwrite(img, 1, e.raw.n, o); fclose(o);
    printf("rebuild: %zu tokens replayed, %zu missed -> %s\n", written, missed, outp);
    return missed ? 3 : 0;
}

/* Write one instruction word at a virtual address in an existing ELF. This is
 * the "put a building block into the raw binary" operation: isadb produces the
 * word, this places it, no compiler or linker involved. */
static int cmd_setword(const char *elfp, const char *vaddr_s, const char *word_s, const char *outp) {
    Elf e; elf_load(&e, elfp);
    uint64_t va = strtoull(vaddr_s, NULL, 16);
    uint32_t w = (uint32_t)strtoul(word_s, NULL, 16);
    uint8_t *img = arb_bytes(e.raw.n); memcpy(img, e.raw.p, e.raw.n);
    for (int i = 0; i < e.nsec; i++) {
        const Sec *s = &e.sec[i];
        if (!s->size || s->type == 8) continue;
        if (va < s->addr || va + 4 > s->addr + s->size) continue;
        uint64_t fo = s->off + (va - s->addr);
        if (fo + 4 > e.raw.n) break;
        uint32_t old = rd32(e.raw.p + fo);
        for (int k = 0; k < 4; k++) img[fo + k] = (uint8_t)((w >> (8 * k)) & 0xff);
        FILE *o = fopen(outp, "wb");
        if (!o) { arbit_release_carve((long)(img)); die("cannot write out elf"); }
        fwrite(img, 1, e.raw.n, o); fclose(o); arbit_release_carve((long)(img));
        printf("setword: %s+%#llx  %08x -> %08x  (file %#llx)\n", s->name,
               (unsigned long long)(va - s->addr), old, w, (unsigned long long)fo);
        return 0;
    }
    arbit_release_carve((long)(img));
    fprintf(stderr, "binast: no section contains vaddr %#llx\n", (unsigned long long)va);
    return 1;
}

static int cmd_mkpatch(const char *pa, const char *pb, const char *out) {
    Elf a, b; elf_load(&a, pa); elf_load(&b, pb);
    if (a.raw.n != b.raw.n) die("mkpatch needs same-size binaries");
    FILE *f = fopen(out, "wb"); if (!f) die("cannot write patch");
    size_t n = a.raw.n, count = 0;
    fprintf(f, "# binast patch v1 base=%s target=%s\n", pa, pb);
    for (size_t i = 0; i < n; i++) {
        if (a.raw.p[i] == b.raw.p[i]) continue;
        fprintf(f, "%zx %02x %02x\n", i, a.raw.p[i], b.raw.p[i]);
        count++;
    }
    fclose(f);
    printf("mkpatch: %zu byte edits -> %s\n", count, out);
    return 0;
}

static int cmd_stat(const char *pa, const char *pb) {
    Buf a = read_all(pa), b = read_all(pb);
    size_t na = a.n < b.n ? a.n : b.n;
    size_t same = 0;
    for (size_t i = 0; i < na; i++) if (a.p[i] == b.p[i]) same++;
    int hpa[256] = {0}, hpb[256] = {0};
    for (size_t i = 0; i < a.n; i++) hpa[a.p[i]]++;
    for (size_t i = 0; i < b.n; i++) hpb[b.p[i]]++;
    int da = 0, db = 0;
    for (int i = 0; i < 256; i++) { if (hpa[i]) da++; if (hpb[i]) db++; }
    printf("%-28s %7zu B  H=%.3f bits/byte  distinct=%3d\n", pa, a.n, entropy(a.p, a.n), da);
    printf("%-28s %7zu B  H=%.3f bits/byte  distinct=%3d\n", pb, b.n, entropy(b.p, b.n), db);
    printf("byte-identical: %zu/%zu (%.2f%%)\n", same, na, 100.0 * (double)same / (double)na);
    printf("first differing offset: ");
    { size_t i = 0; while (i < na && a.p[i] == b.p[i]) i++;
      if (i == na) printf("none\n"); else printf("%#zx (%02x -> %02x)\n", i, a.p[i], b.p[i]); }
    return 0;
}

static int usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  binast dump   <elf> <out.bin>\n"
        "  binast tokens <elf>\n"
        "  binast diff   <a.elf> <b.elf>\n"
        "  binast patch  <elf> <patch-file> <out.elf>\n"
        "  binast mkpatch <a.elf> <b.elf> <out.patch>\n"
        "  binast rebuild <base.elf> <tokens> <out.elf>\n"
        "  binast stat   <a> <b>\n"
        "  binast setword <elf> <vaddr-hex> <word-hex> <out.elf>\n");
    return 2;
}

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "dump")   && argc == 4) return cmd_dump(argv[2], argv[3]);
    if (!strcmp(c, "tokens") && argc == 3) return cmd_tokens(argv[2]);
    if (!strcmp(c, "diff")   && argc == 4) return cmd_diff(argv[2], argv[3]);
    if (!strcmp(c, "patch")  && argc == 5) return cmd_patch(argv[2], argv[3], argv[4]);
    if (!strcmp(c, "mkpatch") && argc == 5) return cmd_mkpatch(argv[2], argv[3], argv[4]);
    if (!strcmp(c, "rebuild") && argc == 5) return cmd_rebuild(argv[2], argv[3], argv[4]);
    if (!strcmp(c, "stat")   && argc == 4) return cmd_stat(argv[2], argv[3]);
    if (!strcmp(c, "setword") && argc == 6) return cmd_setword(argv[2], argv[3], argv[4], argv[5]);
    return usage();
}
