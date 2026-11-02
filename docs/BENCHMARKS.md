# HyperPipe Benchmark & Datacenter Stress Scaling Report

## 0. Methodology & Reproducibility

- **Workload**: in-stream predicate filtering (`status == 200`) plus field
  projection (`host,status,bytes`), full file streamed from page cache,
  output to `/dev/null` (Linux) or `NUL` (Windows). Best of 3+ runs.
- **Throughput** counts raw input bytes (row rate counts data rows).
- **Peak RSS**: Linux numbers are `VmHWM` sampled from `/proc/<pid>/status`
  during the run; Windows numbers come from `GetProcessMemoryInfo`
  (`bench/bench_main.c`). Both include the process baseline, not just
  streaming buffers.
- **Output identity**: every kernel configuration is verified to produce
  byte-identical output (MD5 over the stream) before its timing is accepted.
- **Reproduce**: `python3 scripts/bench_competitors.py <data.csv>` (Linux
  harness with the duckdb/qsv/miller/mawk competitor jobs; kernel-accounted
  RSS via `os.wait4`) or `make` + `build/bench_hyperpipe`.
- **Honesty note**: awk/mawk/perl/python are text-tool baselines, not
  state-of-the-art CSV engines. They are included because they are what
  people actually pipe logs through today. For the heavy-artillery
  comparison (qsv, DuckDB, polars) see the positioning table in the README;
  those numbers are hardware- and version-sensitive, so we publish ours and
  let you run theirs on the same box.

## 1. Hardware Specifications

### Linux run (v1.2.0, this report section)
- **CPU**: 2-vCPU Intel Xeon-class sandbox, AVX-512F/BW/VL + BMI2
- **OS**: Linux x86_64 (Debian), gcc 14.2, `-std=c11 -O3 -Werror`
- **Dataset**: NASA-format access log, 1,035,000 rows / 102.3 MiB
- **Competitors on this host**: mawk 1.3.4 (20250131), Python 3.12.16

### Windows run (v1.1.0 report, kept for cross-platform reference)
- **Host CPU**: AMD Ryzen 5 7600X (Zen 4, 6C/12T, 4.7 GHz base / 5.3 GHz boost, 32 MB L3)
- **Motherboard**: MSI X870 GAMING PLUS WIFI (MS-7E47, AMD X870)
- **Memory**: 32 GB DDR5-6400 (2x16 Kingston FURY Beast, dual channel, ~102.4 GB/s theoretical)
- **Vector Extensions**: AVX-512F/BW/DQ/VL/CD/VPOPCNTDQ, AVX2, BMI2
- **OS**: Windows 11 Pro x86_64; **Compiler**: MinGW-W64 GCC 16.2.0 (`-O3 -march=native -flto`)
- **Dataset**: authentic NASA server access logs (same format), tiers below

---

## 2. Linux Competitive Results (v1.3.0, 1,035,000 rows / 102.2 MiB)

All tools spawned identically by `scripts/bench_competitors.py`
(`posix_spawn` + `os.wait4`), best of 3, warm page cache, output to
`/dev/null`. Dataset: NASA-format access log, 5 columns
(host,timestamp,request,status,bytes).

| Workload | Tool | Time (s) | MiB/s | Peak RSS (MiB) |
|---|---|---:|---:|---:|
| W1 filter+project→CSV | **HyperPipe 1.3 (auto=AVX-512)** | **0.0499** | **2,046** | **10.7** |
| W1 | HyperPipe (`--simd avx2`) | 0.0606 | 1,686 | 10.7 |
| W1 | duckdb 1.4.2 (2 threads) | 0.2414 | 423 | 197.0 |
| W1 | mawk 1.3.4 | 0.2999 | 341 | ~2 (own baseline) |
| W1 | qsv 27.0.1 (search+select, 2 procs) | 0.6322 | 162 | 23.4 |
| W1 | miller 6.13.0 | 1.9640 | 52 | 765.0 |
| W2 full→NDJSON | **HyperPipe 1.3** | **0.1962** | **521** | **10.7** |
| W2 | duckdb 1.4.2 | 0.5413 | 189 | 370.0 |
| W2 | qsv 27.0.1 (tojsonl) | 3.0111 | 34 | n/a (pipe) |
| W2 | miller 6.13.0 | 2.9680 | 34 | 978.8 |
| W3 parse+passthrough→CSV | mawk 1.3.4 | 0.0634 | 1,612 | ~2 |
| W3 | **HyperPipe 1.3** | 0.0641 | 1,595 | **10.7** |
| W3 | qsv 27.0.1 (cat rows) | 0.4070 | 251 | n/a (pipe) |
| W3 | duckdb 1.4.2 | 0.3791 | 270 | 260.2 |
| W3 | miller 6.13.0 | 1.3732 | 74 | 709.8 |

**v1.3 scaling notes (same host):**

| Path | v1.2.0 | v1.3.0 | Δ |
|---|---:|---:|---:|
| filter+project → CSV (AVX-512) | 1,633 MiB/s | 2,046 MiB/s | **+25%** |
| filter+project → CSV (AVX2 mode) | ~1,350 MiB/s* | 1,686 MiB/s | **+25%** |
| full → NDJSON | 259–297 MiB/s | 521 MiB/s | **+80–100%** |
| long rows (120 B–2 KB) → CSV | ~scalar fallback | 4,268 MiB/s | new ceiling |

Note: v1.2.0 had no AVX2 row parser — AVX2 hosts fell back to the scalar state
machine for row assembly; v1.3 adds `hp_csv_parse_row_avx2`.

**Startup** (posix_spawn → exit, 101-row real workload, median of 500):
HyperPipe 0.56 ms — mlr 2.6 ms — qsv 4.0 ms — duckdb 11.2 ms.

**Memory accounting honesty note.** On this sandbox a bare `int main(){}`
C binary reports 9.9 MiB `ru_maxrss` — that is the loader/libc baseline the
kernel charges to every process. HyperPipe's total 10.7 MiB therefore means
**~0.8 MiB of own allocations** (512 KiB ring buffer + 64 KiB output page
buffer + 64 KiB parse arenas), and it stays there from 0.1 GB to 0.3+ GB
inputs (341 MB mixed-row file → 10.8 MiB). The v1.2 report's "2.2 MB" figure
used the same kernel counter on an older sandbox and is kept below for
history; the *constant-vs-input-size* property is the invariant that matters,
and it holds on both hosts.

**Correctness gate.** Before any timing was accepted, `--simd scalar`,
`--simd avx2` and `--simd avx512` outputs were md5-compared over the full
file: byte-identical.

## 2.2 v1.4.0 release runs (same host, same dataset)

Competitive table (best of 3, output to /dev/null, identical I/O for every
tool; generated by `scripts/bench_competitors.py`):

| Workload | Tool | Time (s) | MiB/s | Peak RSS (MiB) |
|---|---|---:|---:|---:|
| W1 | HyperPipe 1.4 | 0.0519 | 1967 | 14.2 |
| W1 | duckdb 1.4.2 | 0.2383 | 429 | 180.9 |
| W1 | mawk 1.3.4 | 0.3036 | 337 | n/a (shell) |
| W1 | qsv 27.0.1 | 0.6319 | 162 | 23.3 |
| W1 | miller 6.13.0 | 1.9192 | 53 | 744.8 |
| W2 | HyperPipe 1.4 | 0.1793 | 570 | 14.2 |
| W2 | duckdb 1.4.2 | 0.5320 | 192 | 339.4 |
| W2 | qsv 27.0.1 | 2.9870 | 34 | n/a (shell) |
| W2 | miller 6.13.0 | 2.9868 | 34 | 830.7 |
| W3 | HyperPipe 1.4 | 0.0659 | 1551 | 14.2 |
| W3 | mawk 1.3.4 | 0.0643 | 1589 | n/a (shell) |
| W3 | duckdb 1.4.2 | 0.3608 | 283 | 268.4 |
| W3 | qsv 27.0.1 | 0.4072 | 251 | n/a (shell) |
| W3 | miller 6.13.0 | 1.3520 | 76 | 666.0 |

v1.4 feature throughput (same host):

| Path | Rate |
|---|---:|
| `--count` (filter, no row output) | ~3,100 MiB/s |
| gzip output (level 1) | ~125 MiB/s input-side, 6.1x ratio |
| gzip input (inflate) | ~190 MiB/s logical (zlib-bound, expected) |

### 2.3 v1.5.0: NDJSON input path, parallel pipeline, summary

Same host, same 1,035,000-row dataset (CSV 102 MiB, NDJSON 154 MiB). The
v1.4.0 column is the released v1.4.0 zip rebuilt on the same host; runs are
best of 3, warm page cache, output to /dev/null, `wait4`-measured. Outputs
of both versions were compared byte-for-byte before timing.

NDJSON input - the structural scanner + hashed keymap + zero-copy fields:

| Workload (NDJSON input) | v1.4.0 | v1.5.0 | Δ |
|---|---:|---:|---:|
| full → CSV | 421 MiB/s | 491 MiB/s | +17% |
| filter+project → CSV | 512 MiB/s | 670 MiB/s | +31% |
| full → NDJSON | 389 MiB/s | 512 MiB/s | +32% |
| filter + `--count` | 614 MiB/s | 885 MiB/s | +44% |

CSV input (parse path untouched; sanity that nothing regressed):

| Workload (CSV input) | v1.4.0 | v1.5.0 | Δ |
|---|---:|---:|---:|
| filter+project → CSV | 1,963 MiB/s | 1,963 MiB/s | 0% |
| full → NDJSON | 572 MiB/s | 616 MiB/s | +8% |
| filter + `--count` | 3,230 MiB/s | 3,296 MiB/s | +2% |

The full→NDJSON gain on a code-identical parse/serialize path (serializer
function bodies diff-verified between the versions) is attributed to hot
structure layout shifts (the keymap joined the transcoder state) and
codegen from the bind-path refactor.

Where the NDJSON-input gain comes from (isolated line-split/parse
micro-bench over the same file, v1.5 development measurements):
- line split alone (memchr): ~7,800 MiB/s - never the bottleneck;
- v1.4 parse: 637 MiB/s - every span walked byte-by-byte, keys resolved by
  linear strcmp, every value copied into the arena;
- v1.5 parse: 1,011 MiB/s in the micro-bench (structural bitmap walk +
  hashed keys + zero-copy).

`--jobs 2` vs single thread (same host, same method; peak RSS includes the
64 MiB total output-buffer budget that non-head workers may fill):

| Workload | jobs=1 | jobs=2 | Δ | peak RSS |
|---|---:|---:|---:|---:|
| CSV filter+project → CSV | 1,963 MiB/s | 2,110 MiB/s | +7% | 13 MiB |
| CSV full → NDJSON | 625 MiB/s | 750 MiB/s | +20% | 65 MiB |
| NDJSON full → CSV | 491 MiB/s | 662 MiB/s | +35% | 49 MiB |
| NDJSON filter → CSV | 670 MiB/s | 1,012 MiB/s | +51% | 16 MiB |

The planner reads only until every boundary is known (the file tail belongs
to the last worker by construction); with `--jobs 2` that is roughly half
the file at memchr-class speed. The sandbox has two cores and the ordered
drain serializes output through the head worker; scaling is expected to
grow with real core counts. Output byte-identity is asserted end-to-end on
the full 102-162 MiB datasets (four conversion pairs, `cmp`) and by
`test_parallel` across quoted, multi-line, ragged, reordered-key, filtered
and count workloads.

`--summary` on the reference data: ~700 MiB/s (CSV input), ~580 MiB/s
(NDJSON input). The accumulator is O(columns); nothing scales with rows.

### A/B against v1.3 and the emit-width investigation

Interleaved runs (alternating binaries, 7 rounds, best-of reported) between
the v1.3 release build and the v1.4 build on identical input:

| Workload | v1.3 | v1.4 | delta |
|---|---:|---:|---:|
| W2 full->NDJSON | 520.7 MiB/s | 577.3 MiB/s | **+10.9%** |
| W3 filter->NDJSON | 568.5 MiB/s | 622.2 MiB/s | **+9.5%** |
| W1 filter+project->CSV | 2077 MiB/s | 2059 MiB/s | -0.9% (same code path, noise) |

Per-SIMD-mode finding that shaped v1.4 (W2, best of 5):

| Mode | v1.3 | pure-64B fused emit | hybrid width (shipped) |
|---|---:|---:|---:|
| scalar | 0.2987 s | 0.2803 s (+6.5%) | 0.2803 s |
| AVX2 | 0.1841 s | 0.1735 s (+6.1%) | 0.1735 s |
| AVX-512 | 0.1970 s | 0.2018 s (**-2.4%**) | 0.177 s (**+10.2%**) |

The pure-64B fused emitter regressed on the AVX-512 default path: on this
Xeon the sustained 512-bit store stream costs more in frequency throttling
than the width wins back on short (<200 B) fields. The shipped policy uses
32-byte chunks below HP_EMIT_512_THRESHOLD (192 B) and the 64-byte loop only
for long values. On Zen 4/5-class cores the threshold is conservative; it is
a single documented constant (src/hp_json_avx512.c) and easy to retune.

### 2.1 PGO investigation (rejected, documented)

`make release-pgo` builds a 3-stage PGO binary (instrument → train → use).
Results on the reference host:

| Training set | filter+project→CSV | full→NDJSON | NDJSON→CSV |
|---|---:|---:|---:|
| none (plain -O3) | 1,994 MiB/s | 519 MiB/s | 421 MiB/s |
| CSV-only workload | **2,186 MiB/s (+9%)** | 486 MiB/s (−6%) | 275 MiB/s (−35%) |
| CSV + NDJSON workloads | 2,161 MiB/s (+8%) | 446 MiB/s (−14%) | — |

Diagnosis: with `-fprofile-use`, functions that did not appear in the
training run (the whole JSON serializer when training CSV-only) are treated
as cold and get size-optimized, regressing every untrained path; and even a
balanced training set made GCC reorder `hp_json_ser_row`'s hot branches with
a net loss. Decision: **PGO stays an opt-in experiment** (`make release-pgo`),
the default build remains plain `-O3`. Re-run the experiment on your own
workload before adopting — the harness above is the gate.

---

## 3. Linux Results (v1.2.0, 1.035M rows / 102.3 MiB)

| Tool | Time | Throughput | Row Rate | Peak RSS (VmHWM) |
|:---|---:|---:|---:|---:|
| **HyperPipe AVX-512 -> CSV** | **0.063 s** | **1,633.5 MiB/s** | 16.6 Mrows/s | **2.2 MB** |
| HyperPipe AVX-512 -> NDJSON | 0.135 s | 756.4 MiB/s | 7.7 Mrows/s | 2.2 MB |
| HyperPipe scalar -> NDJSON | 0.221 s | 462.9 MiB/s | 4.7 Mrows/s | 2.2 MB |
| mawk 1.3.4 (same job) | 0.309 s | 330.6 MiB/s | 3.3 Mrows/s | 2.4 MB |
| Python 3.12 csv (same job) | 1.776 s | 57.5 MiB/s | 0.58 Mrows/s | ~12 MB |

Notes:
- SIMD-vs-scalar speedup on this host: **1.63x** (756 vs 463 MiB/s).
- HyperPipe peak RSS is **flat across input sizes** (0.5 MB -> 1.035M rows
  tested here; 5.18 GB / 51M rows on the Windows host): ~2.2-2.4 MB Linux.
- mawk also streams with bounded memory — the memory advantage over awk
  class tools is *not* the selling point; the selling point is throughput +
  structured parsing + format conversion + resilience in one binary.

---

## 4. Windows Benchmark Results (v1.1.0 reference run)

```mermaid
flowchart LR
    subgraph SmallScale["Small Tier (5K rows / 0.5 MB)"]
        direction TB
        S_HP["HyperPipe (CSV): 35.6 MiB/s (0.014 s, 5.50 MB)"]
        S_AWK["GNU awk: 16.8 MiB/s (0.029 s, 10.22 MB)"]
        S_PERL["Perl: 17.8 MiB/s (0.028 s, 9.68 MB)"]
        S_PY["Python: 13.8 MiB/s (0.036 s, 12.28 MB)"]
    end

    subgraph MediumScale["Medium Tier (1.89M rows / 183 MB)"]
        direction TB
        M_HP["HyperPipe (CSV): 1,591.8 MiB/s (0.115 s, 5.51 MB)"]
        M_PERL["Perl: 171.6 MiB/s (1.067 s, 9.69 MB)"]
        M_AWK["GNU awk: 146.0 MiB/s (1.255 s, 10.22 MB)"]
        M_PY["Python: 108.5 MiB/s (1.689 s, 12.35 MB)"]
    end

    subgraph DatacenterScale["Datacenter Tier (51.07M rows / 5.18 GB)"]
        direction TB
        D_HP_WARM["HyperPipe (Warm): 1,848.9 MiB/s (2.675 s, 5.52 MB)"]
        D_HP["HyperPipe (Cold): 1,403.8 MiB/s (3.523 s, 5.51 MB)"]
        D_PERL["Perl: 166.1 MiB/s (29.77 s, 9.66 MB)"]
        D_AWK["GNU awk: 146.4 MiB/s (33.78 s, 10.21 MB)"]
        D_PY["Python: 109.8 MiB/s (45.04 s, 12.34 MB)"]
    end

    SmallScale --> MediumScale --> DatacenterScale
```

### Small Scale (5,000 rows / 0.49 MiB)

| Tool | Elapsed Time | Throughput | Row Rate | Peak RSS RAM | Speed Difference | Memory Difference |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| **HyperPipe (AVX-512 -> CSV)** | **0.014 s** | **35.61 MiB/s** | **0.361 Mrows/s** | **5.50 MB** | **1.0x (baseline)** | **1.0x (baseline)** |
| **HyperPipe (AVX-512 -> NDJSON)** | 0.014 s | 36.15 MiB/s | 0.366 Mrows/s | 5.50 MB | 0.99x | 1.0x |
| **Perl 5.38** | 0.028 s | 17.75 MiB/s | 0.180 Mrows/s | 9.68 MB | 2.0x slower | 1.76x more RAM |
| **GNU awk 5.0** | 0.029 s | 16.84 MiB/s | 0.171 Mrows/s | 10.22 MB | 2.1x slower | 1.86x more RAM |
| **Python 3.15 (csv)** | 0.036 s | 13.83 MiB/s | 0.140 Mrows/s | 12.28 MB | 2.6x slower | 2.23x more RAM |

---

### Medium Scale (1,891,714 rows / 183.18 MiB)

| Tool | Elapsed Time | Throughput | Row Rate | Peak RSS RAM | Speed Difference | Memory Difference |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| **HyperPipe (AVX-512 -> CSV)** | **0.115 s** | **1,591.80 MiB/s** | **16.439 Mrows/s** | **5.51 MB** | **1.0x (baseline)** | **1.0x (baseline)** |
| **HyperPipe (AVX-512 -> NDJSON)** | 0.206 s | 888.06 MiB/s | 9.171 Mrows/s | 5.51 MB | 1.79x | 1.0x |
| **Perl 5.38** | 1.067 s | 171.62 MiB/s | 1.772 Mrows/s | 9.69 MB | 9.3x slower | 1.76x more RAM |
| **GNU awk 5.0** | 1.255 s | 145.97 MiB/s | 1.508 Mrows/s | 10.22 MB | 10.9x slower | 1.85x more RAM |
| **Python 3.15 (csv)** | 1.689 s | 108.48 MiB/s | 1.120 Mrows/s | 12.35 MB | 14.7x slower | 2.24x more RAM |

---

### Datacenter Scale (51,076,278 rows / 4,945.81 MiB / ~5.18 GB)

| Tool | Elapsed Time | Throughput | Row Rate | Peak RSS RAM | Speed Difference | Memory Difference |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| **HyperPipe (AVX-512 -> CSV, Warm)** | **2.675 s** | **1,848.87 MiB/s** | **19.094 Mrows/s** | **5.52 MB** | **1.0x (baseline)** | **1.0x (baseline)** |
| **HyperPipe (AVX-512 -> CSV, Cold)** | 3.523 s | 1,403.77 MiB/s | 14.497 Mrows/s | 5.51 MB | 1.32x | 1.0x |
| **HyperPipe (AVX-512 -> NDJSON)** | 5.691 s | 869.11 MiB/s | 8.975 Mrows/s | 5.51 MB | 2.13x | 1.0x |
| **Perl 5.38** | 29.774 s | 166.11 MiB/s | 1.715 Mrows/s | 9.66 MB | 11.1x slower | 1.75x more RAM |
| **GNU awk 5.0** | 33.780 s | 146.41 MiB/s | 1.512 Mrows/s | 10.21 MB | 12.6x slower | 1.85x more RAM |
| **Python 3.15 (csv)** | 45.040 s | 109.81 MiB/s | 1.134 Mrows/s | 12.34 MB | 16.8x slower | 2.24x more RAM |

---

## 4. Implementation Details

1. **Multi-Chunk Bitmask Row Vectorization**:
   - Lines up to 192 bytes are analyzed via 64-byte AVX-512 vector chunks (`_mm512_loadu_si512`).
   - Structural delimiters and record terminators are isolated into 64-bit mask registers (`_mm512_cmpeq_epi8_mask`).
   - Field offsets and lengths are extracted via trailing zero count (`__builtin_ctzll` / `TZCNT`) and bit clearing (`delims &= delims - 1` / `BLSR`), avoiding scalar byte loops and repeated SIMD reloads.
   - Field count indices are maintained in CPU registers during parsing to prevent redundant stack spills.

2. **Inlined Fast-Path Filtering and Hoisted Dispatch**:
   - Single-equality integer and string predicates (`status == 200`) evaluate via zero-overhead inline memory comparisons directly against field pointers.
   - Dispatch function pointers (`parse_row_fn`), projection column indices, and output delimiters are hoisted outside the ingestion loops, avoiding per-row branch evaluation.
   - Ring buffer head updates are deferred across the entire contiguous block, eliminating 51 million per-row cursor modifications.

3. **Strict Memory Bounding ($O(1)$ Footprint)**:
   - Dynamic buffer allocation is fixed at 512 KiB for the input ring buffer and 64 KiB for the output page buffer.
   - Working set memory (Peak RSS) remains invariant at **5.50 - 5.52 MB (Windows) / 2.2 - 2.4 MB (Linux)** across all input scales from 0.5 MB to 5.18 GB (51+ million rows).
   - v1.2 additionally survives records larger than the ring buffer: `skip`/`quarantine` modes drop the whole record with exact byte accounting and resynchronize at the next line boundary instead of aborting.
