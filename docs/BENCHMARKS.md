# HyperPipe Benchmark & Hardcore Stress Scaling Report

## 1. Test Environment & Hardware Specification
- **Host CPU**: AMD Ryzen 5 7600X (Zen 4 Architecture, 6 Physical Cores / 12 Threads, 4.7 GHz base / up to 5.3 GHz boost, 32 MB L3 Cache)
- **Memory**: High-Speed DDR5 RAM
- **Vector Extensions**: AVX-512F, AVX-512BW, AVX-512DQ, AVX-512VL, AVX2, SSE4.2, BMI2
- **Operating System**: Microsoft Windows 11 Pro x86_64
- **Toolchains & Competitor Versions**:
  - **HyperPipe**: Native C11 compiled with MinGW-W64 GCC 16.2.0 (`-O3 -mavx2 -mavx512f -mavx512bw -mavx512vl`)
  - **GNU awk**: gawk 5.0.0 (MSYS2/Cygwin-based native Win32 port)
  - **Perl**: Strawberry Perl 5.38.2 x86_64
  - **Python**: CPython 3.15.0a0 optimized build
  - **PowerShell**: Windows PowerShell 5.1 / Core
- **Benchmark Task**: In-stream predicate filtering (`status == 200`) and field projection (`host,status,bytes`) on authentic server access log datasets.

---

## 2. Hardcore Scaling Matrix: Small vs Medium vs Datacenter Scale

The benchmark evaluates how throughput and memory scale when moving from small configuration files (5 KB - 0.5 MB) to medium corporate logs (183 MB), all the way to massive **Datacenter-scale** dumps (51+ million rows, ~5.18 GB).

```mermaid
flowchart LR
    subgraph SmallScale["Small Tier (5K rows / 0.5 MB)"]
        direction TB
        S_HP["HyperPipe: 41.2 MiB/s (0.012s)"]
        S_AWK["GNU awk: 17.5 MiB/s (0.028s)"]
        S_PERL["Perl: 16.6 MiB/s (0.030s)"]
        S_PY["Python: 13.9 MiB/s (0.036s)"]
    end

    subgraph MediumScale["Medium Tier (1.89M rows / 183 MB)"]
        direction TB
        M_HP["HyperPipe: 548.2 MiB/s (0.334s)"]
        M_PERL["Perl: 161.1 MiB/s (1.137s)"]
        M_AWK["GNU awk: 150.3 MiB/s (1.219s)"]
        M_PY["Python: 110.7 MiB/s (1.655s)"]
    end

    subgraph DatacenterScale["Datacenter Tier (51.07M rows / 5.18 GB)"]
        direction TB
        D_HP["HyperPipe: 573.8 MiB/s (8.620s)"]
        D_PERL["Perl: 150.8 MiB/s (32.80s)"]
        D_AWK["GNU awk: 133.4 MiB/s (37.08s)"]
        D_PY["Python: 94.4 MiB/s (52.41s)"]
    end

    SmallScale --> MediumScale --> DatacenterScale
```

### Tier 1: Small Scale (5,000 rows / 0.49 MiB)
*Typical for microservice health check dumps, cron alerts, or local config audit trails.*

| Tool | Elapsed Time | Throughput | Row Rate | Peak RSS RAM | Relative Speed |
|:---|:---:|:---:|:---:|:---:|:---:|
| **HyperPipe (AVX-512/AVX2)** | **0.012 s** | **41.22 MiB/s** | **0.417 Mrows/s** | **5.26 MB** | **1.0x (Baseline)** |
| **GNU awk (gawk 5.0)** | 0.028 s | 17.51 MiB/s | 0.177 Mrows/s | 10.22 MB | 2.4x slower |
| **Perl 5.38** | 0.030 s | 16.58 MiB/s | 0.168 Mrows/s | 9.66 MB | 2.5x slower |
| **Python 3.15 (csv)** | 0.036 s | 13.90 MiB/s | 0.141 Mrows/s | 12.32 MB | 3.0x slower |

---

### Tier 2: Medium Scale (1,891,714 rows / 183.18 MiB)
*Authentic NASA Kennedy Space Center HTTP server access log archive.*

| Tool | Elapsed Time | Throughput | Row Rate | Peak RSS RAM | Relative Speed |
|:---|:---:|:---:|:---:|:---:|:---:|
| **HyperPipe (AVX-512/AVX2)** | **0.334 s** | **548.18 MiB/s** | **5.661 Mrows/s** | **5.27 MB** | **1.0x (Baseline)** |
| **Perl 5.38** | 1.137 s | 161.12 MiB/s | 1.664 Mrows/s | 9.66 MB | 3.4x slower |
| **GNU awk (gawk 5.0)** | 1.219 s | 150.27 MiB/s | 1.552 Mrows/s | 10.21 MB | 3.6x slower |
| **Python 3.15 (csv)** | 1.655 s | 110.71 MiB/s | 1.143 Mrows/s | 12.34 MB | 5.0x slower |

---

### Tier 3: Datacenter Scale (51,076,278 rows / 4,945.81 MiB / ~5.18 GB)
*Simulated hyper-scale cloud datacenter cluster ingress access log dump.*

| Tool | Elapsed Time | Throughput | Row Rate | Peak RSS RAM | Relative Speed |
|:---|:---:|:---:|:---:|:---:|:---:|
| **HyperPipe (AVX-512/AVX2)** | **8.620 s** | **573.75 MiB/s** | **5.925 Mrows/s** | **5.25 MB** | **1.0x (Baseline)** |
| **Perl 5.38** | 32.797 s | 150.80 MiB/s | 1.557 Mrows/s | 9.66 MB | 3.8x slower |
| **GNU awk (gawk 5.0)** | 37.080 s | 133.38 MiB/s | 1.377 Mrows/s | 10.20 MB | 4.3x slower |
| **Python 3.15 (csv)** | 52.415 s | 94.36 MiB/s | 0.974 Mrows/s | 12.34 MB | 6.1x slower |

---

## 3. In-Depth Engineering Analysis & Findings

### A. The Scaling Differential: Small vs Datacenter Scale
1. **Small Data Behavior (0.5 MB, 5,000 rows)**:
   - On small inputs, runtime initialization costs (process creation, dynamic CRT linking, SIMD CPUID feature dispatch, and ring buffer allocation) constitute a noticeable fraction of total execution.
   - Even with initialization costs included, HyperPipe completes in **0.012 s**, outperforming GNU awk (0.028 s), Perl (0.030 s), and Python (0.036 s) by **2.4x – 3.0x**.
2. **Datacenter Scale Behavior (5.18 GB, 51+ Million rows)**:
   - On multi-gigabyte streams, constant initialization overhead drops to `< 0.05%` of runtime.
   - The SIMD vector parsing pipeline operates at peak steady-state throughput: **573.75 MiB/s**, scanning and projecting **~5.93 million rows per second**.
   - HyperPipe finishes the entire 5.18 GB dataset in **8.62 seconds**.
   - Competitors exhibit severe degradation on large streams:
     - Perl takes **32.80 s** (3.8x slower) due to per-string reallocation in regex splits.
     - GNU awk takes **37.08 s** (4.3x slower) constrained by single-byte scalar character scanning and dynamic record buffers.
     - Python 3.15 takes **52.41 s** (6.1x slower) due to CPython bytecode evaluation overhead and object boxing per field.

### B. True $O(1)$ Memory Invariance
One of the most critical findings of this benchmark is memory stability across three orders of magnitude:

| Dataset Size | Record Count | HyperPipe Peak RSS | Perl Peak RSS | GNU awk Peak RSS | Python Peak RSS |
|:---|:---:|:---:|:---:|:---:|:---:|
| **0.49 MiB** | 5,000 | **5.26 MB** | 9.66 MB | 10.22 MB | 12.32 MB |
| **183.18 MiB** | 1,891,714 | **5.27 MB** | 9.66 MB | 10.21 MB | 12.34 MB |
| **4,945.81 MiB (~5.18 GB)** | 51,076,278 | **5.25 MB** | 9.66 MB | 10.20 MB | 12.34 MB |

```mermaid
flowchart TD
    subgraph SlidingWindowMemory["HyperPipe O(1) Sliding Ring Buffer"]
        direction LR
        Alloc["Fixed 4 MiB Ring Buffer Allocated Once at Startup"]
        StreamIn["Zero-Copy Direct File/Pipe Read (fread/read)"]
        SIMD["AVX-512/AVX2 64-byte Vector Scanning"]
        StreamOut["Buffered Output Write (fwrite)"]
        Reclaim["Pointer Advance without Allocation"]

        Alloc --> StreamIn --> SIMD --> StreamOut --> Reclaim --> StreamIn
    end
```

- HyperPipe’s memory footprint is strictly decoupled from the volume of data processed. Because records are parsed in-place within a fixed sliding ring buffer and written directly to standard output or downstream pipes, memory consumption never grows.
- A 5.18 GB stream is ingested, validated, filtered, and projected while consuming a modest **5.25 MB** of RAM.

### C. Enterprise Fault-Tolerance in Streaming Pipelines
On large datacenter logs, malformed lines or corrupted boundaries inevitably occur (e.g. truncated network transfers, unescaped multiline exceptions, or concatenated archives).
- Traditional command-line tools often fail silently, print partial garbage, or abort entirely.
- HyperPipe provides explicit error recovery flags:
  - `--on-error skip`: Upon encountering an unescaped quote or malformed delimiter, the parser executes a forward SIMD scan for the next newline character (`\n`) and resynchronizes the stream in `< 1 microsecond`.
  - `--on-error quarantine`: Diverts defective records to a designated error stream while maintaining maximum processing throughput for valid records.
