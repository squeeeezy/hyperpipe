# Benchmarks

## Method

- Data: the NASA access log replicated to 1,000,000 rows, as CSV (98 MiB,
  five columns) and as NDJSON (149 MiB, the same rows converted by
  HyperPipe).
- Tool: `bench/bench_ab.py`. Every binary runs once per round, rounds are
  repeated (15 here), the table shows the fastest run, and the speedup is
  the median of the per-round ratios, which cancels slow drift in machine
  load. Outputs are hashed and must be identical across binaries.
- Output goes to a file. On Linux it goes to `/dev/shm` so that disk
  writeback does not land in the timings.
- Machine: AMD Ryzen 5 7600X (6 cores, 12 threads, AVX-512), 32 GB DDR5.
  Windows 11 with MinGW-w64 GCC 16; Linux is Ubuntu 24.04 under WSL 2 on
  the same machine, GCC 13. Both built with `make` (`-O3`).

| | Workload |
|---|---|
| W1 | `"status == 200" --select host,status,bytes --to csv` |
| W2 | `--to ndjson` |
| W3 | `--to csv` |
| W4 | `"status == 500" --count` |
| W5 | `"request contains apollo" --to csv` |
| W6 | `"bytes > 5000 and bytes < 20000" --select host,bytes --to csv` |
| W7 | NDJSON input, `--to csv` |
| W8 | NDJSON input, `"status == 404" --select host,request --to csv` |
| W9 | NDJSON input, `--to ndjson` |
| W10 | `--summary` |

Reproduce:

```
python3 bench/bench_ab.py --csv big.csv --ndjson big.ndjson -n 15 \
        --out-dir /dev/shm hyperpipe-1.5 hyperpipe-2.0
```

## 2.0 against 1.5.0

### Linux, default settings (2.0 uses all threads)

| Workload | 1.5.0 | 2.0 | Speedup |
|---|---:|---:|---:|
| W1 | 2,779 MiB/s | 4,332 MiB/s | 1.41x |
| W2 | 636 MiB/s | 1,118 MiB/s | 1.64x |
| W3 | 1,354 MiB/s | 1,778 MiB/s | 1.20x |
| W4 | 5,567 MiB/s | 6,404 MiB/s | 1.14x |
| W5 | 2,180 MiB/s | 4,657 MiB/s | 1.97x |
| W6 | 3,101 MiB/s | 5,814 MiB/s | 1.63x |
| W7 | 677 MiB/s | 2,498 MiB/s | 3.30x |
| W8 | 1,440 MiB/s | 6,158 MiB/s | 4.17x |
| W9 | 644 MiB/s | 1,609 MiB/s | 2.32x |
| W10 | 1,051 MiB/s | 1,838 MiB/s | 1.76x |

### Linux, one thread (`--jobs 1`)

| Workload | 1.5.0 | 2.0 | Speedup |
|---|---:|---:|---:|
| W1 | 2,844 MiB/s | 2,764 MiB/s | 1.02x |
| W2 | 662 MiB/s | 693 MiB/s | 1.03x |
| W3 | 1,386 MiB/s | 1,521 MiB/s | 1.13x |
| W4 | 5,705 MiB/s | 5,521 MiB/s | 1.00x |
| W5 | 2,208 MiB/s | 2,240 MiB/s | 1.02x |
| W6 | 3,149 MiB/s | 3,144 MiB/s | 1.00x |
| W7 | 676 MiB/s | 898 MiB/s | 1.30x |
| W8 | 1,452 MiB/s | 1,604 MiB/s | 1.11x |
| W9 | 640 MiB/s | 651 MiB/s | 1.03x |
| W10 | 1,062 MiB/s | 1,852 MiB/s | 1.74x |

The fastest-run columns can disagree with the median ratio by a few
percent; the ratio is the comparison to trust.

### Windows, default settings

| Workload | 1.5.0 | 2.0 | Speedup |
|---|---:|---:|---:|
| W1 | 1,477 MiB/s | 2,768 MiB/s | 1.87x |
| W2 | 470 MiB/s | 1,234 MiB/s | 2.44x |
| W3 | 951 MiB/s | 1,783 MiB/s | 1.82x |
| W4 | 2,614 MiB/s | 3,594 MiB/s | 1.39x |
| W5 | 1,418 MiB/s | 2,919 MiB/s | 2.13x |
| W6 | 1,745 MiB/s | 3,121 MiB/s | 1.82x |
| W7 | 526 MiB/s | 2,629 MiB/s | 4.99x |
| W8 | 975 MiB/s | 4,417 MiB/s | 4.40x |
| W9 | 482 MiB/s | 2,231 MiB/s | 3.83x |
| W10 | 363 MiB/s | 1,058 MiB/s | 2.90x |

Windows writes output from one thread: NTFS zero-fills the gap whenever a
write lands past the end of a file's data, so workers cannot write their
chunks at their offsets as they do on Linux.

## Parallel scaling

Windows, `--jobs N` against `--jobs 1`:

| Workload | 2 | 4 | 6 | 8 | 12 |
|---|---:|---:|---:|---:|---:|
| W1 | 1.16x | 1.69x | 1.83x | 1.78x | 1.75x |
| W2 | 1.41x | 2.11x | 2.25x | 2.32x | 2.36x |
| W7 | 1.79x | 2.80x | 3.49x | 3.79x | 3.95x |
| W9 | 1.80x | 2.83x | 3.62x | 4.01x | 3.92x |

The CSV workloads are already near memory bandwidth on one core, so they
gain less; NDJSON parsing is heavier and scales further. A parallel run
holds at most `jobs + 2` chunks of a few MiB each.

## Other tools

Linux, same data and machine, 7 interleaved rounds, median time, output to
`/dev/shm`. Peak RSS is measured with GNU time. Versions: qsv 28.0.0 (the
glibc build, PGO and jemalloc), DuckDB 2.0.0, miller 6.11.0, jq 1.7.1.

Every tool gets the command that produces the same result: qsv uses `sqlp`
(multi-threaded Polars) or its conversion commands, DuckDB `COPY ... TO`,
miller and jq their usual pipelines. Results are parsed and compared value
by value; tools differ in quoting and number formatting, not in content.
DuckDB is given the NASA schema, since it would infer the mixed
numbers-and-`-` `bytes` column as JSON and write it back as JSON text.
`bench/bench_tools.py` holds the exact commands:

```
python3 bench/make_events.py big.csv events.ndjson
python3 bench/bench_tools.py --csv big.csv --ndjson big.ndjson \
        --small data/nasa_small.csv --events events.ndjson --out-dir /dev/shm -n 7
```

`events.ndjson` holds the same million rows as nested events, the shape
logging pipelines write (188 MiB):

```
{"time":"01/Jul/1995:00:00:01 -0400","client":{"host":"199.72.81.55"},
 "http":{"request":{"method":"GET","path":"/history/apollo/"},"status":200,"bytes":6245},"tags":[]}
```

**Filter and project, CSV to CSV** (`status == 200`, three columns)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 27 ms | 1.0x | 17 MiB |
| HyperPipe, one thread | 43 ms | 1.6x | 3 MiB |
| DuckDB | 127 ms | 4.7x | 271 MiB |
| qsv sqlp | 167 ms | 6.2x | 231 MiB |
| miller | 504 ms | 18.7x | 554 MiB |

**CSV to NDJSON**

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 97 ms | 1.0x | 33 MiB |
| HyperPipe, one thread | 170 ms | 1.8x | 3 MiB |
| DuckDB | 263 ms | 2.7x | 595 MiB |
| qsv tojsonl | 939 ms | 9.7x | 246 MiB |
| miller | 1,280 ms | 13.2x | 565 MiB |

**NDJSON to CSV**

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 66 ms | 1.0x | 27 MiB |
| HyperPipe, one thread | 192 ms | 2.9x | 3 MiB |
| DuckDB | 222 ms | 3.4x | 529 MiB |
| qsv jsonl | 349 ms | 5.3x | 62 MiB |
| jq | 3,228 ms | 49.0x | 4 MiB |
| miller | 3,857 ms | 58.6x | 137 MiB |

**NDJSON filter and project** (`status == 404`, two columns, NDJSON out)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 26 ms | 1.0x | 12 MiB |
| DuckDB | 99 ms | 3.8x | 231 MiB |
| HyperPipe, one thread | 106 ms | 4.1x | 3 MiB |
| jq | 1,403 ms | 53.8x | 4 MiB |
| miller | 3,870 ms | 148.4x | 137 MiB |

**Count matching rows** (`status == 404`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 17 ms | 1.0x | 11 MiB |
| HyperPipe, one thread | 23 ms | 1.4x | 2 MiB |
| qsv sqlp | 76 ms | 4.4x | 167 MiB |
| DuckDB | 91 ms | 5.3x | 156 MiB |
| miller | 490 ms | 28.6x | 535 MiB |

**Group by status: count and sum of bytes**

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 23 ms | 1.0x | 14 MiB |
| HyperPipe, one thread | 40 ms | 1.7x | 2 MiB |
| qsv sqlp | 91 ms | 3.9x | 183 MiB |
| DuckDB | 97 ms | 4.2x | 190 MiB |

miller's sum is 0 for a group without numbers (status 404, where `bytes`
is always `-`); SQL and HyperPipe leave it empty, so miller is not timed.

**Group by host: count** (many groups)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 21 ms | 1.0x | 20 MiB |
| HyperPipe, one thread | 36 ms | 1.7x | 2 MiB |
| qsv sqlp | 88 ms | 4.2x | 192 MiB |
| DuckDB | 91 ms | 4.3x | 194 MiB |
| miller | 511 ms | 24.3x | 534 MiB |

**NDJSON group by status: count and mean of bytes**

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 33 ms | 1.0x | 14 MiB |
| DuckDB | 108 ms | 3.3x | 231 MiB |
| HyperPipe, one thread | 127 ms | 3.8x | 3 MiB |
| miller | 3,922 ms | 118.8x | 140 MiB |

**Distinct hosts per status** (`--agg "count_distinct(host)"`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 34 ms | 1.0x | 21 MiB |
| HyperPipe, one thread | 58 ms | 1.7x | 3 MiB |
| qsv sqlp | 130 ms | 3.9x | 199 MiB |
| DuckDB | 139 ms | 4.1x | 194 MiB |
| miller | 720 ms | 21.4x | 542 MiB |

**p50, p95 and p99 of bytes per status** (`"bytes >= 0" --agg "p50(bytes), p95(bytes), p99(bytes)"`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 37 ms | 1.0x | 19 MiB |
| HyperPipe, one thread | 99 ms | 2.7x | 3 MiB |
| DuckDB | 144 ms | 3.9x | 242 MiB |
| qsv sqlp | 144 ms | 3.9x | 256 MiB |

The groups hold hundreds of thousands of values, so HyperPipe answers from
its sketch; DuckDB and qsv compute `quantile_cont` exactly, and the results
are compared within 2%. DuckDB 2.0 pushes the cast into its CSV reader, where
`TRY_CAST` fails on `-`, so the SQL filters those rows out first.

**Count per hour** (`--group-by "hour(timestamp)"`; the others parse the
common log format with `strptime` and print the same bucket text)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 32 ms | 1.0x | 14 MiB |
| HyperPipe, one thread | 82 ms | 2.6x | 3 MiB |
| qsv sqlp | 147 ms | 4.6x | 214 MiB |
| DuckDB | 154 ms | 4.9x | 185 MiB |
| miller | 5,592 ms | 176.4x | 542 MiB |

**Count in a time range** (`timestamp >= 1995-07-01T04:30Z and timestamp < 1995-07-01T05:00Z`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 37 ms | 1.0x | 11 MiB |
| HyperPipe, one thread | 112 ms | 3.1x | 3 MiB |
| DuckDB | 171 ms | 4.7x | 184 MiB |
| qsv sqlp | 235 ms | 6.4x | 210 MiB |
| miller | 3,093 ms | 84.6x | 572 MiB |

**Computed column** (`"bytes >= 0" --add "kb = bytes / 1024" --select host,kb`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 36 ms | 1.0x | 19 MiB |
| HyperPipe, one thread | 123 ms | 3.4x | 3 MiB |
| DuckDB | 126 ms | 3.5x | 259 MiB |
| qsv sqlp | 165 ms | 4.5x | 225 MiB |
| miller | 542 ms | 14.9x | 583 MiB |

**Nested NDJSON: filter on a path** (`"http.status == 404" --select time,client.host,http.request.path`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 76 ms | 1.0x | 13 MiB |
| DuckDB | 147 ms | 1.9x | 299 MiB |
| HyperPipe, one thread | 499 ms | 6.6x | 3 MiB |
| jq | 1,866 ms | 24.6x | 4 MiB |
| miller | 8,074 ms | 106.6x | 151 MiB |

**Nested NDJSON: group by a path** (`--group-by http.request.method --agg "count(http.bytes), sum(http.bytes)"`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 64 ms | 1.0x | 15 MiB |
| DuckDB | 130 ms | 2.0x | 296 MiB |
| HyperPipe, one thread | 395 ms | 6.2x | 3 MiB |

miller counts the rows where `bytes` is `null` as values and is left out.

**Sort by bytes, top 10** (`--sort -bytes,host,timestamp,request --head 10`)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 35 ms | 1.0x | 13 MiB |
| DuckDB | 111 ms | 3.2x | 156 MiB |
| HyperPipe, one thread | 120 ms | 3.4x | 3 MiB |
| qsv sqlp | 139 ms | 4.0x | 353 MiB |

**Sort all rows** (same columns, 1,000,000 rows out)

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 206 ms | 1.0x | 266 MiB |
| DuckDB | 401 ms | 1.9x | 678 MiB |
| qsv sqlp | 461 ms | 2.2x | 755 MiB |
| HyperPipe, one thread | 657 ms | 3.2x | 257 MiB |

The sort covers every column, so no two tools can break ties differently.
`bytes` holds `-` where nothing was sent: HyperPipe puts text after the
numbers, SQL gets the same order with `TRY_CAST(...) DESC NULLS LAST`, and
miller's `sort -nr` puts it first, so miller is not timed.

**Start-up: filter a 5,000-row file**

| Tool | Time | | Peak RSS |
|---|---:|---:|---:|
| HyperPipe | 2 ms | 1.0x | 2 MiB |
| miller | 7 ms | 4.5x | 17 MiB |
| qsv sqlp | 23 ms | 14.7x | 61 MiB |
| DuckDB | 39 ms | 24.5x | 51 MiB |

HyperPipe is the fastest on every task, by 2.7x to 150x, in a fraction of
the memory. On one thread it still beats the multi-threaded DuckDB and qsv
everywhere except NDJSON filtering, where DuckDB is 7% faster. DuckDB and
qsv do far more than this - SQL, joins, aggregation; these tasks are the
ones a stream tool is for.
