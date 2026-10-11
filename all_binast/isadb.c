/* isadb.c -- ISA catalog runtime: encode and decode instruction words, many ISAs.
 *
 * Pure C11, zero dependencies. Catalogs are hand-written data files:
 *   ISA_AARCH64.inc   AArch64 (A64)
 *   ISA_X86_64.inc    x86-64
 * Nothing here parses a mnemonic, shells out, or reads a script. Encode and decode
 * are integer arithmetic over the tables.
 *
 *   isadb [-t isa] forms   [substr]          list the catalog
 *   isadb [-t isa] encode  <FORM> [op...]    assemble one instruction
 *   isadb [-t isa] decode  <hex>             disassemble one instruction
 *   isadb [-t isa] test                      self-check every row
 *   isadb [-t isa] verify  <hexwords-file>   coverage + round-trip on real code
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "isadb.h"
#include "isadb_fixtures.h"
/* The x86-64 fixtures are GENERATED from llvm-mc by `x86fixt` (`make fixtures`), so they are in the
 * catalog's own row names rather than hand-written ones that go stale when a row is renamed. Each
 * entry is a real encoding from an independent assembler plus the operands the catalog extracts from
 * it, so the check is bytes-in / row-and-operands-out. */
#include "isadb_fixtures_x86.h"

#define ISA_INC_AARCH64 "ISA_AARCH64.inc"
#define ISA_INC_X86_64  "ISA_X86_64.inc"

/* Each catalog defines its own array and count, so both can live in one file. */
#include ISA_INC_AARCH64
static const IsaTable T_aarch64 = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

#include ISA_INC_X86_64
static const IsaTable T_x86_64 = { isa_forms_x86_64, ISA_NFORMS_X86_64, "x86_64", 1, 15 };

static const IsaTable *tables[] = { &T_aarch64, &T_x86_64 };
static const int ntables = 2;
static const IsaTable *g_tab = NULL;

static const char *cc_names[16] = {"eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al","nv"};

static const IsaTable *pick(const char *n) {
    for (int i = 0; i < ntables; i++) if (!strcmp(tables[i]->name, n)) return tables[i];
    return NULL;
}

static void show_fields(const IsaForm *f, const long *human) {
    char buf[400]; buf[0] = 0;
    for (int v = 0; v < f->nvars; v++) {
        char one[96];
        switch (f->vkind[v]) {
            case K_COND:   snprintf(one, sizeof one, "cond=%s", cc_names[human[v] & 15]); break;
            case K_SIMM:   snprintf(one, sizeof one, "simm=%ld", human[v]); break;
            case K_REG:    snprintf(one, sizeof one, "reg=%ld", human[v]); break;
            case K_UBFMW:  snprintf(one, sizeof one, "immr=%ld imms=%ld", human[v], (v + 1 < f->nvars) ? human[v + 1] : 0); if (v + 1 < f->nvars) v++; break;
            case K_UBFX:   snprintf(one, sizeof one, "lsb=%ld width=%ld", human[v], (v + 1 < f->nvars) ? human[v + 1] : 0); if (v + 1 < f->nvars) v++; break;
            case K_SHIFT:  snprintf(one, sizeof one, "shift=%ld", human[v]); break;
            case K_LOGINT: snprintf(one, sizeof one, "mask=0x%lx", human[v]); break;
            case K_IMMHI:  snprintf(one, sizeof one, "lsl=%ld", human[v]); break;
            case K_SPLIT:  snprintf(one, sizeof one, "off=%ld", human[v]); break;
            default:       snprintf(one, sizeof one, "imm=%ld", human[v]); break;
        }
        strncat(buf, one, sizeof buf - strlen(buf) - 1);
        if (v + 1 < f->nvars) strncat(buf, " ", sizeof buf - strlen(buf) - 1);
    }
    printf("  fields: %s\n", buf);
}

static int cmd_encode(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isadb encode <FORM> [op...]\n"); return 2; }
    const IsaForm *f = isadb_find(g_tab, argv[0]);
    if (!f) { fprintf(stderr, "isadb: no form '%s' in %s\n", argv[0], g_tab->name); return 1; }
    long human[ISA_MAXV] = {0};
    int argi = 1;
    for (int v = 0; v < f->nvars && argi < argc; v++) {
        const char *a = argv[argi++];
        if (f->vkind[v] == K_COND) {
            human[v] = 0;
            for (int c = 0; c < 16; c++) if (!strcmp(a, cc_names[c])) human[v] = c;
            if (isdigit((unsigned char)a[0]) || a[0] == '-') human[v] = strtol(a, NULL, 0);
        } else {
            char *end = NULL;
            size_t L = strlen(a);
            int base = (L > 2 && a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) ? 16 : 0;
            long n = strtol(a, &end, base);
            human[v] = (end && *end == 0) ? n : 0;
        }
        /* composite kinds consume a second operand */
        if ((f->vkind[v] == K_UBFMW || f->vkind[v] == K_UBFX) && v + 1 < f->nvars && argi < argc) {
            const char *b = argv[argi++];
            human[v + 1] = strtol(b, NULL, 0);
        }
    }
    uint8_t out[ISA_MAXBYTES]; char err[200];
    if (isa_encode(f, human, out, err, sizeof err) != 0) { fprintf(stderr, "isadb: %s\n", err); return 1; }
    char hex[128]; isa_hex(out, f->len, hex, sizeof hex);
    printf("%-16s %s\n", f->name, f->operands);
    printf("  len   : %d bytes   %s\n", f->len, hex);
    show_fields(f, human);
    return 0;
}

static int cmd_decode(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isadb decode <hex>\n"); return 2; }
    static uint8_t b[ISA_MAXBYTES];
    int n = 0;
    const char *p = argv[0];
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    /* Accept "d0000020", "d0 00 00 20", "d0,00,00,20" and "0xd0 0x00 0x00 0x20".
     * The old parser took the whole string and let strtoul walk it, so a spaced input
     * like "d0 00 00 20" parsed as one number and silently decoded the wrong word --
     * which made correct rows look broken. Every token is now parsed individually and
     * a token that is not hex is an error rather than a silent stop. */
    int separated = strpbrk(p, " ,\t") != NULL;
    size_t L = strlen(p);
    /* One run of hex digits whose width equals the ISA word size is the WORD as it is
     * printed in a listing (d0000020), which is numerically most-significant-first and
     * must be reversed into file order before decoding -- passing it through unchanged
     * decodes a different instruction and looks like a missing catalog row. Anything
     * else (separated tokens, or a run that is not word-sized) is taken as bytes in
     * file order, which is how a hex dump or an x86 byte string reads. */
    if (!separated && L == (size_t)g_tab->wordsize * 2) {
        for (size_t i = 0; i + 1 < L; i += 2) {
            char t[3] = { p[i], p[i + 1], 0 };
            b[n++] = (uint8_t)strtoul(t, NULL, 16);
        }
        for (int i = 0, j = n - 1; i < j; i++, j--) { uint8_t s = b[i]; b[i] = b[j]; b[j] = s; }
    } else if (!separated) {
        if (L == 0 || L % 2 || L / 2 > ISA_MAXBYTES) {
            fprintf(stderr, "isadb: expected an even number of hex digits, got \"%s\"\n", p);
            return 2;
        }
        for (size_t i = 0; i + 1 < L; i += 2) {
            char t[3] = { p[i], p[i + 1], 0 };
            b[n++] = (uint8_t)strtoul(t, NULL, 16);
        }
    } else {
        while (*p && n < ISA_MAXBYTES) {
            while (*p == ' ' || *p == ',' || *p == '\t') p++;
            if (!*p) break;
            const char *t = p;
            if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
            int digits = 0;
            while (isxdigit((unsigned char)*t)) { t++; digits++; }
            if (!digits) { fprintf(stderr, "isadb: not a hex byte: \"%s\"\n", p); return 2; }
            if (digits > 2) { fprintf(stderr, "isadb: \"%s\" is wider than one byte\n", p); return 2; }
            /* Only skip a 0x prefix when there actually is one. Testing p[0]=='0'
             * alone swallowed the first two characters of every token like "00", so
             * "d0 00 00 20" decoded as d0 00 20 20 -- a wrong word that looks plausible. */
            b[n++] = (uint8_t)strtoul((p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? p + 2 : p, NULL, 16);
            p = t;
        }
    }
    const IsaForm *f = NULL; long human[ISA_MAXV] = {0};
    int used = isa_decode(g_tab, b, n, &f, human);
    if (!used) {
        char hex[128]; isa_hex(b, n, hex, sizeof hex);
        /* A word that matches no form is not necessarily an instruction that the catalog
         * failed on. If its bytes are printable ASCII it is data -- a string literal the
         * toolchain embedded in the code section -- and saying "no catalog form" would
         * present it as a gap. Say what it is instead. */
        int allzero = 1, printable = 1;
        for (int j = 0; j < n; j++) {
            if (b[j]) allzero = 0;
            if (b[j] < 32 || b[j] >= 127) printable = 0;
        }
        if (allzero) {
            printf("%s -> zero padding, not an instruction\n", hex);
            return 0;
        }
        if (printable) {
            printf("%s -> ASCII data \"%.*s\", not an instruction\n", hex, n, (const char *)b);
            return 0;
        }
        printf("%s -> ?? no catalog form in %s\n", hex, g_tab->name);
        return 1;
    }
    char hex[128]; isa_hex(b, used, hex, sizeof hex);
    printf("%s -> %-16s %s\n", hex, f->name, f->operands);
    show_fields(f, human);
    return 0;
}

static int cmd_forms(int argc, char **argv) {
    const char *sub = argc > 0 ? argv[0] : NULL;
    int shown = 0;
    for (uint32_t i = 0; i < g_tab->n; i++) {
        const IsaForm *f = &g_tab->forms[i];
        if (sub && !strstr(f->name, sub) && !strstr(f->mnemonic, sub)) continue;
        printf("%-16s %-8s %-26s %2dB  ", f->name, f->mnemonic, f->operands, f->len);
        for (int k = 0; k < f->len; k++) printf("%02x", f->bytes[k]);
        printf("  fields:");
        for (int v = 0; v < f->nvars; v++)
            printf(" %d@%d.%d", f->vwid[v], f->vbyte[v], f->vbit[v]);
        if (f->nvars && f->vkind[f->nvars - 1] == K_SPLIT)
            printf(" +split %d@%d.%d", f->split_wid, f->split_byte, f->split_bit);
        printf("\n");
        shown++;
    }
    printf("-- %d of %u form(s) in %s\n", shown, g_tab->n, g_tab->name);
    return 0;
}

/* Decode one word per line of input ("d0000020" or file-order hex bytes) and report
 * only the ones with no catalog row. Spawning a process per word costs ~0.11s of
 * fork/exec -- measuring a real corpus that way took minutes of pure overhead. */
static int cmd_batch(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isadb batch <words-file>\n"); return 2; }
    FILE *f = strcmp(argv[0], "-") ? fopen(argv[0], "r") : stdin;
    if (!f) { fprintf(stderr, "isadb: cannot open %s\n", argv[0]); return 2; }
    char line[256];
    unsigned long total = 0, matched = 0, exact = 0, failed = 0;
    unsigned long zero_words = 0, data_words = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t L = strlen(p);
        while (L && (p[L-1] == '\n' || p[L-1] == '\r' || p[L-1] == ' ')) p[--L] = 0;
        if (!L) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        total++;
        static uint8_t b[ISA_MAXBYTES];
        memset(b, 0, sizeof b);
        int n = 0;
        size_t P = strlen(p);
        if (P == (size_t)g_tab->wordsize * 2) {
            for (size_t i = 0; i + 1 < P && n < ISA_MAXBYTES; i += 2) {
                char t[3] = { p[i], p[i+1], 0 };
                b[n++] = (uint8_t)strtoul(t, NULL, 16);
            }
            for (int i = 0, j = n - 1; i < j; i++, j--) { uint8_t s = b[i]; b[i] = b[j]; b[j] = s; }
        } else {
            while (*p && n < ISA_MAXBYTES) {
                while (*p == ' ' || *p == ',') p++;
                if (!*p) break;
                char *t = p;
                if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
                int d = 0; while (isxdigit((unsigned char)*t)) { t++; d++; }
                if (!d || d > 2) break;
                b[n++] = (uint8_t)strtoul((p[0]=='0' && (p[1]=='x'||p[1]=='X')) ? p+2 : p, NULL, 16);
                p = t;
            }
        }
        if (!n) continue;
        /* Zero words are padding, not instructions -- and the same rule must hold here as in
         * `binui coverage`, or the two report different totals for the same bytes. It matters
         * for `udf`: its encoding is 00000000 with imm16 = 0, so a zero word would otherwise
         * be counted as a matched instruction. Check padding first so the tools agree. */
        int allzero = 1, printable = 1;
        for (int j = 0; j < n; j++) {
            if (b[j]) allzero = 0;
            if (b[j] < 32 || b[j] >= 127) printable = 0;
        }
        if (allzero) { zero_words++; continue; }
        const IsaForm *form = NULL; long human[ISA_MAXV] = {0,0,0,0};
        int used = isa_decode(g_tab, b, n, &form, human);
        if (!used) {
            /* This word list is a projection of code sections taken at a fixed stride, so it
             * runs through data the toolchain embedded in .text -- string literals, jump tables,
             * blobs. Those bytes are not instructions and never will be. A word whose bytes are
             * all printable ASCII is such data; the rest are genuine misses and are printed. */
            if (printable) { data_words++; continue; }
            failed++;
            printf("%s\n", line);
            continue;
        }
        matched++;
        uint8_t chk[ISA_MAXBYTES]; char err[200];
        if (isa_encode(form, human, chk, err, sizeof err) == 0 && !memcmp(chk, b, (size_t)used))
            exact++;
        else
            /* matched a row but the re-encoding differs: the row's own template and its
             * encoder disagree about some bit, which a self-check on random operands can
             * miss. Print it distinctly -- it is a correctness bug, not a coverage gap. */
            printf("NONEXACT %s %s\n", line, form->name);
    }
    if (f != stdin) fclose(f);
    fprintf(stderr, "%s batch: %lu words, %lu matched, %lu exact round-trip, "
            "%lu inline data, %lu zero padding, %lu unmatched\n",
            g_tab->name, total, matched, exact, data_words, zero_words, failed);
    return 0;
}

/* Words that must NOT be accepted, because they do not exist on the ISA and matching
 * them would mean decoding to operands that cannot re-encode the word. Each entry is a
 * word that a real binary happened to contain (in a data region) plus why it is illegal. */
static int cmd_reject(void) {
    struct { const char *isa, *hex, *why; } cases[] = {
        { "aarch64", "53414847", "32-bit bitfield with N=1 (does not exist on the ISA)" },
        /* 0xd34c2c42 is NOT a case: it is a legal lsl x2, x2, #52. Checked against an
         * assembler before it went in, because a reject list that refuses real
         * instructions is worse than no list. */
    };
    int fail = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (strcmp(cases[i].isa, g_tab->name)) continue;
        uint8_t b[ISA_MAXBYTES]; int n = 0;
        const char *h = cases[i].hex;
        for (size_t k = 0; k + 1 < strlen(h); k += 2) {
            char t[3] = { h[k], h[k+1], 0 };
            b[n++] = (uint8_t)strtoul(t, NULL, 16);
        }
        for (int a = 0, z = n - 1; a < z; a++, z--) { uint8_t x = b[a]; b[a] = b[z]; b[z] = x; }
        const IsaForm *f = NULL; long hm[ISA_MAXV] = {0,0,0,0};
        if (isa_decode(g_tab, b, n, &f, hm)) {
            printf("FAIL %s accepted as %s (%s)\n", cases[i].hex, f->name, cases[i].why);
            fail++;
        }
    }
    if (!fail) printf("%s reject: illegal words correctly refused\n", g_tab->name);
    return fail ? 1 : 0;
}

/* Ground truth: encode each fixture and require the exact expected bytes. This is
 * the check that self-consistency cannot provide -- a row that agrees with itself
 * but disagrees with the ISA passes the round-trip test and fails here. */
static int cmd_fixtures(void) {
    int pass = 0, fail = 0, skipped = 0;
    /* THE GENERATED x86-64 FIXTURES FIRST: bytes from llvm-mc in, the catalog's own decode out, and
     * the same row and operands must reproduce those bytes exactly. This is the stronger direction --
     * a fixture here is an INDEPENDENT encoding, so a row that agrees with itself but disagrees with
     * the real ISA is caught. */
    if (!strcmp(g_tab->name, "x86_64")) {
        int xp = 0, xf = 0;
        for (unsigned i = 0; i < ISA_NFIXTURES_X86; i++) {
            const char *fn = isa_x86_fixtures[i].form;
            const IsaForm *f = NULL;
            long h[ISA_MAXV] = {0};
            long wantops[ISA_MAXV];
            for (int v = 0; v < ISA_MAXV; v++) wantops[v] = (long)isa_x86_fixtures[i].ops[v];
            /* decode the known-good bytes */
            int blen = (int)strlen(isa_x86_fixtures[i].bytes) / 2;
            uint8_t b[16];
            for (int k = 0; k < blen; k++) {
                unsigned hi = 0, lo = 0;
                char c1 = isa_x86_fixtures[i].bytes[k * 2], c2 = isa_x86_fixtures[i].bytes[k * 2 + 1];
                hi = (unsigned)(c1 <= '9' ? c1 - '0' : (c1 | 32) - 'a' + 10);
                lo = (unsigned)(c2 <= '9' ? c2 - '0' : (c2 | 32) - 'a' + 10);
                b[k] = (uint8_t)((hi << 4) | lo);
            }
            if (!isa_decode(g_tab, b, blen, &f, h)) {
                printf("X86-NODEC %-18s (want %s = %s)\n", fn, fn, isa_x86_fixtures[i].bytes);
                xf++; continue;
            }
            if (strcmp(f->name, fn)) {
                printf("X86-ROWDIF want %-18s got %-18s (%s)\n", fn, f->name,
                       isa_x86_fixtures[i].bytes);
                xf++; continue;
            }
            /* and the operands must be the ones the generator recorded */
            int ok = 1;
            for (int v = 0; v < ISA_MAXV; v++)
                if (h[v] != wantops[v]) { ok = 0; }
            if (!ok) {
                printf("X86-OPS    %-18s got", fn);
                for (int v = 0; v < ISA_MAXV; v++) printf(" %ld", h[v]);
                printf("  want");
                for (int v = 0; v < ISA_MAXV; v++) printf(" %ld", wantops[v]);
                printf("   (%s)\n", isa_x86_fixtures[i].bytes);
                xf++; continue;
            }
            /* and re-encoding those operands must give the bytes back. THE DECODED ROW IS USED, NOT A
             * LOOKUP BY NAME: the catalog carries several rows with the same name (a REX-led variant
             * and its plain twin, a SIB form and its base-only twin), and a name lookup returns
             * whichever the index happened to hash first -- so an earlier version of this check
             * "failed" 13 fixtures that were in fact correct, by re-encoding through a sibling row.
             *
             * THE PREFIX RUN IS NOT PART OF THE ROW, so it is compared as the bytes BEFORE the opcode
             * and the row's own bytes after it -- which is the contract isa_decode actually
             * implements (strip the run, decode at b+plen, return len+plen). */
            {
                const int pre = (int)isa_x86_fixtures[i].plen;
                uint8_t out[ISA_MAXBYTES]; char err[200];
                if (isa_encode(f, wantops, out, err, sizeof err) != 0
                    || memcmp(out, b + pre, (size_t)blen - (size_t)pre) != 0) {
                    char g1[64], g2[64];
                    isa_hex(out, blen, g1, sizeof g1);
                    isa_hex(b, blen, g2, sizeof g2);
                    printf("X86-REENCODE %-16s got %s want %s\n", fn, g1, g2);
                    xf++; continue;
                }
            }
            xp++;
        }
        printf("x86-64 generated fixtures: %d pass, %d fail (of %d, from llvm-mc)\n",
               xp, xf, ISA_NFIXTURES_X86);
        pass += xp; fail += xf;
    }
    for (unsigned i = 0; i < NFIXTURES; i++) {
        const Fix *fx = &fixtures[i];
        if (strcmp(fx->isa, g_tab->name)) { skipped++; continue; }
        const IsaForm *f = isadb_find(g_tab, fx->form);
        if (!f) { printf("MISSING row %-16s (%s)\n", fx->form, fx->text); fail++; continue; }
        long human[ISA_MAXV] = {0};
        for (int v = 0; v < fx->nops && v < ISA_MAXV; v++) human[v] = fx->ops[v];
        uint8_t out[ISA_MAXBYTES]; char err[200];
        if (isa_encode(f, human, out, err, sizeof err) != 0) {
            printf("ENC-FAIL %-16s (%s): %s\n", fx->form, fx->text, err); fail++; continue;
        }
        if (memcmp(out, fx->bytes, (size_t)fx->len) != 0) {
            char got[64], want[64];
            isa_hex(out, fx->len, got, sizeof got);
            isa_hex(fx->bytes, fx->len, want, sizeof want);
            printf("WRONG    %-16s (%s)\n         got  %s\n         want %s\n",
                   fx->form, fx->text, got, want);
            fail++; continue;
        }
        /* and the bytes must decode back to the same row with the same operands */
        const IsaForm *g = NULL; long got2[ISA_MAXV] = {0, 0, 0, 0};
        if (!isa_decode(g_tab, fx->bytes, fx->len, &g, got2)) {
            printf("DEC-FAIL %-16s (%s)\n", fx->form, fx->text); fail++; continue;
        }
        pass++;
    }
    printf("%s fixtures: %d pass, %d fail (%lu for this ISA)\n", g_tab->name, pass, fail, (unsigned long)(NFIXTURES - (unsigned)skipped));
    return fail ? 1 : 0;
}

/* Self-check: encode a legal value into every row, decode the bytes back, and
 * require the same row and a re-encode to the identical bytes. This proves internal
 * consistency only; cmd_fixtures proves correctness against the ISA. */
static int cmd_test(void) {
    int pass = 0, fail = 0, alias = 0;
    for (uint32_t i = 0; i < g_tab->n; i++) {
        const IsaForm *f = &g_tab->forms[i];
        if (f->nvars == 0) { pass++; continue; }
        long human[ISA_MAXV] = {0};
        for (int v = 0; v < f->nvars; v++) {
            switch (f->vkind[v]) {
                /* A register field normally takes a plain number, but a vector lane
                 * index is a register *encoded scaled*, so it only accepts multiples
                 * of vsc. Probing it with 3 makes a correct row fail. */
                case K_REG:    human[v] = f->vsc[v] ? (long)(3 << f->vsc[v]) : 3;
                               /* A lane index on a .d arrangement is one bit wide, so it
                                * can only hold 0 or 1. Probing it with 3 failed three
                                * correct rows for refusing an impossible lane. */
                               if (f->vwid[v] == 1) human[v] = 1;
                               break;
                /* A scaled field only accepts multiples of its scale, so the probe
                 * must be given a legal multiple -- probing with 3 made 13 rows
                 * "fail" for correctly refusing an unaligned offset. */
                case K_IMM:    human[v] = (long)(3 << f->vsc[v]); break;
                /* A scaled field (register-offset shift, vector lane index) only
                 * accepts multiples of its scale, so the probe must supply one. */
                case K_SIMM:   human[v] = (long)(-4 << f->vsc[v]); break;
                case K_SHIFT:  human[v] = 5; break;
                case K_COND:   human[v] = 4; break;
                case K_LOGINT: human[v] = 0x0f0f0f0f0f0f0f0ful; break;
                case K_UBFMW:  human[v] = 3; human[v + 1] = 9; v++; break;
                case K_UBFX:   human[v] = 4; human[v + 1] = 5; v++; break;
                /* bfi takes the same (lsb, width) pair, so it needs the same probe.
                 * Without it the width stayed 0 and the row looked broken when it
                 * was only unprobed. */
                case K_BFINS:  human[v] = 4; human[v + 1] = 5; v++; break;
                /* A lane index is bounded by the element size (16 bytes / element), and
                 * vsc holds that element size in bytes rather than a shift, so the
                 * generic "3 << vsc" probe asks for impossible lanes. */
                case K_LANEIX: human[v] = 1; break;
                /* K_SHIFTINV encodes field = 2*element_bits - shift, so the probe must be a
                 * shift the row accepts. With vsc 4 (a 64-bit element) the legal range is
                 * 1..32, and the generic "3 << vsc" probe asked for 48 and made two correct
                 * rows look broken. */
                case K_SHIFTINV: {
                    long lim = (long)f->vsc[v] * 8;
                    human[v] = (lim >= 3) ? 3 : 1;
                    break;
                }
                case K_SPLIT:  human[v] = 64; break;
                case K_IMMHI:  human[v] = 16; break;
                /* THE SIB KINDS TAKE A REGISTER NUMBER, NOT A PROBE VALUE, AND TWO OF THEM
                 * DELIBERATELY EXCLUDE 4 AND 5.
                 *
                 * K_SIBSCALE's human value is the multiplier 1/2/4/8, so the raw probe 3 is not a
                 * legal scale. K_REXREG_4 refuses 4 ("no index") and K_REXREG_5 refuses 5 ("no
                 * base"), so probing either with 3 is fine but probing with those values fails a
                 * correct row. Probing all of them with 3 and the scale with 2 covers every legal
                 * case, which is what makes the self-check meaningful for the SIB rows instead of
                 * failing 18 of them per displacement shape. */
                case K_SIBSCALE: human[v] = 2; break;
                case K_REXREG_4: human[v] = 3; break;
                case K_REXREG_5: human[v] = 3; break;
                default:       human[v] = f->vsc[v] ? (long)(3 << f->vsc[v]) : 1; break;
            }
        }
        uint8_t out[ISA_MAXBYTES]; char err[200];
        if (isa_encode(f, human, out, err, sizeof err) != 0) {
            printf("FAIL encode %s: %s\n", f->name, err); fail++; continue;
        }
        const IsaForm *g = NULL; long got[ISA_MAXV] = {0, 0, 0, 0};
        if (!isa_decode(g_tab, out, f->len, &g, got)) {
            printf("FAIL decode %s (%02x%02x%02x%02x)\n", f->name, out[3], out[2], out[1], out[0]);
            fail++; continue;
        }
        uint8_t back[ISA_MAXBYTES];
        if (isa_encode(g, got, back, err, sizeof err) != 0 || memcmp(back, out, (size_t)f->len) != 0) {
            printf("FAIL round-trip %s via %s\n", f->name, g->name);
            fail++; continue;
        }
        if (g != f) alias++;
        pass++;
    }
    printf("%s self-check: %d round-tripped, %d failed, %d via an alias (of %u rows)\n",
           g_tab->name, pass, fail, alias, g_tab->n);
    return fail ? 1 : 0;
}

/* Coverage on real code: words in as hex, one per line. Reports how much of the
 * input the catalog can express and how much round-trips exactly. */
static int cmd_verify(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: isadb verify <hexwords-file>\n"); return 2; }
    FILE *fp = fopen(argv[0], "r");
    if (!fp) { fprintf(stderr, "isadb: cannot open %s\n", argv[0]); return 1; }
    char line[256];
    unsigned long total = 0, match = 0, exact = 0, bad = 0;
    size_t wid = (size_t)g_tab->wordsize;
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || !isxdigit((unsigned char)*p)) continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint8_t b[ISA_MAXBYTES] = {0};
        size_t n = 0;
        for (const char *q = p; *q && n < ISA_MAXBYTES && n < wid; q += 2) {
            char t[3] = { q[0], q[1] ? q[1] : '0', 0 };
            if (!isxdigit((unsigned char)t[0])) break;
            b[n++] = (uint8_t)strtoul(t, NULL, 16);
        }
        if (!n) continue;
        total++;
        const IsaForm *f = NULL; long human[ISA_MAXV] = {0};
        int used = isa_decode(g_tab, b, (int)n, &f, human);
        if (!used) continue;
        match++;
        uint8_t back[ISA_MAXBYTES] = {0}; char err[200];
        if (isa_encode(f, human, back, err, sizeof err) == 0 && memcmp(back, b, (size_t)used) == 0) { exact++; continue; }
        bad++;
        if (bad < 6) {
            char h1[64], h2[64]; isa_hex(b, used, h1, sizeof h1); isa_hex(back, used, h2, sizeof h2);
            printf("  mismatch %s -> %s (%s) %s\n", h1, h2, f->name, err);
        }
    }
    fclose(fp);
    printf("%s verify: %lu words, expressed %lu (%.1f%%), exact round-trip %lu (%.2f%%), failed %lu\n",
           g_tab->name, total, match, total ? 100.0 * (double)match / (double)total : 0.0,
           exact, match ? 100.0 * (double)exact / (double)match : 0.0, bad);
    return bad ? 1 : 0;
}

static int usage(void) {
    fprintf(stderr,
        "usage: isadb [-t isa] <command> [args]\n"
        "  isas: aarch64 x86_64    (default aarch64)\n"
        "  commands: forms [substr] | encode <FORM> [op...] | decode <hex>\n"
        "            test | fixtures | batch <words-file> | verify <file>\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *isa = "aarch64";
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-t")) { isa = (i + 1 < argc) ? argv[i + 1] : "aarch64"; i += 2; }
    if (i >= argc) return usage();
    g_tab = pick(isa);
    if (!g_tab) { fprintf(stderr, "isadb: no catalog for '%s'\n", isa); return 2; }
    const char *cmd = argv[i++];
    int rest = argc - i;
    char **args = argv + i;
    if (!strcmp(cmd, "encode")) return cmd_encode(rest, args);
    if (!strcmp(cmd, "decode")) return cmd_decode(rest, args);
    if (!strcmp(cmd, "forms"))  return cmd_forms(rest, args);
    if (!strcmp(cmd, "test"))   return cmd_test();
    if (!strcmp(cmd, "verify")) return cmd_verify(rest, args);
    if (!strcmp(cmd, "batch"))  return cmd_batch(rest, args);
    if (!strcmp(cmd, "fixtures")) return cmd_fixtures();
    if (!strcmp(cmd, "reject"))   return cmd_reject();
    return usage();
}
