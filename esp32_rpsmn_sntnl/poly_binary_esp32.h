/* =========================================================================
 *  poly_binary_esp32.h
 *  =========================================================================
 *  Ultra-Lightweight Poly-Binary Sub-Scalar Gearbox & Sntnl Ghost Transport
 *  Designed for Embedded Microcontrollers: ESP32 / ESP8266 / RP2040 / STM32.
 *  - Footprint: < 4 KB RAM
 *  - Decoy Surface Carrier + Deep Steganographic Payload
 *  - Golden Ratio (Phi) Key Modulation
 *  - Mechanical Gear Tooth Interlocking Integrity & Tamper Jamming
 *  Zero external dependencies. Pure standard C99.
 * ========================================================================= */

#ifndef POLY_BINARY_ESP32_H
#define POLY_BINARY_ESP32_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP32_CARRIER_MAX 128
#define ESP32_PHI_KEY_MOD 161 /* 1.618 * 100 */

/* MicroBool: Chameleon Newtonian Bit */
typedef struct {
    bool    legacy_val; /* C99 bool view (1 or 0) */
    uint8_t gears;      /* [7:5] sector, [4:3] tension, [2:1] damping, [0] spin */
} Esp32MicroBool;

static inline Esp32MicroBool esp32_microbool_make(uint8_t sector, uint8_t tension, uint8_t damp, bool spin) {
    Esp32MicroBool mb;
    mb.legacy_val = (sector < 4);
    mb.gears = (uint8_t)(((sector & 7) << 5) |
                         ((tension & 3) << 3) |
                         ((damp & 3) << 1) |
                         (spin ? 1 : 0));
    return mb;
}

static inline uint8_t esp32_mb_sector(Esp32MicroBool mb)  { return (mb.gears >> 5) & 7; }
static inline uint8_t esp32_mb_tension(Esp32MicroBool mb) { return (mb.gears >> 3) & 3; }
static inline uint8_t esp32_mb_damping(Esp32MicroBool mb) { return (mb.gears >> 1) & 3; }
static inline bool    esp32_mb_spin(Esp32MicroBool mb)    { return (mb.gears & 1) != 0; }

/* Poly-Binary Ghost Stream for ESP32 */
typedef struct {
    char           carrier_text[ESP32_CARRIER_MAX];
    size_t         carrier_len;
    Esp32MicroBool gears[ESP32_CARRIER_MAX * 8];
    uint8_t        torque_hash[ESP32_CARRIER_MAX];
    size_t         total_bits;
} Esp32GhostStream;

/* Fast Keystream Finalizer PRNG */
static inline uint8_t esp32_keystream_bit(uint8_t key_seed, size_t bit_index) {
    uint32_t x = ((uint32_t)key_seed << 16) ^ (uint32_t)bit_index;
    x = ((x >> 16) ^ x) * 0x45d9f3bU;
    x = ((x >> 16) ^ x) * 0x45d9f3bU;
    x = (x >> 16) ^ x;
    return (uint8_t)(x & 1);
}

/* Encode secret payload into decoy surface */
static inline void esp32_ghost_encode(Esp32GhostStream *stream,
                                      const char *surface,
                                      const char *secret,
                                      uint8_t key) {
    memset(stream, 0, sizeof(Esp32GhostStream));
    size_t slen = strlen(surface);
    if (slen >= ESP32_CARRIER_MAX) slen = ESP32_CARRIER_MAX - 1;
    stream->carrier_len = slen;
    memcpy(stream->carrier_text, surface, slen);
    stream->carrier_text[slen] = '\0';
    stream->total_bits = slen * 8;

    size_t sec_len = secret ? strlen(secret) : 0;
    if (sec_len > 255) sec_len = 255;
    size_t max_cap = (slen > 1) ? (slen - 1) : 0;
    if (sec_len > max_cap) sec_len = max_cap;

    for (size_t i = 0; i < stream->total_bits; i++) {
        size_t b = i / 8;
        int bit = 7 - (int)(i % 8);
        bool surf_bit = (stream->carrier_text[b] >> bit) & 1;

        bool sec_bit = false;
        if (b == 0) {
            sec_bit = ((uint8_t)sec_len >> bit) & 1;
        } else if ((b - 1) < sec_len) {
            sec_bit = (secret[b - 1] >> bit) & 1;
        } else {
            sec_bit = (esp32_keystream_bit(key ^ 0x5C, i) != 0);
        }

        uint8_t k_bit = esp32_keystream_bit(key, i);
        bool cipher_bit = sec_bit ^ (k_bit != 0);

        uint8_t sector = surf_bit ? 1 : 5; /* 45 deg vs 225 deg */
        uint8_t tension = cipher_bit ? 3 : 1;
        bool    spin = (((key + (uint8_t)i) & 1) ^ (cipher_bit ? 1 : 0)) != 0;
        uint8_t damp = (key ^ (uint8_t)b) & 3;

        stream->gears[i] = esp32_microbool_make(sector, tension, damp, spin);

        uint8_t prev_phase = (i > 0) ? esp32_mb_sector(stream->gears[i - 1]) : (key & 7);
        stream->torque_hash[b] ^= (uint8_t)((prev_phase << 4) | (sector ^ tension));
    }
}

/* Decode deep secret payload */
static inline void esp32_ghost_decode_deep(const Esp32GhostStream *stream,
                                           char *out_secret,
                                           size_t max_len,
                                           uint8_t key) {
    size_t num_bytes = stream->total_bits / 8;
    if (num_bytes <= 1 || max_len == 0) {
        if (max_len > 0) out_secret[0] = '\0';
        return;
    }

    /* Recover length from Byte 0 */
    uint8_t payload_len = 0;
    for (int bit = 0; bit < 8; bit++) {
        size_t bit_idx = 7 - bit;
        Esp32MicroBool mb = stream->gears[bit_idx];
        bool exp_spin = ((key + (uint8_t)bit_idx) & 1) != 0;
        bool cipher_bit = (esp32_mb_tension(mb) >= 2) || (esp32_mb_spin(mb) != exp_spin);
        uint8_t k_bit = esp32_keystream_bit(key, bit_idx);
        bool plain_bit = cipher_bit ^ (k_bit != 0);
        if (plain_bit) payload_len |= (1U << bit);
    }

    if (payload_len >= max_len) payload_len = (uint8_t)(max_len - 1);
    if (payload_len > (num_bytes - 1)) payload_len = (uint8_t)(num_bytes - 1);

    for (size_t b = 0; b < payload_len; b++) {
        uint8_t c = 0;
        for (int bit = 0; bit < 8; bit++) {
            size_t bit_idx = (b + 1) * 8 + (7 - bit);
            Esp32MicroBool mb = stream->gears[bit_idx];
            bool exp_spin = ((key + (uint8_t)bit_idx) & 1) != 0;
            bool cipher_bit = (esp32_mb_tension(mb) >= 2) || (esp32_mb_spin(mb) != exp_spin);
            uint8_t k_bit = esp32_keystream_bit(key, bit_idx);
            bool plain_bit = cipher_bit ^ (k_bit != 0);
            if (plain_bit) c |= (1U << bit);
        }
        out_secret[b] = (char)c;
    }
    out_secret[payload_len] = '\0';
}

/* Verify mechanical gear tooth integrity */
static inline bool esp32_ghost_verify(const Esp32GhostStream *stream, uint8_t key) {
    uint8_t recomputed[ESP32_CARRIER_MAX];
    memset(recomputed, 0, sizeof(recomputed));

    for (size_t i = 0; i < stream->total_bits; i++) {
        size_t b = i / 8;
        int bit = 7 - (int)(i % 8);
        uint8_t sec = esp32_mb_sector(stream->gears[i]);
        uint8_t ten = esp32_mb_tension(stream->gears[i]);

        bool surf_bit = (stream->carrier_text[b] >> bit) & 1;
        if (surf_bit != stream->gears[i].legacy_val) {
            return false; /* Surface tampering */
        }

        uint8_t prev_phase = (i > 0) ? esp32_mb_sector(stream->gears[i - 1]) : (key & 7);
        recomputed[b] ^= (uint8_t)((prev_phase << 4) | (sec ^ ten));
    }

    for (size_t b = 0; b < stream->carrier_len; b++) {
        if (recomputed[b] != stream->torque_hash[b]) {
            return false; /* Gear tooth mechanical jam */
        }
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* POLY_BINARY_ESP32_H */
