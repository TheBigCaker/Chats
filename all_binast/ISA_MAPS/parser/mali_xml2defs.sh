#!/usr/bin/env bash
# mali_xml2defs.sh
# Pure-bash parser for Mesa's Bifrost/Valhall ISA.xml files.
# Emits the same defs format as td2multiplexer.sh:
#   name|outs_csv|ins_csv|template|enc_csv
# Where template is the mask,exact,unit, and enc_csv carries the actual
# opcode bytes (exact value).
#
# Usage: mali_xml2defs.sh <mesa_dir> <extract_dir>

set -u
MESA_DIR="${1:-/data/data/com.termux/files/home/ISA_MAPS/mesa-fd}"
EXTRACT_DIR="${2:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract}"
mkdir -p "$EXTRACT_DIR"

# Bifrost <ins> has attributes: name, mask, exact, unit, last, dests
# Valhall  <ins> has attributes: name, title, dests, unused, last, unit
# Both have child <src start="N" mask="0x..."/>
#
# A single awk pass walks each XML file. It tracks the inside of <ins ...>...</ins>
# (handling nested <ins> in Valhall). For each leaf <ins>, extracts name, mask,
# exact, unit, and src children.

extract_mali() {
    local variant="$1"   # "Bifrost" or "Valhall"
    local out="$EXTRACT_DIR/mali_${variant,,}.defs"
    : > "$out"
    local xml
    if [[ "$variant" == "Bifrost" ]]; then
        xml="$MESA_DIR/src/panfrost/compiler/bifrost/bifrost/ISA.xml"
    else
        xml="$MESA_DIR/src/panfrost/compiler/bifrost/valhall/ISA.xml"
    fi
    [[ ! -f "$xml" ]] && { echo "Missing $xml" >&2; return 1; }

    awk -v VARIANT="$variant" -v OUT="$out" '
    function attr(s, name,    n, m, val, q) {
        # extract attr value (handles quoted or unquoted)
        n = index(s, name "=")
        if (n == 0) return ""
        val = substr(s, n + length(name) + 1)
        # skip leading " or whitespace
        sub(/^[ \t\n"]*/, "", val)
        if (substr(val, 1, 1) == "\"") {
            val = substr(val, 2)
            # strip everything from the FIRST quote onwards (val is now text inside quotes)
            q = index(val, "\"")
            if (q > 0) val = substr(val, 1, q - 1)
        } else {
            sub(/[ \t\n>].*/, "", val)
        }
        gsub(/"/, "", val)
        return val
    }
    function trim(s) { gsub(/[ \t\r\n]+/, " ", s); gsub(/^ +| +$/, "", s); return s }
    BEGIN { depth = 0; src_list = ""; ins_attrs = ""; last_name = "" }
    # Detect a new <ins ...>
    /<ins[ \t]/ {
        line = $0
        if (match(line, /<ins[^>]*>/)) {
            tag = substr(line, RSTART, RLENGTH)
            # check self-closing (last 2 chars == "/>")
            tail = substr(tag, length(tag)-1, 2)
            closing = (tail == "/>")
            name = attr(tag, "name")
            mask = attr(tag, "mask")
            exact = attr(tag, "exact")
            unit = attr(tag, "unit")
            last_a = attr(tag, "last")
            dests = attr(tag, "dests")
            if (closing) {
                if (name != "") emit_ins(name, mask, exact, unit, last_a, dests, "")
                next
            } else {
                stack[depth] = sprintf("%s|%s|%s|%s|%s|%s", name, mask, exact, unit, last_a, dests)
                src_stack[depth] = ""
                depth++
                next
            }
        }
    }
    # <src .../> child
    /<src[ \t]/ {
        if (depth > 0) {
            line = $0
            if (match(line, /<src[^>]*\/>/)) {
                tag = substr(line, RSTART, RLENGTH)
                start = attr(tag, "start")
                mask2 = attr(tag, "mask")
                # Register source — size comes from start field mask bits
                src_stack[depth-1] = src_stack[depth-1] " src" "," start "," mask2
            }
        }
        next
    }
    # </ins>
    /<\/ins>/ {
        if (depth > 0) {
            depth--
            parts = stack[depth]
            n = split(parts, p, "|")
            nm = p[1]; mk = p[2]; ex = p[3]; un = p[4]; la = p[5]; ds = p[6]
            sl = src_stack[depth]
            if (nm != "") emit_ins(nm, mk, ex, un, la, ds, sl)
            stack[depth] = ""; src_stack[depth] = ""
        }
        next
    }
    function emit_ins(nm, mk, ex, un, la, ds, sl,    outs_csv, ins_csv, tpl, encs) {
        # outs: count of dests (Valhall attribute)
        outs_csv = ""
        if (ds != "" && ds + 0 > 0) {
            for (i=0; i<ds+0; i++) outs_csv = outs_csv "R" ","
        } else {
            outs_csv = "R,"
        }
        sub(/,$/, "", outs_csv)
        # ins: from src list — register width = mask bits
        ins_csv = ""
        if (sl != "") {
            np = split(sl, arr, " src,")
            for (i=1; i<=np; i++) {
                tok = arr[i]
                if (tok == "") continue
                # tok looks like "start,mask"  (first element is just "start,mask" since " src" prefix stripped)
                # Determine register width from mask
                ma = tok
                sub(/^[0-9]+,/, "", ma)   # ma = mask (hex or dec)
                # mask like 0xfb → 8 bits = 8 entries, register width 8 bits per slot
                # default width = 32 (Bifrost/Valhall use 32-bit registers)
                ins_csv = ins_csv "R32" ","
            }
        }
        sub(/,$/, "", ins_csv)
        if (ins_csv == "") ins_csv = ""   # leave empty
        tpl = VARIANT "_" un
        encs = ""
        if (ex != "") encs = ex
        if (mk != "" && mk != "0") {
            if (encs != "") encs = encs "," mk
            else encs = mk
        }
        # Strip any embedded quotes from name (safety)
        gsub(/"/, "", nm)
        printf "%s|%s|%s|%s|%s\n", nm, outs_csv, ins_csv, tpl, encs >> OUT
    }
    ' "$xml"
}

extract_mali "Bifrost"
extract_mali "Valhall"

# Merge both into a single mali.defs for the multiplexer
cat "$EXTRACT_DIR/mali_bifrost.defs" "$EXTRACT_DIR/mali_valhall.defs" > "$EXTRACT_DIR/mali.defs"
echo "Bifrost: $(wc -l < "$EXTRACT_DIR/mali_bifrost.defs") entries"
echo "Valhall: $(wc -l < "$EXTRACT_DIR/mali_valhall.defs") entries"
echo "Merged:  $(wc -l < "$EXTRACT_DIR/mali.defs") entries"
