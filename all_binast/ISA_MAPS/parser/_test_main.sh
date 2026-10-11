#!/usr/bin/env bash
set -u
EXTRACT_DIR=/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract
MAT_DIR=/data/data/com.termux/files/home/ISA_MAPS/multiplexer/matrices

mat="$MAT_DIR/riscv.tsv"
printf 'INST' > "$mat"
for ((j=0; j<64; j++)); do printf '\tB%d' "$j" >> "$mat"; done
printf '\n' >> "$mat"

input="$EXTRACT_DIR/_riscv_td.txt"
in_def=0
def_name=""
acc=()
while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" =~ ^def[[:space:]]+([^[:space:]:]+) ]]; then
        if [[ "$in_def" == "1" ]]; then
            printf '%s' "$def_name" >> "$mat"
            for ((b=0; b<4; b++)); do printf '\t%d' "${acc[$b]:-0}" >> "$mat"; done
            for ((b=4; b<8; b++)); do printf '\t0' >> "$mat"; done
            printf '\n' >> "$mat"
        fi
        def_name="${BASH_REMATCH[1]}"
        in_def=1
        acc=(0 0 0 0 0 0 0 0)
    fi
    if [[ "$in_def" == "1" ]] && [[ "$line" =~ let[[:space:]]+Inst\{([0-9]+)-([0-9]+)\}[[:space:]]*=[[:space:]]*([^,;[:space:]]+) ]]; then
        hi=${BASH_REMATCH[1]}
        lo=${BASH_REMATCH[2]}
        val=${BASH_REMATCH[3]}
        # parse_int simplified
        v=0
        if [[ "$val" == 0b* ]]; then
            for ((i=2; i<${#val}; i++)); do
                v=$(( v * 2 + (${val:i:1} == 1 ? 1 : 0) ))
            done
        elif [[ "$val" =~ ^[0-9]+$ ]]; then
            v=$(( val + 0 ))
        fi
        width=$(( hi - lo + 1 ))
        for ((i=0; i<width; i++)); do
            bit=$(( (v >> i) & 1 ))
            [[ $bit -eq 0 ]] && continue
            pos=$(( lo + i ))
            byte_idx=$(( pos / 8 ))
            bit_in_byte=$(( pos % 8 ))
            cur=${acc[$byte_idx]:-0}
            acc[$byte_idx]=$(( cur | (1 << bit_in_byte) ))
        done
    fi
done < "$input"
# emit final
if [[ "$in_def" == "1" ]]; then
    printf '%s' "$def_name" >> "$mat"
    for ((b=0; b<4; b++)); do printf '\t%d' "${acc[$b]:-0}" >> "$mat"; done
    for ((b=4; b<8; b++)); do printf '\t0' >> "$mat"; done
    printf '\n' >> "$mat"
fi
echo "Rows emitted: $(($(wc -l < "$mat") - 1))"
