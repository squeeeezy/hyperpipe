# HyperPipe

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

GCC or Clang on Linux and other POSIX systems, MinGW-w64 on Windows. No
dependencies; zlib is loaded at run time when gzip is used.

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## Usage

```
hyperpipe [FILE] [FILTER] [options]
```

Input is FILE or stdin. The format is detected from the first bytes, gzip
input is decompressed, and the output format defaults to the other one:
CSV and TSV become NDJSON, NDJSON becomes CSV.

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
| `--select COLUMNS` | columns to output, by name or 0-based position |
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
- NDJSON lines must be complete JSON objects whose values are strings,
  numbers, `true`, `false` or `null`. Truncated lines, missing commas,
  trailing text and invalid escapes are malformed records. Raw control
  characters inside strings are accepted.
- NDJSON rows may omit, reorder or add keys. NDJSON output follows keys that
  appear later; CSV output keeps the columns of the first line. Parallel
  runs keep the first line's keys for every format, so their output does not
  depend on the number of threads.
- Ragged CSV rows keep their width.
- Records up to the input buffer size are accepted, quoted and escaped
  fields included.

Malformed records stop the run in `fail` mode. `skip` drops the record and
resumes at the next line; `quarantine` also copies it, byte for byte, to the
quarantine file.

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

[docs/BENCHMARKS.md](docs/BENCHMARKS.md) has the method, the single-thread
numbers and other tools on the same data.

## Tests

`make test` runs unit, end-to-end, differential (every SIMD engine against
the scalar one), parallel and chaos tests; `make fuzz` runs the fuzz harness
under ASan and UBSan. `tests/fuzz_harness.c` also builds as a libFuzzer
target.

## License

MIT
