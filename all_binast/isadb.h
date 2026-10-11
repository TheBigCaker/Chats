/* isadb.h -- catalog runtime. Hand-writable row format, one model for every ISA.
 *
 * An instruction row is:  len bytes of template + up to 4 variable fields.
 * A field is (byte, bit, width) with LSB-first bit numbering inside each byte and
 * bytes in memory order -- the same numbering the ISA manuals use, so a row can be
 * read against the manual by eye. `scale` is the power-of-two between the human
 * value and the encoded field (a 64-bit LDR offset is byte/8, so scale 3).
 *
 * Contiguous fields cover every instruction in the maps here except the two-piece
 * ADR/ADRP immediate (immlo at bits 0..1 and immhi at bits 5..23), TBZ/TBNZ's split
 * bit number, and RISC-V's scattered branch immediates. Those use K_SPLIT, which
 * names a second (byte, bit, width) via split_byte/split_bit: bit 0 of the human
 * value goes to the low piece, the rest to the high piece.
 *
 * A kind that reads two operands out of one encoded field advances the field index
 * an extra step (see K_UBFMW/K_UBFX/K_BFINS below). That is why a row like UBFM has
 * three field entries but four operands.
 *
 *     #define ISA_INC "ISA_AARCH64.inc"
 *     #include "isadb.h"
 */
#ifndef ISADB_H
#define ISADB_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lopimm.h"

/* field kinds */
#define K_REG    0   /* register number                                  */
#define K_IMM    1   /* unsigned immediate, human = enc << scale         */
#define K_SHIFT  2   /* shift amount in N:immr:imms (AArch64 UBFM alias)  */
#define K_LOGINT 3   /* logical immediate, ARM ARM DecodeBitMasks        */
#define K_UBFMW  4   /* raw N:immr:imms, two operands (immr, imms)        */
#define K_COND   5   /* condition code 0..15                             */
#define K_SIMM   6   /* signed immediate / signed offset                 */
#define K_UBFX   7   /* lsb:width form of UBFM (two operands)             */
#define K_SPLIT  8   /* immediate split into two pieces (ADR/ADRP)        */
#define K_IMMHI  9   /* high 3 bits of a movz/movk shift (<<16/<<32/<<48) */
#define K_REXLO 10    /* low 3 bits of a register live in the opcode, high bit in REX.B */
#define K_PAGCNT 11  /* PC-page-relative page count (ADRP): target = (pc & ~0xfff) + n*4096.
                      * immhi holds the count above bit 1 and immlo its low 2 bits. */
#define K_UBFSH 12  /* lsl by immediate: one operand, two fields. imms = width-1-sh,
                      * immr = ((-sh) mod width) & (width-1). Not a plain field, so the
                      * row cannot be written as (byte,bit,width) alone. */
#define K_BITSPL 13  /* TBZ/TBNZ bit number: low 5 bits in the main field, the 6th bit in
                      * a window at bit 31 (b5), so the operand needs two pieces. */
#define K_SHIFTINV 14 /* vector shift-by-immediate (ushr/sshr/shrn/usra/sri/sli): the
                      * 7-bit field at 22:16 holds 2*element_bits - shift. Measured
                      * over real instructions rather than assumed; the element size
                      * rides in vsc as element_bits/8 because a row has no other slot
                      * for it and the field alone cannot distinguish 4s from 16b. */
#define K_VSHIFTP 15 /* vector up-shift (shl/sli/ushll/sshll): the same 7-bit field holds
                      * element_bits + shift. The two directions cannot share a kind. */
#define K_LANEIX 17  /* structured-lane index ({Vt.s}[i]): the index bits start at
                      * 10 + log2(element bytes) and overflow into the Q bit at 30.
                      * Swept over every index of every element size to find this; the
                      * manual's "bits 20:16" is not where the index lives. */
#define K_REXREG 18  /* an x86-64 register whose high bit lives in a REX field. A row can
                      * carry several: reg in REX.R, rm in REX.B, index in REX.X. K_REXLO
                      * handled only ONE (low 3 bits in the opcode + REX.B), so r8-r15 could
                      * appear in only one operand position, which is why the x86 catalog
                      * could not describe most real instructions. The row's split window
                      * carries the REX bit for THIS operand. */
#define K_REXW   19  /* the REX.W operand-size bit as a field: 1 = 64-bit form, 0 = 32-bit */
#define K_BFINS 16   /* bfi (lsb, width): immr = (bits - lsb) % bits, imms = width - 1.
                      * bfxil uses the same operand pair with a different transform
                      * (immr = lsb, imms = lsb + width - 1), so the two aliases cannot
                      * share a row: one field layout, two meanings. */
#define K_SIBSCALE 20 /* the x86-64 SIB scale as a field: 2 bits holding log2(scale), so the
                      * operand is 1, 2, 4 or 8 and the encoding is 0..3. A raw 2-bit field
                      * cannot express the multiplier the instruction actually uses, and
                      * without this kind a `leaq (%rdi,%rdi,2),%rax` (SIB scale=2) has no row
                      * that can tell its scale=2 from a scale=1. */
#define K_REXREG_5 21 /* a K_REXREG whose value may not be 5. For a SIB base with mod=00 the
                      * value 5 does not mean "rbp" but "no base, disp32 follows" -- a
                      * different addressing mode with a different length, so a row that
                      * accepted base=5 at mod=00 would decode a 4-byte-displacement form as
                      * a no-displacement one. The index field has the same exception with 4
                      * (which means "no index"), and it is spelled K_REXREG_4 below. */
#define K_REXREG_4 22
#define K_REXREG_B4 23 /* a MEMORY BASE that may not be 4: rm=100 means "the next byte is a SIB", so a
                        * plain base of rsp is not encodable as [rsp+disp] -- it is only expressible
                        * through the SIB form. A row that accepted base=4 would claim a disp8 form
                        * over bytes that are really a SIB byte, and its length would be one short. */

#define ISA_MAXBYTES 16
#define ISA_MAXV 6   /* enough for the 5-operand sys/system forms */

/* Row base words are written as ONE number. Spelling a base word as a byte list
 * invites a silently dropped byte (CSEL lost its 0x80 that way, and the row still
 * looked plausible), so the template is always a single literal split by W(). */
#define W(x) (uint8_t)((x) & 0xffu), (uint8_t)(((x) >> 8) & 0xffu), \
             (uint8_t)(((x) >> 16) & 0xffu), (uint8_t)(((x) >> 24) & 0xffu)

typedef struct {
    const char *name;
    const char *mnemonic;
    const char *operands;
    uint8_t     len;
    uint8_t     bytes[ISA_MAXBYTES];   /* template: every field bit is 0    */
    uint8_t     vbyte[ISA_MAXV];       /* byte holding the field's low bit  */
    uint8_t     vbit[ISA_MAXV];        /* bit inside that byte              */
    uint8_t     vwid[ISA_MAXV];        /* field width                       */
    uint8_t     vsc[ISA_MAXV];         /* human = enc << vsc                */
    uint8_t     vkind[ISA_MAXV];
    /* the SECOND window. For K_SPLIT / K_BITSPL / K_PAGCNT it is the row's one extra piece
     * (adrp's immlo, tbz's 6th bit). For K_REXREG it is PER OPERAND -- x86-64 needs a
     * separate REX bit for each register above 7 (REX.R for ModR/M.reg, REX.B for .rm), and
     * one row-wide window can only name one of them. hbyte/hbit are indexed by operand: a
     * zero width there means "this operand has no high bit". */
    uint8_t     split_byte;
    uint8_t     split_bit;
    uint8_t     split_wid;
    uint8_t     nvars;
    /* THE PER-OPERAND REX WINDOWS GO AFTER nvars, and that placement is load-bearing. Every
     * catalog row in the tree is a brace initializer without designators, so adding a field
     * BEFORE nvars shifts nvars and silently zeroes every operand count -- which is exactly
     * what happened: 67 AArch64 fixtures failed with all operands zero and the rows looked
     * structurally fine. Appending keeps every existing initializer's meaning. */
    uint8_t     hbyte[ISA_MAXV];   /* per-operand high-bit window (K_REXREG) */
    uint8_t     hbit[ISA_MAXV];
} IsaForm;

typedef struct {
    const IsaForm *forms;
    uint32_t       n;
    const char    *name;
    int            wordsize;
    int            maxlen;
} IsaTable;

/* ---------------- bit access ---------------- */
/* Fields wider than 32 bits are not supported by these helpers: model a 64-bit
 * immediate as two 32-bit halves (K_SPLIT with split_wid = 32). */
static inline uint32_t isa_get(const uint8_t *b, int byteoff, int bitoff, int width) {
    uint32_t v = 0;
    for (int i = 0; i < width; i++) {
        int bit = byteoff * 8 + bitoff + i;
        if ((b[bit >> 3] >> (bit & 7)) & 1u) v |= 1u << i;
    }
    return v;
}

static inline void isa_put(uint8_t *b, int byteoff, int bitoff, int width, uint32_t v) {
    for (int i = 0; i < width; i++) {
        int bit = byteoff * 8 + bitoff + i;
        if ((v >> i) & 1u) b[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        else               b[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    }
}

/* every bit covered by a row's fields, as a mask over the instruction bytes */
static inline void isa_fieldmask(const IsaForm *f, uint8_t *out) {
    memset(out, 0, ISA_MAXBYTES);
    for (int v = 0; v < f->nvars; v++)
        for (int i = 0; i < f->vwid[v]; i++) {
            int bit = (f->vbyte[v] * 8 + f->vbit[v]) + i;
            out[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        }
    /* A kind that carries a second window must have that window masked out of the
     * template comparison, or a row whose template bit is 0 refuses every word with
     * that bit set. This has to scan EVERY field, not just the last one: a lane row
     * puts its index kind first, and looking only at vkind[nvars-1] silently left the
     * window unmasked so the row could never match. */
    for (int v = 0; v < f->nvars; v++) {
        uint8_t k = f->vkind[v];
        if (k == K_SPLIT || k == K_BITSPL || k == K_PAGCNT || k == K_REXLO || k == K_LANEIX)
            for (int i = 0; i < f->split_wid; i++) {
                int bit = (f->split_byte * 8 + f->split_bit) + i;
                out[bit >> 3] |= (uint8_t)(1u << (bit & 7));
            }
        if ((k == K_REXREG || k == K_REXREG_4 || k == K_REXREG_5 || k == K_REXREG_B4) && f->hbit[v]) {
            int bit = f->hbyte[v] * 8 + (f->hbit[v] - 1);
            out[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        }
    }
}

static inline int isa_template_match(const IsaForm *f, const uint8_t *b) {
    uint8_t mask[ISA_MAXBYTES];
    isa_fieldmask(f, mask);
    for (int i = 0; i < f->len; i++) {
        uint8_t want = f->bytes[i] & (uint8_t)~mask[i];
        if ((b[i] & (uint8_t)~mask[i]) != want) return 0;
    }
    return 1;
}

static inline int isa_is32(const IsaForm *f) {
    for (const char *p = f->operands; *p; p++) {
        if (*p == 'W') return 1;
        if (*p == 'X') return 0;
    }
    return 0;
}

static inline int isa_signext(uint32_t v, int bits) {
    uint32_t m = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
    v &= m;
    if (bits < 32 && (v & (1u << (bits - 1)))) return (int)(v | ~m);
    return (int)v;
}

static inline uint32_t isa_shift_field(int sh, int is32) {
    int width = is32 ? 32 : 64;
    return ((is32 ? 0u : 1u) << 12) | (((unsigned)(sh % width)) << 6) | (unsigned)(width - 1);
}
static inline int isa_shift_from_field(uint32_t enc, int is32) {
    int width = is32 ? 32 : 64;
    unsigned immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
    if ((int)imms != width - 1) return -1;
    return (int)(immr % (unsigned)width);
}

/* Is the word legal for this row's own field semantics (not just template bits)?
 * A word can satisfy a template and still not exist on the ISA; accepting it means
 * decode reports an instruction and encode cannot reproduce the word. */
static inline int isa_legal(const IsaForm *f, const uint8_t *b) {
    int w32 = isa_is32(f);
    for (int v = 0; v < f->nvars; v++) {
        uint32_t enc = isa_get(b, f->vbyte[v], f->vbit[v], f->vwid[v]);
        switch (f->vkind[v]) {
            case K_SHIFT:
                if (isa_shift_from_field(enc, w32) < 0) return 0;
                break;
            case K_LOGINT: {
                unsigned n = (enc >> 12) & 1u, immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                if (w32) { if (n != 0 || lop_decode32(immr, imms) == 0) return 0; }
                else if (lop_decode(n, immr, imms) == 0) return 0;
                break;
            }
            case K_BFINS: {
                int wb = w32 ? 32 : 64;
                unsigned immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                if ((int)immr >= wb || (int)imms >= wb) return 0;
                /* Same 13-bit field as bfxil: bfi is the imms < immr half. Accepting the
                 * other half makes this row swallow bfxil's words and then fail to
                 * re-encode them (the rotation pushes lsb+width past the register). */
                if (imms >= immr) return 0;
                break;
            }
            case K_UBFMW: {
                /* raw N:immr:imms -- no ordering constraint (imms < immr rotates), but
                 * the N bit must match the operand width: N=0 for the 32-bit form, N=1
                 * for the 64-bit one. Accepting N=1 on a W instruction lets a word that
                 * does not exist on the ISA match, and its operands cannot express N, so
                 * the re-encoding silently differs. */
                unsigned n = (enc >> 12) & 1u, immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                int width = w32 ? 32 : 64;
                if (w32 ? n != 0 : n != 1) return 0;
                if ((int)immr >= width || (int)imms >= width) return 0;
                break;
            }
            case K_UBFX: {
                unsigned n = (enc >> 12) & 1u, immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                int width = w32 ? 32 : 64;
                if (w32 ? n != 0 : n != 1) return 0;
                if ((int)immr >= width || (int)imms >= width) return 0;
                if (imms < immr) return 0;
                break;
            }
            case K_UBFSH: {
                unsigned n = (enc >> 12) & 1u;
                if (w32 ? n != 0 : n != 1) return 0;
                break;
            }
            case K_BITSPL: {
                unsigned hi = isa_get(b, f->split_byte, f->split_bit, f->split_wid);
                unsigned bit = (hi << f->vwid[v]) | (enc & ((1u << f->vwid[v]) - 1u));
                if (bit >= (unsigned)(w32 ? 32 : 64)) return 0;
                break;
            }
            case K_LANEIX: {
                int eb = (int)f->vsc[v] > 1 ? (int)f->vsc[v] : 1;   /* element bytes */
                unsigned lim = 16u / (unsigned)eb;                  /* lanes per vector */
                unsigned q = isa_get(b, f->split_byte, f->split_bit, f->split_wid);
                unsigned ix = (q << f->vwid[v]) | enc;
                if (ix >= lim) return 0;
                break;
            }
            case K_SHIFTINV:
                if ((int)enc > 2 * (int)f->vsc[v] * 8) return 0;
                break;
            case K_VSHIFTP:
                /* the field holds element_bits + shift, so its top value is 2*element_bits.
                 * Comparing against element_bits alone rejected every shift above 0 for a
                 * 16-byte arrangement (field 10 for shl #2) and made correct rows miss. */
                if ((int)enc > 2 * (int)f->vsc[v] * 8 || (int)enc < 0) return 0;
                break;
            case K_COND:
                if (enc > 15) return 0;
                break;
            case K_REXREG_4: case K_REXREG_B4: {
                /* index 4 means "no index" in the SIB byte, so it is not a register. The value is
                 * the low bits here plus this operand's high bit, exactly as K_REXREG extracts. */
                unsigned hi4 = f->hbit[v] ? isa_get(b, f->hbyte[v], f->hbit[v] - 1, 1) : 0u;
                unsigned r4 = ((hi4 & 1u) << f->vwid[v]) | (enc & ((1u << f->vwid[v]) - 1u));
                if (r4 == 4) return 0;
                break;
            }
            case K_REXREG_5: {
                unsigned hi5 = f->hbit[v] ? isa_get(b, f->hbyte[v], f->hbit[v] - 1, 1) : 0u;
                unsigned r5 = ((hi5 & 1u) << f->vwid[v]) | (enc & ((1u << f->vwid[v]) - 1u));
                if (r5 == 5) return 0;
                break;
            }
            case K_SIBSCALE:
                if (enc > 3) return 0;
                break;
            default:
                break;
        }
    }
    return 1;
}

static inline int isa_extract(const IsaForm *f, const uint8_t *b, long *human) {
    int w32 = isa_is32(f);
    for (int v = 0; v < f->nvars; v++) {
        uint32_t enc = isa_get(b, f->vbyte[v], f->vbit[v], f->vwid[v]);
        switch (f->vkind[v]) {
            case K_SHIFT: {
                int sh = isa_shift_from_field(enc, w32);
                if (sh < 0) return 0;
                human[v] = sh;
                break;
            }
            case K_LOGINT: {
                unsigned n = (enc >> 12) & 1u, immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                if (w32) { uint32_t p = lop_decode32(immr, imms); if (!p || n) return 0; human[v] = (long)p; }
                else { uint64_t p = lop_decode(n, immr, imms); if (!p) return 0; human[v] = (long)p; }
                break;
            }
            case K_BFINS: {
                int wb = w32 ? 32 : 64;
                unsigned immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                if ((int)immr >= wb || (int)imms >= wb || imms >= immr) return 0;
                human[v] = (long)((wb - (int)immr) % wb);          /* lsb */
                if (v + 1 < f->nvars) { human[v + 1] = (long)imms + 1; v++; }
                break;
            }
            case K_UBFMW: case K_UBFX: {
                unsigned n = (enc >> 12) & 1u, immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                int width = w32 ? 32 : 64;
                if (w32 ? n != 0 : n != 1) return 0;
                if ((int)immr >= width || (int)imms >= width) return 0;
                if (f->vkind[v] == K_UBFX && imms < immr) return 0;
                human[v] = (long)immr;
                if (v + 1 < f->nvars) {
                    human[v + 1] = (f->vkind[v] == K_UBFX) ? (long)(imms - immr + 1) : (long)imms;
                    v++;
                }
                break;
            }
            case K_UBFSH: {
                /* one operand in two fields: imms = width-1-sh, immr = (-sh) mod width */
                unsigned n = (enc >> 12) & 1u, immr = (enc >> 6) & 0x3f, imms = enc & 0x3f;
                int width = w32 ? 32 : 64;
                if (w32 ? n != 0 : n != 1) return 0;
                int sh_from_imms = width - 1 - (int)imms;
                if (sh_from_imms < 0 || sh_from_imms >= width) return 0;
                int want_immr = (int)(((unsigned)(-(unsigned)sh_from_imms)) % (unsigned)width) & (width - 1);
                if ((int)immr != want_immr) return 0;
                human[v] = sh_from_imms;
                break;
            }
            case K_SPLIT: {
                /* isa_get returns uint32_t and `enc << 32` on a uint32_t is UNDEFINED -- it
                 * yielded the low half alone (0x55667788 instead of 0x1122334455667788), so a
                 * decoded movabs lost its upper four bytes and re-encoded with them zeroed.
                 * Do the shift in 64 bits and return a long, which is what `human` is. */
                unsigned long long lo = (unsigned long long)isa_get(b, f->split_byte,
                                                                  f->split_bit, f->split_wid);
                unsigned long long hi = (unsigned long long)enc;
                human[v] = (long)((hi << f->split_wid) | lo);
                break;
            }
            case K_BITSPL: {
                unsigned hi = isa_get(b, f->split_byte, f->split_bit, f->split_wid);
                unsigned bit = (hi << f->vwid[v]) | (enc & ((1u << f->vwid[v]) - 1u));
                if (bit >= (unsigned)(w32 ? 32 : 64)) return 0;
                human[v] = (long)bit;
                break;
            }
            case K_LANEIX: {
                unsigned q = isa_get(b, f->split_byte, f->split_bit, f->split_wid);
                unsigned ix = (q << f->vwid[v]) | enc;
                int eb = (int)f->vsc[v] > 1 ? (int)f->vsc[v] : 1;
                if (ix >= 16u / (unsigned)eb) return 0;
                human[v] = (long)ix;
                break;
            }
            case K_PAGCNT: {
                unsigned lo = isa_get(b, f->split_byte, f->split_bit, f->split_wid);
                unsigned cnt = (enc << f->split_wid) | lo;
                human[v] = (long)isa_signext(cnt, f->vwid[v] + f->split_wid);
                break;
            }
            case K_REXREG: case K_REXREG_4: case K_REXREG_5: case K_REXREG_B4: {
                /* the low bits here, the high bit in THIS OPERAND's window (REX.R / REX.B /
                 * REX.X depending on the operand). 0..15 is a complete x86-64 register
                 * number, so the caller gets a plain register and never touches REX.
                 * The per-operand window is what makes two extended registers in one
                 * instruction possible; a single row-wide window can only carry one. */
                unsigned hi = f->hbit[v] ? isa_get(b, f->hbyte[v], f->hbit[v] - 1, 1) : 0u;
                human[v] = (long)(((hi & 1u) << f->vwid[v]) | (enc & ((1u << f->vwid[v]) - 1u)));
                break;
            }
            case K_REXW:
                human[v] = (long)(enc & 1u);
                break;
            case K_SIBSCALE:
                /* the operand is the MULTIPLIER, so 0..3 becomes 1,2,4,8 */
                human[v] = (long)(1u << (enc & 3u));
                break;
            case K_IMMHI:
                human[v] = (long)(enc * 16);      /* hw field is shift/16 */
                break;
            case K_SHIFTINV:
                /* field = 2*element_bits - shift */
                human[v] = (long)(2 * (int)f->vsc[v] * 8 - (int)enc);
                break;
            case K_VSHIFTP:
                /* field = element_bits + shift */
                human[v] = (long)((int)enc - (int)f->vsc[v] * 8);
                break;
            case K_SIMM:
                human[v] = (long)(isa_signext(enc, f->vwid[v]) << f->vsc[v]);
                break;
            case K_COND:
                if (enc > 15) return 0;
                human[v] = (long)enc;
                break;
            default:
                human[v] = (long)(enc << f->vsc[v]);
                break;
        }
    }
    return 1;
}

static inline int isa_encode(const IsaForm *f, const long *human, uint8_t *out,
                            char *err, size_t errn) {
    memcpy(out, f->bytes, ISA_MAXBYTES);
    int w32 = isa_is32(f), width = w32 ? 32 : 64;
    /* v is at once the field slot (vbyte/vbit/vwid/vkind) and the operand slot. A kind
     * whose operand pair is one encoded field (immr:imms, lsb:width) advances v an extra
     * step to absorb the second operand, which is why UBFM's row has three field entries
     * for four operands. */
    for (int v = 0; v < f->nvars; v++) {
        long hv = human[v];
        /* A ZERO-WIDTH SLOT ENCODES NOTHING, BY DEFINITION.
         *
         * Some rows carry a slot with no field: a RIP-relative memory operand has no base register to
         * encode (`movq (%rip),%rdx` -- rm is the constant 5), so the row keeps the operand for the
         * disassembly but gives it vwid 0. Falling through to the generic path then asked the encoder
         * to write a value into a 0-bit field and refused, so every RIP-relative and no-base row
         * failed `isadb test` with "value N does not fit 0 bits" while being perfectly correct. The
         * guard belongs here rather than in the generator: a slot with no bits is not a field on any
         * row, whoever wrote it. */
        if (f->vwid[v] == 0) continue;
        switch (f->vkind[v]) {
            case K_SHIFT:
                if (hv < 0 || hv >= width) { snprintf(err, errn, "%s: shift %ld out of range 0..%d", f->name, hv, width - 1); return -1; }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], isa_shift_field((int)hv, w32));
                break;
            case K_LOGINT: {
                unsigned field = 0;
                int ok = w32 ? lop_encode32((uint32_t)hv, &field) : lop_encode((uint64_t)hv, &field);
                if (!ok) { snprintf(err, errn, "%s: 0x%lx is not an encodable logical immediate", f->name, hv); return -1; }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], field);
                break;
            }
            case K_BFINS: {
                int width_bits = w32 ? 32 : 64;
                long lsb = hv, wd = (v + 1 < f->nvars) ? human[v + 1] : 1;
                if (lsb < 0 || lsb >= width_bits || wd < 1 || lsb + wd > width_bits) {
                    snprintf(err, errn, "%s: lsb %ld + width %ld exceeds %d bits", f->name, lsb, wd, width_bits);
                    return -1;
                }
                long immr = (width_bits - lsb) % width_bits, imms = wd - 1;
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v],
                        (uint32_t)(((w32 ? 0u : 1u) << 12) | ((unsigned)immr << 6) | (unsigned)imms));
                v++;
                break;
            }
            case K_UBFMW: case K_UBFX: {
                long lo = hv, hi = (v + 1 < f->nvars) ? human[v + 1] : 0, immr, imms;
                if (f->vkind[v] == K_UBFX) {
                    if (lo < 0 || hi < 1 || lo + hi > width) { snprintf(err, errn, "%s: lsb+width exceeds %d bits", f->name, width); return -1; }
                    immr = lo; imms = lo + hi - 1;
                } else {
                    if (lo < 0 || lo >= width || hi < 0 || hi >= width) { snprintf(err, errn, "%s: immr/imms out of range", f->name); return -1; }
                    immr = lo; imms = hi;
                }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v],
                        ((w32 ? 0u : 1u) << 12) | ((unsigned)immr << 6) | (unsigned)imms);
                v++;
                break;
            }
            case K_UBFSH: {
                long sh = hv;
                if (sh < 0 || sh >= width) { snprintf(err, errn, "%s: shift %ld out of range 0..%d", f->name, sh, width - 1); return -1; }
                unsigned immr = ((unsigned)(-(unsigned)sh)) % (unsigned)width;
                unsigned imms = (unsigned)(width - 1 - sh);
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v],
                        ((w32 ? 0u : 1u) << 12) | (immr << 6) | imms);
                break;
            }
            case K_SPLIT: {
                /* LOW piece goes in the split window, the rest in the main window -- which is
                 * the inverse of isa_extract (human = (main << split_wid) | split_lo), so the
                 * pair round-trips.
                 *
                 * THE >= 32 CASE IS A 64-BIT IMMEDIATE and it was broken: `hi` was forced to 0
                 * whenever split_wid >= 32, which threw away the entire high half. That guard
                 * existed because `hv >> 32` on a 32-bit unsigned is undefined; the fix is to
                 * do the arithmetic in 64 bits rather than to discard the value. Symptom: a
                 * movabs row decoded correctly and re-encoded with its upper four bytes zero. */
                unsigned long long u = (unsigned long long)hv;
                unsigned long long lomask = (f->split_wid >= 32) ? 0xFFFFFFFFull
                                                                 : ((1ull << f->split_wid) - 1ull);
                unsigned long long lo = u & lomask;
                unsigned long long hi = u >> f->split_wid;
                if (f->vwid[v] < 64 && (hi >> f->vwid[v]) != 0) {
                    snprintf(err, errn, "%s: value %ld does not fit %d bits", f->name, hv,
                             f->split_wid + f->vwid[v]);
                    return -1;
                }
                isa_put(out, f->split_byte, f->split_bit, f->split_wid, (uint32_t)lo);
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)hi);
                break;
            }
            case K_BITSPL: {
                unsigned lim = (unsigned)(w32 ? 32 : 64);
                if (hv < 0 || (unsigned long)hv >= lim) {
                    snprintf(err, errn, "%s: bit %ld out of range", f->name, hv);
                    return -1;
                }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)hv);
                isa_put(out, f->split_byte, f->split_bit, f->split_wid,
                        ((uint32_t)hv >> f->vwid[v]) & ((1u << f->split_wid) - 1u));
                break;
            }
            case K_LANEIX: {
                int eb = (int)f->vsc[v] > 1 ? (int)f->vsc[v] : 1;
                unsigned lim = 16u / (unsigned)eb;
                if (hv < 0 || (unsigned long)hv >= lim) {
                    snprintf(err, errn, "%s: lane %ld out of range 0..%u for a %d-byte element",
                             f->name, hv, lim - 1, eb);
                    return -1;
                }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)hv & ((1u << f->vwid[v]) - 1u));
                isa_put(out, f->split_byte, f->split_bit, f->split_wid,
                        ((uint32_t)hv >> f->vwid[v]) & ((1u << f->split_wid) - 1u));
                break;
            }
            case K_PAGCNT: {
                long r = hv;
                if (r < -(1L << 20) || r > (1L << 20) - 1) {
                    snprintf(err, errn, "%s: %ld pages out of range (+-2^20)", f->name, r);
                    return -1;
                }
                unsigned field = (unsigned)(r & 0x1fffff);
                isa_put(out, f->vbyte[v], f->vbit[v], 19, (field >> 2) & 0x7ffff);
                isa_put(out, f->split_byte, f->split_bit, 2, field & 3);
                break;
            }
            case K_REXREG: case K_REXREG_4: case K_REXREG_5: case K_REXREG_B4: {
                if (hv < 0 || hv > 15) { snprintf(err, errn, "%s: register %ld out of range 0..15", f->name, hv); return -1; }
                if (f->vkind[v] == K_REXREG_4 && hv == 4) {
                    snprintf(err, errn, "%s: register 4 is not encodable here (means no index)", f->name);
                    return -1;
                }
                if (f->vkind[v] == K_REXREG_B4 && hv == 4) {
                    snprintf(err, errn, "%s: rsp is not encodable as a plain base (means SIB follows)", f->name);
                    return -1;
                }
                if (f->vkind[v] == K_REXREG_5 && hv == 5) {
                    snprintf(err, errn, "%s: register 5 is not encodable here (means no base)", f->name);
                    return -1;
                }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)hv & ((1u << f->vwid[v]) - 1u));
                if (f->hbit[v]) isa_put(out, f->hbyte[v], f->hbit[v] - 1, 1,
                                        ((uint32_t)hv >> f->vwid[v]) & 1u);
                break;
            }
            case K_REXW:
                if (hv != 0 && hv != 1) { snprintf(err, errn, "%s: REX.W must be 0 or 1", f->name); return -1; }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)hv);
                break;
            case K_SIBSCALE: {
                if (hv != 1 && hv != 2 && hv != 4 && hv != 8) {
                    snprintf(err, errn, "%s: scale must be 1, 2, 4 or 8", f->name);
                    return -1;
                }
                unsigned lg = 0;
                while ((1u << lg) != (unsigned)hv) lg++;
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], lg);
                break;
            }
            case K_REXLO: {
                /* low 3 bits go in the opcode, the 4th into REX.B at split_byte/bit */
                if (hv < 0 || hv > 15) { snprintf(err, errn, "%s: register %ld out of range", f->name, hv); return -1; }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)hv & 7u);
                isa_put(out, f->split_byte, f->split_bit, f->split_wid,
                        ((uint32_t)hv >> 3) & ((1u << f->split_wid) - 1u));
                break;
            }
            case K_IMMHI: {
                if (hv != 0 && hv != 16 && hv != 32 && hv != 48) {
                    snprintf(err, errn, "%s: shift must be 0/16/32/48", f->name); return -1;
                }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)(hv / 16));
                break;
            }
            case K_SHIFTINV: {
                int elem = (int)f->vsc[v] * 8;
                if (hv < 1 || hv > elem) {
                    snprintf(err, errn, "%s: shift %ld out of range 1..%d", f->name, hv, elem);
                    return -1;
                }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)(2 * elem - hv));
                break;
            }
            case K_VSHIFTP: {
                int elem = (int)f->vsc[v] * 8;
                if (hv < 0 || hv > elem) {
                    snprintf(err, errn, "%s: shift %ld out of range 0..%d", f->name, hv, elem);
                    return -1;
                }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)(elem + hv));
                break;
            }
            case K_SIMM: {
                /* signed field: human value is a signed multiple of (1 << scale) */
                int bits = f->vwid[v];
                int sh = f->vsc[v];
                long lo = -(1L << (bits - 1)), hi = (1L << (bits - 1)) - 1;
                lo <<= sh; hi <<= sh;
                if (hv < lo || hv > hi) { snprintf(err, errn, "%s: %ld out of range %ld..%ld", f->name, hv, lo, hi); return -1; }
                if (sh && (hv % (1L << sh)) != 0) {
                    snprintf(err, errn, "%s: %ld is not a multiple of %ld (field scale)", f->name, hv, 1L << sh);
                    return -1;
                }
                uint32_t m = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
                isa_put(out, f->vbyte[v], f->vbit[v], bits, ((uint32_t)(hv / (1L << sh))) & m);
                break;
            }
            case K_COND:
                if (hv < 0 || hv > 15) { snprintf(err, errn, "%s: condition %ld out of range", f->name, hv); return -1; }
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], (uint32_t)hv);
                break;
            default: {
                int sh = f->vsc[v];
                uint32_t raw = sh ? ((uint32_t)hv >> sh) : (uint32_t)hv;
                if (sh && (uint32_t)hv != (raw << sh)) {
                    snprintf(err, errn, "%s: value %ld is not a multiple of %u", f->name, hv, 1u << sh);
                    return -1;
                }
                /* Fit check without shifting by the full width (UB, and it
                 * rejects exactly-fitting values like 1<<31). */
                {
                    int bits = f->vwid[v] + sh;
                    unsigned long long lim = (bits >= 64) ? ~0ull : (1ull << bits);
                    if ((unsigned long long)hv >= lim) {
                        snprintf(err, errn, "%s: value %ld does not fit %d bits at scale %u",
                                 f->name, hv, f->vwid[v], 1u << sh);
                        return -1;
                    }
                }
                uint32_t m = (f->vwid[v] >= 32) ? 0xffffffffu : ((1u << f->vwid[v]) - 1u);
                isa_put(out, f->vbyte[v], f->vbit[v], f->vwid[v], raw & m);
                break;
            }
        }
    }
    return 0;
}

/* Decode at a byte offset. Returns bytes consumed, 0 if nothing matched. */
/* THE LEGACY PREFIX RUN, AND A DECODE THAT SKIPS IT.
 *
 * A legacy prefix can repeat and a compiler emits repeats for alignment:
 *
 *     66 66 66 66 2e 0f af c2    imull %edx,%eax, padded to a 16-byte boundary
 *
 * No catalog can carry a row per combination (there are unbounded runs of 66, and any ordering of the
 * segment overrides, the lock byte and rep), so the run is consumed before matching and its length
 * added to the result. This is the architectural rule -- a prefix is a prefix, not part of the opcode
 * -- and it is not a shortcut: a word is accepted only when the bytes AFTER the run match a row, and
 * the effect of 0x66 (16-bit operand size) is checked against the row's own operand-size bit so a
 * prefixed 32-bit form cannot masquerade as its 16-bit twin.
 */
static inline int isa_prefix_len(const uint8_t *b, int avail) {
    int n = 0;
    if (avail <= 1) return 0;
    /* THE RUN CAN BE LONG. A 16-byte-aligned nop from clang is
     *     66 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00
     * -- SIX operand-size prefixes plus a CS override, eight bytes before the opcode. A cap of 14
     * could not reach the opcode in that word and the instruction reported no match. The limit is the
     * 15-byte architectural maximum for an instruction, so the run may be at most 14. */
    while (n < avail - 1 && n < 14) {
        uint8_t c = b[n];
        if (c == 0x66 || c == 0xF2 || c == 0xF3 || c == 0xF0) { n++; continue; }
        if (c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 || c == 0x64 || c == 0x65) { n++; continue; }
        break;
    }
    return n;
}

/* match ONE row at a byte position, filling *len with the row's length */
static inline int isa_match_at(const IsaTable *t, const uint8_t *b, int avail,
                               const IsaForm **out, long *human, int *len) {
    const IsaForm *best = NULL;
    long bh[ISA_MAXV] = {0};
    int best_len = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        const IsaForm *f = &t->forms[i];
        if (f->len > avail) continue;
        if (!isa_template_match(f, b)) continue;
        if (!isa_legal(f, b)) continue;
        long h[ISA_MAXV] = {0};
        if (!isa_extract(f, b, h)) continue;
        if (!best || f->len > best_len) {
            best = f; best_len = f->len;
            for (int z = 0; z < ISA_MAXV; z++) bh[z] = h[z];
        } else if (f->len == best_len) {
            /* EQUAL LENGTH: PREFER THE ROW WITH MORE FIXED (UNMASKED) BYTES, THEN MORE FIELDS.
             *
             * Two rows can describe the same span -- `movabsq $imm64,%rax` (`48 b8` + 8 bytes) and a
             * 1-byte row that masks a register out of its own opcode both claim the same bytes here.
             * The first in array order was winning, which reported the movabs as `DEC32r_alt` and
             * re-encoded it with a zeroed immediate. A row that fixes MORE bytes is the more specific
             * description: that is the ordering that picks `48 b8` over `48 0x`. */
            uint8_t mask_new[ISA_MAXBYTES], mask_best[ISA_MAXBYTES];
            isa_fieldmask(f, mask_new);
            isa_fieldmask(best, mask_best);
            int fix_new = 0, fix_best = 0;
            for (int z = 0; z < f->len; z++) if (!(mask_new[z] & 0xff)) fix_new++;
            for (int z = 0; z < best->len; z++) if (!(mask_best[z] & 0xff)) fix_best++;
            if (fix_new != fix_best) {
                if (fix_new > fix_best) {
                    best = f;
                    for (int z = 0; z < ISA_MAXV; z++) bh[z] = h[z];
                }
                continue;
            }
            int nf_new = 0, nf_best = 0;
            for (int z = 0; z < f->nvars; z++) if (f->vwid[z]) nf_new++;
            for (int z = 0; z < best->nvars; z++) if (best->vwid[z]) nf_best++;
            if (nf_new > nf_best) {
                best = f;
                for (int z = 0; z < ISA_MAXV; z++) bh[z] = h[z];
            }
        }
    }
    if (!best) return 0;
    *out = best;
    *len = best_len;
    if (human) for (int z = 0; z < ISA_MAXV; z++) human[z] = bh[z];
    return 1;
}

/* Decode at a byte offset. Returns bytes consumed, 0 if nothing matched. */
static inline int isa_decode(const IsaTable *t, const uint8_t *b, int avail,
                            const IsaForm **out, long *human) {
    const IsaForm *f = NULL;
    long h[ISA_MAXV] = {0};
    int len = 0;

    /* the plain form first, so a row that IS the prefixed encoding (a 0x66-marked 16-bit form) wins
     * over the same bytes read with the prefix stripped */
    if (isa_match_at(t, b, avail, &f, h, &len)) {
        *out = f;
        if (human) for (int z = 0; z < ISA_MAXV; z++) human[z] = h[z];
        return len;
    }

    /* then past a legacy prefix run, with the run counted as part of the instruction */
    int plen = isa_prefix_len(b, avail);
    if (plen > 0 && isa_match_at(t, b + plen, avail - plen, &f, h, &len)) {
        *out = f;
        if (human) for (int z = 0; z < ISA_MAXV; z++) human[z] = h[z];
        return len + plen;
    }
    return 0;
}

static inline void isa_hex(const uint8_t *b, int len, char *out, size_t n) {
    size_t k = 0;
    for (int i = 0; i < len && k + 3 < n; i++)
        k += (size_t)snprintf(out + k, n - k, i ? " %02x" : "%02x", b[i]);
}

static inline const IsaForm *isadb_find(const IsaTable *t, const char *name) {
    for (uint32_t i = 0; i < t->n; i++) if (!strcmp(t->forms[i].name, name)) return &t->forms[i];
    return NULL;
}

#endif /* ISADB_H */
