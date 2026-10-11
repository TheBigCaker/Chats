/* x86cov.c -- measure the catalog against llvm-objdump's own disassembly.
 *
 * WHY THIS EXISTS. `x86walk` slides through a code section and reports positions covered, which
 * cannot tell a row that is MISSING from a row that matched at the WRONG OFFSET: one wrong row
 * derails the slide and every following position is reported as a gap. That made two very different
 * defects look identical, and cost real time chasing the wrong one.
 *
 * This reads the SAME object twice:
 *
 *   llvm-objdump tells the truth: the byte offset, length and mnemonic of every instruction.
 *   the catalog is asked to decode at each of those offsets.
 *
 * and prints a per-mnemonic tally. A row that is missing shows up as "no form decoded <mnemonic>",
 * a row that is wrong shows up as "decoded X at a <mnemonic> offset", and a row that matches at a
 * wrong offset cannot hide because the offsets come from the oracle rather than from the walk.
 *
 * Usage:  x86cov <object.o> [path-to-llvm-objdump]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arbishim.h"
#include "isadb.h"
#include "ISA_X86_64.inc"

static const IsaTable T = { isa_forms_x86_64, ISA_NFORMS_X86_64, "x86_64" };

/* read the whole text section of a relocatable object and one instruction list from objdump */
typedef struct { uint32_t off; int len; char mn[40]; } Insn;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: x86cov <object.o>\n"); return 2; }
    const char *path = argv[1];
    const char *objdump = argc > 2 ? argv[2] : "llvm-objdump";

    /* ---- 1. the oracle ---- */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s -d --no-show-raw-insn '%s' 2>/dev/null", objdump, path);
    FILE *p = popen(cmd, "r");
    if (!p) { fprintf(stderr, "x86cov: cannot run %s\n", objdump); return 2; }

    Insn *ins = (Insn *)arb_carve_zeroed((long)(sizeof(Insn) * 20000));
    int n = 0;
    char line[512];
    while (fgets(line, sizeof line, p)) {
        /* "      10:      \taddl\t(%rdi,%rdx,4), %eax" */
        char *c = strchr(line, ':');
        if (!c) continue;
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        /* AN OFFSET IN HEX CAN START WITH A LETTER. Requiring a digit rejected every instruction
         * whose address began with a-f -- `a:`, `c:`, `2c:` was fine but `2a:` was not -- so the
         * LENGTH of the preceding instruction was computed against a much later offset and the
         * catalog was blamed for a length mismatch it did not cause. Any hex digit starts an
         * offset, but a disassembly line always has a ':' after it, which is what separates an
         * offset from a stray token. */
        if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F'))) continue;
        {
            char *colon_probe = q;
            int allhex = 1;
            while (*colon_probe && *colon_probe != ':' && *colon_probe != ' ' && *colon_probe != '\t') {
                if (!((*colon_probe >= '0' && *colon_probe <= '9')
                      || (*colon_probe >= 'a' && *colon_probe <= 'f')
                      || (*colon_probe >= 'A' && *colon_probe <= 'F'))) { allhex = 0; break; }
                colon_probe++;
            }
            if (!allhex || *colon_probe != ':') continue;
        }
        unsigned off = (unsigned)strtoul(q, NULL, 16);
        char *tab = strchr(c, '\t');
        if (!tab) continue;
        tab++;
        while (*tab == '\t' || *tab == ' ') tab++;
        if (!*tab) continue;
        /* the mnemonic is the first token; a target annotation is not part of it */
        char mn[40]; int ml = 0;
        while (*tab && *tab != '\t' && *tab != ' ' && ml < 39) mn[ml++] = *tab++;
        mn[ml] = 0;
        if (!ml) continue;
        if (n < 20000) { ins[n].off = off; snprintf(ins[n].mn, sizeof ins[n].mn, "%s", mn); n++; }
    }
    pclose(p);
    if (!n) { fprintf(stderr, "x86cov: %s produced no instructions\n", objdump); return 2; }

    /* LENGTHS COME FROM THE DELTA BETWEEN CONSECUTIVE OFFSETS. The last instruction has no next
     * offset, so its length is taken from the section size once that is known (set below as
     * text_size); defaulting it to 16 made the final instruction report a phantom mismatch. */
    for (int i = 0; i < n; i++) {
        if (i + 1 < n) {
            ins[i].len = (int)(ins[i + 1].off - ins[i].off);
        } else {
            ins[i].len = -1;   /* resolved against the section size below */
        }
        if (ins[i].len == 0) ins[i].len = 1;
    }

    /* ---- 2. the object's text bytes ---- */
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "x86cov: cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fprintf(stderr, "x86cov: empty\n"); return 2; }
    uint8_t *buf = (uint8_t *)arb_carve_zeroed(sz + 1);
    if (!buf) { fprintf(stderr, "x86cov: out of carve space\n"); return 2; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "x86cov: short read\n"); return 2; }
    fclose(f);

    /* find .text: the shstrtab is not parsed here, so use the section the instructions came from
     * by locating the first executable PROGBITS section the way elfobj.h does -- by scanning the
     * section header table for SHF_EXECINSTR. */
    long text_off = -1, text_size = 0;
    if (sz > 64 && buf[0] == 0x7f && buf[1] == 'E') {
        uint64_t sho = 0; uint16_t shent = 0, shnum = 0, shstr = 0;
        memcpy(&sho,    buf + 0x28, 8);
        memcpy(&shent,  buf + 0x3a, 2);
        memcpy(&shnum,  buf + 0x3c, 2);
        memcpy(&shstr,  buf + 0x3e, 2);
        (void)shstr;
        for (int i = 0; i < shnum && shent; i++) {
            long o = (long)sho + (long)i * shent;
            if (o + 8 > sz) break;
            uint32_t type = 0; uint64_t flags = 0, off = 0, siz = 0;
            memcpy(&type,  buf + o + 4, 4);
            memcpy(&flags, buf + o + 8, 8);
            memcpy(&off,   buf + o + 24, 8);
            memcpy(&siz,   buf + o + 32, 8);
            if (type == 1 && (flags & 0x4)) { text_off = (long)off; text_size = (long)siz; break; }
        }
    }
    if (text_off < 0) { fprintf(stderr, "x86cov: no executable section in %s\n", path); return 2; }

    /* resolve the last instruction's length from the section size */
    for (int i = 0; i < n; i++)
        if (ins[i].len < 0) ins[i].len = (int)(text_size - (long)ins[i].off);
    for (int i = 0; i < n; i++)
        if (ins[i].len <= 0 || ins[i].len > 16) ins[i].len = 1;

    /* ---- 3. ask the catalog at every TRUE offset ---- */
    typedef struct { char mn[40]; int total, ok, wrong; } Tally;
    Tally *tal = (Tally *)arb_carve_zeroed((long)(sizeof(Tally) * 512));
    int ntal = 0;
    int total = 0, ok = 0, missing = 0, wrong = 0;

    for (int i = 0; i < n; i++) {
        const uint8_t *b = buf + text_off + ins[i].off;
        if (text_off + (long)ins[i].off + ins[i].len > sz) continue;
        total++;
        const IsaForm *fm = NULL;
        long human[ISA_MAXV] = {0};
        int got = isa_decode(&T, b, ins[i].len, &fm, human);
        /* a decode is RIGHT only if it is the exact length the oracle says */
        int right = got == ins[i].len;
        if (right) ok++; else if (!got) missing++; else wrong++;

        int ti = -1;
        for (int z = 0; z < ntal; z++) if (!strcmp(tal[z].mn, ins[i].mn)) { ti = z; break; }
        if (ti < 0 && ntal < 511) { ti = ntal++; snprintf(tal[ti].mn, sizeof tal[ti].mn, "%s", ins[i].mn); }
        if (ti >= 0) {
            tal[ti].total++;
            if (right) tal[ti].ok++;
            else if (got) tal[ti].wrong++;
        }
        if (!right && (getenv("X86COV_VERBOSE") || total < 0))
            printf("  %04x  %-22s got=%-4d want=%d  %s\n", ins[i].off, ins[i].mn, got, ins[i].len,
                   fm ? fm->name : "(none)");
    }

    printf("%s\n", path);
    printf("  %d instructions, %d decoded at the right length (%.0f%%), %d missing, %d wrong length\n",
           total, ok, total ? 100.0 * ok / total : 0.0, missing, wrong);
    printf("\n  by mnemonic (only the ones that are not fully covered):\n");
    for (int z = 0; z < ntal; z++) {
        if (tal[z].ok == tal[z].total) continue;
        printf("    %-12s %3d/%3d  %s\n", tal[z].mn, tal[z].ok, tal[z].total,
               tal[z].ok == 0 ? "NO FORM" : "wrong length");
    }
    return total && ok == total ? 0 : 1;
}
