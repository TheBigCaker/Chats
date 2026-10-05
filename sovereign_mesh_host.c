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
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#include "esp32_rpsmn_sntnl/poly_binary_esp32.h"
#include "esp32_rpsmn_sntnl/sntnl_command.h"
#include "esp32_rpsmn_sntnl/mesh_auth.h"

#define SNTNL_PORT 9876
#define PHI_KEY    0x5C

/* -------------------------------------------------------------------------
 * PUBLIC FIRST BRIDGE
 * -------------------------------------------------------------------------
 * The LAN transport above only works when every node shares a broadcast
 * domain.  A node on cellular (behind carrier NAT) can never reach an ESP32
 * sitting on the house Wi-Fi.  The bridge fixes that by inverting the
 * direction: every node dials *outbound* to one public rendezvous endpoint,
 * so no port-forwarding is ever required.
 *
 *   node A (cell)  ──outbound──┐
 *                              ├──► public rendezvous relay (TCP 9900)
 *   ESP32 / LAN    ──outbound──┘
 *
 * Wire format on the relay link is length-prefixed so raw Esp32GhostStream
 * structs travel intact (a text protocol would corrupt the binary payload):
 *
 *   [uint32_be payload_len][uint8 origin_tag][payload_len-1 bytes ...]
 *
 * origin_tag is 'L' for a frame captured off the local broadcast domain.
 * Loop suppression is source-based rather than tag-based: the bridge never
 * captures a datagram that originated from loopback, so its own injected
 * fan-out (and any co-hosted bridge's) can never be re-uplinked.
 * ------------------------------------------------------------------------- */

#define RELAY_PORT_DEFAULT 9900
#define MAX_NODES          32
#define FRAME_MAX          8192

#define ORIGIN_LOCAL 'L'

/* =========================================================================
 *  COMMAND CHANNEL  (`cmd` / `listen --exec`)
 * =========================================================================
 *  The ghost mesh could already move a *state string* between nodes.  It could
 *  not ask a node to *do* something.  This section adds that, and the whole
 *  design is about the one place it is dangerous: DRYGON runs the compiler.
 *
 *  Three independent gates stand between a remote frame and an exec():
 *
 *   1. The listener must be started with --exec.  Passive listening, the
 *      relay, and the bridge never dispatch anything, so an existing
 *      deployment cannot be upgraded into a remote-execution service by the
 *      act of an attacker sending a packet.
 *   2. The verb must be in the allow list.  The default allow list is the
 *      four bounded verbs (PING/LED/STATE/REPORT) plus ACK; DRYGON has to be
 *      named explicitly.*
 *   3. A privileged verb must carry an HMAC-SHA256 tag over the command,
 *      keyed by a token this receiver was configured with.  No token here
 *      means DRYGON is refused outright -- never "allowed because unsigned".
 *
 *  And the exec itself is not a shell.  The command is decomposed by the
 *  grammar into an argv the receiver builds from a fixed template; there is no
 *  string for a shell to reinterpret, and the source/output names are checked
 *  against a bare-filename whitelist by sntnl_command.h before we get here.
 * ========================================================================= */

#define BUILD_DIR_DEFAULT   ".sntnl_build"
#define DRYGON_PATH_DEFAULT "./drygon/drygon.elf"
#define DECOY_REPLY_DEFAULT "WEATHER: OBSERVATION HOST_REPLY TEMP 24.6C HUM 46% PRESS 1013HPA PASSIVE_OK"

#define ALLOW_PING   (1u << 0)
#define ALLOW_LED    (1u << 1)
#define ALLOW_STATE  (1u << 2)
#define ALLOW_REPORT (1u << 3)
#define ALLOW_ACK    (1u << 4)
#define ALLOW_DRYGON (1u << 5)

#define ALLOW_BOUNDED_DEFAULT (ALLOW_PING | ALLOW_LED | ALLOW_STATE | ALLOW_REPORT | ALLOW_ACK)

typedef struct {
    bool        exec;        /* --exec: dispatch at all                     */
    bool        ack;         /* emit an ACK burst back to the sender       */
    unsigned    allow;       /* ALLOW_* bitmask                             */
    const char *build_dir;   /* sandbox for generated artefacts            */
    const char *drygon_path; /* the compiler to invoke                     */
    const char *token;       /* shared secret; NULL/empty disables DRYGON  */
    const char *origin;      /* this node's name, for ORIG: and ACK        */
} ExecPolicy;

/* The wire grammar is strictly upper case so that a frame means exactly one
 * thing. A human typing at a prompt is not held to that: canonicalise the verb
 * at the boundary and let the rest of the path stay strict. */
static SntnlVerb lookup_verb_for_send(const char *name, size_t len, char *canon, size_t canon_max) {
    if (!name || len == 0 || len >= canon_max) return SNTNL_VERB_NONE;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        canon[i] = c;
    }
    canon[len] = '\0';
    return sntnl_verb_lookup(canon, len);
}

static unsigned verb_to_allow(SntnlVerb v) {
    switch (v) {
        case SNTNL_VERB_PING:   return ALLOW_PING;
        case SNTNL_VERB_LED:    return ALLOW_LED;
        case SNTNL_VERB_STATE:  return ALLOW_STATE;
        case SNTNL_VERB_REPORT: return ALLOW_REPORT;
        case SNTNL_VERB_ACK:    return ALLOW_ACK;
        case SNTNL_VERB_DRYGON: return ALLOW_DRYGON;
        default:                return 0;
    }
}

/* Parse a comma-separated verb list into an ALLOW_* mask.  Unknown names are
 * a hard error rather than a silent omission -- an operator who types
 * "--allow DRYGN" must not be left believing it worked. */
static bool parse_allow(const char *spec, unsigned *mask, const char **bad, size_t *badlen) {
    unsigned m = 0;
    const char *p = spec;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len && (*p == ' ')) { p++; len--; }
        while (len && (p[len - 1] == ' ')) len--;
        if (len) {
            char canon[16];
            SntnlVerb v = lookup_verb_for_send(p, len, canon, sizeof(canon));
            if (v == SNTNL_VERB_NONE) {
                if (bad) *bad = p;
                if (badlen) *badlen = len;
                return false;
            }
            m |= verb_to_allow(v);
        }
        if (!comma) break;
        p = comma + 1;
    }
    *mask = m;
    return true;
}

/* Build a plausible telemetry line of exactly `need` bytes.  Sizing the decoy
 * explicitly matters: esp32_ghost_encode() silently truncates a payload to
 * carrier_len-1, and a truncated DRYGON command could still parse as some
 * *shorter* valid command.  Failing here is the only safe behaviour. */
static void build_decoy(char *buf, size_t n, size_t need, const char *tag) {
    char base[192];
    snprintf(base, sizeof(base),
             "WEATHER: OBSERVATION STATION_%s TEMP 24.6C HUM 46%% PRESS 1013HPA "
             "WIND 5KMH VIS 10KM UV 3 SENSOR_OK PASSIVE_OK",
             (tag && *tag) ? tag : "HOST");
    if (need > 127) need = 127;
    if (n < need + 1) need = n ? n - 1 : 0;

    size_t bl = strlen(base);
    if (bl >= need) { memcpy(buf, base, need); buf[need] = '\0'; return; }

    memcpy(buf, base, bl);
    size_t i = bl;
    const char *filler = " SENSOR_OK";
    size_t fl = strlen(filler);
    while (i + fl <= need) { memcpy(buf + i, filler, fl); i += fl; }
    while (i < need) buf[i++] = '.';
    buf[need] = '\0';
}

/* Sign (privileged verbs only) and transmit one command frame. */
static int send_command_frame(SntnlCommand cmd, const char *ip, uint16_t port,
                              const char *token, bool quiet) {
    if (!cmd.origin[0]) snprintf(cmd.origin, sizeof(cmd.origin), "HOST");

    char canonical[SNTNL_CMD_MAX + 1];
    char payload[SNTNL_CMD_MAX + 1];
    size_t clen = sntnl_command_format(&cmd, canonical, sizeof(canonical));
    if (clen == 0) { fprintf(stderr, "[-] command does not fit the payload budget\n"); return 1; }
    snprintf(payload, sizeof(payload), "%s", canonical);

    /* The tag costs "|T:" plus 16 hex characters. Check the full wire length
     * before building it, so the error names the right problem. */
    bool privileged = sntnl_verb_is_privileged(cmd.verb);
    size_t wire_len = clen + (privileged ? (size_t)(3 + SNTNL_TOKEN_HEX) : 0);
    if (!sntnl_payload_fits(wire_len)) {
        fprintf(stderr, "[-] command would be %zu bytes on the wire; the budget is %d.\n"
                        "    Shorten the argument, or drop --orig/--seq.\n", wire_len, SNTNL_CMD_MAX);
        return 1;
    }

    if (privileged) {
        if (!token || !*token) {
            fprintf(stderr,
                    "[-] refusing to send privileged verb %s without a token.\n"
                    "    A receiver would refuse it anyway; set SNTNL_MESH_TOKEN or --token.\n",
                    sntnl_verb_name(cmd.verb));
            return 1;
        }
        if (!sntnl_auth_seal(token, canonical, payload, sizeof(payload))) {
            fprintf(stderr, "[-] could not seal command\n");
            return 1;
        }
        cmd.has_token = true;
    }

    size_t plen = strlen(payload);
    if (!sntnl_payload_fits(plen)) {
        fprintf(stderr, "[-] payload is %zu bytes, budget is %d\n", plen, SNTNL_CMD_MAX);
        return 1;
    }
    size_t need = sntnl_decoy_len_for(plen);

    char decoy[ESP32_CARRIER_MAX];
    build_decoy(decoy, sizeof(decoy), need, cmd.origin);

    uint8_t golden_key = PHI_KEY ^ ESP32_PHI_KEY_MOD;
    Esp32GhostStream stream;
    esp32_ghost_encode(&stream, decoy, payload, golden_key);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int bopt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bopt, sizeof(bopt));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = inet_addr(ip ? ip : "255.255.255.255");

    ssize_t sent = sendto(sock, &stream, sizeof(stream), 0, (struct sockaddr *)&dest, sizeof(dest));
    close(sock);

    if (!quiet) {
        printf("[+] %s command emitted (%zd bytes) to %s:%u\n",
               sntnl_verb_name(cmd.verb), sent, ip ? ip : "255.255.255.255", port);
        printf("    Wire Surface View : \"%s\"\n", stream.carrier_text);
        printf("    Deep Payload      : \"%s\"\n", payload);
        printf("    Signed            : %s\n\n",
               cmd.has_token ? "yes (HMAC-SHA256, truncated to 64 bits)" : "no (bounded verb)");
    }
    return 0;
}

/* Run the compiler.  No shell, no system(), no popen(): argv is built from a
 * fixed template and the two variable slots were whitelisted by the parser. */
static int execute_drygon(const char *src, const char *out, const char *build_dir,
                          const char *drygon_path) {
    if (!sntnl_drygon_scope_ok(src)) {
        printf("    -> REFUSED: source '%s' is outside the allowed scope\n", src);
        return -1;
    }
    if (!sntnl_drygon_out_ok(out)) {
        printf("    -> REFUSED: output '%s' is outside the allowed scope\n", out);
        return -1;
    }
    if (access(src, R_OK) != 0) {
        printf("    -> REFUSED: source '%s' not readable here (%s)\n", src, strerror(errno));
        return -1;
    }
    if (access(drygon_path, X_OK) != 0) {
        printf("    -> REFUSED: compiler '%s' not executable (%s)\n", drygon_path, strerror(errno));
        return -1;
    }
    if (mkdir(build_dir, 0700) != 0 && errno != EEXIST) {
        printf("    -> REFUSED: cannot create build dir '%s' (%s)\n", build_dir, strerror(errno));
        return -1;
    }

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/%s", build_dir, out);

    printf("    -> EXEC %s %s -o %s\n", drygon_path, src, outpath);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execl(drygon_path, drygon_path, src, "-o", outpath, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        struct stat st;
        long long size = (stat(outpath, &st) == 0) ? (long long)st.st_size : -1;
        printf("    -> OK exit 0, artefact %s (%lld bytes)\n", outpath, size);
        /* A compiler that "succeeds" without producing an artefact did not
         * succeed.  Treat it as a failure rather than reporting green. */
        if (size < 0) { printf("    -> !! exit 0 but no artefact written\n"); return -1; }
        return 0;
    }
    printf("    -> FAILED exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

/* Decide what a verified command frame means, and say out loud when it is
 * refused and why.  A silent refusal is indistinguishable from a lost packet. */
static void dispatch_command(const SntnlCommand *cmd, const char *payload,
                             const ExecPolicy *pol, const char *from_ip, uint16_t from_port,
                             bool *handled) {
    *handled = false;
    if (!pol->exec) return;
    /* From here every outcome is a deliberate decision: either it was
     * dispatched, or it was refused for a reason we printed. A silent refusal
     * would be indistinguishable from a lost packet. */
    *handled = true;

    const char *vname = sntnl_verb_name(cmd->verb);

    if (!(pol->allow & verb_to_allow(cmd->verb))) {
        printf("    -> REFUSED %s: verb not in the allow list of this listener\n", vname);
        return;
    }

    char canonical[SNTNL_CMD_MAX + 1];
    if (sntnl_verb_is_privileged(cmd->verb)) {
        if (!pol->token || !*pol->token) {
            printf("    -> REFUSED %s: no token configured on this receiver\n", vname);
            printf("       (configure one with --token or SNTNL_MESH_TOKEN to allow it)\n");
            return;
        }
        if (!cmd->has_token) {
            printf("    -> REFUSED %s: privileged verb arrived unsigned\n", vname);
            return;
        }
        if (!sntnl_command_canonical(payload, canonical, sizeof(canonical))) {
            printf("    -> REFUSED %s: no canonical form (tag must be the final field)\n", vname);
            return;
        }
        if (!sntnl_auth_verify(pol->token, canonical, cmd->token)) {
            printf("    -> REFUSED %s: authentication tag mismatch\n", vname);
            return;
        }
        printf("    -> AUTH ok: HMAC-SHA256 tag verified\n");
    }

    switch (cmd->verb) {
        case SNTNL_VERB_PING:
            printf("    -> DISPATCH PING: node '%s' alive\n", cmd->origin[0] ? cmd->origin : "?");
            break;
        case SNTNL_VERB_LED:
            printf("    -> DISPATCH LED %s\n", cmd->arg);
            break;
        case SNTNL_VERB_STATE:
            printf("    -> DISPATCH STATE -> '%s'\n", cmd->arg);
            break;
        case SNTNL_VERB_REPORT:
            printf("    -> DISPATCH REPORT%s%s\n", cmd->arg[0] ? " " : "", cmd->arg);
            break;
        case SNTNL_VERB_ACK:
            printf("    -> DISPATCH ACK from '%s'%s%s\n", cmd->origin[0] ? cmd->origin : "?",
                   cmd->arg[0] ? " for " : "", cmd->arg);
            /* Do not ACK an ACK. Two polite listeners would otherwise trade
             * acknowledgements forever. */
            return;
        case SNTNL_VERB_DRYGON: {
            char out[SNTNL_ARG_MAX];
            if (cmd->out[0]) {
                snprintf(out, sizeof(out), "%s", cmd->out);
            } else {
                /* derive a default artefact name from the source, still a bare
                 * filename inside the sandbox */
                snprintf(out, sizeof(out), "%s", cmd->src);
                char *dot = strrchr(out, '.');
                if (dot) *dot = '\0';
                size_t l = strlen(out);
                snprintf(out + l, sizeof(out) - l, ".elf");
            }
            if (execute_drygon(cmd->src, out, pol->build_dir, pol->drygon_path) != 0) {
                printf("    -> DRYGON did not complete\n");
                /* Tell the sender it failed. A compile that dies silently is
                 * indistinguishable from a datagram that never arrived, and
                 * the remote caller has no other way to learn the difference. */
                if (pol->ack && from_ip) {
                    SntnlCommand fail;
                    memset(&fail, 0, sizeof(fail));
                    fail.verb = SNTNL_VERB_ACK;
                    fail.has_seq = true;
                    fail.seq = cmd->has_seq ? cmd->seq : 0;
                    snprintf(fail.arg, sizeof(fail.arg), "FAILED_DRYGON");
                    snprintf(fail.origin, sizeof(fail.origin), "%s", pol->origin ? pol->origin : "HOST");
                    printf("    -> ACK FAILED_DRYGON back to %s:%u\n", from_ip, from_port);
                    send_command_frame(fail, from_ip, from_port, NULL, true);
                }
                return;
            }
            break;
        }
        default:
            printf("    -> REFUSED %s: no dispatcher\n", vname);
            return;
    }

    *handled = true;

    if (pol->ack && from_ip) {
        SntnlCommand ack;
        memset(&ack, 0, sizeof(ack));
        ack.verb = SNTNL_VERB_ACK;
        ack.has_seq = true;
        ack.seq = cmd->has_seq ? cmd->seq : 0;
        snprintf(ack.arg, sizeof(ack.arg), "%s", vname);
        snprintf(ack.origin, sizeof(ack.origin), "%s", pol->origin ? pol->origin : "HOST");
        printf("    -> ACK %s back to %s:%u\n", vname, from_ip, from_port);
        send_command_frame(ack, from_ip, from_port, NULL, true);
    }
}


static ssize_t send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)off;
}

static bool read_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * relay: the public rendezvous point.  Accepts N outbound node links and
 * fans every frame out to all *other* peers.  Run this on any always-on
 * host and expose it with a reverse tunnel (see DESKTOP_AGENT_INSTRUCTIONS).
 * ------------------------------------------------------------------------- */
static void cmd_relay(uint16_t port) {
    printf("[*] Sntnl Public Rendezvous Relay on 0.0.0.0:%u (TCP)...\n", port);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); close(lfd); return; }
    if (listen(lfd, MAX_NODES) < 0) { perror("listen"); close(lfd); return; }

    printf("[+] Relay live. Nodes dial in outbound; no port-forwarding needed.\n\n");

    int  fds[MAX_NODES];
    char names[MAX_NODES][33];
    for (int i = 0; i < MAX_NODES; i++) { fds[i] = -1; names[i][0] = '\0'; }

    while (1) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        int maxfd = lfd;
        for (int i = 0; i < MAX_NODES; i++) {
            if (fds[i] >= 0) { FD_SET(fds[i], &rd); if (fds[i] > maxfd) maxfd = fds[i]; }
        }

        if (select(maxfd + 1, &rd, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(lfd, &rd)) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
            if (cfd >= 0) {
                char hs[64];
                memset(hs, 0, sizeof(hs));
                ssize_t n = recv(cfd, hs, sizeof(hs) - 1, 0);
                if (n <= 0) { close(cfd); continue; }

                char *nl = strchr(hs, '\n');
                if (nl) *nl = '\0';
                char tag[16] = {0}, name[33] = {0};
                sscanf(hs, "%15s %32s", tag, name);
                if (strcmp(tag, "SMTL1") != 0) {
                    printf("[-] Rejected non-mesh handshake from %s\n", inet_ntoa(peer.sin_addr));
                    close(cfd);
                    continue;
                }

                int slot = -1;
                for (int i = 0; i < MAX_NODES; i++) if (fds[i] < 0) { slot = i; break; }
                if (slot < 0) { close(cfd); continue; }

                int nodelay = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                fds[slot] = cfd;
                snprintf(names[slot], sizeof(names[slot]), "%s", name[0] ? name : "anon");
                printf("[+] NODE JOIN: '%s' from %s:%d (slot %d)\n",
                       names[slot], inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), slot);
            }
        }

        for (int i = 0; i < MAX_NODES; i++) {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &rd)) continue;

            uint32_t len_be = 0;
            if (!read_all(fds[i], &len_be, sizeof(len_be))) {
                printf("[-] NODE LEFT : '%s' (slot %d)\n", names[i], i);
                close(fds[i]);
                fds[i] = -1;
                continue;
            }

            uint32_t len = ntohl(len_be);
            if (len == 0 || len > FRAME_MAX) {
                printf("[-] Frame size %u out of range from '%s' -- dropping node\n", len, names[i]);
                close(fds[i]);
                fds[i] = -1;
                continue;
            }

            uint8_t frame[FRAME_MAX];
            if (!read_all(fds[i], frame, len)) {
                close(fds[i]);
                fds[i] = -1;
                continue;
            }

            printf("[~] ROUTE '%s' -> mesh (%u bytes, origin=%c)\n", names[i], len, frame[0]);

            for (int j = 0; j < MAX_NODES; j++) {
                if (j == i || fds[j] < 0) continue;
                if (send_all(fds[j], &len_be, sizeof(len_be)) < 0 ||
                    send_all(fds[j], frame, len) < 0) {
                    close(fds[j]);
                    fds[j] = -1;
                }
            }
        }
    }

    close(lfd);
}

/* -------------------------------------------------------------------------
 * bridge: a node-side agent.  Dials the public relay and simultaneously
 * scoops UDP ghost bursts off the local broadcast domain, so a LAN-locked
 * ESP32 reaches a cellular peer (and vice versa).
 *
 *   child  : local UDP :9876  -> relay        (captures ESP32 broadcasts)
 *   parent : relay            -> local :9877  (injects remote bursts)
 *
 * The capture and injection ports are deliberately different: a UDP socket
 * bound to the port it also sends to would win the kernel's SO_REUSEPORT
 * load-balancing coin-flip and swallow the very frames it is injecting,
 * starving any co-resident local listener.
 * ------------------------------------------------------------------------- */
static void cmd_bridge(const char *host, uint16_t port, const char *name,
                       uint16_t lan_in, uint16_t lan_out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[-] Cannot resolve relay '%s'\n", host);
        return;
    }

    int rfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (rfd < 0 || connect(rfd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "[-] Cannot reach relay %s:%u (%s)\n", host, port, strerror(errno));
        if (rfd >= 0) close(rfd);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);

    int nodelay = 1;
    setsockopt(rfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char hs[96];
    int hlen = snprintf(hs, sizeof(hs), "SMTL1 %s\n", name);
    if (send_all(rfd, hs, (size_t)hlen) < 0) { close(rfd); return; }

    printf("[+] Bridged to public relay %s:%u as node '%s'\n", host, port, name);
    printf("[+] Local mesh IN  : UDP :%u (ESP32 / LAN peers)\n", lan_in);
    printf("[+] Local mesh OUT : UDP :%u (local listeners bind here)\n\n", lan_out);

    int lfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (lfd < 0) { perror("socket"); close(rfd); return; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in laddr;
    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_addr.s_addr = INADDR_ANY;
    laddr.sin_port = htons(lan_in);
    if (bind(lfd, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
        perror("bind local mesh face");
        close(lfd);
        close(rfd);
        return;
    }

    /* Separate send-only socket: never bound to lan_out, so the injection is
     * delivered to the local listener instead of being stolen back. */
    int ofd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ofd < 0) { perror("socket"); close(lfd); close(rfd); return; }

    struct sockaddr_in inject;
    memset(&inject, 0, sizeof(inject));
    inject.sin_family = AF_INET;
    inject.sin_addr.s_addr = inet_addr("127.0.0.1");
    inject.sin_port = htons(lan_out);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(lfd); close(rfd); return; }

    /* Production guard: a frame arriving from loopback is our own fan-out and
     * must not be re-uplinked.  Same-host test rigs can opt out. */
    bool allow_loopback = (getenv("SMTL_ALLOW_LOOPBACK") != NULL);

    if (pid == 0) {
        /* child: local UDP -> relay */
        while (1) {
            uint8_t frame[FRAME_MAX];
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            ssize_t n = recvfrom(lfd, frame + 1, FRAME_MAX - 1, 0, (struct sockaddr *)&src, &slen);
            if (n <= 0) continue;

            if (!allow_loopback && ntohl(src.sin_addr.s_addr) == INADDR_LOOPBACK) continue;

            frame[0] = ORIGIN_LOCAL;
            uint32_t len = (uint32_t)n + 1;
            uint32_t len_be = htonl(len);
            if (send_all(rfd, &len_be, sizeof(len_be)) < 0 || send_all(rfd, frame, len) < 0) {
                fprintf(stderr, "[-] Relay link dropped (uplink)\n");
                _exit(0);
            }
            printf("[^] UPLINK   %u bytes from local mesh (%s) -> relay\n",
                   len, inet_ntoa(src.sin_addr));
            fflush(stdout);
        }
    }

    /* parent: relay -> local mesh */
    while (1) {
        uint32_t len_be = 0;
        if (!read_all(rfd, &len_be, sizeof(len_be))) {
            fprintf(stderr, "[-] Relay link dropped (downlink)\n");
            break;
        }

        uint32_t len = ntohl(len_be);
        if (len == 0 || len > FRAME_MAX) break;

        uint8_t frame[FRAME_MAX];
        if (!read_all(rfd, frame, len)) break;

        if (frame[0] != ORIGIN_LOCAL) {
            printf("[=] Unknown origin tag 0x%02X -- frame dropped\n", frame[0]);
            fflush(stdout);
            continue;
        }

        ssize_t s = sendto(ofd, frame + 1, (size_t)len - 1, 0, (struct sockaddr *)&inject, sizeof(inject));
        printf("[v] DOWNLINK %zd bytes relay -> local mesh :%u\n", s, lan_out);
        fflush(stdout);
    }

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    close(ofd);
    close(lfd);
    close(rfd);
}

void cmd_listen(uint16_t port, const ExecPolicy *pol) {
    printf("[*] Starting Sntnl Ghost Listener on 0.0.0.0:%u...\n", port);
    if (pol->exec) {
        printf("[!] COMMAND DISPATCH ENABLED\n");
        printf("      allow     :");
        static const struct { unsigned bit; const char *name; } tbl[] = {
            { ALLOW_PING, "PING" }, { ALLOW_LED, "LED" }, { ALLOW_STATE, "STATE" },
            { ALLOW_REPORT, "REPORT" }, { ALLOW_ACK, "ACK" }, { ALLOW_DRYGON, "DRYGON" }
        };
        for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
            if (pol->allow & tbl[i].bit) printf(" %s", tbl[i].name);
        printf("\n      build dir : %s\n", pol->build_dir);
        printf("      compiler  : %s\n", pol->drygon_path);
        printf("      token     : %s\n", (pol->token && *pol->token)
               ? "configured -- privileged verbs may be honoured"
               : "NOT configured -- DRYGON will be refused");
        if (pol->allow & ALLOW_DRYGON) {
            if (!(pol->token && *pol->token))
                printf("[!] DRYGON is allowed but there is no token: every DRYGON will be refused.\n");
            printf("[!] DRYGON invokes the compiler as this user. Only enable it on a network you trust.\n");
        }
    } else {
        printf("[i] Passive mode: frames are parsed and reported, never dispatched.\n"
               "    Re-run with 'listen %u --exec' to enable dispatch.\n", port);
    }
    printf("\n");

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

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

        char from_ip[INET_ADDRSTRLEN];
        snprintf(from_ip, sizeof(from_ip), "%s", inet_ntoa(client_addr.sin_addr));
        uint16_t from_port = ntohs(client_addr.sin_port);

        printf("-------------------------------------------------------------------\n");
        printf("[+] RECEIVED PACKET from %s:%u (%zd bytes)\n", from_ip, from_port, n);
        printf("    Surface Wire View: \"%s\"\n", pkt.carrier_text);

        bool intact = esp32_ghost_verify(&pkt, golden_key);
        if (!intact) {
            printf("    [!] REJECTED: Mechanical Gear Jam Detected! Packet was tampered!\n");
        } else {
            char secret[ESP32_CARRIER_MAX];
            esp32_ghost_decode_deep(&pkt, secret, sizeof(secret), golden_key);
            printf("    Gear Tooth Check : 100%% INTACT (Torque Hash Verified)\n");
            printf("    Decrypted State  : \"%s\"\n", secret);

            SntnlCommand cmd;
            const char *err = NULL;
            if (sntnl_command_parse(secret, &cmd, &err)) {
                printf("    Command Frame    : %s from '%s'%s\n", sntnl_verb_name(cmd.verb),
                       cmd.origin[0] ? cmd.origin : "?",
                       cmd.has_token ? " [signed]" : " [unsigned]");
                bool handled = false;
                dispatch_command(&cmd, secret, pol, from_ip, from_port, &handled);
                if (!handled)
                    printf("    -> not dispatched: this listener is passive "
                           "(restart it with 'listen %u --exec' to act on commands)\n", port);
            } else if (strncmp(secret, "CMD:", 4) == 0) {
                printf("    Command Frame    : REJECTED by grammar -- %s\n",
                       err ? err : "malformed command");
                printf("                       (nothing was dispatched)\n");
            }
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

/* snprintf would silently truncate. A truncated field at the wire boundary
 * would produce a different -- and still perfectly valid -- command, so refuse
 * instead of sending something the operator did not type. */
static bool copy_checked(char *dst, size_t dstsz, const char *src, const char *what) {
    size_t n = strlen(src);
    if (n >= dstsz) {
        fprintf(stderr, "[-] %s is %zu bytes; the field allows %zu\n", what, n, dstsz - 1);
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

static void print_cmd_usage(const char *prog) {
    printf("Usage: %s cmd <VERB> [argument] [options]\n\n", prog);
    printf("  Bounded verbs -- honoured by any node that reaches this frame:\n");
    printf("    ping                              ask listeners to report in\n");
    printf("    led <on|off|blink|identify>       drive the node's indicator\n");
    printf("    state <NAME>                      set the node's visible state\n");
    printf("    report [text]                     ask for a status report\n");
    printf("\n  Privileged verb -- runs the compiler, so it must carry a token:\n");
    printf("    drygon <file.dry> [--out name.elf]\n");
    printf("\n  Options: --to <ip>  --port <n>  --seq <n>  --orig <name>  --token <secret>\n");
    printf("  The token also comes from $SNTNL_MESH_TOKEN, the node name from $SNTNL_MESH_ORIG.\n\n");
}

int cmd_command(int argc, char *argv[], int first) {
    if (first >= argc || strcmp(argv[first], "--help") == 0) {
        print_cmd_usage(argv[0]);
        return first >= argc ? 1 : 0;
    }

    SntnlCommand cmd;
    memset(&cmd, 0, sizeof(cmd));

    const char *verbname = argv[first++];
    char canonverb[16];
    cmd.verb = lookup_verb_for_send(verbname, strlen(verbname), canonverb, sizeof(canonverb));
    if (cmd.verb == SNTNL_VERB_NONE) {
        fprintf(stderr, "[-] unknown verb '%s'\n\n", verbname);
        print_cmd_usage(argv[0]);
        return 1;
    }

    const char *ip = "255.255.255.255";
    uint16_t    port = SNTNL_PORT;
    const char *token = getenv("SNTNL_MESH_TOKEN");
    const char *orig = getenv("SNTNL_MESH_ORIG");
    const char *arg = NULL;
    const char *out = NULL;
    unsigned long seq = 0;
    bool have_seq = false;

    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if      (strcmp(a, "--to") == 0 && i + 1 < argc)    ip = argv[++i];
        else if (strcmp(a, "--port") == 0 && i + 1 < argc)  port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(a, "--out") == 0 && i + 1 < argc)   out = argv[++i];
        else if (strcmp(a, "--token") == 0 && i + 1 < argc) token = argv[++i];
        else if (strcmp(a, "--orig") == 0 && i + 1 < argc)  orig = argv[++i];
        else if (strcmp(a, "--seq") == 0 && i + 1 < argc)   { seq = strtoul(argv[++i], NULL, 10); have_seq = true; }
        else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "[-] unknown option '%s'\n\n", a);
            print_cmd_usage(argv[0]);
            return 1;
        }
        else if (!arg) arg = a;
        else {
            fprintf(stderr, "[-] unexpected extra argument '%s' -- quote multi-word arguments\n", a);
            return 1;
        }
    }

    if (cmd.verb == SNTNL_VERB_DRYGON) {
        if (!arg) { fprintf(stderr, "[-] drygon needs a source filename\n\n"); print_cmd_usage(argv[0]); return 1; }
        if (!sntnl_drygon_scope_ok(arg)) {
            fprintf(stderr,
                    "[-] source '%s' is outside the allowed scope.\n"
                    "    The receiver builds the argv itself, so only a bare relative\n"
                    "    filename ending in .dry/.ccp/.c is accepted -- no '/', no '..',\n"
                    "    no leading '.' or '-'.\n", arg);
            return 1;
        }
        if (!copy_checked(cmd.src, sizeof(cmd.src), arg, "source name")) return 1;
        if (out) {
            if (!sntnl_drygon_out_ok(out)) {
                fprintf(stderr, "[-] output '%s' is outside the allowed scope (bare .elf/.ros name)\n", out);
                return 1;
            }
            if (!copy_checked(cmd.out, sizeof(cmd.out), out, "output name")) return 1;
        }
    } else {
        if ((cmd.verb == SNTNL_VERB_LED || cmd.verb == SNTNL_VERB_STATE) && !arg) {
            fprintf(stderr, "[-] %s needs an argument\n\n", verbname);
            print_cmd_usage(argv[0]);
            return 1;
        }
        if (arg) {
            if (cmd.verb == SNTNL_VERB_LED && !sntnl_led_arg_ok(arg)) {
                fprintf(stderr, "[-] led argument must be one of on|off|blink|identify\n");
                return 1;
            }
            if (!copy_checked(cmd.arg, sizeof(cmd.arg), arg, "argument")) return 1;
        }
    }

    if (have_seq) { cmd.has_seq = true; cmd.seq = (uint32_t)seq; }
    if (orig && !copy_checked(cmd.origin, sizeof(cmd.origin), orig, "node name")) return 1;

    if (cmd.verb == SNTNL_VERB_DRYGON) {
        printf("[i] drygon is privileged: it will only run on a listener that has\n"
               "    --exec, DRYGON in its allow list, and this same token.\n");
    }

    return send_command_frame(cmd, ip, port, token, false);
}

int main(int argc, char *argv[]) {
    /* A mesh node's log is the only record of what it was asked to do. Buffered
     * stdout would lose the tail of it on a crash or a kill, so keep it line
     * buffered even when it is redirected to a file. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("===================================================================\n");
    printf("   SOVEREIGN MESH HOST BRIDGE (Termux / Linux <-> ESP32 / Laptop)  \n");
    printf("===================================================================\n\n");

    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s listen [port] [--exec] [--allow <verbs>] [--build-dir <dir>] [--token <secret>]\n", argv[0]);
        printf("                             - Listen for ghost frames; --exec makes it act on commands\n");
        printf("  %s cmd <VERB> [argument] [--to <ip>] [--token <secret>]\n", argv[0]);
        printf("                             - Send a command frame ('%s cmd --help' for the verbs)\n", argv[0]);
        printf("  %s send <state> [ip]       - Send ghost state command to peer or broadcast\n", argv[0]);
        printf("  %s tamper-test [ip]        - Send tampered packet to demonstrate gear jam\n", argv[0]);
        printf("  %s relay [port]            - PUBLIC rendezvous point; nodes dial in outbound\n", argv[0]);
        printf("  %s bridge <host> <port> <name> [lan_in] [lan_out]\n", argv[0]);
        printf("                             - Join the mesh through a public relay (NAT-proof)\n\n");
        return 1;
    }

    if (strcmp(argv[1], "listen") == 0) {
        uint16_t p = SNTNL_PORT;
        ExecPolicy pol;
        memset(&pol, 0, sizeof(pol));
        pol.ack        = true;                  /* answer command frames by default */
        pol.allow      = ALLOW_BOUNDED_DEFAULT; /* DRYGON is opt-in, never default   */
        pol.build_dir  = getenv("SNTNL_MESH_BUILD_DIR") ? getenv("SNTNL_MESH_BUILD_DIR") : BUILD_DIR_DEFAULT;
        pol.drygon_path = getenv("SNTNL_DRYGON") ? getenv("SNTNL_DRYGON") : DRYGON_PATH_DEFAULT;
        pol.token      = getenv("SNTNL_MESH_TOKEN");
        pol.origin     = getenv("SNTNL_MESH_ORIG");

        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            if      (strcmp(a, "--exec") == 0)     pol.exec = true;
            else if (strcmp(a, "--no-ack") == 0)   pol.ack = false;
            else if (strcmp(a, "--allow") == 0 && i + 1 < argc) {
                const char *bad = NULL;
                size_t badlen = 0;
                if (!parse_allow(argv[++i], &pol.allow, &bad, &badlen)) {
                    fprintf(stderr, "[-] unknown verb '%.*s' in --allow\n", (int)badlen, bad);
                    return 1;
                }
            }
            else if (strcmp(a, "--build-dir") == 0 && i + 1 < argc) pol.build_dir = argv[++i];
            else if (strcmp(a, "--token") == 0 && i + 1 < argc)     pol.token = argv[++i];
            else if (strcmp(a, "--drygon") == 0 && i + 1 < argc)    pol.drygon_path = argv[++i];
            else if (strcmp(a, "--orig") == 0 && i + 1 < argc)      pol.origin = argv[++i];
            else if (a[0] == '-' && a[1] == '-') {
                fprintf(stderr, "[-] unknown option '%s'\n", a);
                return 1;
            }
            else p = (uint16_t)atoi(a);
        }
        cmd_listen(p, &pol);
    } else if (strcmp(argv[1], "cmd") == 0) {
        return cmd_command(argc, argv, 2);
    } else if (strcmp(argv[1], "relay") == 0) {
        uint16_t p = (argc >= 3) ? (uint16_t)atoi(argv[2]) : RELAY_PORT_DEFAULT;
        cmd_relay(p);
    } else if (strcmp(argv[1], "bridge") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: %s bridge <relay_host> <relay_port> <node_name> [lan_in_port] [lan_out_port]\n", argv[0]);
            return 1;
        }
        uint16_t rp = (uint16_t)atoi(argv[3]);
        uint16_t lin = (argc >= 6) ? (uint16_t)atoi(argv[5]) : SNTNL_PORT;
        uint16_t lout = (argc >= 7) ? (uint16_t)atoi(argv[6]) : (uint16_t)(SNTNL_PORT + 1);
        cmd_bridge(argv[2], rp, argv[4], lin, lout);
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
