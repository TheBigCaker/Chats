# Where this plugs into cygemm

The probe is done in pure C11 with **no libm** (all `log2` via `ccp_logf` from
`cygemm/snumblits.h`), no zlib, no numpy, no torch, no Python. The next stage --
if this goes past a spike -- is inference, and that means cygemm modules, not a
new stack. Mapping, by what each piece actually needs:

| stage | what it needs | existing piece to use |
|---|---|---|
| corpus prep | `.text` extraction, dedup by word, byte-position aware | `binast tokens` (this repo) -> feed `cygemm/universal_tokenizer` / `tools/training/tokenize_corpus.dry` |
| model math | GEMM, RMSNorm, softmax kernels in pure C | `cygemm/engine.c`, `kernel.dry` (`smmb_gemm`, `smmb_gemm_add`, `smmb_outer*`), `cygemm/rmsnorm.c` |
| inference runtime | forward pass, layer loop | `cygemm/runtime.dry`, `cygemm/dispatch.c` (`matmul_f32`, `matmul_m1_f32` entry points) |
| backend dispatch | pick cpu/gpu/npu/pim without a Python dispatcher | `tools/training/tricore_pim_dispatcher.dry` is a `.dry` origin -- it still carries Python ABI traces (`_normalize_backend` string logic, `cdef object` backends). The pure-C form of that decision is a small `arbit`-side selector; the backends themselves (`vpim`, `vrdma`, `igpu`, `npu`) each need their own pure-C path first. |
| low-level mem/atomics | arena, copy, futex-based threading | `cygemm/arbit` (arena/allocator), `cygemm/partitricks.h` (`pt_read`, `ccp_clone_run`, `ccp_futex_wait`) -- already pure C, raw syscalls, no pthread |
| crypto/determinism | content hashing, reproducible corpus sharding | `cygemm/cryptids` |
| parallel split | work partitioning across shards/threads | `cygemm/partitricks.h` + `smmb/shared_pipeline.h` (`PipelineLayer`, `smmb_pipeline_forward`) |

Notes that matter for *this* tool's target (a model over instruction words):

- The thing to predict is the positioned 4-byte word stream, not bytes. Measured:
  a 2-byte context horizon is useless (32.6 bits/word, worse than raw); the win
  starts once context covers a whole word (17.4 bits/word held out).
- Vocabulary is large and long-tailed: 419,628 distinct words in 3.7M words of
  train `.text`, 87.4% of held-out words seen. That is a tokenizer problem, not a
  byte-model problem -- `universal_tokenizer` is the right place to solve it, and
  a word-level (not byte-level) model over that vocab is the smaller job.
- Nothing here needs numpy or torch. The probe already runs a 3.7M-word corpus
  with a hand-rolled context model in 13 seconds of pure C.

## Honest scope statement

This repo is a spike plus measurements: a working ELF reader/patcher (pure C,
verified lossless round-trip on a 41k-token real binary), a formatting-vs-binary
canonicality proof, and a held-out learnability probe. It is **not** a model
training pipeline, and it does not touch drygon.
