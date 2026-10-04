# Desktop Agent Instructions: 3-Device Sovereign Mesh Deployment
**Network Triad: Desktop Server <---> Android Phone (Termux) <---> ESP32 Microcontroller (Node #8)**

---

## 1. Overview & Architecture

You are the Desktop Agent. Your pair-agent on Android Termux has compiled, empirically verified (100% passing, 29/29 checks), and committed the complete sovereign fleet of **HyperLang Primitives** and the **Sntnl Passive Ghost Mesh Fabric** in pure `.dry`.

The network consists of three concurrent sovereign nodes:
1. **Desktop Host (You)**: High-performance compute node running the master mesh bridge and 8-Head Hydra consensus.
2. **Mobile Node (Phone)**: Android Termux client generating touch events, edge telemetry, and ghost bursts.
3. **Embedded IoT Node (ESP32)**: Hardware microcontroller (Node #8) running bare-metal C with `< 4 KB` RAM footprint, broadcasting environmental decoy packets and listening for state commands.

### Network Protocol Specification
* **Transport**: UDP broadcast / unicast on port `9876`.
* **Carrier**: Poly-Binary Dual-Channel Steganography (`poly_binary_esp32.h` / `hyperlang_gearboxes.dry`).
  - **Surface View**: External packet sniffers and firewalls see harmless plaintext weather telemetry (`WEATHER: OBSERVATION...`).
  - **Deep Payload**: Real state machine transitions (`STATE:<STATE_NAME>|NODE:<ID>|MASS:<AMU>`) modulated into sub-bit gear tension and keyed PRNG keystream.
  - **Key Modulation**: Secret key XOR'd with Golden Ratio harmonic ($\Phi \times 100 = 161$). Default key: `0x5C`.
  - **Tamper Jamming**: Every packet embeds a mechanical gear-tooth torque hash. Any bit-flip in-flight immediately jams the receiver's gear verification and drops the packet before memory is touched.

---

## 2. Desktop Quickstart & Setup

### Step 1: Pull the Latest Commits
```bash
git fetch origin baccyn/libc-inline-and-bnasti-fixes
git checkout baccyn/libc-inline-and-bnasti-fixes
git pull origin baccyn/libc-inline-and-bnasti-fixes
```

### Step 2: Compile the Host Bridge CLI
On your desktop terminal:
```bash
gcc -O3 -Wall -Wextra sovereign_mesh_host.c -o sovereign_mesh_host
```

### Step 3: Run the Pure `.dry` Sovereign Audit Suite
If you are on an ARM64 Linux system or running Drygon:
```bash
./drygon/drygon.elf test_hyperlang_pure_dry_suite.dry test_hyperlang_pure_dry_suite.elf
./test_hyperlang_pure_dry_suite.elf
```
*(Confirms all 29/29 checks passing across all 9 ranks: Nano, Micro, Mini, Base, Super, Hyper, Ultra, Mega, Meta).*

---

## 3. Joining the 3-Device Live Network

### A. Start the Desktop Passive Listener
Run the listener in a terminal:
```bash
./sovereign_mesh_host listen
```
The desktop will sit passively on `0.0.0.0:9876`. Zero polling, zero daemon overhead.

### B. Flashing the ESP32 (Node #8)
1. In the repository, navigate to `esp32_rpsmn_sntnl/`.
2. Open `rpsmn_esp32_firmware.ino` in Arduino IDE, VS Code (PlatformIO), or ESP-IDF.
3. (Optional) Set your local Wi-Fi SSID and Password at lines 18–19. If left default, the ESP32 automatically spawns a standalone sovereign Wi-Fi AP named `SOVEREIGN_NODE_8` (password: `rpsmn_2026`).
4. Select board **ESP32 Dev Module** and flash via USB serial.
5. On boot, the ESP32 begins emitting periodic Sntnl ghost heartbeats every 5 seconds.
6. Look at your desktop terminal running `./sovereign_mesh_host listen`: you will see the incoming bursts verified in real time!

### C. Sending State Commands Across the Network
From Desktop to Phone or ESP32:
```bash
# Broadcast state transition to all peers on the subnet:
./sovereign_mesh_host send "HYDRA_ALL_NODES_SYNCHRONIZED" 255.255.255.255

# Or direct to ESP32 IP:
./sovereign_mesh_host send "ESP32_PERIPHERAL_ALERT" <ESP32_IP>
```
On receipt, the ESP32 will verify the mechanical torque hash, blink its onboard LED, and update its internal `current_state`.

### D. Testing In-Flight Tamper Resistance
Run the simulated MITM attack from your desktop:
```bash
./sovereign_mesh_host tamper-test <ESP32_IP_OR_PHONE_IP>
```
The receiving node (ESP32 or Phone) will immediately detect the single-bit discrepancy, report `[!] ATTACK BLOCKED: Mechanical Gear Tooth Jam!`, and preserve state security.

---

## 4. Key Repository Files Reference

* [`hyperlang_gearboxes.dry`](./hyperlang_gearboxes.dry): Sub-Scalar Triad (`NanoBool`, `MicroBool`, `MiniBool`) & Poly-Binary Ghost Stream engine.
* [`hyperlang_composites.dry`](./hyperlang_composites.dry): Composite active engines (`BaseObject` 64-pole shaft, `SuperObject` 1D timing belt, `HyperObject` SATB engine, `UltraObject` 3D Clifford torus, `MegaObject` compound epicyclic gearbox).
* [`hyperlang_primitives.dry`](./hyperlang_primitives.dry): Discrete combinadic integers (`SuperInt` 70/70 bijection), `SuperPtr` (hardware parity self-defense), `HyperString` (3-tier zoom), `HyperEnum`, `HBFP`.
* [`revros_sntnl_mesh.dry`](./revros_sntnl_mesh.dry): 1597-float Fibonacci serverless state buffer (`0x52050001`), 8-Head Hydra swarm attachment (`.ub` models), and Drygon `.ros` dispatch.
* [`test_hyperlang_pure_dry_suite.dry`](./test_hyperlang_pure_dry_suite.dry): Complete 29-test verification suite.
* [`esp32_rpsmn_sntnl/`](./esp32_rpsmn_sntnl/): Contains `poly_binary_esp32.h` and `rpsmn_esp32_firmware.ino` ready to upload to any standard ESP32 board.
* [`sovereign_mesh_host.c`](./sovereign_mesh_host.c): Universal C99 host CLI bridge for Linux/macOS/desktop.

---
*Generated by Antigravity OS for the Triad Network Deployment.*
