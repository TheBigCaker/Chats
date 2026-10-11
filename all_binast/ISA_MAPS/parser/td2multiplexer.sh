#!/usr/bin/env bash
# td2multiplexer.sh
# Pure-bash LLVM .td parser → 5-table multiplexer.json
# Follows baccyn/CCP conventions: no python3, no external compilers.
#
# 5 tables emitted (one per semantic group):
#   INDEX       — {mnemonic: {isa: {opname}}}
#   INPUTS      — {mnemonic: {isa: [[regclass,size], ...]}}
#   OUTPUTS     — {mnemonic: {isa: [[regclass,size], ...]}}
#   INPUT_BYTES — {mnemonic: {isa: [encoding_bytes...]}}
#   OUTPUT_BYTES— {mnemonic: {isa: [encoding_bytes...]}}
#
# Usage: td2multiplexer.sh <llvm_isa_tables_dir> <out_dir>

set -u
# Note: `set -u` will catch undeclared vars in X86 extractor's depth counter;
# we initialize them all before the loop.
ISA_DIR="${1:-/data/data/com.termux/files/home/ISA_MAPS/llvm_isa_tables}"
OUT_DIR="${2:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer}"

mkdir -p "$OUT_DIR"

# Backends to process (canonical names match llvm_isa_tables/ subdirs)
BACKENDS=(X86 AArch64 ARM RISCV Mips PowerPC Sparc AVR NVPTX AMDGPU Mali)

# ============================================================================
# Section 1: helper functions
# ============================================================================

# Strip C++ comments and trailing whitespace, trim trailing commas
strip_comment() {
    local s="$1"
    s="${s%%//*}"
    s="${s%%/*}"
    printf '%s' "$s" | sed -E 's/[[:space:]]*$//'
}

# Extract integer from "0b01010" / "0x1F" / "42" / "(bits<5>)"
parse_int() {
    local s="$1"
    s="${s#(}"
    s="${s%)}"
    if [[ "$s" =~ 0b([01]+) ]]; then
        printf '%d' "2#${BASH_REMATCH[1]}"
    elif [[ "$s" =~ 0x([0-9a-fA-F]+) ]]; then
        printf '%d' "0x${BASH_REMATCH[1]}"
    else
        printf '%d' "$s" 2>/dev/null || printf '0'
    fi
}

# Find balanced paren substring containing substring "needle"
# Args: haystack needle  — echoes the matched "needle(...)" group on stdout
find_paren() {
    local haystack="$1" needle="$2"
    local pos rest depth out
    pos="${haystack%%${needle}*}"
    if [[ "$pos" == "$haystack" ]]; then
        return 1
    fi
    rest="${haystack#*${needle}}"
    # find first '('
    local idx=${#rest}
    # walk: must contain '('
    if [[ "$rest" != *"("* ]]; then
        return 1
    fi
    # build balanced substring starting at first (
    depth=0
    out=""
    local i ch
    for ((i=0; i<${#rest}; i++)); do
        ch="${rest:$i:1}"
        if [[ "$ch" == "(" ]]; then
            depth=$((depth+1))
            if [[ $depth -eq 1 ]]; then
                out=""
                continue
            fi
        elif [[ "$ch" == ")" ]]; then
            depth=$((depth-1))
            if [[ $depth -eq 0 ]]; then
                printf '%s' "$out"
                return 0
            fi
        fi
        out+="$ch"
    done
    return 1
}

# Extract outs/ins operand list as array of "RegClass" or "RegClass:$name" strings
# Args: outs_ins_text  — emits one operand per line
parse_operands() {
    local txt="$1"
    txt="${txt#(}"
    txt="${txt%)}"
    # split by comma at top level (no nested parens in typical operand syntax)
    # but operand names can contain ":" and "$". Use simple comma split.
    local IFS=','
    local parts=( $txt )
    local p
    for p in "${parts[@]}"; do
        # trim whitespace
        p="${p#"${p%%[![:space:]]*}"}"
        p="${p%"${p##*[![:space:]]}"}"
        # strip trailing "let..." blocks we already consumed
        p="${p%%[[:space:]]let*}"
        # extract register class (text before ":" or "$")
        local rc="${p%%:*}"
        rc="${rc%%\$*}"
        # also strip leading "ins"/"outs" tag if present
        rc="${rc##ins[[:space:]]}"
        rc="${rc##outs[[:space:]]}"
        rc="${rc##[[:space:]]}"
        rc="${rc%%[[:space:]]*}"
        if [[ -n "$rc" && "$rc" != ")" && "$rc" != "(" ]]; then
            printf '%s\n' "$rc"
        fi
    done
}

# Determine operand size from register class name (heuristic)
#  GR32 → 32, GR64 → 64, GPR32 → 32, GPR64 → 64, GPR → 64 (XLEN),
#  i32imm → 32, i64imm → 64, s16imm → 16
regclass_size() {
    local rc="$1"
    if [[ "$rc" =~ i([0-9]+)imm ]]; then
        printf '%d' "${BASH_REMATCH[1]}"
    elif [[ "$rc" =~ GR([0-9]+) ]]; then
        printf '%d' "${BASH_REMATCH[1]}"
    elif [[ "$rc" =~ GPR([0-9]+) ]]; then
        printf '%d' "${BASH_REMATCH[1]}"
    elif [[ "$rc" =~ s([0-9]+)imm ]]; then
        printf '%d' "${BASH_REMATCH[1]}"
    elif [[ "$rc" =~ u([0-9]+)imm ]]; then
        printf '%d' "${BASH_REMATCH[1]}"
    elif [[ "$rc" == "GPR" ]]; then
        printf '64'   # RISC-V: GPR means XLEN (64)
    elif [[ "$rc" == "iPTR" ]]; then
        printf '64'
    else
        printf '0'
    fi
}

# ============================================================================
# Section 2: per-backend extractors
# ============================================================================
# Each extractor writes ONE line per `def` it understands to its per-backend
# .defs file:
#   <defname>|<outs_csv>|<ins_csv>|<enc1>|<enc2>|...|<asm_or_empty>
# Encoding fields vary per backend.

EXTRACT_DIR="$OUT_DIR/.extract"
mkdir -p "$EXTRACT_DIR"

# ----- X86 ---------------------------------------------------------------
# Format: def NAME : I<opcode, format, (outs ...), (ins ...),
#                      "asm", args...>;
# Opcode may be a constant or Pseudo. Encoding bytes = first arg (or 0 if Pseudo).
#
# Strategy: run a single awk pass per file. Awk tracks when we're inside an
# `I<...>` block (which can span multiple lines), accumulates the buffer
# until the matching `>` is found, then emits a record. This avoids
# fork+exec overhead per def.
extract_x86() {
    local out="$EXTRACT_DIR/x86.defs"
    : > "$out"
    local f
    while IFS= read -r f; do
        awk -v OUT="$out" '
        BEGIN { in_def=0; in_ang=0; buf=""; name=""; }
        {
            line=$0
            # accumulate if we are mid-def
            if (in_def) {
                buf = buf " " line
                for (i=1; i<=length(line); i++) {
                    ch = substr(line, i, 1)
                    if (ch == "<") in_ang++
                    else if (ch == ">") {
                        in_ang--
                        if (in_ang == 0) {
                            # parse buf
                            i_args = buf
                            sub(/^[[:space:]]*def[[:space:]]+[A-Za-z0-9_]+[[:space:]]*:[[:space:]]*I</, "", i_args)
                            sub(/>.*$/, "", i_args)
                            # first comma-split token
                            enc1 = i_args
                            sub(/[[:space:]]*,.*$/, "", enc1)
                            sub(/\/\/.*$/, "", enc1)
                            gsub(/[[:space:]]/, "", enc1)
                            # extract outs/ins via simple regex
                            outs = ""; ins = ""
                            if (match(i_args, /\(outs[[:space:]]*[^)]*\)/)) {
                                outs = substr(i_args, RSTART, RLENGTH)
                                sub(/^\(outs[[:space:]]*/, "", outs)
                                sub(/\)$/, "", outs)
                            }
                            if (match(i_args, /\(ins[[:space:]]*[^)]*\)/)) {
                                ins = substr(i_args, RSTART, RLENGTH)
                                sub(/^\(ins[[:space:]]*/, "", ins)
                                sub(/\)$/, "", ins)
                            }
                            outs_csv = ""; ins_csv = ""
                            n = split(outs, a, ",")
                            for (k=1; k<=n; k++) {
                                v = a[k]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                                # strip "$name" suffix
                                sub(/[:$].*/, "", v)
                                if (v != "") outs_csv = outs_csv "," v
                            }
                            sub(/^,/, "", outs_csv)
                            n = split(ins, a, ",")
                            for (k=1; k<=n; k++) {
                                v = a[k]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                                sub(/[:$].*/, "", v)
                                if (v != "") ins_csv = ins_csv "," v
                            }
                            sub(/^,/, "", ins_csv)
                            if (enc1 != "" && enc1 != "Pseudo" && enc1 != "I") {
                                printf "%s|%s|%s|%s\n", name, outs_csv, ins_csv, enc1 >> OUT
                            }
                            in_def = 0; in_ang = 0; buf = ""; name = ""
                            break
                        }
                    }
                }
                next
            }
            # detect start of new def with I<
            if (match(line, /^def[[:space:]]+[A-Za-z0-9_]+[[:space:]]*:[[:space:]]*I</)) {
                nm = line; sub(/^def[[:space:]]+/, "", nm); sub(/[[:space:]]*:.*$/, "", nm)
                name = nm
                buf = line
                # count < and > in current line
                in_ang = 0
                for (i=1; i<=length(line); i++) {
                    ch = substr(line, i, 1)
                    if (ch == "<") in_ang++
                    else if (ch == ">") in_ang--
                }
                if (in_ang == 0) {
                    # self-contained on one line; parse immediately
                    i_args = line
                    sub(/^def[[:space:]]+[A-Za-z0-9_]+[[:space:]]*:[[:space:]]*I</, "", i_args)
                    sub(/>.*$/, "", i_args)
                    enc1 = i_args
                    sub(/[[:space:]]*,.*$/, "", enc1)
                    sub(/\/\/.*$/, "", enc1)
                    gsub(/[[:space:]]/, "", enc1)
                    outs = ""; ins = ""
                    if (match(i_args, /\(outs[[:space:]]*[^)]*\)/)) {
                        outs = substr(i_args, RSTART, RLENGTH)
                        sub(/^\(outs[[:space:]]*/, "", outs)
                        sub(/\)$/, "", outs)
                    }
                    if (match(i_args, /\(ins[[:space:]]*[^)]*\)/)) {
                        ins = substr(i_args, RSTART, RLENGTH)
                        sub(/^\(ins[[:space:]]*/, "", ins)
                        sub(/\)$/, "", ins)
                    }
                    outs_csv = ""; ins_csv = ""
                    n = split(outs, a, ",")
                    for (k=1; k<=n; k++) {
                        v = a[k]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                        sub(/[:$].*/, "", v)
                        if (v != "") outs_csv = outs_csv "," v
                    }
                    sub(/^,/, "", outs_csv)
                    n = split(ins, a, ",")
                    for (k=1; k<=n; k++) {
                        v = a[k]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                        sub(/[:$].*/, "", v)
                        if (v != "") ins_csv = ins_csv "," v
                    }
                    sub(/^,/, "", ins_csv)
                    if (enc1 != "" && enc1 != "Pseudo" && enc1 != "I") {
                        printf "%s|%s|%s|%s\n", name, outs_csv, ins_csv, enc1 >> OUT
                    }
                    buf = ""; name = ""
                    in_def = 0
                } else {
                    in_def = 1
                }
                next
            }
        }
        ' "$f"
    done < <(find "$ISA_DIR/X86" -name '*.td' -type f)
}

# ----- AArch64 -----------------------------------------------------------
# Same format as generic backend (def NAME : Template<args>;)
extract_aarch64() {
    extract_generic "AArch64"   # reuse generic awk extractor
}

# ----- Generic backend (RISCV, MIPS, PPC, Sparc, AVR, ARM) ---------------
# Many use single-line: def NAME : Class<args>;
# Or RISCV: def NAME : ALU_rr<bits, bits, "asm">, ...;
#
# Single awk pass per file. Tracks <...> depth across continuation lines.
extract_generic() {
    local backend="$1"
    local out="$EXTRACT_DIR/${backend,,}.defs"
    : > "$out"
    local f
    while IFS= read -r f; do
        awk -v OUT="$out" '
        BEGIN { in_def=0; in_ang=0; buf=""; }
        {
            line = $0
            if (in_def) {
                buf = buf " " line
                for (i=1; i<=length(line); i++) {
                    ch = substr(line, i, 1)
                    if (ch == "<") in_ang++
                    else if (ch == ">") {
                        in_ang--
                        if (in_ang == 0) { emit(); in_def=0; buf=""; break }
                    }
                }
                next
            }
            if (match(line, /^[[:space:]]*def[[:space:]]+[A-Za-z0-9_]+[[:space:]]*:[[:space:]]*[A-Za-z0-9_]+/)) {
                buf = line
                # count < and >
                in_ang = 0
                for (i=1; i<=length(line); i++) {
                    ch = substr(line, i, 1)
                    if (ch == "<") in_ang++
                    else if (ch == ">") in_ang--
                }
                if (in_ang == 0) { emit(); buf=""; in_def=0 }
                else { in_def = 1 }
                next
            }
        }
        function emit() {
            # parse buf
            s = buf
            sub(/^[[:space:]]*def[[:space:]]+/, "", s)
            # name = first identifier
            if (!match(s, /^[A-Za-z0-9_]+/)) return
            name = substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART+RLENGTH)
            sub(/^[[:space:]]*:[[:space:]]*/, "", s)
            # template = next identifier
            if (!match(s, /^[A-Za-z0-9_]+/)) return
            template = substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART+RLENGTH)
            # i_args = between first < and last >
            if (!match(s, /</)) return
            s = substr(s, RSTART+1)
            # find last > at depth 0
            depth = 0; end = 0
            for (i=1; i<=length(s); i++) {
                ch = substr(s, i, 1)
                if (ch == "<") depth++
                else if (ch == ">") { depth--; if (depth < 0) { end = i-1; break } }
            }
            if (end == 0) end = length(s)
            i_args = substr(s, 1, end)
            # outs/ins extraction
            outs = ""; ins = ""
            if (match(i_args, /\([[:space:]]*outs[[:space:]]*[^)]*\)/)) {
                outs = substr(i_args, RSTART, RLENGTH)
                sub(/^\([[:space:]]*outs[[:space:]]*/, "", outs)
                sub(/\)$/, "", outs)
            }
            if (match(i_args, /\([[:space:]]*ins[[:space:]]*[^)]*\)/)) {
                ins = substr(i_args, RSTART, RLENGTH)
                sub(/^\([[:space:]]*ins[[:space:]]*/, "", ins)
                sub(/\)$/, "", ins)
            }
            outs_csv = ""; ins_csv = ""; encs = ""
            n = split(outs, a, ",")
            for (k=1; k<=n; k++) {
                v = a[k]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                sub(/[:$].*/, "", v)
                if (v != "") outs_csv = outs_csv "," v
            }
            sub(/^,/, "", outs_csv)
            n = split(ins, a, ",")
            for (k=1; k<=n; k++) {
                v = a[k]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                sub(/[:$].*/, "", v)
                if (v != "") ins_csv = ins_csv "," v
            }
            sub(/^,/, "", ins_csv)
            # numeric encoding bits
            n = split(i_args, a, ",")
            for (k=1; k<=n; k++) {
                v = a[k]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                if (v ~ /^0b[01]+$/ || v ~ /^0x[0-9a-fA-F]+$/ || v ~ /^[0-9]+$/) {
                    encs = encs "," v
                }
            }
            sub(/^,/, "", encs)
            if (outs_csv != "" || ins_csv != "" || encs != "") {
                printf "%s|%s|%s|%s|%s\n", name, outs_csv, ins_csv, template, encs >> OUT
            }
        }
        ' "$f"
    done < <(find "$ISA_DIR/$backend" -name '*.td' -type f)
}

# ----- NVPTX, AMDGPU, Mali (different format) ----------------------------
# These are SIMT/PTX-style; def lines still match the same generic regex,
# so reuse generic extractor.
extract_gpu() {
    local backend="$1"
    extract_generic "$backend"
}

# ============================================================================
# Section 3: category mapping
# ============================================================================
# Map mnemonics to a small set of semantic categories.
category_of() {
    local name="$1"
    # strip suffixes like _rr, _ri, _rri, _Wrr, _Xrr, _MMR6, _DB, _ND, _LOCK, ...
    local base="$name"
    # remove trailing flags
    base="${base%_LOCK}"; base="${base%_LOCK_PREFIX}"
    base="${base%_DB}";   base="${base%_ND}"
    base="${base%_EVEX}"; base="${base%_AVX512}"
    base="${base%_MMR6}"; base="${base%_MM}"
    base="${base%_PSEUDO}"; base="${base%_PS}"
    base="${base%_F}"; base="${base%_D}"
    # split: keep only leading uppercase letters
    local m prefix=""
    local i ch
    for ((i=0; i<${#base}; i++)); do
        ch="${base:$i:1}"
        if [[ "$ch" =~ [A-Z] ]]; then
            prefix+="$ch"
        else
            break
        fi
    done
    m="$prefix"
    case "$m" in
        ADD) printf 'ADD' ;;
        SUB|SUBS) printf 'SUB' ;;
        MUL|MULS|MADD|MADDS) printf 'MUL' ;;
        DIV|DIVS) printf 'DIV' ;;
        AND|ANDs|ANDS|BIC) printf 'AND' ;;
        ORR|OR|ORS) printf 'OR' ;;
        EOR|XOR|EORS|XORS) printf 'XOR' ;;
        SHL|LSL|LSLS|SLL) printf 'SHL' ;;
        SHR|LSR|SRL|ASR|SRA) printf 'SHR' ;;
        MOV|MVN|NEG) printf 'MOV' ;;
        LDR|LD|LOAD|LB|LH|LW|LDUR|LDX|LDRB|LDRH|LDRW|LDRSB|LDRSH|LDRSW|LD*) printf 'LOAD' ;;
        STR|ST|STORE|SB|SH|SW|STUR|STX|STRB|STRH|STRW|ST*) printf 'STORE' ;;
        B|BNZ|BEQ|BNE|BLT|BGT|BLE|BGE|BCC|BHI|BLS|BLO|BHS|BCS|BCC|BR|J*) printf 'BRANCH' ;;
        CMP|CMPS|TST|CMPI|CMPZ) printf 'CMP' ;;
        NOP) printf 'NOP' ;;
        RET|RETURN|RETLM|BLR|BR) printf 'BRANCH' ;;
        PUSH) printf 'PUSH' ;;
        POP) printf 'POP' ;;
        CALL|BL|BLR) printf 'CALL' ;;
        SVC|SYSCALL|SVCALL) printf 'SYSCALL' ;;
        *) printf 'OTHER' ;;
    esac
}

# ============================================================================
# Section 4: emit multiplexer tables
# ============================================================================

emit_json() {
    # Read all backends' .defs in one awk pass, group by category, write
    # per-category JSON. Output is one file per category containing entries
    # from ALL backends (merged multiplexer table).
    local EXTRACT_DIR="$1"
    local OUT_DIR="$2"
    local cats=(ADD SUB MUL DIV AND OR XOR SHL SHR MOV LOAD STORE BRANCH CMP NOP PUSH POP CALL SYSCALL)
    local backends=(X86 AArch64 ARM RISCV Mips PowerPC Sparc AVR NVPTX AMDGPU)

    # Build a single awk program: input is "backend<TAB>name<TAB>outs<TAB>ins<TAB>enc1<TAB>enc_rest"
    local script
    script=$(cat <<'AWK_EOF'
BEGIN {
    FS = "\t"
    n_cats = split("ADD SUB MUL DIV AND OR XOR SHL SHR MOV LOAD STORE BRANCH CMP NOP PUSH POP CALL SYSCALL", cats, " ")
    for (i=1; i<=n_cats; i++) cat_set[cats[i]] = 1
    # init per-category buffers
    for (i=1; i<=n_cats; i++) {
        c = cats[i]
        idx_buf[c] = ""
        ins_buf[c] = ""
        out_buf[c] = ""
        iby_buf[c] = ""
        oby_buf[c] = ""
        bks_buf[c] = ""
    }
}
function category_of(name,    base, i, ch, prefix) {
    base = name
    sub(/_LOCK$/, "", base); sub(/_LOCK_PREFIX$/, "", base)
    sub(/_DB$/, "", base);    sub(/_ND$/, "", base)
    sub(/_EVEX$/, "", base);  sub(/_AVX512$/, "", base)
    sub(/_MMR6$/, "", base);  sub(/_MM$/, "", base)
    sub(/_PSEUDO$/, "", base); sub(/_PS$/, "", base)
    sub(/_F$/, "", base);     sub(/_D$/, "", base)
    prefix = ""
    for (i=1; i<=length(base); i++) {
        ch = substr(base, i, 1)
        if (ch ~ /[A-Z]/) prefix = prefix ch
        else break
    }
    if (prefix == "ADD") return "ADD"
    if (prefix == "SUB" || prefix == "SUBS") return "SUB"
    if (prefix == "MUL" || prefix == "MADD" || prefix == "MADDS") return "MUL"
    if (prefix == "DIV" || prefix == "DIVS") return "DIV"
    if (prefix == "AND" || prefix == "BIC" || prefix == "ANDS") return "AND"
    if (prefix == "OR" || prefix == "ORR" || prefix == "ORS") return "OR"
    if (prefix == "XOR" || prefix == "EOR" || prefix == "EORS" || prefix == "XORS") return "XOR"
    if (prefix == "SHL" || prefix == "LSL" || prefix == "LSLS" || prefix == "SLL") return "SHL"
    if (prefix == "SHR" || prefix == "LSR" || prefix == "SRL" || prefix == "ASR" || prefix == "SRA") return "SHR"
    if (prefix == "MOV" || prefix == "MVN" || prefix == "NEG") return "MOV"
    if (prefix == "LDR" || prefix == "LD" || prefix == "LOAD" || prefix == "LB" || prefix == "LH" || prefix == "LW" || prefix == "LDUR" || prefix == "LDX" || prefix == "LDRB" || prefix == "LDRH" || prefix == "LDRW" || prefix == "LDRSB" || prefix == "LDRSH" || prefix == "LDRSW") return "LOAD"
    if (prefix == "STR" || prefix == "ST" || prefix == "STORE" || prefix == "SB" || prefix == "SH" || prefix == "SW" || prefix == "STUR" || prefix == "STX" || prefix == "STRB" || prefix == "STRH" || prefix == "STRW") return "STORE"
    if (prefix == "B" || prefix == "BNZ" || prefix == "BEQ" || prefix == "BNE" || prefix == "BLT" || prefix == "BGT" || prefix == "BLE" || prefix == "BGE" || prefix == "BCC" || prefix == "BHI" || prefix == "BLS" || prefix == "BLO" || prefix == "BHS" || prefix == "BCS" || prefix == "BR" || prefix == "JMP" || prefix == "J") return "BRANCH"
    if (prefix == "CMP" || prefix == "CMPS" || prefix == "TST" || prefix == "CMPI" || prefix == "CMPZ") return "CMP"
    if (prefix == "NOP") return "NOP"
    if (prefix == "PUSH") return "PUSH"
    if (prefix == "POP") return "POP"
    if (prefix == "CALL" || prefix == "BLR") return "CALL"
    if (prefix == "SVC" || prefix == "SYSCALL" || prefix == "SVCALL") return "SYSCALL"
    if (prefix == "RET" || prefix == "RETURN" || prefix == "RETLM") return "BRANCH"
    return ""
}
function regclass_size(rc,    n) {
    if (match(rc, /i([0-9]+)imm/, n)) return n[1]
    if (match(rc, /GR([0-9]+)/, n)) return n[1]
    if (match(rc, /GPR([0-9]+)/, n)) return n[1]
    if (match(rc, /s([0-9]+)imm/, n)) return n[1]
    if (match(rc, /u([0-9]+)imm/, n)) return n[1]
    if (rc == "GPR") return 64
    if (rc == "iPTR") return 64
    return 0
}
function build_op_array(csv,    arr, n, i, op, sz, out) {
    out = ""
    n = split(csv, arr, ",")
    for (i=1; i<=n; i++) {
        op = arr[i]
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", op)
        if (op == "") continue
        sz = regclass_size(op)
        out = out sprintf("[\"%s\",%d],", op, sz)
    }
    sub(/,$/, "", out)
    return out
}
function build_enc_array(enc1, enc_rest,    out) {
    out = ""
    if (enc1 != "" && enc1 != "Pseudo") out = "\"" enc1 "\""
    if (enc_rest != "") {
        n = split(enc_rest, arr, ",")
        for (i=1; i<=n; i++) {
            e = arr[i]; gsub(/[[:space:]]/, "", e)
            if (e != "" && e != "Pseudo") out = out ",\"" e "\""
        }
    }
    return out
}
{
    backend = $1; name = $2; outs_csv = $3; ins_csv = $4; enc1 = $5; enc_rest = $6
    if (name == "") next
    cat = category_of(name)
    if (cat == "") next
    if (!(cat in cat_set)) next
    in_arr = build_op_array(ins_csv)
    out_arr = build_op_array(outs_csv)
    enc_arr = build_enc_array(enc1, enc_rest)
    sep = (idx_buf[cat] == "" ? "" : ",\n    ")
    idx_buf[cat] = idx_buf[cat] sep sprintf("\"%s\": {\"%s\": \"%s\"}", name, backend, name)
    ins_buf[cat] = ins_buf[cat] sep sprintf("\"%s\": {\"%s\": [%s]}", name, backend, in_arr)
    out_buf[cat] = out_buf[cat] sep sprintf("\"%s\": {\"%s\": [%s]}", name, backend, out_arr)
    iby_buf[cat] = iby_buf[cat] sep sprintf("\"%s\": {\"%s\": [%s]}", name, backend, enc_arr)
    oby_buf[cat] = oby_buf[cat] sep sprintf("\"%s\": {\"%s\": [%s]}", name, backend, enc_arr)
    # track unique backends per category
    if (bks_buf[cat] == "") bks_buf[cat] = "\"" backend "\""
    else if (index(bks_buf[cat], "\"" backend "\"") == 0) bks_buf[cat] = bks_buf[cat] ",\"" backend "\""
}
END {
    for (i=1; i<=n_cats; i++) {
        c = cats[i]
        outfile = c ".json"
        printf "{\n" > outfile
        printf "  \"CATEGORY\": \"%s\",\n", c > outfile
        printf "  \"BACKENDS\": [%s],\n", bks_buf[c] > outfile
        printf "  \"INDEX\": {\n    %s\n  },\n", idx_buf[c] > outfile
        printf "  \"INPUTS\": {\n    %s\n  },\n", ins_buf[c] > outfile
        printf "  \"OUTPUTS\": {\n    %s\n  },\n", out_buf[c] > outfile
        printf "  \"INPUT_BYTES\": {\n    %s\n  },\n", iby_buf[c] > outfile
        printf "  \"OUTPUT_BYTES\": {\n    %s\n  }\n", oby_buf[c] > outfile
        printf "}\n" > outfile
    }
}
AWK_EOF
)

    # Build TSV input: backend<TAB>name<TAB>outs<TAB>ins<TAB>enc1<TAB>enc_rest
    local input_file="$EXTRACT_DIR/_all.tsv"
    : > "$input_file"
    local b lc defs
    for b in "${backends[@]}"; do
        lc="${b,,}"
        defs="$EXTRACT_DIR/${lc}.defs"
        [[ ! -s "$defs" ]] && continue
        awk -F'|' -v B="$b" '{ printf "%s\t%s\t%s\t%s\t%s\t%s\n", B, $1, $2, $3, $4, $5 }' "$defs" >> "$input_file"
    done

    cd "$OUT_DIR"
    awk -f <(echo "$script") "$input_file"
    cd - >/dev/null
    rm -f "$input_file"
}

# ============================================================================
# Section 5: main
# ============================================================================

echo "=== td2multiplexer.sh starting ==="
echo "ISA_DIR=$ISA_DIR"
echo "OUT_DIR=$OUT_DIR"
echo "EXTRACT_DIR=$EXTRACT_DIR"

echo "[1/4] Extracting X86 ..."
extract_x86
echo "    $(wc -l < "$EXTRACT_DIR/x86.defs") entries"

echo "[2/4] Extracting AArch64 ..."
extract_aarch64
echo "    $(wc -l < "$EXTRACT_DIR/aarch64.defs") entries"

echo "[3/4] Extracting generic backends (RISCV, MIPS, PPC, Sparc, AVR, ARM) ..."
for b in RISCV Mips PowerPC Sparc AVR ARM; do
    extract_generic "$b"
    echo "    $b: $(wc -l < "$EXTRACT_DIR/${b,,}.defs") entries"
done

echo "[4/4] Extracting GPU backends (NVPTX, AMDGPU) ..."
for b in NVPTX AMDGPU; do
    extract_gpu "$b"
    echo "    $b: $(wc -l < "$EXTRACT_DIR/${b,,}.defs") entries"
done

echo "[4b/5] Expanding templates (class/multiclass propagation) ..."
bash /data/data/com.termux/files/home/ISA_MAPS/parser/expand_templates.sh \
    "$ISA_DIR" "$EXTRACT_DIR" 2>&1 | tail -8

echo "[5/5] Extracting Mali Bifrost/Valhall from Mesa XML ..."
bash /data/data/com.termux/files/home/ISA_MAPS/parser/mali_xml2defs.sh \
    /data/data/com.termux/files/home/ISA_MAPS/mesa-fd "$EXTRACT_DIR" >/dev/null
echo "    Mali: $(wc -l < "$EXTRACT_DIR/mali.defs") entries"

echo ""
echo "=== Emitting per-category JSON (merged across all backends) ==="
emit_json "$EXTRACT_DIR" "$OUT_DIR"

echo ""
echo "=== Emitting per-backend bytecode value matrices ==="
# Each matrix: rows = instructions in alphabetical order, cols = numeric
# encoding bytes (up to 16 columns). Saved as TSV for matmul input.
emit_matrices() {
    local MAT_DIR="$OUT_DIR/matrices"
    mkdir -p "$MAT_DIR"
    local b
    for b in "${BACKENDS[@]}"; do
        local lc="${b,,}"
        local defs="$EXTRACT_DIR/${lc}.defs"
        [[ ! -s "$defs" ]] && continue
        local mat="$MAT_DIR/${lc}.tsv"
        # Header: cols 0..15 = bytes
        printf 'INST\t' > "$mat"
        local j
        for ((j=0; j<16; j++)); do printf 'B%d\t' "$j" >> "$mat"; done
        printf '\n' >> "$mat"
        # For each instruction, encode the first 4 bytes as a row
        local line name outs_csv ins_csv enc1 enc_rest
        while IFS='|' read -r name outs_csv ins_csv enc1 enc_rest; do
            [[ -z "$name" ]] && continue
            [[ "$enc1" == "Pseudo" ]] && continue
            # Build 16-byte encoding: enc1..enc4 numeric, rest zeros
            printf '%s\t' "$name" >> "$mat"
            local bytes=()
            # Parse enc1
            if [[ "$enc1" =~ ^0b([01]+)$ ]]; then
                bytes+=( "$((2#${BASH_REMATCH[1]}))" )
            elif [[ "$enc1" =~ ^0x([0-9a-fA-F]+)$ ]]; then
                bytes+=( "$((16#${BASH_REMATCH[1]}))" )
            elif [[ "$enc1" =~ ^[0-9]+$ ]]; then
                bytes+=( "$enc1" )
            else
                bytes+=( "0" )
            fi
            # Parse additional enc_rest
            if [[ -n "$enc_rest" ]]; then
                local IFS=','
                local ee=( $enc_rest )
                local e
                for e in "${ee[@]}"; do
                    [[ ${#bytes[@]} -ge 16 ]] && break
                    if [[ "$e" =~ ^0b([01]+)$ ]]; then
                        bytes+=( "$((2#${BASH_REMATCH[1]}))" )
                    elif [[ "$e" =~ ^0x([0-9a-fA-F]+)$ ]]; then
                        bytes+=( "$((16#${BASH_REMATCH[1]}))" )
                    elif [[ "$e" =~ ^[0-9]+$ ]]; then
                        bytes+=( "$e" )
                    else
                        bytes+=( "0" )
                    fi
                done
            fi
            # Pad to 16 columns
            while [[ ${#bytes[@]} -lt 16 ]]; do
                bytes+=( "0" )
            done
            for ((j=0; j<16; j++)); do
                printf '%s\t' "${bytes[$j]}" >> "$mat"
            done
            printf '\n' >> "$mat"
        done < "$defs"
        local rows
        rows=$(wc -l < "$mat")
        echo "    $b: $((rows-1)) instructions → $mat"
    done
}
emit_matrices

echo ""
echo "=== Multiplexer outputs ==="
ls -la "$OUT_DIR"/*.json 2>/dev/null
echo ""
echo "=== Bytecode value matrices ==="
ls -la "$OUT_DIR/matrices/" 2>/dev/null
echo ""
echo "DONE."
