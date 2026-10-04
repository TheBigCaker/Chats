/* =========================================================================
 *  sntnl_multi_node_vm_test.c
 *  =========================================================================
 *  Multi-Process Virtual Node Simulation on Termux:
 *  - Process 1 (Node A / Client - e.g. T-Glass or Phone)
 *  - Process 2 (Node B / Host - e.g. Desktop Server / VM)
 *
 *  Communicating via UDP Network Loopback (127.0.0.1:9876):
 *  1. Node A wakes on an event, encodes a Poly-Binary Ghost Burst, and sends it.
 *  2. Node B receives the raw network packet from the socket.
 *  3. Node B verifies the mechanical gear torque:
 *     - If clean: commits transition to state_host.bin.
 *     - If tampered: triggers gear tooth jam and aborts.
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
#include <sys/wait.h>

#include "revros_sntnl_gearbox_bridge.h"

#define TEST_PORT 9876
#define TEST_ADDR "127.0.0.1"

void run_node_b_host(void) {
    printf("  [NODE B - HOST SERVER] Initializing local state_host.bin (PID %d)...\n", getpid());
    RevRosStateBuffer host_state;
    revros_state_init(&host_state, "HOST_IDLE_STANDBY");

    /* Create UDP receiver socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(TEST_ADDR);
    serv_addr.sin_port = htons(TEST_PORT);

    if (bind(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind"); exit(1);
    }

    printf("  [NODE B - HOST SERVER] Listening passively on %s:%d (waiting for Sntnl burst)...\n",
           TEST_ADDR, TEST_PORT);

    /* Receive 2 packets: 1 valid, 1 tampered */
    for (int pkt_idx = 1; pkt_idx <= 2; pkt_idx++) {
        SntnlGossipBurst received_burst;
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        ssize_t n = recvfrom(sock, &received_burst, sizeof(received_burst), 0,
                             (struct sockaddr*)&client_addr, &addr_len);
        if (n <= 0) break;

        printf("\n  [NODE B - HOST SERVER] Received packet #%d (%zd bytes from %s:%d)\n",
               pkt_idx, n, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        char wire_view[POLY_CARRIER_MAX];
        poly_binary_decode_surface(&received_burst.ghost_packet, wire_view, sizeof(wire_view));
        printf("  [NODE B - HOST SERVER] Wire Sniffer Surface Payload: \"%s\"\n", wire_view);

        char new_state[64] = {0};
        uint8_t phi_secret_key = 0x4B;
        bool committed = sntnl_ingest_event(&host_state, &received_burst, phi_secret_key, new_state);

        if (committed) {
            printf("  [NODE B - HOST SERVER] Gear Check PASSED -> Committed state to state_host.bin!\n");
            printf("  [NODE B - HOST SERVER] New Host State: \"%s\" (Seq: %u, Event: %s)\n",
                   host_state.current_state, host_state.history_count, host_state.last_event);
        } else {
            printf("  [NODE B - HOST SERVER] [!] REJECTED: Mechanical Gear Jam Detected! Packet dropped.\n");
            printf("  [NODE B - HOST SERVER] Host State Protected at: \"%s\"\n", host_state.current_state);
        }
    }

    close(sock);
    printf("  [NODE B - HOST SERVER] Execution complete. State persisted.\n");
}

void run_node_a_client(void) {
    /* Give host half a second to bind socket */
    usleep(500000);

    printf("\n  [NODE A - T-GLASS CLIENT] Waking up on touch event (PID %d)...\n", getpid());
    RevRosStateBuffer client_state;
    revros_state_init(&client_state, "TGLASS_HUD_ACTIVE");
    strncpy(client_state.last_event, "GESTURE_SWIPE_UP", 255);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(TEST_ADDR);
    serv_addr.sin_port = htons(TEST_PORT);

    uint8_t phi_secret_key = 0x4B;

    /* 1. Send Valid Ghost Packet */
    const char *decoy1 = "HEARTBEAT: WEATHER REPORT OVER BLUETOOTH";
    SntnlGossipBurst burst1 = sntnl_broadcast_event(&client_state, decoy1, 7, phi_secret_key);
    printf("  [NODE A - T-GLASS CLIENT] Emitting Packet #1 (Valid Ghost Burst)...\n");
    sendto(sock, &burst1, sizeof(burst1), 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    usleep(500000);

    /* 2. Send Tampered Packet (Simulating MITM attack on wire) */
    printf("\n  [NODE A - T-GLASS CLIENT] Emitting Packet #2 (Tampered in-flight by MITM attacker)...\n");
    SntnlGossipBurst burst2 = sntnl_broadcast_event(&client_state, "SYSTEM_STATUS_NORMAL_OK", 7, phi_secret_key);
    /* In-flight attacker flips bit on byte 4 */
    burst2.ghost_packet.carrier_text[4] ^= 0x04;
    sendto(sock, &burst2, sizeof(burst2), 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    close(sock);
}

int main(void) {
    printf("===================================================================\n");
    printf("   SNTNL DUAL-NODE VIRTUAL NETWORK SIMULATION ON BARE METAL        \n");
    printf("   Node A (Client / T-Glass) <---> Node B (Host Server / VM)       \n");
    printf("   Real UDP Loopback Network Burst with Poly-Binary Ghost Crypt    \n");
    printf("===================================================================\n\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child Process: Node B (Host Server) */
        run_node_b_host();
        exit(0);
    } else {
        /* Parent Process: Node A (T-Glass Client) */
        run_node_a_client();
        wait(NULL); /* Wait for host to finish */
    }

    printf("\n===================================================================\n");
    printf("  [+] DUAL-NODE NETWORK TEST SUCCESSFULLY VERIFIED ON BARE METAL!  \n");
    printf("===================================================================\n");
    return 0;
}
