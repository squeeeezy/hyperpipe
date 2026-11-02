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

Not measured yet for 2.0. The comparison will use miller, jq, qsv, DuckDB
and awk on the same data and machine, each with the command that produces
the same output.
