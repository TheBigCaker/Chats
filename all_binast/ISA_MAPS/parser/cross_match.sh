#!/usr/bin/env bash
# cross_match.sh
# Cross-backend semantic matching for the multiplexer.
#
# For each (UIR semantic) op derived from a backend instruction, builds a
# signature = (op_class, num_outputs, num_inputs, output_bitwidth, input_bitwidths).
# Then groups instructions across all backends that share an identical signature
# into a single "shared function" row of the multiplexer.
#
# Output: multiplexer/uir_index.json  — { shared_name: { isa: opname, ... } }

set -u
EXTRACT_DIR="${1:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract}"
OUT_DIR="${2:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer}"

mkdir -p "$OUT_DIR"

# ============================================================================
# Section 1: build per-instruction signatures
# ============================================================================
# Read all <backend>.defs, compute (op_class, n_out, n_in, out_bw, in_bw_sorted)
# Emit TSV: backend<TAB>opname<TAB>sig

build_signatures() {
    local out="$EXTRACT_DIR/_signatures.tsv"
    : > "$out"
    local b lc defs
    for b in X86 AArch64 ARM RISCV Mips PowerPC Sparc AVR NVPTX AMDGPU Mali; do
        lc="${b,,}"
        defs="$EXTRACT_DIR/${lc}.defs"
        [[ ! -s "$defs" ]] && continue
        awk -F'|' -v B="$b" '
        function class_of(name,    base, i, ch, prefix, mc) {
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
            mc = prefix
            if (mc == "ADD" || mc == "IADD" || mc == "FADD" || mc == "FADDC" || mc == "IADDC" || mc == "ADDS") return "ADD"
            if (mc == "SUB" || mc == "SUBS" || mc == "FSUB" || mc == "ISUB") return "SUB"
            if (mc == "MUL" || mc == "MADD" || mc == "MADDS" || mc == "FMUL" || mc == "IMUL") return "MUL"
            if (mc == "DIV" || mc == "DIVS" || mc == "FDIV" || mc == "IDIV" || mc == "SDIV" || mc == "UDIV") return "DIV"
            if (mc == "AND" || mc == "BIC" || mc == "ANDS" || mc == "ANDI") return "AND"
            if (mc == "OR"  || mc == "ORR" || mc == "ORS"  || mc == "ORI")  return "OR"
            if (mc == "XOR" || mc == "EOR" || mc == "EORS" || mc == "XORS" || mc == "XORI") return "XOR"
            if (mc == "SHL" || mc == "LSL" || mc == "LSLS" || mc == "SLL") return "SHL"
            if (mc == "SHR" || mc == "LSR" || mc == "SRL" || mc == "ASR" || mc == "SRA") return "SHR"
            if (mc == "MOV" || mc == "MVN" || mc == "NEG" || mc == "FMOV" || mc == "MOVI" || mc == "MOVZ" || mc == "MOVN" || mc == "MOVK") return "MOV"
            if (mc == "LDR" || mc == "LD" || mc == "LOAD" || mc == "LB" || mc == "LH" || mc == "LW" ||
                mc == "LDUR" || mc == "LDX" || mc == "LDRB" || mc == "LDRH" || mc == "LDRW" ||
                mc == "LDRSB" || mc == "LDRSH" || mc == "LDRSW" || mc == "LDM" || mc == "LDNP" ||
                mc == "LDP" || mc == "LDXR" || mc == "LDAR" || mc == "LDAXR" || mc == "LDXP") return "LOAD"
            if (mc == "STR" || mc == "ST" || mc == "STORE" || mc == "SB" || mc == "SH" || mc == "SW" ||
                mc == "STUR" || mc == "STX" || mc == "STRB" || mc == "STRH" || mc == "STRW" ||
                mc == "STM" || mc == "STNP" || mc == "STP" || mc == "STXR" || mc == "STLR" ||
                mc == "STLXR" || mc == "STXP") return "STORE"
            if (mc == "B" || mc == "BNZ" || mc == "BEQ" || mc == "BNE" || mc == "BLT" || mc == "BGT" ||
                mc == "BLE" || mc == "BGE" || mc == "BCC" || mc == "BHI" || mc == "BLS" ||
                mc == "BLO" || mc == "BHS" || mc == "BCS" || mc == "BR" || mc == "JMP" || mc == "J" ||
                mc == "BLX" || mc == "BX" || mc == "CBZ" || mc == "CBNZ" || mc == "TBZ" || mc == "TBNZ" ||
                mc == "BT" || mc == "BF") return "BRANCH"
            if (mc == "CMP" || mc == "CMPS" || mc == "TST" || mc == "CMPI" || mc == "CMPZ" ||
                mc == "FCMP" || mc == "CMN") return "CMP"
            if (mc == "NOP") return "NOP"
            if (mc == "PUSH") return "PUSH"
            if (mc == "POP") return "POP"
            if (mc == "CALL" || mc == "BLR") return "CALL"
            if (mc == "SVC" || mc == "SYSCALL" || mc == "SVCALL" || mc == "SVC") return "SYSCALL"
            if (mc == "RET" || mc == "RETURN" || mc == "RETLM") return "BRANCH"
            if (mc == "FMA") return "MUL"
            if (mc == "FMIN" || mc == "FMAX") return "MOV"
            if (mc == "SQRT" || mc == "FSQRT") return "MUL"
            if (mc == "ABS" || mc == "FABS") return "MOV"
            if (mc == "EXT" || mc == "EXTR" || mc == "EXTEND" || mc == "SXTB" || mc == "UXTB" ||
                mc == "SXTH" || mc == "UXTH" || mc == "SXTW" || mc == "UXTW" ||
                mc == "SXT" || mc == "UXT") return "MOV"
            if (mc == "CSEL") return "MOV"
            if (mc == "CLZ" || mc == "CTZ" || mc == "POPCOUNT" || mc == "CNT") return "MOV"
            if (mc == "RBIT" || mc == "REV" || mc == "REV16" || mc == "REV32") return "MOV"
            if (mc == "FNEG" || mc == "INEG") return "MOV"
            if (mc == "FADDI" || mc == "SUBI") return "ADD"
            if (mc == "FMADD" || mc == "FMSUB" || mc == "FNMADD" || mc == "FNMSUB") return "MUL"
            # catch-all: return the prefix itself so even unknown ops get a sig
            if (length(mc) >= 2) return mc
            return ""
        }
        function reg_bw(rc,    n) {
            if (match(rc, /i([0-9]+)imm/, n)) return n[1]
            if (match(rc, /GR([0-9]+)/, n)) return n[1]
            if (match(rc, /GPR([0-9]+)/, n)) return n[1]
            if (match(rc, /s([0-9]+)imm/, n)) return n[1]
            if (match(rc, /u([0-9]+)imm/, n)) return n[1]
            if (match(rc, /R([0-9]+)/, n)) return n[1]
            if (rc == "GPR") return 64
            if (rc == "iPTR") return 64
            if (rc == "R")   return 32
            return 32   # default
        }
        function bw_list(csv,    arr, n, i, v, out) {
            out = ""
            n = split(csv, arr, ",")
            for (i=1; i<=n; i++) {
                v = arr[i]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                if (v == "") continue
                out = out "," reg_bw(v)
            }
            return out
        }
        function sort_bw(s,    arr, n, i, j, t, m) {
            # parse leading comma list, sort numerically, rebuild
            n = split(s, arr, ",")
            for (i=1; i<=n; i++) if (arr[i] == "" || arr[i] == "0") { arr[i] = arr[n]; n--; i-- }
            # bubble sort
            for (i=1; i<n; i++) for (j=i+1; j<=n; j++) if (arr[i]+0 > arr[j]+0) { t=arr[i]; arr[i]=arr[j]; arr[j]=t }
            m = ""
            for (i=1; i<=n; i++) m = m "," arr[i]
            return m
        }
        {
            name = $1; outs_csv = $2; ins_csv = $3; template = $4; encs = $5
            if (name == "") next
            cls = class_of(name)
            if (cls == "") next
            outs_bw = sort_bw(bw_list(outs_csv))
            ins_bw  = sort_bw(bw_list(ins_csv))
            n_out = split(outs_csv, a, ","); for (i=1; i<=n_out; i++) if (a[i] == "") n_out--
            n_in  = split(ins_csv,  a, ","); for (i=1; i<=n_in;  i++) if (a[i] == "") n_in--
            sig = cls "|" n_out "|" n_in "|" outs_bw "|" ins_bw
            print B "\t" name "\t" sig
        }
        ' "$defs" >> "$out"
    done
    echo "Signatures: $(wc -l < "$out") total"
}

# ============================================================================
# Section 2: group by signature, emit UIR index
# ============================================================================
build_uir_index() {
    local sigs="$EXTRACT_DIR/_signatures.tsv"
    local out="$OUT_DIR/uir_index.json"
    # Two-stage awk:
    #   pass1: count backends per signature
    #   pass2: emit JSON grouped by signature, sorted by coverage
    awk -F'\t' '
    BEGIN { FS="\t" }
    { count[$3]++ ; cov[$3][$1] = $2 }
    END {
        # build sorted list of signatures by coverage desc
        n = 0
        for (s in count) { n++; arr[n] = s; cov_arr[n] = count[s] }
        # simple sort by coverage
        for (i=1; i<=n; i++) for (j=i+1; j<=n; j++) if (cov_arr[i] < cov_arr[j]) {
            t = cov_arr[i]; cov_arr[i] = cov_arr[j]; cov_arr[j] = t
            t = arr[i]; arr[i] = arr[j]; arr[j] = t
        }
        printf "{\n" > "'"$out"'"
        printf "  \"SHARED_FUNCTIONS\": [\n" > "'"$out"'"
        first = 1
        for (k=1; k<=n; k++) {
            sig = arr[k]
            n_back = split(sig, p, "|")
            cls = p[1]; n_out = p[2]; n_in = p[3]; outs_bw = p[4]; ins_bw = p[5]
            # shared name: <CLASS>_<OUT_BW>_<IN_BW_SUM_or_count>
            # strip leading comma
            sub(/^,/, "", outs_bw); sub(/^,/, "", ins_bw)
            shared = cls "_o" n_out "_i" n_in
            if (outs_bw != "") shared = shared "_ow" outs_bw
            if (ins_bw  != "") shared = shared "_iw" ins_bw
            # collect backends
            sep = ""
            bs = ""
            for (b in cov[sig]) {
                bs = bs sep "\"" b "\": \"" cov[sig][b] "\""
                sep = ", "
            }
            if (first) first = 0
            else printf ",\n" > "'"$out"'"
            printf "    {\n" > "'"$out"'"
            printf "      \"name\": \"%s\",\n", shared > "'"$out"'"
            printf "      \"class\": \"%s\",\n", cls > "'"$out"'"
            printf "      \"n_out\": %d,\n", n_out > "'"$out"'"
            printf "      \"n_in\": %d,\n",  n_in  > "'"$out"'"
            printf "      \"outs_bw\": \"%s\",\n", outs_bw > "'"$out"'"
            printf "      \"ins_bw\":  \"%s\",\n", ins_bw  > "'"$out"'"
            printf "      \"coverage\": %d,\n", count[sig] > "'"$out"'"
            printf "      \"backends\": { %s }\n", bs > "'"$out"'"
            printf "    }" > "'"$out"'"
        }
        printf "\n  ],\n" > "'"$out"'"
        printf "  \"TOTAL_SHARED\": %d,\n", n > "'"$out"'"
        printf "  \"TOTAL_INSTRUCTIONS\": %d\n", NR > "'"$out"'"
        printf "}\n" > "'"$out"'"
    }
    ' "$sigs"
}

# ============================================================================
# Section 3: coverage report — which UIR ops are supported by which backends
# ============================================================================
coverage_report() {
    local out="$OUT_DIR/coverage.tsv"
    awk -F'\t' '
    BEGIN { FS="\t" }
    { backends[$3][$1] = 1; cls[$3] = $3 }
    END {
        n = 0
        for (s in backends) { n++ }
        printf "CLASS\tN_OUT\tN_IN\tOUTS_BW\tINS_BW\tCOVERAGE\tBACKENDS\n"
        for (s in cls) {
            split(s, p, "|")
            cls2 = p[1]; n_out = p[2]; n_in = p[3]; outs_bw = p[4]; ins_bw = p[5]
            sub(/^,/, "", outs_bw); sub(/^,/, "", ins_bw)
            c = 0; bs = ""
            for (b in backends[s]) { c++; bs = bs "," b }
            sub(/^,/, "", bs)
            printf "%s\t%d\t%d\t%s\t%s\t%d\t%s\n", cls2, n_out, n_in, outs_bw, ins_bw, c, bs
        }
    }
    ' "$EXTRACT_DIR/_signatures.tsv" | sort -k6 -nr > "$out"
    echo "Coverage rows: $(wc -l < "$out") (header + data)"
}

# ============================================================================
# Section 4: build per-category JSON files using ALL dynamic classes from
# coverage report (not the hardcoded 19)
# ============================================================================
emit_dynamic_json() {
    # We rebuild the category JSONs by re-running emit_json with a wider cat set.
    # Use awk to extract unique classes from coverage
    local cats=$(awk -F'\t' 'NR>1 { print $1 }' "$OUT_DIR/coverage.tsv" | sort -u | tr '\n' ' ')
    # Build awk program inline using the cats list
    local awk_script
    awk_script=$(cat <<'AWK_EOF'
BEGIN {
    FS = "\t"
    n_cats = split(CATS, cats, " ")
    for (i=1; i<=n_cats; i++) cat_set[cats[i]] = 1
    for (i=1; i<=n_cats; i++) {
        c = cats[i]
        idx_buf[c] = ""; ins_buf[c] = ""; out_buf[c] = ""
        iby_buf[c] = ""; oby_buf[c] = ""; bks_buf[c] = ""
    }
}
function class_of(name,    base, i, ch, prefix, mc) {
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
    mc = prefix
    if (mc == "ADD" || mc == "IADD" || mc == "FADD" || mc == "FADDC" || mc == "IADDC" || mc == "ADDS" || mc == "FADDI" || mc == "SUBI") return "ADD"
    if (mc == "SUB" || mc == "SUBS" || mc == "FSUB" || mc == "ISUB") return "SUB"
    if (mc == "MUL" || mc == "MADD" || mc == "MADDS" || mc == "FMUL" || mc == "IMUL" || mc == "FMA" ||
        mc == "SQRT" || mc == "FSQRT" || mc == "FMADD" || mc == "FMSUB" || mc == "FNMADD" || mc == "FNMSUB") return "MUL"
    if (mc == "DIV" || mc == "DIVS" || mc == "FDIV" || mc == "IDIV" || mc == "SDIV" || mc == "UDIV") return "DIV"
    if (mc == "AND" || mc == "BIC" || mc == "ANDS" || mc == "ANDI") return "AND"
    if (mc == "OR"  || mc == "ORR" || mc == "ORS"  || mc == "ORI")  return "OR"
    if (mc == "XOR" || mc == "EOR" || mc == "EORS" || mc == "XORS" || mc == "XORI") return "XOR"
    if (mc == "SHL" || mc == "LSL" || mc == "LSLS" || mc == "SLL") return "SHL"
    if (mc == "SHR" || mc == "LSR" || mc == "SRL" || mc == "ASR" || mc == "SRA") return "SHR"
    if (mc == "MOV" || mc == "MVN" || mc == "NEG" || mc == "FMOV" || mc == "MOVI" || mc == "MOVZ" ||
        mc == "MOVN" || mc == "MOVK" || mc == "FMIN" || mc == "FMAX" || mc == "ABS" || mc == "FABS" ||
        mc == "EXT" || mc == "EXTR" || mc == "SXTB" || mc == "UXTB" || mc == "SXTH" || mc == "UXTH" ||
        mc == "SXTW" || mc == "UXTW" || mc == "SXT" || mc == "UXT" || mc == "CSEL" || mc == "CLZ" ||
        mc == "CTZ" || mc == "POPCOUNT" || mc == "CNT" || mc == "RBIT" || mc == "REV" || mc == "REV16" ||
        mc == "REV32" || mc == "FNEG" || mc == "INEG") return "MOV"
    if (mc == "LDR" || mc == "LD" || mc == "LOAD" || mc == "LB" || mc == "LH" || mc == "LW" ||
        mc == "LDUR" || mc == "LDX" || mc == "LDRB" || mc == "LDRH" || mc == "LDRW" ||
        mc == "LDRSB" || mc == "LDRSH" || mc == "LDRSW" || mc == "LDM" || mc == "LDNP" ||
        mc == "LDP" || mc == "LDXR" || mc == "LDAR" || mc == "LDAXR" || mc == "LDXP") return "LOAD"
    if (mc == "STR" || mc == "ST" || mc == "STORE" || mc == "SB" || mc == "SH" || mc == "SW" ||
        mc == "STUR" || mc == "STX" || mc == "STRB" || mc == "STRH" || mc == "STRW" ||
        mc == "STM" || mc == "STNP" || mc == "STP" || mc == "STXR" || mc == "STLR" ||
        mc == "STLXR" || mc == "STXP") return "STORE"
    if (mc == "B" || mc == "BNZ" || mc == "BEQ" || mc == "BNE" || mc == "BLT" || mc == "BGT" ||
        mc == "BLE" || mc == "BGE" || mc == "BCC" || mc == "BHI" || mc == "BLS" ||
        mc == "BLO" || mc == "BHS" || mc == "BCS" || mc == "BR" || mc == "JMP" || mc == "J" ||
        mc == "BLX" || mc == "BX" || mc == "CBZ" || mc == "CBNZ" || mc == "TBZ" || mc == "TBNZ" ||
        mc == "BT" || mc == "BF" || mc == "RET" || mc == "RETURN" || mc == "RETLM") return "BRANCH"
    if (mc == "CMP" || mc == "CMPS" || mc == "TST" || mc == "CMPI" || mc == "CMPZ" || mc == "FCMP" || mc == "CMN") return "CMP"
    if (mc == "NOP") return "NOP"
    if (mc == "PUSH") return "PUSH"
    if (mc == "POP") return "POP"
    if (mc == "CALL" || mc == "BLR") return "CALL"
    if (mc == "SVC" || mc == "SYSCALL" || mc == "SVCALL") return "SYSCALL"
    if (length(mc) >= 2) return mc
    return ""
}
function regclass_size(rc,    n) {
    if (match(rc, /i([0-9]+)imm/, n)) return n[1]
    if (match(rc, /GR([0-9]+)/, n)) return n[1]
    if (match(rc, /GPR([0-9]+)/, n)) return n[1]
    if (match(rc, /s([0-9]+)imm/, n)) return n[1]
    if (match(rc, /u([0-9]+)imm/, n)) return n[1]
    if (match(rc, /R([0-9]+)/, n)) return n[1]
    if (rc == "GPR") return 64
    if (rc == "iPTR") return 64
    if (rc == "R")   return 32
    return 0
}
function build_op_array(csv,    arr, n, i, op, sz, out) {
    out = ""
    n = split(csv, arr, ",")
    for (i=1; i<=n; i++) {
        op = arr[i]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", op)
        if (op == "") continue
        sz = regclass_size(op)
        out = out sprintf("[\"%s\",%d],", op, sz)
    }
    sub(/,$/, "", out); return out
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
    cat = class_of(name)
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
    # Build the input TSV from all backends
    local input="$EXTRACT_DIR/_all.tsv"
    : > "$input"
    local b lc defs
    for b in X86 AArch64 ARM RISCV Mips PowerPC Sparc AVR NVPTX AMDGPU Mali; do
        lc="${b,,}"
        defs="$EXTRACT_DIR/${lc}.defs"
        [[ -s "$defs" ]] && awk -F'|' -v B="$b" '{ printf "%s\t%s\t%s\t%s\t%s\t%s\n", B, $1, $2, $3, $4, $5 }' "$defs" >> "$input"
    done
    cd "$OUT_DIR"
    awk -v CATS="$cats" -f <(echo "$awk_script") "$input"
    cd - >/dev/null
    rm -f "$input"
    echo "Dynamic JSONs emitted: $(ls "$OUT_DIR"/*.json 2>/dev/null | wc -l) files"
}

# ============================================================================
# Main
# ============================================================================

build_signatures
build_uir_index
coverage_report
emit_dynamic_json

echo ""
echo "=== Sample UIR shared functions (top coverage) ==="
head -20 "$OUT_DIR/coverage.tsv" | column -t -s $'\t' 2>/dev/null || head -20 "$OUT_DIR/coverage.tsv"
echo ""
echo "=== Full UIR index size ==="
ls -la "$OUT_DIR/uir_index.json"
echo ""
echo "DONE."
