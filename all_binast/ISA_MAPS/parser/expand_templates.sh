#!/usr/bin/env bash
# expand_templates.sh
# Pure-bash template expander for LLVM .td files.
#
# Scans all .td files in a backend, extracts:
#   - `class TPL<args> : BaseClass<...>, (outs ...), (ins ...), ...`
#   - `multiclass TPL<args> { def X : ...; def Y : ...; ... }`
# For each def NAME : TPL<args>; usage, the template's (outs/ins) are propagated
# to the def entry.
#
# Re-emits updated <backend>.defs files in-place.

set -u
ISA_DIR="${1:-/data/data/com.termux/files/home/ISA_MAPS/llvm_isa_tables}"
EXTRACT_DIR="${2:-/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract}"

# Backends that need template expansion (use class/multiclass heavily)
EXPAND_BACKENDS=(AArch64 RISCV Mips Sparc AVR ARM PowerPC)

# Step 1: build template database — for each (template, arg_count), store the
# (outs_csv, ins_csv) signature by walking the template definition in the .td
# source. We do this in a single awk pass that emits:
#   template_name<TAB>arity<TAB>outs_csv<TAB>ins_csv
#
# Templates often span multiple lines, so first concatenate each file into one
# long line (replacing newlines with spaces) before parsing.

build_template_db() {
    local backend="$1"
    local db="$EXTRACT_DIR/${backend,,}.templates.tsv"
    : > "$db"
    find "$ISA_DIR/$backend" -name '*.td' -type f -print0 | \
    xargs -0 -I {} sh -c 'tr "\n" " " < "{}"; echo ""' | \
    awk '
    function extract_outs_ins(body,    outs, ins) {
        outs = ""; ins = ""
        if (match(body, /\([[:space:]]*outs[[:space:]]*[^)]*\)/)) {
            outs = substr(body, RSTART, RLENGTH)
            sub(/^\([[:space:]]*outs[[:space:]]*/, "", outs)
            sub(/\)$/, "", outs)
        }
        if (match(body, /\([[:space:]]*ins[[:space:]]*[^)]*\)/)) {
            ins = substr(body, RSTART, RLENGTH)
            sub(/^\([[:space:]]*ins[[:space:]]*/, "", ins)
            sub(/\)$/, "", ins)
        }
        return outs "|" ins
    }
    {
        s = $0
        i = 1
        while (i <= length(s)) {
            # find next "class " or "multiclass "
            ci = index(substr(s, i), "class ")
            mi = index(substr(s, i), "multiclass ")
            if (ci == 0 && mi == 0) break
            # pick whichever comes first (by absolute position)
            if (ci > 0 && (mi == 0 || ci < mi)) {
                start = i + ci - 1
                kind = "class"
                offset = 6   # length of "class "
            } else {
                start = i + mi - 1
                kind = "multiclass"
                offset = 10  # length of "multiclass "
            }
            # ensure preceded by start-of-string or whitespace
            if (start > 1) {
                prev = substr(s, start-1, 1)
                if (prev ~ /[A-Za-z0-9_]/) { i = start + 1; continue }
            }
            # parse template name
            j = start + offset
            while (j <= length(s) && substr(s, j, 1) ~ /[A-Za-z0-9_]/) j++
            nm = substr(s, start + offset, j - (start + offset))
            if (nm == "") { i = j; continue }
            # find matching < ... > (allow nested)
            ang = 0; ang_start = 0; ang_end = 0
            k = j
            while (k <= length(s)) {
                ch = substr(s, k, 1)
                if (ch == "<") {
                    if (ang == 0) ang_start = k + 1
                    ang++
                } else if (ch == ">") {
                    ang--
                    if (ang == 0) { ang_end = k - 1; break }
                }
                k++
            }
            if (ang_end == 0) { i = k; continue }
            ang_content = substr(s, ang_start, ang_end - ang_start + 1)
            # compute arity: commas at top level
            arity = 1; depth = 0
            for (p=1; p<=length(ang_content); p++) {
                ch = substr(ang_content, p, 1)
                if (ch == "<") depth++
                else if (ch == ">") depth--
                else if (ch == "," && depth == 0) arity++
            }
            # skip whitespace after > and look for either ":" (inheritance) or "{"
            k = ang_end + 2
            while (k <= length(s) && substr(s, k, 1) ~ /[ \t]/) k++
            inherit = ""
            if (k <= length(s) && substr(s, k, 1) == ":") {
                # read until matching { or ;
                brace_depth = 0; ang2 = 0
                istart = k + 1
                while (k <= length(s)) {
                    ch = substr(s, k, 1)
                    if (ch == "<") ang2++
                    else if (ch == ">") ang2--
                    else if (ch == "{") { if (ang2 == 0) break }
                    else if (ch == ";") break
                    k++
                }
                inherit = substr(s, istart, k - istart)
                # skip whitespace to {
                while (k <= length(s) && substr(s, k, 1) ~ /[ \t]/) k++
            }
            # find body after the header: find {
            if (k > length(s) || substr(s, k, 1) != "{") { i = k; continue }
            brace = 1; bstart = k + 1; bend = 0
            k++
            while (k <= length(s) && brace > 0) {
                ch = substr(s, k, 1)
                if (ch == "{") brace++
                else if (ch == "}") {
                    brace--
                    if (brace == 0) { bend = k - 1; break }
                }
                k++
            }
            if (bend == 0) { i = k; continue }
            body = substr(s, bstart, bend - bstart + 1)
            # combine inheritance + body for outs/ins search
            combined = inherit " " body
            oi = extract_outs_ins(combined)
            if (oi != "|" || kind == "class") {
                print nm "\t" arity "\t" oi
            }
            i = k
        }
    }
    ' > "$db" 2>/dev/null
    echo "  $backend template db: $(wc -l < "$db") entries"
}

# Step 2: For each def NAME : TPL<args>; in the defs file, look up TPL and
# propagate its outs/ins if our def has empty ones.

expand_defs() {
    local backend="$1"
    local defs="$EXTRACT_DIR/${backend,,}.defs"
    local db="$EXTRACT_DIR/${backend,,}.templates.tsv"
    [[ ! -s "$defs" ]] && return
    [[ ! -s "$db" ]] && return
    local tmp="$defs.expanded"
    awk -F'|' -v DB="$db" '
    BEGIN {
        # Load template DB: tpl_name -> "arity|outs|ins"
        while ((getline line < DB) > 0) {
            n = split(line, f, "\t")
            tname = f[1]; arity = f[2]; oi = f[3]
            split(oi, oip, "|")
            tpl_db[tname] = arity "\t" oip[1] "\t" oip[2]
        }
        close(DB)
    }
    {
        name = $1; outs = $2; ins = $3; template = $4; encs = $5
        if (template == "") { print; next }
        # if outs/ins already populated, keep
        if (outs != "" || ins != "") { print; next }
        # try to find template in DB
        if (template in tpl_db) {
            split(tpl_db[template], parts, "\t")
            ar = parts[1]; t_outs = parts[2]; t_ins = parts[3]
            if (t_outs != "") outs = t_outs
            if (t_ins != "") ins = t_ins
        }
        print name "|" outs "|" ins "|" template "|" encs
    }
    ' "$defs" > "$tmp"
    mv "$tmp" "$defs"
}

echo "=== Building template DBs ==="
for b in "${EXPAND_BACKENDS[@]}"; do
    build_template_db "$b"
done

echo ""
echo "=== Expanding defs ==="
for b in "${EXPAND_BACKENDS[@]}"; do
    before=$(wc -l < "$EXTRACT_DIR/${b,,}.defs" 2>/dev/null || echo 0)
    expand_defs "$b"
    after=$(wc -l < "$EXTRACT_DIR/${b,,}.defs" 2>/dev/null || echo 0)
    nonempty=$(awk -F'|' 'NF>=4 && ($2!="" || $3!="")' "$EXTRACT_DIR/${b,,}.defs" 2>/dev/null | wc -l)
    printf "  %-10s  total=%5d  with_operands=%5d\n" "$b" "$after" "$nonempty"
done
