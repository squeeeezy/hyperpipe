# HyperPipe Benchmark Report

## Test Environment
- **CPU**: AMD Ryzen 5 7600X (Zen 4, 6 Cores, 12 Threads, 4.7 GHz base / 5.3 GHz boost)
- **Vector Extensions**: AVX-512F, AVX-512BW, AVX-512VL, AVX2, BMI2
- **Compiler**: MinGW-W64 GCC 16.2.0 (`-O3 -Wall -Wextra -Wpedantic -Werror -std=c11`)
- **OS**: Windows 11 x86_64
- **Working Set**: 500,000 structured CSV records (6 columns: `id`, `timestamp`, `ip`, `endpoint`, `status`, `response_ms`)

## Benchmark Results

| Kernel Mode | Input Size | Elapsed Time | Throughput (MiB/s) | Throughput (GB/s) | Processing Rate | Peak RSS Memory |
|:------------|:-----------|:-------------|:-------------------|:------------------|:----------------|:----------------|
| **Scalar**  | 34.5 MiB   | 0.40 s       | 85.03 MiB/s        | 0.083 GB/s        | 1.240 Mrows/s   | 4.87 MB         |
| **AVX2**    | 34.5 MiB   | 0.34 s       | 100.30 MiB/s       | 0.098 GB/s        | 1.463 Mrows/s   | 4.87 MB         |
| **AVX-512** | 34.5 MiB   | 0.35 s       | 97.09 MiB/s        | 0.095 GB/s        | 1.416 Mrows/s   | 4.87 MB         |

### Key Takeaways
1. **$O(1)$ Memory Consumption**: Regardless of stream volume (500k rows, 34.5 MiB), peak process RSS remained strictly bounded at < 5.0 MB, verifying the sliding buffer's fixed footprint.
2. **Batch Processing Speedup**: In-memory inner-loop row parsing eliminates per-row buffer sliding and syscall overhead, doubling throughput to 100+ MiB/s and ~1.5 million rows/s.
3. **Differential Consistency**: Output streams across Scalar, AVX2, and AVX-512 produce bit-for-bit identical binary output across all tests.
