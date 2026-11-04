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
hyperpipe access.csv --group-by status --agg "count, sum(bytes)"
hyperpipe access.csv --sort -bytes --head 10
hyperpipe access.csv "timestamp >= 1995-07-01T12:00" --group-by "hour(timestamp)"
```

| Option | |
|---|---|
| `-o FILE` | output file; a `.gz` name compresses |
| `--from`, `--to` | `csv`, `tsv` or `ndjson` |
| `--select COLUMNS` | comma-separated columns to output, by name or 0-based position |
| `--where FILTER` | the filter, when not given as an argument |
| `--count` | print the number of matching rows |
| `--head N` | stop after N rows |
| `--group-by COLUMNS` | one row per distinct value of the columns (see Grouping) |
| `--agg FUNCTIONS` | what each group row holds; default `count` |
| `--sort COLUMNS` | sort rows, or groups; `-col` descends (see Sorting) |
| `--memory SIZE` | memory for sorting before temporary files are used (default `512M`) |
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

`A, B` is the same as `A and B`. Values with spaces need quotes. An
unquoted ISO 8601 literal (`2024-03-05`, `2024-03-05T12:00+02:00`) compares
times: `ts >= 2024-03-05` matches the fields that are times at or after
midnight UTC, in any form listed under Time. A filter
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

## Grouping

`--group-by` writes one row per distinct value of its columns, in the order
the values first appear, and `--agg` says what each row holds. Without
`--group-by` the whole input is one group.

```
hyperpipe access.csv "status >= 400" --group-by host --agg "count, sum(bytes), max(bytes)"
hyperpipe events.jsonl --agg "count, avg(latency)"
hyperpipe access.csv --group-by host --count            # the number of distinct hosts
```

| Function | |
|---|---|
| `count` | rows in the group |
| `count(col)` | rows where the column is not empty |
| `sum(col)`, `avg(col)` | sum and mean of the numbers in the column |
| `min(col)`, `max(col)` | smallest and largest number |
| `count_distinct(col)` | distinct non-empty values, compared as text |
| `median(col)`, `p95(col)`, `p99.9(col)` | quantiles of the numbers |

- Keys are compared as text: `1` and `"1"` are one group; a missing NDJSON
  key is a group of its own.
- `sum`, `avg`, `min` and `max` use the values that are numbers in their
  whole length, the rule numeric filters follow, and skip the rest. A group
  without numbers leaves them empty, as SQL does.
- Sums, minimums and maximums are exact decimals: `0.1 + 0.2` is `0.3`.
  They stay exact while each value fits in 64 bits with at most 18 decimal
  places and the sum fits in 128 bits; past that they are doubles. `avg` is
  the double nearest to the exact sum divided by the count.
- `count_distinct` is exact. Quantiles are exact, interpolated as numpy and
  pandas do, for groups of up to 1,024 numbers; past that they come from a
  sketch and are within 1% of the true value, in a few KiB per group.
- The result is the same for every `--jobs`. `--head N` writes the first N
  groups. Memory grows with the number of groups, and for `count_distinct`
  with the number of distinct values.

## Time

Timestamps are recognized in any field: ISO 8601 (`2024-03-05`,
`2024-03-05T12:34`, `2024-03-05 12:34:56.789+02:00`), the common log format
of web servers (`01/Jul/1995:00:00:01 -0400`), and Unix time in seconds or
milliseconds. Offsets are applied, so times are in UTC; a time without an
offset is taken as UTC.

`--group-by` takes `year(col)`, `month(col)`, `day(col)`, `hour(col)` and
`minute(col)`, which group by the start of that unit, written as sortable
text (`1995-07-01 04:00`). Values that are not times fall into one group
with an empty key.

```
hyperpipe access.log --group-by "day(timestamp), status" --agg "count, sum(bytes)"
hyperpipe events.jsonl "ts >= 2024-03-01 and ts < 2024-04-01" --group-by "hour(ts)" --sort "hour(ts)"
```

## Sorting

`--sort` takes comma-separated columns; a leading `-` sorts that column in
descending order. Rows sort by input columns, so the sort column need not
be selected; with `--group-by` the groups sort by their output columns.

```
hyperpipe access.csv --sort -bytes --head 10 --select host,bytes
hyperpipe access.csv --group-by host --agg "count, sum(bytes)" --sort "-sum(bytes)" --head 5
```

- Numbers compare by value, text by bytes. Numbers come first, then text,
  then empty values, in either direction, so `-bytes` starts with the
  largest number even when the column holds some text.
- Equal rows keep their input order, for every `--jobs`.
- The sort and the merge use every processor, from stdin too; `--jobs N`
  limits them to N threads.
- `--head N` after `--sort` keeps only N rows in memory, whatever the
  input size.
- Otherwise rows are held up to `--memory`; past that they are sorted in
  runs, written to temporary files and merged, sixteen at a time, so any
  input sorts in bounded memory.

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
the single-threaded run. Grouping and sorting run in parallel too: every
worker groups or sorts its chunk, and the coordinator merges the results,
sorted runs on several threads. Parallel runs do not apply, and the run
stays sequential, for stdin, gzip input, `--head` without grouping or
sorting, `--summary`, and CSV or TSV in `skip` or `quarantine` mode: there
a stray quote could move a chunk boundary.

## Memory

A 512 KiB input buffer and a 256 KiB output buffer, plus the longest record
seen. A parallel run holds at most `jobs + 2` chunks of a few MiB each.
Nothing grows with the input size; grouping grows with the number of groups,
and sorting is bounded by `--memory`.

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
| group by status: count, sum | 23 ms, 14 MiB | 97 ms, 190 MiB | 91 ms, 183 MiB | | |
| group by host: count | 21 ms, 20 MiB | 91 ms, 194 MiB | 88 ms, 192 MiB | 511 ms, 534 MiB | |
| NDJSON group by status: count, avg | 33 ms, 14 MiB | 108 ms, 231 MiB | | 3,922 ms, 140 MiB | |
| distinct hosts per status | 34 ms, 21 MiB | 139 ms, 194 MiB | 130 ms, 199 MiB | 720 ms, 542 MiB | |
| p50, p95, p99 per status | 37 ms, 19 MiB | 144 ms, 242 MiB | 144 ms, 256 MiB | | |
| count per hour | 32 ms, 14 MiB | 154 ms, 185 MiB | 147 ms, 214 MiB | 5,592 ms, 542 MiB | |
| count in a time range | 37 ms, 11 MiB | 171 ms, 184 MiB | 235 ms, 210 MiB | 3,093 ms, 572 MiB | |
| sort by bytes, top 10 | 35 ms, 13 MiB | 111 ms, 156 MiB | 139 ms, 353 MiB | | |
| sort 1M rows by four columns | 206 ms, 266 MiB | 401 ms, 678 MiB | 461 ms, 755 MiB | | |
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
