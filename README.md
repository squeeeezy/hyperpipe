# HyperPipe

[![ci](https://github.com/squeeeezy/hyperpipe/actions/workflows/ci.yml/badge.svg)](https://github.com/squeeeezy/hyperpipe/actions/workflows/ci.yml)

A streaming filter and converter for CSV, TSV and NDJSON, written in C11.
It detects the input format, parses with SIMD kernels chosen at run time
(AVX-512, AVX2 or scalar), spreads large files over all cores, and keeps
memory flat whatever the input size.

```
hyperpipe access.log "status == 500 or url startswith /api" --select host,url,status
```

## Build

```
make            # build/hyperpipe
make test       # every test, TAP output
```

GCC or Clang on Linux and macOS, MinGW-w64 on Windows; x86-64 and ARM64,
all tested in CI. No dependencies; zlib is loaded at run time when gzip is
used. The vector engines are x86 only for now; ARM runs the scalar engine.

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## Usage

```
hyperpipe [FILE] [FILTER] [options]
```

Input is FILE or stdin. The format is detected from the first bytes, gzip
input is decompressed, and the output format defaults to the other one:
CSV and TSV become NDJSON, NDJSON becomes CSV. A single argument is the
input file when such a file exists or when it cannot be a filter (no space
and none of `= < > !`); otherwise it is the filter.

```
hyperpipe access.csv                                    # CSV to NDJSON
hyperpipe access.csv "status == 200 and bytes > 5000" --select host,bytes
hyperpipe events.jsonl --to csv -o events.csv
hyperpipe huge.csv "status == 500" --count
hyperpipe huge.csv "status == 500" --head 100
hyperpipe access.csv.gz --to ndjson -o out.jsonl.gz
hyperpipe dirty.csv --on-error quarantine --quarantine bad.txt
hyperpipe access.csv --summary
```

| Option | |
|---|---|
| `-o FILE` | output file; a `.gz` name compresses |
| `--from`, `--to` | `csv`, `tsv` or `ndjson` |
| `--select COLUMNS` | comma-separated columns to output, by name or 0-based position |
| `--where FILTER` | the filter, when not given as an argument |
| `--count` | print the number of matching rows |
| `--head N` | stop after N rows |
| `--summary` | per-column type, counts, min, max and mean instead of rows |
| `--schema FILE` | declared column types, one `name,int` or `name,string` per line |
| `--jobs N\|auto` | threads; auto uses one per processor for files of 16 MiB or more |
| `--on-error MODE` | `fail` (default), `skip` or `quarantine` |
| `--gzip` | compress the output |
| `--simd ENGINE` | `auto`, `avx512`, `avx2` or `scalar` |
| `--buffer-size N` | input buffer; also the largest record accepted (default 512 KiB) |
| `--stats` | throughput on stderr |

## Filters

| Expression | |
|---|---|
| `col == 200`, `col != 200` | numeric when the literal is a number |
| `col > 10`, `>=`, `<`, `<=` | numeric, or byte order for text |
| `col contains foo` | substring |
| `col startswith /api`, `col endswith .json` | prefix, suffix |
| `A and B`, `A && B` | both |
| `A or B`, `A \|\| B` | either; `A and B or C` is `(A and B) or C` |
| `col == "007"` | a quoted literal always compares as text |

`A, B` is the same as `A and B`. Values with spaces need quotes. A filter
that does not parse is an error that points at the place it stopped:

```
hyperpipe: invalid filter: expected a value
  status ==
          ^
```

A numeric comparison matches only fields that are numbers in their whole
length: `5000`, `5000.0` and `5e3` are equal, while `200abc`, `0x10`, `inf`
and an empty field match no numeric predicate. A column name is looked up
in the header first; a number that is not a column name selects a column by
position.

## Data rules

- CSV follows RFC 4180. A quoted field keeps its string type: `"007"` stays
  the string `"007"` in NDJSON, unquoted `007` becomes a number, and `""`
  stays an empty string rather than an absent value.
- Text after a closing quote, or a quote inside an unquoted field, is a
  malformed record.
- NDJSON lines must be complete JSON objects. Truncated lines, missing
  commas, trailing text and invalid escapes are malformed records. Raw
  control characters inside strings are accepted.
- Object and array values pass through as JSON text: verbatim to NDJSON,
  as a text field to CSV and TSV. Filters and `--summary` treat them as
  strings.
- NDJSON rows may omit, reorder or add keys. NDJSON output follows keys that
  appear later; CSV output keeps the columns of the first line. Parallel
  runs keep the first line's keys for every format, so their output does not
  depend on the number of threads.
- Ragged CSV rows keep their width.
- Records up to the input buffer size are accepted, quoted and escaped
  fields included.

Malformed records stop the run in `fail` mode with the record's number and
byte offset, which hold for parallel runs too:

```
hyperpipe: malformed CSV quoting at record 1234569, byte 31975292
```

Records are counted from the start of the input, header and blank lines
included, so the record number is the line number unless quoted fields span
lines. `skip` drops the record and resumes at the next line; `quarantine`
also copies it, byte for byte, to the quarantine file.

## Parallel runs

For a regular file, `--jobs` splits the input into chunks of whole records
and runs the full pipeline on each in parallel. Output is byte-identical to
the single-threaded run. It does not apply, and the run stays sequential,
for stdin, gzip input, `--head`, `--summary`, and CSV or TSV in `skip` or
`quarantine` mode: there a stray quote could move a chunk boundary.

## Memory

A 512 KiB input buffer and a 256 KiB output buffer, plus the longest record
seen. A parallel run holds at most `jobs + 2` chunks of a few MiB each.
Nothing grows with the input size.

## Performance

NASA access log (100 MiB CSV, 150 MiB NDJSON), Ryzen 5 7600X, Windows 11,
output to a file. Default settings, compared with 1.5.0:

| Workload | 1.5.0 | 2.0.0 |
|---|---:|---:|
| filter + projection, CSV to CSV | 1,480 MiB/s | 2,770 MiB/s |
| CSV to NDJSON | 470 MiB/s | 1,230 MiB/s |
| NDJSON to CSV | 530 MiB/s | 2,630 MiB/s |
| NDJSON filter + projection | 980 MiB/s | 4,420 MiB/s |
| `--summary` | 360 MiB/s | 1,060 MiB/s |

Against other tools on Linux, same data and machine, results checked to be
equal (median time, peak RSS):

| Task | HyperPipe | DuckDB 2.0 | qsv 28 | miller 6.11 | jq 1.7 |
|---|---:|---:|---:|---:|---:|
| filter + project, CSV | 27 ms, 17 MiB | 127 ms, 271 MiB | 167 ms, 231 MiB | 504 ms, 554 MiB | |
| CSV to NDJSON | 97 ms, 33 MiB | 263 ms, 595 MiB | 939 ms, 246 MiB | 1,280 ms, 565 MiB | |
| NDJSON to CSV | 66 ms, 27 MiB | 222 ms, 529 MiB | 349 ms, 62 MiB | 3,857 ms, 137 MiB | 3,228 ms, 4 MiB |
| NDJSON filter + project | 26 ms, 12 MiB | 99 ms, 231 MiB | | 3,870 ms, 137 MiB | 1,403 ms, 4 MiB |
| count matching rows | 17 ms, 11 MiB | 91 ms, 156 MiB | 76 ms, 167 MiB | 490 ms, 535 MiB | |
| start-up, 5,000 rows | 2 ms, 2 MiB | 39 ms, 51 MiB | 23 ms, 61 MiB | 7 ms, 17 MiB | |

[docs/BENCHMARKS.md](docs/BENCHMARKS.md) has the method, the exact commands
and the single-thread numbers.

## Tests

`make test` runs unit, end-to-end, differential (every SIMD engine against
the scalar one), parallel and chaos tests; `make fuzz` runs the fuzz harness
under ASan and UBSan. `tests/fuzz_harness.c` also builds as a libFuzzer
target.

## License

MIT
