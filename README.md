# HyperPipe

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C: C11](https://img.shields.io/badge/standard-C11-informational.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![SIMD: AVX-512 / AVX2](https://img.shields.io/badge/SIMD-AVX--512%20%7C%20AVX2-green.svg)](https://en.wikipedia.org/wiki/Advanced_Vector_Extensions)
[![Version: 1.4.0](https://img.shields.io/badge/version-1.4.0-orange.svg)](CHANGELOG.md)

HyperPipe is a zero-dependency streaming transcoder and in-flight filter for
structured line data (CSV, TSV, NDJSON) written in pure C11. It parses with
runtime-dispatched SIMD (AVX-512BW / AVX2 / scalar), keeps memory strictly
bounded regardless of input size, and starts in under a millisecond — built
for Unix pipes, not for loading tables.

```
cat access.log | hyperpipe "status == 500 or url contains /api" --select host,url,status
```

---

## Why HyperPipe?

| | `jq` | `xsv` / `qsv` | `mlr` (miller) | `duckdb` | **HyperPipe** |
|---|---|---|---|---|---|
| **Memory model** | Buffers whole input | Moderate | Moderate | Moderate–high | **Strictly bounded: ~0.8 MB own buffers, flat vs input size** |
| **Startup** | ~15 ms | ~10–50 ms | ~20 ms | ~50 ms | **< 1 ms (0.56 ms measured incl. a real 101-row run; duckdb 11.2 ms, qsv 4.0 ms, mlr 2.6 ms on the same box)** |
| **Binary size** | ~3.5 MB | ~8 MB | ~10 MB | ~60 MB | **< 60 KB, single file, no deps** |
| **CSV + NDJSON in one tool** | JSON only | CSV only | Yes | Yes (SQL) | **Yes, auto-sniffed** |
| **Natural filter expressions** | Full jq lang | No | Yes | SQL | **Yes: `and`/`or`, `contains`, `startswith`, `endswith`** |
| **Dirty-data resilience** | Aborts | Aborts | Aborts | Aborts | **`--on-error skip \| quarantine` with line resync** |
| **Quoted-field fidelity** | n/a | Yes | Yes | Yes | **Yes (RFC 4180 round-trip, v1.2)** |

The honest positioning: DuckDB will beat HyperPipe on multi-core analytic
queries — it is a database. qsv has a far larger feature set. HyperPipe
occupies the niche between `grep`/`awk` and those tools: **instant, tiny,
constant-memory filtering and conversion of huge streams**, where a 50 ms
startup or an O(N) load step is a dealbreaker.

---

## Key Capabilities

1. **Zero-friction CLI.** Input format is sniffed from the first bytes
   (CSV / TSV / NDJSON, BOM-aware); output format defaults to the "other"
   side (CSV→NDJSON, NDJSON→CSV). No flags needed for the common case.
2. **Natural query expressions.**
   ```bash
   hyperpipe access.log "status == 200 and bytes > 5000"
   hyperpipe access.log "status == 500 or status == 503" --select host,status
   hyperpipe access.log "url startswith /api and url endswith .json"
   hyperpipe access.log "request contains wp-login" --on-error skip
   ```
   `and` / `&&` conjoin, `or` / `||` start an alternative branch
   (`A and B or C` = `(A and B) or (C)`). Numeric literals compare
   numerically, quoted or string operators compare as text.
3. **Resilient streaming.** Malformed rows are resynchronized at line
   boundaries instead of killing an hours-long pipeline. Rows larger than
   the streaming buffer are handled too: skipped whole (quarantined
   byte-exact) or aborted loudly in the default `fail` mode.
4. **Type-faithful output (v1.2, hardened by fuzzing in v1.4).** Quoted CSV
   input keeps string identity (`"007"` stays `"007"`, unquoted `007` becomes
   a number; `"null"` stays a string, unquoted `null` becomes JSON null;
   an empty `""` cell survives as `""`, not as a blank). Unicode escapes,
   surrogate pairs and control characters survive NDJSON round-trips.
5. **Sparse-schema NDJSON.** Rows may reorder keys, omit keys or introduce
   new ones mid-stream. NDJSON output follows the grown schema; CSV/TSV
   output is locked to the first-row header (a streaming CSV header cannot
   be rewritten) and omits late columns. Ragged CSV rows keep their real
   width — no silent truncation.
6. **Transparent gzip (v1.4).** Feed `.csv.gz` / `.jsonl.gz` straight in —
   the magic bytes are detected, libz is located at runtime and the stream
   decompresses on the fly. `--gzip` (or a `.gz` output name) compresses on
   the way out. No hard dependency: without libz everything except gzip
   keeps working, and gzip fails with a clear message.
7. **`--count` (v1.4).** Streaming `grep -c` for structured data:
   `hyperpipe access.log "status == 500" --count` prints the number of
   matching rows (~3,100 MiB/s) without materializing output.
8. **Parallel pipeline (v1.5).** `--jobs N` splits a plain CSV/TSV/NDJSON
   file at quote-aware record boundaries (SIMD planner kernels) and runs
   the full parse→filter→serialize pipeline per chunk with ordered
   reassembly — byte-identical to the single-threaded stream, output
   buffering capped at 64 MiB total regardless of input size. Parallel
   NDJSON locks the schema to the first line (the same rule CSV output
   documents); gzip input falls back with a notice. On the 2-vCPU
   reference host: +7% (CSV filter) to +51% (NDJSON filter) at `--jobs 2`;
   the win grows with real core counts.
9. **`--summary` (v1.5).** Per-column statistics instead of row output:
   inferred type (int / number / string / mixed), present/empty/absent
   counts, numeric min/max/mean, lexicographic string bounds. Streaming and
   allocation-free — the accumulator is O(columns), not O(rows).
10. **`--schema` (v1.5).** Declared column types (`name,type` lines) turn
   numeric filter predicates into a fast int64 span parse — no per-row
   `strtod` — and document the data contract next to the pipeline.
11. **O(1) memory.** Own allocations are fixed (~1 MB: 512 KiB ring buffer,
   256 KiB output page buffer, 64 KiB parse arenas). Measured peak RSS is
   flat from 0.1 GB to 0.3+ GB inputs on the same host — while DuckDB reached
   197–370 MB and Miller 710–980 MB on the same jobs (see Benchmarks).

---

## Quick Start

```bash
# Build (Linux: gcc/clang; Windows: MinGW-w64)
make            # or: cmake -B build && cmake --build build

# Auto-sniff CSV, stream NDJSON
hyperpipe access.log

# Filter + project (natural expression, positional)
hyperpipe access.log "status == 200 and bytes > 5000" --select host,bytes

# Quarantine malformed rows into a side file, keep going
hyperpipe dirty.csv --on-error skip --quarantine bad_rows.log --to ndjson

# TSV in, CSV out, both auto-detected
cat metrics.tsv | hyperpipe --to csv > metrics.csv

# First 100 matching rows of a 50 GB dump — instant
hyperpipe huge.csv "status == 500" --head 100

# Count matching rows of a 100+ MiB file — no row output
hyperpipe access.log "status == 500" --count

# Gzip in and out — no flags needed for .gz input, .gz suffix for output
hyperpipe access.csv.gz "status == 200" --to ndjson -o filtered.jsonl.gz

# Explicit SIMD engine and stats
hyperpipe huge.csv --simd avx512 --stats > /dev/null

# Per-column statistics instead of rows (v1.5)
hyperpipe access.log --summary

# Parallel pipeline on a multi-core host (v1.5) - byte-identical output,
# CSV/TSV/NDJSON files; compressed input runs single-threaded
hyperpipe huge.csv "status == 200" --select host,bytes --jobs 8 -o out.ndjson
hyperpipe huge.jsonl --to csv --jobs 8 -o out.csv

# Declared column types: int columns skip per-row strtod in filters (v1.5)
hyperpipe events.csv "bytes > 5000.5" --schema types.csv
```

## Filter Expression Reference

| Expression | Meaning |
|---|---|
| `col == 200` / `col != 200` | equality (numeric literal → numeric compare) |
| `col > 10`, `>=`, `<`, `<=` | numeric / lexicographic comparison |
| `col contains "foo"` | substring match (text) |
| `col startswith /api` | prefix match (text, quotes optional) |
| `col endswith .json` | suffix match (text) |
| `A and B` / `A && B` | both must match |
| `A or B` / `A \|\| B` | at least one branch must match |
| `"5"` (quoted value) | forces string compare (`col == "007"` is text) |

Columns may be referenced by name or 1-based-ish index (`--select 0,3` accepts
numeric indices). Predicates are limited to 16 terms — enough for pipe
filtering; anything richer belongs in DuckDB.

---

## Performance

Same machine, same data, same logical workload — `scripts/bench_competitors.py`
spawns every tool with identical I/O (best of 3, output to `/dev/null`, warm
page cache, 102 MiB / 1.035M-row NASA-format log, 2-vCPU Xeon, output of
every tool verified for content identity):

**W1 — filter `status == 200` + project `host,status,bytes` → CSV**

| Tool | Throughput | Time | Peak RSS |
|---|---:|---:|---:|
| **HyperPipe 1.5 (AVX-512)** | **1,963 MiB/s** | **0.052 s** | **13 MB** |
| duckdb 1.4.2 (2 threads) | 429 MiB/s | 0.238 s | 181 MB |
| mawk 1.3.4 | 337 MiB/s | 0.304 s | ~2 MB |
| qsv 27.0.1 (search+select) | 162 MiB/s | 0.632 s | 23 MB (2 procs) |
| miller 6.13.0 | 53 MiB/s | 1.919 s | 745 MB |

**W2 — full conversion → NDJSON**

| Tool | Throughput | Time | Peak RSS |
|---|---:|---:|---:|
| **HyperPipe 1.5** | **616 MiB/s** | **0.163 s** | **13 MB** |
| duckdb 1.4.2 | 192 MiB/s | 0.532 s | 339 MB |
| qsv 27.0.1 (tojsonl) | 34 MiB/s | 2.987 s | n/a (pipe) |
| miller 6.13.0 | 34 MiB/s | 2.987 s | 831 MB |

HyperPipe rows are from the v1.5.0 release run; competitor rows are the
v1.4.0-run numbers in [docs/BENCHMARKS.md](docs/BENCHMARKS.md) 2.2 (same
host, same harness, same method).

**W3 — NDJSON input, 154 MiB / 1.035M rows (paired A/B vs a v1.4.0 rebuild)**

The v1.5 structural scanner reads each 64-byte chunk of a line once as a
quote bitmap and walks fields as span arithmetic; escape-free fields are
zero-copy and keys resolve through a hash index (v1.4 scanned every span
byte-by-byte). Escape-bearing lines fall back to the proven byte-span
path, so semantics are bit-identical (verified on full-file outputs,
v1.4 vs v1.5 byte-for-byte).

| Workload (NDJSON input) | v1.4.0 | v1.5.0 | Δ |
|---|---:|---:|---:|
| full → CSV | 421 MiB/s | **491 MiB/s** | **+17%** |
| filter+project → CSV | 512 MiB/s | **670 MiB/s** | **+31%** |
| full → NDJSON | 389 MiB/s | **512 MiB/s** | **+32%** |
| filter + `--count` | 614 MiB/s | **885 MiB/s** | **+44%** |

**`--jobs 2` on the same 2-vCPU host (byte-identical output, 64 MiB
output-buffer cap):**

| Workload | jobs=1 | jobs=2 | Δ |
|---|---:|---:|---:|
| CSV filter+project → CSV | 1,963 MiB/s | 2,110 MiB/s | +7% |
| CSV full → NDJSON | 625 MiB/s | 750 MiB/s | +20% |
| NDJSON full → CSV | 491 MiB/s | 662 MiB/s | +35% |
| NDJSON filter → CSV | 670 MiB/s | 1,012 MiB/s | +51% |

**Startup** (spawn + full run over a 101-row file): HyperPipe **0.56 ms**,
mlr 2.6 ms, qsv 4.0 ms, duckdb 11.2 ms.

Honest footnotes: on the raw byte-passthrough workload mawk statistically
ties HyperPipe (1,589 vs 1,551 MiB/s on the v1.4 run) — awk streams untouched
lines cheaply; the win over awk appears the moment you parse, filter, project
or convert. duckdb is multithreaded and wins on complex analytical SQL;
HyperPipe is a single-threaded stream tool. PGO was investigated and **not**
adopted (see [docs/BENCHMARKS.md](docs/BENCHMARKS.md) for the full experiment
log). Numbers vary by CPU and OS — run the harness on your own hardware
before quoting any multiplier.

## Memory Model

- Input: sliding ring buffer (default 512 KiB, `--buffer-size`), zero-copy
  field slices with SIMD-safe padding; optional gzip inflate staging
  (256 KiB, only when the input is actually gzip).
- Output: 256 KiB page buffer batching writes; with `--gzip` a deflate
  staging buffer of the same order. With `--jobs N` the non-head workers
  may buffer up to 64 MiB of serialized output in total while the
  coordinator drains in chunk order - the only allocation that is larger
  than the single-stream footprint, and it is capped regardless of input
  size or job count.
- Parse arena: 64 KiB per NDJSON line; quoted-field unescape arena: 64 KiB.
- Nothing scales with input size. Feed it 5 GB or 5 TB — the footprint chart
  is flat (verified by `test_real_dataset` and `/proc VmHWM` sampling).

## Building & Verification

```bash
make            # build/hyperpipe + libhyperpipe.a
make test       # 11 suites: RFC4180, ring buffer, differential, filter,
                # roundtrip, fault tolerance, real dataset, gzip, chaos fuzz,
                # summary, parallel pipeline
make fuzz       # 20k ASan+UBSan mutation iterations + in-repo corpus
make fuzz-long  # 200k iterations (FUZZ_K scales it)
make coverage   # gcov report over the full suite
```

The differential suite proves Scalar == AVX2 == AVX-512 bit-for-bit on every
fixture and on the full NASA-log dataset. The chaos fuzzer generates 400
seeded adversarial documents per run and pins the fidelity regressions it
has found (typed-empty cells, ragged rows) as byte-exact cases; the fuzz
harness runs the same invariants under sanitizers and compiles as a
libFuzzer target for OSS-Fuzz-style continuous fuzzing.

## Roadmap

The live, prioritized roadmap (including the v1.3 performance work that is
already merged: the chunk-loop SIMD row parsers for AVX-512 *and* AVX2, the
zero-copy JSON output path and the memoized key-fragment serializer) lives in
[docs/ROADMAP.md](docs/ROADMAP.md).

Short version:
- **done (v1.5)** — SIMD structural NDJSON scanner (+42% parse), zero-copy
  fields, hashed key lookup, `--jobs` parallel pipeline, `--summary`,
  `--schema` int hints, NDJSON roundtrip fuzzing;
- **done (v1.4)** — fused SIMD JSON emitter, direct-to-page serialization,
  transparent gzip in/out, `--count`, in-tree fuzzing;
- **next (v1.6)** — zstd/brotli via the runtime-loading compression layer,
  CI matrix (Linux x86-64/ARM, macOS, Windows), NEON scan paths, Arrow IPC
  output as a build option.

See [CHANGELOG.md](CHANGELOG.md) for the release notes.
