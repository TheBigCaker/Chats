#!/usr/bin/env bash
input=/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract/_riscv_td.txt
in_def=0
def_count=0
let_count=0
while IFS= read -r line || [[ -n "$line" ]]; do
    # skip blank
    [[ -z "$line" ]] && continue
    if [[ "$line" =~ ^def[[:space:]]+([^[:space:]:]+) ]]; then
        def_count=$(( def_count + 1 ))
        [[ "$def_count" -le 3 ]] && echo "DEF: ${BASH_REMATCH[1]}"
        in_def=1
    fi
    if [[ "$in_def" == "1" ]] && [[ "$line" =~ let[[:space:]]+Inst\{([0-9]+)-([0-9]+)\}[[:space:]]*=[[:space:]]*([^,;[:space:]]+) ]]; then
        let_count=$(( let_count + 1 ))
        [[ "$let_count" -le 3 ]] && echo "LET: hi=${BASH_REMATCH[1]} lo=${BASH_REMATCH[2]} val=${BASH_REMATCH[3]}"
    fi
done < "$input"
echo ""
echo "Total defs matched: $def_count"
echo "Total lets matched: $let_count"
