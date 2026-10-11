function flush_tpl(    line, i, k) {
    if (tpl_name == "") return
    line = tpl_name "\t" tpl_nparams
    for (i=1; i<=tpl_nparams; i++) line = line "\t" tpl_params[i]
    n_bases = 0; for (k in bases) n_bases++
    line = line "\t" n_bases
    for (k in bases) line = line "\t" bases[k]
    for (k in bits_map) line = line "\t" k "=" bits_map[k]
    print line
    tpl_name = ""; tpl_nparams = 0; delete tpl_params; delete bases; delete bits_map
}
function extract_bits(s,    key, after, val, e) {
    idx = 1
    while (match(substr(s, idx), /let[[:space:]]+Inst\{[0-9]+-[0-9]+\}/)) {
        tail = substr(s, idx + RSTART - 1)
        if (match(tail, /\{([0-9]+)-([0-9]+)\}/)) {
            key = substr(tail, RSTART + 1, RLENGTH - 2)
            after = substr(tail, RSTART + RLENGTH)
            sub(/^[[:space:]]*=[[:space:]]*/, "", after)
            if (substr(after, 1, 1) == "{") {
                e = index(after, "}")
                if (e > 0) { val = substr(after, 1, e); gsub(/[[:space:]]/, "", val) }
                else val = ""
            } else {
                if (match(after, /^[[:space:]]*[A-Za-z_][A-Za-z0-9_.]*/)) {
                    val = substr(after, RSTART, RLENGTH)
                    gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
                } else val = ""
            }
            if (!(key in bits_map)) bits_map[key] = val
        }
        idx = idx + RSTART + RLENGTH
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
                        tpl_nparams++; tpl_params[tpl_nparams] = param
                    }
                    break
                }
                param = param ">"
            } else if (ch == "," && ang_depth == 1) {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", param)
                if (param != "" && param !~ /^bits<[0-9]+>$/) {
                    tpl_nparams++; tpl_params[tpl_nparams] = param
                }
                param = ""
            } else { param = param ch }
        }
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
