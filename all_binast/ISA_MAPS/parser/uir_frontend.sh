#!/usr/bin/env bash
# uir_frontend.sh — Universal Intermediate Representation compiler frontend.
#
# Reads UIR source, looks up each instruction in the multiplexer INDEX,
# resolves to a backend-specific variant, emits the encoded bytes.
#
# UIR syntax (line-based):
#   # comments start with hash
#   .backend aarch64
#   .section text
#   ADDG r0, r1, 100
#   RET
#
# Backend names: x86 aarch64 arm riscv mips powerpc sparc avr nvptx amdgpu mali

set -u
MUX_DIR="${1:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer}"
MAT_DIR="${MUX_DIR}/matrices"

# Strip suffix on backend key (multiplexer uses "X86", "AArch64", "RISCV", etc.)
backend_key() {
    case "$1" in
        x86) echo X86 ;;
        aarch64) echo AArch64 ;;
        arm) echo ARM ;;
        riscv) echo RISCV ;;
        mips) echo Mips ;;
        powerpc) echo PowerPC ;;
        sparc) echo Sparc ;;
        avr) echo AVR ;;
        nvptx) echo NVPTX ;;
        amdgpu) echo AMDGPU ;;
        mali) echo Mali ;;
        *) echo "$1" ;;
    esac
}

declare -A OP32
declare -A OP32_LOADED

load_backend_matrix() {
    local lc="$1"
    [[ -n "${OP32_LOADED[$lc]:-}" ]] && return 0
    local f="$MAT_DIR/${lc}.tsv"
    [[ -f "$f" ]] || return 1
    while IFS=$'\t' read -r -a row; do
        [[ "${row[0]}" == "INST" ]] && continue
        local inst="${row[0]}"
        local v=$(( ${row[1]:-0} + (${row[2]:-0} << 8) + (${row[3]:-0} << 16) + (${row[4]:-0} << 24) ))
        OP32["$inst"]=$v
    done < "$f"
    OP32_LOADED[$lc]=1
    return 0
}

# Look up a category file. Return matching variants that have the requested backend.
find_variant() {
    local category="$1" backend="$2" nargs="$3"
    local f="$MUX_DIR/${category}.json"
    [[ -f "$f" ]] || return 1
    # Use grep/sed to find INDEX entries with the backend
    grep -oE '"[A-Za-z_][A-Za-z0-9_]*":[[:space:]]*\{[^}]*"'"$backend"'":[[:space:]]*"[^"]+"' "$f" | head -20
}

emit_for_backend() {
    local backend="$1" inst_raw="$2"
    # Split: first whitespace separates opcode from args
    inst_raw="${inst_raw#"${inst_raw%%[![:space:]]*}"}"
    inst_raw="${inst_raw%"${inst_raw##*[![:space:]]}"}"
    local opcode="${inst_raw%%[[:space:]]*}"
    local rest="${inst_raw#* }"
    rest="${rest// /}"
    rest="${rest%;}"
    local args=()
    if [[ -n "$rest" ]]; then
        IFS=',' read -r -a args <<< "$rest"
    fi
    local nargs=${#args[@]}

    # Look up the category JSON
    local json="$MUX_DIR/${opcode}.json"
    if [[ ! -f "$json" ]]; then
        local upper="${opcode^^}"
        json="$MUX_DIR/${upper}.json"
    fi
    if [[ ! -f "$json" ]]; then
        local base="${opcode%%_}"
        json="$MUX_DIR/${base}.json"
    fi
    if [[ ! -f "$json" ]]; then
        echo "ERR no category JSON for $opcode" >&2
        return 1
    fi

    # Find ALL matching variants from INDEX that use this backend, pick one with non-zero matrix bytes
    local key
    key=$(backend_key "$backend")
    load_backend_matrix "$backend" || true
    local variant=""
    local best_bytes=0
    while IFS= read -r match; do
        local v
        v=$(echo "$match" | sed -E 's/.*"([^"]+)":[[:space:]]*\{.*/\1/')
        [[ -z "$v" ]] && continue
        local vb="${OP32[$v]:-0}"
        # Prefer non-zero bytes; among those, the first one wins
        if [[ -z "$variant" ]]; then
            variant="$v"
            best_bytes=$vb
        elif (( vb > 0 && best_bytes == 0 )); then
            variant="$v"
            best_bytes=$vb
        fi
    done < <(grep -oE '"[A-Za-z_][A-Za-z0-9_]*":[[:space:]]*\{[[:space:]]*"'"$key"'":[[:space:]]*"[^"]+"' "$json")
    if [[ -z "$variant" ]]; then
        echo "ERR no variant for $opcode on $key" >&2
        return 1
    fi

    # Now look up the opcode bytes from the matrix
    load_backend_matrix "$backend" || true
    local bytes="${OP32[$variant]:-0}"

    # If matrix has 0, but the JSON has OUTPUT_BYTES with literal bytes, parse those
    if (( bytes == 0 )); then
        local ob
        ob=$(grep -A3 "\"OUTPUT_BYTES\"" "$json" | grep -A2 "\"$variant\"" | tail -1 | grep -oE '"[A-Za-z_0-9]+|"[0-9]+"|"0x[0-9a-fA-F]+"' | tr -d '"' | tr '\n' ' ')
        for tok in $ob; do
            if [[ "$tok" =~ ^0x ]]; then
                bytes=$(( bytes | (16#$tok) << 24 ))
            elif [[ "$tok" =~ ^[0-9]+$ ]] && (( tok >= 0 && tok < 256 )); then
                bytes=$(( bytes | (tok << 24) ))
            fi
        done
    fi

    local i=0
    local pos=0
    for arg in "${args[@]}"; do
        arg="${arg// /}"
        arg="${arg#\$}"
        arg="${arg#x}"
        arg="${arg#X}"
        if [[ "$arg" =~ ^[0-9]+$ ]] && (( arg < 32 )); then
            local byte_idx=$(( pos / 8 ))
            local bit_off=$(( pos % 8 ))
            if (( byte_idx < 4 )); then
                bytes=$(( bytes | (arg << (byte_idx * 8 + bit_off)) ))
            fi
            pos=$(( pos + 5 ))
        fi
        i=$(( i + 1 ))
    done

    local b0=$(( bytes & 0xFF ))
    local b1=$(( (bytes >> 8) & 0xFF ))
    local b2=$(( (bytes >> 16) & 0xFF ))
    local b3=$(( (bytes >> 24) & 0xFF ))
    printf '%02x %02x %02x %02x' "$b0" "$b1" "$b2" "$b3"
    echo "  ; $opcode $variant"
}

# Main
backend="aarch64"
section="text"
declare -a OUTPUT_LINES

while IFS= read -r line; do
    # Strip comments
    line="${line%%#*}"
    # Trim
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" ]] && continue
    case "$line" in
        .backend\ *) backend="${line#*.backend }" ;;
        .section\ *) section="${line#*.section }" ;;
        .*)
            echo "WARN unknown directive: $line" >&2 ;;
        *)
            OUTPUT_LINES+=("$line")
            ;;
    esac
done

echo "; backend=$backend section=$section"
for line in "${OUTPUT_LINES[@]}"; do
    if [[ "$backend" == "auto" ]]; then
        # auto mode: try each backend in priority order
        for try_b in aarch64 arm riscv mips powerpc x86; do
            echo "  ; --- attempting $try_b ---"
            emit_for_backend "$try_b" "$line" && break
        done
    else
        emit_for_backend "$backend" "$line"
    fi
done