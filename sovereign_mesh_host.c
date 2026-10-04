/* =========================================================================
 *  sovereign_mesh_host.c
 *  =========================================================================
 *  Sovereign Host CLI Tool for Termux & Linux:
 *  - Communicates with ESP32 (Node #8) and Laptop peers over Wi-Fi / LAN
 *  - Sends & Receives Poly-Binary Ghost Streams with Golden Ratio (Phi) Key
 *  - Mechanical Gear Tooth Integrity Checking & Tamper Simulation
 * ========================================================================= */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp32_rpsmn_sntnl/poly_binary_esp32.h"

#define SNTNL_PORT 9876
#define PHI_KEY    0x5C

void cmd_listen(void) {
    printf("[*] Starting Sntnl Passive Ghost Listener on 0.0.0.0:%d...\n", SNTNL_PORT);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(SNTNL_PORT);

    if (bind(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(sock);
        return;
    }

    printf("[+] Listening for Ghost Bursts from ESP32 or Laptop peers (Ctrl+C to stop)...\n\n");

    uint8_t golden_key = PHI_KEY ^ ESP32_PHI_KEY_MOD;

    while (1) {
        Esp32GhostStream pkt;
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        ssize_t n = recvfrom(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&client_addr, &addr_len);
        if (n <= 0) break;

        printf("-------------------------------------------------------------------\n");
        printf("[+] RECEIVED PACKET from %s:%d (%zd bytes)\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), n);
        printf("    Surface Wire View: \"%s\"\n", pkt.carrier_text);

        bool intact = esp32_ghost_verify(&pkt, golden_key);
        if (!intact) {
            printf("    [!] REJECTED: Mechanical Gear Jam Detected! Packet was tampered!\n");
        } else {
            char secret[128];
            esp32_ghost_decode_deep(&pkt, secret, sizeof(secret), golden_key);
            printf("    Gear Tooth Check : 100%% INTACT (Torque Hash Verified)\n");
            printf("    Decrypted State  : \"%s\"\n", secret);
        }
        printf("-------------------------------------------------------------------\n\n");
    }

    close(sock);
}

void cmd_send(const char *target_ip, const char *new_state, bool inject_tamper) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return; }

    int broadcast_opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SNTNL_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(target_ip ? target_ip : "255.255.255.255");

    const char *decoy = "WEATHER: OBSERVATION HOST_4402 SUNNY 74F WIND 5MPH PASSIVE_OK";
    char secret[64];
    snprintf(secret, sizeof(secret), "STATE:%s|ORIGIN:HOST", new_state);

    uint8_t golden_key = PHI_KEY ^ ESP32_PHI_KEY_MOD;
    Esp32GhostStream stream;
    esp32_ghost_encode(&stream, decoy, secret, golden_key);

    if (inject_tamper) {
        printf("[!] Injecting single-bit tamper into packet...\n");
        stream.carrier_text[8] ^= 0x01;
    }

    ssize_t sent = sendto(sock, &stream, sizeof(stream), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    printf("[+] Emitted Sntnl Ghost Burst (%zd bytes) to %s:%d\n", sent, target_ip ? target_ip : "255.255.255.255", SNTNL_PORT);
    printf("    Wire Surface View : \"%s\"\n", stream.carrier_text);
    printf("    Secret State Sent : \"%s\"\n\n", secret);

    close(sock);
}

int main(int argc, char *argv[]) {
    printf("===================================================================\n");
    printf("   SOVEREIGN MESH HOST BRIDGE (Termux / Linux <-> ESP32 / Laptop)  \n");
    printf("===================================================================\n\n");

    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s listen                  - Listen passively for ESP32/peer ghost beacons\n", argv[0]);
        printf("  %s send <state> [ip]       - Send ghost state command to peer or broadcast\n", argv[0]);
        printf("  %s tamper-test [ip]        - Send tampered packet to demonstrate gear jam\n\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "listen") == 0) {
        cmd_listen();
    } else if (strcmp(argv[1], "send") == 0) {
        const char *st = (argc >= 3) ? argv[2] : "HOST_ACTIVE";
        const char *ip = (argc >= 4) ? argv[3] : "127.0.0.1";
        cmd_send(ip, st, false);
    } else if (strcmp(argv[1], "tamper-test") == 0) {
        const char *ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        cmd_send(ip, "ATTACK_PAYLOAD", true);
    } else {
        printf("Unknown command '%s'\n", argv[1]);
        return 1;
    }

    return 0;
}
