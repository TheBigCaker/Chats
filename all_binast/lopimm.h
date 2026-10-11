/* lopimm.h -- AArch64 logical-immediate field (N:immr:imms) encode/decode.
 *
 * Straight from the ARM ARM pseudocode (DecodeBitMasks / A64LogicalImmediate).
 * Encoding is a brute-force search over the legal field values: instant, and it
 * cannot be subtly wrong the way a clever bit-twiddling derivation can.
 *
 * Pure C11, no dependencies.
 */
#ifndef LOPIMM_H
#define LOPIMM_H

#include <stdint.h>

/* Decode N:immr:imms to a 64-bit pattern. Returns 0 for reserved encodings.
 *
 * The rotation is performed within the element size, not within 64 bits. That is
 * the whole point of the encoding and it is easy to get wrong: rotating a 7-bit
 * element right by 31 within 64 bits shifts every bit off the top and yields zero,
 * which makes a legal immediate look reserved. The ISA rotates a 32-bit element,
 * so the same value gives 0xfe. Getting this wrong rejects legal logical
 * immediates -- `and w8, w8, #0xfe` among them -- while every self-check still
 * passes, because encode and decode agreed on the same wrong answer. */
static inline uint64_t lop_decode(unsigned n, unsigned immr, unsigned imms) {
    unsigned t = ((n & 1u) << 6) | ((~imms) & 0x3fu);
    int len = -1;
    for (int b = 6; b >= 0; b--) if ((t >> b) & 1u) { len = b; break; }
    if (len < 1) return 0;                     /* reserved: no element size */
    unsigned esize = 1u << len;                /* 2 .. 64 */
    unsigned levels = esize - 1u;
    unsigned S = imms & levels, R = immr & levels;
    if (S == levels) return 0;                 /* reserved */
    uint64_t elem_mask = (esize >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << esize) - 1);
    uint64_t welem = (S + 1 >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << (S + 1)) - 1);
    welem &= elem_mask;
    unsigned r = R % esize;
    uint64_t elem = r ? (((welem >> r) | (welem << (esize - r))) & elem_mask) : welem;
    if (!elem) return 0;
    uint64_t out = 0;
    for (unsigned k = 0; k < 64 / esize; k++) out |= elem << (k * esize);
    return out;
}

/* Encode a 64-bit logical immediate. 1 on success, field in *field_out. */
static inline int lop_encode(uint64_t want, unsigned *field_out) {
    if (want == 0 || want == ~(uint64_t)0) return 0;
    for (unsigned n = 0; n < 2; n++) {
        for (unsigned immr = 0; immr < 64; immr++) {
            for (unsigned imms = 0; imms < 64; imms++) {
                if (lop_decode(n, immr, imms) == want) {
                    *field_out = (n << 12) | (immr << 6) | imms;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* 32-bit logical immediate. AArch64 restricts the 32-bit form to N=0 with the
 * pattern repeating every 32 bits; a 64-bit search would happily return an N=1
 * field that reproduces the same low 32 bits and then fails to round-trip. */
static inline int lop_encode32(uint32_t want, unsigned *field_out) {
    if (want == 0 || want == 0xffffffffu) return 0;
    for (unsigned immr = 0; immr < 64; immr++)
        for (unsigned imms = 0; imms < 64; imms++) {
            uint64_t v = lop_decode(0, immr, imms);
            if (!v) continue;
            if ((uint32_t)v != want) continue;
            if ((v >> 32) != (v & 0xffffffffu)) continue;
            *field_out = (immr << 6) | imms;
            return 1;
        }
    return 0;
}

/* Decode a 32-bit form's field. 0 if the field is not a legal 32-bit immediate. */
static inline uint32_t lop_decode32(unsigned immr, unsigned imms) {
    uint64_t v = lop_decode(0, immr, imms);
    if (!v) return 0;
    if ((v >> 32) != (v & 0xffffffffu)) return 0;
    return (uint32_t)v;
}

#endif /* LOPIMM_H */
