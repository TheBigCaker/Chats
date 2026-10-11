/* binui.c -- interactive front end for the binast toolchain.
 *
 * One screen, three panes, no arguments to memorise:
 *
 *   SEARCH   type a mnemonic or a form name, the catalog filters live
 *   FORMS    the matching rows; pick one to load its field template
 *   OPERANDS type the operand values, see the bytes as you go
 *   BYTES    the encoding, and what the word decodes back to
 *
 * Also answers the two questions a person actually has in front of an unfamiliar
 * binary: what is this word, and what is at this address.
 *
 * Commands in the operand pane:
 *   <value>      set the next operand
 *   -            clear the operand being set
 *   d            decode the bytes currently shown
 *   w <hex>      decode a word you paste in
 *   r <a> <n>    read n instructions at address a from the loaded ELF
 *   f <path>     load an ELF to read from / patch
 *   p <addr>     plant the current encoding at an address in the loaded ELF
 *   o <addr>     show the encoding that address currently holds
 *   x            switch ISA (aarch64 / x86-64)
 *   h / ?        help
 *   q            quit
 *
 * Pure C11, no dependencies. Terminal handling is ANSI + termios, so it works over
 * adb shell, ssh, and a local terminal alike.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdarg.h>

#include "isadb.h"

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"

static const char *kind_name(int k);

#include "ISA_AARCH64.inc"
#include "ISA_X86_64.inc"
static const IsaTable T_aarch64 = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };
static const IsaTable T_x86_64  = { isa_forms_x86_64,  ISA_NFORMS_X86_64,  "x86_64",  1, 16 };
static const IsaTable *g_tab = &T_aarch64;

/* ---- terminal ---- */
static struct termios g_saved;
static int g_raw = 0;

static void raw_on(void) {
    if (g_raw || !isatty(0)) return;
    tcgetattr(0, &g_saved);
    struct termios t = g_saved;
    t.c_lflag &= ~(unsigned)(ICANON | ECHO);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    g_raw = 1;
}
static void raw_off(void) {
    if (!g_raw) return;
    tcsetattr(0, TCSANOW, &g_saved);
    g_raw = 0;
}

#define RESET "\033[0m"
#define BOLD  "\033[1m"
#define DIM   "\033[2m"
#define REV   "\033[7m"
#define CYAN  "\033[36m"
#define GREEN "\033[32m"
#define RED   "\033[31m"
#define YEL   "\033[33m"

static int term_rows(void) {
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row > 10) return w.ws_row;
    return 24;
}
static int term_cols(void) {
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 40) return w.ws_col;
    return 80;
}
static void clear_screen(void) { fputs("\033[2J\033[H", stdout); }

/* ---- state ---- */
#define MAXMATCH 512
static const IsaForm *g_match[MAXMATCH];
static int g_nmatch = 0;
static int g_sel = 0;

static char g_search[64] = "";
static const IsaForm *g_form = NULL;
static long g_ops[ISA_MAXV] = {0,0,0,0};
static int  g_nops = 0;          /* operands entered so far */
static uint8_t g_bytes[ISA_MAXBYTES];
static int  g_len = 0;
static char g_msg[256] = "";
static int  g_msg_kind = 0;      /* 0 info, 1 ok, 2 error */

/* what is loaded, if anything */
static char g_elf[256] = "";
static uint8_t *g_image = NULL;
static size_t g_image_n = 0;

static void msg(int kind, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_msg_kind = kind;
}

/* ---- ELF access: map through the executable PT_LOAD ---- */
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint64_t g_seg_off = 0, g_seg_va = 0;
static size_t   g_seg_len = 0;

/* Code sections of the loaded file. A PT_LOAD covers everything the kernel maps -- .rodata,
 * .eh_frame, relocation tables -- and only the SHF_EXECINSTR sections hold instructions.
 * Coverage must be measured against those, or the denominator is inflated with data. */
#define MAX_CODE_SEC 32
static struct { uint64_t off, size; } g_code[MAX_CODE_SEC];
static int g_ncode = 0;

static int elf_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { msg(2, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 64) { fclose(f); msg(2, "%s is too small to be an ELF", path); return -1; }
    uint8_t *p = arb_bytes((size_t)n);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { fclose(f); arbit_release_carve((long)(p)); msg(2, "short read on %s", path); return -1; }
    fclose(f);
    if (memcmp(p, "\x7f" "ELF", 4)) { arbit_release_carve((long)(p)); msg(2, "%s is not an ELF", path); return -1; }
    int phnum = r16(p + 0x38), phent = r16(p + 0x36);
    uint64_t phoff = r64(p + 0x20);
    uint64_t off = 0, va = 0; size_t len = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = p + phoff + (uint64_t)i * phent;
        if (r32(ph) != 1) continue;
        if (!(r32(ph + 4) & 1)) continue;
        uint64_t o = r64(ph + 8), a = r64(ph + 16), fs = r64(ph + 32);
        if (!fs || o >= (uint64_t)n) continue;
        if (o + fs > (uint64_t)n) fs = (uint64_t)n - o;
        off = o; va = a; len = (size_t)fs;
        break;
    }
    if (!len) { arbit_release_carve((long)(p)); msg(2, "%s has no executable segment", path); return -1; }
    arbit_release_carve((long)(g_image));
    g_image = p; g_image_n = (size_t)n;
    g_seg_off = off; g_seg_va = va; g_seg_len = len;
    snprintf(g_elf, sizeof g_elf, "%s", path);

    /* Collect SHF_EXECINSTR sections so coverage and scans can tell code from data. */
    g_ncode = 0;
    uint64_t shoff = r64(p + 0x28);
    int shnum = r16(p + 0x3c), shent = r16(p + 0x3a);
    for (int i = 0; i < shnum && g_ncode < MAX_CODE_SEC; i++) {
        const uint8_t *sh = p + shoff + (uint64_t)i * shent;
        uint64_t so = r64(sh + 0x18), ss = r64(sh + 0x20);
        if (so && ss && so < (uint64_t)n) {
            uint64_t fl = r64(sh + 0x08);
            if (fl & 0x4) {   /* SHF_EXECINSTR */
                if (ss > (uint64_t)n - so) ss = (uint64_t)n - so;
                g_code[g_ncode].off = so;
                g_code[g_ncode].size = ss;
                g_ncode++;
            }
        }
    }
    msg(1, "loaded %s  code %#llx..%#llx (%zu bytes), %d code section(s)", path,
        (unsigned long long)va, (unsigned long long)(va + len), len, g_ncode);
    return 0;
}

static const uint8_t *image_at(uint64_t addr) {
    if (!g_image) return NULL;
    if (addr < g_seg_va || addr + 4 > g_seg_va + g_seg_len) return NULL;
    return g_image + g_seg_off + (addr - g_seg_va);
}
static uint64_t file_off_of(uint64_t addr) {
    return g_seg_off + (addr - g_seg_va);
}

/* ---- catalog ---- */
static void refilter(void) {
    g_nmatch = 0;
    for (uint32_t i = 0; i < g_tab->n && g_nmatch < MAXMATCH; i++) {
        const IsaForm *f = &g_tab->forms[i];
        if (g_search[0]) {
            if (!strstr(f->name, g_search) && !strstr(f->mnemonic, g_search)
                && !strstr(f->operands, g_search)) continue;
        }
        g_match[g_nmatch++] = f;
    }
    if (g_sel >= g_nmatch) g_sel = g_nmatch ? g_nmatch - 1 : 0;
}

static void hex_of(const uint8_t *b, int len, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < len; i++) {
        char t[8];
        snprintf(t, sizeof t, "%02x%s", b[i], i + 1 < len ? " " : "");
        strncat(out, t, n - strlen(out) - 1);
    }
}
/* render bytes as the word a listing shows: most significant byte first */
static void word_of(const uint8_t *b, int len, char *out, size_t n) {
    out[0] = 0;
    for (int i = len - 1; i >= 0; i--) {
        char t[8];
        snprintf(t, sizeof t, "%02x", b[i]);
        strncat(out, t, n - strlen(out) - 1);
    }
}

static void encode_current(void) {
    if (!g_form) return;
    char err[256];
    if (isa_encode(g_form, g_ops, g_bytes, err, sizeof err) != 0) {
        g_len = 0;
        snprintf(g_msg, sizeof g_msg, "%s", err);
        g_msg_kind = 2;
        return;
    }
    g_len = g_form->len;
}

static void select_form(int idx) {
    if (idx < 0 || idx >= g_nmatch) return;
    g_sel = idx;
    g_form = g_match[idx];
    g_nops = 0;
    memset(g_ops, 0, sizeof g_ops);
    memset(g_bytes, 0, sizeof g_bytes);
    g_len = 0;
    encode_current();
}

static void apply_operand(long v) {
    if (!g_form) return;
    if (g_nops >= g_form->nvars) {
        /* wrap: start again with a clean operand set */
        memset(g_ops, 0, sizeof g_ops);
        g_nops = 0;
    }
    g_ops[g_nops++] = v;
    encode_current();
}

/* ---- rendering ---- */
static const char *kind_name(int k) {
    switch (k) {
        case K_REG: return "reg";
        case K_IMM: return "imm";
        case K_SHIFT: return "shift";
        case K_LOGINT: return "logical-imm";
        case K_UBFMW: return "immr/imms";
        case K_COND: return "cond";
        case K_SIMM: return "signed";
        case K_UBFX: return "lsb/width";
        case K_SPLIT: return "split-imm";
        case K_IMMHI: return "shift<<16";
        default: return "value";
    }
}

static void draw(void) {
    int rows = term_rows(), cols = term_cols();
    (void)cols;
    clear_screen();

    printf(BOLD "binast" RESET DIM "  ISA catalog and binary editor" RESET "\n");
    if (g_elf[0]) printf(DIM "  file: " RESET "%s  " DIM "code %#llx..%#llx" RESET "\n",
                         g_elf, (unsigned long long)g_seg_va, (unsigned long long)(g_seg_va + g_seg_len));
    else printf(DIM "  no file loaded  (f <path> to load one)" RESET "\n");
    printf("\n");

    printf(BOLD "1 SEARCH" RESET "  " CYAN "%s" RESET "_\n", g_search);
    printf("\n");

    /* forms list, with a window around the selection */
    int listrows = rows - 16;
    if (listrows < 4) listrows = 4;
    printf(BOLD "2 FORMS" RESET DIM "  %d match%s of %u" RESET "\n", g_nmatch,
           g_nmatch == 1 ? "" : "es", g_tab->n);
    int start = 0;
    if (g_sel >= listrows) start = g_sel - listrows + 1;
    for (int i = start; i < g_nmatch && i < start + listrows; i++) {
        const IsaForm *f = g_match[i];
        if (i == g_sel) printf(REV "  %-22s %-9s %-28s%2dB" RESET "\n", f->name, f->mnemonic, f->operands, f->len);
        else            printf("  %-22s %-9s %-28s%2dB\n", f->name, f->mnemonic, f->operands, f->len);
    }
    if (!g_nmatch) printf(DIM "  (nothing matches; backspace to widen)\n" RESET);
    printf("\n");

    /* operand + byte panes */
    if (g_form) {
        printf(BOLD "3 OPERANDS" RESET "  %s %s  " DIM "(%d of %d set)" RESET "\n",
               g_form->mnemonic, g_form->operands, g_nops, g_form->nvars);
        for (int v = 0; v < g_form->nvars; v++) {
            const char *mark = (v == g_nops) ? "  <-- next" : "";
            printf("    %-14s = %-12ld %s" DIM "%s" RESET "\n", kind_name(g_form->vkind[v]),
                   v < g_nops ? g_ops[v] : 0, v < g_nops ? "" : "", mark);
        }
        printf("\n");
        printf(BOLD "4 BYTES" RESET "  ");
        if (g_len) {
            char hx[64], wd[64];
            hex_of(g_bytes, g_len, hx, sizeof hx);
            word_of(g_bytes, g_len, wd, sizeof wd);
            printf(GREEN "%s" RESET DIM "   word %s" RESET "\n", hx, wd);
            if (g_tab->wordsize == 4) {
                const IsaForm *d = NULL; long h[ISA_MAXV] = {0,0,0,0};
                if (isa_decode(g_tab, g_bytes, g_len, &d, h)) printf(DIM "     decodes back as %s\n" RESET, d->name);
                else printf(DIM "     no catalog row decodes these bytes\n" RESET);
            }
        } else printf(DIM "(not encodable yet -- see the message below)\n" RESET);
    } else {
        printf(BOLD "3 OPERANDS" RESET DIM "  pick a form above\n" RESET);
    }
    printf("\n");

    if (g_msg[0]) {
        const char *c = g_msg_kind == 1 ? GREEN : g_msg_kind == 2 ? RED : YEL;
        printf("%s%s" RESET "\n", c, g_msg);
    }
    printf("\n" DIM "up/down select   enter load   type to search   h help   q quit" RESET "\n");
    fflush(stdout);
}

static void help(void) {
    clear_screen();
    printf(BOLD "binast -- commands\n\n" RESET);
    printf("  " BOLD "search pane" RESET "   just type. The list filters as you go.\n");
    printf("                    up/down or k/j move, enter loads the form.\n\n");
    printf("  " BOLD "operand pane" RESET "\n");
    printf("    <number>        set the next operand (decimal, or 0x... for hex)\n");
    printf("    -               clear the operands and start again\n");
    printf("    d               decode the bytes in pane 4\n");
    printf("    w <hex>         decode a word you paste, e.g. w d0000020\n");
    printf("    r <addr> <n>    read n instructions at an address in the loaded file\n");
    printf("    o <addr>        show the encoding already at an address\n");
    printf("    p <addr>        plant the current encoding at an address (writes a copy)\n");
    printf("    f <path>        load an ELF\n");
    printf("    x               switch ISA (aarch64 / x86-64)\n");
    printf("    h               this help\n");
    printf("    q               quit\n\n");
    printf(DIM "  Nothing here writes to the file you loaded until you use p, and that writes a\n"
           "  side-by-side copy so the original is never touched.\n\n" RESET);
    printf("press any key\n");
    fflush(stdout);
    raw_on();
    { int rc = getchar(); (void)rc; }
}

/* ---- read instructions out of the loaded ELF ---- */
static void read_at(uint64_t addr, int n) {
    clear_screen();
    printf(BOLD "instructions at %#llx\n\n" RESET, (unsigned long long)addr);
    const uint8_t *p = image_at(addr);
    if (!p) { printf(RED "  address is outside the executable segment\n" RESET); }
    else {
        int left = (int)(g_seg_va + g_seg_len - addr) ;
        int shown = 0, unknown = 0;
        for (int i = 0; i < n && shown < left; i++) {
            const IsaForm *f = NULL; long h[ISA_MAXV] = {0,0,0,0};
            int used = isa_decode(g_tab, p + i * g_tab->wordsize, left - i * g_tab->wordsize, &f, h);
            char hx[64];
            hex_of(p + i * g_tab->wordsize, g_tab->wordsize, hx, sizeof hx);
            if (!used) {
                printf("  " YEL "%012llx" RESET "  %-14s " DIM "no catalog row" RESET "\n",
                       (unsigned long long)(addr + (uint64_t)i * g_tab->wordsize), hx);
                unknown++;
                used = g_tab->wordsize;
            } else {
                printf("  " CYAN "%012llx" RESET "  %-14s %-20s", 
                       (unsigned long long)(addr + (uint64_t)i * g_tab->wordsize), hx, f->name);
                for (int v = 0; v < f->nvars; v++) printf(" %ld", h[v]);
                printf("\n");
            }
            i += used / g_tab->wordsize - 1;
            shown++;
        }
        printf("\n" DIM "  %d word(s), %d with no catalog row\n" RESET, shown, unknown);
    }
    printf("\npress any key\n");
    fflush(stdout);
    raw_on();
    { int rc = getchar(); (void)rc; }
}

static void show_at(uint64_t addr) {
    const uint8_t *p = image_at(addr);
    if (!p) { msg(2, "%#llx is outside the executable segment", (unsigned long long)addr); return; }
    char hx[64];
    hex_of(p, g_tab->wordsize, hx, sizeof hx);
    const IsaForm *f = NULL; long h[ISA_MAXV] = {0,0,0,0};
    if (isa_decode(g_tab, p, g_tab->wordsize, &f, h)) {
        char extra[128] = "";
        for (int v = 0; v < f->nvars; v++) {
            char t[24];
            snprintf(t, sizeof t, " %ld", h[v]);
            strncat(extra, t, sizeof extra - strlen(extra) - 1);
        }
        msg(1, "%#llx  %s  %s%s", (unsigned long long)addr, hx, f->name, extra);
    } else {
        msg(0, "%#llx  %s  no catalog row", (unsigned long long)addr, hx);
    }
}

static void plant_at(uint64_t addr) {
    if (!g_image) { msg(2, "load a file first (f <path>)"); return; }
    if (!g_len) { msg(2, "nothing encoded yet"); return; }
    const uint8_t *p = image_at(addr);
    if (!p) { msg(2, "%#llx is outside the executable segment", (unsigned long long)addr); return; }
    char out[300], hx[64];
    snprintf(out, sizeof out, "%s.patched", g_elf);
    FILE *o = fopen(out, "wb");
    if (!o) { msg(2, "cannot write %s", out); return; }
    fwrite(g_image, 1, g_image_n, o);
    fclose(o);
    FILE *r = fopen(out, "r+b");
    if (!r) { msg(2, "cannot reopen %s", out); return; }
    fseek(r, (long)file_off_of(addr), SEEK_SET);
    fwrite(g_bytes, 1, (size_t)g_len, r);
    fclose(r);
    chmod(out, 0755);
    hex_of(g_bytes, g_len, hx, sizeof hx);
    msg(1, "planted %s at %#llx -> %s", hx, (unsigned long long)addr, out);
}

/* ---- input ---- */
static void do_operand_line(char *line) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return;

    if (*s == 'q' && !s[1]) { raw_off(); clear_screen(); exit(0); }
    if ((*s == 'h' || *s == '?') && !s[1]) { help(); return; }
    if (*s == 'x' && !s[1]) {
        g_tab = (g_tab == &T_aarch64) ? &T_x86_64 : &T_aarch64;
        g_form = NULL; g_nops = 0; g_len = 0;
        memset(g_ops, 0, sizeof g_ops);
        refilter();
        msg(0, "ISA is now %s", g_tab->name);
        return;
    }
    if (*s == '-' && !s[1]) {
        memset(g_ops, 0, sizeof g_ops);
        g_nops = 0;
        encode_current();
        msg(0, "operands cleared");
        return;
    }
    if (*s == 'd' && !s[1]) {
        if (!g_len) { msg(2, "nothing to decode"); return; }
        const IsaForm *f = NULL; long h[ISA_MAXV] = {0,0,0,0};
        if (isa_decode(g_tab, g_bytes, g_len, &f, h)) {
            char extra[128] = "";
            for (int v = 0; v < f->nvars; v++) {
                char t[24]; snprintf(t, sizeof t, " %ld", h[v]);
                strncat(extra, t, sizeof extra - strlen(extra) - 1);
            }
            msg(1, "decodes as %s%s", f->name, extra);
        } else msg(2, "no catalog row decodes these bytes");
        return;
    }
    if (*s == 'w') {
        char *a = s + 1;
        while (*a == ' ') a++;
        if (!*a) { msg(2, "usage: w <hex word>"); return; }
        /* accept d0000020 or d0 00 00 20 */
        uint8_t b[ISA_MAXBYTES]; memset(b, 0, sizeof b); int n = 0;
        char *sp = strpbrk(a, " ,");
        if (!sp && strlen(a) == (size_t)g_tab->wordsize * 2) {
            for (size_t i = 0; i + 1 < strlen(a); i += 2) {
                char t[3] = { a[i], a[i+1], 0 };
                b[n++] = (uint8_t)strtoul(t, NULL, 16);
            }
            for (int i = 0, j = n - 1; i < j; i++, j--) { uint8_t x = b[i]; b[i] = b[j]; b[j] = x; }
        } else {
            while (*a && n < ISA_MAXBYTES) {
                while (*a == ' ' || *a == ',') a++;
                if (!*a) break;
                char *t = a;
                if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
                int dg = 0; while (isxdigit((unsigned char)*t)) { t++; dg++; }
                if (!dg || dg > 2) break;
                b[n++] = (uint8_t)strtoul((a[0]=='0' && (a[1]=='x'||a[1]=='X')) ? a+2 : a, NULL, 16);
                a = t;
            }
        }
        if (!n) { msg(2, "could not read a word from that"); return; }
        memcpy(g_bytes, b, sizeof b); g_len = n;
        const IsaForm *f = NULL; long h[ISA_MAXV] = {0,0,0,0};
        if (isa_decode(g_tab, b, n, &f, h)) {
            char hx[64]; hex_of(b, n, hx, sizeof hx);
            msg(1, "%s  =  %s", hx, f->name);
            g_form = f;
            for (int v = 0; v < f->nvars; v++) g_ops[v] = h[v];
            g_nops = f->nvars;
        } else {
            char hx[64]; hex_of(b, n, hx, sizeof hx);
            msg(2, "%s  no catalog row", hx);
        }
        return;
    }
    if (*s == 'f') {
        char *a = s + 1; while (*a == ' ') a++;
        if (!*a) { msg(2, "usage: f <path-to-elf>"); return; }
        elf_load(a);
        return;
    }
    if (*s == 'r') {
        char *a = s + 1; unsigned long long addr = 0; int n = 8;
        while (*a == ' ') a++;
        if (!*a) { msg(2, "usage: r <address> [count]"); return; }
        addr = strtoull(a, &a, 16);
        while (*a == ' ') a++;
        if (*a) n = atoi(a);
        if (n <= 0 || n > 200) n = 8;
        read_at((uint64_t)addr, n);
        return;
    }
    if (*s == 'o') {
        char *a = s + 1; while (*a == ' ') a++;
        if (!*a) { msg(2, "usage: o <address>"); return; }
        show_at((uint64_t)strtoull(a, NULL, 16));
        return;
    }
    if (*s == 'p') {
        char *a = s + 1; while (*a == ' ') a++;
        if (!*a) { msg(2, "usage: p <address>"); return; }
        plant_at((uint64_t)strtoull(a, NULL, 16));
        return;
    }
    /* otherwise: an operand value */
    if (!g_form) { msg(2, "pick a form first"); return; }
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s) { msg(2, "\"%s\" is not a number or a command (h for help)", s); return; }
    apply_operand(v);
}

/* editing the search string */
static void search_key(int c) {
    size_t n = strlen(g_search);
    if (c == 127 || c == 8) { if (n) g_search[n-1] = 0; }
    else if (c == 21) g_search[0] = 0;                 /* ctrl-U */
    else if (isprint(c) && n + 1 < sizeof g_search) { g_search[n] = (char)c; g_search[n+1] = 0; }
    refilter();
}

/* ---- non-interactive mode for agents -------------------------------------------
 * `binui agent` prints a machine-readable description of everything this tool can do:
 * the ISAs, the forms with their operand kinds, and the commands. Nothing here needs a
 * terminal, so a script or another program can drive the toolchain without parsing
 * human output or reading documentation.
 *
 * `binui tell <isa> <form> <ops...>` answers one question and exits: it prints the
 * encoding and the decoded fields as key=value lines.
 * `binui look <isa> <hex>` answers "what is this word".
 */
static int agent_manifest(void) {
    const IsaTable *tabs[2] = { &T_aarch64, &T_x86_64 };
    printf("tool binui\n");
    printf("isa_count 2\n");
    for (int t = 0; t < 2; t++) {
        const IsaTable *tb = tabs[t];
        printf("isa %s forms %u wordsize %d\n", tb->name, tb->n, tb->wordsize);
    }
    printf("command encode  isa form ops...   -> bytes on one line\n");
    printf("command decode  isa hex           -> form and operand values\n");
    printf("command forms   isa [substring]   -> matching form names\n");
    printf("command read    elf addr count    -> address, bytes, form, operands\n");
    printf("command plant   elf addr isa form ops... -> writes ELF.patched\n");
    printf("command coverage elf              -> words, matched, unmatched\n");
    printf("command words   elf [--forms]     -> EXECUTABLE words only, one per line\n");
    printf("form_operand_kinds");
    for (int k = 0; k <= K_PAGCNT; k++) printf(" %s", kind_name(k));
    printf("\nend\n");
    return 0;
}

static const IsaTable *tab_by_name(const char *n) {
    if (!strcmp(n, "aarch64")) return &T_aarch64;
    if (!strcmp(n, "x86_64")) return &T_x86_64;
    return NULL;
}

static int agent_encode_decode(const char *isa, const char *form, char **ops, int nops) {
    const IsaTable *tb = tab_by_name(isa);
    if (!tb) { fprintf(stderr, "unknown isa %s\n", isa); return 2; }
    const IsaForm *f = isadb_find(tb, form);
    if (!f) { fprintf(stderr, "no form %s\n", form); return 2; }
    long v[ISA_MAXV] = {0,0,0,0};
    for (int i = 0; i < nops && i < ISA_MAXV; i++) v[i] = strtol(ops[i], NULL, 0);
    uint8_t b[ISA_MAXBYTES]; char err[256];
    if (isa_encode(f, v, b, err, sizeof err) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    char hx[64]; hex_of(b, f->len, hx, sizeof hx);
    printf("isa %s\nform %s\nlen %d\nbytes %s\n", tb->name, f->name, f->len, hx);
    for (int i = 0; i < f->nvars; i++) printf("op%d %ld\n", i, v[i]);
    return 0;
}

static int agent_decode(const char *isa, const char *hex) {
    const IsaTable *tb = tab_by_name(isa);
    if (!tb) { fprintf(stderr, "unknown isa %s\n", isa); return 2; }
    uint8_t b[ISA_MAXBYTES]; memset(b, 0, sizeof b);
    int n = 0;
    size_t L = strlen(hex);
    if (L == (size_t)tb->wordsize * 2) {
        for (size_t i = 0; i + 1 < L; i += 2) { char t[3] = { hex[i], hex[i+1], 0 }; b[n++] = (uint8_t)strtoul(t, NULL, 16); }
        for (int i = 0, j = n - 1; i < j; i++, j--) { uint8_t x = b[i]; b[i] = b[j]; b[j] = x; }
    } else {
        const char *a = hex;
        while (*a && n < ISA_MAXBYTES) {
            while (*a == ' ' || *a == ',') a++;
            if (!*a) break;
            char *t = (char *)a;
            if (t[0]=='0' && (t[1]=='x'||t[1]=='X')) t += 2;
            int d = 0; while (isxdigit((unsigned char)*t)) { t++; d++; }
            if (!d || d > 2) break;
            b[n++] = (uint8_t)strtoul((a[0]=='0'&&(a[1]=='x'||a[1]=='X')) ? a+2 : a, NULL, 16);
            a = t;
        }
    }
    const IsaForm *f = NULL; long h[ISA_MAXV] = {0,0,0,0};
    if (!isa_decode(tb, b, n, &f, h)) { printf("form none\n"); return 1; }
    printf("isa %s\nform %s\noperands %s\n", tb->name, f->name, f->operands);
    for (int i = 0; i < f->nvars; i++) printf("op%d %ld\n", i, h[i]);
    return 0;
}

/* read one address range out of an ELF and print it machine-readably */
static int agent_read(const char *path, const char *addr_s, const char *count_s) {
    if (elf_load(path) != 0) return 2;
    uint64_t addr = strtoull(addr_s, NULL, 16);
    int n = count_s ? atoi(count_s) : 1;
    if (n <= 0) n = 1;
    if (n > 100000) n = 100000;
    const uint8_t *p = image_at(addr);
    if (!p) { fprintf(stderr, "address outside the executable segment\n"); return 2; }
    int left = (int)(g_seg_va + g_seg_len - addr);
    int unknown = 0, real = 0;
    for (int i = 0; i < n && i * g_tab->wordsize < left; i++) {
        const uint8_t *w = p + (size_t)i * g_tab->wordsize;
        const IsaForm *f = NULL; long h[ISA_MAXV] = {0,0,0,0};
        int used = isa_decode(g_tab, w, left - i * g_tab->wordsize, &f, h);
        char hx[64]; hex_of(w, g_tab->wordsize, hx, sizeof hx);
        if (!used) {
            printf("addr %012llx bytes %s form none\n",
                   (unsigned long long)(addr + (uint64_t)i * g_tab->wordsize), hx);
            unknown++;
        } else {
            printf("addr %012llx bytes %s form %s op",
                   (unsigned long long)(addr + (uint64_t)i * g_tab->wordsize), hx, f->name);
            for (int v = 0; v < f->nvars; v++) printf(" %ld", h[v]);
            printf("\n");
            real++;
        }
    }
    printf("count %d matched %d unmatched %d\n", real + unknown, real, unknown);
    return 0;
}

/* A word that fails to decode may still be perfectly legitimate: an assembler/compiler can
 * embed data inside the code section -- string literals, jump tables, embedded blobs -- and
 * those bytes are not instructions and never will be. Walk left and right from the word while
 * the bytes stay printable ASCII; if the run is at least INLINE_DATA_RUN bytes, the word is
 * part of an embedded string and is reported as data rather than as a coverage gap. */
#define INLINE_DATA_RUN 6
static int word_is_inline_data(const uint8_t *seg, size_t len, size_t off, int wordsize) {
    if (off + (size_t)wordsize > len) return 0;
    size_t lo = off;
    while (lo > 0 && seg[lo - 1] >= 32 && seg[lo - 1] < 127) lo--;
    size_t hi = off + (size_t)wordsize;
    while (hi < len && seg[hi] >= 32 && seg[hi] < 127) hi++;
    return (int)(hi - lo) >= INLINE_DATA_RUN;
}

/* the answer to "how much does this cover": every word of every code section, counted */
static int agent_coverage(const char *path) {
    if (elf_load(path) != 0) return 2;
    unsigned long total = 0, matched = 0, exact = 0, nonzero_unmatched = 0, inline_data = 0;
    unsigned long zero_words = 0;

    /* Prefer the executable sections. If the file has none recorded, fall back to the whole
     * executable segment so the command still says something useful. */
    size_t nsec = (size_t)g_ncode;
    for (size_t s = 0; s < (nsec ? nsec : 1); s++) {
        uint64_t so = nsec ? g_code[s].off : g_seg_off;
        size_t   ss = nsec ? (size_t)g_code[s].size : g_seg_len;
        if (!ss || ss < (size_t)g_tab->wordsize) continue;
        const uint8_t *seg = g_image + so;
        for (size_t off = 0; off + g_tab->wordsize <= ss; off += g_tab->wordsize) {
            const uint8_t *w = seg + off;
            int zero = 1;
            for (int k = 0; k < g_tab->wordsize; k++) if (w[k]) { zero = 0; break; }
            total++;
            if (zero) { zero_words++; continue; }
            const IsaForm *f = NULL; long h[ISA_MAXV] = {0,0,0,0};
            int used = isa_decode(g_tab, w, (int)(ss - off), &f, h);
            if (!used) {
                if (word_is_inline_data(seg, ss, off, g_tab->wordsize)) { inline_data++; continue; }
                nonzero_unmatched++;
                continue;
            }
            matched++;
            uint8_t chk[ISA_MAXBYTES]; char err[256];
            if (isa_encode(f, h, chk, err, sizeof err) == 0 && !memcmp(chk, w, (size_t)used)) exact++;
        }
    }
    printf("file %s\ncode_sections %d\nwords %lu\nmatched %lu\nexact %lu\n"
           "inline_data %lu\nzero_padding %lu\nunmatched_nonzero %lu\n",
           path, g_ncode, total, matched, exact, inline_data, zero_words, nonzero_unmatched);
    return 0;
}

/* plant a freshly encoded instruction into a copy of an ELF */
static int agent_plant(const char *path, const char *addr_s, const char *isa,
                       const char *form, char **ops, int nops) {
    if (elf_load(path) != 0) return 2;
    const IsaTable *tb = tab_by_name(isa);
    if (!tb) { fprintf(stderr, "unknown isa %s\n", isa); return 2; }
    const IsaForm *f = isadb_find(tb, form);
    if (!f) { fprintf(stderr, "no form %s\n", form); return 2; }
    long v[ISA_MAXV] = {0,0,0,0};
    for (int i = 0; i < nops && i < ISA_MAXV; i++) v[i] = strtol(ops[i], NULL, 0);
    uint8_t b[ISA_MAXBYTES]; char err[256];
    if (isa_encode(f, v, b, err, sizeof err) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    g_tab = tb;                                   /* image_at/plant use the active table */
    uint64_t addr = strtoull(addr_s, NULL, 16);
    if (!image_at(addr)) { fprintf(stderr, "address outside the executable segment\n"); return 2; }
    char before[64]; hex_of(image_at(addr), tb->wordsize, before, sizeof before);
    memcpy(g_bytes, b, sizeof g_bytes); g_len = f->len;
    char out[300];
    snprintf(out, sizeof out, "%s.patched", path);
    { FILE *r = fopen(out, "wb"); if (!r) { fprintf(stderr, "cannot write %s\n", out); return 2; } fwrite(g_image, 1, g_image_n, r); fclose(r); }
    { FILE *r = fopen(out, "r+b"); if (!r) { fprintf(stderr, "cannot reopen %s\n", out); return 2; }
      fseek(r, (long)file_off_of(addr), SEEK_SET); fwrite(b, 1, (size_t)f->len, r); fclose(r); }
    chmod(out, 0755);
    char after[64]; hex_of(b, f->len, after, sizeof after);
    printf("file %s\naddr %012llx\nbefore %s\nafter %s\nform %s\nout %s\n",
           path, (unsigned long long)addr, before, after, f->name, out);
    return 0;
}


/* ============================================================ agent: words ======
 * Dump the EXECUTABLE sections as bare hex words, one per line. This is the stream the
 * analysis tools want, and the distinction matters: `blink code <elf> --hex` dumps the
 * whole file (ELF header, strings, tables), and feeding THAT to a next-row predictor
 * yields a repeat-previous rate ABOVE the order-2 rate -- a symptom that looks like a
 * finding. Only SHF_EXECINSTR sections hold instructions.
 *
 *   binui words <elf> [--forms]   -> one bare word per line, or "rowid:payload"
 */
int agent_words(const char *path, int with_forms) {
    arbit_init(0);
    if (elf_load(path) != 0) { fprintf(stderr, "%s\n", g_msg); return 2; }
    if (!g_ncode) { fprintf(stderr, "%s: no executable section\n", path); return 2; }
    for (int s = 0; s < g_ncode; s++) {
        long n = (long)(g_code[s].size / 4);
        const uint8_t *base = g_image + g_code[s].off;
        for (long i = 0; i < n; i++) {
            uint32_t w = r32(base + i * 4);
            if (w == 0) continue;                 /* unreachable padding, not an instruction */
            if (!with_forms) { printf("%08x\n", w); continue; }
            const IsaForm *f = NULL; long ops[ISA_MAXV] = {0};
            if (isa_decode(&T_aarch64, base + i * 4, 4, &f, ops) && f) {
                uint32_t pay = 0;
                for (uint32_t v = 0; v < f->nvars; v++) {
                    int sc = f->vsc[v] > 0 ? f->vsc[v] : 0;
                    pay |= (uint32_t)ops[v] << (v * 8);
                    (void)sc;
                }
                long rowid = -1;
                for (uint32_t r = 0; r < T_aarch64.n; r++)
                    if (&T_aarch64.forms[r] == f) { rowid = (long)r; break; }
                printf("%08lx:%016llx\n", (unsigned long)(rowid < 0 ? 0 : rowid),
                       (unsigned long long)pay);
            } else {
                printf("ffffffff:%08x\n", (unsigned)w);   /* no form: keep the word, mark it */
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    arbit_init(0);   /* bring the cosmic sea up before the first allocation */
    if (argc >= 2 && !strcmp(argv[1], "agent")) return agent_manifest();
    if (argc >= 4 && !strcmp(argv[1], "tell")) return agent_encode_decode(argv[2], argv[3], argv + 4, argc - 4);
    if (argc >= 4 && !strcmp(argv[1], "look")) return agent_decode(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "coverage")) return agent_coverage(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "words"))
        return agent_words(argv[2], argc > 3 && !strcmp(argv[3], "--forms"));
    if (argc >= 4 && !strcmp(argv[1], "read"))  return agent_read(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    if (argc >= 6 && !strcmp(argv[1], "plant")) return agent_plant(argv[2], argv[3], argv[4], argv[5], argv + 6, argc - 6);
    if (argc >= 3 && !strcmp(argv[1], "forms")) {
        const IsaTable *tb = argc > 2 ? tab_by_name(argv[2]) : &T_aarch64;
        if (!tb) { fprintf(stderr, "unknown isa %s\n", argv[2]); return 2; }
        const char *sub = argc > 3 ? argv[3] : "";
        int n = 0;
        for (uint32_t i = 0; i < tb->n; i++) {
            const IsaForm *f = &tb->forms[i];
            if (sub[0] && !strstr(f->name, sub) && !strstr(f->mnemonic, sub)) continue;
            printf("%s %s %s\n", f->name, f->mnemonic, f->operands);
            n++;
        }
        printf("count %d\n", n);
        return 0;
    }
    raw_on();
    atexit(raw_off);
    refilter();
    if (argc > 1) elf_load(argv[1]);

    int mode = 0;  /* 0 = browsing forms, 1 = typing a command line */
    char line[256];
    size_t linelen = 0;
    line[0] = 0;

    for (;;) {
        draw();
        if (mode == 1) {
            printf(CYAN "cmd> " RESET "%s", line);
            fflush(stdout);
        }
        int c = getchar();
        if (c == EOF) break;

        if (mode == 1) {
            if (c == 127 || c == 8) { if (linelen) line[--linelen] = 0; }
            else if (c == 27) { linelen = 0; line[0] = 0; mode = 0; }
            else if (c == '\n' || c == '\r') {
                line[linelen] = 0;
                char tmp[256];
                snprintf(tmp, sizeof tmp, "%s", line);
                linelen = 0; line[0] = 0; mode = 0;
                do_operand_line(tmp);
            } else if (isprint(c) && linelen + 1 < sizeof line) {
                line[linelen++] = (char)c; line[linelen] = 0;
            }
            continue;
        }

        switch (c) {
            case 'q': raw_off(); clear_screen(); return 0;
            case 'h': case '?': help(); break;
            case '\n': case '\r':
                if (g_nmatch) select_form(g_sel);
                break;
            case 'j': if (g_sel + 1 < g_nmatch) select_form(g_sel + 1); break;
            case 'k': if (g_sel > 0) select_form(g_sel - 1); break;
            case ' ': mode = 1; break;             /* space starts a command */
            case '<': case '>': {                  /* nudge the first operand */
                if (g_form && g_form->nvars) {
                    if (g_nops == 0) { g_ops[0] = 0; g_nops = 1; }
                    g_ops[0] += (c == '>' ? 1 : -1);
                    encode_current();
                }
                break;
            }
            case 27: {                              /* escape: arrows, or nothing */
                int a = getchar();
                if (a == '[') {
                    int b = getchar();
                    if (b == 'A' && g_sel > 0) select_form(g_sel - 1);
                    else if (b == 'B' && g_sel + 1 < g_nmatch) select_form(g_sel + 1);
                    else if (b == 'C' || b == 'D') { /* left/right: ignore */ }
                } else if (a == 27) {
                    /* ESC ESC: leave command mode */
                    mode = 0;
                }
                /* a bare ESC is swallowed, not re-interpreted as the next key */
                break;
            }
            case '/': mode = 1; break;
            default:
                if (!*g_search && g_form && (isdigit(c) || c == '-')) {
                    /* a form is loaded and the search box is empty: the natural thing to
                     * type next is an operand, so start the command line with this digit
                     * rather than filtering the list with it */
                    mode = 1;
                    line[0] = (char)c; line[1] = 0; linelen = 1;
                } else if (isprint(c)) search_key(c);
                break;
        }
    }
    raw_off();
    clear_screen();
    return 0;
}
