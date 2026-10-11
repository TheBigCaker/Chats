/* x86verify.c -- verify the x86-64 catalog against llvm-mc, in C, with no shell parsing.
 *
 * WHY THIS EXISTS. The shell version parsed tool output with awk and got the field numbers
 * wrong three separate times -- reading the byte column instead of the row name, missing the
 * second output line, splitting on a separator that moves with instruction length. Each bug
 * looked like a catalog failure. A verifier that mis-reads its own input is worse than no
 * verifier: it sends you to fix rows that were already right.
 *
 * So this does the whole loop in one process: assemble with llvm-mc, pull the code bytes out of
 * the object, hand them to the catalog's isa_decode, re-encode with isa_encode, and compare.
 * Nothing is parsed from text except the instruction bytes themselves, which come from the
 * object's section table by offset.
 *
 *   x86verify <elf-with-instructions> ...
 *   x86verify                       -- run the built-in form list
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "isadb.h"
#include "arbishim.h"
#include "ISA_X86_64.inc"

static const IsaTable T = { isa_forms_x86_64, ISA_NFORMS_X86_64, "x86_64", 1, 1 };

/* assemble one line and return the code bytes; -1 on failure */
static int assemble(const char *line, uint8_t *out, int outmax) {
    char base[128], src[144], obj[160], cmd[512];
    snprintf(base, sizeof base, "%s", "/data/data/com.termux/files/home/binast-spike/tmp/xvXXXXXX");
    int fd = mkstemp(base);
    if (fd < 0) return -1;
    close(fd);
    snprintf(src, sizeof src, "%s.s", base);
    snprintf(obj, sizeof obj, "%s.o", base);
    FILE *f = fopen(src, "w");
    if (!f) return -1;
    fprintf(f, ".text\n%s\n", line);
    fclose(f);
    snprintf(cmd, sizeof cmd,
             "llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o %s %s 2>/dev/null", obj, src);
    if (system(cmd) != 0) { remove(src); remove(obj); return -1; }
    FILE *o = fopen(obj, "rb");
    if (!o) { remove(src); remove(obj); return -1; }
    fseek(o, 0, SEEK_END);
    long n = ftell(o);
    rewind(o);
    uint8_t *buf = (uint8_t *)arb_bytes((long)n);
    if (!buf || fread(buf, 1, (size_t)n, o) != (size_t)n) { fclose(o); remove(src); remove(obj); return -1; }
    fclose(o);
    int len = -1;
    if (n > 64 && !memcmp(buf, "\x7f""ELF", 4)) {
        uint64_t shoff = 0; for (int i = 7; i >= 0; i--) shoff = (shoff << 8) | buf[0x28 + i];
        uint16_t shent = (uint16_t)(buf[0x3a] | (buf[0x3b] << 8));
        uint16_t shn   = (uint16_t)(buf[0x3c] | (buf[0x3d] << 8));
        for (uint16_t i = 0; i < shn; i++) {
            uint64_t o2 = shoff + (uint64_t)i * shent;
            if (o2 + 64 > (uint64_t)n) break;
            uint64_t fl = 0; for (int k = 7; k >= 0; k--) fl = (fl << 8) | buf[o2 + 0x8 + k];
            uint64_t fo = 0; for (int k = 7; k >= 0; k--) fo = (fo << 8) | buf[o2 + 0x18 + k];
            uint64_t sz = 0; for (int k = 7; k >= 0; k--) sz = (sz << 8) | buf[o2 + 0x20 + k];
            if (!(fl & 0x4) || !sz || fo + sz > (uint64_t)n) continue;
            if ((int)sz > outmax) break;
            memcpy(out, buf + fo, (size_t)sz);
            len = (int)sz;
            break;
        }
    }
    arbit_release_carve((long)buf);
    remove(src); remove(obj);
    return len;
}

static void hex(const uint8_t *b, int n, char *out, size_t on) {
    size_t k = 0;
    for (int i = 0; i < n && k + 2 < on; i++) k += (size_t)snprintf(out + k, on - k, "%02x", b[i]);
}

/* the forms to check: every one an assembler accepts, exercising both register banks. */
static const char *FORMS[] = {
    "addq %rax, %rbx", "addq %r8, %r9", "addq %r15, %r14", "addq %rcx, %rdx",
    "subq %r10, %r11", "xorq %rax, %rax", "cmpq %r8, %rcx", "testq %rdx, %r9",
    "movq %rax, %rbx", "movq %r8, %r15", "andq %r12, %r13", "orq %rsi, %rdi",
    "adcq %rbx, %rbp", "sbbq %rsp, %r12",
    "addl %eax, %ecx", "addl %r8d, %r9d", "movl %eax, %ecx", "movl %r10d, %r11d",
    "cmovgq %rax, %rcx", "cmovlq %r8, %r9", "cmovneq %rdx, %rsi",
    "sete %al", "setg %cl", "setne %dl", "seto %bl",
    "movzbq %al, %rcx", "movzbq %r8b, %r9", "movswq %ax, %rdx", "movsbq %cl, %r10",
    "leaq (%rax), %rcx", "leaq (%r8), %r9", "leaq (%r15), %r14",
    "notq %rax", "negq %r8", "imulq %rcx", "idivq %rdx", "mulq %rsi", "divq %rdi",
    "movabsq $0x1122334455667788, %rax", "movabsq $0x1122334455667788, %r9",
    "shlq $3, %rax", "sarq $2, %r8", "sarq $12, %r9", "shrq $7, %rcx",
    "addq $17, %rax", "addq $17, %r9", "subq $5, %r8", "andq $255, %rcx",
    "xorq $1, %rax", "orq $16, %rdx", "cmpq $100, %rsi", "adcq $3, %rdi",
};
#define NFORMS ((int)(sizeof FORMS / sizeof FORMS[0]))

int main(void) {
    arbit_init(0);
    int pass = 0, miss = 0, wrong = 0, skip = 0;
    for (int i = 0; i < NFORMS; i++) {
        uint8_t bytes[16];
        int len = assemble(FORMS[i], bytes, sizeof bytes);
        if (len <= 0) { skip++; continue; }
        char want[40];
        hex(bytes, len, want, sizeof want);

        const IsaForm *f = NULL;
        long ops[ISA_MAXV] = {0};
        if (!isa_decode(&T, bytes, len, &f, ops)) {
            miss++;
            printf("MISS  %-28s %s\n", FORMS[i], want);
            continue;
        }
        uint8_t back[ISA_MAXBYTES];
        char err[200];
        if (isa_encode(f, ops, back, err, sizeof err) != 0) {
            wrong++;
            printf("ENC?  %-28s %s  row=%s err=%s\n", FORMS[i], want, f->name, err);
            continue;
        }
        char got[40];
        hex(back, len, got, sizeof got);
        if (strcmp(got, want)) {
            wrong++;
            printf("DIFF  %-28s want %s got %s  row=%s\n", FORMS[i], want, got, f->name);
            continue;
        }
        pass++;
    }
    printf("--- x86-64 vs llvm-mc: %d exact, %d missing, %d wrong, %d unassemblable\n",
           pass, miss, wrong, skip);
    return (miss || wrong) ? 1 : 0;
}
