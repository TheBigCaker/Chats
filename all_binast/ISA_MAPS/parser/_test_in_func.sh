#!/usr/bin/env bash
set -u

build_matrix_generic() {
    local backend="$1" max_bits="$2"
    local lc="${backend,,}"
    local mat="/data/data/com.termux/files/home/ISA_MAPS/multiplexer/_test_${lc}.tsv"
    printf 'INST' > "$mat"
    for ((j=0; j<64; j++)); do printf '\tB%d' "$j" >> "$mat"; done
    printf '\n' >> "$mat"

    local input="/data/data/com.termux/files/home/ISA_MAPS/multiplexer/_test_${lc}.td"
    : > "$input"
    local f
    for f in /data/data/com.termux/files/home/ISA_MAPS/llvm_isa_tables/RISCV/*.td; do
        cat "$f" >> "$input"
        printf '\n' >> "$input"
    done

    local in_def=0 def_name=""
    local -a acc
    while IFS= read -r line || [[ -n "$line" ]]; do
        if [[ "$line" =~ ^def[[:space:]]+([^[:space:]:]+) ]]; then
            if [[ "$in_def" == "1" ]]; then
                printf '%s' "$def_name" >> "$mat"
                local b
                for ((b=0; b<max_bits/8; b++)); do printf '\t%d' "${acc[$b]:-0}" >> "$mat"; done
                for ((b=max_bits/8; b<8; b++)); do printf '\t0' >> "$mat"; done
                printf '\n' >> "$mat"
            fi
            def_name="${BASH_REMATCH[1]}"
            in_def=1
            acc=()
            local i
            for ((i=0; i<8; i++)); do acc[$i]=0; done
            continue
        fi
        if [[ "$in_def" == "1" ]] && [[ "$line" =~ let[[:space:]]+Inst\{([0-9]+)-([0-9]+)\}[[:space:]]*=[[:space:]]*([^,;[:space:]]+) ]]; then
            local hi=${BASH_REMATCH[1]}
            local lo=${BASH_REMATCH[2]}
            local val=${BASH_REMATCH[3]}
            local v=0
            if [[ "$val" == 0b* ]]; then
                for ((k=2; k<${#val}; k++)); do
                    v=$(( v * 2 + (${val:k:1} == 1 ? 1 : 0) ))
                done
            elif [[ "$val" =~ ^[0-9]+$ ]]; then
                v=$(( val + 0 ))
            fi
            local width=$(( hi - lo + 1 ))
            local i pos byte_idx cur
            for ((i=0; i<width; i++)); do
                local bit=$(( (v >> i) & 1 ))
                if [[ $bit -eq 0 ]]; then continue; fi
                pos=$(( lo + i ))
                byte_idx=$(( pos / 8 ))
                bit_in_byte=$(( pos % 8 ))
                cur=${acc[$byte_idx]:-0}
                acc[$byte_idx]=$(( cur | (1 << bit_in_byte) ))
            done
        fi
    done < "$input"
    if [[ "$in_def" == "1" ]]; then
        printf '%s' "$def_name" >> "$mat"
        local b
        for ((b=0; b<max_bits/8; b++)); do printf '\t%d' "${acc[$b]:-0}" >> "$mat"; done
        for ((b=max_bits/8; b<8; b++)); do printf '\t0' >> "$mat"; done
        printf '\n' >> "$mat"
    fi
    echo "Rows for $backend: $(($(wc -l < "$mat") - 1))"
}

build_matrix_generic "RISCV" 32
