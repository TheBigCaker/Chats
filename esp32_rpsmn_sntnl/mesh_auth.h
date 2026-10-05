/* =========================================================================
 *  mesh_auth.h
 *  =========================================================================
 *  Command authentication for the Sntnl ghost mesh.
 *
 *  Why this exists
 *  ---------------
 *  The ghost transport is obfuscation, not cryptography: its "key" is the
 *  published constant 0x5C and its "hash" is an unkeyed chained XOR that any
 *  receiver recomputes.  On a broadcast LAN that is survivable, because the
 *  blast radius is the room.  The moment the mesh crosses a public relay it is
 *  not: anyone who can reach the endpoint can craft a frame that verifies.
 *
 *  So the transport is left alone -- the steganographic carrier is the point
 *  of the project -- and authenticity is layered *inside* the payload, where
 *  it costs nothing but bytes:
 *
 *      CMD:DRYGON|SRC:foo.dry|OUT:foo.elf|ORIG:PHONE|SEQ:7|T:9f2c0b71e4a8d316
 *                                                          \_____________/
 *                                              HMAC-SHA256(token, prefix)
 *                                              truncated to 64 bits, hex
 *
 *  The tag covers every byte before the trailing |T: field.  A receiver that
 *  has a token configured refuses any privileged verb whose tag is absent,
 *  malformed or wrong.  A receiver with *no* token configured refuses
 *  privileged verbs outright rather than falling open.
 *
 *  This is SHA-256 and HMAC-SHA256 per FIPS 180-4 / RFC 2104, no external
 *  dependencies, no allocation, ~150 lines of C99, small enough for an ESP32.
 *  A 64-bit truncation is a deliberate budget choice: the deep payload only
 *  has 126 bytes and a full 256-bit tag would consume half of them.  64 bits
 *  is far beyond what an online forgery attempt can brute-force, and offline
 *  attacks are irrelevant to a tag that expires with the frame.
 *
 *  This authenticates *the sender*.  It does not make the channel private --
 *  the decoy surface stays readable, and the payload stays obfuscated at best.
 *  Confidentiality still belongs in cygemm/cryptids_kernels.dry.
 * ========================================================================= */

#ifndef MESH_AUTH_H
#define MESH_AUTH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNTNL_SHA256_DIGEST 32

/* Tag shape, shared with the command grammar in sntnl_command.h: the token
 * field is 16 hex characters standing for the leading 64 bits of the HMAC. */
#define SNTNL_TOKEN_LEN      8
#define SNTNL_TOKEN_HEX      (SNTNL_TOKEN_LEN * 2)
#define SNTNL_TOKEN_HEX_MAX  (SNTNL_TOKEN_HEX + 1)

/* ----------------------------------------------------------- SHA-256 core */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} SntnlSha256;

static inline uint32_t sntnl_rotr_(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static const uint32_t sntnl_sha256_k_[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static inline void sntnl_sha256_block_(SntnlSha256 *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sntnl_rotr_(w[i-15],7) ^ sntnl_rotr_(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = sntnl_rotr_(w[i-2],17) ^ sntnl_rotr_(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sntnl_rotr_(e,6) ^ sntnl_rotr_(e,11) ^ sntnl_rotr_(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + sntnl_sha256_k_[i] + w[i];
        uint32_t S0 = sntnl_rotr_(a,2) ^ sntnl_rotr_(a,13) ^ sntnl_rotr_(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static inline void sntnl_sha256_init(SntnlSha256 *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

static inline void sntnl_sha256_update(SntnlSha256 *c, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        c->buf[c->buflen++] = data[i];
        if (c->buflen == 64) {
            sntnl_sha256_block_(c, c->buf);
            c->bitlen += 512;
            c->buflen = 0;
        }
    }
}

static inline void sntnl_sha256_final(SntnlSha256 *c, uint8_t out[SNTNL_SHA256_DIGEST]) {
    /* The message length must be captured *before* padding, because padding
     * itself compresses blocks and would otherwise be counted as message. */
    uint64_t total_bits = c->bitlen + (uint64_t)c->buflen * 8;

    uint8_t pad = 0x80;
    sntnl_sha256_update(c, &pad, 1);
    uint8_t z = 0x00;
    while (c->buflen != 56) sntnl_sha256_update(c, &z, 1);

    uint8_t lenb[8];
    for (int k = 0; k < 8; k++) lenb[k] = (uint8_t)(total_bits >> (56 - 8 * k));
    memcpy(c->buf + c->buflen, lenb, 8);
    sntnl_sha256_block_(c, c->buf);

    for (int k = 0; k < 8; k++) {
        out[k * 4]     = (uint8_t)(c->state[k] >> 24);
        out[k * 4 + 1] = (uint8_t)(c->state[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(c->state[k] >> 8);
        out[k * 4 + 3] = (uint8_t)(c->state[k]);
    }
}

static inline void sntnl_sha256(const uint8_t *data, size_t len, uint8_t out[SNTNL_SHA256_DIGEST]) {
    SntnlSha256 c;
    sntnl_sha256_init(&c);
    sntnl_sha256_update(&c, data, len);
    sntnl_sha256_final(&c, out);
}

/* --------------------------------------------------------------- HMAC-256 */

static inline void sntnl_hmac_sha256(const uint8_t *key, size_t klen,
                                    const uint8_t *msg, size_t mlen,
                                    uint8_t out[SNTNL_SHA256_DIGEST]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (klen > 64) sntnl_sha256(key, klen, k);
    else memcpy(k, key, klen);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    SntnlSha256 c;
    uint8_t inner[SNTNL_SHA256_DIGEST];
    sntnl_sha256_init(&c);
    sntnl_sha256_update(&c, ipad, 64);
    sntnl_sha256_update(&c, msg, mlen);
    sntnl_sha256_final(&c, inner);

    sntnl_sha256_init(&c);
    sntnl_sha256_update(&c, opad, 64);
    sntnl_sha256_update(&c, inner, SNTNL_SHA256_DIGEST);
    sntnl_sha256_final(&c, out);
}

/* ------------------------------------------------------------ command tag */

/* Tag "canonical" with the shared token and render the leading 64 bits as
 * lowercase hex.  An empty token yields false: there is nothing to tag with,
 * and a receiver must be able to tell the difference. */
static inline bool sntnl_auth_tag(const char *token, const char *canonical,
                                  char out_hex[SNTNL_TOKEN_HEX_MAX + 1]) {
    if (!token || !*token || !canonical || !out_hex) return false;

    uint8_t mac[SNTNL_SHA256_DIGEST];
    sntnl_hmac_sha256((const uint8_t *)token, strlen(token),
                      (const uint8_t *)canonical, strlen(canonical), mac);

    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < SNTNL_TOKEN_LEN; i++) {
        out_hex[i * 2]     = hexd[(mac[i] >> 4) & 0xf];
        out_hex[i * 2 + 1] = hexd[mac[i] & 0xf];
    }
    out_hex[SNTNL_TOKEN_HEX] = '\0';
    return true;
}

/* Constant-time-ish comparison: no early exit on the first wrong nibble, so
 * a remote attacker cannot walk the tag out one character at a time. */
static inline bool sntnl_auth_verify(const char *token, const char *canonical,
                                     const char *received_hex) {
    if (!token || !*token || !canonical || !received_hex) return false;
    if (strlen(received_hex) != SNTNL_TOKEN_HEX) return false;

    char expected[SNTNL_TOKEN_HEX_MAX + 1];
    if (!sntnl_auth_tag(token, canonical, expected)) return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < SNTNL_TOKEN_HEX; i++) {
        char a = expected[i], b = received_hex[i];
        if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
        diff |= (uint8_t)(a ^ b);
    }
    return diff == 0;
}

/* Convenience for the sender: append |T:<tag> to a canonical payload. */
static inline bool sntnl_auth_seal(const char *token, const char *canonical,
                                   char *out, size_t max) {
    char tag[SNTNL_TOKEN_HEX_MAX + 1];
    if (!sntnl_auth_tag(token, canonical, tag)) return false;
    int n = snprintf(out, max, "%s|T:%s", canonical, tag);
    return n > 0 && (size_t)n < max;
}

#ifdef __cplusplus
}
#endif

#endif /* MESH_AUTH_H */
