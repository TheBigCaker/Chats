/* x86walk.c -- walk an x86-64 ELF's code section through the catalog, sequentially.
 *
 * The catalog is length-aware: isa_decode takes the remaining bytes and returns the length of
 * the row it matched. So a decoder walks the stream by trusting that length, not by guessing.
 *
 * A shell loop that tries lengths longest-first consumes 10 bytes of an unrelated instruction
 * and reports the result as a gap in the catalog -- which is what a first attempt did, and it
 * looked like three missing rows when nothing was missing at all. Instruction length is an
 * OUTPUT of decoding, never an input.
 *
 *   x86walk <elf>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "isadb.h"
#include "arbishim.h"
#include "ISA_X86_64.inc"

static const IsaTable T = { isa_forms_x86_64, ISA_NFORMS_X86_64, "x86_64", 1, 1 };

static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | ((p[1] << 8))); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: x86walk <elf>\n"); return 2; }
    arbit_init(0);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "x86walk: cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *img = (uint8_t *)arb_bytes(n);
    if (!img || fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; }
    fclose(f);
    if (memcmp(img, "\x7f""ELF", 4)) { fprintf(stderr, "x86walk: not an ELF\n"); return 2; }

    uint64_t shoff = r64(img + 0x28);
    uint16_t shent = r16(img + 0x3a), shnum = r16(img + 0x3c);
    uint16_t shstr = r16(img + 0x3e);
    uint64_t stroff = r64(img + shoff + (uint64_t)shstr * shent + 0x18);

    long total = 0, decoded = 0, gaps = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        uint64_t o = shoff + (uint64_t)i * shent;
        uint64_t fl = r64(img + o + 0x8), ad = r64(img + o + 0x10);
        uint64_t fo = r64(img + o + 0x18), sz = r64(img + o + 0x20);
        if (!(fl & 0x4) || !sz || fo + sz > (uint64_t)n) continue;
        const char *nm = (const char *)(img + stroff + (uint32_t)r64(img + o));
        printf("section %s at %#llx, %llu bytes\n", nm, (unsigned long long)ad,
               (unsigned long long)sz);
        const uint8_t *code = img + fo;
        uint64_t pos = 0;
        while (pos < sz) {
            const IsaForm *row = NULL;
            long ops[ISA_MAXV] = {0};
            int len = isa_decode(&T, code + pos, (int)(sz - pos), &row, ops);
            total++;
            if (len <= 0 || !row) {
                gaps++;
                char hx[40]; size_t k = 0;
                for (int b = 0; b < 8 && pos + (uint64_t)b < sz; b++)
                    k += (size_t)snprintf(hx + k, sizeof hx - k, "%02x", code[pos + (uint64_t)b]);
                printf("  %08llx  %s  NO ROW\n", (unsigned long long)(ad + pos), hx);
                pos += 1;   /* resynchronise one byte and keep going: x86 has no alignment */
                continue;
            }
            decoded++;
            printf("  %08llx  ", (unsigned long long)(ad + pos));
            for (int b = 0; b < len; b++) printf("%02x", code[pos + (uint64_t)b]);
            for (int b = len; b < 10; b++) printf("  ");
            printf("  %-20s", row->name);
            for (uint32_t v = 0; v < row->nvars; v++) printf(" %ld", ops[v]);
            printf("\n");
            pos += (uint64_t)len;
        }
    }
    printf("--- %ld instructions decoded, %ld gaps of %ld positions (%ld%% covered)\n",
           decoded, gaps, total, total ? (100 * decoded / total) : 0);
    return 0;
}
