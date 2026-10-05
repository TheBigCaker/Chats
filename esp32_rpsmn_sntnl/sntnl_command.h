/* =========================================================================
 *  sntnl_command.h
 *  =========================================================================
 *  Shared Sntnl Mesh Command Grammar.
 *
 *  The ghost stream already carries a "STATE:<name>|..." payload.  This
 *  header adds a second, strictly-parsed payload class so one node can ask
 *  another to *do* something -- including running a drygon compile -- without
 *  ever handing a remote string to a shell.
 *
 *  Wire grammar (inside the steganographic deep payload):
 *
 *      CMD:<VERB>[|<KEY>:<VALUE>]...
 *
 *  Keys: SRC, OUT, ARG, ORIG, SEQ, T
 *
 *      CMD:DRYGON|SRC:foo.dry|OUT:foo.elf|ORIG:PHONE|SEQ:7|T:0123456789abcdef
 *      CMD:PING|ORIG:HOST|SEQ:8
 *      CMD:LED|ARG:blink|ORIG:HOST|SEQ:9
 *      CMD:STATE|ARG:HYDRA_HARMONIC_LOCK|ORIG:HOST|SEQ:10|T:...
 *
 *  Design rules that this parser enforces, deliberately:
 *    - The verb is an enum, never a free string.  Unknown verbs are rejected.
 *    - Keys are a closed set and may appear at most once.  Unknown or
 *      duplicated keys are rejected rather than ignored, so a frame cannot
 *      smuggle a second SRC past a lax reader.
 *    - Values may not contain '|', ':' or control bytes, so no value can
 *      fabricate a new field.
 *    - DRYGON source/output names must be *bare relative filenames* with an
 *      allowed suffix.  No '/', no '..', no leading '.', no leading '-', no
 *      NUL.  The receiver supplies the directory.
 *
 *  Everything here is pure C99 with no I/O, no allocation and no libc beyond
 *  string.h, so the identical code runs on an ESP32 (< 4 KB RAM) and on the
 *  host CLI.  Callers decide what a verb *means*; this header only decides
 *  what a verb is allowed to *say*.
 * ========================================================================= */

#ifndef SNTNL_COMMAND_H
#define SNTNL_COMMAND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>   /* snprintf -- present in both the Arduino core and the host */
#include <string.h>

/* The token field's width is owned by the crypto layer, so the grammar and
 * the tag can never disagree about how long a tag is. */
#include "mesh_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deep payload budget.  ESP32_CARRIER_MAX is 128, the encoder reserves byte 0
 * for the payload length, so at most 126 bytes of command can ever survive a
 * decode.  A command longer than this must be rejected by the *sender*, not
 * silently truncated by the encoder. */
#define SNTNL_CMD_MAX        126
#define SNTNL_ARG_MAX        64
#define SNTNL_ORIG_MAX       16
/* SNTNL_TOKEN_LEN / SNTNL_TOKEN_HEX / SNTNL_TOKEN_HEX_MAX come from mesh_auth.h */

/* Verbs fall into two classes and the split is the whole security story:
 *
 *   BOUNDED  -- no code execution, no data exfiltration, no filesystem write.
 *               Safe to honour from anyone who can reach the mesh.
 *               PING, LED, STATE, REPORT, ACK
 *
 *   PRIVILEGED -- runs the compiler, writes to disk.  Equivalent to remote
 *               code execution, so a receiver must never honour one unless
 *               the frame carries a token that matches its own configured
 *               secret.  A receiver with no secret configured refuses.
 *               DRYGON
 */
typedef enum {
    SNTNL_VERB_NONE = 0,
    SNTNL_VERB_PING,
    SNTNL_VERB_LED,
    SNTNL_VERB_STATE,
    SNTNL_VERB_REPORT,
    SNTNL_VERB_ACK,
    SNTNL_VERB_DRYGON
} SntnlVerb;

typedef struct {
    SntnlVerb verb;
    char      src[SNTNL_ARG_MAX];   /* DRYGON source filename           */
    char      out[SNTNL_ARG_MAX];   /* DRYGON output filename (optional)*/
    char      arg[SNTNL_ARG_MAX];   /* LED / STATE / REPORT argument    */
    char      origin[SNTNL_ORIG_MAX];
    char      token[SNTNL_TOKEN_HEX_MAX];
    uint32_t  seq;
    bool      has_seq;
    bool      has_token;
} SntnlCommand;

/* ------------------------------------------------------------------ utils */

static inline size_t sntnl_strnlen_(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0') n++;
    return n;
}

static inline bool sntnl_is_hex16_(const char *s) {
    for (int i = 0; i < SNTNL_TOKEN_HEX; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return s[SNTNL_TOKEN_HEX] == '\0';
}

static inline bool sntnl_is_decimal_(const char *s) {
    if (!*s) return false;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return false;
    return true;
}

/* ------------------------------------------------------------------ verbs */

typedef struct {
    const char *name;
    size_t      len;
    SntnlVerb   verb;
    bool        privileged;
} SntnlVerbEntry;

static const SntnlVerbEntry sntnl_verb_table_[] = {
    { "PING",   4, SNTNL_VERB_PING,   false },
    { "LED",    3, SNTNL_VERB_LED,    false },
    { "STATE",  5, SNTNL_VERB_STATE,  false },
    { "REPORT", 6, SNTNL_VERB_REPORT, false },
    { "ACK",    3, SNTNL_VERB_ACK,    false },
    { "DRYGON", 6, SNTNL_VERB_DRYGON, true  }
};

#define SNTNL_VERB_COUNT (sizeof(sntnl_verb_table_) / sizeof(sntnl_verb_table_[0]))

static inline SntnlVerb sntnl_verb_lookup(const char *name, size_t len) {
    for (size_t i = 0; i < SNTNL_VERB_COUNT; i++) {
        if (sntnl_verb_table_[i].len == len &&
            memcmp(sntnl_verb_table_[i].name, name, len) == 0) {
            return sntnl_verb_table_[i].verb;
        }
    }
    return SNTNL_VERB_NONE;
}

static inline bool sntnl_verb_is_privileged(SntnlVerb v) {
    for (size_t i = 0; i < SNTNL_VERB_COUNT; i++)
        if (sntnl_verb_table_[i].verb == v) return sntnl_verb_table_[i].privileged;
    return true; /* unknown -> treat as privileged */
}

static inline const char *sntnl_verb_name(SntnlVerb v) {
    for (size_t i = 0; i < SNTNL_VERB_COUNT; i++)
        if (sntnl_verb_table_[i].verb == v) return sntnl_verb_table_[i].name;
    return "?";
}

/* ------------------------------------------------------------- validation */

/* A DRYGON *source* must be a bare relative filename of a compilable source.
 * The receiver -- not the sender -- prepends the directory, so nothing a
 * remote frame says can traverse out of the working tree. */
static inline bool sntnl_drygon_scope_ok(const char *name) {
    if (!name || !*name) return false;
    size_t n = strlen(name);
    if (n == 0 || n >= SNTNL_ARG_MAX) return false;
    if (name[0] == '.' || name[0] == '-') return false;   /* no hidden, no option */

    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;                 /* kills '/', '\\', ':', '|', space */
    }
    if (strstr(name, "..")) return false;

    const char *suffix[] = { ".dry", ".ccp", ".c" };
    for (size_t s = 0; s < sizeof(suffix) / sizeof(suffix[0]); s++) {
        size_t sl = strlen(suffix[s]);
        if (n > sl && strcmp(name + n - sl, suffix[s]) == 0) return true;
    }
    return false;
}

/* A DRYGON *output* must be a bare relative filename the receiver can place
 * inside its own build sandbox. */
static inline bool sntnl_drygon_out_ok(const char *name) {
    if (!name || !*name) return false;
    size_t n = strlen(name);
    if (n == 0 || n >= SNTNL_ARG_MAX) return false;
    if (name[0] == '.' || name[0] == '-') return false;

    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    if (strstr(name, "..")) return false;

    const char *suffix[] = { ".elf", ".ros" };
    for (size_t s = 0; s < sizeof(suffix) / sizeof(suffix[0]); s++) {
        size_t sl = strlen(suffix[s]);
        if (n > sl && strcmp(name + n - sl, suffix[s]) == 0) return true;
    }
    return false;
}

static inline bool sntnl_led_arg_ok(const char *arg) {
    return strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0 ||
           strcmp(arg, "blink") == 0 || strcmp(arg, "identify") == 0;
}

/* A STATE name is free text but must survive the payload budget and may not
 * contain a field separator. */
static inline bool sntnl_state_arg_ok(const char *arg) {
    if (!arg || !*arg) return false;
    if (strlen(arg) >= SNTNL_ARG_MAX) return false;
    for (const char *p = arg; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '|' || c == ':' || c < 0x20) return false;
    }
    return true;
}

/* --------------------------------------------------------------- grammar */

/* Returns false, with *err pointing at a static reason, on any violation. */
static inline bool sntnl_command_parse(const char *payload, SntnlCommand *out,
                                      const char **err) {
    const char *e = "";
    if (err) *err = e;
    if (!payload || !out) { if (err) *err = "null payload"; return false; }

    memset(out, 0, sizeof(*out));

    size_t total = strlen(payload);
    if (total > SNTNL_CMD_MAX) { if (err) *err = "payload over 126 bytes"; return false; }
    /* "CMD:ACK" is the shortest legal frame: seven bytes. */
    if (total < 7 || memcmp(payload, "CMD:", 4) != 0) { if (err) *err = "missing CMD: prefix"; return false; }

    const char *p = payload + 4;

    /* verb */
    const char *bar = strchr(p, '|');
    size_t vlen = bar ? (size_t)(bar - p) : strlen(p);
    out->verb = sntnl_verb_lookup(p, vlen);
    if (out->verb == SNTNL_VERB_NONE) { if (err) *err = "unknown verb"; return false; }

    /* fields -- strict: closed key set, no duplicates, values non-empty and
     * free of '|' ':' and control bytes */
    bool seen_src = false, seen_out = false, seen_arg = false;
    bool seen_orig = false, seen_seq = false, seen_tok = false;

    while (bar) {
        const char *f = bar + 1;
        const char *end = strchr(f, '|');
        size_t flen = end ? (size_t)(end - f) : strlen(f);

        const char *colon = NULL;
        for (size_t i = 0; i < flen; i++) if (f[i] == ':') { colon = f + i; break; }
        if (!colon) { if (err) *err = "field without key:value"; return false; }

        size_t klen = (size_t)(colon - f);
        const char *val = colon + 1;
        size_t vallen = flen - klen - 1;
        if (vallen == 0) { if (err) *err = "empty field value"; return false; }
        for (size_t i = 0; i < vallen; i++) {
            unsigned char c = (unsigned char)val[i];
            if (c == '|' || c == ':' || c < 0x20) { if (err) *err = "illegal byte in value"; return false; }
        }
        if (vallen >= SNTNL_ARG_MAX) { if (err) *err = "field value too long"; return false; }

        char buf[SNTNL_ARG_MAX];
        memcpy(buf, val, vallen);
        buf[vallen] = '\0';

        if (klen == 3 && memcmp(f, "SRC", 3) == 0) {
            if (seen_src) { if (err) *err = "duplicate SRC"; return false; }
            seen_src = true;
            memcpy(out->src, buf, vallen + 1);
        } else if (klen == 3 && memcmp(f, "OUT", 3) == 0) {
            if (seen_out) { if (err) *err = "duplicate OUT"; return false; }
            seen_out = true;
            memcpy(out->out, buf, vallen + 1);
        } else if (klen == 3 && memcmp(f, "ARG", 3) == 0) {
            if (seen_arg) { if (err) *err = "duplicate ARG"; return false; }
            seen_arg = true;
            memcpy(out->arg, buf, vallen + 1);
        } else if (klen == 4 && memcmp(f, "ORIG", 4) == 0) {
            if (seen_orig) { if (err) *err = "duplicate ORIG"; return false; }
            seen_orig = true;
            if (vallen >= SNTNL_ORIG_MAX) { if (err) *err = "ORIG too long"; return false; }
            memcpy(out->origin, buf, vallen + 1);
        } else if (klen == 3 && memcmp(f, "SEQ", 3) == 0) {
            if (seen_seq) { if (err) *err = "duplicate SEQ"; return false; }
            seen_seq = true;
            if (!sntnl_is_decimal_(buf) || vallen > 10) { if (err) *err = "SEQ not decimal"; return false; }
            uint32_t acc = 0;
            for (size_t i = 0; i < vallen; i++) acc = acc * 10u + (uint32_t)(buf[i] - '0');
            out->seq = acc;
            out->has_seq = true;
        } else if (klen == 1 && f[0] == 'T') {
            if (seen_tok) { if (err) *err = "duplicate T"; return false; }
            seen_tok = true;
            if (!sntnl_is_hex16_(buf)) { if (err) *err = "T not 16 hex chars"; return false; }
            memcpy(out->token, buf, SNTNL_TOKEN_HEX + 1);
            out->has_token = true;
        } else {
            if (err) *err = "unknown field key";
            return false;
        }

        bar = end;
    }

    /* per-verb required fields */
    switch (out->verb) {
        case SNTNL_VERB_DRYGON:
            if (!seen_src) { if (err) *err = "DRYGON requires SRC"; return false; }
            if (!sntnl_drygon_scope_ok(out->src)) { if (err) *err = "SRC outside allowed scope"; return false; }
            if (seen_out && !sntnl_drygon_out_ok(out->out)) { if (err) *err = "OUT outside allowed scope"; return false; }
            break;
        case SNTNL_VERB_LED:
            if (!seen_arg || !sntnl_led_arg_ok(out->arg)) { if (err) *err = "LED requires ARG on|off|blink|identify"; return false; }
            break;
        case SNTNL_VERB_STATE:
            if (!seen_arg || !sntnl_state_arg_ok(out->arg)) { if (err) *err = "STATE requires a valid ARG"; return false; }
            break;
        default:
            break;
    }
    return true;
}

/* ------------------------------------------------------------- formatting */

/* Canonical field order, always.  A tag is computed over the canonical form
 * *before* the token is appended, so verifiers must reproduce that order. */
static inline size_t sntnl_command_format(const SntnlCommand *c, char *out, size_t max) {
    if (!c || !out || max == 0) return 0;
    const char *vname = sntnl_verb_name(c->verb);
    int n = 0;
    if (c->verb == SNTNL_VERB_DRYGON && c->src[0])
        n = snprintf(out, max, "CMD:%s|SRC:%s", vname, c->src);
    else if (c->arg[0])
        n = snprintf(out, max, "CMD:%s|ARG:%s", vname, c->arg);
    else
        n = snprintf(out, max, "CMD:%s", vname);
    if (n < 0 || (size_t)n >= max) return 0;

    if (c->verb == SNTNL_VERB_DRYGON && c->out[0]) {
        int m = snprintf(out + n, max - (size_t)n, "|OUT:%s", c->out);
        if (m < 0 || (size_t)(n + m) >= max) return 0;
        n += m;
    }
    if (c->origin[0]) {
        int m = snprintf(out + n, max - (size_t)n, "|ORIG:%s", c->origin);
        if (m < 0 || (size_t)(n + m) >= max) return 0;
        n += m;
    }
    if (c->has_seq) {
        int m = snprintf(out + n, max - (size_t)n, "|SEQ:%lu", (unsigned long)c->seq);
        if (m < 0 || (size_t)(n + m) >= max) return 0;
        n += m;
    }
    return (size_t)n;
}

/* Everything before the trailing |T: field, i.e. the bytes the tag covers. */
static inline bool sntnl_command_canonical(const char *payload, char *out, size_t max) {
    if (!payload || !out || max == 0) return false;
    const char *t = strstr(payload, "|T:");
    if (!t) return false;
    if (!sntnl_is_hex16_(t + 3)) return false;   /* T must be last and well formed */
    size_t n = (size_t)(t - payload);
    if (n >= max) return false;
    memcpy(out, payload, n);
    out[n] = '\0';
    return true;
}

/* ------------------------------------------------------------- decoy sizing */

/* The encoder silently clamps a payload to (carrier_len - 1).  A truncated
 * DRYGON command would still parse as a *shorter valid command* in the worst
 * case, so the sender must size the carrier explicitly instead of hoping. */
static inline size_t sntnl_decoy_len_for(size_t payload_len) {
    size_t need = payload_len + 1;
    return need > 127 ? 127 : need;
}

static inline bool sntnl_payload_fits(size_t payload_len) {
    return payload_len <= SNTNL_CMD_MAX;
}

#ifdef __cplusplus
}
#endif

#endif /* SNTNL_COMMAND_H */
