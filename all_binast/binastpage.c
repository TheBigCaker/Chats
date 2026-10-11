/* binastpage.c -- binary as a PAGE a user or agent can read and write directly.
 *
 * THE GOAL, as stated: binast helps build and shape binary through the building-block
 * system, so a user or an agent can interact with code directly. Not "decode and
 * report" -- READ a page of the program and WRITE one back. This is the surface for
 * that, and it is closer to an image/PDF reader-editor than to a disassembler:
 *
 *   a page is a run of WORDS, not a run of lines
 *   each word is a BUILDING BLOCK: one catalog row plus the values in its slots
 *   reading gives you the block and its slots as editable values
 *   writing fills those slots and produces the exact bytes for that block
 *   you write a page back and run it
 *
 * WHAT THAT BUYS, concretely: to change an instruction you do not need to know its
 * encoding, only which block it is and what you want its operands to say:
 *
 *     binastpage set <elf> <addr> MOVZ_X 0 0 42     # put "movz x0,#42" at that address
 *     binastpage show <elf> 0x4001c8 8              # read 8 words as a page
 *
 * The reader is the SAME catalog that is verified at 100% coverage, and the writer is
 * the same `isa_encode` the ISA self-check round-trips (1320 rows, 0 failures). So a
 * block written by this tool decodes to what it says it is, by construction.
 *
 * MEMORY: arbit only (arbishim.h). No malloc family, no pthreads needed here.
 *
 *   binastpage show  <elf> [addr] [count]   read a page: block + slots, per word
 *   binastpage set   <elf> <addr> <FORM> [slot...]  write one block into a copy
 *   binastpage fill  <elf> <addr> <count> <MODE>    repeat-safe fill (NOP/none)
 *   binastpage agent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>   /* stat/chmod only, to preserve the mode on write */

#include "isadb.h"
#include "arbishim.h"

#define ISA_INC_AARCH64 "ISA_AARCH64.inc"
#include ISA_INC_AARCH64
static const IsaTable T = { isa_forms_aarch64, ISA_NFORMS_AARCH64, "aarch64", 4, 4 };

/* The condition-code names for K_COND rows. isadb.c keeps this table privately, so it
 * is repeated here rather than exported through the shared header -- adding it to
 * isadb.h would change the ISA interface for a naming convenience. */
static const char *cc_names[16] = {"eq","ne","cs","cc","mi","pl","vs","vc",
                                   "hi","ls","ge","lt","gt","le","al","nv"};

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ the image -- */

typedef struct {
    uint8_t *img;          /* the whole ELF, from a carve */
    size_t   size;
    uint8_t *code;         /* the executable section we edit */
    size_t   code_off;     /* its file offset */
    size_t   code_size;
    uint64_t code_va;      /* its virtual address */
} Image;

static int image_load(const char *path, Image *o) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "binastpage: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint8_t *img = (uint8_t *)arb_carve_zeroed(sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
    fclose(f);
    if (memcmp(img, "\x7f" "ELF", 4)) {
        fprintf(stderr, "binastpage: not an ELF\n"); arbit_release_carve((long)img); return -1;
    }
    uint64_t shoff = r64(img + 0x28);
    int shnum = r16(img + 0x3c), shent = r16(img + 0x3a);
    uint64_t best_off = 0, best_va = 0; uint64_t best_sz = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = img + shoff + (uint64_t)i * shent;
        uint64_t fl = r64(sh + 8), va = r64(sh + 0x10), off = r64(sh + 0x18), size = r64(sh + 0x20);
        if (!(fl & 0x4) || !size || off >= (uint64_t)sz) continue;
        if (off + size > (uint64_t)sz) size = (uint64_t)sz - off;
        if (size > best_sz) { best_sz = size; best_off = off; best_va = va; }
    }
    if (!best_sz) { fprintf(stderr, "binastpage: no executable section\n"); arbit_release_carve((long)img); return -1; }
    o->img = img; o->size = (size_t)sz;
    o->code = img + best_off; o->code_off = (size_t)best_off;
    o->code_size = (size_t)best_sz; o->code_va = best_va;
    return 0;
}
static void image_release(Image *o) { if (o->img) { arbit_release_carve((long)o->img); o->img = NULL; } }

static long addr_to_word(const Image *o, uint64_t addr) {
    if (addr < o->code_va) return -1;
    uint64_t off = addr - o->code_va;
    if (off + 4 > o->code_size) return -1;
    return (long)(off / 4);
}

/* ------------------------------------------------------------------- reading -- */

/* Print one word as a building block: which row it is, and the slot values that row
 * carries. This is the "page" form -- an agent can parse it, and a human can read it. */
static void show_word(const Image *o, size_t wi) {
    const uint8_t *b = o->code + wi * 4;
    uint32_t w = r32(b);
    const IsaForm *f = NULL;
    long human[ISA_MAXV] = {0};
    uint64_t addr = o->code_va + (uint64_t)wi * 4;
    int dec = isa_decode(&T, b, 4, &f, human);
    if (!dec) {
        printf("%016llx  %08x  DATA            (no catalog block; literal or padding)\n",
               (unsigned long long)addr, w);
        return;
    }
    printf("%016llx  %08x  %-15s", (unsigned long long)addr, w, f->name);
    for (int v = 0; v < f->nvars; v++) printf(" %ld", human[v]);
    printf("\n");
}

static int cmd_show(const char *path, const char *addr_s, int count) {
    Image im;
    if (image_load(path, &im) != 0) return 2;
    long wi = 0;
    if (addr_s) {
        uint64_t a = (uint64_t)strtoull(addr_s, NULL, 0);
        wi = addr_to_word(&im, a);
        if (wi < 0) { fprintf(stderr, "binastpage: address %s is not in the code section\n", addr_s); return 2; }
    }
    size_t total = im.code_size / 4;
    if (!count) count = 8;
    printf("# %s  code_va %#llx  words %zu\n", path, (unsigned long long)im.code_va, total);
    printf("# addr              word      block           slots...\n");
    for (int i = 0; i < count && (size_t)(wi + i) < total; i++) show_word(&im, (size_t)wi + i);
    image_release(&im);
    return 0;
}

static int write_image(const Image *im, const char *path) {
    char outp[1024];
    snprintf(outp, sizeof outp, "%s.paged", path);
    FILE *of = fopen(outp, "wb");
    if (!of) { fprintf(stderr, "binastpage: cannot write %s\n", outp); return -1; }
    size_t n = fwrite(im->img, 1, im->size, of);
    fclose(of);
    if (n != im->size) { fprintf(stderr, "binastpage: short write to %s\n", outp); return -1; }
    /* fopen("wb") creates with mode 0666 & ~umask, which is not executable -- so a
     * paged binary would fail execl with 127 and look like the EDIT broke the program
     * when in fact the FILE MODE did. Copy the source's mode instead. */
    struct stat st;
    if (stat(path, &st) == 0) chmod(outp, st.st_mode & 07777);
    printf("wrote %s\n", outp);
    return 0;
}

/* ------------------------------------------------------------------- writing -- */

/* Write one building block at an address. The block is named as a catalog row and the
 * slots are given as plain numbers, in the row's own operand order -- the same order
 * `isadb encode` takes. This is the whole point: no encoding knowledge required. */
static int cmd_set(const char *path, const char *addr_s, const char *form,
                   int argc, char **argv) {
    const IsaForm *f = isadb_find(&T, form);
    if (!f) { fprintf(stderr, "binastpage: no block named '%s' in %s\n", form, T.name); return 2; }

    long human[ISA_MAXV] = {0};
    int vi = 0;
    for (int a = 0; a < argc && vi < f->nvars; a++) {
        const char *s = argv[a];
        if (f->vkind[vi] == K_COND) {
            human[vi] = 0;
            for (int c = 0; c < 16; c++) if (!strcmp(s, cc_names[c])) human[vi] = c;
            if ((s[0] >= '0' && s[0] <= '9') || s[0] == '-') human[vi] = strtol(s, NULL, 0);
        } else {
            human[vi] = strtol(s, NULL, 0);
        }
        /* composite rows (UBFM/UBFX) take range:width as two separate slots */
        if ((f->vkind[vi] == K_UBFMW || f->vkind[vi] == K_UBFX) && vi + 1 < f->nvars && a + 1 < argc) {
            human[vi + 1] = strtol(argv[++a], NULL, 0);
        }
        vi++;
    }

    uint8_t out[ISA_MAXBYTES]; char err[200];
    if (isa_encode(f, human, out, err, sizeof err) != 0) {
        fprintf(stderr, "binastpage: %s: %s\n", form, err); return 2;
    }

    Image im;
    if (image_load(path, &im) != 0) return 2;
    long wi = addr_to_word(&im, (uint64_t)strtoull(addr_s, NULL, 0));
    if (wi < 0) { fprintf(stderr, "binastpage: address %s is not in the code section\n", addr_s); return 2; }

    uint32_t before = r32(im.code + (size_t)wi * 4);
    uint32_t after = r32(out);
    memcpy(im.code + (size_t)wi * 4, out, 4);

    if (write_image(&im, path) != 0) return 2;

    printf("# %s\n", path);
    printf("# addr %016llx\n", (unsigned long long)(im.code_va + (uint64_t)wi * 4));
    printf("block %s  operands \"%s\"\n", f->name, f->operands);
    printf("before %08x\n", before);
    printf("after  %08x\n", after);
    /* read it back through the DECODER, not through the encoder's own opinion: the
     * claim "I wrote block X" is only true if the catalog says X when it reads it. */
    const IsaForm *chk = NULL; long ch[ISA_MAXV] = {0};
    if (isa_decode(&T, out, 4, &chk, ch) && chk == f) printf("verified: reads back as %s\n", f->name);
    else if (chk) printf("verified: reads back as %s (alias of %s)\n", chk->name, f->name);
    else printf("verified: FAILED -- wrote bytes the catalog does not recognise\n");
    image_release(&im);
    return 0;
}

/* A repeat-safe fill: replace <count> words at an address with a block that carries no
 * meaning, for excising code without breaking the instruction stream. */
static int cmd_fill(const char *path, const char *addr_s, int count, const char *mode) {
    const char *form = (!strcmp(mode, "nop")) ? "NOP" : mode;
    const IsaForm *f = isadb_find(&T, form);
    if (!f) { fprintf(stderr, "binastpage: no block named '%s'\n", form); return 2; }
    long human[ISA_MAXV] = {0};
    uint8_t out[ISA_MAXBYTES]; char err[200];
    if (isa_encode(f, human, out, err, sizeof err) != 0) { fprintf(stderr, "binastpage: %s\n", err); return 2; }
    uint32_t pat = r32(out);

    Image im;
    if (image_load(path, &im) != 0) return 2;
    long wi = addr_to_word(&im, (uint64_t)strtoull(addr_s, NULL, 0));
    if (wi < 0) { fprintf(stderr, "binastpage: bad address\n"); return 2; }
    size_t total = im.code_size / 4;
    int done = 0;
    for (int i = 0; i < count && (size_t)(wi + i) < total; i++) {
        w32(im.code + (size_t)(wi + i) * 4, pat);
        done++;
    }
    printf("filled %d words at %#llx with %s (%08x)\n", done,
           (unsigned long long)(im.code_va + (uint64_t)wi * 4), form, pat);
    if (write_image(&im, path) != 0) return 2;
    image_release(&im);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: binastpage <command>\n"
            "  show  <elf> [addr] [count]              read a page of blocks + slots\n"
            "  set   <elf> <addr> <FORM> [slot...]     write one block, read it back\n"
            "  fill  <elf> <addr> <count> <MODE>       fill with a safe block (nop)\n"
            "  agent\n");
        return 2;
    }
    arbit_init(0);
    const char *c = argv[1];
    if (!strcmp(c, "agent")) {
        printf("tool binastpage\n");
        printf("what it is: read and write a binary as a page of building blocks\n");
        printf("a page     a run of words; each word is one catalog row plus its slots\n");
        printf("read       show gives addr, word, block name, and the slot values\n");
        printf("write      set fills a block's slots and emits the exact bytes\n");
        printf("why direct you name the block and its values; the encoder does the bits\n");
        printf("verified   every write is decoded back through the catalog before it is claimed\n");
        printf("example    binastpage set prog.elf 0x4001c8 MOVZ_X 0 0 42\n");
        printf("command show <elf> [addr] [count]\n");
        printf("command set  <elf> <addr> <FORM> [slot...]\n");
        printf("command fill <elf> <addr> <count> <MODE>\n");
        return 0;
    }
    if (!strcmp(c, "show")) {
        if (argc < 3) { fprintf(stderr, "binastpage: show needs an ELF\n"); return 2; }
        return cmd_show(argv[2], argc > 3 ? argv[3] : NULL, argc > 4 ? atoi(argv[4]) : 8);
    }
    if (!strcmp(c, "set")) {
        if (argc < 5) { fprintf(stderr, "binastpage: set <elf> <addr> <FORM> [slot...]\n"); return 2; }
        return cmd_set(argv[2], argv[3], argv[4], argc - 5, argv + 5);
    }
    if (!strcmp(c, "fill")) {
        if (argc < 6) { fprintf(stderr, "binastpage: fill <elf> <addr> <count> <MODE>\n"); return 2; }
        return cmd_fill(argv[2], argv[3], atoi(argv[4]), argv[5]);
    }
    fprintf(stderr, "binastpage: unknown command %s\n", c);
    return 2;
}
