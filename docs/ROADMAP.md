# HyperPipe Roadmap

Priorities: correctness > simplicity > single-core speed > features.
Every item below is scoped so it can be benchmarked with
`scripts/bench_competitors.py` and gated by `make test` (11 suites) and
`make fuzz` before merge.

## Done

### v1.5 (this release)
- [x] SIMD NDJSON input: dispatchable span scanners + structural fast path
      (per-chunk quote bitmaps, zero-copy fields) - +17-44% on NDJSON-input
      workloads.
- [x] Hashed header lookup (`hp_keymap`): O(1) key resolution with one
      confirming memcmp, schema-growth aware.
- [x] `--schema types.csv`: declared int hints replace per-row number
      validation in filter predicates.
- [x] `--jobs N` parallel chunked pipeline: SIMD quote-aware planner
      (`plan_masks` kernels), per-chunk workers, ordered byte-identical
      reassembly, 64 MiB total output-buffer budget, CSV/TSV/NDJSON plain
      files, gzip fallback with a notice.
- [x] `--summary`: streaming per-column statistics, O(columns) memory.
- [x] Fuzzing scale-up: in-repo corpus (10 seeds), `make fuzz-long`,
      `make coverage`. The 200k-iteration release gate found and fixed the
      unterminated-quoted-EOF fidelity bug.

### v1.4
- [x] Fused SIMD JSON string emitter (scan+copy in one pass, vector stores,
      shared `hp_json_escape_one` expander) with the hybrid width policy
      (32B chunks below 192 bytes, 64B above) - +10.9% full-file NDJSON
      conversion vs v1.3, +6% on AVX2-only hosts.
- [x] Direct-to-page NDJSON serialization (no intermediate row-buffer copy).
- [x] Transparent gzip input (magic-byte sniff, runtime libz via dlopen,
      multi-member streams) and gzip output (`--gzip` / `.gz` suffix).
- [x] `--count` mode (~3,100 MiB/s on the reference workload).
- [x] In-tree fuzzing: chaos suite (400 seeded documents, wired into
      `make test`), byte-exact fidelity regressions, standalone ASan+UBSan
      mutation driver + libFuzzer entry point. Found and fixed two real
      fidelity bugs on its first day (typed-empty cells, ragged-row
      truncation).
- [x] RFC 8259-exact whitespace handling in the NDJSON parser.

### v1.3
- [x] Chunk-loop SIMD row parsers for AVX-512 **and** AVX2.
- [x] Zero-copy NDJSON output path; memoized JSON key fragments.
- [x] Reproducible competitive benchmark harness (kernel-accounted RSS,
      output-identity gates).
- [x] PGO investigation (measured, rejected for the default build,
      documented in docs/BENCHMARKS.md 2.1).

## Next (v1.6)

1. **zstd/brotli via the runtime-loading compression layer.** The dlopen
   pattern from gzip generalizes: magic-byte sniff, no hard dependency,
   graceful explicit error when the library is absent.
2. **NEON scan paths.** The chunked parsers and the plan_masks/struct_masks
   kernels are mechanical 128-bit twins; gated by differential tests on
   aarch64 (Apple Silicon / ARM servers), scalar fallback stays the
   correctness reference.
3. **Parallel `--summary`.** Fold per-worker accumulators into one table
   (min/max/mean/count merge exactly; type inference merges as `mixed` on
   disagreement). The CLI currently refuses the combination.
4. **Windows CI + release binaries.** The library is portable; only the
   fuzz/memstream tests need guards. Release artifacts: linux x86-64,
   windows x64, macOS arm64.
5. **OSS-Fuzz-style corpus growth.** Longer CI fuzz budgets, coverage
   report artifact per run.

## Later (v1.7+)

- Arrow IPC output as a build option.
- Optional s3/http input adapters (out of core scope; likely a companion
  tool, not a dependency).
- Parallel NDJSON schema-growth: today the parallel path locks the schema
  to the first line (documented); a bounded two-pass variant could follow
  growth like the single-stream path.
