#!/usr/bin/env bash
# expand_matrix.sh — emit 64-byte wide opcode value matrices per backend.
# Uses POSIX awk (no strtonum, no gawk).
#
# Pre-processes .td files by joining continuation lines (those that don't end
# with ; or { or }), so each template header fits on one line.

set -u
ISA_DIR="${1:-/data/data/com.termux/files/home/ISA_MAPS/llvm_isa_tables}"
EXTRACT_DIR="${2:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract}"
OUT_DIR="${3:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer}"
MAT_DIR="$OUT_DIR/matrices"
mkdir -p "$MAT_DIR"

concat_tds() {
    local backend="$1"
    local out="$EXTRACT_DIR/_${backend,,}_td.txt"
    : > "$out"
    local f
    for f in "$ISA_DIR/$backend"/*.td; do
        [[ -f "$f" ]] || continue
        cat "$f" >> "$out"
        printf '\n' >> "$out"
    done
    # Step 1: drop comment lines and blank-only lines so they don't glue
    # the rest of the file together. A comment is anything from // to end.
    local tmp="$out.tmp1"
    awk '
    {
        s = $0
        if (s ~ /^[[:space:]]*\/\//) next
        if (s ~ /^[[:space:]]*$/) next
        sub(/[[:space:]]*\/\/.*$/, "", s)
        print s
    }
    ' "$out" > "$tmp"
    mv "$tmp" "$out"

    # Step 2: Join continuation lines: if a line doesn't end in ; { or }
    # (allowing trailing whitespace), append the next line.
    local tmp2="$out.tmp2"
    awk '
    {
        s = $0
        sub(/[ \t]+$/, "", s)
        if (length(prev) > 0) prev = prev " " s
        else prev = s
        if (s ~ /[;{}]$/) {
            print prev
            prev = ""
        }
    }
    END { if (length(prev) > 0) print prev }
    ' "$out" > "$tmp2"
    mv "$tmp2" "$out"
}

extract_template_bits() {
    local backend="$1"
    local input="$EXTRACT_DIR/_${backend,,}_td.txt"
    local db="$EXTRACT_DIR/_${backend,,}_tmpl_bits.tsv"
    : > "$db"
    awk -v DB="$db" '
    function flush_tpl(    line, i, k) {
        if (tpl_name == "") return
        line = tpl_name "\t" tpl_nparams
        for (i=1; i<=tpl_nparams; i++) line = line "\t" tpl_params[i]
        n_bases = 0; for (k in bases) n_bases++
        line = line "\t" n_bases
        for (k in bases) line = line "\t" bases[k]
        for (k in bits_map) line = line "\t" k "=" bits_map[k]
        print line >> DB
        tpl_name = ""; tpl_nparams = 0; delete tpl_params; delete bases; delete bits_map
    }
    # Scan entire line for `let Inst{HI-LO} = VAL` patterns, regardless of brace depth
    function extract_bits(s,    key, after, val, e, lpos, mend, mlen, mstr, ai, hi, lo, found) {
        lpos = 1
        while (lpos <= length(s)) {
            found = 0
            ai = index(substr(s, lpos), "let Inst{")
            if (ai == 0) break
            ai = ai + lpos - 1
            lpos = ai + 9
            # Now look for closing }
            cend = index(substr(s, lpos), "}")
            if (cend == 0) break
            cend = lpos + cend - 1
            inner = substr(s, lpos, cend - lpos)
            if (match(inner, /^([0-9]+)-([0-9]+)$/)) {
                key = inner
                after = substr(s, cend + 1)
                sub(/^[[:space:]]*=[[:space:]]*/, "", after)
                if (substr(after, 1, 1) == "{") {
                    # dag like {bits{5}, isSub} — extract as "bits<5>,isSub"
                    e = index(after, "}")
                    if (e > 0) {
                        val = substr(after, 2, e - 2)
                        gsub(/[[:space:]]/, "", val)
                        # Convert bits{N} to marker "bits<N>"
                        sub(/bits\{([0-9]+)\}/, "bits<\\1>", val)
                    } else val = ""
                } else {
                    # match identifier OR 0b... OR 0x... OR decimal
                    if (match(after, /^[[:space:]]*(0b[01]+|0x[0-9a-fA-F]+|[0-9]+|[A-Za-z_][A-Za-z0-9_.]*)/)) {
                        val = substr(after, RSTART, RLENGTH)
                        gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
                    } else val = ""
                }
                if (!(key in bits_map)) bits_map[key] = val
            }
            lpos = cend + 1
        }
    }
    BEGIN { in_tpl = 0; brace_depth = 0 }
    {
        s = $0
        if (!in_tpl && match(s, /^[[:space:]]*(class|multiclass)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/)) {
            flush_tpl()
            t = substr(s, RSTART, RLENGTH)
            sub(/^[[:space:]]*(class|multiclass)[[:space:]]+/, "", t)
            tpl_name = t
            tpl_nparams = 0
            in_tpl = 1
            brace_depth = 0
            idx_lt = index(s, "<")
            ang_depth = 1
            rest = substr(s, idx_lt + 1)
            param = ""
            while (ang_depth > 0 && length(rest) > 0) {
                ch = substr(rest, 1, 1)
                rest = substr(rest, 2)
                if (ch == "<") { ang_depth++; param = param "<" }
                else if (ch == ">") {
                    ang_depth--
                    if (ang_depth == 0) {
                        gsub(/^[[:space:]]+|[[:space:]]+$/, "", param)
                        if (param != "" && param !~ /^bits<[0-9]+>$/) {
                            # Strip type prefix: "bit ", "string ", "dag ", etc.
                            ptype = param
                            sub(/^(bit|string|dag|RegisterClass|Operand|SDPatternOperator|list<dag>|bits<[0-9]+>)[[:space:]]+/, "", ptype)
                            if (ptype == "") ptype = param
                            tpl_nparams++; tpl_params[tpl_nparams] = ptype
                        }
                        break
                    }
                    param = param ">"
                } else if (ch == "," && ang_depth == 1) {
                    gsub(/^[[:space:]]+|[[:space:]]+$/, "", param)
                    if (param != "" && param !~ /^bits<[0-9]+>$/) {
                        ptype = param
                        sub(/^(bit|string|dag|RegisterClass|Operand|SDPatternOperator|list<dag>|bits<[0-9]+>)[[:space:]]+/, "", ptype)
                        if (ptype == "") ptype = param
                        tpl_nparams++; tpl_params[tpl_nparams] = ptype
                    }
                    param = ""
                } else { param = param ch }
            }
            # inheritance: after >, look for : then Base<...>, Base2<...> then {
            while (length(rest) > 0 && substr(rest, 1, 1) ~ /[ \t\n]/) rest = substr(rest, 2)
            if (length(rest) > 0 && substr(rest, 1, 1) == ":") {
                rest = substr(rest, 2)
                while (length(rest) > 0) {
                    while (length(rest) > 0 && substr(rest, 1, 1) ~ /[ \t\n]/) rest = substr(rest, 2)
                    if (length(rest) == 0 || substr(rest, 1, 1) == "{") break
                    bn = ""
                    while (length(rest) > 0) {
                        c = substr(rest, 1, 1)
                        if (c ~ /[A-Za-z0-9_]/) { bn = bn c; rest = substr(rest, 2) }
                        else break
                    }
                    if (bn == "") break
                    bases[bn] = bn
                    if (length(rest) > 0 && substr(rest, 1, 1) == "<") {
                        ang = 1; rest = substr(rest, 2)
                        while (length(rest) > 0 && ang > 0) {
                            c = substr(rest, 1, 1); rest = substr(rest, 2)
                            if (c == "<") ang++
                            else if (c == ">") ang--
                        }
                    }
                    while (length(rest) > 0 && substr(rest, 1, 1) ~ /[ \t\n]/) rest = substr(rest, 2)
                    if (length(rest) > 0 && substr(rest, 1, 1) == ",") rest = substr(rest, 2)
                    else break
                }
            }
            # Now process rest of THIS line for brace counting + bit extraction
            for (i = 1; i <= length(rest); i++) {
                c = substr(rest, i, 1)
                if (c == "{") brace_depth++
                else if (c == "}") {
                    brace_depth--
                    if (brace_depth == 0) { flush_tpl(); in_tpl = 0; break }
                }
            }
            if (in_tpl) extract_bits(rest)
            next
        }
        if (in_tpl) {
            for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                if (c == "{") brace_depth++
                else if (c == "}") {
                    brace_depth--
                    if (brace_depth == 0) { flush_tpl(); in_tpl = 0; break }
                }
            }
            if (!in_tpl) next
            extract_bits(s)
        }
    }
    END { flush_tpl() }
    ' "$input"
}

build_matrix() {
    local backend="$1"
    local lc="${backend,,}"
    local mat="$MAT_DIR/${lc}.tsv"
    local db="$EXTRACT_DIR/_${lc}_tmpl_bits.tsv"
    local input="$EXTRACT_DIR/_${lc}_td.txt"

    printf 'INST' > "$mat"
    local j
    for ((j=0; j<64; j++)); do printf '\tB%d' "$j" >> "$mat"; done
    printf '\n' >> "$mat"

    awk -v DB="$db" -v MAT="$mat" '
    function parse_int(v,    i, c, n, out) {
        sub(/^[[:space:]]+|[[:space:]]+$/, "", v)
        if (v ~ /^0b/) {
            out = 0
            for (i=2; i<=length(v); i++) {
                c = substr(v, i, 1)
                out = out * 2 + (c == "1" ? 1 : 0)
            }
            return out
        }
        if (v ~ /^-?[0-9]+$/) return v + 0
        return 0
    }
    BEGIN {
        while ((getline line < DB) > 0) {
            n = split(line, f, "\t")
            nm = f[1]
            npar[nm] = f[2] + 0
            np_ = npar[nm]
            for (i=1; i<=np_; i++) par[nm, i] = f[2+i]
            nb_ = f[3+np_] + 0
            nbase[nm] = nb_
            for (i=1; i<=nb_; i++) base[nm, i] = f[3+np_+i]
            nbits[nm] = n - (3+np_+nb_)
            for (i=1; i<=nbits[nm]; i++) spec[nm, i] = f[3+np_+nb_+i]
        }
        close(DB)
    }
    function apply_tpl(nm, args_str, depth,    np_, nb_, ns_, i, j, p, sp, eq, d, hi, lo, val, v, w, pos, bi, bb_, bit) {
        if (depth > 4 || nm == "") return
        if ((nm, depth) in seen) return
        seen[nm, depth] = 1
        np_ = npar[nm] + 0
        nb_ = nbase[nm] + 0
        for (i=1; i<=nb_; i++) apply_tpl(base[nm, i], "", depth+1)
        ns_ = split(args_str, argv, ",")
        # Strip quotes from arg values
        for (p=1; p<=ns_; p++) {
            gsub(/^"|"$/, "", argv[p])
        }
        for (i=1; i<=nbits[nm]; i++) {
            sp = spec[nm, i]
            eq = index(sp, "="); if (eq == 0) continue
            d = index(sp, "-")
            if (d == 0 || d > eq) continue
            hi = substr(sp, 1, d-1) + 0
            lo = substr(sp, d+1, eq-d-1) + 0
            val = substr(sp, eq+1)
            # Handle dag form "bits<N>,name" — zero the lower bits, param value in upper
            if (match(val, /^bits<[0-9]+>(,|$)/)) {
                bw_pos = index(val, "<")
                bw_end = index(val, ">")
                bw = substr(val, bw_pos + 1, bw_end - bw_pos - 1) + 0
                tail = substr(val, bw_end + 1)
                if (match(tail, /^,([A-Za-z_][A-Za-z0-9_.]*)/)) {
                    pname = substr(tail, RSTART + 1, RLENGTH - 1)
                    pv = 0
                    for (p=1; p<=np_; p++) {
                        if (par[nm, p] == pname && p <= ns_) { pv = parse_int(argv[p]); break }
                    }
                    for (j=0; j<(hi-lo+1-bw); j++) {
                        bit = int(pv / 2^j) % 2
                        if (bit == 0) continue
                        pos = lo + bw + j
                        bi = int(pos / 8)
                        bb_ = pos % 8
                        if (bi >= 8) continue
                        acc[bi] = int(acc[bi]) + bit * 2^bb_
                    }
                    continue
                }
            }
            v = parse_int(val)
            if (v == 0 && val !~ /^0/ && val !~ /^0x/ && val !~ /^0b/ && val != "0") {
                for (p=1; p<=np_; p++) {
                    if (par[nm, p] == val && p <= ns_) { v = parse_int(argv[p]); break }
                }
            }
            w = hi - lo + 1
            for (j=0; j<w; j++) {
                bit = int(v / 2^j) % 2
                if (bit == 0) continue
                pos = lo + j
                bi = int(pos / 8)
                bb_ = pos % 8
                if (bi >= 8) continue
                acc[bi] = int(acc[bi]) + bit * 2^bb_
            }
        }
    }
    {
        s = $0
        if (s !~ /^[[:space:]]*def[[:space:]]+[A-Za-z_]/) next
        nm = s
        sub(/^[[:space:]]*def[[:space:]]+/, "", nm)
        if (!match(nm, /^[A-Za-z_][A-Za-z0-9_]*/)) next
        nm = substr(nm, RSTART, RLENGTH)
        for (k=0; k<8; k++) acc[k] = 0
        delete seen
        rest = s
        sub(/^[^:]*:/, "", rest)
        sub(/^[[:space:]]+/, "", rest)
        while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
            tn = substr(rest, RSTART, RLENGTH)
            sub(/[A-Za-z_][A-Za-z0-9_]*/, "", rest)
            sub(/^[[:space:]]+/, "", rest)
            targs = ""
            if (length(rest) > 0 && substr(rest, 1, 1) == "<") {
                ang = 1; rest = substr(rest, 2)
                targs = ""
                while (length(rest) > 0 && ang > 0) {
                    c = substr(rest, 1, 1); rest = substr(rest, 2)
                    if (c == "<") { ang++; targs = targs c }
                    else if (c == ">") { ang--; if (ang == 0) break; targs = targs c }
                    else targs = targs c
                }
            }
            apply_tpl(tn, targs, 1)
            sub(/^[[:space:]]*,[[:space:]]*/, "", rest)
        }
        line = nm
        for (k=0; k<4; k++) line = line "\t" acc[k]
        for (k=4; k<8; k++) line = line "\t0"
        print line >> MAT
    }
    ' "$input"
}

build_matrix_mali() {
    local mat="$MAT_DIR/mali.tsv"
    local defs="$EXTRACT_DIR/mali.defs"
    printf 'INST' > "$mat"
    local j
    for ((j=0; j<64; j++)); do printf '\tB%d' "$j" >> "$mat"; done
    printf '\n' >> "$mat"
    awk -F'|' '
    function hexdec(h,    i, v, c, d) {
        v = 0
        for (i=1; i<=length(h); i++) {
            c = substr(h, i, 1)
            if (c >= "0" && c <= "9") d = c - "0"
            else if (c >= "a" && c <= "f") d = 10 + c - "a"
            else if (c >= "A" && c <= "F") d = 10 + c - "A"
            else d = 0
            v = v * 16 + d
        }
        return v
    }
    function byte_at(v, n,    i, byte) {
        byte = 0
        for (i=0; i<8; i++) {
            if (int(v / 2^(n*8 + i)) % 2 == 1) byte = byte + 2^i
        }
        return byte
    }
    {
        name = $1; encs = $5
        v = 0
        if (encs != "") {
            n = split(encs, parts, ",")
            v = hexdec(parts[1])
        }
        line = name
        for (j=0; j<4; j++) line = line "\t" byte_at(v, j)
        for (j=4; j<8; j++) line = line "\t0"
        print line
    }
    ' "$defs" >> "$mat"
}

build_matrix_x86() {
    local mat="$MAT_DIR/x86.tsv"
    local defs="$EXTRACT_DIR/x86.defs"
    printf 'INST' > "$mat"
    local j
    for ((j=0; j<64; j++)); do printf '\tB%d' "$j" >> "$mat"; done
    printf '\n' >> "$mat"
    awk -F'|' '
    function hexdec(h,    i, v, c, d) {
        v = 0
        for (i=1; i<=length(h); i++) {
            c = substr(h, i, 1)
            if (c >= "0" && c <= "9") d = c - "0"
            else if (c >= "a" && c <= "f") d = 10 + c - "a"
            else if (c >= "A" && c <= "F") d = 10 + c - "A"
            else d = 0
            v = v * 16 + d
        }
        return v
    }
    function bintodec(b,    i, v, c) {
        v = 0
        for (i=1; i<=length(b); i++) {
            c = substr(b, i, 1)
            v = v * 2 + (c == "1" ? 1 : 0)
        }
        return v
    }
    function byte_at(v, n,    i, byte) {
        byte = 0
        for (i=0; i<8; i++) {
            if (int(v / 2^(n*8 + i)) % 2 == 1) byte = byte + 2^i
        }
        return byte
    }
    function hash_str(s,    i, h, c) {
        h = 0
        for (i=1; i<=length(s); i++) {
            c = substr(s, i, 1) + 0
            h = (h * 31 + c) - int((h * 31 + c) / 4294967296) * 4294967296
            if (h < 0) h = h + 4294967296
        }
        return h
    }
    {
        name = $1; enc1 = $5
        v = 0
        if (enc1 ~ /^0b/) v = bintodec(substr(enc1, 3))
        else if (enc1 ~ /^0x/) v = hexdec(substr(enc1, 3))
        else if (enc1 ~ /^[0-9]+$/) v = enc1 + 0
        h = hash_str(name)
        packed = v + h * 256
        line = name
        for (j=0; j<8; j++) line = line "\t" byte_at(packed, j)
        print line
    }
    ' "$defs" >> "$mat"
}

for b in AArch64 ARM RISCV Mips PowerPC Sparc AVR NVPTX AMDGPU; do
    echo "Backend: $b"
    concat_tds "$b"
    extract_template_bits "$b"
    build_matrix "$b"
done
build_matrix_mali
build_matrix_x86

echo ""
echo "=== Full-width matrices ==="
for f in "$MAT_DIR"/*.tsv; do
    rows=$(wc -l < "$f")
    printf '  %-30s rows=%d  cols=65\n' "$(basename "$f")" "$((rows - 1))"
done