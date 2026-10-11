/* x86batch.h -- assemble MANY samples in ONE llvm-mc invocation.
 *
 * WHY THIS EXISTS. The first generator spawned one llvm-mc process per sample: eight processes
 * and sixteen temp files per form, thousands of processes for a catalog emit. A process spawn
 * on this machine costs tens of milliseconds, so a full emit took ten minutes of wall clock --
 * and that wait was mistaken for the work being hard.
 *
 * llvm-mc takes many instructions per file and its object output resolves branch displacements
 * for real, so a whole form's samples go into ONE source file. Each sample is preceded by a
 * label and followed by `.set`, so the symbol table reports its offset and its LENGTH -- and the
 * length is what a branch sample needs, because llvm-mc encodes target - (start + length), so
 * asking for the encoded value D requires writing the target as LABEL + (D + len).
 *
 * No allocation: a form has a handful of samples, so the batch is a fixed array. Nothing here
 * touches the alloc family, so the tree's gate is satisfied by construction.
 */
#ifndef X86BATCH_H
#define X86BATCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define XB_MAX   64
#define XB_LINE  256

typedef struct {
    char    line[XB_LINE];
    uint8_t by[16];
    int     len;      /* -1 = llvm-mc refused this line */
    int     ok;
} Batch;

static void batch_reset(Batch *b) {
    for (int i = 0; i < XB_MAX; i++) { b[i].len = -1; b[i].ok = 0; }
}

/* little-endian field readers over a raw file image */
static uint64_t xb_rd(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* Assemble every line in the batch with ONE llvm-mc call, then read the bytes and lengths back
 * out of the object file's .text and symbol table.
 *
 * Returns the number of lines that assembled. A line llvm-mc rejects poisons the whole batch
 * (the assembler stops at the first error), which is reported as 0 so the caller can fall back
 * to assembling that form's lines one at a time -- the batch is the fast path, not the only one.
 */
static int batch_assemble(Batch *b, int n, const char *tag) {
    if (n <= 0 || n > XB_MAX) return 0;
    char src[220], obj[240], cmd[1024];
    /* THE FILE NAME MUST BE UNIQUE PER CALL, not per row. Naming it after the row made the low
     * bank (0..7) and the full-range pass write the SAME path, so whichever ran second read the
     * first one's object -- and a form the low bank assembles perfectly was solved from another
     * bank's bytes, or from bytes that were never rewritten because the assembler failed and the
     * old file was still there. That is how JO_REL8 assembled in `probe` and was dropped in
     * `emit`: the two paths ask for different ranges of the same row. A counter makes every
     * call's artifacts its own. */
    static unsigned g_batch_seq;
    snprintf(src, sizeof src,
             "/data/data/com.termux/files/home/binast-spike/tmp/xb_%u_%s.s", g_batch_seq, tag);
    snprintf(obj, sizeof obj,
             "/data/data/com.termux/files/home/binast-spike/tmp/xb_%u_%s.o", g_batch_seq, tag);
    g_batch_seq++;
    /* a stale object from an earlier run must never be read as this run's answer */
    remove(obj);

    FILE *f = fopen(src, "w");
    if (!f) return 0;
    fprintf(f, ".text\n");
    for (int i = 0; i < n; i++) {
        /* B<i> at the instruction start and E<i> after it: the symbol table then gives the
         * offset and the length without any parsing of disassembly text. */
        fprintf(f, "B%d:\n%s\n.set E%d, .-B%d\n", i, b[i].line, i, i);
    }
    fclose(f);

    snprintf(cmd, sizeof cmd,
             "llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o %s %s 2>/dev/null",
             obj, src);
    if (system(cmd) != 0) { remove(src); remove(obj); return 0; }

    FILE *o = fopen(obj, "rb");
    if (!o) return 0;
    fseek(o, 0, SEEK_END);
    long fsz = ftell(o);
    rewind(o);
    if (fsz <= 64 || fsz > (long)(64u << 20)) { fclose(o); return 0; }
    static uint8_t img[64u << 20];
    if (fread(img, 1, (size_t)fsz, o) != (size_t)fsz) { fclose(o); return 0; }
    fclose(o);
    if (memcmp(img, "\x7f""ELF", 4)) return 0;

    uint64_t shoff = xb_rd(img + 0x28, 8);
    uint16_t shent = (uint16_t)xb_rd(img + 0x3a, 2);
    uint16_t shn   = (uint16_t)xb_rd(img + 0x3c, 2);

    const uint8_t *text = NULL; uint64_t textsz = 0;
    const uint8_t *sym  = NULL; uint64_t symsz  = 0;
    const uint8_t *str  = NULL; uint64_t strsz  = 0;

    for (uint16_t i = 0; i < shn; i++) {
        uint64_t o2 = shoff + (uint64_t)i * shent;
        if (o2 + 64 > (uint64_t)fsz) break;
        uint32_t ty = (uint32_t)xb_rd(img + o2 + 0x4, 4);
        uint64_t fo = xb_rd(img + o2 + 0x18, 8);
        uint64_t sz = xb_rd(img + o2 + 0x20, 8);
        if (fo + sz > (uint64_t)fsz) continue;
        if      (ty == 2) { sym  = img + fo; symsz  = sz; }
        else if (ty == 3) { str  = img + fo; strsz  = sz; }
        else if (ty == 1) { text = img + fo; textsz = sz; }
    }
    if (!text || !sym || !str) return 0;

    /* symbol entry layout: name(4) info(1) other(1) shndx(2) value(8) size(8) = 24 bytes */
    static int off[XB_MAX];
    for (int i = 0; i < n; i++) off[i] = -1;

    for (uint64_t s = 0; s + 24 <= symsz; s += 24) {
        uint32_t nm  = (uint32_t)xb_rd(sym + s, 4);
        uint64_t val = xb_rd(sym + s + 0x8, 8);
        if (nm >= strsz) continue;
        const char *sn = (const char *)(str + nm);
        if (sn[0] != 'B' && sn[0] != 'E') continue;
        char *end = NULL;
        long idx = strtol(sn + 1, &end, 10);
        if (!end || *end || idx < 0 || idx >= n) continue;
        if (sn[0] == 'B') off[idx] = (int)val;
        else if (off[idx] >= 0) {
            int len = (int)val;
            if (len <= 0 || len > 16) { off[idx] = -1; continue; }
            if ((uint64_t)off[idx] + (uint64_t)len > textsz) { off[idx] = -1; continue; }
            memcpy(b[idx].by, text + off[idx], (size_t)len);
            b[idx].len = len;
            b[idx].ok  = 1;
        }
    }
    remove(src);
    remove(obj);
    int got = 0;
    for (int i = 0; i < n; i++) if (b[i].ok) got++;
    return got;
}

#endif
