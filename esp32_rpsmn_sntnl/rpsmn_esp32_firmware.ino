/* =========================================================================
 *  rpsmn_esp32_firmware.ino
 *  =========================================================================
 *  Sovereign ESP32 Rpsmn VRDMA & Sntnl Passive Ghost Mesh Firmware
 *  Compatible with: Arduino IDE / PlatformIO / ESP-IDF (C++)
 *
 *  Features:
 *  - Passive Ghost Broadcasts disguised as Environmental Telemetry
 *  - Dual-Channel Steganographic Decoding via poly_binary_esp32.h
 *  - Mechanical Gear Tooth Torque Hash Verification & Tamper Jamming
 *  - Wi-Fi Station Mode with fallback to Standalone Sovereign AP Mode
 *  - Clifford Torus Consensus Anchor (Node #8 of the Sovereign Swarm)
 * ========================================================================= */

#include <WiFi.h>
#include <WiFiUdp.h>
#include "poly_binary_esp32.h"
/* The command grammar and the authentication tag are shared, byte for byte,
 * with sovereign_mesh_host.c -- the same header is compiled into both. A node
 * can therefore never disagree with the host about what a frame means. */
#include "sntnl_command.h"
#include "mesh_auth.h"

/*
 * Wi-Fi Configuration
 * -------------------
 * Credentials are NEVER committed to this repository. To join a LAN in
 * Station mode, create `wifi_secrets.h` beside this sketch (it is gitignored;
 * `wifi_secrets.h.example` shows the shape):
 *
 *     #define WIFI_SSID "my-network"
 *     #define WIFI_PASS "my-password"
 *
 * With no such header the placeholders below apply, the guard below skips
 * Station mode, and the node stands up its own Sovereign AP -- so a fresh
 * clone can neither broadcast a password nor dial out with someone else's.
 */
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif

#ifndef WIFI_SSID
#  define WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASS
#  define WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

/*
 * Mesh Command Token
 * ------------------
 * Same discipline as the Wi-Fi credentials: never committed. Create
 * `mesh_token.h` beside this sketch (gitignored; `mesh_token.h.example` shows
 * the shape) to let this node honour signed DRYGON frames:
 *
 *     #define SNTNL_MESH_TOKEN "a-long-shared-secret"
 *
 * With no such header this node has NO token, and a node with no token refuses
 * every privileged verb. It does not fall open.
 */
#if defined(__has_include)
#  if __has_include("mesh_token.h")
#    include "mesh_token.h"
#  endif
#endif
#ifndef SNTNL_MESH_TOKEN
#  define SNTNL_MESH_TOKEN ""
#endif

/* Sntnl Mesh Network Configuration */
const unsigned int SNTNL_UDP_PORT   = 9876;
const uint8_t      SNTNL_PHI_KEY    = 0x5C;
const int          STATUS_LED_PIN   = 2; /* Built-in LED on most ESP32 boards */

/* 64-Bit Clifford Torus Consensus Anchor for ESP32 (Node #8) */
const uint64_t     ESP32_CLIFFORD_SEED = 0x5A5AA5A55A5AA5A5ULL;

WiFiUDP udp;
unsigned long last_beacon_time = 0;
uint32_t state_sequence = 1;
char current_state[64] = "ESP32_BOOT_STANDBY";

void setup() {
    Serial.begin(115200);
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    delay(1000);
    Serial.println("\n=======================================================");
    Serial.println("   SOVEREIGN RPSMN + SNTNL ESP32 NODE #8 INITIALIZING  ");
    Serial.println("   Poly-Binary Sub-Scalar Gearbox & Passive Ghost Mesh ");
    Serial.println("=======================================================");

    /* Attempt Station mode connection */
    bool connected = false;
    if (strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0) {
        Serial.printf("[*] Connecting to Wi-Fi SSID: %s...\n", WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            Serial.printf("\n[+] Wi-Fi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        }
    }

    /* Fallback: Create Sovereign Standalone Access Point */
    if (!connected) {
        Serial.println("\n[*] Starting Sovereign Standalone AP: 'SOVEREIGN_NODE_8'...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("SOVEREIGN_NODE_8", "rpsmn_2026");
        Serial.printf("[+] Sovereign AP Active! AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }

    /* Start UDP listener */
    udp.begin(SNTNL_UDP_PORT);
    Serial.printf("[+] Sntnl Passive Ghost Listener listening on UDP port %u\n", SNTNL_UDP_PORT);
    Serial.printf("[+] Clifford Torus Seed: 0x%016llX\n\n",
                  (unsigned long long)ESP32_CLIFFORD_SEED);
    Serial.printf("[+] Mesh command token: %s\n\n",
                  strlen(SNTNL_MESH_TOKEN) > 0
                      ? "configured -- signed privileged verbs will be authenticated"
                      : "NOT configured -- every privileged verb will be refused");

    /* Blink 3 times to indicate ready */
    for (int i = 0; i < 3; i++) {
        digitalWrite(STATUS_LED_PIN, HIGH); delay(100);
        digitalWrite(STATUS_LED_PIN, LOW);  delay(100);
    }
}

/* Build a plausible telemetry line of exactly `need` bytes. The encoder clamps
 * a payload to (carrier_len - 1), so the carrier is sized *to* the payload
 * instead of being left to chance -- a truncated command could otherwise still
 * parse as some shorter, perfectly valid one. */
size_t build_decoy_line(char *buf, size_t n, size_t need) {
    char base[160];
    float temp_c = 24.5f + (float)(random(-10, 10)) * 0.1f;
    int hum = 45 + random(-5, 5);
    snprintf(base, sizeof(base),
             "WEATHER: OBSERVATION ESP32_NODE_8 TEMP %.1fC HUM %d%% PRESS 1013HPA "
             "WIND 3KMH SENSOR_OK PASSIVE_OK",
             temp_c, hum);

    if (need > ESP32_CARRIER_MAX - 1) need = ESP32_CARRIER_MAX - 1;
    if (n && need > n - 1) need = n - 1;

    size_t bl = strlen(base);
    if (bl >= need) { memcpy(buf, base, need); buf[need] = '\0'; return need; }

    memcpy(buf, base, bl);
    size_t i = bl;
    const char *filler = " SENSOR_OK";
    size_t fl = strlen(filler);
    while (i + fl <= need) { memcpy(buf + i, filler, fl); i += fl; }
    while (i < need) buf[i++] = '.';
    buf[need] = '\0';
    return need;
}

/* Encode `secret` under a carrier sized to fit it, and put it on the wire. */
bool emit_ghost(const char *secret, IPAddress to, uint16_t port) {
    size_t plen = strlen(secret);
    if (!sntnl_payload_fits(plen)) {
        Serial.printf("[!] refusing to emit a %u-byte payload (budget %d)\n",
                      (unsigned)plen, SNTNL_CMD_MAX);
        return false;
    }

    char decoy[ESP32_CARRIER_MAX];
    build_decoy_line(decoy, sizeof(decoy), sntnl_decoy_len_for(plen));

    uint8_t golden_key = SNTNL_PHI_KEY ^ ESP32_PHI_KEY_MOD;
    Esp32GhostStream stream;
    esp32_ghost_encode(&stream, decoy, secret, golden_key);

    udp.beginPacket(to, port);
    udp.write((const uint8_t*)&stream, sizeof(stream));
    udp.endPacket();
    return true;
}

/* Emit a Sntnl Ghost Heartbeat disguised as weather telemetry */
void emit_sntnl_heartbeat() {
    char secret[ESP32_CARRIER_MAX];
    snprintf(secret, sizeof(secret), "STATE:%s|NODE:8|SEQ:%u", current_state, state_sequence++);

    if (emit_ghost(secret, IPAddress(255, 255, 255, 255), SNTNL_UDP_PORT))
        Serial.printf("[SNTNL EMIT] Seq: %u | Deep Payload: \"%s\"\n", state_sequence - 1, secret);
}

/* Answer a command frame, unicast straight back to whoever sent it. This node
 * never claims to have done something it did not do, so a refused DRYGON is
 * acknowledged as refused instead of being silently dropped. */
void emit_command_ack(const char *verb, const char *outcome, IPAddress to, uint16_t port) {
    SntnlCommand ack;
    memset(&ack, 0, sizeof(ack));
    ack.verb = SNTNL_VERB_ACK;
    snprintf(ack.arg, sizeof(ack.arg), "%s_%s", outcome, verb);
    snprintf(ack.origin, sizeof(ack.origin), "NODE8");

    char canonical[SNTNL_CMD_MAX + 1];
    if (sntnl_command_format(&ack, canonical, sizeof(canonical)) == 0) return;

    if (emit_ghost(canonical, to, port))
        Serial.printf("    [ACK] \"%s\" -> %s:%u\n", canonical, to.toString().c_str(), port);
}

void apply_led_arg(const char *arg) {
    if (strcmp(arg, "on") == 0) {
        digitalWrite(STATUS_LED_PIN, HIGH);
    } else if (strcmp(arg, "off") == 0) {
        digitalWrite(STATUS_LED_PIN, LOW);
    } else if (strcmp(arg, "blink") == 0) {
        for (int i = 0; i < 3; i++) {
            digitalWrite(STATUS_LED_PIN, HIGH); delay(120);
            digitalWrite(STATUS_LED_PIN, LOW);  delay(120);
        }
    } else if (strcmp(arg, "identify") == 0) {
        for (int i = 0; i < 10; i++) {
            digitalWrite(STATUS_LED_PIN, HIGH); delay(60);
            digitalWrite(STATUS_LED_PIN, LOW);  delay(60);
        }
    } else {
        Serial.printf("    [!] LED argument \"%s\" is not one of on|off|blink|identify\n", arg);
    }
}

/* Act on a command frame that already passed the grammar. Returns true if the
 * frame was acted upon. */
bool dispatch_command_frame(const SntnlCommand *cmd, const char *payload,
                            IPAddress from_ip, uint16_t from_port) {
    const char *vname = sntnl_verb_name(cmd->verb);

    if (sntnl_verb_is_privileged(cmd->verb)) {
        /* This node runs no compiler, so DRYGON can never be honoured here.
         * The token is still checked, and for the honest reason: a node that
         * skipped authentication on the strength of "we were going to refuse
         * anyway" would report the wrong one of two very different events. */
        bool token_ok = false;
        if (strlen(SNTNL_MESH_TOKEN) > 0 && cmd->has_token) {
            char canonical[SNTNL_CMD_MAX + 1];
            if (sntnl_command_canonical(payload, canonical, sizeof(canonical)))
                token_ok = sntnl_auth_verify(SNTNL_MESH_TOKEN, canonical, cmd->token);
            Serial.printf("    [AUTH] tag %s\n", token_ok ? "VERIFIED" : "REJECTED");
        } else {
            Serial.println("    [AUTH] frame is unsigned, or this node has no token configured");
        }
        Serial.printf("    [REFUSE] %s: this node runs no compiler -- nothing was executed\n", vname);
        emit_command_ack(vname, token_ok ? "NOCAP" : "DENIED", from_ip, from_port);
        return false;
    }

    if (cmd->verb == SNTNL_VERB_PING) {
        Serial.printf("    [PING] node '%s' is alive\n", cmd->origin[0] ? cmd->origin : "?");
    } else if (cmd->verb == SNTNL_VERB_LED) {
        apply_led_arg(cmd->arg);
    } else if (cmd->verb == SNTNL_VERB_STATE) {
        snprintf(current_state, sizeof(current_state), "%s", cmd->arg);
        Serial.printf("    [VRDMA SYNC] state -> \"%s\"\n", current_state);
    } else if (cmd->verb == SNTNL_VERB_REPORT) {
        Serial.printf("    [REPORT] state=%s seq=%u uptime=%lus%s%s\n", current_state,
                      state_sequence, millis() / 1000UL,
                      cmd->arg[0] ? " note=" : "", cmd->arg);
    } else if (cmd->verb == SNTNL_VERB_ACK) {
        /* Do not ACK an ACK: that is a packet storm between polite nodes. */
        Serial.printf("    [ACK] peer '%s' acknowledged %s\n",
                      cmd->origin[0] ? cmd->origin : "?", cmd->arg);
        return true;
    }

    emit_command_ack(vname, "OK", from_ip, from_port);
    return true;
}

void loop() {
    /* 1. Periodic Ghost Broadcast every 5 seconds */
    if (millis() - last_beacon_time >= 5000) {
        last_beacon_time = millis();
        emit_sntnl_heartbeat();
    }

    /* 2. Check for incoming Sntnl Ghost Bursts from Phone / Laptop */
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        Esp32GhostStream incoming;
        if (packetSize == sizeof(Esp32GhostStream)) {
            udp.read((char*)&incoming, sizeof(incoming));

            uint8_t golden_key = SNTNL_PHI_KEY ^ ESP32_PHI_KEY_MOD;

            /* Check mechanical gear tooth integrity */
            bool intact = esp32_ghost_verify(&incoming, golden_key);

            if (!intact) {
                Serial.printf("\n[!] ATTACK BLOCKED: Mechanical Gear Tooth Jam! Dropped tampered packet from %s:%d\n",
                              udp.remoteIP().toString().c_str(), udp.remotePort());
                /* Rapid error blink */
                for (int i = 0; i < 5; i++) {
                    digitalWrite(STATUS_LED_PIN, HIGH); delay(50);
                    digitalWrite(STATUS_LED_PIN, LOW);  delay(50);
                }
            } else {
                char secret[ESP32_CARRIER_MAX];
                esp32_ghost_decode_deep(&incoming, secret, sizeof(secret), golden_key);

                IPAddress from_ip = udp.remoteIP();
                uint16_t  from_port = (uint16_t)udp.remotePort();

                Serial.printf("\n[+] SNTNL GHOST BURST RECEIVED from %s:%u\n",
                              from_ip.toString().c_str(), from_port);
                Serial.printf("    Wire Decoy View : \"%s\"\n", incoming.carrier_text);
                Serial.printf("    Decrypted State : \"%s\"\n", secret);

                SntnlCommand cmd;
                const char *err = NULL;
                if (sntnl_command_parse(secret, &cmd, &err)) {
                    Serial.printf("    Command Frame   : %s from '%s'%s\n",
                                  sntnl_verb_name(cmd.verb),
                                  cmd.origin[0] ? cmd.origin : "?",
                                  cmd.has_token ? " [signed]" : " [unsigned]");
                    dispatch_command_frame(&cmd, secret, from_ip, from_port);
                } else if (strncmp(secret, "CMD:", 4) == 0) {
                    Serial.printf("    Command Frame   : REJECTED by grammar -- %s\n",
                                  err ? err : "malformed command");
                    Serial.println("                      (nothing was dispatched)");
                } else if (strncmp(secret, "STATE:", 6) == 0) {
                    /* The original state-sync path, unchanged. */
                    char *pipe = strchr(secret + 6, '|');
                    if (pipe) *pipe = '\0';
                    strncpy(current_state, secret + 6, sizeof(current_state) - 1);
                    current_state[sizeof(current_state) - 1] = '\0';
                    Serial.printf("    [VRDMA SYNC] Local Node State Transitioned to: \"%s\"\n", current_state);

                    /* Solid blink to confirm state transition */
                    digitalWrite(STATUS_LED_PIN, HIGH);
                    delay(300);
                    digitalWrite(STATUS_LED_PIN, LOW);
                }
            }
        }
    }
}
