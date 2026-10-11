#!/usr/bin/env bash
# matmul_clusters.sh — wire matrices into real matmul + similarity clusters.
#
# Strategy:
#  1. For each backend, compute the per-instruction opcode-as-vector (32 bits).
#  2. Compute a backend cross-similarity matrix via matmul of normalized vectors.
#  3. Greedy cluster instructions across backends by opcode bit-pattern similarity.
#
# Uses bash bigint (since drygon isn't compiled and available right now).

set -u
MAT_DIR="${1:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer/matrices}"
OUT_DIR="${2:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer}"

# All backends
BACKENDS="aarch64 amdgpu arm avr mali mips nvptx powerpc riscv sparc x86"

echo "=== 1. Loading matrices ==="
# Load each matrix into memory: declare -A M_<backend>[inst]="b0,b1,b2,b3,b4,b5,b6,b7"
declare -A M
for b in $BACKENDS; do
    f="$MAT_DIR/$b.tsv"
    [[ -f "$f" ]] || { echo "missing $f"; continue; }
    while IFS=$'\t' read -r -a row; do
        [[ "${row[0]}" == "INST" ]] && continue
        inst="${row[0]}"
        # Take bytes 0..31 as 32-bit value (bits 0..7 = byte0, ...)
        # Reverse for big-endian layout (bit 0 = MSB)
        # We'll treat byte3 as highest, byte0 as lowest (little-endian)
        M["$b|$inst"]="${row[1]},${row[2]},${row[3]},${row[4]}"
    done < "$f"
    echo "  $b: ${#M[@]} entries so far"
done

# Per-backend instruction lists
declare -A insts
for k in "${!M[@]}"; do
    b="${k%%|*}"
    i="${k##*|}"
    insts["$b"]="${insts[$b]:-} $i"
done

echo ""
echo "=== 2. Computing per-instruction 32-bit opcode value (big-endian) ==="
# Convert 4 bytes (B0..B3) to 32-bit value using bash bigint
declare -A OP32
for k in "${!M[@]}"; do
    bytes="${M[$k]}"
    IFS=',' read -r -a bs <<< "$bytes"
    # Byte 3 = high, byte 0 = low (little-endian reading as 32-bit word)
    # value = b0 | (b1<<8) | (b2<<16) | (b3<<24)
    OP32["$k"]=$(( ${bs[0]} + (${bs[1]} << 8) + (${bs[2]} << 16) + (${bs[3]} << 24) ))
done

# Verify with ADDG: bit31=1 (b3 has 128), bits28-24=10001 (b3 has 17)
# So b3 = 128|17 = 145, b0=b1=b2=0
# OP32 = 0 + 0 + 0 + (145<<24) = 2432696320
echo "ADDG aarch64 OP32 = ${OP32[aarch64|ADDG]}  (expected 2432696320 = 0x91000000)"
echo "  hex: $(printf '0x%x' ${OP32[aarch64|ADDG]})"

echo ""
echo "=== 3. Computing backend-pairwise opcode centroid matmul ==="
# For each backend pair, compute the dot product of (sum of normalized opcodes)
# Simpler: build per-backend opcode histogram of upper 16 bits, then dot product.
# Real matmul: matrix B × matrix B^T (per backend) → nxn matrix of opcode similarities.

declare -A CENTROIDS
for b in $BACKENDS; do
    # Centroid = sum of all (op32 >> 16) values (upper 16 bits, easier to multiply)
    sum=0
    for k in "${!OP32[@]}"; do
        [[ "${k%%|*}" == "$b" ]] || continue
        v=${OP32["$k"]}
        sum=$(( sum + (v >> 16) ))
    done
    CENTROIDS[$b]=$sum
done

# Print centroid table
echo ""
echo "Backend opcode centroid (sum of upper-16-bits):"
for b in $BACKENDS; do
    echo "  $b: ${CENTROIDS[$b]}"
done

# Pairwise dot-product of backend histograms via matmul:
# matrix B has rows = instructions, cols = 16 features (upper16 bits split into 16 single bits)
# B × B^T → N_inst_a × N_inst_b similarity
# For full matmul: too expensive in bash. Use vector form: histogram bin dot product.

echo ""
echo "=== 4. Building per-backend 16-bin opcode histograms ==="
declare -A HIST
for b in $BACKENDS; do
    for ((bit=0; bit<16; bit++)); do HIST["$b|$bit"]=0; done
    for k in "${!OP32[@]}"; do
        [[ "${k%%|*}" == "$b" ]] || continue
        v=${OP32["$k"]}
        for ((bit=0; bit<16; bit++)); do
            if (( (v >> (bit+16)) & 1 )); then
                HIST["$b|$bit"]=$(( ${HIST["$b|$bit"]} + 1 ))
            fi
        done
    done
done

# Print one backend's histogram
echo "  aarch64 hist: ${HIST[aarch64|0]} ${HIST[aarch64|1]} ... ${HIST[aarch64|15]}"

echo ""
echo "=== 5. Backend pairwise matmul (dot product of histograms) ==="
echo "        $(printf '%-10s' '') $(for b in $BACKENDS; do printf '%-10s' "$b"; done)"
printf '        '
for b in $BACKENDS; do printf '%-10s' "$b"; done
echo ""
for b1 in $BACKENDS; do
    printf '%-8s' "$b1"
    for b2 in $BACKENDS; do
        # Matmul: dot product of hist vectors = sum over bit positions of (hist[b1,bit] * hist[b2,bit])
        dot=0
        for ((bit=0; bit<16; bit++)); do
            dot=$(( dot + ${HIST["$b1|$bit"]} * ${HIST["$b2|$bit"]} ))
        done
        printf '%-10s' "$dot"
    done
    echo ""
done

echo ""
echo "=== 6. Greedy similarity clustering across backends ==="
# Cluster instructions that share opcode patterns.
# Strategy: hash = ((op32 >> 8) & 0xFFFFFF) — keeps top 24 bits, which is the encoding fingerprint.
# Cluster: group by fingerprint.
# Output: clusters with members across backends.

CLUSTERS=$(mktemp)
for k in "${!OP32[@]}"; do
    v=${OP32["$k"]}
    fp=$(( (v >> 4) & 0xFFFFFF ))  # fingerprint: bits 4..27 (24 bits)
    echo "$fp $k" >> "$CLUSTERS"
done

sort -k1,1 -n "$CLUSTERS" > "${CLUSTERS}.sorted"

# Aggregate clusters: count members per fingerprint
declare -A CLUSTER_COUNT
declare -A CLUSTER_FIRST
while read -r fp rest; do
    CLUSTER_COUNT[$fp]=$(( ${CLUSTER_COUNT[$fp]:-0} + 1 ))
    [[ -z "${CLUSTER_FIRST[$fp]:-}" ]] && CLUSTER_FIRST[$fp]="$rest"
done < "${CLUSTERS}.sorted"

# Output: clusters with >1 member (cross-backend or repetition)
echo ""
echo "Top-20 largest clusters by member count (cross-backend opcode patterns):"
for fp in "${!CLUSTER_COUNT[@]}"; do
    c=${CLUSTER_COUNT[$fp]}
    f=${CLUSTER_FIRST[$fp]}
    b="${f%%|*}"
    inst="${f##*|}"
    v=${OP32["$f"]}
    printf '  cluster fingerprint=0x%06x members=%d first=%s:%s op32=0x%08x\n' "$fp" "$c" "$b" "$inst" "$v"
done | sort -k4 -t= -n -r | head -20

# Multi-backend clusters
echo ""
echo "Clusters spanning 4+ different backends:"
for fp in "${!CLUSTER_COUNT[@]}"; do
    c=${CLUSTER_COUNT[$fp]}
    (( c < 4 )) && continue
    backends=$(awk -v fp="$fp" -v cm="$CLUSTERS" 'BEGIN{} $1==fp {print $2}' "${CLUSTERS}.sorted" | sed 's/|.*//' | sort -u | wc -l)
    if (( backends >= 4 )); then
        printf '  fp=0x%06x members=%d backends=%d\n' "$fp" "$c" "$backends"
    fi
done | sort -k3 -t= -n -r | head -10

rm -f "$CLUSTERS" "${CLUSTERS}.sorted"