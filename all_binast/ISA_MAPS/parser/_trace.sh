#!/usr/bin/env bash
input=/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract/_riscv_td.txt
: > "$input"
for f in /data/data/com.termux/files/home/ISA_MAPS/llvm_isa_tables/RISCV/*.td; do
    cat "$f" >> "$input"
    printf '\n' >> "$input"
done
echo "Total lines: $(wc -l < "$input")"
echo "Lines starting with def: $(grep -c '^def ' "$input")"
echo "Sample def line:"
grep -m1 '^def ' "$input"
