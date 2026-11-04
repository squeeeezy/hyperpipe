# Changelog

All notable changes to HyperPipe are documented here. Format based on
[Keep a Changelog](https://keepachangelog.com/).

## [2.0.0] - 2026-11-02

A correctness and speed release. Fifteen defects are fixed, several of them
silent data loss or memory errors; parallel runs now scale and are the
default for large files; and every workload is at least as fast as in 1.5.

### Changed (breaking)
- `--jobs` defaults to `auto`: one thread per processor, at most 16, for
  regular files of 16 MiB or more. `--jobs 1` restores sequential runs;
  `--jobs 0` is rejected.
- NDJSON input must be valid JSON: a complete object per line. Truncated
  lines, missing commas, trailing text and invalid escapes are malformed
  records (`HP_ERR_PARSE_JSON`) handled by `--on-error`.
- CSV text after a closing quote (`"abc"xyz`) is a malformed record.
- Filters parse strictly. A missing value, an unterminated quote, two
  conditions without `and`/`or`, a value over 255 bytes or more than 16
  conditions are errors; before, they matched silently or were truncated.
- `--select` splits on commas only and trims each name, so names may hold
  spaces; a name over 127 bytes is an error instead of being truncated.
- A single argument is the input file when the file exists or the argument
  cannot be a filter; before, names containing `.csv` or `.log` anywhere
  were taken as files.
- `--help` prints to stdout. Every message starts with `hyperpipe:`.
- Numeric predicates compare values: `a == 5000` matches `5000.0` and `5e3`;
  fields such as `200abc`, `0x10` and `inf` are not numbers.
- Column references resolve by header name first, by position otherwise.
- Parallel NDJSON runs lock the schema to the first line in every chunk, so
  output no longer depends on the job count.
- CSV and TSV with `--on-error skip` or `quarantine` run sequentially.
- Errors print as `hyperpipe: <reason>`; `--help`, `--version` and
  `--stats` use plain text.
- Library: `hp_csv_parser_free()`, output and quarantine sinks and
  `lock_schema` in `hp_transcoder_config_t`, `hp_strerror()`,
  `hp_parallel_auto_jobs()`; `hp_filter_matches()` is inline and calls
  `hp_filter_eval()`; the `plan_masks` kernels are gone.

### Added
- `count_distinct(col)`, exact, and quantiles: `median(col)` and `pN(col)`
  for any percentile (`p95`, `p99.9`). Quantiles are exact, as numpy and
  pandas compute them, up to 1,024 values per group and within 1% past
  that, from a mergeable sketch; parallel runs give the same results.
- Time: ISO 8601, the common log format and Unix time are recognized in any
  field. `--group-by` takes `year`, `month`, `day`, `hour` and `minute` of a
  column, and unquoted ISO 8601 literals in filters compare as times
  (`ts >= 2024-03-05T12:00`). In UTC, independent of the machine's time
  zone. Library: `hp_time.h`, `is_time` in `hp_predicate_t`.
- `--sort` with `-col` for descending order and `--memory` for its budget.
  Stable, numbers by value and text by bytes, empty values last; `--head N`
  keeps N rows in a heap; larger inputs spill sorted runs to temporary
  files and merge them a level at a time. Parallel runs sort their chunks
  and merge key ranges on several threads, byte-identical to one thread;
  other runs, stdin included, sort and merge their last rows on every
  processor, or on `--jobs` threads.
  Library: `hp_sort.h`, `sort_spec`/`sort_memory` in
  `hp_transcoder_config_t`, `hp_transcoder_write_sorted()`.
- `--group-by` and `--agg`: `count`, `count(col)`, `sum`, `avg`, `min` and
  `max` per group, in first-seen order, in one pass and in parallel. Sums,
  minimums and maximums are exact decimals, so `0.1 + 0.2` is `0.3` and the
  result does not depend on the job count. 4x faster than DuckDB and qsv on
  the benchmark data, at a tenth of the memory. Library: `hp_agg.h`,
  `group_by_spec`/`agg_spec` in `hp_transcoder_config_t`,
  `hp_transcoder_write_groups()`, `HP_ERR_AGG`.
- Errors say where: a malformed or oversized record by its number and byte
  offset (parallel runs included), a filter by the place parsing stopped,
  a missing column by its name. Library: `records_in`, `error_record`,
  `error_offset` and `error_column` in `hp_transcoder_stats_t`, and
  `HP_ERR_FILTER` with `error`/`error_at` in `hp_filter_t`.
- Nested NDJSON values. Objects and arrays are checked against the JSON
  grammar and pass through as JSON text: verbatim to NDJSON, as a text
  field to CSV and TSV. Nesting deeper than 512 levels is a malformed
  record. Library: `is_json` in `hp_str_t`.

### Fixed
- Heap overflow when a CSV record over 256 KiB was written as CSV.
- Windows: stdout wrote `\r\n` and stdin stopped at the first 0x1A byte,
  breaking output and gzip pipes.
- A single `==` predicate compared bytes, not numbers.
- Numeric predicates accepted numeric prefixes and rejected the int64
  minimum.
- A column named like a number (`2019`) was read as a position.
- Text after a closing quote was dropped silently.
- The Makefile did not include its header dependency files, so header
  changes left stale objects.
- Quoted CSV fields with `""` escapes over 64 KiB failed.
- Escaped NDJSON strings over 64 KiB were cut short and read past their
  buffer.
- TSV cells containing commas were not quoted in CSV output.
- `--gzip` wrote records larger than the output page uncompressed.
- NDJSON accepted malformed lines as rows with wrong values.
- Failed parallel runs exited without a message.
- The CMake build left out three source files and could not link.
- NDJSON keys and CSV header names were cut at 255 bytes, merging columns.
- Smaller: absent projected columns kept a stale `present` flag; a file
  handle leaked per positional argument; zlib could load from the current
  directory; `--buffer-size` accepted garbage.

### Performance
NASA access log, Ryzen 5 7600X, Windows, default settings against 1.5.0:
NDJSON to CSV 5.0x, NDJSON filter 4.4x, NDJSON to NDJSON 3.8x, `--summary`
2.9x, CSV to NDJSON 2.4x, CSV filters 1.8-2.1x. Single-threaded, every
workload is as fast or faster.

- Parallel runs find chunk boundaries in parallel from per-range quote
  parity and schedule small chunks from a pool, instead of scanning the
  whole file first and stalling workers on a shared output budget.
- `--summary` reads integers without `strtod` (2.9x).
- CSV records are written with one bounds check, short fields are copied
  inline, and the quoting decision scans a word at a time.
- The single-equality filter is decided inline with one byte compare.

### Build and tests
- Every test runs on Windows and reports TAP; `make test` runs all of them.
- `tests/test_regressions.c` holds one case per fixed defect.
- The Makefile uses pattern rules; CMake builds portable binaries (no
  `-march=native`) and registers every test.
- `bench/bench_ab.py` compares binaries with interleaved runs and checks
  that their outputs are identical.

## [1.5.0] - 2026-11-02

NDJSON input throughput release (+17-44% on NDJSON-input workloads), a
parallel pipeline for multi-core hosts with byte-identical output
guarantees, two data-facing modes (`--summary`, `--schema`), and a
release-gate fuzz run (200k mutations) that found one more fidelity bug.
The build now tracks header dependencies, which uncovered and fixed a
silent stale-object ABI hazard.

### Added
- **`--jobs N` parallel pipeline.** A quote-aware planner finds record
  boundaries with the new `plan_masks` kernels (newline/quote bitmaps per
  64-byte chunk; AVX-512 / AVX2 / scalar twins, differentially tested),
  one worker per chunk runs the full parse→filter→serialize pipeline, and
  the coordinator drains bounded page queues strictly in chunk order:
  output is byte-identical to the single-threaded stream - asserted
  end-to-end on 102-162 MiB real datasets across CSV→NDJSON, NDJSON→CSV,
  NDJSON→NDJSON, filters, projections and count, plus `test_parallel` on
  quoted, multi-line, ragged and reordered-key shapes. Plain CSV/TSV and
  NDJSON files are supported (NDJSON locks the schema to the first line,
  the same rule CSV output documents); gzip input falls back with a
  notice; the input must be a regular seekable file. Output buffering is
  capped at 64 MiB total regardless of input size and job count.
  Measured on the 2-vCPU reference host at `--jobs 2`: +7% on the CSV
  filter workload, +20% full CSV→NDJSON, +35% full NDJSON→CSV, +51%
  filtered NDJSON→CSV - the win grows with real core counts.
- **`--summary`.** Streaming per-column statistics instead of row output:
  inferred type (int / number / string / mixed), present/empty/absent
  counts, numeric min/max/mean, lexicographic string bounds. The
  accumulator is O(columns) and retains no row pointers.
- **`--schema <file>`.** Declared column types (`name,type` lines, `#`
  comments). Columns declared `int` take a fast int64 span parse in numeric
  filter predicates instead of locale-bound `strtod`; fractional literals
  still compare correctly against int columns.
- **Fuzzing:** in-repo seed corpus (`tests/corpus/`, 10 structural seeds
  including the regression case below), an NDJSON roundtrip invariant in
  the fuzz harness (order-free XOR fingerprint over present fields),
  `make fuzz-long` (FUZZ_K-scale mutation budget) and `make coverage`
  (gcov report over the full suite; ~70% line coverage - the library is
  exercised by the suites, the CLI entry point by benchmarks).

### Changed
- **SIMD structural NDJSON scanner.** One masked vector pass per 64-byte
  chunk builds the quote/backslash bitmap of the line; keys and escape-free
  string values become span arithmetic between structural quotes and
  literals keep a short scalar scan. Lines that do not match the clean
  shape (any backslash, unknown key, unexpected byte) fall back to the
  byte-span scanner with the exact historical semantics - outputs are
  bit-identical to v1.4 on full-file conversions.
- **Zero-copy NDJSON fields.** The per-line staging copy and the per-value
  arena copy are gone; fields point into the ring buffer until the row is
  consumed. Only strings containing backslash escapes are decoded into the
  arena.
- **Hashed key lookup (`hp_keymap`).** Field-to-column resolution probes an
  open-addressing index instead of walking the header with strcmp; keys of
  up to 8 bytes (the vast majority) confirm with two integer compares and
  no memory walk. The map stays in sync when the header grows mid-stream.
- **Build system.** `-MMD -MP` header dependency tracking; before this, a
  header layout change could link stale objects silently (caught the hard
  way during development: a dispatch-table field insertion linked an old
  object and produced wrong dispatch). `-pthread`, `-D_FILE_OFFSET_BITS=64`,
  SIMD translation units compile only on x86 (other architectures run the
  scalar dispatch); the dispatch table is mutex-guarded for the parallel
  workers.

### Fixed
- **Unterminated quoted field at EOF was emitted raw** (found by the
  200k-iteration fuzz gate on release day). The lenient EOF tail of a
  quoted field bypassed the re-quoting write path, leaking CR/LF/quote
  bytes into the output as record structure - a CSV→CSV roundtrip drifted
  fields 2 -> 3. Quoted-origin fields now always travel the re-serializing
  write path (with `""` pairs taken from the unescape arena), and the
  roundtrip reaches a fingerprint-stable fixed point. The input is a
  permanent corpus seed and a `test_roundtrip` case.
- `--count` and `--summary` no longer emit the CSV/TSV header line (v1.4
  printed a header before the count on NDJSON->CSV and CSV->CSV counts).
- An NDJSON line larger than the ring buffer now quarantines the buffered
  span and resynchronizes at the next newline; previously its tail could be
  fed to the parser as fake rows (each counted separately in skip mode).
- Integer predicate values that overflow int64 are rejected instead of
  silently wrapping (9223372036854775808 previously compared as a negative
  number).
- NDJSON lines between 64 KiB and the ring-buffer size no longer fail in
  `--on-error fail` mode: the 64 KiB staging copy is gone, so line length is
  bounded by the ring buffer alone.

### Performance (2-vCPU Xeon reference host; paired A/B, v1.4.0 rebuilt from
### its release zip on the same host, best of 3, outputs byte-compared)

NDJSON input (154 MiB / 1.035M rows) - the structural scanner + keymap:

| Workload (NDJSON input) | v1.4.0 | v1.5.0 | Δ |
|---|---:|---:|---:|
| full → CSV | 421 MiB/s | 491 MiB/s | +17% |
| filter+project → CSV | 512 MiB/s | 670 MiB/s | +31% |
| full → NDJSON | 389 MiB/s | 512 MiB/s | +32% |
| filter + `--count` | 614 MiB/s | 885 MiB/s | +44% |

CSV input (102 MiB) - parse path untouched, sanity numbers:

| Workload (CSV input) | v1.4.0 | v1.5.0 | Δ |
|---|---:|---:|---:|
| filter+project → CSV | 1,963 MiB/s | 1,963 MiB/s | 0% |
| full → NDJSON | 572 MiB/s | 616 MiB/s | +8% |
| filter + `--count` | 3,230 MiB/s | 3,296 MiB/s | +2% |

The full→NDJSON gain on an unchanged parse/serialize path is attributed to
hot-structure layout shifts (the keymap joined the transcoder state) and
codegen from the bind-path refactor; the serializer function bodies are
byte-identical between the versions (verified by diff).

`--jobs 2` vs single thread on the same host (peak RSS includes the 64 MiB
output-buffer budget):

| Workload | jobs=1 | jobs=2 | Δ | peak RSS |
|---|---:|---:|---:|---:|
| CSV filter+project → CSV | 1,963 MiB/s | 2,110 MiB/s | +7% | 13 MiB |
| CSV full → NDJSON | 625 MiB/s | 750 MiB/s | +20% | 65 MiB |
| NDJSON full → CSV | 491 MiB/s | 662 MiB/s | +35% | 49 MiB |
| NDJSON filter → CSV | 670 MiB/s | 1,012 MiB/s | +51% | 16 MiB |

`--summary` on the reference data: ~700 MiB/s (CSV input), ~580 MiB/s
(NDJSON input).

## [1.4.0] - 2026-11-02

Throughput + compression release: ~10% more NDJSON conversion speed, native
gzip support with zero hard dependencies, a count mode, and an in-tree fuzzing
framework that immediately paid for itself by finding two fidelity bugs.

### Added
- **Transparent gzip input.** Any input starting with the gzip magic
  (0x1F 0x8B) is decompressed on the fly - no flag needed. libz is located
  at runtime via `dlopen()`/`LoadLibrary()`; without it, plain files work
  exactly as before and gzip inputs fail with a dedicated, explicit error
  code. Concatenated multi-member gzip files (gzip(1) -c semantics) decode
  fully.
- **gzip output.** `--gzip` or any `.gz` output suffix compresses the
  output stream (level 1, Z_BEST_SPEED; ~6x ratio on the reference log at
  ~125 MiB/s input-side on the 2-vCPU reference host). Reads back with
  system gunzip byte-identically.
- **`--count`.** Prints the number of matching rows to stdout and emits no
  row output - a streaming `grep -c` for structured data (measured:
  ~3,100 MiB/s on the reference workload). Combines with filters and
  `--head`.
- **In-tree fuzzing framework.** `tests/test_chaos.c` (400 seeded adversarial
  documents per run, wired into `make test`) checks kernel differential
  equality and value fidelity; `tests/fuzz_harness.c` adds the same
  invariants as a libFuzzer entry point plus a standalone ASan+UBSan
  mutation driver (`make fuzz`, 20k iterations by default, no libFuzzer
  required).

### Fixed
- **Empty quoted fields lost their type on CSV/TSV output** (found by the
  new fuzzer on day one). `""` degenerated to a bare empty field, which
  collapses to a blank line for single-column rows - rows disappeared from
  round-trips. Typed empty cells (and empty header names) are now written
  back as `""`.
- **CSV->CSV silently truncated ragged rows** to the first-row header width
  (found by the fuzzer). A transcoder must never lose columns silently:
  CSV input now emits each row's real width. NDJSON input keeps the
  documented first-row schema lock (a streaming header cannot be rewritten).
- **Non-fragment key fallback escaped nothing.** Header names containing
  quotes/backslashes/control bytes now go through the same escaping rules
  as values in the slow path too.
- NDJSON input parsing used locale-dependent `isspace()`; it now uses the
  exact RFC 8259 whitespace set (also faster: no locale table indirection).

### Changed
- **Fused SIMD JSON string emitter.** The serializer no longer scans for
  escapable bytes and then memcpy-s; it copies and scans in one pass with
  vector stores (AVX-512 masked stores for clean 64-byte chunks, AVX2 twin,
  shared scalar expander for special bytes - one source of truth in
  `hp_json_escape_one`).
- **Direct-to-page NDJSON serialization.** Rows are emitted straight into
  the output page buffer; the intermediate serializer-buffer copy is gone
  from the hot path (giant rows still take the growable-buffer fallback).
- **Hybrid emit width policy.** Measured on the reference Xeon: sustained
  512-bit stores throttle the core enough that a pure-64B emit regressed
  vs AVX2 (-2.4%). Strings below 192 bytes now use the 32-byte loop and
  only long values take the 64-byte path: +10.9% full-file NDJSON
  conversion vs v1.3 (and +6% on AVX2-only hosts). The full A/B log is in
  docs/BENCHMARKS.md section 2.2.
- Output page buffer sized 64 KiB -> 256 KiB (fewer write syscalls; flat
  on tmpfs, better on real disks).
- `test_rfc4180` no longer relies on stack garbage: parser state is zeroed
  before use (latent UB surfaced by a rebuild).

### Performance (2-vCPU Xeon reference host, 102 MiB / 1.035M rows)
- full -> NDJSON: 521 -> 577 MiB/s (+10.9%)
- filter -> NDJSON: 568 -> 622 MiB/s (+9.5%)
- filter+project -> CSV: unchanged (~2,050 MiB/s, parse path untouched)
- `--count`: ~3,100 MiB/s
- gzip out: ~125 MiB/s input-side at 6.1x ratio (level 1)
- gzip in: ~190 MiB/s logical (inflate-bound, expected for zlib)

## [1.3.0] - 2026-11-02

Performance release: same correctness bar, 25-100% more throughput, plus a
reproducible competitive benchmark harness.

### Changed
- **AVX-512 row parser rewritten as a chunk loop.** The v1.2 parser unrolled
  three 64-byte chunks and then re-parsed the whole row with the scalar state
  machine. It now walks every 64-byte chunk vectorized and only defers to the
  scalar RFC 4180 machine when a quote appears. Rows longer than 192 bytes
  keep SIMD speed: 1,633 -> 2,046 MiB/s (+25%) on the reference workload,
  and 120 B-2 KB rows stream at 4,268 MiB/s.
- **AVX2 hosts get the same fast path.** Added `hp_csv_parse_row_avx2`
  (256-bit twin of the AVX-512 loop). Previously AVX2-only hosts assembled
  rows scalar: ~1,350 -> 1,686 MiB/s.
- **Zero-copy NDJSON output.** CSV->NDJSON serialized rows are no longer
  copied through a 24 KB row struct per row; with a projection only the
  selected columns move. Full-file conversion: 259-297 -> 521 MiB/s
  (+80-100%).
- **Memoized JSON key fragments.** `"key":` fragments are built once per
  header (with proper escaping for hostile header names) and emitted with a
  single memcpy per key instead of strlen + double-store per row.

### Added
- `scripts/bench_competitors.py`: one-command competitive benchmark
  (duckdb / qsv / miller / mawk) with kernel-accounted peak RSS
  (`os.wait4` -> `ru_maxrss`), output-identity gates and a Markdown table.
- `make release-pgo`: opt-in profile-guided build, with the full
  investigation log (it was measured and **rejected** for the default build;
  see docs/BENCHMARKS.md 2.1).
- `docs/ROADMAP.md`: live prioritized roadmap.

### Fixed
- `--help` claimed a 65,536-byte default ring buffer; the actual default is
  524,288 bytes. Help text now matches `HP_DEFAULT_BUFFER_CAPACITY`.
- README RSS figures now use the verified kernel accounting (ru_maxrss vs a
  9.9 MiB bare-binary baseline on the reference sandbox) instead of the
  unexplained 2.2 MB number.

## [1.2.0] - 2026-11-02

Correctness release: five data-fidelity bugs fixed, resilience gaps closed,
query language extended. Performance unchanged or slightly better
(1,633 MiB/s -> CSV vs 1,571 MiB/s in v1.1 on the Linux reference host).

### Fixed
- **CSV/TSV output corruption on quoted fields (critical).** Fields parsed
  from quoted CSV values (`"x,1"`, embedded newlines) were written back
  unquoted, silently changing the column count of the output. All quoted
  values now travel through the re-serializing write path; output is valid
  RFC 4180. The 3-column projection fast path (used by the benchmark) now
  detects quoted fields and falls back instead of bypassing quoting.
- **NDJSON `\uXXXX` escapes were not decoded.** `caf\u00e9` produced the
  literal text `cafu00e9`. All JSON escapes including `\b`, `\f` and UTF-16
  surrogate pairs (`\ud83d\ude00` -> emoji) are now decoded; lone surrogates
  map to U+FFFD.
- **NDJSON rows with reordered keys were silently mislabeled.** Values were
  matched to columns positionally, so `{"b":3,"a":4}` emitted `a=3`. Fields
  are now resolved by key against the running header.
- **Keys introduced mid-stream were renamed to `col_N`.** NDJSON output now
  follows schema growth (new keys get their real names); CSV/TSV output is
  documented as locked to the first-row header and truncates late columns.
- **A record larger than the streaming buffer aborted the whole run, even
  with `--on-error skip`.** Oversized records are now dropped with exact
  byte accounting into quarantine and the stream resynchronizes at the next
  line boundary; their tails are never parsed as fake rows.
- **`test_real_dataset` hard-coded 1.89M-row expectations** and failed on
  the bundled 5K sample. Expectations now scale with the dataset actually
  present.

### Added
- `or` / `||` in filter expressions (each branch is a conjunction:
  `a == 1 and b == 2 or c == 3`).
- String operators `contains`, `startswith`, `endswith` (always text compare).
- `--head N` / `--limit N`: stop after N emitted rows — instant sampling of
  huge files.
- **Type fidelity through quoting**: quoted CSV values and JSON strings keep
  string identity in NDJSON output (`"null"` stays `"null"`, unquoted
  `null` becomes JSON null; `"007"` stays `"007"`, unquoted `007` becomes
  `7`). Sparse NDJSON rows omit absent keys instead of emitting empties.
- `make test` now runs all 7 suites (previously 2 suites were only wired in
  CMake).

### Docs
- README rewritten: working CLI examples (the old `--on-error quarantine
  bad.log` positional syntax never worked — `--quarantine` is required),
  honest performance summary, competitor positioning table, memory model,
  roadmap. Removed the unimplemented "Arrow" claim.
- BENCHMARKS.md: methodology & reproducibility section, Linux (v1.2.0) and
  Windows (v1.1.0) results separated with exact tool versions.

## [1.1.0] - 2026-11-01

Initial public version: CSV/TSV/NDJSON auto-sniffing, AVX-512BW/AVX2/scalar
runtime dispatch, natural predicate filtering, column projection, fault
tolerance (`fail`/`skip`/`quarantine`), 512 KiB bounded streaming buffers,
differential test suite.
