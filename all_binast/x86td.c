/* x86td.c -- build the x86-64 catalog FROM THE LLVM TABLES.
 *
 * WHY. The previous generator rediscovered x86 encodings by assembling sampled instructions with
 * llvm-mc and solving for field positions. That is a measurement of something already written
 * down: the LLVM isa tables under ~/ISA_MAPS state every encoding directly, as
 *
 *     def MOV64rr : RI<0x89, MRMDestReg, (outs GR64:$dst), (ins GR64:$src), "mov{q}...">;
 *
 * with the OPCODE byte literal, the FIELD LAYOUT named by the Format, and the operands typed.
 * Sampling could only ever recover what this file says, one process per guess. This reads it.
 *
 * WHAT IT DERIVES. Three independent things, all stated in the table:
 *
 *   opcode byte        from the class's first template argument:   I<0x89, ...>
 *   prefix             from the class NAME: RI = REX.W, I = none, Ii8/16/32/64 = none with an
 *                      immediate, Ii8PCRel/Ii32PCRel = a relative branch. OpSize16 adds 0x66 and
 *                      OpSize32 adds nothing (32-bit is the default in 64-bit mode).
 *   ModR/M layout      from the Format: MRMDestReg puts the destination in reg, MRMSrcReg the
 *                      source, MRMSrcMem means rm is memory, MRM0r..MRM7r pin the reg field to a
 *                      group index, AddRegFrm packs the register into the opcode, RawFrm has no
 *                      ModR/M at all.
 *
 * WHAT IT DOES NOT DO. It does not guess a row it cannot justify. A def whose class it does not
 * know, or whose format it does not know, is REPORTED BY NAME and skipped -- never emitted
 * half-formed. That is the one rule that makes the output trustworthy.
 *
 * VECTORS AND SYSTEM. SSE/AVX/AVX512 are a separate catalog: a VEX/EVEX prefix is a different
 * encoding machine (2- and 3-byte prefixes with their own field layout), and mixing them into the
 * integer rows would make both wrong. This file reads the integer files only, and names what it
 * left out so the boundary is explicit rather than silent.
 *
 * Usage:
 *     x86td emit   > ISA_X86_64.inc      regenerate the catalog
 *     x86td report                       what was parsed, derived, and refused
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arbishim.h"

/* ============================================================ the table ===== */

/* The integer instruction files. Anything not listed is out of scope BY NAME. */
static const char *TDIR = "/data/data/com.termux/files/home/ISA_MAPS/llvm_isa_tables/X86/";
static const char *TFILES[] = {
    "X86InstrArithmetic.td",   /* ALU, inc/dec, neg/not, mul/imul, div/idiv, test */
    "X86InstrMisc.td",         /* mov, lea, push/pop, xchg, nop, cmpxchg, bswap */
    "X86InstrControl.td",      /* jmp, jcc, call, ret, loop, syscall */
    "X86InstrShiftRotate.td",  /* shl/shr/sar/rol/ror/rcl/rcr, shld/shrd */
    "X86InstrExtension.td",    /* movzx, movsx, movsxd, cbw/cwde/cdqe */
    "X86InstrCMovSetCC.td",    /* setcc, cmovcc */
    "X86InstrSystem.td",       /* lgdt, lidt, in/out, cli/sti, cpuid, rdtsc */
    "X86InstrInfo.td",         /* the rest of the integer space */
    "X86InstrCompiler.td",     /* the compiler-only encodings that still carry an opcode */
    /* THE SCALAR AND PACKED FP FAMILIES. These are ordinary x86-64 instructions and clang emits
     * them by default at -O2 for any float or vector code, so leaving them out is a hole in the
     * catalog, not a scope decision: the same machinery covers them unchanged (`PSI` is a
     * Format + TB + a prefix, exactly like the integer classes). Their encodings come from the same
     * local .td map, so this costs one line and closes the gap. */
    "X86InstrSSE.td",          /* SSE1/SSE2/SSE3/SSSE3/SSE4: movaps, xorps, addps, ... */
    "X86InstrMMX.td",          /* MMX forms */
};
/* CLASS DEFINITIONS LIVE IN MORE FILES THAN THE DEFS DO. X86InstrFormats.td and
 * X86InstrUtils.td define Format, the encoding base classes and the multiclass families; without
 * them only 94 of the classes resolve, which is why 188 defs were refused as "unknown class" --
 * their class exists, it just had not been read. */
static const char *CFILES[] = {
    "X86InstrFormats.td", "X86InstrUtils.td", "X86InstrOperands.td",
    "X86InstrFragments.td", "X86InstrPredicates.td",
};
#define NCF (int)(sizeof CFILES / sizeof CFILES[0])
#define NTF (int)(sizeof TFILES / sizeof TFILES[0])

/* ---------------------------------------------------------------- formats --
 * Format gives the ModR/M layout. Values are the table's own Format<> numbers
 * (X86InstrFormats.td), because those are what the defs actually name.
 */
enum {
    F_RAW = 1,          /* RawFrm        -- no ModR/M; an immediate may follow   */
    F_ADDREG = 2,       /* AddRegFrm     -- register in the low 3 bits of the opcode */
    F_RAWMOFFS = 3,     /* RawFrmMemOffs -- moffs, an absolute address operand   */
    F_RAWSRC = 4,       /* RawFrmSrc     -- a register in the opcode, a source    */
    F_RAWDST = 5,       /* RawFrmDst     -- a register in the opcode, a destination */
    F_RAWDSTSRC = 6,    /* RawFrmDstSrc  -- two registers packed in the opcode    */
    F_RAW_IMM8 = 7,
    F_RAW_IMM16 = 8,
    F_ADDCC = 9,        /* AddCCFrm      -- condition code in the low 4 bits      */
    F_MRMDESTMEM = 24,  /* reg=source, rm=memory destination                      */
    F_MRMSRCMEM = 25,   /* reg=destination, rm=memory source                      */
    F_MRMDESTREG = 40,  /* reg=destination, rm=source register                    */
    F_MRMSRCREG = 41,   /* reg=source, rm=destination register                    */
    F_REGMEM0 = 32,     /* MRM0m .. MRM7m: reg field pinned to a group index      */
    F_REGREG0 = 48,     /* MRM0r .. MRM7r                                        */
    F_MRMX_M = 31,      /* MRMXm, reg field ignored                               */
    F_MRMX_R = 47,
};

/* ---------------------------------------------------------- encoding classes --
 * The class name says what prefix the opcode carries. Everything is expressed as flags so a def
 * can be judged by name alone, without a TableGen evaluator.
 */
enum {
    C_UNKNOWN = 0,
    C_NOREX    = 1u << 0,   /* plain opcode, no REX.W (8-bit forms)                 */
    C_REXW     = 1u << 1,   /* REX.W = 1: the 64-bit form                           */
    C_OPSIZE16 = 1u << 2,   /* a 0x66 prefix selects 16-bit operand size            */
    C_IMM8     = 1u << 3,   /* a byte immediate follows                             */
    C_IMM16    = 1u << 4,
    C_IMM32    = 1u << 5,
    C_IMM64    = 1u << 6,
    C_REL8     = 1u << 7,   /* a one-byte relative branch displacement              */
    C_REL32    = 1u << 8,
    /* THE MANDATORY PREFIX OF THE SSE FORMS. `PI`, `PSI`, `PDI`, `SSI`, `SDI` and their `*i8`
     * variants are the whole difference between the packed/scalar FP encodings:
     *
     *     PI  (SSEPackedInt)    -> a 0x66 prefix, REQUIRED
     *     PSI (SSEPackedSingle) -> none
     *     SDI (SSEPackedDouble) -> 0xF2, REQUIRED
     *     SSI (SSEScalarSingle) -> 0xF3, REQUIRED
     *
     * This is a different axis from C_OPSIZE16: that one is an OPTIONAL 0x66 that selects 16-bit
     * operand size, while these prefixes are part of the instruction's identity -- `movaps` with a
     * 0xF3 in front is `movss`, a different instruction. They are separate flags so a row can never
     * confuse "the prefix that makes this instruction" with "the prefix that changes its width". */
    C_PFX66    = 1u << 9,   /* a mandatory 0x66                                       */
    C_PFXF2    = 1u << 10,  /* a mandatory 0xF2                                       */
    C_PFXF3    = 1u << 11,  /* a mandatory 0xF3                                       */
};

/* ============================================================ operand kinds ===== */

enum {
    A_REG8 = 1, A_REG16, A_REG32, A_REG64,
    /* A VECTOR REGISTER: xmm/ymm/zmm. 16 of them, named by ModR/M.reg (3 bits) plus REX.R --
     * exactly the window A_REG64 uses, so the encoding path is shared and only the name differs.
     * Without this kind every packed and scalar FP form was refused: `MOVAPSrr` is
     *     def rr : PI<0x28, MRMSrcReg, (outs VR128:$dst), (ins VR128:$src), ...>
     * and VR128 fell through operand_kind to A_NONE, which the emitter treats as "an operand this
     * parser did not understand" and skips -- 12,512 of them, which is why the whole SSE family was
     * absent while the catalog claimed success. */
    A_XMM,
    A_IMM8, A_IMM16, A_IMM32, A_IMM64,
    A_REL8, A_REL32,
    A_MEM,          /* a memory operand: disp32(base) */
    A_MOFFS,        /* an absolute address */
    A_CCODE,        /* a condition code: the low 4 bits of the opcode */
    A_GROUP,        /* the /digit in the ModR/M reg field -- not an operand */
    A_SIBSCALE,     /* the SIB scale: the multiplier 1, 2, 4 or 8 */
    A_DISP,         /* the memory displacement: 1 or 4 bytes, signed */
    A_RIPBASE,      /* the RIP-relative base: rm=5 at mod=00, a literal, not a register */
    A_NONE,
};



/* ============================================================ the row ===== */

#define MAXOPS 6      /* the ISA_MAXV the runtime carries. A SIB memory row needs five operand slots --
                       * destination, the memory base, the SIB index, the SIB scale and the
                       * displacement -- and 4 silently dropped the displacement, which is why
                       * `movzbl 0x1(%rsi,%rax),%ecx` had no row. */
#define MAXB   16

typedef struct {
    const char *name;
    const char *mnem;
    int   ops[MAXOPS];      /* operand kinds, in Intel order */
    int   nops;
    int   imm_wid;          /* width of a trailing immediate, 0 if none */
    int   has_modrm;
    int   group;            /* /digit when the format pins the reg field, -1 otherwise */
    int   mrm_byte;         /* a NAMED MRM_<HEX> format's literal ModR/M byte (0xF9 for MRM_F9),
                             * or -1. Distinct from `group`: the name states the WHOLE byte, so
                             * masking it to three bits (`rdtscp` 0xF9 -> /1) is wrong. */
    int   reg_in_opcode;    /* AddRegFrm: the register sits in the opcode's low bits */
    long  opcode;
    unsigned cls;
    int   split_byte, split_bit, split_wid;   /* the K_SPLIT window: a 64-bit immediate's low half */
    int   fk;        /* the format enum, kept so the emitter needs no second lookup */
    int   opmap;     /* 0 = one-byte opcode, 1 = 0x0F escape, 2 = 0x0F 0x38, 3 = 0x0F 0x3A */
    int   pfx;       /* the OpPrefix attribute's value as the table states it: 0 = none,
                      * 1 = PD (0x66), 2 = XS (0xF3), 3 = XD (0xF2), 4 = PS (also none, but a
                      * marker that the PD/XS/XD forms of the same opcode exist). Read from the
                      * row's text rather than inferred from the class NAME, because PS and NoPrfx
                      * both mean "no byte" and a name-based guess cannot tell that apart from the
                      * 0x66 that `PI` really wants. */
    char  opty[120]; /* the IN operand's TableGen type, e.g. `lea64_8mem`. Needed because the
                      * ADDRESS SIZE decides the ModR/M layout: `lea64_8mem` means 16-bit
                      * addressing (0x67), whose layout has no SIB byte and no RIP-relative
                      * base -- a fact that is invisible in the operand KIND (it is A_MEM) and
                      * in the operand size (these forms are 32/64-bit). */
} Row;

/* ============================================================ parsing ===== */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0) { fclose(f); return NULL; }
    char *b = (char *)arb_carve_zeroed(n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    fclose(f);
    b[n] = 0;
    return b;
}

/* =================================================== TableGen parameter binding =====
 *
 * A def can pass its Format as a TEMPLATE ARGUMENT rather than naming it directly:
 *
 *     class ITy<bits<8> o, Format f, X86TypeInfo t, ...>
 *     def INC64r : IncOpR_RF<Xi64>;          // IncOpR_RF<...> forwards to ITy
 *
 * So matching on the class NAME is not enough -- the class has to be RESOLVED. This parses every
 * `class NAME<params>` in the tables, records which position holds the Format and which holds the
 * type info, then binds a def's arguments positionally, following inheritance until it lands on a
 * class whose Format is known. That covers every composite family in the integer files without a
 * growing list of special cases.
 */

typedef struct {
    char name[64];
    char pform[8][32];    /* parameter names that are a Format */
    char ptype[8][32];    /* parameter names that are an X86TypeInfo */
    char parg[192][32];   /* all parameter names, in order. A multiclass in the vector families
                           * takes well over a dozen parameters -- `PDI_binop_rmi` has 13 before its
                           * optional tail, and the 12-slot array silently dropped everything past the
                           * twelfth, so a parameter like `RC` could not be substituted by name and
                           * the operand parse produced nothing. The array is sized for the widest
                           * multiclass in the tables, not for the common case. */
    int  narg;
    int  fmt_pos;         /* index of the Format parameter, -1 if none */
    int  ty_pos;          /* index of the type-info parameter, -1 if none */
    char base[64];        /* the class this one derives from, or "" */
    char baseargs[600];   /* the arguments it passes to that base, as written */
    char oppart[400];     /* the (outs)/(ins) text found in this class's chain, or "" */
    char inher_tail[300]; /* everything on this class's inheritance line AFTER the base's
                           * argument list -- where the predicates live:
                           *   class IncDec_Alt<...> : UnaryOpR_RF<...>, Requires<[Not64BitMode]> */
    char bodylet[600];    /* the `let` assignments inside the class BODY, as written.
                           * A class can override its base's literals in its own body:
                           *
                           *   class IMulOpMI8_R<X86TypeInfo t, X86FoldableSchedWrite sched>
                           *     : BinOpMI8<"imul", binop_ndd_args, t, MRMSrcMem, (outs t.RegClass:$dst)> {
                           *     let Opcode = 0x6B;
                           *     let SchedRW = [sched.Folded];
                           *   }
                           *
                           * The inheritance line passes NO opcode, so the walk fell through to the
                           * base's literal (0x83, which is the ALU-immediate opcode) and
                           * `IMUL64rmi8` shipped as `48 83`. The real three-operand imul-to-memory is
                           * `48 6b 45 f0 03` and it had no row. Body attributes are read here and
                           * applied before anything from the chain. */
} ClassDef;

static ClassDef g_cls[512];
static int      g_ncls;

static void load_classes(void) {
    for (int fi = 0; fi < NTF + NCF; fi++) {
        char path[512];
        snprintf(path, sizeof path, "%s%s", TDIR,
                 fi < NTF ? TFILES[fi] : CFILES[fi - NTF]);
        char *txt = slurp(path);
        if (!txt) continue;
        char *p = txt;
        while ((p = strstr(p, "class ")) != NULL) {
            p += 6;
            char name[64]; int nl = 0;
            while (*p && *p != '<' && *p != ' ' && *p != '\n' && nl < 63) name[nl++] = *p++;
            name[nl] = 0;
            if (!nl || *p != '<') continue;
            char *lt = p;
            /* THE CLOSING ANGLE BRACKET MUST BE DEPTH-AWARE. `class X<bits<8> o, Format f>`
             * contains a `>` inside `bits<8>`, and taking the FIRST one truncated the parameter
             * list at "bits<8", so fmt_pos stayed -1, the inheritance line after it was never
             * read, and every composite family reported base="" -- which is why 188 defs stayed
             * unresolvable while the resolver looked correct. */
            char *gt = NULL;
            {
                int d = 0;
                /* THE PARAMETER LIST CAN SPAN MULTIPLE LINES. ArithBinOp_RF's does:
                 *     multiclass ArithBinOp_RF<bits<8> BaseOpc, bits<8> BaseOpc2, bits<8> BaseOpc4,
                 *                              string mnemonic, Format RegMRM, Format MemMRM,
                 *                              SDNode opnodeflag, SDNode opnode, ...>
                 * Breaking at the first newline found no closing '>' and the whole multiclass was
                 * dropped from the class table -- which is why `defm ADD : ArithBinOp_RF<...>` could
                 * not bind its 252 held defs and every ALU family stayed out of the catalog. The
                 * scan is bounded to a window instead of a line, and still stops at ';' or '{' so a
                 * malformed header cannot run away through the file. */
                for (char *z = lt; *z; z++) {
                    if (*z == '<') d++;
                    else if (*z == '>') { d--; if (d == 0) { gt = z; break; } }
                    else if (*z == ';' || *z == '{') break;
                    if (z - lt > 3000) break;
                }
            }
            if (!gt) continue;
            ClassDef *c = &g_cls[g_ncls < 511 ? g_ncls : 511];
            snprintf(c->name, sizeof c->name, "%s", name);
            c->narg = 0; c->fmt_pos = -1; c->ty_pos = -1;
            int nf = 0, nt = 0;
            /* the parameter list: split on commas at depth 0 */
            char pl[600]; int pln = 0;
            for (char *q = lt + 1; q < gt && pln < 599; q++) pl[pln++] = *q;
            pl[pln] = 0;
            char *save = pl, *tok;
            while ((tok = strsep(&save, ",")) != NULL) {
                while (*tok == ' ' || *tok == '\t') tok++;
                /* a parameter looks like "Format f" or "X86TypeInfo t" or "bits<8> o" */
                char ty[32] = "", nm[32] = "";
                if (sscanf(tok, "%31s %31s", ty, nm) == 2) {
                    if (strstr(ty, "Format") && c->fmt_pos < 0) c->fmt_pos = c->narg;
                    if (strstr(ty, "X86TypeInfo") && c->ty_pos < 0) c->ty_pos = c->narg;
                } else {
                    snprintf(nm, sizeof nm, "%s", tok);
                    while (*nm && (nm[strlen(nm)-1] == ' ' || nm[strlen(nm)-1] == '\t'))
                        nm[strlen(nm)-1] = 0;
                }
                if (c->narg < 192) snprintf(c->parg[c->narg], sizeof c->parg[0], "%s", nm);
                c->narg++;
                (void)nf; (void)nt;
            }
            /* THE INHERITANCE LINE. `class IncOpR_RF<X86TypeInfo t> : UnaryOpR_RF<0xFF, MRM0r, ...>`
             * hardcodes the opcode and format at the point of derivation -- that is where the
             * literal encoding lives for a family like inc/dec/neg/not, so it has to be recorded
             * or the whole family is unresolvable. */
            c->base[0] = 0; c->baseargs[0] = 0; c->inher_tail[0] = 0; c->bodylet[0] = 0;
            {
                /* START AFTER THE CLOSING ANGLE BRACKET. `gt` points AT the '>', so beginning the
                 * scan there left q2 sitting on it and the colon was never found -- which is why
                 * every composite class reported an empty base even after the newline fix. */
                char *q2 = gt + 1;
                /* THE INHERITANCE CLAUSE CAN START ON THE NEXT LINE:
                 *     class IncDec_Alt<bits<8> o, string m, X86TypeInfo t>
                 *       : UnaryOpR_RF<o, AddRegFrm, m, t, null_frag>, Requires<[Not64BitMode]>;
                 * Stopping at the newline missed it entirely, so every such class reported an
                 * empty base and its family stayed unresolvable. Whitespace (newlines included)
                 * is skipped before looking for the colon, and again after it. */
                int guard = 0;
                while (*q2 && guard++ < 8 && (*q2 == ' ' || *q2 == '\t' || *q2 == '\n' || *q2 == '\r'))
                    q2++;
                if (*q2 == ':') {
                    q2++;
                    guard = 0;
                    while (*q2 && guard++ < 8 && (*q2 == ' ' || *q2 == '\t' || *q2 == '\n' || *q2 == '\r'))
                        q2++;
                    /* an optional `public`/`private` qualifier */
                    if (!strncmp(q2, "public ", 7)) q2 += 7;
                    int bl = 0;
                    while (*q2 && *q2 != '<' && *q2 != '{' && *q2 != ';' && bl < 63)
                        c->base[bl++] = *q2++;
                    c->base[bl] = 0;
                    while (bl > 0 && (c->base[bl-1] == ' ' || c->base[bl-1] == '\t')) c->base[--bl] = 0;
                    if (*q2 == '<') {
                        int d2 = 1, al = 0; q2++;
                        while (*q2 && d2 > 0 && al < 599) {
                            if (*q2 == '<') d2++;
                            else if (*q2 == '>') { d2--; if (!d2) break; }
                            c->baseargs[al++] = *q2++;
                        }
                        c->baseargs[al] = 0;
                        /* THE REST OF THE INHERITANCE LINE: `, Requires<[Not64BitMode]>` and friends.
                         * The base's argument list ends at its '>', and the predicate that decides
                         * whether the form even exists in 64-bit mode sits after it -- so a filter
                         * that only looked at the def's own text or the base's arguments missed it,
                         * and a 32-bit-mode-only `inc`/`dec` shipped as a row whose opcode byte
                         * (0x48) IS the REX.W prefix. */
                        int tl2 = 0;
                        char *t9 = q2;   /* q2 sits just past the base's '>' */
                        while (*t9 && *t9 != ';' && *t9 != '{' && *t9 != '\n' && tl2 < 299)
                            c->inher_tail[tl2++] = *t9++;
                        c->inher_tail[tl2] = 0;
                    } else {
                        int tl3 = 0;
                        char *t8 = q2;
                        while (*t8 && *t8 != ';' && *t8 != '{' && *t8 != '\n' && tl3 < 299)
                            c->inher_tail[tl3++] = *t8++;
                        c->inher_tail[tl3] = 0;
                    }
                }
            }
            if (getenv("X86TD_BASE") && (!strcmp(name, "BinOpRR") || !strcmp(name, "BinOpRR_F")))
                fprintf(stderr, "  [base] %-12s base=|%s| baseargs=|%.100s|\n",
                        name, c->base, c->baseargs);
            /* THE CLASS BODY'S `let` ASSIGNMENTS. A class that overrides a literal its body rather
             * than on the inheritance line is the ONLY place that literal exists:
             *
             *   class IMulOpMI8_R<X86TypeInfo t, ...>
             *     : BinOpMI8<"imul", binop_ndd_args, t, MRMSrcMem, (outs t.RegClass:$dst)> {
             *     let Opcode = 0x6B;
             *   }
             *
             * Walking the chain found the base's 0x83 (the ALU-immediate opcode) and `IMUL64rmi8`
             * shipped as `48 83`, so the real `48 6b 45 f0 03` had no row. The body is read to its
             * matching close brace, with nested blocks skipped, and only the depth-0 `let ... =`
             * clauses are kept. */
            {
                const char *bopen = NULL;
                int d = 0;
                for (const char *z = gt + 1; *z; z++) {
                    if (*z == '{') { bopen = z; break; }
                    if (*z == ';') break;                 /* no body: a plain derived class */
                }
                if (bopen) {
                    int bl2 = 0;
                    for (const char *z = bopen + 1; *z && bl2 < 599; z++) {
                        if (*z == '{' || *z == '(' || *z == '[') d++;
                        else if (*z == '}' || *z == ')' || *z == ']') {
                            if (d == 0) break;            /* the class body closed */
                            d--;
                        } else if (*z == ';' && d == 0) {
                            /* one complete `let X = Y;` (or `def`/other) statement -- keep only lets */
                        }
                        if (d == 0) c->bodylet[bl2++] = *z;
                    }
                    c->bodylet[bl2] = 0;
                }
            }
            g_ncls++;
            p = gt;
        }
        arbit_release_carve((long)txt);
    }
}

/* ---- is the def's opcode actually a legacy prefix byte? ---- */
static int opcode_is_prefix(const char *cls, const char *defargs, const char *name) {
    (void)cls; (void)name;
    /* the first 0x.. literal in the def's arguments, or in the class when the def has none */
    for (const char *z = defargs; *z; z++) {
        if (z[0] == '0' && (z[1] == 'x' || z[1] == 'X')) {
            long v = strtol(z, NULL, 0);
            /* 66 (operand size), 67 (address size), F0 (lock), F2/F3 (rep), 2E/36/3E/26/64/65
             * (segment overrides) are prefixes, not opcodes: a def whose "opcode" is one of them is
             * relying on an escape this catalog does not model. */
            if (v == 0x66 || v == 0x67 || v == 0xF0 || v == 0xF2 || v == 0xF3
                || v == 0x2E || v == 0x36 || v == 0x3E || v == 0x26 || v == 0x64 || v == 0x65)
                return 1;
            return 0;
        }
    }
    return 0;
}

static ClassDef *find_class(const char *n) {
    for (int i = 0; i < g_ncls; i++) if (!strcmp(g_cls[i].name, n)) return &g_cls[i];
    return NULL;
}



static int format_kind(const char *f);

/* ================================================ resolving a def to its LITERAL opcode =====
 *
 * `def INC64r : IncOpR_RF<Xi64>` names no opcode. The literal is one level down, in the class's own
 * inheritance line:
 *
 *     class IncOpR_RF<X86TypeInfo t, bit ndd = 0> : UnaryOpR_RF<0xFF, MRM0r, "inc", t, null_frag, ndd>;
 *     class UnaryOpR_RF<bits<8> o, Format f, string m, X86TypeInfo t, ...>
 *
 * So resolution is a SUBSTITUTION WALK: bind the def's arguments to the class's parameters, then
 * rewrite the base class's arguments with those bindings, and repeat until an argument IS a
 * literal opcode and the next is a named Format. That is what TableGen itself does, restricted to
 * the two things this tool needs.
 *
 * Everything here is textual because TableGen's evaluator (!if, !eq, multiclass instantiation) is
 * a language of its own. A definition whose chain reaches an expression this cannot reduce is
 * refused by name -- never guessed.
 */

/* split `a, b, c` at depth 0 */
static int split_args(const char *in, char out[][64], int max) {
    int depth = 0, n = 0, cl = 0;
    char cur[64];
    for (const char *q = in; ; q++) {
        char ch = *q;
        if (ch == '<' || ch == '[' || ch == '(') depth++;
        else if (ch == '>' || ch == ']' || ch == ')') { if (depth == 0 && ch == '>') break; depth--; }
        if ((ch == ',' && depth == 0) || ch == 0 || (ch == '>' && depth == 0)) {
            while (cl > 0 && (cur[cl-1] == ' ' || cur[cl-1] == '\t'
                              || cur[cl-1] == '\n' || cur[cl-1] == '\r')) cur[--cl] = 0;
            cur[cl] = 0;
            /* STRIP LEADING WHITESPACE TOO. Arguments arrive as "0xFF, MRM0r, ..." so every
             * element after the first carried a leading space: sub[1] was " MRM0r", which is not
             * a name any lookup matches. The opcode was found (it is first, so unspaced) while the
             * format never resolved -- exactly the symptom seen, and the reason 184 defs stayed
             * refused with their class chain sitting right there in the table. */
            char *lead = cur;
            while (*lead == ' ' || *lead == '\t' || *lead == '\n' || *lead == '\r') lead++;
            if (*lead && n < max) snprintf(out[n], 64, "%s", lead);
            if (*lead) n++;
            cl = 0;
            if (ch == 0 || (ch == '>' && depth == 0)) break;
            continue;
        }
        if (cl < 63) cur[cl++] = ch;
    }
    return n;
}

/* ============================================== operand lists through the class chain =====
 *
 * AN OPERAND LIST CAN BE A CLASS PARAMETER. Every arithmetic family builds its operands one
 * level down and forwards them by name:
 *
 *     class BinOpRR_RF<o, m, t, node, ndd> : BinOpRR<o, m, ..., t, (outs t.RegClass:$dst), [...]>
 *     class BinOpRR<o, m, args, t, out, p> : ITy<o, MRMDestReg, t, out,
 *                                                 (ins t.RegClass:$src1, t.RegClass:$src2), ...>
 *
 * At the BinOpRR hop the destination list is the parameter `out`; the only place the full pair of
 * operand lists exists as text is ITy's argument list once `out` has been replaced by what
 * BinOpRR_RF passed for it. So deriving operands is the same substitution walk as the opcode, run
 * over the inheritance line instead of the encoding arguments -- and it ends when the substituted
 * text actually contains an (outs) or (ins).
 *
 * `t.RegClass` IS NOT A CLASS. It is a member of the def's X86TypeInfo argument, and the table
 * states the mapping one level further down:
 *
 *     def Xi8  : X86TypeInfo<i8, "b", GR8, loadi8, i8mem, Imm8, i8imm, ...>
 *     def Xi64 : X86TypeInfo<i64, "q", GR64, loadi64, i64mem, Imm32S, i64i32imm, ...>
 *
 * so Xi64.RegClass is GR64 and Xi64.MemOperand is i64mem. Resolving those turns the forwarded
 * parameter into the same GRn / iNm e m spelling the direct defs use, and from there parse_operands
 * needs no new cases. That is the whole reason every ALU family was missing: their operand text
 * says `t.RegClass`, which operand_kind refuses by design, so the rows were never emitted.
 */
typedef struct {
    char name[32];
    char regclass[32];
    char memop[32];
    char immop[32];         /* Xi64.ImmOperand is i64i32imm: the immediate's own class */
    char imm8op[32];        /* Xi64.Imm8Operand is i64i8imm: the sign-extended byte form */
    int  has_even_opcode;   /* Xi8 sets this: the opcode's low bit is forced to 0 */
    int  has_rexw;
} TypeInfo;
static TypeInfo g_ti[64];
static int      g_nti;

static void load_typeinfos(void) {
    for (int fi = 0; fi < NTF + NCF; fi++) {
        char path[512];
        snprintf(path, sizeof path, "%s%s", TDIR, fi < NTF ? TFILES[fi] : CFILES[fi - NTF]);
        char *txt = slurp(path);
        if (!txt) continue;
        char *p = txt;
        while ((p = strstr(p, ": X86TypeInfo<")) != NULL) {
            /* the def NAME sits before the colon; walk back to the `def ` keyword */
            char *d = p;
            while (d > txt && strncmp(d, "def ", 4)) d--;
            if (strncmp(d, "def ", 4)) { p += 14; continue; }
            char nm[32]; int a = 0;
            for (char *z = d + 4; *z && *z != ' ' && *z != '<' && *z != ':' && a < 31; z++) {
                if (*z != '\t') nm[a++] = *z;
            }
            nm[a] = 0;
            /* DEPTH-AWARE, LIKE EVERY OTHER TABLEGEN SCAN HERE. X86TypeInfo's argument list is
             * long and wraps across lines; a bare strchr(lt,'>') is only correct while no argument
             * itself contains a '>', and the moment one does the list is truncated -- which is what
             * made Xi8's trailing `1, 0` flags invisible. */
            char *lt = strchr(p, '<');
            char *gt = NULL;
            if (lt) {
                int d = 0;
                for (char *z = lt; *z; z++) {
                    if (*z == '<') d++;
                    else if (*z == '>') { d--; if (!d) { gt = z; break; } }
                    else if (*z == ';') break;
                    if (z - lt > 600) break;
                }
            }
            if (!nm[0] || !lt || !gt) { p += 14; continue; }
            /* the argument list, split at depth 0: the register class is the one that starts
             * with GR and the memory operand is the one containing "mem" -- by NAME, not by
             * position, because the table has grown parameters on both ends of this list */
            char args[400]; int al = 0;
            for (char *z = lt + 1; z < gt && al < 399; z++) args[al++] = *z;
            args[al] = 0;
            char av[24][64];
            int na = split_args(args, av, 24);
            TypeInfo *ti = &g_ti[g_nti < 63 ? g_nti : 63];
            snprintf(ti->name, sizeof ti->name, "%s", nm);
            ti->regclass[0] = 0;
            ti->memop[0] = 0;
            ti->has_even_opcode = 0;
            ti->has_rexw = 0;
            ti->immop[0] = 0;
            ti->imm8op[0] = 0;
            for (int i = 0; i < na; i++) {
                if (!ti->regclass[0] && !strncmp(av[i], "GR", 2)) snprintf(ti->regclass, sizeof ti->regclass, "%s", av[i]);
                if (!ti->memop[0] && strstr(av[i], "mem")) snprintf(ti->memop, sizeof ti->memop, "%s", av[i]);
                /* THE IMMEDIATE OPERAND CLASS, which is NOT the one before it.
                 *
                 *     def Xi8  : X86TypeInfo<i8, "b", GR8, loadi8, i8mem, Imm8, i8imm, ...>
                 *                                                      ^^^^^  ^^^^^
                 *                                                      ImmType Operand
                 * The ImmType (`Imm8`) is a scheduler/type tag; the OPERAND is `i8imm`, spelled
                 * like an operand (a lowercase width prefix and the literal "imm"). Only the
                 * operand form can appear in an (ins) list, so taking the first argument that
                 * starts with i/u and ends in "imm" picks it without a position guess. */
                if ((av[i][0] == 'i' || av[i][0] == 'u') && strstr(av[i], "imm")
                    && !strchr(av[i], '_')) {
                    /* THE FIRST imm OPERAND IS ImmOperand; THE ONE THAT NAMES 8 IS Imm8Operand.
                     *     def Xi64 : X86TypeInfo<i64, "q", GR64, loadi64, i64mem, Imm32S,
                     *                             i64i32imm, ..., i64i8imm, ...>
                     *                                     ^^^^^^^^^            ^^^^^^^^
                     *                                     ImmOperand          Imm8Operand
                     * BinOpRI8's operand list says `t.Imm8Operand:$src2` -- the sign-extended byte
                     * form -- so resolving only ImmOperand left every `addq $imm8, %r64` with an
                     * unresolved second operand. */
                    if (!ti->imm8op[0] && strstr(av[i], "8") && ti->immop[0])
                        snprintf(ti->imm8op, sizeof ti->imm8op, "%s", av[i]);
                    else if (!ti->immop[0])
                        snprintf(ti->immop, sizeof ti->immop, "%s", av[i]);
                }
            }
            /* THE LAST TWO ARGUMENTS ARE hasEvenOpcode AND hasREX_W, and they change the ENCODING
             * rather than describing an operand:
             *
             *     def Xi8  : X86TypeInfo<i8,  "b", GR8,  ..., null_frag, null_frag, 1, 0>;
             *     def Xi64 : X86TypeInfo<i64, "q", GR64, ..., i64immSExt8,          0, 1>;
             *
             * ITy, the class that actually assembles the opcode byte, reads them:
             *
             *     : I<{o{7}, o{6}, o{5}, o{4}, o{3}, o{2}, o{1},
             *          !if(!eq(t.HasEvenOpcode, 1), 0, o{0})}, ...>
             *         ↑ the 8-bit form FORCES the opcode's low bit to zero
             *
             * That is why the 8-bit and 32-bit forms of the same ALU opcode are different bytes
             * (ADD8rr = 0x00, ADD32rr = 0x01) even though the multiclass passes one BaseOpc to all
             * four widths. Ignoring it made ADD8rr claim 0x01 -- byte-identical to ADD32rr -- and
             * since isa_decode returns the FIRST longest match, every 32-bit `add` decoded as an
             * 8-bit one. */
            /* THE TWO FLAGS ARE READ FROM THE RAW TEXT, NOT BY POSITION.
             *
             * split_args skips an empty element, so `..., null_frag, null_frag, 1, 0>` can come back
             * with fewer than 14 entries and av[na-2] is then a null_frag rather than the flag --
             * which is exactly why Xi8 read even=0 and ADD8rr kept the odd opcode 0x01, colliding
             * with ADD32rr. The flags are the last two comma-separated tokens of the argument list,
             * which is unambiguous from the text. */
            {
                char lt2[400];
                snprintf(lt2, sizeof lt2, "%s", args);
                if (getenv("X86TD_TIARGS") && !strcmp(nm, "Xi8"))
                    fprintf(stderr, "  [tiargs] Xi8 args_len=%d |%s|\n", (int)strlen(args), lt2);
                /* THE FLAG VALUE FOLLOWS WHITESPACE OF ANY KIND, NEWLINES INCLUDED.
                 *
                 * The argument list wraps, so the second-to-last flag is written as
                 * `..., null_frag, null_frag,\n                       1, 0>` -- the value after the
                 * comma begins with a NEWLINE, not a space or a tab. Testing only for ' ' and '\t'
                 * then read the newline itself as the value, so Xi8's hasEvenOpcode came out 0 and
                 * ADD8rr kept the odd opcode 0x01 -- byte-identical to ADD32rr, which made every
                 * 32-bit `add` decode as an 8-bit one. Any whitespace is skipped. */
                char *lastc = strrchr(lt2, ',');
                if (lastc) {
                    char *v = lastc + 1;
                    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
                    ti->has_rexw = (*v == '1');
                    *lastc = 0;
                    char *last2c = strrchr(lt2, ',');
                    if (last2c) {
                        char *v2 = last2c + 1;
                        while (*v2 == ' ' || *v2 == '\t' || *v2 == '\n' || *v2 == '\r') v2++;
                        ti->has_even_opcode = (*v2 == '1');
                    }
                }
            }
            if (getenv("X86TD_TI"))
                fprintf(stderr, "  [ti] %-8s imm=|%s| imm8=|%s| rc=%s mem=%s\n",
                        nm, ti->immop, ti->imm8op, ti->regclass, ti->memop);
            if (ti->regclass[0] || ti->memop[0]) g_nti++;
            p = gt;
        }
        arbit_release_carve((long)txt);
    }
}

static TypeInfo *find_typeinfo(const char *n) {
    for (int i = 0; i < g_nti; i++) if (!strcmp(g_ti[i].name, n)) return &g_ti[i];
    return NULL;
}

/* Substitute identifiers in `text` from `b`, and resolve `<XiN>.<member>` through the type table.
 * A whole-token rewrite: `out` is replaced where it stands alone, never inside another word. */
typedef struct { char nm[32]; char val[200]; } Binding;

static void subst_text(const char *text, Binding *b, int nb, char *out, size_t cap) {
    size_t o = 0;
    const char *z = text;
    while (*z && o + 2 < cap) {
        if ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z') || *z == '_') {
            char tok[64]; int tl = 0;
            while ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z')
                   || (*z >= '0' && *z <= '9') || *z == '_') {
                if (tl < 63) tok[tl++] = *z;
                z++;
            }
            tok[tl] = 0;
            /* `<X>.<member>`: the member comes from a type-info, and X can be EITHER a type name
             * (`Xi32.RegClass`, written literally) OR A PARAMETER bound to one -- which is the
             * common case in this table:
             *
             *     def ADD8rr : BinOpRR_RF<BaseOpc, mnemonic, Xi8, opnodeflag>;
             *     class BinOpRR_RF<o, m, t, node, ndd> : BinOpRR<..., (outs t.RegClass:$dst), ...>
             *
             * Here `t` is the parameter and `Xi8` is its value, so the token must be resolved
             * through the bindings BEFORE the type table is consulted. Looking up "t" directly
             * found no type-info, left the text as `t.RegClass`, and operand_kind then refused the
             * operand by design -- which is why every ALU row came out with all operands A_NONE
             * and was skipped at emit. */
            if (*z == '.') {
                const char *m = z + 1;
                char mem[32]; int ml = 0;
                while ((*m >= 'A' && *m <= 'Z') || (*m >= 'a' && *m <= 'z') || *m == '_') {
                    if (ml < 31) mem[ml++] = *m;
                    m++;
                }
                mem[ml] = 0;
                const char *tname = tok;
                for (int i = 0; i < nb; i++)
                    if (!strcmp(tok, b[i].nm) && b[i].val[0]) { tname = b[i].val; break; }
                TypeInfo *ti = find_typeinfo(tname);
                if (ti && !strcmp(mem, "RegClass") && ti->regclass[0]) {
                    o += (size_t)snprintf(out + o, cap - o, "%s", ti->regclass);
                    z = m;
                    continue;
                }
                if (ti && !strcmp(mem, "MemOperand") && ti->memop[0]) {
                    o += (size_t)snprintf(out + o, cap - o, "%s", ti->memop);
                    z = m;
                    continue;
                }
                /* t.ImmOperand -- the immediate's own class. Every immediate form in the tables is
                 * forwarded this way (`(ins t.RegClass:$src1, t.ImmOperand:$src2)`), so without it
                 * the second operand of every `addq $imm, %r64` came out unresolved and the row was
                 * skipped: that is the whole add/sub/cmp-with-immediate family. */
                if (ti && !strcmp(mem, "ImmOperand") && ti->immop[0]) {
                    o += (size_t)snprintf(out + o, cap - o, "%s", ti->immop);
                    z = m;
                    continue;
                }
                if (ti && !strcmp(mem, "Imm8Operand") && ti->imm8op[0]) {
                    o += (size_t)snprintf(out + o, cap - o, "%s", ti->imm8op);
                    z = m;
                    continue;
                }
            }
            const char *rep = NULL;
            for (int i = 0; i < nb; i++)
                if (!strcmp(tok, b[i].nm) && b[i].val[0]) { rep = b[i].val; break; }
            if (rep) o += (size_t)snprintf(out + o, cap - o, "%s", rep);
            else     o += (size_t)snprintf(out + o, cap - o, "%s", tok);
        } else {
            out[o++] = *z++;
            out[o] = 0;
        }
    }
    out[o] = 0;
}

/* Derive the (outs)/(ins) text for a def by walking its class chain and substituting.
 *
 * THE WALK GOES def -> class -> base -> base, BINDING AND SUBSTITUTING AT EACH HOP, and it returns
 * the argument list of the FIRST hop whose substituted text contains a real operand group. The
 * operand groups only exist at the ITy/I level:
 *
 *     def ADD8rr : BinOpRR_RF<BaseOpc, mnemonic, Xi8, opnodeflag>;
 *     class BinOpRR_RF<o,m,t,node,ndd> : BinOpRR<o, m, ..., t, (outs t.RegClass:$dst), [...]>
 *     class BinOpRR<o,m,args,t,out,p>  : ITy<o, MRMDestReg, t[,] out,
 *                                            (ins t.RegClass:$src1, t.RegClass:$src2), m, args, p>
 *
 * At the BinOpRR hop the substituted text carries `(outs GR8:$dst)` (t bound to Xi8) and the
 * parameter `out`; at the ITy hop `out` has been replaced by that list and the real (ins) appears.
 * Returning the first hop that has ANY operand text yields only the (outs) -- which produced rows
 * with one operand, and the encoder then wrote one register where two belong.
 */
static int derive_operands(const char *cls, const char *args, char *out, size_t cap, int depth) {
    if (depth > 8) return 0;
    ClassDef *c = find_class(cls);
    if (!c) return 0;
    char av[12][64];
    int na = split_args(args, av, 12);
    Binding b[16]; int nb = 0;
    for (int i = 0; i < c->narg && i < 12 && nb < 16; i++) {
        snprintf(b[nb].nm, sizeof b[nb].nm, "%s", c->parg[i]);
        snprintf(b[nb].val, sizeof b[nb].val, "%s", (i < na) ? av[i] : "");
        nb++;
    }
    if (!c->base[0]) return 0;
    char sub[2400];
    subst_text(c->baseargs, b, nb, sub, sizeof sub);

    /* RESOLVE MEMBERS AT THIS HOP BEFORE RECURSING. `t.RegClass` and `t.ImmOperand` appear at the
     * level that forwards them into ITy, and the operand groups only become usable text once those
     * are substituted -- a lower hop cannot do it because `t` is not its parameter. */
    char res[2400];
    subst_text(sub, b, nb, res, sizeof res);
    /* the per-hop debug print, kept because it is the tool that found the escape/prefix chain bug */
    if (getenv("X86TD_HOP"))
        fprintf(stderr, "  [hop%d] cls=%s base=%s regcls=%d res=|%.200s|\n",
                depth, cls, c->base, strstr(res, "RegClass") ? 1 : 0, res);

    /* A HOP WHOSE OWN TEXT CARRIES A REAL (ins) GROUP IS THE ONE TO STOP AT.
     *
     * `class BinOpRI<o, m, args, t, f, dag out, list<dag> p>
     *      : ITy<o, f, t, out, (ins t.RegClass:$src1, t.ImmOperand:$src2), ...>`
     * The outs dag is a PARAMETER here and the ins list is literal, so the "both groups in one text"
     * test never holds anywhere along `IMulOpRI_RF` -- whose own text has an outs and a PATTERN but
     * no ins. Recursing first then overwrote whatever this hop found.
     *
     * The pattern text is what made the first attempt at this wrong: `[(set ... t.ImmNoSuOperator:
     * $src2)]` contains the string `(ins` only by accident (`NoSuOperator` has no `(ins`, but the
     * def's raw argument list passed to this function starts at `0x69, MRMSrcReg, Xi64, (outs ...)`
     * and the pattern's `(set` follows), so requiring the ins to sit OUTSIDE a `[ ... ]` pattern is
     * what distinguishes a real operand group from pattern noise. */
    int ins_at = -1, pat_at = -1;
    {
        const char *pi = strstr(sub, "(ins");
        if (pi) ins_at = (int)(pi - sub);
        const char *pb = strchr(sub, '[');
        if (pb) pat_at = (int)(pb - sub);
    }
    /* THE TESTS ARE ON `sub`, NOT `res`: `sub` is this hop's inheritance arguments with the bindings
     * applied, which is where the operand groups live. `res` is the same text with the member
     * spellings resolved, and it is what gets RETURNED -- but `res` may carry an unresolved
     * `<type>.<member>` that `sub` does not (and vice versa), so gating the search on `res` found
     * groups in the wrong place and the walk fell through to the caller's raw argument list. */
    if (ins_at >= 0 && (pat_at < 0 || ins_at < pat_at)
        && !strstr(sub, "RegClass") && !strstr(sub, ".MemOperand") && !strstr(sub, "{7}")) {
        for (int bi2 = 0; bi2 < nb; bi2++) {
            if (!strcmp(b[bi2].nm, "out") && strstr(b[bi2].val, "(outs")
                && !strstr(b[bi2].val, "RegClass")) {
                snprintf(out, cap, "%s, %s", b[bi2].val, res);
                return 1;
            }
        }
    }
    if (strstr(sub, "(outs") && ins_at >= 0 && (pat_at < 0 || ins_at < pat_at)
        && !strstr(sub, "RegClass") && !strstr(sub, ".MemOperand") && !strstr(sub, "{7}")) {
        snprintf(out, cap, "%s", res);
        return 1;
    }

    /* recurse: a LOWER hop has the complete pair (the (ins) list too) */
    char deeper[2400];
    if (derive_operands(c->base, sub, deeper, sizeof deeper, depth + 1)) {
        /* THE DEEPER TEXT GETS RESOLVED WITH THIS HOP'S BINDINGS AND WITH AN EMPTY SET -- the member
         * spellings inside it are either literal type names (`Xi64.Imm8Operand`, which the type table
         * resolves directly) or parameters of a class further down, which this hop cannot bind. Both
         * cases are handled by resolving with this hop's bindings first and then again with none, so a
         * literal type name is never left unresolved just because this hop has no `t`. */
        subst_text(deeper, b, nb, res, sizeof res);
        /* A LOWER HOP MAY RETURN ONLY THE (ins), BECAUSE THE (outs) IS THIS HOP'S `out` PARAMETER.
         * Joining them here is what gives `imulq $imm32,%rcx,%rax` its three operands: `BinOpRI` owns
         * `(ins t.RegClass:$src1, t.ImmOperand:$src2)` and takes its outs dag as a parameter, which
         * `IMulOpRI_RF` supplies as `(outs t.RegClass:$dst)`. Without the join the row described two
         * operands over a three-operand encoding and matched nothing. */
        if (!strstr(res, "(outs")) {
            for (int bi3 = 0; bi3 < nb; bi3++) {
                if (!strcmp(b[bi3].nm, "out") && strstr(b[bi3].val, "(outs")) {
                    char joined[2400];
                    snprintf(joined, sizeof joined, "%s, %s", b[bi3].val, res);
                    snprintf(res, sizeof res, "%s", joined);
                    break;
                }
            }
        }
        if (strstr(res, ".Imm") || strstr(res, "RegClass") || strstr(res, "MemOperand")) {
            Binding none[1]; none[0].nm[0] = 0; none[0].val[0] = 0;
            subst_text(res, none, 1, out, cap);
            return 1;
        }
        snprintf(out, cap, "%s", res);
        return 1;
    }
    /* no lower hop has them: this text is the outermost place they exist, so require BOTH groups
     * (one alone is an incomplete description of the encoding) */
    if (strstr(sub, "(outs") && strstr(sub, "(ins")
        && !strstr(sub, "RegClass") && !strstr(sub, ".MemOperand") && !strstr(sub, "{7}")) {
        snprintf(out, cap, "%s", sub);
        return 1;
    }
    /* A HOP CAN CARRY ONLY THE (ins), WITH THE (outs) ARRIVING AS ITS OWN `out` PARAMETER.
     *
     * `class BinOpRI<o, m, args, t, f, dag out, list<dag> p>
     *      : ITy<o, f, t, out, (ins t.RegClass:$src1, t.ImmOperand:$src2), m, args, p>`
     * -- the outs dag is whatever the caller passed and the ins list is literal here. The
     * requirement above then never holds at any hop of `IMulOpRI_RF` (whose own text has an outs and
     * a pattern but no ins), and the walk gave up with the def's raw arguments: the row for
     * `imulq $imm32,%rcx,%rax` described two operands over a three-operand encoding, so it matched
     * nothing. When this hop HAS an ins group and its own `out` binding resolved to real text, the
     * pair is complete and this is the hop to stop at.
     *
     * `out` and `p` are bound by name here, so the substitution has already replaced them. */
    if (strstr(sub, "(ins") && !strstr(sub, "RegClass") && !strstr(sub, ".MemOperand")) {
        for (int bi2 = 0; bi2 < nb; bi2++) {
            if (!strcmp(b[bi2].nm, "out") && strstr(b[bi2].val, "(outs")) {
                snprintf(out, cap, "%s, %s", b[bi2].val, sub);
                return 1;
            }
        }
    }
    return 0;
}

/* THE WIDTH PREFIX COMES FROM THE TYPE ARGUMENT, NOT THE CLASS NAME.
 *
 *     let hasREX_W = t.HasREX_W;          <- ITy, and the table states it outright
 *     def Xi64 : X86TypeInfo<i64, "q", GR64, ..., 0, 1>;   <- the last field is HasREX_W
 *
 * So BinOpRR_RF carries REX.W for its 64rr form and NOT for 8rr/32rr, even though all four name
 * the same class. Deriving the prefix from the class name alone got this wrong: every ALU row
 * came out either 64-bit or 8-bit depending on which name pattern happened to match first. */
static unsigned flags_from_type(const char *args, const char *defname, const char *cls) {
    unsigned cf = C_NOREX;
    char av[12][64];
    int na = split_args(args, av, 12);
    for (int i = 0; i < na; i++) {
        TypeInfo *ti = find_typeinfo(av[i]);
        if (!ti) continue;
        /* the width follows the type's own hasREX_W, which the table states outright */
        if (ti->has_rexw)              cf = C_REXW;
        else if (!strcmp(av[i], "Xi16")) cf = C_OPSIZE16 | C_NOREX;
        else                           cf = C_NOREX;
        break;
    }
    /* a type that survived as a bare width in the NAME (INC64r) still has to set the prefix.
     *
     * THE WIDTH IS THE LAST ONE IN THE NAME, NOT ANY ONE OF THEM. `LEA64_32r` is a 32-BIT
     * destination with a 64-bit address size -- the `64` describes the addressing, not the operand
     * width -- so `strstr(defname, "64")` gave it REX.W and its row became `48 8d`, which left
     * `leal (%rdi),%eax` (plain `8d 07`, what llvm-mc emits) with no row at all. The trailing number
     * is the operand width in every name in these tables: LEA64_32r -> 32, LEA64_8r -> 8. */
    if (cf == C_NOREX) {
        /* A 64-BIT REGISTER OPERAND IS THE STATEMENT OF REX.W, whatever the name's numbers say:
         * `MOV64ri32 : RIi32S<0xC7, MRM0r, (outs GR64:$dst), (ins i64i32imm:$src)>` is a 64-bit
         * destination with a 32-bit SIGN-EXTENDED immediate (`48 c7 c0 34 12 00 00`), so the trailing
         * `32` is the immediate's width, not the operand's. */
        int has_gr64 = 0;
        /* `GR64` IS A 64-BIT GENERAL REGISTER; `VR64` IS AN MMX REGISTER AND SAYS NOTHING ABOUT REX.W.
         *
         * They share the trailing `64`, and treating VR64 as a 64-bit GPR was the whole reason
         * `psllq %mm1,%mm0` came out `48 0f f3 c0` -- a REX.W prefix on an MMX instruction, which the
         * CPU does not define. MMX registers are 64 bits wide in an encoding space that never grew a
         * REX byte; `VR128`/`VR256` are XMM/YMM and likewise state no REX.W. Only `GR64` and the
         * explicit REX_W class attribute carry that meaning.
         *
         * A 64-BIT MEMORY OPERAND IS THE SAME STATEMENT. `MOV64mi32 : RIi32S<0xC7, MRM0m, (outs),
         * (ins i64mem:$dst, i64i32imm:$src)>` names no GR64 at all -- the destination is memory -- so
         * the name's trailing number (`32`, the immediate's width) was taken as the operand width and
         * the row came out with no REX.W. `48 c7 45 f0 00 00 00 00` (`movq $0,-0x10(%rbp)`) had no row
         * of its own and the first two bytes matched a raw pseudo row instead. `i64mem` and `i64imm`
         * state the 64-bit width directly. */
        int has_vec_reg = 0;
        for (const char *z = args; *z; z++)
            if (!strncmp(z, "VR", 2) || !strncmp(z, "FR", 2)) { has_vec_reg = 1; break; }
        for (const char *z = args; *z; z++)
            if (!strncmp(z, "GR64", 4)
                /* A 64-BIT MEMORY OPERAND STATES THE WIDTH ONLY IN THE INTEGER SPACE. `MOVQI2PQIrm :
                 * I<0x7E, MRMSrcMem, (outs VR128:$dst), (ins i64mem:$src)>` also names `i64mem`, but its
                 * real encoding is `f3 0f 7e` -- the `XS` prefix and no REX.W at all -- and taking the
                 * width from the memory operand gave it a spurious `48` (`f3 48 0f 7e`), so
                 * `movq (%rsi),%xmm0` had no row. In the vector space the width is carried by the
                 * register and the prefix, never by REX.W. */
                || (!has_vec_reg && (!strncmp(z, "i64mem", 6) || !strncmp(z, "i64imm", 6))))
                { has_gr64 = 1; break; }
        int lastw = 0;
        for (const char *z = defname; *z; z++)
            if (*z >= '0' && *z <= '9') {
                int v = 0;
                while (*z >= '0' && *z <= '9') { v = v * 10 + (*z - '0'); z++; }
                if (v == 8 || v == 16 || v == 32 || v == 64) lastw = v;
                z--;
            }
        /* THE DESTINATION REGISTER DECIDES THE WIDTH WHEN THE OUTS NAMES ONE LITERALLY.
         *
         * `MOVSX32rm16` sign-extends a 16-BIT SOURCE to a 32-bit destination and its encoding is
         * `0f bf` -- no 0x66 -- while `MOVSX16rm8` (movsbw) IS `66 0f be`. The trailing number in the
         * NAME is the SOURCE's width in this family, so reading the width from it gave MOVSX32rm16 the
         * 16 and the prefix, and `movswl -0x2(%rbp),%eax` matched nothing. Measured:
         *     movsbw %al,%cx = 66 0f be   movsbl %al,%ecx = 0f be   movsbq %al,%rcx = 48 0f be
         *     movswl %ax,%ecx = 0f bf     movswq %ax,%rcx = 48 0f bf
         * Only a LITERAL GR16/GR32/GR64 in the (outs) group is read here; a class parameter
         * (`outs RC:$dst`) falls through to the name, as before. */
        int outs_w_known = 0;
        {
            const char *o = strstr(args, "(outs");
            if (o) {
                const char *oe = strchr(o, ')');
                for (const char *z = o; z && (!oe || z < oe); z++) {
                    if (!strncmp(z, "GR16", 4)) { cf = C_OPSIZE16 | C_NOREX; outs_w_known = 1; break; }
                    if (!strncmp(z, "GR64", 4)) { cf = C_REXW; outs_w_known = 1; break; }
                    if (!strncmp(z, "GR32", 4)) { cf = C_NOREX; outs_w_known = 1; break; }
                }
            }
        }
        if (outs_w_known) { /* the outs register is authoritative */ }
        else if (has_gr64 || lastw == 64)  cf = C_REXW;
        else if (lastw == 16)         cf = C_OPSIZE16 | C_NOREX;
        /* SOME FAMILIES ARE ALREADY 64-BIT IN LONG MODE AND NEVER CARRY REX.W.
         *
         * `jmp *%rax` is `ff e0`, `call *%rax` is `ff d0`, `pushq %rax` is `50` -- the 64-bit operand
         * size is the long-mode default, so REX.W is not merely optional, it is absent. The tables say
         * the same thing: `JMP64r : I<0xFF, MRM4r, (outs), (ins GR64:$dst)>` is the normal form, while
         * the REX.W variant exists as its own def inside an explicit `let hasREX_W = 1` block
         * (`JMP64r_REX`, for the Win64 ABI) -- so taking REX.W from the def name's `64` put a `48` on
         * `jmp *%rax` and the real two-byte encoding had no row. The name's trailing number states the
         * operand width, and for these families that width is the default rather than a prefix. */
        if (cf == C_REXW && lastw == 64) {
            static const char *NO_REXW_FAMILY[] = {
                "JMP", "CALL", "PUSH", "POP", "RET", "LEAVE", "ENTER", "IRET", "SYSRET", "SYSEXIT",
                "LJMP", "LCALL", "LOOP", "JCXZ", "JECXZ", "JRCXZ", NULL
            };
            for (int f = 0; NO_REXW_FAMILY[f]; f++) {
                size_t fl = strlen(NO_REXW_FAMILY[f]);
                /* THE FAMILY MUST BE FOLLOWED BY THE WIDTH DIGIT. `POP` is a family but `POPCNT64rr`
                 * is not a member of it -- `f3 48 0f b8 c0` does carry REX.W -- so the character after
                 * the family name has to be a digit for the override to apply. */
                if (!strncmp(defname, NO_REXW_FAMILY[f], fl)
                    && defname[fl] >= '0' && defname[fl] <= '9') { cf = C_NOREX; break; }
            }
        }
    }
    /* the immediate width is stated by the class: BinOpRI8_RF takes a byte, BinOpRI_RF a dword,
     * Ii8/Ii16/Ii32/Ii64 their own widths. */
    if (strstr(cls, "RI8") || strstr(cls, "ri8"))       cf |= C_IMM8;
    else if (strstr(cls, "RI16"))                       cf |= C_OPSIZE16 | C_IMM16;
    else if (strstr(cls, "RI64"))                       cf |= C_IMM64;
    else if (strstr(cls, "RI_RF") || strstr(cls, "RI32") || strstr(cls, "ri32")) cf |= C_IMM32;
    else if (strstr(cls, "Ii8"))                        cf |= C_IMM8;
    else if (strstr(cls, "Ii16"))                       cf |= C_OPSIZE16 | C_IMM16;
    else if (strstr(cls, "Ii32"))                       cf |= C_IMM32;
    else if (strstr(cls, "Ii64"))                       cf |= C_IMM64;
    else if (strstr(cls, "PCRel8"))                     cf |= C_REL8;
    else if (strstr(cls, "PCRel"))                      cf |= C_REL32;
    /* THE WIDTH PREFIX CAN COME FROM A CLASS IN THE CHAIN, BUT ONLY WHEN NOTHING ELSE DECIDED IT.
     *
     * `class RS2I<...> : S2I<o, F, outs, ins, asm, pattern>, REX_W;` states the 64-bit prefix as an
     * attribute on the CLASS, and
     *     def MOV64toPQIrr : RS2I<0x6E, MRMSrcReg, (outs VR128:$dst), (ins GR64:$src), ...>
     * passes no type-info token at all -- literal register classes only -- so the type lookup below
     * finds nothing and the row would ship without REX.W (`movq %rax,%xmm0` is 66 48 0f 6e c0).
     *
     * It must NOT override a width that the operands state, though: `def LEA32r : RI<0x8D, ...>` names
     * GR32 operands and a `REX_W` would make `leal` emit 48 8d. So this runs only when the operand
     * list itself named no 64-bit register class. */
    int operands_say64 = 0, operands_say32 = 0;
    for (const char *z = args; *z; z++) {
        if (!strncmp(z, "GR64", 4) || !strncmp(z, "VR64", 4)) operands_say64 = 1;
        if (!strncmp(z, "GR32", 4) || !strncmp(z, "VR32", 4)) operands_say32 = 1;
    }
    /* ...AND ONLY WHEN THE CLASS DOES NOT ITSELF DESCRIBE A NARROWER FORM. A memory type like
     * `lea32mem` names no register class, so the operand scan above sees nothing and `leal` would
     * take REX.W from the chain -- emitting 48 8d for an encoding that is 8d. The class names that
     * opt into REX.W are the 64-bit ones; everything else in the `RI`/`Ii`/`S2I` families is
     * 32-bit or narrower, so the chain rule is applied only outside them. */
    /* `RI` IS A REX.W CLASS, NOT A NARROW ONE.
     *
     *     class RI<bits<8> o, Format F, dag outs, dag ins, string asm, list<dag> pattern>
     *           : I<o, F, outs, ins, asm, pattern>, REX_W;
     *
     * So it belongs in the REX_W half of the decision, and listing it here suppressed the class-chain
     * walk that would have found its `REX_W` -- `CDQE : RI<0x98, RawFrm, (outs), (ins)>` (`48 98`) came
     * out as `40 98` (an empty REX, which is a different encoding) and `cltq` had no row. */
    int cls_is_narrow = strstr(cls, "Ii") || strstr(cls, "IMul")
                        || strstr(cls, "S2I")
                        /* THE VECTOR FAMILIES NEVER TAKE REX.W FROM A CHAIN ATTRIBUTE. `MMXI`,
                         * `PSI`, `PDI`, `PI`, `SSI`, `SDI` and their relatives are the MMX/SSE
                         * spaces, where a REX byte is either absent or written as part of the
                         * instruction; `MMX_PSLLQrr` came out `48 0f f3 c0` (a REX.W on an MMX
                         * instruction, which does not exist) because some class in its chain carries
                         * REX_W for an unrelated purpose. */
                        || strstr(cls, "MMX") || strstr(cls, "SSE")
                        || !strncmp(cls, "PI", 2) || strstr(cls, "PSI") || strstr(cls, "PDI")
                        || strstr(cls, "SSI") || strstr(cls, "SDI") || strstr(cls, "SI_");
    if (!operands_say64 && !operands_say32 && !cls_is_narrow && cf != C_REXW) {
        char cn9[80];
        snprintf(cn9, sizeof cn9, "%s", cls);
        for (int hop9 = 0; hop9 < 8 && cn9[0]; hop9++) {
            ClassDef *cd9 = find_class(cn9);
            if (!cd9) break;
            if (strstr(cd9->baseargs, "REX_W") || strstr(cd9->inher_tail, "REX_W")) {
                cf = (cf & ~C_OPSIZE16) | C_REXW;
                break;
            }
            if (!cd9->base[0]) break;
            snprintf(cn9, sizeof cn9, "%s", cd9->base);
        }
    }
    return cf;
}

/* THE OpPrefix ATTRIBUTE, read rather than inferred.
 *
 * X86InstrFormats.td defines the values and they are the table's own vocabulary:
 *
 *     def NoPrfx : Prefix<0>;   def PD : Prefix<1>;   def XS : Prefix<2>;
 *     def XD     : Prefix<3>;   def PS : Prefix<4>;
 *
 * The prefix BYTE each one selects is fixed by the ISA: PD -> 0x66, XS -> 0xF3, XD -> 0xF2, and
 * NoPrfx/PS -> no byte at all (PS is the "no prefix, but the disassembler must know PD/XS/XD forms
 * of this opcode exist" marker -- it is not a 0x66, which is exactly what the class-NAME inference
 * got wrong for `PSI`).
 *
 * `TB`, `T8`, `TA` are the opcode MAPS of the same class, not prefixes, so they are matched
 * separately and never conflated with these.
 */
static int sse_prefix_of(const char *text) {
    if (!text || !*text) return 0;
    /* a whole-token match: `PD` must not be found inside another identifier, and these are spelled
     * as bare attributes after the class arguments (`..., TB, PD;`). */
    for (const char *p = text; *p; p++) {
        if (p != text && ((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z')
                          || (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_')) continue;
        if (!strncmp(p, "PD", 2) && !((p[2] >= 'A' && p[2] <= 'Z') || p[2] == '_')) return 1;
        if (!strncmp(p, "XS", 2) && !((p[2] >= 'A' && p[2] <= 'Z') || p[2] == '_')) return 2;
        if (!strncmp(p, "XD", 2) && !((p[2] >= 'A' && p[2] <= 'Z') || p[2] == '_')) return 3;
        if (!strncmp(p, "PS", 2) && !((p[2] >= 'A' && p[2] <= 'Z') || p[2] == '_')) return 4;
    }
    return 0;
}

/* THE ENCLOSING defm'S NAME, for resolving `#NAME#` inside a multiclass body.
 *
 * A multiclass can instantiate its own members under a multiple of the outer name
 * (`defm PS : ...` inside `multiclass sse12_fp_packed_logical`, reached through
 * `defm XOR : sse12_fp_packed_logical<...>`), and the real name is the outer defm's name plus the
 * inner defm's (`XOR` + `PS` = `XORPS`). `#NAME#` is that outer name. Set by the defm pass as it
 * walks, so a name containing the placeholder resolves against the instantiation enclosing it. */
static char g_outer_defm[80];
/* THE MULTICLASS ENCLOSING THE DEF BEING HELD, carried from the test that decides whether the def is
 * held to the code that records WHICH multiclass it belongs to. Deriving that twice with two
 * separate text walks let them disagree -- a `def ri` inside `multiclass sse2_pshuffle` was held
 * (correct) but attributed to `RorX` (wrong, its body closed hundreds of lines earlier), so the
 * outer `defm PSHUFD` never matched it and every pshufd row was dropped. One walk, one answer. */
static const char *g_last_mc_name = NULL;

/* THE BODY OF `multiclass NAME<...> { ... }`, for expanding inner defms.
 *
 * A multiclass can instantiate its own members (`defm PS : ...` inside
 * `multiclass sse12_fp_packed_logical`), and the real name of each is the OUTER instantiation's name
 * plus the inner one's (`XOR` + `PS` = `XORPS`). The inner defm also references the outer multiclass's
 * parameters (`opc`, `OpcodeStr`), which only have values at the outer `defm XOR : ...` line.
 *
 * So the pair is expanded where both are known: this returns the text between the multiclass's braces
 * so the caller can find its inner defms, substitute the outer parameters into their arguments, and
 * emit one synthetic `defm` line per pair. `*end` gets the matching `}`. */
static const char *mc_body(const char *txt, const char *name, const char **end) {
    *end = NULL;
    size_t nl = strlen(name);
    for (const char *p = txt; (p = strstr(p, "multiclass ")) != NULL; ) {
        const char *q = p + 11;
        if (strncmp(q, name, nl) || (q[nl] && q[nl] != '<' && q[nl] != ' ' && q[nl] != '{')) {
            p = q;
            continue;
        }
        /* find the opening brace: the parameter list wraps, so scan depth-aware */
        int d = 0;
        while (*q && (*q != '{' || d > 0)) {
            if (*q == '<') d++;
            else if (*q == '>') { if (d > 0) d--; }
            q++;
        }
        if (*q != '{') { p = q; continue; }
        const char *bs = q + 1;
        int depth = 1;
        const char *r = bs;
        while (*r && depth > 0) {
            if (*r == '{') depth++;
            else if (*r == '}') { depth--; if (!depth) break; }
            r++;
        }
        *end = r;
        return bs;
    }
    return NULL;
}

/* THE ROW'S TIE TEXT: the `Constraints = "$src1 = $dst"` that says which in re-states an out.
 *
 * It is stated on the class (`BinOpRR_RF` builds it from `tie_dst_src1`) or by a surrounding `let`,
 * never inside the operand list, so it is collected by walking the same inheritance chain the escape
 * and prefix use. A row with no tie gets "" and every in is a real field. */
static const char *tie_of(const char *cls) {
    static char tiebuf[160];
    tiebuf[0] = 0;
    char cn[80];
    snprintf(cn, sizeof cn, "%s", cls);
    for (int hop = 0; hop < 8 && cn[0]; hop++) {
        ClassDef *cd = find_class(cn);
        if (!cd) break;
        const char *c1 = strstr(cd->baseargs, "Constraints");
        if (!c1) c1 = strstr(cd->inher_tail, "Constraints");
        if (c1) {
            const char *q = strchr(c1, '"');
            if (q) {
                q++;
                int tl = 0;
                while (*q && *q != '"' && tl < 159) tiebuf[tl++] = *q++;
                tiebuf[tl] = 0;
                if (tiebuf[0]) return tiebuf;
            }
        }
        if (!cd->base[0]) break;
        snprintf(cn, sizeof cn, "%s", cd->base);
    }
    return tiebuf;
}

/* WALK the chain. Fills *opcode and fmtname when the chain reaches literals.
 *
 * THE WALK HAS EXACTLY ONE JOB AT EACH HOP, and doing it as a single scan instead of a chain of
 * special cases is what makes it terminate correctly:
 *
 *   1. bind this class's parameters to the arguments it was called with
 *   2. substitute those bindings into this class's OWN inheritance arguments
 *   3. look for a literal opcode and a named Format IN THOSE SUBSTITUTED ARGUMENTS
 *   4. if either is still missing, recurse into the base with the substituted arguments
 *
 * The previous version checked "is there a literal opcode in bav" as the gate for also looking for
 * the format, so a hop where the opcode had already been bound (BinOpRR, reached from BinOpRR_F with
 * 0x85 in hand) skipped the format search and never saw the literal `MRMDestReg` in its own
 * arguments. It also accepted any argument as the format, which let the mnemonic string "test"
 * through. Both are fixed by looking for the two independently, every hop, and by rejecting a quoted
 * string or a bare 0x literal as a format.
 */
/* AN OPCODE OF 0x00 IS A REAL ENCODING, SO 0 CANNOT BE THE "NOT FOUND" SENTINEL.
 *
 * `PSHUFB` is `defm PSHUFB : SS3I_binop_rm<0x00, "pshufb", ...>` -- the opcode byte is genuinely
 * zero (0F 38 00). The resolver treated `*opcode == 0` as "nothing found yet" and returned 0 from
 * every class in the chain, so no PSHUFB row was ever emitted and `66 0f 38 00 c1` matched nothing.
 * This flag is set at the moment an opcode literal is read, at any depth, so a found 0x00 is
 * distinguishable from an absent one. */
static int g_op_seen = 0;

static int resolve_def(const char *cls, const char *args, long *opcode, char *fmtname, size_t fn,
                       int depth) {
    if (depth > 10) return 0;
    if (depth == 0) g_op_seen = 0;      /* a fresh top-level query; recursion must not clear it */
    ClassDef *c = find_class(cls);
    if (!c) return 0;

    char av[12][64];
    int na = split_args(args, av, 12);

    /* 1. bind */
    Binding b[16]; int nb = 0;
    for (int i = 0; i < c->narg && i < 12 && nb < 16; i++) {
        snprintf(b[nb].nm, sizeof b[nb].nm, "%s", c->parg[i]);
        snprintf(b[nb].val, sizeof b[nb].val, "%s", (i < na) ? av[i] : "");
        nb++;
    }

    /* a def that names the literals directly (its own arguments ARE the class arguments) */
    for (int i = 0; i < na; i++) {
        if (!strncmp(av[i], "0x", 2) && *opcode == 0) { *opcode = strtol(av[i], NULL, 0); g_op_seen = 1; }
    }

    /* THE CLASS BODY'S OWN `let Opcode` OVERRIDES ANYTHING FURTHER DOWN THE CHAIN.
     *
     *     class IMulOpMI8_R<X86TypeInfo t, ...>
     *       : BinOpMI8<"imul", binop_ndd_args, t, MRMSrcMem, (outs t.RegClass:$dst)> {
     *       let Opcode = 0x6B;
     *     }
     *
     * The inheritance line passes no opcode, so the walk reached BinOpMI8's literal 0x83 -- the
     * ALU-immediate opcode -- and `IMUL64rmi8` shipped as `48 83`. Nothing in real code is that, so
     * `48 6b 45 f0 03` (`imulq $3,-0x10(%rbp),%rax`) had no row. Checked before the recursion and
     * only when the def/args did not already name one, so a class that states it on the line still
     * wins. */
    if (*opcode == 0) {
        const char *bp = strstr(c->bodylet, "Opcode");
        if (bp) {
            const char *eq = strchr(bp, '=');
            if (eq) {
                const char *v = eq + 1;
                while (*v == ' ' || *v == '\t') v++;
                if (!strncmp(v, "0x", 2)) { *opcode = strtol(v, NULL, 0); g_op_seen = 1; }
            }
        }
    }

    if (!c->base[0]) {
        /* nothing to recurse into: the literals must be here */
        for (int i = 0; i < na; i++) {
            if (av[i][0] == '"' || !strncmp(av[i], "0x", 2)) continue;
            if (format_kind(av[i])) { snprintf(fmtname, fn, "%s", av[i]); break; }
        }
        return (g_op_seen && fmtname[0]) ? 1 : 0;
    }

    /* 2. substitute into this class's inheritance arguments */
    char sub[2400];
    subst_text(c->baseargs, b, nb, sub, sizeof sub);

    char subav[12][64];
    int nsa = split_args(sub, subav, 12);

    /* 3. look for the literals HERE, independently of each other */
    for (int i = 0; i < nsa; i++) {
        if (!strncmp(subav[i], "0x", 2)) {
            if (*opcode == 0 && !g_op_seen) { *opcode = strtol(subav[i], NULL, 0); }
            g_op_seen = 1;
            continue;
        }
        if (subav[i][0] == '"') continue;                    /* the mnemonic, never a format */
        if (format_kind(subav[i]) && !fmtname[0]) snprintf(fmtname, fn, "%s", subav[i]);
    }

    /* 4. anything still missing comes from further down */
    if (*opcode == 0 || !fmtname[0]) {
        char dfmt[80] = "";
        long dop = *opcode;
        int got = resolve_def(c->base, sub, &dop, dfmt, sizeof dfmt, depth + 1);
        if (got) {
            if (*opcode == 0 && dop) *opcode = dop;
            if (!fmtname[0] && dfmt[0]) snprintf(fmtname, fn, "%s", dfmt);
        }
    }
    if (*opcode == 0 && !g_op_seen) return 0;
    return 1;      /* the opcode alone is useful; the caller checks the format */
}

/* ---- class name -> flags ---- */
static unsigned class_flags(const char *c) {
    /* the IiNN / RIiNN families first, they are the longest names */
    if (!strcmp(c, "Ii8PCRel"))         return C_REL8;
    if (!strcmp(c, "Ii32PCRel"))        return C_REL32;
    if (!strcmp(c, "Ii16PCRel"))        return C_OPSIZE16 | C_REL32;   /* 0x66 + rel16: not emitted */
    /* AN IMMEDIATE'S WIDTH DOES NOT IMPLY THE 0x66 PREFIX.
     *
     * `Ii16` names the immediate's size, not the operand size: RETI32 is Ii16<0xC2, ...> with
     * OpSize32 and its encoding is plain `c2 08 00`. Mapping Ii16 to C_OPSIZE16 produced
     * `66 c2 08 00`, an encoding that does not exist, and it hid the real `ret $imm` from the
     * decoder. The prefix comes from the def's own OpSize16 attribute (applied below) or from the
     * type's HasREX_W, never from the immediate's width. */
    if (!strcmp(c, "Ii8"))    return C_IMM8;
    if (!strcmp(c, "Ii16"))   return C_IMM16;
    if (!strcmp(c, "Ii32"))   return C_IMM32;
    if (!strcmp(c, "Ii64"))   return C_IMM64;
    if (!strcmp(c, "RIi8"))   return C_REXW | C_IMM8;
    if (!strcmp(c, "RIi16"))  return C_REXW | C_IMM16;
    if (!strcmp(c, "RIi32"))  return C_REXW | C_IMM32;
    if (!strcmp(c, "RIi64"))  return C_REXW | C_IMM64;
    if (!strcmp(c, "RI"))     return C_REXW;
    if (!strcmp(c, "I"))      return C_NOREX;
    if (!strcmp(c, "R"))      return C_REXW;
    /* multiclass-instantiated families */
    if (!strcmp(c, "BinOpRR_RF"))   return C_REXW;
    if (!strcmp(c, "BinOpRM_RF"))   return C_REXW;
    if (!strcmp(c, "BinOpRI_RF"))   return C_REXW | C_IMM32;
    if (!strcmp(c, "BinOpRI8_RF"))  return C_REXW | C_IMM8;
    if (!strcmp(c, "BinOpRR_RF_Rev")) return C_REXW;
    if (!strcmp(c, "UnaryOpR_RF"))  return C_REXW;
    if (!strcmp(c, "UnaryOpM_MF"))  return C_REXW;
    return C_UNKNOWN;
}

/* ---- format name -> enum ---- */
static int format_kind(const char *f) {
    if (!strcmp(f, "RawFrm"))          return F_RAW;
    if (!strcmp(f, "AddRegFrm"))       return F_ADDREG;
    if (!strcmp(f, "RawFrmMemOffs"))   return F_RAWMOFFS;
    if (!strcmp(f, "RawFrmSrc"))       return F_RAWSRC;
    if (!strcmp(f, "RawFrmDst"))       return F_RAWDST;
    if (!strcmp(f, "RawFrmDstSrc"))    return F_RAWDSTSRC;
    if (!strcmp(f, "RawFrmImm8"))      return F_RAW_IMM8;
    if (!strcmp(f, "RawFrmImm16"))     return F_RAW_IMM16;
    if (!strcmp(f, "AddCCFrm"))        return F_ADDCC;
    if (!strcmp(f, "MRMDestMem"))      return F_MRMDESTMEM;
    if (!strcmp(f, "MRMDestMem4VOp3")) return F_MRMDESTMEM;
    if (!strcmp(f, "MRMSrcMem"))       return F_MRMSRCMEM;
    if (!strcmp(f, "MRMSrcMem4VOp3"))  return F_MRMSRCMEM;
    if (!strcmp(f, "MRMSrcMemOp4"))    return F_MRMSRCMEM;
    if (!strcmp(f, "MRMDestReg"))      return F_MRMDESTREG;
    if (!strcmp(f, "MRMSrcReg"))       return F_MRMSRCREG;
    if (!strcmp(f, "MRMSrcReg4VOp3"))  return F_MRMSRCREG;
    if (!strcmp(f, "MRMSrcRegOp4"))    return F_MRMSRCREG;
    if (!strcmp(f, "MRMXm"))           return F_MRMX_M;
    if (!strcmp(f, "MRMXr"))           return F_MRMX_R;
    /* THE SETCC FAMILY. `def SETCCr : I<0x90, MRMXrCC, (outs GR8:$dst), (ins ccode:$cond)>` -- the
     * same "reg field ignored, condition in the opcode's low nibble" shape as MRMXr/MRMXm, with the
     * CC suffix marking that the nibble is the condition. Without these two names every setcc row
     * was refused for an unknown format, so `setae` (0f 93 c0) -- 2 of the 133 instructions in an
     * -O2 object, and a very common compiler idiom -- had no form at all. */
    if (!strcmp(f, "MRMXrCC"))         return F_MRMX_R;
    if (!strcmp(f, "MRMXmCC"))         return F_MRMX_M;
    /* THE CONDITION-CODE FORMS. A cmovcc / setcc carries the condition in the opcode's low nibble
     * the same way a jcc does, so the layout is F_ADDCC and the field rule is the one already
     * written for AddCCFrm -- the only difference is that these formats also have a ModR/M.
     * Without these two names no CMOV row was emitted at all (`48 0f 4c c1` had no form). */
    if (!strcmp(f, "MRMSrcRegCC"))     return F_MRMSRCREG;
    if (!strcmp(f, "MRMSrcMemCC"))     return F_MRMSRCMEM;
    if (!strcmp(f, "MRMDestRegCC"))    return F_MRMDESTREG;
    if (!strcmp(f, "MRMDestMemCC"))    return F_MRMDESTMEM;
    /* MRM0m..MRM7m (32..39) and MRM0r..MRM7r (48..55) */
    if (f[0] == 'M' && f[1] == 'R' && f[2] == 'M' && f[3] >= '0' && f[3] <= '7') {
        if (f[4] == 'm' && f[5] == 0) return F_REGMEM0;
        if (f[4] == 'r' && f[5] == 0) return F_REGREG0;
    }
    /* THE NAMED MRM_<HEX> FORMATS. The group digit is written in HEX in the name, and the trailing
     * letter says whether the ModR/M's rm field addresses memory:
     *
     *     def MRM7X : Format<62>;      ->  /7, "X" = neither (no rm operand at all)
     *     def MRM_F9 : Format<121>;    ->  /F9
     *     def MRM_D0 : Format<80>;     ->  /D0
     *
     * `SFENCE : I<0xAE, MRM7X, ...>` is `0f ae f8`, `RDTSCP : I<0x01, MRM_F9, ...>` is `0f 01 f9`,
     * `XGETBV : I<0x01, MRM_D0, ...>` is `0f 01 d0`, `CLAC : I<0x01, MRM_CA, ...>` is `0f 01 ca`.
     * All of these are ordinary instructions clang emits (SFENCE/LFENCE/MFENCE on any atomic, RDTSCP
     * and XGETBV in runtime code), and every one was refused for an unknown format, so none of them
     * had a row. The X form carries a ModR/M with a fixed reg field and NO rm operand, which is
     * F_REGMEM0 with the group set -- one group byte and nothing else. */
    if (!strncmp(f, "MRM_", 4)) {
        const char *hx = f + 4;
        long g = -1;
        if (hx[0] && hx[1] && !hx[2]) {
            g = (long)strtol((char[]){(char)hx[0], (char)hx[1], 0}, NULL, 16);
        }
        if (g >= 0) {
            (void)g;                /* the caller reads the digit out of the name via mrm_group_of() */
            return F_REGMEM0;
        }
    }
    if (f[0] == 'M' && f[1] == 'R' && f[2] == 'M' && (f[3] >= '0' && f[3] <= '9' || (f[3] >= 'A' && f[3] <= 'F'))
        && (f[4] == 'X' || f[4] == 'x') && f[5] == 0)
        return F_REGMEM0;
    return 0;
}

/* THE /DIGIT OF A NAMED MRM_<HEX> FORMAT. `MRM7X` is /7, `MRM_F9` is /0xF9, `MRM_D0` is /0xD0.
 * Returns -1 when the format is not one of these. */
static int mrm_group_of(const char *f) {
    if (f[0] == 'M' && f[1] == 'R' && f[2] == 'M') {
        if (f[3] == '_') {
            char t[3] = { f[4], f[5], 0 };
            if (t[0] && t[1] && !f[6]) return (int)strtol(t, NULL, 16);
            return -1;
        }
        if (f[4] == 'X' || f[4] == 'x') {
            char t[2] = { f[3], 0 };
            return (int)strtol(t, NULL, 16);
        }
    }
    return -1;
}

/* ---- an operand class from the table -> a kind my rows can describe ----
 *
 * The integer tables use 80 distinct operand spellings. They fall into six families and the
 * mapping is mechanical:
 *
 *   GR8/GR16/GR32/GR64 and their _NOREX variants    -> a register of that width
 *   t.RegClass                                      -> the multiclass's type parameter, already
 *                                                      substituted to a GR* by the time it is seen
 *   t.MemOperand, iNNmem, anymem, opaquemem,        -> a memory operand
 *   iNmem_NOREX, i512mem_GRxx, ptr_rc_tailcall
 *   iNimm, u8imm, iNi8imm, iNu8imm, i64i32imm,      -> an immediate, width from the name
 *   iNimm_brtarget, brtargetN                      -> a relative branch target
 *   ccode                                           -> a condition code
 *   variable_ops, and the system/segment register   -> NOT an operand of the encoding; recorded
 *   classes, SEGMENT_REG, DEBUG_REG, CONTROL_REG       as A_NONE so the row is refused, not
 *                                                      emitted with a missing field
 *
 * The width suffixes are read from the name rather than enumerated, because the tables keep
 * inventing combinations (i64i8imm, i32u8imm, i64i32imm_brtarget) and a hand-written list goes
 * stale silently.
 */
static int operand_kind(const char *t, int *is_group) {
    *is_group = 0;
    if (!t || !*t) return A_NONE;

    /* A `<Type>.<member>` SPELLING IS RESOLVED HERE, NOT ONLY IN THE SUBSTITUTION.
     *
     * subst_text resolves it when the token's type is a binding or a literal name it knows, but the
     * text reaching the operand parser can still carry the member form -- the (ins) list of a class
     * two hops down names `Xi64.Imm8Operand` and the substitution that produced the text did not
     * know `Xi64` at that level. Leaving it made operand_kind see a name it could not classify:
     * `Xi64.Imm8Operand` has a capital I so strstr(t,"imm") failed, and the operand fell through to
     * A_NONE -- which silently dropped the second operand of every `addq $imm8, %r64`. Resolving it
     * here means the parser is correct regardless of how much substitution ran. */
    {
        const char *dot = strchr(t, '.');
        if (dot) {
            char tn[32]; size_t tl = (size_t)(dot - t);
            if (tl < sizeof tn) {
                memcpy(tn, t, tl); tn[tl] = 0;
                const char *mem = dot + 1;
                TypeInfo *ti = find_typeinfo(tn);
                if (ti) {
                    if (!strcmp(mem, "RegClass") && ti->regclass[0]) return operand_kind(ti->regclass, is_group);
                    if (!strcmp(mem, "MemOperand") && ti->memop[0])   return operand_kind(ti->memop, is_group);
                    if (!strcmp(mem, "ImmOperand") && ti->immop[0])   return operand_kind(ti->immop, is_group);
                    if (!strcmp(mem, "Imm8Operand") && ti->imm8op[0]) return operand_kind(ti->imm8op, is_group);
                }
                return A_NONE;   /* a member this table cannot express: refuse, do not guess */
            }
        }
    }

    /* a vector register: VR128 (xmm) / VR256 (ymm) / VR512 (zmm), and the FR16/FR32/FR64 forms
     * where the same registers are named by scalar size. All are the same ModR/M.reg window; the
     * register FILE is what the name says, and only VR128 is reachable without a VEX/EVEX prefix --
     * which is the form the catalog targets, so the narrower names map here too. */
    if (!strncmp(t, "VR", 2) || !strncmp(t, "FR", 2) || !strcmp(t, "RST"))
        return A_XMM;

    /* a register: GR8 / GR16 / GR32 / GR64, with any _NOREX / _REX suffix.
     * THE WIDTH IS ONE OR TWO DIGITS: GR8 has a single digit and GR64 two, so reading t[2] and
     * t[3] unconditionally mis-parsed every 8-bit register class as "80" (not a match) and refused
     * GR8, GR8_NOREX and every byte form built on them. */
    if (!strncmp(t, "GR", 2) && t[2] >= '1' && t[2] <= '9') {
        int w = t[2] - '0';
        if (t[3] >= '0' && t[3] <= '9') w = w * 10 + (t[3] - '0');
        /* a suffix such as _NOREX or _REX must not be read as more digits -- t[3] is '_' for
         * GR8_NOREX, so the single-digit path is taken, which is correct. */
        if (w == 8)  return A_REG8;
        if (w == 16) return A_REG16;
        if (w == 32) return A_REG32;
        if (w == 64) return A_REG64;
    }
    /* GR16orGR32orGR64 -- the name says which; 64 is the widest the encoding can be */
    if (!strncmp(t, "GR16orGR", 8)) return A_REG64;
    /* the multiclass type parameter, substituted before this point; if it survived, the row is
     * refused rather than guessed */
    /* t.RegClass is the multiclass's X86TypeInfo parameter's register class. It appears literally
     * in the operand list and its WIDTH is decided by the type argument the def passed (Xi64 ->
     * GR64). The caller substitutes it before this point; if it is still here the row is refused,
     * because guessing a width would produce a row that encodes 64-bit forms as 32-bit. */
    if (!strcmp(t, "t.RegClass") || !strcmp(t, "RC")) return A_NONE;

    /* a register NUMBER rather than a register: used by the string instructions (movs/stos), where
     * it is an implicit operand and not encoded as a field */
    if (!strncmp(t, "srcidx", 6) || !strncmp(t, "dstidx", 6)) return A_NONE;

    /* a memory operand: every spelling contains "mem" */
    if (strstr(t, "mem") || !strcmp(t, "t.MemOperand") || !strcmp(t, "ptr_rc_tailcall"))
        return A_MEM;

    /* a condition code: jcc / setcc / cmovcc */
    if (!strcmp(t, "ccode")) return A_CCODE;

    /* a branch target, before the general immediate test (its name contains "imm") */
    if (strstr(t, "brtarget") || strstr(t, "PCRel")) {
        if (strstr(t, "8"))  return A_REL8;
        return A_REL32;
    }

    /* an immediate. The width is the FIRST number in the name: i32imm, u8imm, i64i32imm (the
     * encoded width is the LAST, not the first -- i64i32imm is a 32-bit immediate sign-extended to
     * 64, so the encoding is 32 bits). The table writes the ENCODED width last, so take the LAST
     * number. */
    /* `variable_ops` IS A MARKER, NOT AN ENCODED OPERAND. RET64 is
     *     I<0xC3, RawFrm, (outs), (ins variable_ops), "ret{q}", []>
     * -- the variable_ops list is empty for the encoding and the instruction takes no field.
     * Returning A_NONE refused the row, so `ret` -- which clang emits 11 times in a small function
     * -- had no row at all. */
    if (!strcmp(t, "variable_ops")) return A_NONE;   /* skipped by parse_operands */
    if (strstr(t, "imm")) {
        int last = -1;
        for (const char *z = t; *z; z++)
            if (*z >= '0' && *z <= '9') {
                int v = 0;
                while (*z >= '0' && *z <= '9') { v = v * 10 + (*z - '0'); z++; }
                if (v == 8 || v == 16 || v == 32 || v == 64) last = v;
                z--;
            }
        if (last == 8)  return A_IMM8;
        if (last == 16) return A_IMM16;
        if (last == 32) return A_IMM32;
        if (last == 64) return A_IMM64;
        return A_IMM32;
    }

    /* a segment/debug/control register or anything else this parser does not model */
    return A_NONE;
}


/* PARSE (outs ...), (ins ...) INTO OPERAND KINDS. Used by both the direct path and the
 * multiclass path, so a row's operands are derived the same way wherever it came from. */
static int parse_operands_tie(const char *seg, int *ops, int *no_out, const char *tie);
static int parse_operands(const char *seg, int *ops, int *no_out) {
    return parse_operands_tie(seg, ops, no_out, NULL);
}

/* THE TIE TEXT ARRIVES SEPARATELY FROM THE OPERAND TEXT, so the parser takes it as an argument
 * rather than trying to find it inside `seg` (it lives on a different line of the def, or on the
 * class, and a row's operands and its Constraints are never adjacent in these tables). */
static int parse_operands_tie(const char *seg, int *ops, int *no_out, const char *tie) {
    int outs[4], nouts = 0;
    int ins[4],  nins  = 0;
    char *o1 = strstr(seg, "(outs");
    char *i1 = strstr(seg, "(ins");
    for (int z = 0; z < 2; z++) {
        char *kw = z ? i1 : o1;
        if (!kw) continue;
        char *s = kw;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '(') s++;
        while (*s && *s != ' ' && *s != '\t' && *s != '(' && *s != ')') s++;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '(') s++;
        char *e = s; int gd = 0;
        while (*e) {
            if (*e == '(') gd++;
            else if (*e == ')') { if (gd == 0) break; gd--; }
            else if (*e == '>' || *e == ';' || *e == '\n') break;
            e++;
        }
        int only_ws = 1;
        for (char *w = s; w < e; w++)
            if (*w != ' ' && *w != '\t' && *w != '\n') { only_ws = 0; break; }
        if (only_ws) continue;
        char tmp[400]; int tl = 0;
        for (char *t = s; t < e && tl < 399; t++) tmp[tl++] = *t;
        tmp[tl] = 0;
        char *save = tmp, *tok;
        while ((tok = strsep(&save, ",")) != NULL) {
            while (*tok == ' ' || *tok == '\t') tok++;
            char *end = tok + strlen(tok);
            while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
            if (!*tok) continue;
            char *colon = strchr(tok, ':');
            if (colon) *colon = 0;
            int isg = 0;
            /* variable_ops is a marker: skip it entirely rather than recording an operand */
            if (!strcmp(tok, "variable_ops")) continue;
            int k = operand_kind(tok, &isg);
            if (z == 0) { if (nouts < 4) outs[nouts++] = k; }
            else        { if (nins  < 4) ins[nins++]  = k; }
        }
    }
    /* THE ENCODED OPERANDS ARE EVERY OUT PLUS THE LAST IN.
     *
     * The table lists TIED operands as real ins -- BSF64rr is
     *     RI<0xBC, MRMSrcReg, (outs GR64:$dst), (ins GR64:$fallback, GR64:$src), ...>
     * three operands for two encoded positions; $fallback mirrors $dst and is not a field. XADD64rr
     * is (outs GR64:$dst1, GR64:$dst2), (ins GR64:$src): two outs and one ins. Keeping the fallback
     * made the row claim a three-operand encoding whose third field does not exist, so it wrote the
     * wrong registers and the round-trip check caught it. */
    /* ALL OPERANDS, IN THE TABLE'S OWN ORDER. Trimming to "every out plus the last in" was an
     * attempt to model LLVM's tied operands and it made the round-trip WORSE (80 failures against
     * 59): the tied operand is real for a two-address form, and dropping it shifted the field
     * assignment for every XCHG/XADD/BSF/BSR/BT row. Measured, not assumed -- reverted. */
    /* THE ENCODED OPERANDS ARE EVERY OUT, PLUS THE INS THAT ARE NOT TIED TO AN OUT.
     *
     * A tie exists when the instruction writes a register AND reads that same register as its first
     * input -- the two-address form. The tables state it positionally, and the count decides:
     *
     *   MOV64rr  : RI<0x89, MRMDestReg, (outs GR64:$dst), (ins GR64:$src)>
     *              1 out, 1 in  -> BOTH are encoded (the in is the source, not a tie)
     *   ADD64rr  : (outs GR64:$dst), (ins GR64:$src1, GR64:$src2)
     *              1 out, 2 ins -> $src1 ties to $dst, so the encoded pair is [$dst, $src2]
     *   TEST64rr : (outs), (ins GR64:$src1, GR64:$src2)
     *              0 outs, 2 ins -> nothing to tie to, BOTH encoded
     *   XADD64rr : (outs GR64:$dst1, GR64:$dst2), (ins GR64:$src)
     *              2 outs, 1 in -> the pair of outs IS the encoding, the in ties to the first
     *
     * So: an (ins) entry is a tie only when the number of OUTS is at least as large as the number of
     * INS -- which is exactly when the ins are re-stating the destinations. Otherwise every in is a
     * real field. This is a count rule, not a position rule, because MOV64rr and ADD64rr differ only
     * in how many ins they declare. */
    int no = 0;
    for (int i = 0; i < nouts && no < MAXOPS; i++) ops[no++] = outs[i];
    /* THE TIE IS THE FIRST IN, AND IT ONLY EXISTS WHEN THERE IS AN OUT TO TIE TO.
     *
     *   MOV64rr  (outs GR64:$dst), (ins GR64:$src)         1 out, 1 in  -> [$dst, $src]
     *   ADD64rr  (outs GR64:$dst), (ins $src1, $src2)      1 out, 2 in  -> [$dst, $src2]  ($src1 ties)
     *   TEST64rr (outs),           (ins $src1, $src2)      0 out, 2 in  -> [$src1, $src2] (no out)
     *   JCC_1    (outs),           (ins brtarget8:$dst, ccode:$cond)
     *                                                      0 out, 2 in  -> BOTH encoded
     *   XADD64rr (outs $dst1, $dst2), (ins GR64:$src)      2 out, 1 in  -> the two outs
     *
     * The earlier count comparison (`nins > nouts`) dropped the first in for JCC_1 and TEST64rr,
     * which have no outs at all -- so every conditional branch lost its displacement and no jcc
     * could decode. With an out present, a tie is exactly the case where there are more ins than
     * outs; with none, nothing can be tied and every in is a real field. */
    /* THE TIE IS STATED BY THE TABLE, NOT INFERRED FROM AN OPERAND COUNT.
     *
     * The tables say exactly which in re-states an out:
     *
     *     defvar tie_dst_src1 = "$src1 = $dst";
     *     class BinOpRR_RF<...> : ... { string Constraints = !if(!eq(ndd, 0), tie_dst_src1, ""); }
     *     let Constraints = "$src1 = $dst" in { ... }
     *
     * So `$src1` is a tie only where that text appears -- the count rule that stood here was a
     * guess that happened to fit MOV/ADD/TEST and got `imulq $imm32,%rcx,%rax` wrong: 1 out and 2
     * ins, so it dropped `$src1`, and the row described a two-operand encoding over `48 69 c1 ...`.
     * A count cannot distinguish "the first in is a tie" from "the first in is a real field", and
     * the table already distinguishes them in words.
     *
     * `tie` is the constraint text for this row ("" when the row states none), and a tie exists only
     * when it names an in ($src1 = $dst => the in called $src1 is not a field) AND there is an out to
     * tie to. */
    int skip_first_in = 0;
    if (nouts > 0 && tie && strstr(tie, "$src1")) skip_first_in = 1;
    /* AN ASM STRING THAT NAMES $dst BUT NOT $src1 IS A TIE STATED IN THE OPERAND TEXT.
     *
     *     def rr : MMXI<opc, MRMSrcReg, (outs VR64:$dst), (ins VR64:$src1, VR64:$src2),
     *                   OpcodeStr "\t{$src2, $dst|$dst, $src2}", ...>
     *
     * `$src1` appears in the (ins) list and in the pattern, but NOT in the asm string -- because the
     * instruction is two-address and `$src1` IS `$dst`. The encoding is `0f f3 c0` with two operands,
     * so `$src1` is a tie and must not be given a field. Without this the row claimed three operands
     * over a two-operand encoding (`MMX_PSLLQrr` -> `xmm,xmm,xmm`), which is why `psllq %mm1,%mm0`
     * had no matching row. The asm string is the table's own statement of which operands are printed,
     * and an in that is never printed is not an encoded operand of its own. */
    if (!skip_first_in && nouts > 0 && nins > 1) {
        const char *as = strstr(seg, "\"\\t{");
        if (!as) as = strchr(seg, '"');
        if (as) {
            const char *ae = strchr(as + 1, '"');
            if (ae && ae > as) {
                int saw_dst = 0, saw_src1 = 0;
                for (const char *z = as; z + 5 < ae; z++) {
                    if (!strncmp(z, "$src1", 5)) saw_src1 = 1;
                    if (!strncmp(z, "$dst", 4))  saw_dst = 1;
                }
                if (saw_dst && !saw_src1) skip_first_in = 1;
            }
        }
    }
    for (int i = skip_first_in ? 1 : 0; i < nins && no < MAXOPS; i++) ops[no++] = ins[i];
    *no_out = no;
    return no;
}

/* ============================================================ emit ===== */

/* THE ENCODING IS COMPUTED, NOT ASSEMBLED.
 *
 * Every field position follows from the format, so the bytes are arithmetic:
 *
 *   optional 0x66 (16-bit operand size)
 *   optional REX  (0x40 | W<<3 | R<<2 | X<<1 | B)
 *   the opcode byte from the table
 *   optional ModR/M  (mod<<6 | reg<<3 | rm)
 *   optional SIB     (only for a rsp base or an index -- see below)
 *   optional displacement
 *   optional immediate
 *
 * A register operand's field is 3 bits at a fixed position plus the REX bit for its POSITION:
 * reg -> REX.R, rm -> REX.B, index -> REX.X, and a register packed into the opcode -> REX.B.
 * That is the K_REXREG shape the runtime already implements: three low bits at the field, the
 * high bit in the per-operand window.
 */

/* the mod field the current row is being emitted for: 0x00, 0x40 (disp8) or 0x80 (disp32).
 * A memory operand is NOT one encoding: mod=00 has no displacement, mod=01 has a byte and
 * mod=10 four bytes, and all three appear in compiler output. */
static int g_dispmod = 0x80;
/* WHEN SET, the row being emitted is the SIB form of its memory operand. One memory operand yields
 * TWO rows -- [base+disp] and [base+index*scale+disp] -- because they are different encodings of the
 * same instruction and both appear in compiler output: `leaq -0x1(%rdi),%rcx` is 48 8d 4f ff while
 * `leaq (%rdi,%rdi,2),%rax` is 48 8d 04 7f. */
static int g_make_sib = 0;
/* 1 WHEN the row being emitted is the RIP-relative form of its memory operand: rm is fixed to 5 at
 * mod=00, which is not a register but the "no base, disp32 follows" encoding. A separate row because
 * it is a separate addressing mode with a different length from [rbp+disp8]. */
static int g_riprel = 0;
/* 1 WHEN the SIB row being emitted fixes the index at 4 ("no index register"). A SIB with index 4
 * is a real encoding -- it is the only way to write [rsp+disp] and how a base with REX.B is written
 * when no index is wanted -- so it is a row of its own, not a degenerate case of the indexed one. */
static int g_noindex = 0;
/* 1 WHEN the SIB row being emitted is the NO-BASE form: mod=00 with a SIB whose base field is 5,
 * which means "no base, disp32 follows" -- `leal (,%rax,8),%edx` is 8d 14 c5 00 00 00 00. A
 * scaled-index-only access is ordinary compiler output and the indexed pass cannot express it,
 * because there the base is an ordinary register field and 5 is excluded. */
static int g_nobase = 0;
/* WHEN SET, a 32-bit row is emitted WITH the leading REX byte reserved, so a REX-led instruction
 * (44 89 ... , a REX.R/X/B prefix on an otherwise-prefixless form) has a row to match. The same
 * form is emitted once with the byte and once without, because both encodings exist. */
static int g_rex_optional = 0;
/* WHEN SET, the row being emitted is the SIB form of its memory operand: rm=4 marks the SIB byte and
 * the scale/index/base live in it. One memory operand therefore yields TWO rows -- the plain
 * [base+disp] form and the [base+index*scale+disp] form -- because the two are different encodings
 * and both occur in compiler output (leaq -0x1(%rdi),%rcx vs leaq (%rdi,%rdi,2),%rax). */
static int g_emit_skip_none, g_emit_skip_len, g_emit_ok;

/* the byte offset of a field, given how many optional fields precede it */
typedef struct {
    int has_66, has_rex, has_modrm, has_sib, has_disp;
    int has_pfx;            /* the mandatory SSE prefix (0x66/0xF2/0xF3) is present */
    uint8_t pfxbyte;        /* which one it is */
    int len_extra_disp;     /* a no-base SIB's mandatory disp32, counted into len by the caller */
    int opmap;
    int opcode_at, modrm_at, disp_at, imm_at;
    int len;
} Layout;

static void layout_of(const Row *rw, int fk, Layout *L) {
    memset(L, 0, sizeof *L);
    int at = 0;
    L->has_66 = (rw->cls & C_OPSIZE16) != 0;
    /* THE MANDATORY SSE PREFIX COMES FROM THE ROW'S OWN OpPrefix ATTRIBUTE, NOT FROM THE CLASS NAME.
     *
     * X86InstrFormats.td states the mapping outright:
     *
     *     class Prefix<bits<3> val> { bits<3> Value = val; }
     *     def NoPrfx : Prefix<0>;   def PD : Prefix<1>;   def XS : Prefix<2>;
     *     def XD     : Prefix<3>;   def PS : Prefix<4>;   // "similar to NoPrfx"
     *
     * and `Prefix OpPrefix = !if(!eq(OpEnc, EncNormal), NoPrfx, PS)` is what a def inherits. The
     * prefix byte each one means is fixed by the ISA: PD -> 0x66, XS -> 0xF3, XD -> 0xF2, and NoPrfx
     * / PS -> none (PS is the "no prefix, but the disassembler must know the PD/XS/XD forms exist"
     * marker -- it is NOT a 0x66). Inferring the prefix from the class NAME got this wrong in exactly
     * that place: MOVAPSrm shipped as `66 0f 28` (a 7-byte row) because PSI looked like it wanted
     * 0x66, when the real encoding is a bare `0f 28`. That wrong extra byte let PALIGNRrmi -- whose
     * own template is the shorter `0f 00` -- tie on length and win, so `movaps (%rdi),%xmm0` was
     * reported as a palignr.
     *
     * The attribute is read from the row's text and stored, so the prefix is a FACT FROM THE TABLE
     * rather than a guess about a name. */
    if (rw->pfx == 1)      L->has_pfx = 1, L->pfxbyte = 0x66;
    else if (rw->pfx == 2) L->has_pfx = 1, L->pfxbyte = 0xF3;
    else if (rw->pfx == 3) L->has_pfx = 1, L->pfxbyte = 0xF2;
    /* a mandatory 0x66 that is not an operand-size prefix still adds a byte to the length */
    if (L->has_pfx) at += 1;
    /* A REX BYTE IS PRESENT ONLY WHEN THE ENCODING NEEDS ONE.
     *
     * It was forced on for every row with a register operand, on the theory that the byte must exist
     * for the runtime's K_REXREG window to write into. That is true only for a 64-bit form:
     *
     *     ADD32rr  ->  01 c0          (no prefix)
     *     ADD64rr  ->  48 01 c0       (REX.W)
     *
     * Emitting 0x40 on a 32-bit row made it 40 01 c0 for an encoding that is really 01 c0, so every
     * 32-bit arithmetic and move form failed to decode -- which is why clang's own 32-bit output
     * (testl, movl, xorl, addl, cmpl, subl, leal) had no matching row at all.
     *
     * An 8-bit form that uses spl/bpl/sil/dil ALSO needs the byte, but that is a property of the
     * register VALUE (4..7 vs the classic ah/ch/dh/bh), which the runtime's K_REXREG window handles
     * by being allowed to write the byte when the value needs it -- the row must therefore leave
     * the byte out and let the encoder decide. */
    /* REX IS PRESENT WHEN THE ENCODING ALLOWS ONE.
     *
     * A 64-bit form NEEDS REX.W and a 32-bit form allows a bare REX (0x40) to carry REX.R/X/B for
     * extended registers. Declaring the byte absent means the row's byte 0 is compared against the
     * REX prefix itself, so `44 0f af c1` (imul with REX.R) matched a 2-byte CMOV template on its
     * first two bytes. Every non-8-bit integer row therefore reserves the byte, and the 8-bit forms
     * that must NOT have one (ah/bh/ch/dh) are marked by Xi8's IgnoresW path -- they carry no
     * extended register, so their K_REXREG window has hbit 0 and a REX-led word cannot match them
     * because the byte is not in their template either.
     *
     * The exception is a row with no register operand at all: a REX there is a different instruction's
     * prefix and reserving the byte would make the row unable to match its own encoding. */
    /* THE REX BYTE IS A FIELD WHEN THE ROW NEEDS IT, AND THE 8-BIT FORMS THAT CANNOT HAVE ONE
     * SIMPLY DO NOT RESERVE IT. A 32-bit row's encoding is `89 d1` and a REX-led form of the same
     * instruction is `44 89 ...` -- TWO encodings, so the catalog carries two rows per form, emitted
     * by varying g_rex_optional. This flag is set by the emitter for the second pass. */
    /* THE REX-LED VARIANT ONLY MAKES SENSE FOR A ONE-BYTE-OPCODE ROW.
     *
     * A 0F-escaped form with a REX prefix is `44 0f 4c c1`, which the row for the plain form already
     * covers once its escape byte is in the template -- adding a second pass with the REX reserved
     * shifted the layout by one and produced rows whose OPCODE BYTE WAS MASKED OUT (len 2, template
     * `40 00`, mask `0f 3f`), so they matched any `44 xx` pair and stole the decode from the real
     * instruction. Gating the variant on opmap == 0 keeps the two encodings that actually differ. */
    {
        /* A REGISTER PACKED INTO THE OPCODE IS EXPRESSIBLE WITHOUT A REX BYTE.
         *
         * The high bit of such a register needs REX.B, but only when the register IS 8..15 -- and that
         * is a property of the VALUE, not of the row. Reserving the byte unconditionally made
         * `movl $0x21,%ecx` (b9 21 00 00 00) ship a leading 0x40 that the encoding does not have,
         * because the row's byte 0 was the REX slot instead of 0xb8+ecx. The runtime's K_REXREG
         * window can WRITE the byte when the value needs it (the byte is present in the template when
         * it is reserved), so the correct rule is: reserve it only for a form that REQUIRES REX.W. */
        int op_in_opcode = 0;
        for (int i = 0; i < rw->nops; i++) {
            int k = rw->ops[i];
            if ((k == A_REG8 || k == A_REG16 || k == A_REG32 || k == A_REG64)
                && (fk == F_ADDREG || fk == F_RAWSRC || fk == F_RAWDST || fk == F_RAWDSTSRC))
                op_in_opcode = 1;
        }
        (void)op_in_opcode;
        L->has_rex = (rw->cls & C_REXW) != 0 || g_rex_optional;
    }
    if (L->has_66) at += 1;
    if (L->has_rex) at += 1;
    /* LAYOUT: [0x66] [REX] [escape bytes] opcode [ModR/M] [disp] [imm]
     *
     * THE ESCAPE GOES BEFORE THE OPCODE FIELD, AND THE FIELD STAYS ONE BYTE. The first attempt put
     * the escape AT opcode_at with the opcode after it while advancing `at` by only one -- so
     * modrm_at landed on the opcode's second byte. The symptom was precise: JCC_4 matched the bytes
     * "01 c1" (the tail of `addq %r8,%r9`) at every offset of the gate program, and the real
     * instructions were refused. The escape is a CONSTANT for the row, so it is written into the
     * template ahead of the field, and only the bytes AFTER the escape move. */
    /* THE ESCAPE BYTES ARE PART OF THE LENGTH.
     *
     * The opcode field can be one, two or three bytes: 0F 1F, 0F 8x, 0F 38 xx, 0F 3A xx. `at` was
     * advanced by one for every row, so each 0F-escaped row claimed to be one byte shorter than it
     * is -- and isa_decode returns the row's length as the instruction length, so a movzx (`0f b6`)
     * was reported as a 1-byte instruction whose best match was BSWAP32r. That single off-by-one
     * made the 0F family unusable: movzx, movsx, setcc, cmovcc, near jcc, bswap.
     *
     * The escape byte is written into the template BEFORE the field (see the write below), so the
     * field offset is opcode_at and the LENGTH has to include the escape. */
    L->opmap = rw->opmap;
    /* THE OPCODE FIELD IS 1 + opmap BYTES WIDE -- EXCEPT THAT opmap=3 IS `0F 3A xx`, WHICH IS THREE.
     *
     *   opmap=0 -> [opcode]           1 byte
     *   opmap=1 -> [0F][opcode]       2 bytes
     *   opmap=2 -> [0F][38][opcode]   3 bytes
     *   opmap=3 -> [0F][3A][opcode]   3 bytes, the SAME length as opmap=2
     *
     * The 0F 38 and 0F 3A forms are both two-byte escapes followed by a one-byte opcode, so `1 + opmap`
     * is right for 0/1/2 and ONE TOO MANY for 3. `PINSRDrri : SS4AIi8<0x22, MRMSrcReg, ...>` came out
     * seven bytes long with the ModR/M written at index 5 and a hole at 4 (`66 0f 3a 22 00 c0`), so
     * `66 0f 3a 22 c7 01` -- a real pinsrd -- matched nothing. The escape length is `opmap < 3 ? opmap
     * : 2`. */
    /* THE ESCAPE SITS AT opcode_at, BEFORE THE OPCODE BYTE.
     *
     * `at` was advanced by one regardless, so for every escaped row the ModR/M offset came out one
     * (or two) bytes early and landed INSIDE the opcode: the mod write masks the byte with 0x3f, so
     * `0f b6` (movzx) came out `0f 36` -- and no real movzx, movsx, setcc, cmovcc or near jcc could
     * decode. */
    L->opcode_at = at;
    at += 1 + (rw->opmap < 3 ? rw->opmap : 2);

    int has_reg_operand = 0, mem_ops = 0;
    for (int i = 0; i < rw->nops; i++) {
        int k = rw->ops[i];
        if (k == A_REG8 || k == A_REG16 || k == A_REG32 || k == A_REG64 || k == A_XMM)
            has_reg_operand = 1;
        if (k == A_MEM) mem_ops++;
    }
    /* a ModR/M byte exists for every MRM* format; a register form needs it even with no memory */
    L->has_modrm = (fk != F_RAW && fk != F_ADDREG && fk != F_ADDCC
                    && fk != F_RAWMOFFS && fk != F_RAW_IMM8 && fk != F_RAW_IMM16
                    && fk != F_RAWSRC && fk != F_RAWDST && fk != F_RAWDSTSRC
                    && (has_reg_operand || mem_ops
                        /* A NAMED MRM_<HEX> / MRM<N>X FORMAT ALWAYS HAS A ModR/M AND NO OPERANDS.
                         * The byte IS the instruction's operand -- `sfence` is `0f ae f8`, `rdtscp`
                         * is `0f 01 f9` -- so requiring a named operand withheld the byte and the row
                         * came out two bytes long, matching nothing. It is recognised by its group,
                         * which the caller has already resolved out of the format name. */
                        || (fk == F_REGMEM0 && rw->nops == 0 && rw->group >= 0)));
    if (L->has_modrm) { L->modrm_at = at; at += 1; }
    /* THE SIB BYTE IS PART OF THE MEMORY FORM, so a plain (base) memory operand as emitted here
     * does not need one: disp32(base) with base in 0..3,6,7,8..15 encodes as mod=10, rm=base and
     * no SIB. mod=10 always carries a 4-byte displacement, which is why one memory shape covers
     * every base -- and why no separate no-displacement row is needed. */
    L->has_disp = (mem_ops > 0);
    /* A NO-BASE SIB ALWAYS CARRIES A DISP32 even though its mod is 00: `leal (,%rax,8),%edx` is
     * 8d 14 c5 00 00 00 00, and that trailing dword is part of the encoding. mod=00 normally means
     * "no displacement", so the length has to be told otherwise here or the row comes out four bytes
     * short and matches nothing but the first three bytes of real code. */
    if (g_nobase && mem_ops > 0) {
        /* ONE disp32, counted ONCE: the field appended below is that same displacement, so the
         * length must not also grow by the shift loop. Setting disp_at to the byte just past the
         * SIB and letting `len` be computed from it is what keeps the two in step. */
        L->disp_at = at + 1;          /* the SIB byte is inserted at modrm_at + 1 */
        at += 5;                      /* SIB byte + disp32 */
        L->len_extra_disp = 4;
    }
    if (L->has_disp && !g_nobase) {
        /* THE DISPLACEMENT IS 0, 1 OR 4 BYTES ACCORDING TO mod. */
        L->disp_at = at;
        at += (g_dispmod == 0x40) ? 1 : (g_dispmod == 0x80 ? 4 : 0);
    }
    /* THE TRAILING IMMEDIATE'S WIDTH COMES FROM ITS OPERAND, AND THE CLASS IS A FALLBACK.
     *
     * RETI32 is `I<0xC2, RawFrm, (outs), (ins i16imm:$imm), ...>`: the class bits say nothing about
     * the immediate, the operand does. Reading only the class left the row with an imm16 operand and
     * no room for it, so the row's length was one byte short and `ret $0x8` (c2 08 00) matched as a
     * one-byte instruction. The operand kind is authoritative; the class is what to use when a def
     * names an immediate in its class but not in its operands. */
    {
        int iw = 0;
        for (int i = 0; i < rw->nops; i++) {
            if (rw->ops[i] == A_IMM8)  iw = 1;
            else if (rw->ops[i] == A_IMM16) iw = 2;
            else if (rw->ops[i] == A_IMM32) iw = 4;
            else if (rw->ops[i] == A_IMM64) iw = 8;
        }
        if (iw) {
            L->imm_at = at;
            at += iw;
        } else if (rw->cls & (C_IMM8 | C_REL8))  { L->imm_at = at; at += 1; }
        else if (rw->cls & C_IMM16)       { L->imm_at = at; at += 2; }
        else if (rw->cls & (C_IMM32 | C_REL32)) { L->imm_at = at; at += 4; }
        else if (rw->cls & C_IMM64)       { L->imm_at = at; at += 8; }
    }
    L->len = at;
}

/* emit one row as an IsaForm initializer */
static void emit_row_printf(Row *rw, int fk, int op_index_shift);

static void emit_row_printf(Row *rw, int fk, int op_index_shift) {
    /* WORK ON A COPY. emit_row_printf appends operands (the SIB index/scale and the displacement are
     * fields the row's author never wrote), and it is called SEVERAL TIMES per row -- once per
     * displacement shape and once per SIB/plain form. Mutating the caller's Row therefore leaked the
     * operands of one emission into the next: the plain [base+disp8] row came out claiming four
     * operands ("r64,mem,r64,scale") over a two-operand encoding, and every memory form whose
     * displacement was not zero failed to decode. */
    Row copy = *rw;
    rw = &copy;
    /* A ROW WITH AN OPERAND THIS PARSER DID NOT UNDERSTAND IS NOT EMITTED. Emitting it with the
     * operand missing produces a row that matches words and encodes them wrong -- the failure mode
     * the catalog exists to avoid. Reported by name instead. */
    for (int i = 0; i < rw->nops; i++)
        if (rw->ops[i] == A_NONE) {
            g_emit_skip_none++;
            if (getenv("X86TD_SKIPN") && g_emit_skip_none < 4)
                fprintf(stderr, "  [skipn] %s nops=%d vals=%d,%d,%d,%d\n", rw->name, rw->nops,
                        rw->ops[0], rw->ops[1], rw->ops[2], rw->ops[3]);
            if (getenv("X86TD_SKIP") && g_emit_skip_none < 8)
                fprintf(stderr, "  !! skipped %s: nops=%d ops=%d,%d,%d,%d\n", rw->name, rw->nops,
                        rw->ops[0], rw->ops[1], rw->ops[2], rw->ops[3]);
            return;
        }
    Layout L;
    layout_of(rw, fk, &L);
    if (L.len <= 0 || L.len > 15) {
        g_emit_skip_len++;
        if (getenv("X86TD_LEN") && g_emit_skip_len < 8)
            fprintf(stderr, "  !! bad length %d: %s (fk=%d nops=%d ops=%d,%d,%d)\n",
                    L.len, rw->name, fk, rw->nops, rw->ops[0], rw->ops[1], rw->ops[2]);
        return;
    }

    uint8_t bytes[MAXB];
    memset(bytes, 0, sizeof bytes);
    /* THE PREFIX OFFSETS MUST ACCOUNT FOR THE OPCODE'S LENGTH. The opcode field can be one, two or
     * three bytes (a 0x0F escape, or 0x0F 0x38 / 0x0F 0x3A); computing the 0x66 and REX positions
     * as "opcode_at - 2" and "opcode_at - 1" assumed one byte, so on a two-byte-opcode form they
     * landed INSIDE the opcode and overwrote it -- NOOPW came out {00,0F,80} instead of
     * {66,0F,1F}. */
    /* LAYOUT: [0x66] [REX] [escape] opcode [ModR/M] [disp] [imm]
     * `opcode_at` is the ESCAPE's offset when there is one, so the prefix positions follow from
     * opcode_at alone -- subtracting opmap as well double-counted it and put the REX byte inside the
     * escape, which turned `0f b6` (movzx) into `00 0f 36`. */
    if (L.has_rex) bytes[L.opcode_at - 1] = 0x40 | ((rw->cls & C_REXW) ? 0x08 : 0);
    if (L.has_66)  bytes[L.opcode_at - (L.has_rex ? 2 : 1)] = 0x66;
    /* THE MANDATORY PREFIX, at byte 0 (nothing optional precedes it: a mandatory-prefix form has no
     * REX.W and no 0x66 of its own, which is what makes byte 0 the right place). */
    if (L.has_pfx) bytes[0] = L.pfxbyte;
    if (rw->opmap == 1) { bytes[L.opcode_at] = 0x0F; bytes[L.opcode_at + 1] = (uint8_t)rw->opcode; }
    else if (rw->opmap >= 2) {
        bytes[L.opcode_at] = 0x0F;
        bytes[L.opcode_at + 1] = (rw->opmap == 2) ? 0x38 : 0x3A;
        bytes[L.opcode_at + 2] = (uint8_t)rw->opcode;
    } else bytes[L.opcode_at] = (uint8_t)rw->opcode;

    /* FIELD SLOTS. Every operand that is a register or a memory base gets a K_REXREG window:
     * vbyte/vbit/vwid for the low 3 bits, hbyte/hbit for the per-operand REX bit.
     *   reg field  -> ModR/M bits 3..5, REX.R
     *   rm field   -> ModR/M bits 0..2, REX.B
     *   opcode reg -> opcode bits 0..2,  REX.B
     */
    int vb[MAXOPS], vbit[MAXOPS], vwid[MAXOPS], vk[MAXOPS];
    int hb[MAXOPS], hbit[MAXOPS];
    for (int i = 0; i < MAXOPS; i++) {
        vb[i] = vbit[i] = vwid[i] = 0; vk[i] = 0; hb[i] = 0; hbit[i] = 0;
    }

    (void)op_index_shift;
    if (L.has_modrm) {
        int regfield_op = -1, rmfield_op = -1;
        /* WHERE EACH OPERAND GOES, TAKEN FROM THE ENCODINGS THEMSELVES.
         *
         *   movl %esi,%ecx        = 89 f1        reg=esi(src)  rm=ecx(dst)   MRMDestReg
         *   movl (%rdi),%ecx      = 8b 0f        reg=ecx(dst)  rm=(rdi)      MRMSrcMem
         *   movl %ecx,(%rdi)      = 89 0f        reg=ecx(src)  rm=(rdi)      MRMDestMem
         *   leaq 0x1(%rdi),%rcx   = 48 8d 4f 01  reg=rcx(dst)  rm=(rdi)      MRMSrcMem
         *
         * The format names say where the RM operand is, not where the destination is, so the rule is
         * uniform: MRMDest* puts operand 0 in rm, MRMSrc* puts operand 0 in reg.
         *
         * Both were inverted before, which is why `movl %esi,%ecx` decoded with its operands swapped
         * and why every memory form failed to match: for `movzbl (%rsi,%rax),%ecx` (0f b6 0c 06) the
         * destination ecx sits in the reg field (0x0c -> reg=1) but no field covered those bits, so
         * the row compared them against the template's 0 and demanded reg=0 -- refusing every form
         * that did not name eax. That is the whole movzx/movsx/lea/mov-memory family. */
        switch (fk) {
            case F_MRMDESTREG: case F_MRMDESTMEM:
                if (rw->nops > 1) { rmfield_op = 0; regfield_op = 1; }
                break;
            case F_MRMSRCREG: case F_MRMSRCMEM:
                if (rw->nops > 1) { regfield_op = 0; rmfield_op = 1; }
                break;
            case F_REGMEM0: case F_REGREG0: case F_MRMX_M: case F_MRMX_R:
                /* the reg field is a fixed /digit or ignored; the operand is rm */
                if (rw->nops > 0) rmfield_op = 0;
                break;
            default: break;
        }
        if (regfield_op >= 0) {
            vb[regfield_op] = L.modrm_at; vbit[regfield_op] = 3; vwid[regfield_op] = 3;
            vk[regfield_op] = 18;                       /* K_REXREG */
            /* THE REX WINDOW MUST BE ABSENT, NOT ZERO, WHEN THE ROW HAS NO REX BYTE.
             *
             * isa_fieldmask masks bit (hbyte*8 + hbit - 1) for every K_REXREG operand whose hbit is
             * non-zero. Emitting hbit=1 with hbyte=0 for a row without a REX byte therefore masked
             * BIT 0 OF BYTE 0 -- the opcode's low bit. That made the 8-bit ADD (template 0x00)
             * match `01 c8` (a 32-bit add), and since isa_decode keeps the first longest match every
             * 32-bit register-register ALU form decoded as its 8-bit twin. hbit=0 means "no window"
             * and is what a row with no REX byte must carry. */
            hb[regfield_op] = L.has_rex ? (L.opcode_at - 1) : 0;
            hbit[regfield_op] = L.has_rex ? 3 : 0;   /* REX.R */
        }
        if (rmfield_op >= 0) {
            vb[rmfield_op] = L.modrm_at; vbit[rmfield_op] = 0; vwid[rmfield_op] = 3;
            vk[rmfield_op] = 18;
            hb[rmfield_op] = L.has_rex ? (L.opcode_at - 1) : 0;
            hbit[rmfield_op] = L.has_rex ? 1 : 0;
            if (rw->ops[rmfield_op] == A_MEM) {
                /* THE MEMORY SHAPE IS SELECTED BY mod, AND ALL THREE ARE REAL ENCODINGS.
                 *
                 *   mod=00  [base]           no displacement
                 *   mod=01  [base+disp8]     1-byte displacement
                 *   mod=10  [base+disp32]    4-byte displacement
                 *   mod=00 with base=101     disp32 with NO base (RIP-relative in 64-bit mode)
                 *
                 * Only mod=10 was emitted, so `leaq -0x1(%rdi),%rcx` (48 8d 4f ff -- mod=01) had no
                 * row to match. The displacement WIDTH is a property of the field, so the layout is
                 * rebuilt per shape by the caller; here the row's own shape is chosen by g_dispmod,
                 * which emit_rows varies over the three values. */
                bytes[L.modrm_at] = (uint8_t)(g_dispmod | (rw->group >= 0 ? ((rw->group & 7) << 3) : 0));
                vk[rmfield_op] = 18;                    /* the base register, 0..15 */
                vbit[rmfield_op] = 0;
                hbit[rmfield_op] = L.has_rex ? 1 : 0;
                /* mod=01 WITH rm=4 IS A SIB FORM, NOT A disp8 FORM.
                 *
                 * A memory row whose rm comes out 4 (rsp) is not encodable as a plain base: rm=100
                 * means "the next byte is a SIB". The row was emitted with mod=01 and no field over
                 * that SIB byte, so the template required it to be 0x00 while the length already
                 * counted it -- and `movzbl 0x1(%rsi,%rax),%ecx` (0f b6 4c 06 01) matched as a 4-byte
                 * instruction whose displacement was read from the SIB byte. A row whose base is
                 * rsp is therefore refused here; the SIB pass emits the encodable one. */
                if (g_dispmod != 0x00) {
                    /* rm=4 marks a SIB, so the base cannot be 4 in a non-SIB memory row */
                    if (!g_make_sib) vk[rmfield_op] = 23;    /* K_REXREG_B4 */
                }
            }
        }
        /* the /digit for a group opcode */
        if (rw->group >= 0 && regfield_op < 0)
            bytes[L.modrm_at] = (uint8_t)(bytes[L.modrm_at] | ((rw->group & 7) << 3));
    }

    /* MOD = 11 FOR A REGISTER rm. The ModR/M byte's top two bits say "both operands are
     * registers" (11), "memory with disp8" (01), "memory with disp32" (10) or "memory, no
     * displacement" (00). The template was built from zeroed bytes and only the memory case set
     * them, so every register form shipped a ModR/M with mod=00 -- claiming a memory operand -- and
     * decoded as its memory twin: `48 89 c1` (mov %rax,%rcx) was reported as "no catalog form"
     * while `48 89 0a` (mem shape) matched. Setting mod here is what makes the register forms
     * decode. */
    {
        int rm_is_mem = 0;
        if (L.has_modrm) {
            /* any memory operand means the rm slot is memory, so mod stays 00/01/10 */
            for (int i = 0; i < rw->nops; i++) if (rw->ops[i] == A_MEM) rm_is_mem = 1;
        }
        if (L.has_modrm && !rm_is_mem) bytes[L.modrm_at] |= 0xC0;
        /* A NAMED MRM_<HEX> FORMAT PUTS THE WHOLE BYTE IN THE MODR/M POSITION.
         *
         * `MRM_F9` is the literal byte 0xF9 (`0f 01 f9`), `MRM_D0` is 0xD0, `MRM_CA` is 0xCA -- the
         * name states the byte itself, not a /digit in the reg field. Masking it to three bits gave
         * `sfence` (MRM7X, /7) the right byte by luck and `rdtscp` (MRM_F9) a wrong one, which is why
         * `0f 01 f9` decoded as MWAITXrrr. The X forms are the /digit plus mod=11,rm=0. */
        if (L.has_modrm) {
            int g = rw->mrm_byte;                           /* the caller resolved this from the fmt */
            if (g > 7) bytes[L.modrm_at] = (uint8_t)g;      /* MRM_<HEX>: the byte IS the value */
        }
        /* THE DISPLACEMENT FIELD GOES ON A MEMORY ROW, AND ITS WIDTH IS THE LAYOUT'S.
         *
         * A memory row's template must carry a field over the displacement bytes even when the
         * displacement is a single byte: leaving them as constant zeros made the row demand
         * displacement 0 and mismatch every real access. */
    }

    /* a register packed into the opcode (AddRegFrm): the low 3 bits of the opcode byte */
    if (!L.has_modrm) {
        for (int i = 0; i < rw->nops; i++) {
            int k = rw->ops[i];
            if (k == A_REG8 || k == A_REG16 || k == A_REG32 || k == A_REG64) {
                /* THE REGISTER SITS IN THE LOW 3 BITS OF THE LAST OPCODE BYTE, so with a 0x0F
                 * escape that is opcode_at + 1 -- the same off-by-one the condition code had. */
                int lastop = L.opcode_at + (rw->opmap == 1 ? 1 : (rw->opmap >= 2 ? 2 : 0));
                vb[i] = lastop; vbit[i] = 0; vwid[i] = 3; vk[i] = 18;
                hb[i] = L.has_rex ? (L.opcode_at - 1) : 0;
                hbit[i] = L.has_rex ? 1 : 0;   /* REX.B */
                /* THE OPCODE'S LOW 3 BITS ARE THE OPERAND'S FIELD, so the template byte keeps a
                 * zero there and the register's low bits go in through the field. */
                rw->opcode &= ~7L;
                bytes[lastop] = (uint8_t)(rw->opcode & ~7L);
                break;
            }
        }
    }

    /* A CONDITION CODE IS THE LOW FOUR BITS OF THE OPCODE (jcc, setcc, cmovcc), which the table
     * marks with the AddCCFrm format. */
    /* THE CONDITION CODE FIELD. jcc/setcc/cmovcc all put the condition in the low nibble of the
     * opcode, whatever format carries them: AddCCFrm for the bare forms and MRMSrcRegCC /
     * MRMSrcMemCC for cmovcc. Keying the rule off the FORMAT alone missed cmov, so the row had a
     * cc operand with no field and could never match. */
    {
        int has_cc = 0;
        for (int i = 0; i < rw->nops; i++) if (rw->ops[i] == A_CCODE) has_cc = 1;
    if (has_cc) {
        for (int i = 0; i < rw->nops; i++)
            if (rw->ops[i] == A_CCODE) {
                /* THE CONDITION CODE IS IN THE LOW NIBBLE OF THE LAST OPCODE BYTE.
                 * JCC_4 is opmap=1: the encoding is 0F 8x, so the nibble that varies with the
                 * condition is at opcode_at + 1, not opcode_at. Writing it at opcode_at put the
                 * field on the 0x0F ESCAPE -- the mask then covered the escape's low nibble and
                 * left 0x8x free, so JCC_4 matched `0f b6` (a movzx) and every near-branch row
                 * swallowed an arbitrary two-byte instruction. That single error accounted for the
                 * gate program's 14 gaps. */
                int lastop = L.opcode_at + (rw->opmap == 1 ? 1 : (rw->opmap >= 2 ? 2 : 0));
                vb[i] = lastop; vbit[i] = 0; vwid[i] = 4; vk[i] = 1;   /* K_IMM */
            }
    }
    }

    /* A TRAILING IMMEDIATE OR DISPLACEMENT IS A K_IMM / K_SIMM FIELD. */
    if (L.imm_at > 0) {
        for (int i = 0; i < rw->nops; i++) {
            int k = rw->ops[i];
            if (k == A_IMM8 || k == A_IMM16 || k == A_IMM32 || k == A_IMM64
                || k == A_REL8 || k == A_REL32) {
                vb[i] = L.imm_at; vbit[i] = 0; vwid[i] = 0;
                if (k == A_IMM8 || k == A_REL8)        vwid[i] = 8;
                else if (k == A_IMM16)                 vwid[i] = 16;
                else if (k == A_IMM32 || k == A_REL32) vwid[i] = 32;
                else                                   vwid[i] = 32;
                vk[i] = (k == A_REL8 || k == A_REL32) ? 6 : 1;   /* K_SIMM for a branch, K_IMM */
                /* A 64-BIT IMMEDIATE IS TWO 32-BIT HALVES, NOT A 32-BIT FIELD.
                 *
                 * `movabsq $imm64, %r10` (49 ba + 8 bytes) carries a full 64-bit value, and the row
                 * was emitting it as ONE K_IMM field of width 32 -- so the encode path's fit check
                 * rejected any value at or above 2^32, and `movabsq $0x1122334455667788` (the gate
                 * program's own test) matched no row at all. K_SPLIT is the kind that exists for
                 * this: the LOW half goes in the split window, the HIGH half in the main one, which
                 * is exactly the inverse of isa_extract's `(main << split_wid) | split_lo`. */
                if (k == A_IMM64) {
                    vb[i] = L.imm_at + 4; vbit[i] = 0; vwid[i] = 32;
                    vk[i] = 8;                       /* K_SPLIT */
                    rw->split_byte = L.imm_at; rw->split_bit = 0; rw->split_wid = 32;
                }
            }
        }
    }
    if (L.has_disp) {
        bytes[L.modrm_at] = (uint8_t)((bytes[L.modrm_at] & 0x3f) | g_dispmod);
    }

    /* THE DISPLACEMENT IS A FIELD, NOT PART OF THE TEMPLATE.
     *
     * A memory operand's displacement lives in 0, 1 or 4 bytes after the ModR/M (and after the SIB
     * byte when there is one) and its VALUE is what the instruction actually addresses. Leaving it as
     * template zeros meant a row could only ever match a displacement of 0: `leaq -0x1(%rdi),%rcx`
     * (48 8d 4f ff) had no row at all, and every real memory operand -- the majority of compiler
     * output -- was unreachable. It is signed, so K_SIMM is the kind: disp8 0xff extracts as -1.
     *
     * ONE FIELD, ADDED ONCE. This block and the SIB pass both used to append a displacement, so a
     * plain memory row came out with two A_DISP slots and the extra one (windowed at byte 0, width 0)
     * matched anything; a SIB row got none and its template demanded displacement 0. The rule is now
     * a single helper -- add_memory_fields -- called from one place with the SIB shift already known.
     */
    /* THE MEMORY FIELDS: the displacement, the base's reserved values, and the RIP-relative form.
     *
     * ONE PLACE OWNS THIS. The displacement was appended by two different blocks, so a plain memory
     * row came out with a second A_DISP slot windowed at byte 0 with width 0 -- which matched
     * anything -- while a SIB row got none at all and demanded displacement 0. The rules are:
     *
     *   mod=00  the base may not be 5 (rm=5 means "no base, disp32 follows", not rbp)
     *   mod=01  a disp8 field
     *   mod=10  a disp32 field
     *   SIB     the displacement sits one byte later, after the SIB byte
     *   RIP     rm is FIXED to 5 and the displacement is a disp32: a different addressing mode from
     *           [rbp+disp8] and so a different row
     */
    if (L.has_disp && !g_make_sib) {
        int dw = (g_dispmod == 0x40) ? 8 : (g_dispmod == 0x80 ? 32 : 0);
        if (g_riprel && g_dispmod == 0x80) {
            int m2 = -1;
            for (int i2 = 0; i2 < rw->nops; i2++) if (rw->ops[i2] == A_MEM) { m2 = i2; break; }
            if (m2 >= 0) {
                /* rm becomes a CONSTANT: there is no base register to encode */
                bytes[L.modrm_at] = (uint8_t)0x05;
                if (rw->group >= 0) bytes[L.modrm_at] |= (uint8_t)((rw->group & 7) << 3);
                vb[m2] = 0; vbit[m2] = 0; vwid[m2] = 0; vk[m2] = 0;
                hb[m2] = 0; hbit[m2] = 0;
                rw->ops[m2] = A_RIPBASE;
                dw = 32;
            } else {
                g_riprel = -1;
            }
        } else if (g_riprel > 0) {
            g_riprel = -1;
        }
        if (dw && rw->nops < MAXOPS && !g_make_sib) {
            int ds = rw->nops;
            /* NO SIB SHIFT HERE. This block runs BEFORE the SIB pass, and that pass already moves every
             * field at or after the ModR/M up by one (its own loop over vb[]/hb[]). Adding `shift`
             * here as well moved the displacement two bytes and every SIB form with a displacement
             * read its value from the wrong place -- which cost 10% of the corpus when tried. */
            vb[ds] = L.disp_at; vbit[ds] = 0; vwid[ds] = dw;
            vk[ds] = 6;                     /* K_SIMM: signed, so disp8 0xff is -1 */
            hb[ds] = 0; hbit[ds] = 0;
            rw->ops[ds] = A_DISP;
            rw->nops = ds + 1;
        }
        /* BASE 5 IS ONLY ILLEGAL AT mod=00. At mod=01/mod=10 a base of 5 (rbp/r13) is an ordinary
         * [rbp+disp] access and refusing it made every such row unencodable.
         *
         * mod=00 with rm=5 does not mean rbp at all: it is the "no base, disp32 follows" form, which
         * is emitted as its own RIP-relative row (`g_riprel`) -- so this only tightens the plain
         * mod=00 row whose rm really is a register field. */
        if (g_dispmod == 0x00 && !g_riprel) {
            for (int i2 = 0; i2 < rw->nops; i2++)
                if (rw->ops[i2] == A_MEM && vwid[i2] > 0) vk[i2] = 21;   /* K_REXREG_5 */
        }
    }

    /* THE NO-BASE SIB FORM: mod=00 with SIB base=5 means "no base, disp32 follows".
     *
     * `leal (,%rax,8),%edx` is `8d 14 c5 00 00 00 00` -- modrm=14 (mod=00, reg=edx, rm=4 -> SIB),
     * sib=c5 (scale=8, index=rax, base=5) and then a disp32 the encoding requires even though it is
     * zero. Base 5 at mod=00 is the "no base" code, exactly as rm=5 is for a non-SIB memory operand.
     *
     * THIS IS ITS OWN PASS, SELECTED BY g_nobase, AND NOT FOLDED INTO THE CALLER'S EMISSION. Running
     * it unconditionally for every mod=00 SIB row made it CONSUME the ordinary indexed rows -- the
     * base slot was overwritten with the constant 5 and those rows stopped matching, which cost 13
     * instructions on the corpus it was meant to help.
     */
    if (g_nobase && g_dispmod == 0x00 && L.has_modrm) {
        if (getenv("X86TD_NB")) fprintf(stderr, "  [nb] row=%s nops=%d disp_at=%d len=%d\n", rw->name, rw->nops, L.disp_at, L.len);
        int memop4 = -1;
        for (int i2 = 0; i2 < rw->nops; i2++)
            if (rw->ops[i2] == A_MEM) { memop4 = i2; break; }
        if (memop4 >= 0 && rw->nops + 2 <= MAXOPS) {
            /* THE LAYOUT IS RECOMPUTED, because under mod=00 the caller laid out no displacement --
             * and this form REQUIRES a disp32. Shifting the existing offsets by the SIB byte alone
             * would leave the row 4 bytes shorter than the encoding, which is exactly what made the
             * decode report 3 bytes for `8d 14 c5 00 00 00 00`. */
            L.disp_at = L.modrm_at + 2;            /* after the ModR/M and the SIB byte */
            L.imm_at = (L.imm_at > L.modrm_at) ? L.disp_at + 4 : L.imm_at;
            L.len = L.disp_at + 4;
            L.has_disp = 1;
            L.has_sib = 1;
            int sib4 = L.modrm_at + 1;
            /* THE SIB BYTE AND THE DISP32 ARE ALREADY IN L.len (the layout counted them), so only
             * the offsets that lie after the ModR/M move up -- the length must not grow again. */
            if (L.imm_at > L.modrm_at) L.imm_at += 1;
            for (int v2 = 0; v2 < rw->nops; v2++) {
                if (vb[v2] > L.modrm_at) vb[v2] += 1;
                if (hb[v2] > L.modrm_at) hb[v2] += 1;
            }
            L.disp_at += 1;      /* the displacement moved with the SIB byte */
            for (int k2 = L.len; k2 > L.modrm_at + 1; k2--) bytes[k2] = bytes[k2 - 1];
            bytes[L.modrm_at] = (uint8_t)((bytes[L.modrm_at] & 0xF8) | 0x04);   /* rm = 4 */
            bytes[L.modrm_at] = (uint8_t)((bytes[L.modrm_at] & 0x3F) | 0x00);   /* mod = 00 */
            bytes[sib4] = (uint8_t)((0u << 6) | (0u << 3) | 5u);                 /* base = 5 */
            int idx4 = rw->nops, scl4 = rw->nops + 1;
            vb[idx4] = sib4; vbit[idx4] = 3; vwid[idx4] = 3; vk[idx4] = 22;      /* K_REXREG_4 */
            hb[idx4] = L.has_rex ? (L.opcode_at - 1) : 0;
            hbit[idx4] = L.has_rex ? 2 : 0;                                     /* REX.X */
            vb[scl4] = sib4; vbit[scl4] = 6; vwid[scl4] = 2; vk[scl4] = 20;      /* K_SIBSCALE */
            hb[scl4] = 0; hbit[scl4] = 0;
            /* the base slot becomes a literal, not a field: it is the constant 5 */
            vb[memop4] = 0; vbit[memop4] = 0; vwid[memop4] = 0; vk[memop4] = 0;
            hb[memop4] = 0; hbit[memop4] = 0;
            rw->ops[memop4] = A_MEM;
            rw->ops[idx4] = A_REG64;
            rw->ops[scl4] = A_SIBSCALE;
            rw->nops = scl4 + 1;
            /* THE DISPLACEMENT IS REQUIRED BY THE ENCODING AND IS FOUR BYTES. A "no base" SIB always
             * carries disp32 -- that field is part of the form even when the value is 0 -- so the
             * length and the template both include it. */
            if (rw->nops < MAXOPS) {
                int ds4 = rw->nops;
                vb[ds4] = L.disp_at; vbit[ds4] = 0; vwid[ds4] = 32; vk[ds4] = 6;  /* K_SIMM */
                hb[ds4] = 0; hbit[ds4] = 0;
                rw->ops[ds4] = A_DISP;
                rw->nops = ds4 + 1;
            }
            /* NO RECURSIVE CALL HERE. This runs inside emit_row_printf, so calling it again would
             * re-enter the whole emitter; the fields above are all it needs to prepare, and the
             * caller performs the single emit for this row. */
        }
    }

    /* THE SIB FORM OF A MEMORY OPERAND.
     *
     * rm=4 in the ModR/M means "the next byte is a SIB", and that byte says
     * scale(2) : index(3) : base(3). It is a DIFFERENT ENCODING of the same instruction, not a
     * variant of the displacement, so it needs its own row: `leaq (%rdi,%rdi,2),%rax` is
     * 48 8d 04 7f while `leaq -0x1(%rdi),%rcx` is 48 8d 4f ff, and one row cannot match both
     * because the second byte differs in kind. The base and the index are both K_REXREG (each with
     * its own REX bit), the scale is K_SIBSCALE (2 bits, human 1/2/4/8), and the ModR/M's reg field
     * still carries the destination/source register exactly as the plain form does.
     *
     * The SIB byte goes AFTER the ModR/M, which shifts the displacement and any immediate -- the
     * layout was computed for the plain form, so those offsets move by one here. */
    /* NOTE: the SIB form of a memory operand is not emitted yet -- see the commit message.
     * The runtime kinds it needs (K_SIBSCALE, K_REXREG_4, K_REXREG_5) are implemented
     * and tested; what is missing is the row emitter for the shifted layout. */

    /* ---- THE SIB FORM ----------------------------------------------------------------
     *
     * rm=4 in the ModR/M means "the next byte is a SIB": scale(2):index(3):base(3). It is a
     * different ENCODING of the same instruction, so it gets its own row. The SIB byte is inserted
     * after the ModR/M, which shifts the displacement and any immediate by one -- so the layout is
     * recomputed from `L` with every offset at or after sib_at moved up.
     *
     * The two registers in the SIB are separate fields: the BASE at bits 0..2 with REX.B, the INDEX
     * at bits 3..5 with REX.X. The index must not be 4 and the base must not be 5 (those mean "no
     * index" and "no base"), which K_REXREG_4 / K_REXREG_5 enforce. The scale is K_SIBSCALE, whose
     * human value is the multiplier 1/2/4/8 rather than the raw 0..3. */
    int sib_at = -1;
    /* THE NO-INDEX SIB FORM IS A DISTINCT ENCODING OF THE SAME INSTRUCTION.
     *
     * `leaq 0x1(%r12), %r13` is `4d 8d 6c 24 01`: modrm=6c (mod=01, reg=r13, rm=4 -> SIB), sib=24
     * = index 4, base 4. An index of 4 means "no index register" -- it is how the assembler encodes a
     * base-plus-displacement access when a REX.B is already needed for the base, and also how
     * `[rsp+disp]` must be written, since rm=4 alone would mean "a SIB follows" and there would be no
     * way to name rsp. So this pass emits it, with the index slot fixed at 4, and the indexed form
     * above emits index != 4. Both are real; neither subsumes the other.
     *
     * THE NO-BASE PASS IS EXCLUDED HERE: `g_nobase` fixes the base at 5 while this one carries a real
     * base register, so running both emitted the same row with the base slot meaning two different
     * things -- which cost 13 instructions on the corpus when it was tried. */
    if (g_make_sib && !g_nobase && !g_riprel && L.has_modrm) {
        for (int i2 = 0; i2 < rw->nops; i2++)
            if (rw->ops[i2] == A_MEM) { sib_at = i2; break; }
        if (sib_at >= 0 && rw->nops + 3 <= MAXOPS) {
            int memop = sib_at;
            sib_at = L.modrm_at + 1;
            /* make room: everything after the ModR/M byte moves up one */
            for (int k2 = L.len; k2 > L.modrm_at + 1; k2--) bytes[k2] = bytes[k2 - 1];
            if (L.disp_at > L.modrm_at) L.disp_at += 1;
            if (L.imm_at  > L.modrm_at) L.imm_at  += 1;
            for (int v2 = 0; v2 < rw->nops; v2++) {
                if (vb[v2] > L.modrm_at) vb[v2] += 1;
                if (hb[v2] > L.modrm_at) hb[v2] += 1;
            }
            L.len += 1;
            /* rm=4 marks the SIB; mod keeps the displacement shape, reg keeps the group/dest */
            bytes[L.modrm_at] = (uint8_t)((bytes[L.modrm_at] & 0xF8) | 0x04);
            bytes[L.modrm_at] = (uint8_t)((bytes[L.modrm_at] & 0x3F) | g_dispmod);
            /* the SIB byte itself: base in the memory operand's slot, index and scale appended */
            /* THE INDEX SLOT IS NOT RESERVED WHEN IT IS FIXED AT 4.
             *
             * A "no index" SIB has no index operand at all, so pushing an empty slot for it left a
             * field entry with vwid=0 in the middle of the row -- and `isadb test` probes every field
             * with a value, so it asked the row to put 3 into a zero-width field and 18 rows per
             * displacement shape failed with "value 3 does not fit 0 bits". The slot must not exist:
             * the scale moves down to fill the gap. */
            int base_slot = memop, scl_slot = rw->nops + (g_noindex ? 0 : 1);
            int idx_slot = g_noindex ? -1 : rw->nops;
            bytes[sib_at] = (uint8_t)((0u << 6) | (0u << 3) | 0u);
            /* the base: the field the plain form already put in rm, now at SIB bits 0..2 */
            vb[base_slot] = sib_at; vbit[base_slot] = 0; vwid[base_slot] = 3;
            vk[base_slot] = 21;                       /* K_REXREG_5 */
            hb[base_slot] = L.has_rex ? (L.opcode_at - 1) : 0;
            hbit[base_slot] = L.has_rex ? 1 : 0;       /* REX.B */
            /* the index: SIB bits 3..5, REX.X -- OR THE FIXED "NO INDEX" VALUE 4.
             *
             * A SIB index of 4 means there is no index register, and that is a legitimate encoding:
             * `4d 8d 6c 24 01` is `leaq 1(%r12),%r13` with sib=0x24 (index 4, base 4, scale 1). The
             * row with the index fixed at 4 and the row with a real index field are two encodings of
             * the same instruction -- the first is the only way to write `[rsp+disp]` and the way a
             * REX.B base is written when no index is wanted. */
            if (g_noindex) {
                /* the fixed "no index" value 4 -- a template byte, NOT a field */
                bytes[sib_at] = (uint8_t)(bytes[sib_at] | (4u << 3));
            } else {
                vb[idx_slot] = sib_at; vbit[idx_slot] = 3; vwid[idx_slot] = 3;
                vk[idx_slot] = 22;                         /* K_REXREG_4 */
                hb[idx_slot] = L.has_rex ? (L.opcode_at - 1) : 0;
                hbit[idx_slot] = L.has_rex ? 2 : 0;        /* REX.X */
                rw->ops[idx_slot] = A_REG64;
            }
            /* the scale: SIB bits 6..7, human 1/2/4/8 -- 1 by default, which is the encoding 0 */
            vb[scl_slot] = sib_at; vbit[scl_slot] = 6; vwid[scl_slot] = 2;
            vk[scl_slot] = 20;                         /* K_SIBSCALE */
            hb[scl_slot] = 0; hbit[scl_slot] = 0;
            rw->ops[scl_slot] = A_SIBSCALE;
            rw->nops = scl_slot + 1;
            /* THE DISPLACEMENT FIELD FOR A SIB ROW COMES LAST, because the SIB byte pushed it one byte
             * beyond the layout's disp_at. The base/index/scale take the slots above, so this is the
             * slot after them -- appending it earlier overwrote the index.
             *
             * `movzbl 0x1(%rsi,%rax),%ecx` (0f b6 4c 06 01: mod=01, rm=4, SIB=06, disp8=01) is the
             * case: without this field the row's template required a displacement of 0 and refused
             * every indexed memory access that carried an offset. */
            if (L.has_disp && rw->nops < MAXOPS && g_dispmod != 0x00) {
                int dw3 = (g_dispmod == 0x40) ? 8 : 32;
                int ds3 = rw->nops;
                /* L.disp_at WAS ALREADY SHIFTED by the "move everything after the ModR/M up one" loop
                 * above, so it already points past the SIB byte. Adding one more put the field on a
                 * byte the instruction does not have: the row read a 5-byte instruction as needing a
                 * disp at byte 5, so `movzbl 0x1(%rsi,%rax),%ecx` (0f b6 4c 06 01) still matched
                 * nothing even with the field present. */
                vb[ds3] = L.disp_at; vbit[ds3] = 0; vwid[ds3] = dw3;
                vk[ds3] = 6;                    /* K_SIMM */
                hb[ds3] = 0; hbit[ds3] = 0;
                rw->ops[ds3] = A_DISP;
                rw->nops = ds3 + 1;
            }
        } else {
            sib_at = -1;
        }
    }

    /* REFUSE A ROW WHOSE FIELD COVERS THE WHOLE OPCODE BYTE, OR THE ESCAPE ITSELF.
     * A field over part of the opcode is LEGITIMATE and load-bearing: jcc/setcc/cmovcc put the
     * condition in the low nibble (low 4 bits of the opcode byte is a field), and the one-byte
     * inc/dec pack the register into the low 3 bits. What is never legitimate is a field covering
     * the ENTIRE opcode byte or the escape byte -- then the row states no opcode at all and matches
     * anything. That is the shape that made `CMOV32rm` rows of `{40,00}` with mask `0f 3f` match
     * `44 0f` and steal the decode from every REX-led instruction. */
    {
        int lastop = L.opcode_at + (rw->opmap == 1 ? 1 : (rw->opmap >= 2 ? 2 : 0));
        for (int z9 = 0; z9 < rw->nops; z9++) {
            if (!vwid[z9]) continue;
            int lo = vb[z9] * 8 + vbit[z9], hi = lo + vwid[z9] - 1;
            /* the escape byte must be a constant */
            if (L.opcode_at != lastop) {
                int a = L.opcode_at * 8, b2 = a + 7;
                if (!(hi < a || lo > b2)) return;
            }
            /* the opcode byte may have fields, but not one covering all 8 bits */
            int a = lastop * 8, b2 = a + 7;
            if (lo <= a && hi >= b2) return;
        }
    }

    /* REFUSE A ROW WITH NO FIELDS OVER AN ENCODING THAT HAS VARIABLE BYTES.
     *
     * isa_fieldmask masks exactly the bits the fields cover, so every other bit must match the
     * template. A row that claims a ModR/M (or a displacement) and assigns NO field over it is
     * therefore a constant pattern rather than a description of an instruction, and a short constant
     * pattern matches garbage: `44 01` (the REX prefix of an ALU op) decoded as CMOV32rm because a
     * 2-byte CMOV row with zero fields matched those two bytes.
     *
     * THE TEST IS "ANY FIELD AT ALL", NOT "A FIELD AT EACH POSITION". Requiring a field specifically
     * over modrm_at also rejected every SIB row, whose base and index live in the SIB byte rather
     * than the ModR/M -- 1,191 good rows thrown away to catch a handful, and coverage fell from 90%
     * to 81%. A row with at least one field over the variable part of its encoding cannot be a
     * constant, which is exactly the property that matters here. */
    /* A FIXED-BYTE FORM IS NOT A CONSTANT PATTERN, EVEN WITH NO FIELDS.
     *
     * `SFENCE : I<0xAE, MRM7X, ...>` is `0f ae f8` -- three bytes, no variable part at all. The
     * instruction IS those bytes, so "a row with no fields over its variable part" does not apply:
     * there is no variable part. The guard below was written for rows that CLAIM a ModR/M yet assign
     * no field over it, which is a different thing (that row cannot distinguish itself from any other
     * word with those bytes). Recognised by the resolved MRM byte, which only a named MRM_<HEX> /
     * MRM<N>X format has. */
    if ((L.has_modrm || L.has_disp) && rw->mrm_byte < 0) {
        int any = 0;
        for (int z9 = 0; z9 < rw->nops; z9++)
            if (vk[z9] && vb[z9] >= L.opcode_at) any = 1;
        if (!any) return;
    }

    printf("{ \"%s%s\", \"%s\", \"", rw->name, sib_at >= 0 ? "_SIB" : "", rw->mnem);
    for (int i = 0; i < rw->nops; i++) {
        const char *t = "?";
        switch (rw->ops[i]) {
            case A_REG8: t = "r8"; break;   case A_REG16: t = "r16"; break;
            case A_REG32: t = "r32"; break; case A_REG64: t = "r64"; break;
            case A_XMM: t = "xmm"; break;
            case A_IMM8: t = "imm8"; break; case A_IMM16: t = "imm16"; break;
            case A_IMM32: t = "imm32"; break; case A_IMM64: t = "imm64"; break;
            case A_REL8: t = "rel8"; break; case A_REL32: t = "rel32"; break;
            case A_MEM: t = "mem"; break;   case A_CCODE: t = "cc"; break;
            case A_SIBSCALE: t = "scale"; break;
            case A_DISP: t = "disp"; break;
            case A_RIPBASE: t = "rip"; break;
            default: t = "op"; break;
        }
        printf("%s%s", i ? "," : "", t);
    }
    printf("\", %d, {", L.len);
    for (int i = 0; i < 16; i++) printf("%s0x%02x", i ? "," : "", i < L.len ? bytes[i] : 0);
    printf("},\n");
    printf("  {");
    for (int i = 0; i < MAXOPS; i++) printf("%s%d", i ? "," : "", vb[i]);
    printf("},\n  {");
    for (int i = 0; i < MAXOPS; i++) printf("%s%d", i ? "," : "", vbit[i]);
    printf("},\n  {");
    for (int i = 0; i < MAXOPS; i++) printf("%s%d", i ? "," : "", vwid[i]);
    printf("},\n  {");
    for (int i = 0; i < MAXOPS; i++) printf("%s%d", i ? "," : "", 0);
    printf("},\n  {");
    for (int i = 0; i < MAXOPS; i++) printf("%s%d", i ? "," : "", vk[i]);
    printf("},\n  %d,%d,%d, %d,\n  {", rw->split_byte, rw->split_bit, rw->split_wid, rw->nops);
    for (int i = 0; i < MAXOPS; i++) printf("%s%d", i ? "," : "", hb[i]);
    printf("},\n  {");
    for (int i = 0; i < MAXOPS; i++) printf("%s%d", i ? "," : "", hbit[i]);
    printf("} },\n");
    g_emit_ok++;
}

static void selftest_resolve(void) {
    load_classes();
    const char *cases[][2] = { {"IncOpR_RF","Xi64"}, {"NotOpR_R","Xi64"}, {"NegOpR_RF","Xi64"},
                               {"BinOpRR_RF","0x01, add, Xi32, null_frag, 0"},
                               {"BinOpRI_RF","0x83, add, Xi32, R, 0"},
                               {"IMulOpRR_RF","Xi16, WriteIMul16Reg"} };
    for (int i = 0; i < 6; i++) {
        long op = 0; char fm[80] = "";
        int got = resolve_def(cases[i][0], cases[i][1], &op, fm, sizeof fm, 0);
        fprintf(stderr, "  resolve %-14s -> got=%d op=0x%lx fmt=%s\n",
                cases[i][0], got, op, fm);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "selftest")) { selftest_resolve(); return 0; }
    if (argc < 2) { fprintf(stderr, "usage: x86td emit|report\n"); return 2; }
    int report_only = !strcmp(argv[1], "report");

    /* THE CLASS TABLE MUST BE PARSED FIRST. A def's Format can arrive as a template argument
     * through two levels of inheritance, so the class definitions are needed before any def can
     * be understood. */
    load_classes();
    load_typeinfos();

    /* THE ROW ARRAY MUST BE BIG ENOUGH FOR THE WHOLE CATALOG, and it was not.
     *
     * It was carved for 12,288 rows, and a second hardcoded `nrow < 12280` guard dropped everything
     * beyond that -- silently, because the emit counts only what it writes. The catalog reached
     * 15,088 rows, so the last ~2,800 rows of the *defm* pass (every SSE/MMX family added by reading
     * X86InstrSSE.td) were thrown away with no diagnostic: the generator reported success and the
     * rows simply were not there. A cap on a generated catalog is a correctness bug, not a
     * performance knob, so the limit lives in one named constant that the array and every guard
     * share. */
    #define MAXROWS 65536
    Row *rows = (Row *)arb_carve_zeroed((long)(sizeof(Row) * MAXROWS));
    /* multiclass-scoped defs are kept aside until every defm in the file has been read, because a
     * defm normally appears AFTER the multiclass body it instantiates. */
    typedef struct {
        char defname[80], cls[80], defargs[3000], mc[80], mndefmargs[3000];
        char seg[1200];   /* the def's (outs)/(ins), kept so the row's operands can be derived */
        char defm_name[80];  /* the defm that instantiated this def's multiclass: ADD + 8rr = ADD8rr */
        int  used;
    } PendingDef;
    PendingDef *pend = (PendingDef *)arb_carve_zeroed((long)(sizeof(PendingDef) * 8192));
    int npend = 0;
    if (!rows) { fprintf(stderr, "x86td: out of carve space\n"); return 1; }
    int nrow = 0;
    int nseen = 0, nrefused = 0, noclass = 0, nofmt = 0, nonint = 0;

    for (int fi = 0; fi < NTF; fi++) {
        char path[512];
        snprintf(path, sizeof path, "%s%s", TDIR, TFILES[fi]);
        char *txt = slurp(path);
        if (!txt) { fprintf(stderr, "x86td: cannot read %s\n", path); continue; }
        /* PENDING DEFS ACCUMULATE ACROSS FILES, and each file's defm pass marks the defs whose
         * multiclass it instantiates. A multiclass declared in X86InstrArithmetic.td is
         * instantiated by a defm in the same file, so this is safe -- and a def is marked used
         * once, so a later defm cannot rebind it. */
        

        /* Walk every `def NAME : CLASS<...>` in the file, TRACKING THE ENCLOSING MULTICLASS.
         *
         * A def nested in a multiclass uses the multiclass's parameters as if they were its own:
         *
         *     multiclass Mul<bits<8> o, string m, Format RegMRM, Format MemMRM, ...> {
         *       def 8r : MulDivOpR<o, RegMRM, m, Xi8, ...>;
         *     }
         *     defm MUL : Mul<0xF7, "mul", MRM4r, MRM4m, ...>;
         *
         * `o` and the format are bound one level FURTHER out, at the `defm`. Without the scope the
         * def's arguments are the literal strings "o" and "m", which resolve to nothing. The
         * enclosing multiclass's parameter names are captured as it opens, and the defm line
         * supplies their values. */
        char *p = txt;
        while ((p = strstr(p, "def ")) != NULL) {
            p += 4;
            /* The name runs to whitespace or a colon. */
            char name[80]; int nl = 0;
            char *q = p;
            while (*q && *q != ' ' && *q != '\t' && *q != ':' && *q != '\n' && nl < 79)
                name[nl++] = *q++;
            name[nl] = 0;
            if (!nl) continue;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != ':') continue;                 /* a pattern or a let, not a def */
            q++;
            while (*q == ' ' || *q == '\t') q++;
            /* CLASS */
            char cls[80]; int cl = 0;
            while (*q && *q != '<' && *q != ';' && *q != '\n' && cl < 79) cls[cl++] = *q++;
            cls[cl] = 0;
            if (*q != '<') continue;                 /* no template arguments */
            /* THE DEF'S OWN ARGUMENT LIST, TO ITS MATCHING '>' AND ACROSS LINES.
             *
             * A def's class argument list wraps:
             *
             *     def rr#suffix : ITy<0x40, MRMSrcRegCC, t, (outs t.RegClass:$dst),
             *                         (ins t.RegClass:$src1, t.RegClass:$src2, ccode:$cond), ...>
             *
             * Stopping at the newline captured `0x40, MRMSrcRegCC, Xi64, (outs GR64:$dst),` and
             * dropped the (ins) group entirely, so every cmov row described one operand and could
             * not match. The scan is depth-aware and bounded instead of line-bounded. */
            char defargs[3000];
            {
                int d = 1, al = 0;
                char *z = q + 1;
                while (*z && d > 0 && al < 2999) {
                    if (*z == '<') d++;
                    else if (*z == '>') { d--; if (!d) break; }
                    else if (*z == ';') break;
                    defargs[al++] = *z++;
                }
                defargs[al] = 0;
            }
            q++;
            while (*q == ' ' || *q == '\t') q++;

            /* A DEF CAN HAVE NO LITERAL OPCODE HERE AND STILL BE A REAL ENCODING.
             *
             *     def INC64r : IncOpR_RF<Xi64>;
             *     class IncOpR_RF<X86TypeInfo t> : UnaryOpR_RF<0xFF, MRM0r, "inc", t, ...>;
             *
             * The `continue` that used to sit on this line threw the def away because its own
             * argument list has no "0x" in it -- which discarded every inc/dec/neg/not/alu-immediate
             * family in the tables. The opcode is one level down, so the check moves to AFTER the
             * class chain has been walked. */
            int has_literal_here = (!strncmp(q, "0x", 2) || !strncmp(q, "0b", 2));
            long op = 0;
            char fmt[80];
            fmt[0] = 0;
            if (has_literal_here) {
                op = strtol(q, &q, 0);
                while (*q == ' ' || *q == '\t') q++;
                if (*q == ',') {
                    q++;
                    while (*q == ' ' || *q == '\t') q++;
                    int fl = 0;
                    while (*q && *q != ',' && *q != '>' && *q != '\n' && fl < 79) fmt[fl++] = *q++;
                    fmt[fl] = 0;
                }
            }

            /* THE REST OF THE DEF, UP TO ITS TERMINATING SEMICOLON.
             *
             * A def's operands and its attribute suffixes are NOT adjacent -- the operands are
             * followed by the asm string and a pattern list, and the attributes (TB, OpSize16, ...)
             * come after those, several lines down:
             *
             *     def MOVZX32rm8 : I<0xB6, MRMSrcMem, (outs GR32:$dst), (ins i8mem :$src),
             *                        "movz{bl|x}\t{$src, $dst|$dst, $src}",
             *                        [(set GR32:$dst, (zextloadi32i8 addr:$src))]>, TB,
             *                        OpSize32, Sched<[WriteLoad]>;
             *
             * Stopping at the first ';' captured only the operand line, so the TB attribute was
             * never seen and every 0F-escaped row shipped a one-byte opcode. The capture therefore
             * runs to the def's own ';', and the operand parser below is unaffected because it looks
             * for the (outs)/(ins) groups by name. */
            char seg[2000];
            int sn = 0;
            char *r = q;
            int paren_depth = 0, bracket_depth = 0, brace_depth = 0;
            while (*r && sn < 1999) {
                if (*r == '(') paren_depth++;
                else if (*r == ')' && paren_depth > 0) paren_depth--;
                else if (*r == '[') bracket_depth++;
                else if (*r == ']' && bracket_depth > 0) bracket_depth--;
                else if (*r == '{') brace_depth++;
                else if (*r == '}' && brace_depth > 0) brace_depth--;
                else if (*r == ';' && !paren_depth && !bracket_depth && !brace_depth) break;
                seg[sn++] = *r++;
            }
            seg[sn] = 0;
            if (getenv("X86TD_SEG") && !strcmp(name, "8rr"))
                fprintf(stderr, "  SEG for 8rr = cls=%s args=|%.90s| seg=|%.150s|\n", cls, defargs, seg);

            nseen++;
            if (strstr(seg, "OpSize32") && strstr(seg, "Requires<[In32BitMode]>")) { nonint++; continue; }
            if (strstr(name, "Pseudo") || strstr(name, "_DB") || strstr(name, "WIN")) { nonint++; continue; }
            /* A COMPILER-ONLY PSEUDO IS NOT AN INSTRUCTION THE ASSEMBLER CAN WRITE.
             *
             *     let isTerminator = 1, isReturn = 1, isBarrier = 1,
             *         hasCtrlDep = 1, isCodeGenOnly = 1 in {
             *     def EH_RETURN64 : I<0xC3, RawFrm, (outs), (ins GR64:$addr),
             *                         "ret\t#eh_return, addr: $addr",
             *                         [(X86ehret GR64:$addr)]>, Sched<[WriteJumpLd]>;
             *     }
             *
             * `EH_RETURN64` entered the catalog as a 2-byte row `48 c0`: the `48` from the GR64 operand
             * and the `c0` from the RawFrm's absent ModR/M, which the emitter writes as a zero anyway.
             * It then matched the first two bytes of `48 c7 45 f0 00 00 00 00` (`movq $0,-0x10(%rbp)`)
             * -- a RawFrm declares no ModR/M, so nothing in the row distinguishes it -- and every -O0
             * frame initialisation in the corpora decoded as EH_RETURN64.
             *
             * THE ENCLOSING `let` IS THE SIGNAL, and reading it backwards to the previous `;` or `}` is
             * exact for this form: the header between them IS the scope this def sits in. (Reading the
             * block's whole body instead pulled in text from unrelated defs -- it filtered `LEA64r`,
             * `BSWAP32r`, `MOVSDrr_REV` and 30 more real rows.) The check is on the header only. */
            int pseudo_scope = 0;
            {
                /* (a) the def's own text */
                if (strstr(seg, "isCodeGenOnly") || strstr(seg, "isPseudo")) pseudo_scope = 1;
                /* (b) A `let ... isCodeGenOnly = 1 ... in {` IMMEDIATELY ABOVE, with nothing but
                 * whitespace, comments and the `let` clause between it and this def. Scanning only to
                 * the previous `;`/`}` was not enough -- in a file where `let` blocks sit back to back
                 * the scan ran into the PREVIOUS block's `let` too and filtered 24 real rows
                 * (`BSWAP32r`, `MMX_MOVD64to64rm`, ...). The text between the `let`'s `in {` and the
                 * def must contain no other `def` token. */
                if (!pseudo_scope) {
                    /* WALK BACK LINE BY LINE, NOT BY CHARACTER.
                     *
                     * TableGen's `let` applies to the statement that follows it, and those statements
                     * are written one per line (a `let` clause may itself wrap over several lines, and a
                     * braced one opens with `in {`). Walking back line by line therefore lands on the
                     * clause that governs this def, and STOPS as soon as a line carries a complete
                     * statement of its own -- a `;` terminator, or another `def`/`defm` keyword. That
                     * stop is what distinguishes `let isCodeGenOnly = ... in def LEA64r` (governs us)
                     * from a clause whose def has already been read on an intervening line (governs that
                     * def, not this one).
                     *
                     * Character-granularity scans of the same region were wrong in both directions:
                     * reading back to the previous `;`/`}`/`{` sometimes stopped at a comment line and
                     * picked up a distant clause (76 real rows lost: `BSWAP32r`, `CMPSDrri`, `CALL64r_NT`
                     * ...), and sometimes stopped at the clause's own brace and saw nothing at all
                     * (`EH_RETURN64` survived as a 2-byte `48 c0` row). */
                    const char *dk = q;
                    while (dk > txt && strncmp(dk, "def ", 4) && strncmp(dk, "defm ", 5)
                           && strncmp(dk, "def\t", 4)) dk--;
                    char hdr[1400];
                    int hl = 0;
                    /* THE CLAUSE MAY BE ON THE DEF'S OWN LINE (`let X in def Y : ...`), so the first
                     * line examined is the text before `def` on `dk`'s line. Only lines ABOVE that one
                     * are subject to the statement-boundary stop. */
                    {
                        const char *ls = dk;
                        while (ls > txt && ls[-1] != '\n') ls--;
                        size_t ll = (size_t)(dk - ls);
                        if (ll > 0 && ll + 2 < sizeof hdr) {
                            memcpy(hdr, ls, ll);
                            hl = (int)ll;
                            hdr[hl] = 0;
                        }
                    }
                    const char *ln = dk;            /* now walk back over complete lines */
                    for (int back = 0; back < 14 && ln > txt; back++) {
                        const char *le = ln;
                        while (le > txt && le[-1] != '\n') le--;      /* start of this line */
                        if (le > txt) {
                            /* the line just examined, to see whether the walk may continue */
                            int stop = 0;
                            for (const char *z = le; z < ln; z++) {
                                if (*z == ';') { stop = 1; break; }
                                if (!strncmp(z, "def ", 4) || !strncmp(z, "defm ", 5)) { stop = 1; break; }
                            }
                            if (stop) break;
                            size_t ll = (size_t)(ln - le);
                            if (hl + (int)ll + 2 < (int)sizeof hdr) {
                                memmove(hdr + ll, hdr, (size_t)hl);
                                memcpy(hdr, le, ll);
                                hl += (int)ll;
                                hdr[hl] = 0;
                            }
                        }
                        ln = le;
                    }
                    if (hl > 0 && (strstr(hdr, "isCodeGenOnly") || strstr(hdr, "isPseudo")))
                        pseudo_scope = 1;
                }
            }
            if (pseudo_scope) { nonint++; continue; }
            /* `_ND` AND `_NF` ARE THE EVEX/APX VARIANTS, NOT LEGACY ENCODINGS.
             *
             * `SHL16m1_ND` is generated from a `_ND` def whose whole point is the EVEX no-flags form;
             * as a legacy row it came out with a phantom 16-bit register operand in slot 0 (the group
             * format's fixed /4 reg field read as a field), so `isadb -t x86_64 test` failed it 24
             * times with "value 3 does not fit 0 bits". Their legacy twins (SHL16m1, SHR16mCL) are
             * already in the catalog from the plain defs, so these add nothing but a wrong row. */
            if (strstr(name, "_ND") || strstr(name, "_NF")) { nonint++; continue; }
            /* (the isCodeGenOnly test is part of `pseudo_scope` above -- it needs the enclosing `let`
             * header, which the def's own text does not carry) */
            /* A FORM THE TABLE RESTRICTS TO 32-BIT MODE IS NOT AN INSTRUCTION IN 64-BIT MODE.
             *
             * `class IncDec_Alt<...> : UnaryOpR_RF<o, AddRegFrm, ...>, Requires<[Not64BitMode]>` and
             * its defs are `INC16r_alt : IncDec_Alt<0x40, ...>` / `DEC32r_alt : IncDec_Alt<0x48, ...>`.
             * Those opcode bytes ARE the REX prefixes in 64-bit mode: 0x48 is REX.W and 0x40 is a bare
             * REX, so a row built from `0x48` with the register packed into the low 3 bits masked
             * exactly the bits that distinguish REX from anything else -- and it matched `48 b8 ...`
             * (movabs with a 64-bit immediate), which the catalog then re-encoded with a zeroed
             * immediate. Predicates that exclude the mode the catalog targets are a filter, not a
             * detail: this is the general rule, not a special case for inc/dec. */
            if (strstr(seg, "Not64BitMode") || strstr(seg, "In32BitMode")) { nonint++; continue; }
            /* THE PREDICATE CAN BE ON THE CLASS RATHER THAN THE DEF.
             * `def DEC32r_alt : IncDec_Alt<0x48, "dec", Xi32>, OpSize32;` has no predicate of its own --
             * `class IncDec_Alt<...> : UnaryOpR_RF<o, AddRegFrm, ...>, Requires<[Not64BitMode]>` does.
             * Walking the chain is what makes the filter general instead of catching only the defs that
             * happen to spell the predicate inline. */
            {
                int excl = 0;
                char cn[80];
                snprintf(cn, sizeof cn, "%s", cls);
                for (int hop2 = 0; hop2 < 8 && cn[0]; hop2++) {
                    ClassDef *cd2 = find_class(cn);
                    if (!cd2) break;
                    if (strstr(cd2->baseargs, "Not64BitMode") || strstr(cd2->baseargs, "In32BitMode")
                        || strstr(cd2->inher_tail, "Not64BitMode") || strstr(cd2->inher_tail, "In32BitMode")) {
                        excl = 1; break;
                    }
                    if (!cd2->base[0]) break;
                    snprintf(cn, sizeof cn, "%s", cd2->base);
                }
                if (excl) { nonint++; continue; }
            }
            /* VEX/EVEX-ENCODED VARIANTS ARE OUT OF SCOPE. An `_EVEX` def's opcode field holds only the
             * escape -- the VEX prefix itself is assembled by LLVM's encoder from the operands -- so
             * the row's template is a two-byte pattern that matches any `66 xx` sequence. Those rows
             * stole the decode from real instructions (a padded `nopw` matched ADCX32rm_EVEX). The
             * vector space needs its own prefix machine and is excluded by name rather than shipped
             * broken. */
            if (strstr(name, "_EVEX") || strstr(name, "EVEX_") || strstr(name, "VEX_")) { nonint++; continue; }
            /* A VEX/EVEX FORM IS NOT A LEGACY ENCODING, and the name is the only place that says so.
             *
             * `defm V#NAME : PDI_binop_rm<...>, VEX, VVVV, WIG;` prepends a literal `V` to every name,
             * so `VPALIGNRrmi` comes from `V#PALIGNR` with the VEX flag on the defm -- and the VEX
             * prefix itself is ASSEMBLED BY LLVM'S ENCODER from the operands rather than being written
             * as bytes anywhere in the table. A row built from one therefore has no VEX byte at all:
             * its template is `0f 00` with a mask over the ModR/M and immediate, so it matched
             * `0f 28 ...` (a legacy MOVAPS) and won the equal-length tie against the real row --
             * reporting `movaps (%rdi),%xmm0` as `VPALIGNRrmi` with a wrong length. A leading `V` on
             * a name whose text carries the VEX/VVVV/WIG attributes is that family, and it is
             * refused rather than emitted with a fabricated prefix. */
            if (name[0] == 'V' && (strstr(seg, "VEX") || strstr(seg, "VVVV") || strstr(seg, "WIG")
                                   || strstr(seg, "EVEX"))) { nonint++; continue; }
            /* AN OPCODE THAT IS A LEGACY PREFIX BYTE IS NOT AN OPCODE.
             *
             * ADCX's def is `I<0x66, MRMSrcMem, ...>` with the 0F 38 escape supplied by a separate
             * attribute, so the table's opcode field holds 0x66 -- which is the operand-size prefix.
             * A row built from it matched any run of `66` bytes: a 16-byte-aligned nop
             * (`66 66 66 66 66 66 2e 0f 1f 84 00 ...`) decoded as ADCX32rm_ND with a displacement of
             * 102. Those rows also need the 0F 38 map, which this catalog does not carry, so they are
             * refused by name rather than shipped as a prefix-eating wildcard. */
            if (opcode_is_prefix(cls, defargs, name)) { nonint++; continue; }
            /* ---- MULTICLASS-SCOPED DEF ---------------------------------------------------
             * `def 8r : MulDivOpR<o, RegMRM, m, Xi8, ...>` inside
             * `multiclass Mul<bits<8> o, string m, Format RegMRM, ...>` uses identifiers that come
             * from the multiclass HEADER, and their values come from a later
             * `defm MUL : Mul<0xF7, "mul", MRM4r, MRM4m, mul>;`. Resolving them here is
             * impossible -- the values do not exist yet -- so the def is held until the defm is
             * read and the scope's bindings are known.
             *
             * The test for "is this multiclass-scoped" is that the first argument of the class
             * list is a bare identifier rather than a literal. */
            {
                char first[64]; int fa = 0;
                for (const char *z = defargs; *z && *z != ',' && *z != '>' && fa < 63; z++) {
                    if (*z != ' ' && *z != '\t' && *z != '\n') first[fa++] = *z;
                }
                first[fa] = 0;
                /* A PARAMETER NAME CAN BE CAPITALISED: multiclass headers use BaseOpc, MemMRM,
                 * AddRegFrm, RegMRM. Testing only for a lowercase first letter let `def 8rr :
                 * BinOpRR_RF<BaseOpc, mnemonic, Xi8, opnodeflag>` through as if BaseOpc were a
                 * literal, so it resolved to opcode 0 and was refused. The real test is that the
                 * token is an IDENTIFIER -- not a number, not a call, not a template. */
                /* ANY ARGUMENT THAT IS A BARE IDENTIFIER MEANS MULTICLASS SCOPING.
                 * `def 8ri : BinOpRI_RF<0x80, mnemonic, Xi8, opnodeflag, RegMRM>` starts with a
                 * literal but uses `mnemonic` and `RegMRM` later -- both are the enclosing
                 * multiclass's parameters. A first-token test let it through, the resolver found
                 * the opcode and then reported the parameter NAME as the format, and the row was
                 * refused with "unknown format" while the format sat one substitution away. */
                int is_ident = 0;
                /* ANY DEF WITH AN OPEN MULTICLASS ABOVE IT BELONGS TO THAT MULTICLASS.
                 *
                 * The parameter-name test below catches the usual case, but a def whose class chain
                 * resolves on its own (Cmov's `rr#suffix` names ITy directly) passed it and was
                 * emitted TWICE: once here, without the defm's TB attribute, and once from the defm.
                 * The direct copy carried map=0, so its template had no 0x0F escape byte and its
                 * first byte was the REX slot -- a row matching `44 0f`, which stole the decode from
                 * the real instruction. A def inside a multiclass is never emitted by the direct
                 * path; its defm supplies the attributes. */
                /* THE INNERMOST OPEN MULTICLASS, FOUND BY DEPTH RELATIVE TO ITS OWN BODY.
                 *
                 * A multiclass is often wrapped in `let ... in { ... }`:
                 *
                 *     let ExeDomain = SSEPackedInt in {
                 *     multiclass sse2_pshuffle<...> { ... }
                 *     }
                 *
                 * so at the `multiclass` keyword the brace depth is ALREADY 1, and a `depth == 0`
                 * test never fires -- `mc_open2` stayed NULL for every wrapped multiclass, and a
                 * `def ri` inside `sse2_pshuffle` was therefore held but attributed to `RorX`
                 * (or to nothing at all). That is why `PSHUFDri` never existed and the catalog
                 * carried bare rows named `ri`/`mi` instead: `pshufd $0xee,%xmm1,%xmm0` matched
                 * nothing.
                 *
                 * The rule that works is relative: remember the depth AT the multiclass keyword,
                 * then watch for the depth to fall back below the body's opening depth -- that is
                 * the moment its body closes, whatever wrapped it. */
                {
                    int d2 = 0; const char *mc_open2 = NULL;
                    int mc_body_depth = 0, mc_pending = 0;
                    int in_str2 = 0, in_line2 = 0;
                    for (const char *z = txt; z < p; z++) {
                        if (in_line2) { if (*z == '\n') in_line2 = 0; continue; }
                        if (in_str2) {
                            if (*z == '\\' && z[1]) { z++; continue; }
                            if (*z == '"') in_str2 = 0;
                            continue;
                        }
                        if (*z == '"') { in_str2 = 1; continue; }
                        if (z[0] == '/' && z[1] == '/') { in_line2 = 1; continue; }
                        if (!strncmp(z, "multiclass ", 11)) {
                            mc_open2 = z + 11;
                            mc_pending = 1;
                        } else if (*z == '{') {
                            d2++;
                            if (mc_pending) { mc_body_depth = d2; mc_pending = 0; }
                        } else if (*z == '}') {
                            if (d2 > 0) d2--;
                            if (mc_open2 && !mc_pending && d2 < mc_body_depth) mc_open2 = NULL;
                        }
                    }
                    if (mc_open2) { is_ident = 1; g_last_mc_name = mc_open2; }
                    else g_last_mc_name = NULL;
                }
                {
                    char tmp2[600]; snprintf(tmp2, sizeof tmp2, "%s", defargs);
                    char *sv2 = tmp2, *tk2;
                    while ((tk2 = strsep(&sv2, ",")) != NULL) {
                        while (*tk2 == ' ' || *tk2 == '\t') tk2++;
                        char *e2 = tk2 + strlen(tk2);
                        while (e2 > tk2 && (e2[-1] == ' ' || e2[-1] == '\t')) *--e2 = 0;
                        if (!*tk2) continue;
                        int ok = 1;
                        for (char *zz = tk2; *zz; zz++) {
                            if (!((*zz >= 'A' && *zz <= 'Z') || (*zz >= 'a' && *zz <= 'z')
                                  || (*zz >= '0' && *zz <= '9') || *zz == '_')) { ok = 0; break; }
                        }
                        if (!ok) continue;
                        if (tk2[0] >= '0' && tk2[0] <= '9') continue;
                        if (!strcmp(tk2, "null_frag") || !strcmp(tk2, "nullptr")
                            || !strcmp(tk2, "SDNPVariadic")) continue;
                        /* A NAME IS A MULTICLASS PARAMETER ONLY IF THE ENCLOSING MULTICLASS
                         * DECLARES IT. Guessing from the shape of the token held back 414 good
                         * defs (every Xi32/GR64 operand looked like a parameter). The multiclass
                         * header is found below anyway, so the parameter list it declares is the
                         * test. */
                        {
                            char *mcbest = NULL;
                            for (char *zz2 = txt; zz2 < p; zz2++)
                                if (!strncmp(zz2, "multiclass ", 11)) mcbest = zz2;
                            if (mcbest) {
                                ClassDef *mctry = NULL;
                                {
                                    char mn2[64]; int a2 = 0;
                                    char *zz3 = mcbest + 11;
                                    while (*zz3 && *zz3 != '<' && *zz3 != ' ' && a2 < 63) mn2[a2++] = *zz3++;
                                    mn2[a2] = 0;
                                    mctry = find_class(mn2);
                                }
                                if (mctry) {
                                    for (int pi = 0; pi < mctry->narg && pi < 192; pi++)
                                        if (!strcmp(tk2, mctry->parg[pi])) { is_ident = 1; break; }
                                }
                            }
                        }
                        if (is_ident) break;
                    }
                }
                /* A DEF WHOSE CLASS CHAIN CANNOT RESOLVE, INSIDE A MULTICLASS, IS SCOPED BY IT.
                 * `def 8rr : BinOpRR_RF<BaseOpc, mnemonic, Xi8, opnodeflag>` inside ArithBinOp_RF
                 * cannot be resolved from its own arguments -- BaseOpc's value only exists at the
                 * `defm ADD : ArithBinOp_RF<0x01, ...>`. The parameter-name test above catches the
                 * common case; this catches the rest, and it is narrow: it needs BOTH an unresolved
                 * class AND an open multiclass, so a def like `LEA64r : RI<0x8D, ...>` (which
                 * resolves) is never held. */
                if (!is_ident && npend < 2047) {
                    ClassDef *ctry = find_class(cls);
                    long rop = 0; char rfmt[80] = "";
                    int res = ctry ? resolve_def(cls, defargs, &rop, rfmt, sizeof rfmt, 0) : 0;
                    /* `rop == 0` IS NOT A FAILURE: PSHUFB's opcode byte is genuinely 0x00.
                     * `res` already reports whether an opcode literal was found at all, so the
                     * value 0 must not be treated as "missing" here. */
                    if (!res || !format_kind(rfmt)) {
                        /* only if a multiclass is open at this point */
                        int depth = 0; const char *mc_open = NULL;
                        for (char *z = txt; z < p; z++) {
                            if (!strncmp(z, "multiclass ", 11)) { if (depth == 0) mc_open = z + 11; }
                            else if (*z == '{') depth++;
                            else if (*z == '}') { if (depth > 0) depth--; if (!depth) mc_open = NULL; }
                        }
                        if (mc_open) is_ident = 1;
                    }
                }
                if (is_ident && npend < 8191) {
                    PendingDef *pd = &pend[npend++];
                    snprintf(pd->defname, sizeof pd->defname, "%s", name);
                    snprintf(pd->cls, sizeof pd->cls, "%s", cls);
                    snprintf(pd->defargs, sizeof pd->defargs, "%s", defargs);
                    snprintf(pd->seg, sizeof pd->seg, "%s", seg);
                    /* THE ENCLOSING MULTICLASS IS THE MOST RECENTLY *OPENED* ONE, NOT THE MOST
                     * RECENTLY MENTIONED ONE. Scanning for the last "multiclass" keyword before the
                     * def kept naming a multiclass that had already CLOSED (its body ends with '}'),
                     * so a def in ArithBinOp_RF was attributed to an unrelated earlier multiclass
                     * and `defm ADD : ArithBinOp_RF<...>` never matched it -- 985 defs held, 114
                     * ever bound. This tracks the brace depth and remembers the multiclass whose
                     * body is still open at the def. */
                    pd->mc[0] = 0;
                    /* THE ENCLOSING MULTICLASS IS ALREADY KNOWN: `mc_open2` from the is_ident test.
                     * Re-deriving it here with a second walk kept disagreeing with that one (a `def ri`
                     * inside `multiclass sse2_pshuffle` was attributed to `RorX`, whose body had closed
                     * hundreds of lines earlier), and two walks over the same text that disagree is a
                     * bug by construction. The one that decides whether the def is held is the one
                     * that must also name it. `g_last_mc` carries that name out of the is_ident block. */
                    {
                        const char *mn = g_last_mc_name;
                        int a = 0;
                        while (mn && *mn && *mn != '<' && *mn != ' ' && *mn != '\n' && a < 79)
                            pd->mc[a++] = *mn++;
                        pd->mc[a] = 0;
                        g_last_mc_name = NULL;
                    }
                    pd->used = 0;
                    pd->mndefmargs[0] = 0;
                    pd->defm_name[0] = 0;
                    p = r;      /* advance past this def and carry on */
                    continue;
                }
            }

            /* ---- RESOLVE THE CLASS CHAIN -------------------------------------------------
             * A def can name neither the opcode nor the format:
             *     def INC64r : IncOpR_RF<Xi64>;
             *     class IncOpR_RF<X86TypeInfo t> : UnaryOpR_RF<0xFF, MRM0r, "inc", t, ...>;
             * The literal 0xFF and the format MRM0r are in the class's own inheritance line. So
             * walk up, substituting this def's arguments into each base's arguments, until a
             * literal opcode and a named Format appear. The prefix rule is read from whichever
             * class in the chain declares one.
             */
            long resolved_op = op;
            char resolved_fmt[80];
            snprintf(resolved_fmt, sizeof resolved_fmt, "%s", fmt);
            unsigned cf = class_flags(cls);

            /* THE DEF'S OWN LITERAL OPCODE WINS; THE CHAIN IS CONSULTED FOR WHATEVER IS MISSING.
             *
             * Walking the chain to "improve" an opcode the def already names replaced 0x1f with an
             * unrelated value from a base class -- NOOPW shipped opcode 0x80. But refusing to walk
             * at all when the opcode IS present left every def whose FORMAT comes from the chain
             * without one:
             *
             *     def TEST64rr : BinOpRR_F<0x85, "test", Xi64, null_frag>;
             *     class BinOpRR_F<o,m,t,node> : BinOpRR<..., (outs), ...>
             *     class BinOpRR<o,m,args,t,out,p> : ITy<o, MRMDestReg, ...>
             *
             * The opcode is in the def, MRMDestReg is two hops down. So the walk runs whenever
             * EITHER is missing, and it only ever OVERWRITES the opcode when that was 0. */
            if (cf == C_UNKNOWN || !format_kind(resolved_fmt) || resolved_op == 0) {
                char curname[64];
                snprintf(curname, sizeof curname, "%s", cls);
                char cargs[600];
                snprintf(cargs, sizeof cargs, "%s", defargs);
                for (int hop = 0; hop < 8; hop++) {
                    ClassDef *cd = find_class(curname);
                    if (!cd) break;
                    long rop = 0; char rfmt[80] = "";
                    int got = resolve_def(curname, cargs, &rop, rfmt, sizeof rfmt, 0);
                    if (got) {
                        /* NEVER OVERWRITE AN OPCODE THE DEF NAMED ITSELF */
                        if (rop && resolved_op == 0) resolved_op = rop;
                        if (rfmt[0] && format_kind(rfmt))
                            snprintf(resolved_fmt, sizeof resolved_fmt, "%s", rfmt);
                    }
                    if (cf == C_UNKNOWN) {
                        unsigned bf = class_flags(cd->base);
                        if (bf != C_UNKNOWN) cf = bf;
                    }
                    if (!cd->base[0]) break;
                    snprintf(curname, sizeof curname, "%s", cd->base);
                    snprintf(cargs, sizeof cargs, "%s", cd->baseargs);
                    if (cf != C_UNKNOWN && format_kind(resolved_fmt) && resolved_op) break;
                }
            }
            if (cf == C_UNKNOWN) {
                /* THE PREFIX IS DERIVED, NOT LOOKED UP.
                 *
                 * A class list can never be complete -- the tables have 338 classes and composite
                 * families forward through two or three of them. But the prefix is decidable from
                 * two facts that ARE always present:
                 *
                 *   1. the def NAME says the operand width: INC64r/INC32r/INC16r/INC8r, ADD64rr,
                 *      PUSH64r, MOV64rm. A name containing 64 needs REX.W; 16 needs 0x66; 8 and 32
                 *      need neither.
                 *   2. the chain's literal opcode says the rest: a 0x66-prefixed opcode (0x66 is
                 *      not used as an opcode in the integer set) or a rel8/rel32 form.
                 *
                 * Anything that still cannot be decided is refused by name, which keeps the
                 * output trustworthy rather than plausible.
                 */
                cf = C_NOREX;
                /* THE WIDTH IS THE DESTINATION REGISTER'S, NOT THE LAST NUMBER IN THE NAME.
                 *
                 * `MOVSX32rm16` sign-extends a 16-BIT SOURCE into a 32-bit destination. Its real
                 * encoding is `0f bf` -- no 0x66 at all, 32-bit being the default in 64-bit mode --
                 * while `MOVSX16rm8` (`movsbw`) IS `66 0f be`. Scanning the name's last number gave
                 * `MOVSX32rm16` the 16, added the prefix, and `movswl -0x2(%rbp),%eax` had no row.
                 *
                 * The (outs) register class states the width outright: GR16 -> 0x66, GR32 -> nothing,
                 * GR64 (or a 64-bit class) -> REX.W. Measured against llvm-mc:
                 *     movsbw %al,%cx = 66 0f be   movsbl %al,%ecx = 0f be   movsbq %al,%rcx = 48 0f be
                 *     movswl %ax,%ecx = 0f bf     movswq %ax,%rcx = 48 0f bf
                 * The name is the fallback for defs whose outs are a parameter rather than a literal. */
                int gotw = 0;
                {
                    const char *o = strstr(defargs, "(outs");
                    if (o) {
                        const char *oe = strchr(o, ')');
                        for (const char *z = o; z && (!oe || z < oe); z++) {
                            if (!strncmp(z, "GR16", 4) || !strncmp(z, "VR16", 4)
                                || !strncmp(z, "FR16", 4)) { cf = C_OPSIZE16 | C_NOREX; gotw = 1; break; }
                            if (!strncmp(z, "GR64", 4) || !strncmp(z, "VR64", 4)
                                || !strncmp(z, "FR64", 4)) { cf = C_REXW; gotw = 1; break; }
                            if (!strncmp(z, "GR32", 4) || !strncmp(z, "VR32", 4)
                                || !strncmp(z, "FR32", 4)) { cf = C_NOREX; gotw = 1; break; }
                        }
                    }
                }
                if (!gotw) {
                    int lw = 0;
                    for (const char *z = name; *z; z++)
                        if (*z >= '0' && *z <= '9') {
                            int v = 0;
                            while (*z >= '0' && *z <= '9') { v = v * 10 + (*z - '0'); z++; }
                            if (v == 8 || v == 16 || v == 32 || v == 64) lw = v;
                            z--;
                        }
                    if (lw == 64)      cf = C_REXW;
                    else if (lw == 16) cf = C_OPSIZE16 | C_NOREX;
                }
                /* a trailing immediate in the class chain */
                if (strstr(cls, "RI8") || strstr(cls, "ri8")) cf |= C_IMM8;
                else if (strstr(cls, "RI32") || strstr(cls, "ri32") || strstr(cls, "RI_RF")) cf |= C_IMM32;
                else if (strstr(cls, "Ii8")) cf |= C_IMM8;
                else if (strstr(cls, "Ii16")) cf |= C_OPSIZE16 | C_IMM16;
                else if (strstr(cls, "Ii32")) cf |= C_IMM32;
                else if (strstr(cls, "Ii64")) cf |= C_IMM64;
                else if (strstr(cls, "PCRel8") || strstr(cls, "brtarget8")) cf |= C_REL8;
                else if (strstr(cls, "PCRel") || strstr(cls, "brtarget")) cf |= C_REL32;
            }
            /* THE 0x66 PREFIX COMES FROM THE DEF'S OWN OpSize16 ATTRIBUTE. Class-based prefix
             * derivation misses it whenever the class has a rule of its own (NOOPW is class I,
             * which maps to C_NOREX, while the def carries OpSize16) -- so the row lacked its 0x66
             * and every 16-bit form failed to decode. */
            if (strstr(seg, "OpSize16")) cf |= C_OPSIZE16;
            /* THE WIDTH COMES FROM THE TYPE ARGUMENT WHERE THE CLASS NAME DOES NOT SAY IT.
             * BinOpRR_RF serves 8rr, 16rr, 32rr and 64rr alike -- the difference is Xi8/Xi16/Xi32/
             * Xi64, and the table states the consequence (`let hasREX_W = t.HasREX_W` in ITy).
             * Reading the prefix from the def NAME alone made all four the same width. */
            /* THE TYPE ARGUMENT DECIDES THE WIDTH, FOR EVERY FAMILY.
             *
             * A list of class-name prefixes cannot be complete: `def IMUL32rm : IMulOpRM_RF<Xi32,
             * WriteIMul32Reg>` is 32-bit (no REX.W) but "IMul" was not in the list, so its width was
             * never read from the type and the chain-walk's C_REXW stood -- the row shipped
             * `48 0f af` for an encoding that is `0f af`. The real test is whether the def's
             * arguments name an X86TypeInfo at all, which is what states the width. */
            {
                char tv[12][64];
                int tn = split_args(defargs, tv, 12);
                int has_type = 0;
                for (int ti8 = 0; ti8 < tn; ti8++) if (find_typeinfo(tv[ti8])) { has_type = 1; break; }
                /* CALLED EITHER WAY. The type-info lookup decides the width for the families that
                 * state it as a type argument, and flags_from_type ALSO walks the class chain for a
                 * `REX_W` attribute -- which is how `def MOV64toPQIrr : RS2I<...>` (whose operands are
                 * literal GR64/VR128, with no type-info token at all) gets its prefix. Gating the
                 * call on has_type left those rows at C_NOREX, so `movq %rax,%xmm0` (66 48 0f 6e c0)
                 * shipped without REX.W and matched nothing. */
                cf = flags_from_type(defargs, name, cls);
                (void)has_type;
            }
            snprintf(fmt, sizeof fmt, "%s", resolved_fmt);
            long op_final = resolved_op;
            int fk = format_kind(fmt);
            if (!fk) {
                if (getenv("X86TD_NFMT") && (!strcmp(name, "JCC_1") || !strcmp(name, "JCC_4")))
                    fprintf(stderr, "  [nofmt] %s cls=%s cf=0x%x resolved_fmt=|%s| op=0x%lx\n",
                            name, cls, cf, resolved_fmt, resolved_op);
                nofmt++;
                /* NAME EVERY DEF THAT PRODUCED NO FORMAT. `nofmt` is the bucket that actually accounts
                 * for the missing rows, so listing it turns "646 defs have no row" into a work list
                 * instead of a number. Gated on an env var so the normal emit stays quiet. */
                if (getenv("X86TD_LEFT"))
                    fprintf(stderr, "  [left] %-22s cls=%-20s op=0x%lx fmt=|%s| args=|%.90s|\n",
                            name, cls, resolved_op, resolved_fmt, defargs);
                if (getenv("X86TD_FMT"))
                    fprintf(stderr, "  [fmt] def=%-14s cls=%-14s defargs=|%.70s| resolved_op=0x%lx resolved_fmt=|%s|\n",
                            name, cls, defargs, resolved_op, resolved_fmt);
                continue;
            }
            (void)op_final;

            /* ---- operands: outs then ins, in the table's own order ----
             *
             * ONLY THE OPERANDS THE FORMAT ENCODES ARE KEPT. The table lists TIED operands as real
             * ins -- BSF64rr is (outs GR64:$dst), (ins GR64:$fallback, GR64:$src) with three
             * operands for two encoded positions -- and XADD64rr has two outs and one ins. Keeping
             * the fallback made the row describe a three-operand encoding whose third field does
             * not exist, so it encoded the wrong registers.
             *
             * The encoded set is: every OUT, plus the LAST IN. That is the shape the formats
             * MRMSrcReg / MRMSrcMem / MRMDestReg / MRMDestMem describe for the two-operand integer
             * space, and it is what the round-trip check measures.
             */
            int ops[MAXOPS]; int no = 0;
            /* THE OPERAND LIST MAY LIVE IN THE CLASS CHAIN, NOT IN THE DEF.
             *
             * `def ADD8rr : BinOpRR_RF<BaseOpc, mnemonic, Xi8, opnodeflag>` has no (outs)/(ins)
             * of its own -- the body ends at the '>' -- because BinOpRR_RF forwards them:
             *
             *   BinOpRR_RF<o,m,t,node,ndd> : BinOpRR<o,m,...,t, (outs t.RegClass:$dst), [...]>
             *   BinOpRR<o,m,args,t,out,p>  : ITy<o, MRMDestReg, t, out, (ins t.RegClass:$src1,...)>
             *
             * So the same substitution walk that finds the opcode is run again over the
             * inheritance line, and it terminates when the text really contains an operand list
             * with every `t.RegClass` already resolved through the type table. This is the piece
             * whose absence kept every arithmetic family out of the catalog. */
            /* THE IN OPERAND'S TABLEGEN TYPE, captured where the text is actually available. It is
             * needed because the ADDRESS SIZE decides the ModR/M layout and nothing else records it:
             * `lea64_8mem` on LEA64_8r is a 16-bit-addressing operand (the 0x67 prefix), whose layout
             * has no SIB byte and no RIP-relative base -- even though the row is 32/64-bit at the
             * operand level and its memory operand is an A_MEM like any other. That is not derivable
             * from the operand kind, so the text is carried out of this scope. */
            char inopty[120];
            inopty[0] = 0;
            parse_operands_tie(seg, ops, &no, tie_of(cls));
            {
                const char *i0 = strstr(seg, "(ins");
                if (i0) {
                    i0 += 4;
                    while (*i0 == ' ') i0++;
                    int tl0 = 0;
                    while (*i0 && *i0 != ':' && *i0 != ')' && tl0 < (int)sizeof inopty - 1)
                        inopty[tl0++] = *i0++;
                    inopty[tl0] = 0;
                }
            }
            if (no == 0) {
                char oseg[1600];
                int dgot = derive_operands(cls, defargs, oseg, sizeof oseg, 0);
                if (getenv("X86TD_DERIV") && !strcmp(name, "IMUL64rri32"))
                    fprintf(stderr, "  [dd2] %-14s cls=%s dgot=%d |%.160s|\n", name, cls, dgot, oseg);
                if (dgot) {
                    parse_operands_tie(oseg, ops, &no, tie_of(cls));
                    /* the DERIVED text wins when it exists: it is the same list with t.RegClass and
                     * the multiclass parameters already substituted, so its type names are resolved */
                    const char *i1 = strstr(oseg, "(ins");
                    if (i1) {
                        i1 += 4;
                        while (*i1 == ' ') i1++;
                        int tl1 = 0;
                        while (*i1 && *i1 != ':' && *i1 != ')' && tl1 < (int)sizeof inopty - 1)
                            inopty[tl1++] = *i1++;
                        inopty[tl1] = 0;
                    }
                    if (getenv("X86TD_DERIV") && no)
                        fprintf(stderr, "  [deriv] %-16s from %-16s -> nops=%d |%.120s|\n",
                                name, cls, no, oseg);
                }
            }
            {
                int grp_unused = -1; (void)grp_unused;
            }
            if (getenv("X86TD_JCC") && (!strcmp(name, "JCC_1") || !strcmp(name, "JCC_4")))
                fprintf(stderr, "  [jcc] %-8s cls=%-12s fk=%d no=%d ops=%d,%d,%d\n",
                        name, cls, fk, no, ops[0], ops[1], ops[2]);
            /* A NAMED MRM_<HEX> / MRM<N>X FORMAT HAS NO OPERANDS BY DESIGN.
             *
             * `SFENCE : I<0xAE, MRM7X, (outs), (ins), ...>`, `RDTSCP : I<0x01, MRM_F9, (outs), (ins)>`,
             * `CLAC : I<0x01, MRM_CA, ...>` -- the whole instruction is a fixed opcode plus a fixed
             * ModR/M byte with the group in its reg field. There is nothing to name as an operand, and
             * refusing them for having none is what left sfence/lfence/mfence/rdtscp/xgetbv/clac/stac
             * with no rows. The group byte is the operand. */
            int mrm_fixed = (fk == F_REGMEM0
                         && (fmt[0] == 'M' && fmt[1] == 'R' && fmt[2] == 'M'
                             && (fmt[3] == '_' || (fmt[4] == 'X' || fmt[4] == 'x'))));
            if (no == 0 && fk != F_RAW && fk != F_ADDREG && fk != F_ADDCC && !mrm_fixed) {
                /* a form with no typed operand but a ModR/M: refuse rather than invent */
                nrefused++;
                if (!report_only) fprintf(stderr, "  !! no operands: %s (%s/%s)\n", name, cls, fmt);
                continue;
            }
            int grp = -1; (void)grp;

            /* THE NAME MUST BE COPIED INTO DURABLE STORAGE. `name` is a stack buffer reused by
             * the next def in the loop, so storing its pointer made every row alias the same
             * bytes -- the report printed 334 rows with blank names, which is what that looks
             * like from the outside. Carve a copy per row. */
            char *nmdup = (char *)arb_carve_zeroed((long)nl + 1);
            if (!nmdup) continue;
            memcpy(nmdup, name, (size_t)nl);

            Row *rw = &rows[nrow];
            rw->name = nmdup; rw->mnem = nmdup;
            /* the operand type captured above, where the text was in scope */
            {
                int ol = 0;
                while (inopty[ol] && ol < (int)sizeof rw->opty - 1) { rw->opty[ol] = inopty[ol]; ol++; }
                rw->opty[ol] = 0;
            }
            /* THE OpPrefix ATTRIBUTE, read as the table writes it. `PD`, `XS`, `XD`, `PS` and
             * `NoPrfx` are prefix VALUES, not names to interpret -- see the Layout comment. */
            rw->pfx = sse_prefix_of(seg);
            /* THE 8-BIT FORM'S OPCODE HAS ITS LOW BIT CLEARED (ITy: !if(!eq(t.HasEvenOpcode, 1), 0,
             * o{0})). Applying it here is what separates ADD8rr (0x00) from ADD32rr (0x01) -- two
             * rows that were otherwise byte-identical, which made every 32-bit add decode as 8-bit
             * because isa_decode keeps the first longest match. */
            {
                char tav2[12][64];
                int tna2 = split_args(defargs, tav2, 12);
                for (int ti3 = 0; ti3 < tna2; ti3++) {
                    TypeInfo *t3 = find_typeinfo(tav2[ti3]);
                    if (t3 && t3->has_even_opcode) { op_final &= ~1L; break; }
                    if (t3) break;
                }
            }
            rw->opcode = op_final; rw->cls = cf; rw->fk = fk;
            /* TWO-BYTE OPCODES. `TB` on a def means the opcode is preceded by the 0x0F escape --
             * `JCC_4 : Ii32PCRel<0x80, ...>, ...` and `BSF64rr : RI<0xBC, ...>` both carry the TB
             * attribute, and the real encoding is 0F 8x / 0F BC. Without it every such row shipped
             * a one-byte opcode and decoded as a different instruction -- and the whole 0F family
             * (movzx, movsx, setcc, cmovcc, jcc near, imul, bsf/bsr) was absent from the catalog,
             * which is 20 of the 146 remaining gaps in clang's own output. */
            /* THE OPCODE MAP IS THE ATTRIBUTE TOKEN TB / T8 / TA, wherever it sits in the def.
             *
             * Matching ", TB" missed `]>, TB,` when the def's text was captured only to the first
             * ';' of a LATER line, and it missed a token that follows a '>' with no space. A
             * whole-token search over the captured text is correct for every spelling the tables
             * use -- and getting this wrong shipped a one-byte 0x0F escape, so movzx/movsx/setcc/
             * cmovcc/jcc all had an opcode that no real instruction starts with. */
            rw->opmap = 0;
            /* THE ESCAPE AND THE PREFIX CAN BE ON THE CLASS, NOT THE DEF.
             *
             * `def rmi : SS3AI<0x0F, MRMSrcMem, ...>` names neither -- and
             *     class SS3AI<...> : Ii8<o, F, outs, ins, asm, pattern, SSEPackedInt>, TA, PD,
             *                        Requires<[UseSSSE3]>;
             * carries BOTH the 0x0F 0x3A escape (TA) and the mandatory 0x66 (PD). Reading only the
             * def's own text left PALIGNRrmi with map=0 and no prefix, so its template degenerated to
             * a bare `0f 00` -- an instruction no real code has -- and it then tied with, and
             * outranked, the genuine `0f 10`/`0f 28` MOVUPS/MOVAPS rows. The same is true of the
             * prefix, so both are resolved by walking the inheritance chain. */
            {
                /* the def's own text first, then every class it inherits from */
                char cn[80];
                snprintf(cn, sizeof cn, "%s", cls);
                char *chain = (char *)arb_carve_zeroed(4096);
                if (chain) {
                    chain[0] = 0;
                    for (int hop = 0; hop < 8 && cn[0]; hop++) {
                        ClassDef *cd = find_class(cn);
                        if (!cd) break;
                        size_t cl2 = strlen(chain);
                        snprintf(chain + cl2, 4096 - cl2, " %s %s", cd->baseargs, cd->inher_tail);
                        if (!cd->base[0]) break;
                        snprintf(cn, sizeof cn, "%s", cd->base);
                    }
                }
                static const struct { const char *tok; int map; } TM[] = {
                    { "TB", 1 }, { "T8", 2 }, { "TA", 3 },
                };
                const char *srcs[2] = { seg, chain ? chain : "" };
                for (int si = 0; si < 2 && !rw->opmap; si++) {
                    for (int ti2 = 0; ti2 < 3; ti2++) {
                        const char *t2 = srcs[si];
                        size_t tl2 = strlen(TM[ti2].tok);
                        while (t2 && (t2 = strstr(t2, TM[ti2].tok)) != NULL) {
                            int pre_ok  = (t2 == srcs[si]) || !((t2[-1] >= 'A' && t2[-1] <= 'Z')
                                                        || (t2[-1] >= 'a' && t2[-1] <= 'z')
                                                        || (t2[-1] >= '0' && t2[-1] <= '9') || t2[-1] == '_');
                            char nxt = t2[tl2];
                            int post_ok = !((nxt >= 'A' && nxt <= 'Z') || (nxt >= 'a' && nxt <= 'z')
                                         || (nxt >= '0' && nxt <= '9') || nxt == '_');
                            if (pre_ok && post_ok) { rw->opmap = TM[ti2].map; break; }
                            t2 += tl2;
                        }
                        if (rw->opmap) break;
                    }
                }
                /* the prefix likewise: the def's text, then the chain */
                rw->pfx = sse_prefix_of(seg);
                if (!rw->pfx && chain) rw->pfx = sse_prefix_of(chain);
            }
            /* THE /DIGIT IS PART OF THE FORMAT NAME: MRM3r / MRM3m means reg=3. It is not an
             * operand, and leaving it at 0 makes every group instruction encode as /0 -- inc
             * becomes "add a byte" and the row is wrong rather than missing. */
            rw->group = -1;
            rw->mrm_byte = -1;
            if (fk == F_REGMEM0 || fk == F_REGREG0) {
                rw->group = fmt[3] - '0';
                /* A NAMED MRM_<HEX> FORMAT DOES NOT PUT ITS DIGIT AT fmt[3]. `MRM_F9` gave
                 * `'_' - '0'` = -49 and `MRM7X` gave 55, so `rdtscp` (0f 01 f9) and `sfence`
                 * (0f ae f8) were emitted with a nonsense group and matched nothing. The hex group is
                 * read from the name when the name is one of these forms. */
                if (fmt[0] == 'M' && fmt[1] == 'R' && fmt[2] == 'M'
                    && (fmt[3] == '_' || fmt[4] == 'X' || fmt[4] == 'x')) {
                    rw->group = mrm_group_of(fmt);
                    rw->mrm_byte = mrm_group_of(fmt);
                } else if (fmt[0] == 'M' && fmt[1] == 'R' && fmt[2] == 'M'
                         && !(fmt[3] >= '0' && fmt[3] <= '7'))
                    rw->group = -1;
            }
            if (getenv("X86TD_SEGMAP") && !strcmp(name, "MOVZX32rm8"))
                fprintf(stderr, "  [segmap] opmap=%d op=0x%lx fk=%d\n", rw->opmap, rw->opcode, rw->fk);
            for (int z = 0; z < no; z++) rw->ops[z] = ops[z];
            rw->nops = no;
            nrow++;
            if (nrow >= MAXROWS - 8) break;
        }
        /* ---- APPLY EVERY defm TO THE DEFS INSIDE ITS MULTICLASS ----
         * `defm MUL : Mul<0xF7, "mul", MRM4r, MRM4m, mul>;` binds Mul's parameters positionally.
         * The defs held earlier each named their multiclass; substituting the defm's arguments for
         * the multiclass's parameters turns `def 8r : MulDivOpR<o, RegMRM, m, Xi8, ...>` into
         * `def 8r : MulDivOpR<0xF7, MRM4r, mul, Xi8, ...>` -- which the ordinary resolver can then
         * walk. This is the last piece that kept every arithmetic family out of the catalog.
         */
        /* ---- INNER defms: a defm INSIDE a multiclass body ------------------------------------
         *
         * A multiclass can instantiate its own members, and the real name is the OUTER
         * instantiation's name followed by the inner defm's:
         *
         *     multiclass sse12_fp_packed_logical<bits<8> opc, string OpcodeStr, ...> {
         *       let Constraints = "$src1 = $dst" in {
         *         defm PS : sse12_fp_packed_logical_rm<opc, VR128, SSEPackedSingle, ...], TB;
         *         defm PD : sse12_fp_packed_logical_rm<opc, VR128, SSEPackedDouble, ...], TB, PD;
         *       }
         *     }
         *     defm XOR : sse12_fp_packed_logical<0x57, "xor", SchedWriteFLogic>;
         *
         * The inner defm appears EARLIER in the file than the outer `defm XOR` that gives it its
         * name and its `opc`/`OpcodeStr` values, so a single text-order pass cannot resolve it -- the
         * first version of this pass read the inner token as a name, truncated it at the first `#`
         * and emitted nothing, which is why no xorps/andps/orps row existed and clang's own
         * `xorps %xmm0,%xmm0` had no row to match.
         *
         * So the inner defms are collected with their ENCLOSING MULTICLASS recorded, and applied
         * when that multiclass is instantiated: at that point the enclosing name is known (it is the
         * outer defm) and the enclosing parameters are bound, which is what lets the inner defm's
         * argument text be resolved into the inner class's arguments. */
        for (char *dm = txt; (dm = strstr(dm, "defm ")) != NULL; ) {
            dm += 5;
            char mname[80]; int a = 0;
            /* THE NAME STOPS AT A COLON, NOT ONLY AT WHITESPACE.
             *
             *     defm SHL: ShiftRotate<...>      <- no space before the colon
             *     defm ADD : ArithBinOp_RF<...>   <- with one
             *
             * Both spellings are in the tables. Stopping only at ' ' or '<' swallowed the colon into
             * the name ("SHL:"), so the `if (*dm != ':')` test never matched and every compact-form
             * defm was skipped -- which is why seven `defm SHL` instantiations produced no shl row
             * at all while the `defm ADD : ...` spelling worked. */
            while (*dm && *dm != ' ' && *dm != '<' && *dm != ':' && a < 79) mname[a++] = *dm++;
            mname[a] = 0;
            /* A defm NAME CAN CONTAIN PLACEHOLDERS, and they must be resolved or the row is garbage.
             *
             * The packed FP families are built by a multiclass that instantiates its own members
             * under a MULTIPLE of the outer name:
             *
             *     multiclass sse12_fp_packed_logical<bits<8> opc, string OpcodeStr, ...> {
             *       ...
             *       let Constraints = "$src1 = $dst" in {
             *         defm PS : sse12_fp_packed_logical_rm<opc, VR128, SSEPackedSingle, ...], TB;
             *         defm PD : sse12_fp_packed_logical_rm<opc, VR128, SSEPackedDouble, ...], TB, PD;
             *       }
             *     }
             *     defm XOR : sse12_fp_packed_logical<0x57, "xor", SchedWriteFLogic>;
             *
             * so the real names are `XORPS` / `XORPD` -- the outer defm's name plus the inner defm's.
             * A `#` in a name is TableGen's concatenation; the tokens it joins are the enclosing
             * instantiation's NAME and the literal text. Left unresolved, the token was read as the
             * name and then truncated at the first `#` by the guard below, producing no xorps/andps/
             * orps row at all -- and clang emits `xorps %xmm0,%xmm0` routinely to zero a vector.
             *
             * The substitution is done where the enclosing instantiation is known: this loop records
             * the current outer defm as it walks, so a nested `defm` inside that defm's multiclass
             * resolves against it. */
            if (strchr(mname, '#')) {
                char nm2[200]; int w2 = 0;
                const char *s2 = mname;
                while (*s2 && w2 < 190) {
                    if (!strncmp(s2, "#NAME#", 6)) {
                        w2 += snprintf(nm2 + w2, sizeof nm2 - (size_t)w2, "%s", g_outer_defm);
                        s2 += 6;
                    } else if (*s2 == '#') {
                        s2++;    /* a bare separator between two literals concatenates */
                    } else nm2[w2++] = *s2++;
                }
                nm2[w2] = 0;
                snprintf(mname, sizeof mname, "%s", nm2);
            }
            while (*dm == ' ' || *dm == '\t') dm++;
            if (*dm != ':') continue;
            dm++;
            while (*dm == ' ' || *dm == '\t') dm++;
            /* A defm CAN INSTANTIATE SEVERAL MULTICLASSES AT ONCE.
             *
             *     defm SQRT : sse1_fp_unop_s<...>, sse1_fp_unop_p<...>, sse2_fp_unop_s<...>, ...;
             *
             * Each name in that comma-separated list defines its own set of defs. Only the first was
             * read, so `SQRT` produced just the `sse1_fp_unop_s` family and `sqrtsd` -- whose def lives
             * in `sse2_fp_unop_s` -- had no row at all. The list is split here and each pair is run
             * through the same instantiation below, by rewriting `mcls`/`margs` per pass. */
            char mclist[1600]; int ml = 0;
            while (*dm && *dm != ';' && ml < 1599) {
                if (*dm == '\n') { dm++; continue; }
                mclist[ml++] = *dm++;
            }
            mclist[ml] = 0;
            /* walk the list: NAME<args> [, NAME<args>]... */
            const char *mcur = mclist;
            while (*mcur) {
            while (*mcur == ' ' || *mcur == '\t') mcur++;
            if (!*mcur || *mcur == ';') break;
            char mcls[80]; int b = 0;
            while (*mcur && *mcur != '<' && *mcur != ';' && b < 79) mcls[b++] = *mcur++;
            mcls[b] = 0;
            {
                int bb = 0;
                while (mcls[bb] == ' ' || mcls[bb] == '\t') bb++;
                if (bb) memmove(mcls, mcls + bb, strlen(mcls + bb) + 1);
                for (int cc = (int)strlen(mcls); cc > 0
                     && (mcls[cc-1] == ' ' || mcls[cc-1] == '\t'); cc--) mcls[cc-1] = 0;
            }
            if (!mcls[0]) break;
            char margs[4000]; margs[0] = 0;
            if (*mcur == '<') {
                int d = 1, c2 = 0; mcur++;
                while (*mcur && d > 0 && c2 < 3999) {
                    if (*mcur == '<') d++;
                    else if (*mcur == '>') { d--; if (!d) break; }
                    margs[c2++] = *mcur++;
                }
                margs[c2] = 0;
            }
            if (*mcur == '>') mcur++;
            while (*mcur == ' ' || *mcur == '\t' || *mcur == '\n') mcur++;
            if (*mcur == ',') mcur++;
            /* THE defm'S TRAILING ATTRIBUTES APPLY TO EVERY DEF IT INSTANTIATES. `defm CMOV64 :
             * Cmov<Xi64, binop_args>, TB;` puts the 0x0F escape on the defm line, while the defs
             * inside Cmov carry none -- so a row built from the def alone shipped `48 40 c0` where
             * the real encoding is `48 0f 4x c0`, and no cmov could decode. The rest of the defm's
             * line (up to its ';') is captured and searched for the map token. */
            /* THE defm'S TRAILING ATTRIBUTES APPLY TO EVERY DEF IT INSTANTIATES. `defm CMOV64 :
             * Cmov<Xi64, binop_args>, TB;` carries the 0x0F escape on the defm LINE while the defs
             * inside Cmov carry none -- so a row built from the def alone shipped `48 40 c0` where
             * the real encoding is `48 0f 4x c0`, and no cmov decoded.
             *
             * dm POINTS PAST THE ARGUMENT LIST ALREADY (the scan above consumed it to the closing
             * '>'), so the tail is read from where dm now stands rather than by re-scanning forward,
             * which ran straight into the ';' and produced an empty string. */
            char dmtail[400];
            /* the defm's own trailing attributes were captured into `mclist`, whose remainder after
             * the whole class list is exactly the `, TB, XD` text. Reuse it as the tail. */
            snprintf(dmtail, sizeof dmtail, "%s", mcur);
            /* THE `let Constraints` ABOVE THE defm IS THE TIE, and it must be captured here because
             * the defs inside the multiclass name no tie of their own:
             *
             *     let Constraints = "$src1 = $dst" in
             *       defm NAME : PDI_binop_rmi<...>;
             *
             * `def ri : PDIi8<...>` in that body has no Constraints, so the class-chain lookup finds
             * nothing and the row kept BOTH operands -- `PSRLQri` described `xmm,xmm,imm8` where
             * `66 0f 73 /2 ib` takes one xmm and the imm8, the destination being implicit. This scans
             * backwards from the defm to the nearest preceding `let` in the same body. */
            char dmtie[160]; dmtie[0] = 0;
            {
                const char *dfm = dm - 20;              /* the defm keyword start, with slack */
                if (dfm < txt) dfm = txt;
                for (const char *ls = dm; ls > dfm; ls--) {
                    if (ls[-1] == 'l' && !strncmp(ls - 1, "let ", 4)) {
                        const char *cx = strstr(ls - 1, "Constraints");
                        if (cx) {
                            const char *q9 = strchr(cx, '"');
                            if (q9) {
                                q9++;
                                int t9 = 0;
                                while (*q9 && *q9 != '"' && t9 < 159) dmtie[t9++] = *q9++;
                                dmtie[t9] = 0;
                            }
                        }
                        break;
                    }
                }
            }
            {
                if (getenv("X86TD_DMF"))
                    fprintf(stderr, "  [dmf] %-10s mcls=%-14s dmtail=|%s|\n", mname, mcls, dmtail);
            }
            /* WHICH MULTICLASS ENCLOSES THIS defm? A flat text scan cannot tell, and the inner
             * expansion needs it: `defm NAME : PDI_binop_rm<...>` inside `PDI_binop_all` must expand
             * against the instantiation of PDI_binop_all that encloses it, so the body of the
             * ENCLOSING multiclass is what gets scanned, not the body of the class being named.
             * The enclosing one is whichever multiclass's braces contain this defm. */
            char encl[80]; encl[0] = 0;
            {
                const char *pcur = dm - 5;
                int depth = 0;
                for (const char *z = txt; z < pcur; z++) {
                    if (!strncmp(z, "multiclass ", 11)) { if (depth == 0) snprintf(encl, sizeof encl, "%s", z + 11); }
                    else if (*z == '{') depth++;
                    else if (*z == '}') { if (depth > 0) depth--; if (!depth) encl[0] = 0; }
                }
                /* the name runs to '<' */
                for (int e2 = 0; encl[e2]; e2++)
                    if (encl[e2] == '<' || encl[e2] == ' ' || encl[e2] == '{') { encl[e2] = 0; break; }
            }
            /* the class this defm instantiates, and the multiclass enclosing it */
            char scancls[80];
            snprintf(scancls, sizeof scancls, "%s", mcls);
            if (encl[0] && find_class(encl)) snprintf(scancls, sizeof scancls, "%s", encl);
            /* find the multiclass header to learn its parameter names */
            ClassDef *mcd = find_class(mcls);
            if (getenv("X86TD_DEFM"))
                fprintf(stderr, "  [defm] name=%-10s cls=%-16s mcd=%s narg=%d\n",
                        mname, mcls, mcd ? "yes" : "NO", mcd ? mcd->narg : -1);
            if (!mcd) continue;
            char mcv[192][64];
            int nmv = split_args(margs, mcv, 192);
            /* ---- INNER defms: expand this multiclass's own `defm` members ------------------------
             *
             * A multiclass can instantiate its own members, and each real name is the OUTER
             * instantiation's name followed by the inner defm's:
             *
             *     multiclass sse12_fp_packed_logical<bits<8> opc, string OpcodeStr, ...> {
             *       let Constraints = "$src1 = $dst" in {
             *         defm PS : sse12_fp_packed_logical_rm<opc, VR128, SSEPackedSingle, ...], TB;
             *         defm PD : sse12_fp_packed_logical_rm<opc, VR128, SSEPackedDouble, ...], TB, PD;
             *       }
             *     }
             *     defm XOR : sse12_fp_packed_logical<0x57, "xor", SchedWriteFLogic>;
             *
             * The inner defm appears EARLIER in the file than the outer `defm XOR` that gives it both
             * its name and its `opc`/`OpcodeStr` values, so a text-order pass cannot resolve it: it
             * read the inner token as a name, truncated it at the first `#` and emitted nothing --
             * which is why no xorps/andps/orps row existed and clang's own `xorps %xmm0,%xmm0` (its
             * standard way to zero a vector) had no row to match.
             *
             * The pair is expanded HERE, where the outer name is in scope and the outer parameters
             * have values. Each inner defm becomes one synthetic instantiation of its own class with
             * the outer parameters already substituted into its argument list, so the existing binding
             * machinery does the rest unchanged. */
            {
                const char *body = NULL, *bend = NULL;
                body = mc_body(txt, scancls, &bend);
                if (getenv("X86TD_INNER") && !strcmp(mcls, "PDI_binop_all"))
                    fprintf(stderr, "  [body] mcls=%s found=%d len=%ld defms=%d\n", mcls,
                            body ? 1 : 0, body ? (long)(bend - body) : 0,
                            body ? (int)(strstr(body, "defm ") ? 1 : 0) : 0);
                if (body && bend) {
                    for (const char *im = body; ; ) {
                        const char *hit = strstr(im, "defm ");
                        if (!hit || hit >= bend) break;
                        im = hit + 5;
                        char iname[80]; int ia = 0;
                        while (im < bend && *im && *im != ' ' && *im != '<' && *im != ':'
                               && ia < 79) iname[ia++] = *im++;
                        iname[ia] = 0;
                        while (im < bend && (*im == ' ' || *im == '\t' || *im == '\n')) im++;
                        if (im >= bend || *im != ':') continue;
                        im++;
                        while (im < bend && (*im == ' ' || *im == '\t' || *im == '\n')) im++;
                        char icls[80]; int ib = 0;
                        while (im < bend && *im && *im != '<' && *im != ';' && ib < 79)
                            icls[ib++] = *im++;
                        icls[ib] = 0;
                        if (!icls[0] || *im != '<') continue;
                        /* the inner argument list, to its matching '>', with the OUTER parameters
                         * substituted -- `opc` and `OpcodeStr` are the outer multiclass's params */
                        char iargs[3000]; int ic = 0;
                        {
                            int d = 1; im++;
                            while (im < bend && *im && d > 0 && ic < 2999) {
                                if (*im == '<') d++;
                                else if (*im == '>') { d--; if (!d) break; }
                                iargs[ic++] = *im++;
                            }
                            iargs[ic] = 0;
                        }
                        if (ic == 0) continue;
                        /* the inner defm's own trailing attributes (`TB`, `TB, PD`, `VEX, VVVV`) --
                         * these sit between the argument list's '>' and the ';', which can be
                         * several lines further on, so the scan is not line-bounded */
                        char itail[400]; int itl = 0;
                        while (im < bend && *im && *im != ';' && itl < 399) {
                            if (*im == '\n') { im++; continue; }
                            itail[itl++] = *im++;
                        }
                        itail[itl] = 0;
                        /* the inner class's OWN attributes count too: `PSI` is `I<...>, TB`, so a VEX
                         * form that names VPSI carries VEX on the inner defm and TB on the class. */
                        char itail2[800];
                        snprintf(itail2, sizeof itail2, "%s", itail);
                        {
                            ClassDef *icd0 = find_class(icls);
                            char cn0[80]; snprintf(cn0, sizeof cn0, "%s", icls);
                            for (int h0 = 0; h0 < 8 && cn0[0]; h0++) {
                                ClassDef *cd0 = icd0;
                                if (!cd0) break;
                                size_t l0 = strlen(itail2);
                                snprintf(itail2 + l0, sizeof itail2 - l0, " %s %s",
                                         cd0->baseargs, cd0->inher_tail);
                                if (!cd0->base[0]) break;
                                snprintf(cn0, sizeof cn0, "%s", cd0->base);
                                icd0 = find_class(cn0);
                            }
                        }
                        /* THE TIE CAN BE A `let` IMMEDIATELY ABOVE THE INNER defm.
                         *
                         *     let Constraints = "$src1 = $dst" in
                         *       defm NAME : PDI_binop_rmi<opc, opc2, ImmForm, ...>;
                         *
                         * `def ri : PDIi8<...>` inside `PDI_binop_rmi` names no tie of its own, so
                         * walking the class chain finds nothing and the row kept BOTH operands
                         * (`PSRLQri` described `xmm,xmm,imm8` where the encoding `66 0f 73 /2 ib`
                         * takes one xmm and the imm8, the destination being implicit). The let text
                         * is the only place the tie exists, so it is read from the text between the
                         * body start and this defm. */
                        char tiebuf_inner[160]; tiebuf_inner[0] = 0;
                        {
                            /* search the text from the body start up to where this defm's arguments
                             * began; `im` has already advanced past the name and the class, so the
                             * window is `body .. (this defm's start)` -- recorded before the walk */
                            const char *dfm_start = hit + 5;
                            for (const char *ls = body; ls < dfm_start; ls++) {
                                if (*ls == 'l' && !strncmp(ls, "let ", 4)
                                    && strstr(ls, "Constraints") && strstr(ls, "Constraints") < dfm_start) {
                                    const char *q9 = strchr(ls, '"');
                                    if (q9 && q9 < dfm_start) {
                                        q9++;
                                        int t9 = 0;
                                        while (*q9 && *q9 != '"' && t9 < 159) tiebuf_inner[t9++] = *q9++;
                                        tiebuf_inner[t9] = 0;
                                    }
                                }
                            }
                        }
                        char isub[2400]; int is = 0;
                        {
                            /* THE PARAMETERS ARE THE ENCLOSING MULTICLASS'S, and their values are
                             * this defm's arguments -- that is what makes `defm NAME : PDI_binop_rm<opc,
                             * OpcodeStr, ...>` inside PDI_binop_all resolve: `opc` is PDI_binop_all's
                             * parameter and `PXOR : PDI_binop_all<0xEF, "pxor", ...>` supplies it. */
                            ClassDef *mcd2 = find_class(scancls);
                            if (!mcd2) mcd2 = mcd;
                            const char *z = iargs;
                            while (*z && is < (int)sizeof isub - 80) {
                                if ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z') || *z == '_') {
                                    char tok[64]; int tl = 0;
                                    while ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z')
                                           || (*z >= '0' && *z <= '9') || *z == '_') {
                                        if (tl < 63) tok[tl++] = *z;
                                        z++;
                                    }
                                    tok[tl] = 0;
                                    const char *rep = tok;
                                    for (int pi = 0; pi < mcd2->narg && pi < 192; pi++)
                                        if (!strcmp(tok, mcd2->parg[pi]) && pi < nmv) { rep = mcv[pi]; break; }
                                    is += snprintf(isub + is, sizeof isub - (size_t)is, "%s", rep);
                                } else {
                                    if (is < (int)sizeof isub - 2) isub[is++] = *z;
                                    isub[is] = 0;
                                    z++;
                                }
                            }
                            isub[sizeof isub - 1] = 0;
                        }
                        /* THE REAL NAME IS THE OUTER INSTANTIATION'S NAME FOLLOWED BY THE INNER ONE.
                         *
                         * A nested `defm` inside a multiclass is auto-prefixed with the name of the
                         * instantiation enclosing it: `defm PS : ...` inside `multiclass
                         * sse12_fp_packed_logical` reached through `defm XOR : ...` defines `XORPS`.
                         * So the placeholder is substituted first, and the outer name is prefixed
                         * only when the result does not already carry it -- `defm V#NAME#PS` spells
                         * its own `VXORPS` once NAME is bound, and prefixing that would invent a name
                         * no table has. */
                            /* THE PREFIXED NAME. `#NAME#` is the outer instantiation, and a name that
                             * already mentions it (`V#NAME#PS` -> `VXORPS`) must NOT be prefixed
                             * again: that produced `XORVXORPS`. Only a name carrying the outer name
                             * because the placeholder put it there is left alone, which is what the
                             * `sub2` test detects. */
                            char iname2[160];
                            {
                                char sub2[200]; int w = 0;
                                const char *nm = iname;
                                int had_ph = 0;
                                while (*nm && w < 190) {
                                    if (!strncmp(nm, "#NAME#", 6)) {
                                        w += snprintf(sub2 + w, sizeof sub2 - (size_t)w, "%s", mname);
                                        nm += 6; had_ph = 1;
                                    } else if (!strncmp(nm, "NAME", 4)
                                               && !((nm[4] >= 'A' && nm[4] <= 'Z')
                                                    || (nm[4] >= 'a' && nm[4] <= 'z')
                                                    || (nm[4] >= '0' && nm[4] <= '9') || nm[4] == '_')) {
                                        /* `NAME` IS THE BARE KEYWORD FOR THE ENCLOSING INSTANTIATION.
                                         * `defm NAME : PDI_binop_rm<...>` inside multiclass
                                         * PDI_binop_all, reached from `defm PXOR : PDI_binop_all<...>`,
                                         * defines `PXOR` -- no prefix, because NAME already stands for
                                         * the outer name in full. */
                                        w += snprintf(sub2 + w, sizeof sub2 - (size_t)w, "%s", mname);
                                        nm += 4; had_ph = 1;
                                    } else sub2[w++] = *nm++;
                                }
                                sub2[w] = 0;
                                if (had_ph) snprintf(iname2, sizeof iname2, "%s", sub2);
                                else        snprintf(iname2, sizeof iname2, "%s%s", mname, sub2);
                            }
                        /* emit one synthetic instantiation: bind the inner class's defs to these
                         * arguments, exactly as an ordinary `defm` would */
                        ClassDef *icd = find_class(icls);
                        if (!icd) continue;
                        char icv[192][64];
                        int nicv = split_args(isub, icv, 192);
                        for (int q3 = 0; q3 < npend; q3++) {
                            PendingDef *pd3 = &pend[q3];
                            /* THE DEF'S MULTICLASS IS MATCHED BY PREFIX. A multiclass body is scanned
                             * with the innermost open name, so a def inside `PDI_binop_rm` that is
                             * itself inside `PDI_binop_rmi` can carry either label depending on where
                             * the brace-depth walk stopped. Requiring an exact name missed every def
                             * whose label was the nested one, which is why `defm NAME : PDI_binop_rm`
                             * produced nothing. A prefix match is exact enough here: the labels differ
                             * only by the suffix of the nested multiclass they were read in. */
                            size_t l3 = strlen(icls);
                            if (strncmp(pd3->mc, icls, l3)) continue;
                            char subs3[2400]; int sl3 = 0; subs3[0] = 0;
                            {
                                const char *z = pd3->defargs;
                                while (*z && sl3 < (int)sizeof subs3 - 80) {
                                    if ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z') || *z == '_') {
                                        char tok[64]; int tl = 0;
                                        while ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z')
                                               || (*z >= '0' && *z <= '9') || *z == '_') {
                                            if (tl < 63) tok[tl++] = *z;
                                            z++;
                                        }
                                        tok[tl] = 0;
                                        const char *rep = tok;
                                        for (int pi = 0; pi < icd->narg && pi < 192; pi++)
                                            if (!strcmp(tok, icd->parg[pi]) && pi < nicv) { rep = icv[pi]; break; }
                                        sl3 += snprintf(subs3 + sl3, sizeof subs3 - (size_t)sl3, "%s", rep);
                                    } else {
                                        if (sl3 < (int)sizeof subs3 - 2) subs3[sl3++] = *z;
                                        subs3[sl3] = 0;
                                        z++;
                                    }
                                }
                                subs3[sizeof subs3 - 1] = 0;
                            }
                            char fullname3[160];
                            snprintf(fullname3, sizeof fullname3, "%s%s", iname2, pd3->defname);
                            {
                                char *hs3 = strchr(fullname3, '#');
                                if (hs3) *hs3 = 0;
                            }
                            int is_vex3 = (iname2[0] == 'V')
                                          && (strstr(itail2, "VEX") || strstr(itail2, "VVVV")
                                              || strstr(itail2, "WIG") || strstr(itail2, "EVEX"));
                            if (getenv("X86TD_INNER"))
                                fprintf(stderr, "  [iscan] mcls=%s iname=%s icls=%s ic=%d\n",
                                        scancls, iname, icls, ic);
                            if (is_vex3) continue;
                            long rop3 = 0; char rfmt3[80] = "";
                            int ok3 = resolve_def(pd3->cls, subs3, &rop3, rfmt3, sizeof rfmt3, 0);
                            /* the opcode may legitimately be 0x00 (PSHUFB); `ok3` is the found/not-found
                             * signal, not the value. */
                            if (!ok3 || !format_kind(rfmt3)) continue;
                            Row *rw3 = &rows[nrow];
                            char *nmdup3 = (char *)arb_carve_zeroed((long)strlen(fullname3) + 1);
                            if (!nmdup3) continue;
                            snprintf(nmdup3, strlen(fullname3) + 1, "%s", fullname3);
                            rw3->name = nmdup3; rw3->mnem = nmdup3;
                            rw3->opcode = rop3; rw3->fk = format_kind(rfmt3);
                            rw3->pfx = sse_prefix_of(dmtail);
                            if (!rw3->pfx) rw3->pfx = sse_prefix_of(itail);
                            if (!rw3->pfx) rw3->pfx = sse_prefix_of(pd3->seg);
                            /* THE MANDATORY PREFIX CAN BE ON THE CLASS CHAIN TOO. `PDI_binop_rmi`
                             * derives from `PDI`, and `PD` is the class that carries the 0x66; a row
                             * whose def and defm tails are bare (`def rr : PDI<...>`) takes it from
                             * there. Without this walk `PSRLWrr` came out as `0f d1 c0` -- the MMX
                             * encoding of a *different* instruction -- and the real `66 0f d1 c0` had
                             * no row of its own, so it bound to the MMX row. */
                            if (!rw3->pfx) {
                                char cn7[80]; snprintf(cn7, sizeof cn7, "%s", pd3->cls);
                                for (int hop7 = 0; hop7 < 8 && cn7[0]; hop7++) {
                                    ClassDef *cd7 = find_class(cn7);
                                    if (!cd7) break;
                                    int p7 = sse_prefix_of(cd7->baseargs);
                                    if (!p7) p7 = sse_prefix_of(cd7->inher_tail);
                                    if (p7) { rw3->pfx = p7; break; }
                                    if (!cd7->base[0]) break;
                                    snprintf(cn7, sizeof cn7, "%s", cd7->base);
                                }
                            }
                            rw3->opmap = 0;
                            {
                                char cch[4096]; cch[0] = 0;
                                char cn8[80]; snprintf(cn8, sizeof cn8, "%s", pd3->cls);
                                for (int hop8 = 0; hop8 < 8 && cn8[0]; hop8++) {
                                    ClassDef *cd8 = find_class(cn8);
                                    if (!cd8) break;
                                    size_t cl8 = strlen(cch);
                                    snprintf(cch + cl8, sizeof cch - cl8, " %s %s",
                                             cd8->baseargs, cd8->inher_tail);
                                    if (!cd8->base[0]) break;
                                    snprintf(cn8, sizeof cn8, "%s", cd8->base);
                                }
                                /* THE OUTER defm'S TAIL CARRIES THE ESCAPE AND THE PREFIX TOO.
                                 * `defm MOVSD : sse12_move<...>, TB, XD;` puts both on the defm
                                 * line; the defs inside `sse12_move` are plain `def rr : SI<0x10,...>`
                                 * and name neither, so `MOVSDrr` came out as the 3-byte `f2 10 c0`
                                 * -- the `0f` escape missing -- while `MOVUPDrr`, whose family class
                                 * supplies TB, was correct. */
                                const char *msrc[4] = { pd3->seg, dmtail, itail, cch };
                                static const char *TK[3] = { "TB", "T8", "TA" };
                                for (int msi = 0; msi < 4 && !rw3->opmap; msi++) {
                                    for (int tki = 0; tki < 3; tki++) {
                                        const char *t8 = msrc[msi];
                                        while (t8 && (t8 = strstr(t8, TK[tki])) != NULL) {
                                            int pre = (t8 == msrc[msi])
                                                      || !((t8[-1] >= 'A' && t8[-1] <= 'Z')
                                                           || (t8[-1] >= 'a' && t8[-1] <= 'z')
                                                           || (t8[-1] >= '0' && t8[-1] <= '9')
                                                           || t8[-1] == '_');
                                            char nx = t8[2];
                                            int post = !((nx >= 'A' && nx <= 'Z') || (nx >= 'a' && nx <= 'z')
                                                         || (nx >= '0' && nx <= '9') || nx == '_');
                                            if (pre && post) { rw3->opmap = tki + 1; break; }
                                            t8 += 2;
                                        }
                                        if (rw3->opmap) break;
                                    }
                                }
                                if (!rw3->opmap && strstr(rfmt3, "CC")) rw3->opmap = 1;
                            }
                            rw3->group = -1;
                            rw3->mrm_byte = -1;
                            if (rw3->fk == F_REGMEM0 || rw3->fk == F_REGREG0) {
                                rw3->group = rfmt3[3] - '0';
                                if (rfmt3[0] == 'M' && rfmt3[1] == 'R' && rfmt3[2] == 'M'
                                    && (rfmt3[3] == '_' || rfmt3[4] == 'X' || rfmt3[4] == 'x')) {
                                    rw3->group = mrm_group_of(rfmt3);
                                    rw3->mrm_byte = mrm_group_of(rfmt3);
                                }
                            }
                            rw3->cls = flags_from_type(subs3, pd3->defname, pd3->cls);
                            if (strstr(pd3->seg, "OpSize16")) rw3->cls |= C_OPSIZE16;
                            if (strstr(dmtail, "OpSize16")) rw3->cls |= C_OPSIZE16;
                            {
                                int pops3[MAXOPS]; int pno3 = 0;
                                int got3 = 0;
                                /* THE DERIVED LIST FIRST, because the inner def's own text names the
                                 * multiclass's PARAMETERS, not the types:
                                 *     def rr : PI<opc, MRMSrcReg, (outs RC:$dst), (ins RC:$src1, ...)>
                                 * -- `RC` binds to VR128 only through this instantiation, and
                                 * operand_kind("RC") is A_NONE by design (a register class whose width
                                 * is not known here is refused, not guessed). parse_operands therefore
                                 * "succeeds" while producing nothing but A_NONE, and the row is dropped
                                 * at emit. The substituted list has RC replaced, which is what makes
                                 * these rows expressible at all. */
                                char dseg3[1600];
                                const char *tie3 = tiebuf_inner[0] ? tiebuf_inner : tie_of(pd3->cls);
                                /* THE OPERANDS COME FROM THE SUBSTITUTED TEXT OR THE CLASS CHAIN, NEVER
                                 * FROM `pd3->seg`: for an inner def that is still the multiclass's
                                 * parameter names (`opc`, `RC`), and parsing it gave `ops=19,5` -- slot
                                 * 1 was A_NONE because `RC` is not a register class the parser knows, so
                                 * the row was skipped at emit. `subs3` has the parameters bound and
                                 * `derive_operands` walks the class chain with that binding, which is
                                 * where `PDI` -> `I` carries the real operand text. */
                                if (parse_operands_tie(subs3, pops3, &pno3, tie3)) got3 = 1;
                                if (!got3 && derive_operands(pd3->cls, subs3, dseg3, sizeof dseg3, 0)
                                    && parse_operands_tie(dseg3, pops3, &pno3, tie3)) got3 = 1;
                                if (!got3) {
                                    if (getenv("X86TD_INNER"))
                                        fprintf(stderr, "  [inner-nops] %s ops=%.70s\n",
                                                fullname3, pd3->seg);
                                    continue;
                                }
                                for (int z3 = 0; z3 < pno3; z3++) rw3->ops[z3] = pops3[z3];
                                rw3->nops = pno3;
                                rw3->opty[0] = 0;
                            }
                            nrow++;
                        }
                    }
                }
            }
            for (int q2 = 0; q2 < npend; q2++) {
                PendingDef *pd = &pend[q2];
                (void)mcls;
                if (strcmp(pd->mc, mcls)) continue;
                /* SUBSTITUTE EVERY MULTICLASS PARAMETER NAME WHEREVER IT APPEARS AS A WHOLE TOKEN.
                 *
                 *   multiclass ArithBinOp_RF<bits<8> BaseOpc, bits<8> BaseOpc2, bits<8> BaseOpc4,
                 *                            string mnemonic, Format RegMRM, Format MemMRM, ...>
                 *     def 8ri : BinOpRI_RF<0x80, mnemonic, Xi8, opnodeflag, RegMRM>;
                 *   defm ADD : ArithBinOp_RF<0x01, 0x03, 0x05, "add", MRM0r, MRM0m, ...>;
                 *
                 * A comma-token rewrite is not enough here: `mnemonic` and `RegMRM` are whole
                 * tokens that must be replaced, and the header has more parameters than the 12 the
                 * table kept, so positional lookup alone missed them. This walks the def's argument
                 * text and replaces any identifier that IS a multiclass parameter name with that
                 * parameter's bound value. */
                char subs[900]; int sl = 0; subs[0] = 0;
                {
                    const char *z = pd->defargs;
                    while (*z && sl < (int)sizeof subs - 80) {
                        if ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z') || *z == '_') {
                            char tok[64]; int tl = 0;
                            const char *st = z;
                            while ((*z >= 'A' && *z <= 'Z') || (*z >= 'a' && *z <= 'z')
                                   || (*z >= '0' && *z <= '9') || *z == '_') {
                                if (tl < 63) tok[tl++] = *z;
                                z++;
                            }
                            tok[tl] = 0;
                            const char *rep = tok;
                            for (int pi = 0; pi < mcd->narg && pi < 192; pi++) {
                                if (!strcmp(tok, mcd->parg[pi]) && pi < nmv) { rep = mcv[pi]; break; }
                            }
                            sl += snprintf(subs + sl, sizeof subs - (size_t)sl, "%s", rep);
                            (void)st;
                        } else {
                            if (sl < (int)sizeof subs - 2) subs[sl++] = *z;
                            subs[sl] = 0;
                            z++;
                        }
                    }
                    subs[sizeof subs - 1] = 0;
                }
                snprintf(pd->mndefmargs, sizeof pd->mndefmargs, "%s", subs);
                snprintf(pd->defm_name, sizeof pd->defm_name, "%s", mname);
                if (getenv("X86TD_MATCH"))
                    fprintf(stderr, "  [match] %s + %s (mc=%s) <= defm %s\n",
                            mname, pd->defname, pd->mc, mcls);

                /* EMIT HERE, INSIDE THE defm LOOP.
                 *
                 * ONE MULTICLASS CAN BE INSTANTIATED MANY TIMES, and each instantiation is a
                 * separate copy of every def in its body:
                 *
                 *     defm AND : ArithBinOp_RF<0x21, ...>;
                 *     defm OR  : ArithBinOp_RF<0x09, ...>;
                 *     defm XOR : ArithBinOp_RF<0x31, ...>;
                 *     defm ADD : ArithBinOp_RF<0x01, ...>;
                 *     defm SUB : ArithBinOp_RF<0x29, ...>;
                 *
                 * Emitting after the loop with a `used` flag gave the WHOLE family to whichever
                 * defm came first (AND) and left ADD, OR, XOR and SUB with nothing -- 4 of the 5
                 * arithmetic opcodes absent, which is exactly what the catalog was missing. A def
                 * is therefore resolved once per defm that instantiates its multiclass.
                 */
                {
                    char fullname[160];
                    snprintf(fullname, sizeof fullname, "%s%s", mname, pd->defname);
                    /* A defm's `#suffix` placeholder is replaced by the value the defm passed (often
                     * the empty string, meaning the plain name). Substitution already did that; a
                     * `#` left in the name means the parameter was not in the multiclass header, so
                     * cut it there rather than shipping "CMOV32rr#suffix" as a row name. */
                    /* ONE PASS, NOT A LOOP. The loop form never terminated when the character after
                     * the '#' was itself the '#' or a space: `he` started AT `hs`, memmove copied the
                     * text onto itself, and the '#' was still there on the next test -- an infinite
                     * loop that hung the emit and left a zero-byte catalog behind. The '#' and the
                     * rest of that placeholder are simply truncated once. */
                    {
                        char *hs = strchr(fullname, '#');
                        if (hs) *hs = 0;
                    }
                    long rop = 0; char rfmt[80] = "";
                    /* A defm THAT CARRIES THE VEX/EVEX ATTRIBUTES IS NOT A LEGACY ENCODING. The check
                     * has to be here as well as in the plain-def path, because this loop builds the
                     * name itself and never passes through that filter: `defm VPALIGNR :
                     * ssse3_palignr<...>, VEX, VVVV, WIG;` still produced a row (VPALIGNRrmi), whose
                     * template is a bare `0f 00` with no VEX byte -- LLVM assembles that prefix from
                     * the operands, so the table has no bytes for it. The row then matched `0f 28 ...`
                     * and outranked the real MOVAPS on the equal-length tie. */
                    int is_vex_defm = (mname[0] == 'V')
                                      && (strstr(dmtail, "VEX") || strstr(dmtail, "VVVV")
                                          || strstr(dmtail, "WIG") || strstr(dmtail, "EVEX"));
                    if (is_vex_defm) { nonint++; continue; }
                    /* `rop` may legitimately be 0 (an opcode byte of 0x00); resolve_def's return value
                     * is the found/not-found signal. */
                    if (!resolve_def(pd->cls, pd->mndefmargs, &rop, rfmt, sizeof rfmt, 0)
                        || !format_kind(rfmt)) {
                        if (getenv("X86TD_PFAIL"))
                            fprintf(stderr, "  [pfail] %s cls=%s op=0x%lx fmt=|%s|\n",
                                    fullname, pd->cls, rop, rfmt);
                    } else if (nrow < MAXROWS) {
                        Row *rw = &rows[nrow];
                        char *nmdup = (char *)arb_carve_zeroed((long)strlen(fullname) + 1);
                        snprintf(nmdup, strlen(fullname) + 1, "%s", fullname);
                        rw->name = nmdup; rw->mnem = nmdup;
                        {
                            char tav3[12][64];
                            int tna3 = split_args(pd->mndefmargs, tav3, 12);
                            for (int ti4 = 0; ti4 < tna3; ti4++) {
                                TypeInfo *t4 = find_typeinfo(tav3[ti4]);
                                if (t4 && t4->has_even_opcode) { rop &= ~1L; break; }
                                if (t4) break;
                            }
                        }
                        rw->opcode = rop; rw->fk = format_kind(rfmt);
                        /* THE PREFIX AND THE ESCAPE COME FROM THE CLASS CHAIN TOO, AND THIS PATH MUST
                         * WALK IT FOR THE SAME REASON THE PLAIN-DEF PATH DOES.
                         *
                         * `def rmi : SS3AI<0x0F, MRMSrcMem, ...>` states neither, and SS3AI is
                         * `Ii8<...>, TA, PD` -- so the 0x0F 0x3A escape and the mandatory 0x66 are two
                         * hops up the inheritance line. Reading only pd->seg and dmtail (the def's own
                         * text and the defm's tail) left PALIGNRrmi with map=0 and no prefix, so its
                         * template degenerated to a bare `0f 00` -- and an instruction table entry
                         * that no real code starts with is worse than a missing one, because it wins
                         * ties. The chain is collected once here and used for both. */
                        char cls_chain[4096];
                        cls_chain[0] = 0;
                        {
                            char cn7[80];
                            snprintf(cn7, sizeof cn7, "%s", pd->cls);
                            for (int hop7 = 0; hop7 < 8 && cn7[0]; hop7++) {
                                ClassDef *cd7 = find_class(cn7);
                                if (!cd7) break;
                                size_t cl7 = strlen(cls_chain);
                                snprintf(cls_chain + cl7, sizeof cls_chain - cl7, " %s %s",
                                         cd7->baseargs, cd7->inher_tail);
                                if (!cd7->base[0]) break;
                                snprintf(cn7, sizeof cn7, "%s", cd7->base);
                            }
                        }
                        /* the OpPrefix attribute: the defm's tail, the def's text, then the chain */
                        rw->pfx = sse_prefix_of(dmtail);
                        if (!rw->pfx) rw->pfx = sse_prefix_of(pd->seg);
                        if (!rw->pfx) rw->pfx = sse_prefix_of(cls_chain);
                        /* THE FORMAT CAN IMPLY THE OPCODE MAP. MRMSrcRegCC / MRMSrcMemCC name the
                         * 0F-escaped cmovcc family, whose escape is not optional and not always
                         * written on the defm line: the `_ND` instantiations carry no TB attribute
                         * yet their encoding is still 44/0F 4x. Relying on the token alone left
                         * those rows with opmap=0, so their first byte became the REX slot and the
                         * rows matched `44 0f` -- stealing the decode from every REX-led
                         * instruction until this was traced with the field-mask probe. */
                        rw->opmap = 0;
                        {
                            const char *rf2 = rfmt;
                            if (strstr(rf2, "CC")) rw->opmap = 1;
                        }
                        {
                            /* the map token can sit on the def's own text, the defm line, or the
                             * inheritance chain -- see the SS3AI note above */
                            const char *mapsrc[3] = { pd->seg, dmtail, cls_chain };
                            static const char *TOKS[3][2] = { {"TB",0}, {"T8",0}, {"TA",0} };
                            for (int ms = 0; ms < 3 && !rw->opmap; ms++) {
                                for (int ti6 = 0; ti6 < 3; ti6++) {
                                    const char *tk = TOKS[ti6][0];
                                    const char *t7 = mapsrc[ms];
                                    while (t7 && (t7 = strstr(t7, tk)) != NULL) {
                                        int pre = (t7 == mapsrc[ms]) || !((t7[-1] >= 'A' && t7[-1] <= 'Z')
                                                  || (t7[-1] >= 'a' && t7[-1] <= 'z')
                                                  || (t7[-1] >= '0' && t7[-1] <= '9') || t7[-1] == '_');
                                        char nx = t7[2];
                                        int post = !((nx >= 'A' && nx <= 'Z') || (nx >= 'a' && nx <= 'z')
                                                  || (nx >= '0' && nx <= '9') || nx == '_');
                                        if (pre && post) { rw->opmap = ti6 + 1; break; }
                                        t7 += 2;
                                    }
                                    if (rw->opmap) break;
                                }
                            }
                        }
                        rw->cls = flags_from_type(pd->mndefmargs, pd->defname, pd->cls);
                        if (strstr(pd->seg, "OpSize16")) rw->cls |= C_OPSIZE16;
                        {
                            int pops[MAXOPS]; int pno = 0;
                            /* DERIVE THE OPERANDS FROM THE SUBSTITUTED ARGUMENT LIST.
                             *
                             * For a def instantiated by a defm, pd->mndefmargs IS the class's
                             * argument list with the defm's values in place:
                             *
                             *     ITy: 0x40, MRMSrcRegCC, Xi64, (outs Xi64.RegClass:$dst),
                             *                      (ins Xi64.RegClass:$src1, Xi64.RegClass:$src2,
                             *                           ccode:$cond), ...
                             *
                             * The def's own `seg` is the UNSUBSTITUTED text (`outs t.RegClass:$dst`
                             * with `t` the multiclass's parameter), which parse_operands rightly
                             * refuses -- one A_NONE operand, row skipped. So the groups are taken from
                             * the substituted list, with each `Type.member` resolved through the type
                             * table (Xi64.RegClass -> GR64) and `t` bound to the type argument found
                             * in that same list. Any (outs)/(ins) still holding an unresolved member
                             * is not used, so a row is never emitted with a guessed operand.
                             */
                            {
                                Binding xb[4]; int xnb = 0;
                                char xseg[2400];
                                char tav4[12][64];
                                int tna4 = split_args(pd->mndefmargs, tav4, 12);
                                /* bind `t` to whichever argument is a type-info */
                                for (int t5 = 0; t5 < tna4 && xnb < 3; t5++)
                                    if (find_typeinfo(tav4[t5])) {
                                        snprintf(xb[xnb].nm, sizeof xb[xnb].nm, "t");
                                        snprintf(xb[xnb].val, sizeof xb[xnb].val, "%s", tav4[t5]);
                                        xnb++;
                                        break;
                                    }
                                subst_text(pd->mndefmargs, xb, xnb, xseg, sizeof xseg);
                                if (getenv("X86TD_IMMT") && !strcmp(rw->name, "ADD64ri8"))
                                    fprintf(stderr, "  [immt] cls=%s args=|%.200s| xseg=|%.300s|\n",
                                            pd->cls, pd->mndefmargs, xseg);
                                if (getenv("X86TD_CMOV2") && !strcmp(rw->name, "ADD64ri8"))
                                    fprintf(stderr, "  [cmov2] xseg=|%.260s|\n", xseg);
                                if ((strstr(xseg, "(outs") || strstr(xseg, "(ins"))
                                    && !strstr(xseg, "RegClass") && !strstr(xseg, ".MemOperand"))
                                    parse_operands_tie(xseg, pops, &pno,
                                                       dmtie[0] ? dmtie : tie_of(pd->cls));
                                /* THE OPERANDS USUALLY LIVE IN THE CLASS CHAIN, NOT THE ARG LIST.
                                 * `def 64ri8 : BinOpRI8_RF<0x83, mnemonic, Xi64, RegMRM>` states the
                                 * opcode, the group and the width; the (outs)/(ins) pair is two hops
                                 * up in BinOpRI8 -> ITy. So the chain walk is the PRIMARY source and
                                 * the text above is the fallback, not the other way round. */
                                if (pno == 0) parse_operands(pd->seg, pops, &pno);
                                if (pno == 0) {
                                    char oseg[2400];
                                    if (derive_operands(pd->cls, pd->mndefmargs, oseg, sizeof oseg, 0)) {
                                        subst_text(oseg, xb, xnb, xseg, sizeof xseg);
                                        if (getenv("X86TD_IMMT") && !strcmp(rw->name, "ADD64ri8")) {
                                            int zz[MAXOPS]; int zn = 0;
                                            parse_operands(xseg, zz, &zn);
                                            fprintf(stderr, "  [immc] chain=|%.290s|\n         -> zn=%d ops=%d,%d\n",
                                                    xseg, zn, zz[0], zz[1]);
                                        }
                                        parse_operands(xseg, pops, &pno);
                                    }
                                }
                                if (pno == 0) {
                                    char oseg[2400];
                                    if (derive_operands(pd->cls, pd->mndefmargs, oseg, sizeof oseg, 0)) {
                                        subst_text(oseg, xb, xnb, xseg, sizeof xseg);
                                        parse_operands(xseg, pops, &pno);
                                    }
                                }
                            }
                            for (int z = 0; z < MAXOPS; z++) rw->ops[z] = (z < pno) ? pops[z] : A_NONE;
                            rw->nops = pno;
                        }
                        rw->group = -1;
                        rw->mrm_byte = -1;
                        if (rw->fk == F_REGMEM0 || rw->fk == F_REGREG0) {
                            rw->group = rfmt[3] - '0';
                            if (rfmt[0] == 'M' && rfmt[1] == 'R' && rfmt[2] == 'M'
                                && (rfmt[3] == '_' || rfmt[4] == 'X' || rfmt[4] == 'x')) {
                                rw->group = mrm_group_of(rfmt);
                                rw->mrm_byte = mrm_group_of(rfmt);
                            }
                        }
                        if (getenv("X86TD_CMOV") && !strcmp(rw->name, "CMOV64rr")) {
                            int zz[MAXOPS]; int zn = 0;
                            parse_operands(pd->seg, zz, &zn);
                            fprintf(stderr, "  [cmov] seg=|%.150s| -> zn=%d cls=%s args=|%.150s| nops=%d\n",
                                    pd->seg, zn, pd->cls, pd->mndefmargs, rw->nops);
                        }
                        nrow++;
                    }
                }
            }
            }   /* next multiclass in the defm's class list */
        }
        /* the held defs were resolved and emitted inside the defm loop above, once per
         * instantiation -- see the comment there for why the `used` flag had to go. */
        arbit_release_carve((long)txt);
        if (nrow >= 12287) break;
    }

    fprintf(stderr, "x86td: %d classes parsed\n", g_ncls);
    if (getenv("X86TD_ALLCLS")) {
        for (int i = 0; i < g_ncls; i++) fprintf(stderr, "  C %s\n", g_cls[i].name);
    }
    if (getenv("X86TD_PEND"))
        fprintf(stderr, "  PEND n=%d  (used=%d)  e.g. def=%s mc=%s args=%.50s -> %.50s\n",
                npend, npend ? pend[0].used : 0,
                npend ? pend[0].defname : "", npend ? pend[0].mc : "",
                npend ? pend[0].defargs : "", npend ? pend[0].mndefmargs : "");
    if (getenv("X86TD_CLS")) {
        for (int i = 0; i < g_ncls; i++)
            if (strstr(g_cls[i].name, "IncDec") || strstr(g_cls[i].name, "UnaryOpR") ||
                strstr(g_cls[i].name, "BinOpRI8_R"))
                fprintf(stderr, "  CLS %-16s fmt_pos=%d base=%-16s baseargs=%.60s\n",
                        g_cls[i].name, g_cls[i].fmt_pos, g_cls[i].base, g_cls[i].baseargs);
    }
    fprintf(stderr,
            "x86td: %d defs scanned, %d carried an opcode and a known shape,\n"
            "        refused: %d unknown class, %d unknown format, %d out of scope, %d other\n",
            nseen, nrow, noclass, nofmt, nonint, nrefused);
    if (report_only) {
        printf("\n--- derived rows ---\n");
        for (int i = 0; i < nrow; i++) {
            printf("%-28s op=0x%02lx cls=0x%02x fk=%d map=%d nops=%d grp=%d ops=%d,%d,%d",
                   rows[i].name, rows[i].opcode, rows[i].cls, rows[i].fk, rows[i].opmap,
                   rows[i].nops, rows[i].group, rows[i].ops[0], rows[i].ops[1], rows[i].ops[2]);
            if (getenv("X86TD_OPTY")) printf("  opty=|%s|", rows[i].opty);
            printf("\n");
        }
        return 0;
    }

    printf("/* ISA_X86_64.inc -- DERIVED FROM THE LLVM TABLES, not sampled.\n"
           " *\n"
           " * Source of truth: ~/ISA_MAPS/llvm_isa_tables/X86/*.td, the same tables LLVM itself\n"
           " * generates its encoder from. Every opcode byte below is read from a def's template\n"
           " * argument (`def MOV64rr : RI<0x89, MRMDestReg, ...>`), and every field position is\n"
           " * computed from that def's Format class: MRMDestReg puts operand 0 in the reg field\n"
           " * with REX.R, MRMSrcReg puts it in rm with REX.B, and so on.\n"
           " *\n"
           " * Regenerate with `x86td emit > ISA_X86_64.inc`. Unlike a sampled catalog this is\n"
           " * instant, deterministic, and cannot silently drop a family: anything the parser\n"
           " * cannot justify is REPORTED BY NAME on stderr instead of being emitted half-formed.\n"
           " *\n"
           " * Register numbers: 0..7 are the classic registers, 8..15 the r8-r15 bank.\n"
           " */\n"
           "static const IsaForm isa_forms_x86_64[] = {\n");
    int emitted = 0;
    /* A MEMORY ROW IS EMITTED ONCE PER DISPLACEMENT SHAPE.
     *
     * mod=00 (no displacement), mod=01 (disp8) and mod=10 (disp32) are three distinct encodings of
     * the same mnemonic and all three occur in real code, so one row per shape is what makes
     * `leaq -0x1(%rdi),%rcx` (48 8d 4f ff) decode alongside `leaq (%rdi,%rdi,2),%rax`. A row with
     * no memory operand has one shape and is emitted once. */
    for (int i = 0; i < nrow; i++) {
        int has_mem = 0;
        for (int z = 0; z < rows[i].nops; z++) if (rows[i].ops[z] == A_MEM) has_mem = 1;
        if (!has_mem) {
            /* A REGISTER FORM STILL HAS TWO ENCODINGS: the plain one and the REX-led one that names
             * an extended register (`44 01 c0` is `addl %r8d,%eax`). Emitting only the plain form left
             * every extended-register ALU/move instruction unmatched. */
            g_dispmod = 0;
            for (int rx = 0; rx < 2; rx++) {
                g_rex_optional = (rx == 1);
                int before = g_emit_ok;
                emit_row_printf(&rows[i], rows[i].fk, 0);
                if (g_emit_ok != before) emitted++;
            }
            g_rex_optional = 0;
            continue;
        }
        static const int mods[3] = { 0x00, 0x40, 0x80 };
        for (int m = 0; m < 3; m++) {
          for (int sib = 0; sib < 2; sib++) {
           for (int nix = 0; nix < (sib ? 2 : 1); nix++) {
           for (int nbx = 0; nbx < (sib ? 2 : 1); nbx++) {
           for (int rp = 0; rp < 2; rp++) {
           for (int rx = 0; rx < 2; rx++) {
            g_dispmod = mods[m];
            /* THE SIB BYTE AND RIP-RELATIVE ADDRESSING DO NOT EXIST IN 16-BIT ADDRESSING -- but 16-bit
             * addressing does not exist in 64-bit mode at all, so this is always true here. The check
             * is kept explicit because the generator is also pointed at 32-bit tables: an `_8mem` /
             * `lea16` operand type is the 0x67 address-size override, whose ModR/M has neither a SIB
             * byte nor a RIP-relative form. */
            int is16 = strstr(rows[i].opty, "_8mem") != NULL;
            g_make_sib = (sib == 1 && !is16);
            g_riprel = (rp == 1 && !is16);
            g_noindex = (nix == 1);
            g_nobase = (nbx == 1);
            /* THE NO-BASE AND INDEXED SIB PASSES ARE MUTUALLY EXCLUSIVE: one fixes the base at 5,
             * the other carries a real base register. Running both would emit the same row twice
             * with the base slot meaning two different things. */
            if (g_nobase && g_noindex) { continue; }
            /* A RIP-RELATIVE FORM HAS NO SIB AND NO BASE: the two are different addressing modes, so
             * the combination is not an encoding and is skipped rather than emitted as a half-form. */
            if (g_riprel && g_make_sib) { continue; }
            /* THE REX-LED VARIANT APPLIES TO EVERY FORM THAT CAN CARRY ONE.
             *
             * `44 01 c0` is `addl %r8d,%eax` (REX.R) and `44 0f af 04 97` is `imull (%rdi,%rdx,4),%r8d`
             * -- the same instructions as `01 c0` and `0f af 04 97` with a REX prefix naming an
             * extended register. The first attempt gated this pass on opmap == 0 because it produced
             * rows whose opcode byte was masked out; that was a symptom of the PHANTOM duplicate rows
             * (opmap actually 0 for a 0F-escaped def), which are now gone -- the EVEX filter, the
             * multiclass-scope rule and the format-implies-escape rule removed them. With the real
             * opmap in place, layout_of puts the REX at opcode_at-1 and the escape at opcode_at
             * correctly, so the pass is safe for both maps. */
            /* THE REX-LED VARIANT IS FOR A MODRM FORM. A row whose register lives IN THE OPCODE
             * (AddRegFrm) has no ModR/M byte to prefix, and reserving a REX there produced a 1-byte
             * row of `{0x48}` with a 3-bit field over its own opcode -- so it matched `48 b8 ...`
             * (a movabs with a 64-bit immediate, and the very first byte of any 64-bit instruction
             * whose REX.W is 1) and reported it as `DEC32r_alt`, re-encoding it with a zeroed
             * immediate. Those forms reach their extended registers through the K_REXREG window the
             * plain row already carries. */
            g_rex_optional = (rx == 1 && rows[i].fk != F_ADDREG && rows[i].fk != F_RAWSRC
                              && rows[i].fk != F_RAWDST && rows[i].fk != F_RAWDSTSRC);
            int before = g_emit_ok;
            emit_row_printf(&rows[i], rows[i].fk, 0);
            if (g_emit_ok != before) emitted++;
           }
          }
         }
        }
        }
        }
        g_make_sib = 0;
        g_rex_optional = 0;
        g_riprel = 0;
        g_noindex = 0;
        g_nobase = 0;
    }
    /* THE COUNT MUST BE THE ROWS ACTUALLY WRITTEN, NOT THE ROWS ATTEMPTED. Emitting `emitted`
     * (the loop counter) while the emitter silently refused some rows made the catalog claim 993
     * forms over a 919-row array -- so isa_decode read 74 rows of whatever followed the array and
     * `isadb -t x86_64 test` produced no output at all. */
    printf("};\n#define ISA_NFORMS_X86_64 %d\n", g_emit_ok);
    fprintf(stderr, "x86td: %d rows emitted (ok=%d skip_none=%d skip_len=%d)\n",
            emitted, g_emit_ok, g_emit_skip_none, g_emit_skip_len);
    /* THE FULL ACCOUNTING, printed unconditionally: every def the scanner looked at is in exactly
     * one of these buckets, so "how many are left" is answered by the numbers themselves rather
     * than by an estimate. A leftover def is one that was never turned into a row even though it
     * carries an encoding -- `defscanned - nonint - nofmt - nrefused` counts it. */
    fprintf(stderr, "x86td: defs scanned=%d  filtered=%d (VEX/EVEX/pseudo/mode)  no-format=%d  "
                    "refused-no-operands=%d  ->  %d defs became NO ROW AT ALL\n",
            nseen, nonint, nofmt, nrefused, (nseen - nonint - nofmt - nrefused));
    return 0;
}
