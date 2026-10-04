/* =========================================================================
 *  poly_binary_gearbox.h
 *  =========================================================================
 *  HyperLang Sub-Scalar Triad & Poly-Dimensional Binary Cryptography Engine
 *
 *  CHAMELEON LEGACY C99 INTEROP:
 *  Every sub-scalar bool struct maintains canonical C99 ABI compliance:
 *  - Offset 0 is ALWAYS `bool legacy_val` (guaranteed 0x01 for TRUE, 0x00 for FALSE).
 *  - Any legacy C library, system call, or ABI expecting `bool*` or `bool`
 *    can dereference offset 0 directly with zero translation and zero copy!
 *
 *  THE SUB-SCALAR TRIAD:
 *  1. NanoBool  (4-bit active gear payload + canonical C99 bool at offset 0)
 *  2. MicroBool (8-bit active gear payload + canonical C99 bool at offset 0)
 *  3. MiniBool  (16-bit clockwork gear payload + canonical C99 bool at offset 0)
 * ========================================================================= */

#ifndef POLY_BINARY_GEARBOX_H
#define POLY_BINARY_GEARBOX_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. NANOBOOL (Chameleon Sub-Byte Gear)
 *    Offset 0: `bool legacy_val` (0x01 for TRUE, 0x00 for FALSE)
 *    Offset 1: 4-bit gear payload:
 *              [3:2] Quadrant (0..3 => 0 deg, 90 deg, 180 deg, 270 deg)
 *              [1]   Tension (0=Slack, 1=Taut)
 *              [0]   Spin (0=CCW, 1=CW)
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    bool    legacy_val; /* Offset 0: Exact canonical C99 bool (0x01 or 0x00) */
    uint8_t gears;      /* Offset 1: Active 4-bit sub-byte gear */
} NanoBool;

static inline NanoBool nanobool_create(uint8_t quadrant, bool taut, bool spin_cw) {
    NanoBool nb;
    bool is_true = (quadrant & 0x03) < 2; /* Northern quadrants (0, 1) = True */
    nb.legacy_val = is_true;
    nb.gears = (uint8_t)(((quadrant & 0x03) << 2) | ((taut ? 1 : 0) << 1) | (spin_cw ? 1 : 0));
    return nb;
}

static inline bool    nanobool_collapse(NanoBool nb) { return nb.legacy_val; }
static inline uint8_t nanobool_quadrant(NanoBool nb) { return (nb.gears >> 2) & 0x03; }
static inline bool    nanobool_is_taut(NanoBool nb)  { return (nb.gears & 0x02) != 0; }
static inline bool    nanobool_is_cw(NanoBool nb)    { return (nb.gears & 0x01) != 0; }

/* Pack two NanoBool gear payloads into 1 byte for storage */
static inline uint8_t nanobool_pack_pair(NanoBool high, NanoBool low) {
    return (uint8_t)(((high.gears & 0x0F) << 4) | (low.gears & 0x0F));
}

static inline void nanobool_unpack_pair(uint8_t byte, NanoBool *high, NanoBool *low) {
    if (high) {
        uint8_t g = (byte >> 4) & 0x0F;
        *high = nanobool_create((g >> 2) & 0x03, (g & 0x02) != 0, (g & 0x01) != 0);
    }
    if (low) {
        uint8_t g = byte & 0x0F;
        *low = nanobool_create((g >> 2) & 0x03, (g & 0x02) != 0, (g & 0x01) != 0);
    }
}

/* =========================================================================
 * 2. MICROBOOL (Chameleon Newtonian Bit)
 *    Offset 0: `bool legacy_val` (0x01 for TRUE, 0x00 for FALSE)
 *    Offset 1: 8-bit gear payload:
 *              [7:5] 8-Sector Phase (0..7 => 0 deg to 315 deg)
 *              [4:3] Tension Spring Load (0..3)
 *              [2:1] Friction Damping (0..3)
 *              [0]   Chirality / Spin (0=CCW, 1=CW)
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    bool    legacy_val; /* Offset 0: Exact canonical C99 bool (0x01 or 0x00) */
    uint8_t gears;      /* Offset 1: Full 8-bit Newtonian gear */
} MicroBool;

static inline MicroBool microbool_create(uint8_t sector_8, uint8_t tension_4, uint8_t damping_4, bool spin_cw) {
    MicroBool mb;
    bool is_true = (sector_8 & 0x07) < 4; /* Northern Hemisphere = True */
    mb.legacy_val = is_true;
    mb.gears = (uint8_t)(((sector_8 & 0x07) << 5) |
                         ((tension_4 & 0x03) << 3) |
                         ((damping_4 & 0x03) << 1) |
                         (spin_cw ? 1 : 0));
    return mb;
}

static inline bool    microbool_collapse(MicroBool mb) { return mb.legacy_val; }
static inline uint8_t microbool_phase(MicroBool mb)    { return (mb.gears >> 5) & 0x07; }
static inline uint8_t microbool_tension(MicroBool mb)  { return (mb.gears >> 3) & 0x03; }
static inline uint8_t microbool_damping(MicroBool mb)  { return (mb.gears >> 1) & 0x03; }
static inline bool    microbool_spin(MicroBool mb)     { return (mb.gears & 0x01) != 0; }

static inline MicroBool microbool_rotate(MicroBool mb, int delta_ticks) {
    uint8_t new_sector = (microbool_phase(mb) + delta_ticks) & 0x07;
    return microbool_create(new_sector, microbool_tension(mb), microbool_damping(mb), microbool_spin(mb));
}

/* =========================================================================
 * 3. MINIBOOL (Chameleon Precision Clockwork)
 *    Offset 0: `bool legacy_val` (0x01 for TRUE, 0x00 for FALSE)
 *    Offset 1: 256-Tick Continuous Phase Wheel (0..255)
 *    Offset 2: Mechanics (High 4 = Spring Load, Low 4 = Momentum)
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    bool    legacy_val;   /* Offset 0: Exact canonical C99 bool (0x01 or 0x00) */
    uint8_t phase_wheel;  /* Offset 1: 256-tick escapement */
    uint8_t mechanics;    /* Offset 2: High 4 = Tension, Low 4 = Momentum */
} MiniBool;

static inline MiniBool minibool_create(uint8_t phase, uint8_t tension, uint8_t momentum) {
    MiniBool mb;
    mb.legacy_val = (phase < 128); /* Northern hemisphere = True */
    mb.phase_wheel = phase;
    mb.mechanics = (uint8_t)(((tension & 0x0F) << 4) | (momentum & 0x0F));
    return mb;
}

static inline bool    minibool_collapse(MiniBool mb) { return mb.legacy_val; }
static inline uint8_t minibool_phase(MiniBool mb)    { return mb.phase_wheel; }
static inline uint8_t minibool_tension(MiniBool mb)  { return (mb.mechanics >> 4) & 0x0F; }
static inline uint8_t minibool_momentum(MiniBool mb) { return mb.mechanics & 0x0F; }

/* =========================================================================
 * 4. POLY-DIMENSIONAL BINARY: CARRIER & DEEP STEGANOGRAPHY
 * ========================================================================= */
#define POLY_CARRIER_MAX 256

typedef struct {
    char      carrier_text[POLY_CARRIER_MAX]; /* Surface visible ASCII */
    size_t    carrier_len;
    MicroBool gears[POLY_CARRIER_MAX * 8];    /* 1 MicroBool per surface bit */
    uint8_t   torque_hash[POLY_CARRIER_MAX];  /* Interlocking gear tooth hashes */
    size_t    total_bits;
} PolyBinaryStream;

static inline uint8_t poly_keystream_bit(uint8_t key_seed, size_t bit_index) {
    uint32_t x = ((uint32_t)key_seed << 16) ^ (uint32_t)bit_index;
    x = ((x >> 16) ^ x) * 0x45d9f3bU;
    x = ((x >> 16) ^ x) * 0x45d9f3bU;
    x = (x >> 16) ^ x;
    return (uint8_t)(x & 1);
}

static inline void poly_binary_encode(PolyBinaryStream *stream,
                                      const char *surface,
                                      const char *secret,
                                      uint8_t key_seed) {
    memset(stream, 0, sizeof(PolyBinaryStream));
    stream->carrier_len = strlen(surface);
    if (stream->carrier_len >= POLY_CARRIER_MAX) stream->carrier_len = POLY_CARRIER_MAX - 1;
    strncpy(stream->carrier_text, surface, stream->carrier_len);
    stream->total_bits = stream->carrier_len * 8;

    size_t secret_len = secret ? strlen(secret) : 0;
    if (secret_len > 255) secret_len = 255;
    size_t max_secret_capacity = (stream->carrier_len > 1) ? (stream->carrier_len - 1) : 0;
    if (secret_len > max_secret_capacity) secret_len = max_secret_capacity;

    for (size_t i = 0; i < stream->total_bits; i++) {
        size_t b = i / 8;
        int bit = 7 - (i % 8);
        bool surface_bit = (stream->carrier_text[b] >> bit) & 1;

        /* Extract plaintext secret bit: Byte 0 = length, Bytes 1..L = payload, rest = padding */
        bool secret_bit = false;
        if (b == 0) {
            secret_bit = ((uint8_t)secret_len >> bit) & 1;
        } else if ((b - 1) < secret_len) {
            secret_bit = (secret[b - 1] >> bit) & 1;
        } else {
            /* Keyed pseudo-random padding */
            secret_bit = (poly_keystream_bit(key_seed ^ 0x5C, i) != 0);
        }

        /* Cryptographic Stream Cipher: XOR with keyed pseudo-random keystream */
        uint8_t k_bit = poly_keystream_bit(key_seed, i);
        bool cipher_bit = secret_bit ^ (k_bit != 0);

        uint8_t base_sector = surface_bit ? 1 : 5; /* Northern (45 deg) vs Southern (225 deg) */
        uint8_t tension = cipher_bit ? 3 : 1;      /* Modulate tension with cipher bit */
        bool    spin_cw = ((key_seed + (uint8_t)i) & 1) ^ cipher_bit;
        uint8_t damping = (key_seed ^ (uint8_t)b) & 0x03;

        stream->gears[i] = microbool_create(base_sector, tension, damping, spin_cw);

        uint8_t prev_phase = (i > 0) ? microbool_phase(stream->gears[i - 1]) : key_seed;
        stream->torque_hash[b] ^= (uint8_t)((prev_phase << 4) | (base_sector ^ tension));
    }
}

static inline void poly_binary_decode_surface(const PolyBinaryStream *stream, char *out_surface, size_t max_len) {
    size_t num_bytes = stream->total_bits / 8;
    if (num_bytes >= max_len) num_bytes = max_len - 1;

    for (size_t b = 0; b < num_bytes; b++) {
        uint8_t byte_val = 0;
        for (int bit = 0; bit < 8; bit++) {
            size_t bit_idx = b * 8 + (7 - bit);
            if (stream->gears[bit_idx].legacy_val) {
                byte_val |= (1U << bit);
            }
        }
        out_surface[b] = (char)byte_val;
    }
    out_surface[num_bytes] = '\0';
}

static inline void poly_binary_decode_deep(const PolyBinaryStream *stream,
                                           char *out_secret,
                                           size_t max_len,
                                           uint8_t key_seed) {
    size_t num_bytes = stream->total_bits / 8;
    if (num_bytes == 0 || max_len == 0) {
        if (max_len > 0) out_secret[0] = '\0';
        return;
    }

    /* 1. Recover Byte 0: payload length */
    uint8_t payload_len = 0;
    for (int bit = 0; bit < 8; bit++) {
        size_t bit_idx = 7 - bit;
        MicroBool mb = stream->gears[bit_idx];
        bool expected_spin = ((key_seed + (uint8_t)bit_idx) & 1) != 0;
        bool cipher_bit = (microbool_tension(mb) >= 2) || (microbool_spin(mb) != expected_spin);
        uint8_t k_bit = poly_keystream_bit(key_seed, bit_idx);
        bool plain_bit = cipher_bit ^ (k_bit != 0);
        if (plain_bit) {
            payload_len |= (1U << bit);
        }
    }

    /* Bound payload length */
    if (payload_len >= max_len) payload_len = (uint8_t)(max_len - 1);
    size_t available_payload_bytes = (num_bytes > 1) ? (num_bytes - 1) : 0;
    if (payload_len > available_payload_bytes) payload_len = (uint8_t)available_payload_bytes;

    /* 2. Recover payload bytes */
    for (size_t b = 0; b < payload_len; b++) {
        uint8_t sec_byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            size_t bit_idx = (b + 1) * 8 + (7 - bit);
            MicroBool mb = stream->gears[bit_idx];
            bool expected_spin = ((key_seed + (uint8_t)bit_idx) & 1) != 0;
            bool cipher_bit = (microbool_tension(mb) >= 2) || (microbool_spin(mb) != expected_spin);
            uint8_t k_bit = poly_keystream_bit(key_seed, bit_idx);
            bool plain_bit = cipher_bit ^ (k_bit != 0);
            if (plain_bit) {
                sec_byte |= (1U << bit);
            }
        }
        out_secret[b] = (char)sec_byte;
    }
    out_secret[payload_len] = '\0';
}

static inline bool poly_binary_verify_integrity(const PolyBinaryStream *stream, uint8_t key_seed, size_t *tampered_bit) {
    uint8_t recomputed_hash[POLY_CARRIER_MAX] = {0};

    for (size_t i = 0; i < stream->total_bits; i++) {
        size_t b = i / 8;
        int bit = 7 - (i % 8);
        uint8_t base_sector = microbool_phase(stream->gears[i]);
        uint8_t tension = microbool_tension(stream->gears[i]);

        bool surface_bit = (stream->carrier_text[b] >> bit) & 1;
        if (surface_bit != stream->gears[i].legacy_val) {
            if (tampered_bit) *tampered_bit = i;
            return false;
        }

        uint8_t prev_phase = (i > 0) ? microbool_phase(stream->gears[i - 1]) : key_seed;
        recomputed_hash[b] ^= (uint8_t)((prev_phase << 4) | (base_sector ^ tension));
    }

    for (size_t b = 0; b < stream->carrier_len; b++) {
        if (recomputed_hash[b] != stream->torque_hash[b]) {
            if (tampered_bit) *tampered_bit = b * 8;
            return false;
        }
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* POLY_BINARY_GEARBOX_H */
