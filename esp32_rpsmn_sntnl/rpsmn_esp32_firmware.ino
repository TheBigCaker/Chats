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

/* Wi-Fi Configuration: Set your LAN credentials or leave empty for Auto-AP */
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

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
    Serial.printf("[+] Clifford Torus Seed: 0x%016llX\n\n", ESP32_CLIFFORD_SEED);

    /* Blink 3 times to indicate ready */
    for (int i = 0; i < 3; i++) {
        digitalWrite(STATUS_LED_PIN, HIGH); delay(100);
        digitalWrite(STATUS_LED_PIN, LOW);  delay(100);
    }
}

/* Emit a Sntnl Ghost Heartbeat disguised as weather telemetry */
void emit_sntnl_heartbeat() {
    float temp_c = 24.5f + (float)(random(-10, 10)) * 0.1f;
    int hum = 45 + random(-5, 5);

    char decoy[ESP32_CARRIER_MAX];
    snprintf(decoy, sizeof(decoy),
             "WEATHER: OBSERVATION ESP32_NODE_8 TEMP %.1fC HUM %d%% PRESS 1013HPA PASSIVE_OK",
             temp_c, hum);

    char secret[64];
    snprintf(secret, sizeof(secret), "STATE:%s|NODE:8|SEQ:%u", current_state, state_sequence++);

    uint8_t golden_key = SNTNL_PHI_KEY ^ ESP32_PHI_KEY_MOD;
    Esp32GhostStream stream;
    esp32_ghost_encode(&stream, decoy, secret, golden_key);

    /* Broadcast over UDP to all peers on the subnet */
    IPAddress broadcastIP(255, 255, 255, 255);
    udp.beginPacket(broadcastIP, SNTNL_UDP_PORT);
    udp.write((const uint8_t*)&stream, sizeof(stream));
    udp.endPacket();

    Serial.printf("[SNTNL EMIT] Seq: %u | Surface Decoy: \"%s\"\n",
                  state_sequence - 1, stream.carrier_text);
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
                char secret[128];
                esp32_ghost_decode_deep(&incoming, secret, sizeof(secret), golden_key);

                Serial.printf("\n[+] SNTNL GHOST BURST RECEIVED from %s:%d\n",
                              udp.remoteIP().toString().c_str(), udp.remotePort());
                Serial.printf("    Wire Decoy View : \"%s\"\n", incoming.carrier_text);
                Serial.printf("    Decrypted State : \"%s\"\n", secret);

                /* Apply state update */
                if (strncmp(secret, "STATE:", 6) == 0) {
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
