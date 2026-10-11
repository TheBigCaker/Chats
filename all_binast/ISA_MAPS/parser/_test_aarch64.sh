#!/usr/bin/env bash
source <(grep -E '^(parse_int|concat_tds|build_template_db|parse_template_chain_inline|apply_template_bits_inline|emit_def_row_inline|build_matrix_generic|emit_header) \(\)' /data/data/com.termux/files/home/ISA_MAPS/parser/expand_matrix.sh | sed 's/ () {/(){/')
echo "Functions loaded: $(type -t build_matrix_generic)"

EXTRACT_DIR=/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract
ISA_DIR=/data/data/com.termux/files/home/ISA_MAPS/llvm_isa_tables
MAT_DIR=/data/data/com.termux/files/home/ISA_MAPS/multiplexer/matrices

# Override paths to local
backend="AArch64"
concat_tds "$backend"
build_template_db "$backend"
echo "DB entries: $(wc -l < $EXTRACT_DIR/_aarch64_tmpl_bits.tsv)"

# time the matrix build
time build_matrix_generic "$backend" 2>&1
echo "AArch64 rows: $(($(wc -l < $MAT_DIR/aarch64.tsv) - 1))"
