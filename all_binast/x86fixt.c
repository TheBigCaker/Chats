/* x86fixt.c -- regenerate the x86-64 fixtures from llvm-mc, using the catalog's own row names.
 *
 * WHY THIS IS A GENERATOR AND NOT A HAND-WRITTEN FILE. The fixtures are the one check that does not
 * go through my own encoder: each line is a REAL encoding from an independent assembler (llvm-mc)
 * with the row name and the operand values the catalog needs to reproduce it. Written by hand they
 * go stale the moment a row is renamed -- which is exactly what happened when the catalog stopped
 * being 73 hand-written rows and became 18,774 derived ones: every fixture named a row that no
 * longer exists (`CMOVA_64_X0`, `LEA_64_X0`), so 116 of 116 "failed" while the catalog was in fact
 * correct on every one of those instructions.
 *
 * HOW IT WORKS. For each probe it
 *
 *   1. assembles the instruction with llvm-mc and reads the bytes back from the object,
 *   2. decodes those bytes with the catalog,
 *   3. requires the decode to consume exactly the instruction's length,
 *   4. emits a fixture line naming the row the catalog chose and the operands it extracted.
 *
 * A probe whose decode is wrong is REPORTED, not silently skipped -- a fixture file that quietly
 * omits what it could not do is worse than none, because it makes the pass rate look complete.
 *
 * Usage:  x86fixt > isadb_fixtures_x86.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arbishim.h"
#include "isadb.h"
#include "ISA_X86_64.inc"

static const IsaTable T = { isa_forms_x86_64, ISA_NFORMS_X86_64, "x86_64" };

/* one probe: the assembler text, and what kind of operands it has */
typedef struct {
    const char *asm_text;
    const char *kind;      /* rr, ri, rm, j, set, cmov -- only used for grouping in the report */
} Probe;

static const Probe PROBES[] = {
    /* register-to-register, both banks, both directions of the ModR/M */
    { "movq %rax, %rbx",       "rr" }, { "movq %r8, %r9",     "rr" },
    { "movl %esi, %ecx",       "rr" }, { "movl %r8d, %r9d",   "rr" },
    { "movw %ax, %bx",         "rr" }, { "movb %al, %bl",     "rr" },
    { "addq %r8, %r9",         "rr" }, { "addl %esi, %ecx",   "rr" },
    { "addw %ax, %bx",         "rr" }, { "addb %al, %bl",     "rr" },
    { "subq %rax, %rbx",       "rr" }, { "andq %rax, %rbx",   "rr" },
    { "orq %rax, %rbx",        "rr" }, { "xorq %rax, %rbx",   "rr" },
    { "xorl %eax, %eax",       "rr" }, { "cmpq %rdx, %rcx",   "rr" },
    { "testq %rdx, %rdx",      "rr" }, { "testl %esi, %esi",  "rr" },
    { "adcq %rax, %rbx",       "rr" }, { "sbbq %rax, %rbx",   "rr" },
    { "imulq %rbx, %rax",      "rr" }, { "imull %esi, %eax",  "rr" },
    /* immediate forms, every width */
    { "addq $0x7f, %rax",      "ri" }, { "addl $0x7f, %eax",  "ri" },
    { "addq $0x11223344, %rax","ri" }, { "cmpq $0xb, %rdi",   "ri" },
    { "subq $1, %rsp",         "ri" }, { "xorl $0x5a, %eax",  "ri" },
    { "movl $0x21, %ecx",      "ri" }, { "movq $0x1234, %rax", "ri" },
    { "movb $0x41, %al",       "ri" }, { "shlq $3, %rax",     "ri" },
    { "sarq $3, %rax",         "ri" }, { "shrq $1, %rdx",     "ri" },
    { "movabsq $0x1122334455667788, %r10", "ri" },
    { "movabsq $0x1122334455667788, %rax", "ri" },
    /* memory forms: every mod, with and without SIB, with and without disp */
    { "movq (%rax), %rdx",     "rm" }, { "movq 0x8(%rax), %rdx", "rm" },
    { "movq 0x11223344(%rax), %rdx", "rm" },
    { "movq (%rax,%rbx,4), %rdx", "rm" },
    { "movq 0x10(%rax,%rbx,8), %rdx", "rm" },
    { "movq (%rip), %rdx",     "rm" }, { "movq %rax, (%rbx)",  "rm" },
    { "leaq 0x1(%rdi), %rcx",  "rm" }, { "leaq (%rdi,%rdi,2), %rax", "rm" },
    { "leaq (%r8), %rdx",      "rm" }, { "leal (%rdi), %eax",  "rm" },
    { "movzbl (%rsi,%rax), %ecx", "rm" },
    { "movzbl 0x1(%rsi,%rax), %ecx", "rm" },
    { "movsbl (%rdi), %eax",   "rm" },
    { "cmpq $0xb, (%rdi)",     "rm" }, { "addl (%rdi,%rdx,4), %eax", "rm" },
    { "imull (%rdi,%rdx,4), %eax", "rm" },
    /* unary group forms */
    { "incq %rax",             "u" },  { "decq %rbx",        "u" },
    { "negq %rax",             "u" },  { "notq %rax",        "u" },
    { "incq (%rax)",           "u" },  { "decq (%rbx)",      "u" },
    /* control transfer, every displacement direction */
    { "retq",                  "j" },  { "jmp .+2",          "j" },
    { "je .+2",                "j" },  { "jne .-2",          "j" },
    { "jl .+2",                "j" },  { "jg .+2",           "j" },
    { "jle .+2",               "j" },  { "jae .+2",          "j" },
    { "setne %al",             "set" },{ "sete %al",         "set" },
    { "setg %dl",              "set" },{ "setl %al",         "set" },
    { "setae %al",             "set" },{ "sets %r8b",        "set" },
    { "cmovlq %rcx, %rax",     "cmov" },{ "cmovneq %rcx, %rax", "cmov" },
    { "cmovgq %r8, %rdx",      "cmov" },{ "cmovael %ecx, %eax", "cmov" },
    /* extended registers in every position, which is what the REX windows exist for */
    { "movq %r15, %r14",       "rex" },{ "movq %r10, %r11",   "rex" },
    { "addq %r13, %r12",       "rex" },{ "xorl %r8d, %r15d",  "rex" },
    { "leaq 0x1(%r12), %r13",  "rex" },{ "movq (%r13), %r12", "rex" },
    { "testl %r9d, %r9d",      "rex" },{ "movq %rax, %r12",   "rex" },
    { "movq %r12, %rax",       "rex" },
    { "nop",                   "misc" },{ "nopw (%rax,%rax)", "misc" },
    { "nopw %cs:(%rax,%rax)",  "misc" },
    /* SSE: the packed FP families clang emits routinely to zero and move vectors. These come from
     * the nested-defm path (`defm PS : ...` inside `multiclass sse12_fp_packed_logical`), so a
     * regression there is caught here rather than only on the -O2 corpus. */
    { "xorps %xmm0, %xmm0",    "sse" },{ "xorps %xmm1, %xmm2",  "sse" },
    { "andps %xmm3, %xmm4",    "sse" },{ "orps %xmm5, %xmm6",    "sse" },
    { "xorps (%rdi), %xmm0",   "sse" },{ "movaps %xmm0, %xmm1",  "sse" },
    { "movaps (%rdi), %xmm0",  "sse" },{ "movaps %xmm0, (%rdi)", "sse" },
    { "movups (%rdi), %xmm2",  "sse" },{ "movups %xmm2, (%rdi)", "sse" },
    { "addss %xmm1, %xmm0",    "sse" },{ "addsd %xmm1, %xmm0",   "sse" },
    { "mulss %xmm1, %xmm0",    "sse" },{ "sqrtss %xmm1, %xmm0",  "sse" },
    { "movss %xmm0, %xmm1",    "sse" },{ "movsd %xmm0, %xmm1",   "sse" },
    { "movd %eax, %xmm0",      "sse" },{ "movq %rax, %xmm0",     "sse" },
    { "paddb %xmm1, %xmm0",    "sse" },{ "pxor %xmm0, %xmm0",    "sse" },
    { "ptest %xmm1, %xmm0",    "sse" },
    /* the packed-shuffle family, whose defs are nested inside a `let ... in { }` wrapping the
     * multiclass -- the shape that made every one of them absent */
    { "pshufd $0xee, %xmm1, %xmm0", "sse" },
    { "pshufd $0x55, %xmm0, %xmm1", "sse" },
    { "pshufd $0x1b, (%rdi), %xmm2", "sse" },
    { "pshuflw $0x1b, %xmm2, %xmm3", "sse" },
    { "pshufhw $0x1b, %xmm2, %xmm3", "sse" },
    { "imulq $-0x6db6db6d, %rcx, %rax", "imul" },
    { "imull $-0x1234, %esi, %eax", "imul" },
    { "imulw $-0x12, %si, %ax", "imul" },
    { "leal (,%rax,8), %edx",   "sib" },
    { "leaq (,%rdi,4), %rax",   "sib" },
    { "leal (%rdi), %eax",      "sib" },
    /* the packed/logical shifts: the def lives in `PDI_binop_rmi_all` and the tie is a `let
     * Constraints` inside the multiclass body, so neither the operands nor the implicit destination
     * were visible without reading it */
    { "psllq $3, %xmm0",       "sse" },
    { "psrlq $3, %xmm0",       "sse" },
    { "psraw $3, %xmm0",       "sse" },
    { "psrld $3, %xmm0",       "sse" },
    { "psllw $3, %xmm0",       "sse" },
    { "pslld $3, %xmm0",       "sse" },
    { "psrlw $3, %xmm0",       "sse" },
    { "psrad $3, %xmm0",       "sse" },
    { "psrlw %xmm1, %xmm0",    "sse" },
    { "pslld %xmm1, %xmm0",    "sse" },
    { "psrad %xmm1, %xmm0",    "sse" },
    { "psllq %xmm1, %xmm0",    "sse" },
    { "psraw %xmm1, %xmm0",    "sse" },
    { "psrld %xmm1, %xmm0",    "sse" },
    { "pslldq $4, %xmm0",      "sse" },
    { "psrldq $4, %xmm0",      "sse" },
    /* the scalar float forms: MULTICLASS PREFIX AND ESCAPE BOTH LIVE ON THE OUTER defm
     * (`defm MOVSD : sse12_move<...>, TB, XD;`), and `defm SQRT` instantiates FOUR multiclasses at
     * once -- the shapes that left this whole family with one-byte opcodes or no row at all */
    { "movsd %xmm1, %xmm0",    "sse" },
    { "movsd %xmm1, (%rdi)",   "sse" },
    { "movsd (%rdi), %xmm0",   "sse" },
    { "movss %xmm1, %xmm0",    "sse" },
    { "movss (%rdi), %xmm0",   "sse" },
    { "sqrtsd %xmm1, %xmm0",   "sse" },
    { "sqrtss %xmm1, %xmm0",   "sse" },
    { "sqrtsd (%rdi), %xmm0",  "sse" },
    { "addsd %xmm1, %xmm0",    "sse" },
    { "subsd %xmm1, %xmm0",    "sse" },
    { "mulsd %xmm1, %xmm0",    "sse" },
    { "divsd %xmm1, %xmm0",    "sse" },
    { "addss %xmm1, %xmm0",    "sse" },
    { "comisd %xmm1, %xmm0",   "sse" },
    { "ucomisd %xmm1, %xmm0",  "sse" },
    { "cvtsi2sdl %edi, %xmm0", "sse" },
    { "cvttsd2si %xmm0, %edi", "sse" },
    { "cvtsd2ss %xmm0, %xmm0", "sse" },
    { "cvtss2sd %xmm0, %xmm0", "sse" },
    { "unpcklps %xmm1, %xmm0", "sse" },
    { "shufps $0x1b, %xmm1, %xmm0", "sse" },
    { "pinsrd $1, %edi, %xmm0", "sse" },
    { "pextrd $1, %xmm0, %edi", "sse" },
    /* THE LONG-MODE DEFAULT FAMILIES AND THE SIGN-EXTEND IDIOMS. `jmp *%rax` is `ff e0` -- no REX.W,
     * the 64-bit operand size being the default -- and `cltq` is `48 98`, which is REX.W from the `RI`
     * class rather than from the def name. */
    { "jmpq *%rax",            "ctl" },
    { "jmpq *(%rdi)",          "ctl" },
    { "callq *%rax",           "ctl" },
    { "pushq %rax",            "ctl" },
    { "popq %rax",             "ctl" },
    { "cltq",                  "ctl" },
    { "cqto",                  "ctl" },
    { "cltd",                  "ctl" },
    { "cwtd",                  "ctl" },
};

/* read one instruction's bytes out of an llvm-mc object, and its length from a delta listing */
static int assemble_one(const char *text, uint8_t *out, int *olen) {
    /* THE SOURCE IS WRITTEN WITH fopen, NOT THROUGH A SHELL printf.
     * AT&T operands are full of `%` (`%rax`, `%cs:(%rax,%rax)`) and the shell's printf treats `%r`
     * as a directive: "sh: printf: %r: invalid directive" and the instruction never reached the
     * assembler, so 87 of 89 probes reported no encoding. A file handle has no format string. */
    FILE *src = fopen("tmp/fx.s", "w");
    if (!src) return 0;
    fprintf(src, "\t.text\n\t%s\n", text);
    fclose(src);

    /* -show-encoding prints the bytes; reading them from the listing is what makes the fixture an
     * INDEPENDENT encoding rather than my own encoder's output.
     *
     * A BRANCH COMES BACK AS A FIXUP, NOT AS BYTES. `jmp .+2` lists `[0xeb,A]` with
     * `fixup A - offset: 1, value: .Ltmp0+2, kind: FK_PCRel_1` -- the displacement is a symbol
     * reference the assembler leaves for the linker. The value is still fully determined though, and
     * resolving it here keeps the ground truth coming from llvm-mc instead of from an assumption
     * about branch encoding: for a field at `offset` of width `w`, A = value - (offset + w). */
    FILE *p = popen("llvm-mc -triple=x86_64-unknown-linux-gnu -show-encoding tmp/fx.s 2>/dev/null",
                    "r");
    if (!p) return 0;
    char line[512];
    int got = 0;
    long fixoff = -1, fixval = 0;
    int fixw = 0;
    while (fgets(line, sizeof line, p)) {
        if (!got) {
            /* the first line carrying an encoding is this instruction's */
            const char *e = strstr(line, "encoding: [");
            if (e) {
                const char *q = e;
                while (*q && got < *olen) {
                    if (q[0] == '0' && q[1] == 'x') {
                        out[got++] = (uint8_t)strtol(q + 2, NULL, 16);
                        q += 4;
                    } else if ((*q >= 'A' && *q <= 'Z') && strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ", *q)) {
                        /* a fixup placeholder: the byte is unresolved, remember its slot */
                        fixoff = got;
                        out[got++] = 0;
                        q++;
                    } else q++;
                }
            }
            continue;
        }
        /* the fixup lines follow, on '#' comment lines */
        const char *fo = strstr(line, "fixup");
        const char *of = fo ? strstr(line, "offset:") : NULL;
        const char *va = fo ? strstr(line, "value:") : NULL;
        const char *ki = fo ? strstr(line, "kind:") : NULL;
        if (of && va && ki) {
            fixoff = strtol(of + 7, NULL, 10);
            /* `value: .Ltmp0+2` -- take the byte position of the label plus its addend */
            const char *plus = strchr(va + 6, '+');
            const char *minus = strchr(va + 6, '-');
            fixval = 0;
            if (plus) fixval = strtol(plus + 1, NULL, 10);
            else if (minus) fixval = -strtol(minus + 1, NULL, 10);
            fixw = 0;
            if (strstr(ki, "PCRel_1")) fixw = 1;
            else if (strstr(ki, "PCRel_2")) fixw = 2;
            else if (strstr(ki, "PCRel_4")) fixw = 4;
            /* A = target - (field_offset + field_width), which is the definition of a PC-relative
             * displacement and not something to take on faith */
            if (fixw && fixoff >= 0 && fixoff < got) {
                long rel = fixval - (fixoff + fixw);
                for (int k = 0; k < fixw && fixoff + k < got; k++)
                    out[fixoff + k] = (uint8_t)((rel >> (8 * k)) & 0xff);
            }
        }
        if (*line != '#' && !strchr(line, '#')) break;
    }
    pclose(p);
    *olen = got;
    return got > 0;
}

int main(void) {
    printf("/* isadb_fixtures_x86.h -- GENERATED by `x86fixt` from llvm-mc.\n"
           " *\n"
           " * Each line is a REAL encoding produced by an independent assembler, with the row the\n"
           " * catalog chooses for it and the operand values it extracts. The check is therefore\n"
           " * end-to-end: bytes from llvm-mc in, the catalog's own decode out, and the fixture\n"
           " * requires the same row and operands to reproduce those bytes.\n"
           " *\n"
           " * Regenerate with `make fixtures`. A probe that does not decode is REPORTED on stderr\n"
           " * rather than omitted, because a fixture file that silently drops its hard cases\n"
           " * reports a pass rate that means nothing.\n"
           " */\n"
           "#ifndef ISADB_FIXTURES_X86_H\n#define ISADB_FIXTURES_X86_H\n\n"
           "static const struct { const char *form; long ops[ISA_MAXV]; const char *bytes; int plen; } "
           "isa_x86_fixtures[] = {\n");

    int n = 0, bad = 0;
    for (size_t i = 0; i < sizeof PROBES / sizeof PROBES[0]; i++) {
        uint8_t b[16];
        int blen = 16;
        if (!assemble_one(PROBES[i].asm_text, b, &blen)) {
            fprintf(stderr, "  !! llvm-mc produced nothing for `%s`\n", PROBES[i].asm_text);
            bad++;
            continue;
        }
        const IsaForm *f = NULL;
        long h[ISA_MAXV] = {0};
        int got = isa_decode(&T, b, blen, &f, h);
        if (!f || got != blen) {
            fprintf(stderr, "  !! `%s` -> %d bytes decoded of %d (%s)\n",
                    PROBES[i].asm_text, got, blen, f ? f->name : "no row");
            bad++;
            continue;
        }
        char hex[64]; int hl = 0;
        for (int k = 0; k < blen; k++) hl += snprintf(hex + hl, sizeof hex - (size_t)hl, "%02x", b[k]);
        /* A LEADING LEGACY PREFIX RUN IS RECORDED SEPARATELY, BECAUSE IT IS NOT PART OF THE ROW.
         *
         * `nopw %cs:(%rax,%rax)` is `2e 66 0f 1f 04 00`: a CS override and an operand-size prefix
         * before the opcode. Prefixes are a separate axis from the ModR/M-and-opcode row -- the SAME
         * row is reached with zero, one or six of them (clang emits `66 66 66 66 66 66 2e 0f 1f ...`
         * for a 16-byte-aligned nop), so a row cannot carry them. isa_decode strips the run and
         * returns len + plen, which means a byte-exact reconstructor already has what it needs: the
         * run it stripped, plus the row's own bytes. The fixture states both, so the check verifies
         * the real contract instead of a property the catalog never claimed -- and it does not need
         * the prefix case DELETED to reach a clean number, which is the whole point of listing it. */
        printf("  { \"%s\", {", f->name);
        for (int v = 0; v < ISA_MAXV; v++) printf(" %ld%s", h[v], v + 1 < ISA_MAXV ? "," : "");
        printf(" }, \"%s\", %d },\n", hex, blen - f->len);
        n++;
    }
    printf("};\n#define ISA_NFIXTURES_X86 %d\n\n#endif\n", n);
    fprintf(stderr, "x86fixt: %d fixtures, %d probes refused\n", n, bad);
    return bad ? 1 : 0;
}
