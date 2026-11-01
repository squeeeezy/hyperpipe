#include "hyperpipe/hp_transcoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <psapi.h>
    static size_t get_peak_rss_bytes(void) {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return (size_t)pmc.PeakWorkingSetSize;
        }
        return 0;
    }
#else
    #include <sys/resource.h>
    static size_t get_peak_rss_bytes(void) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (size_t)ru.ru_maxrss * 1024;
    }
#endif

static void generate_test_csv(const char *filename, size_t row_count) {
    printf("[bench] Generating %zu rows synthetic dataset in '%s'...\n", row_count, filename);
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Failed to create bench dataset\n");
        exit(1);
    }

    fprintf(f, "id,timestamp,ip,endpoint,status,response_ms\n");
    for (size_t i = 1; i <= row_count; i++) {
        fprintf(f, "%zu,2026-11-01T23:59:59Z,192.168.1.%u,/api/v1/resource/%zu,%u,%.2f\n",
                i, (unsigned)(i % 254 + 1), i % 100, (unsigned)(200 + (i % 5 == 0 ? 300 : 0)), (double)(10.0 + (i % 250)));
    }
    fclose(f);
}

static void run_benchmark_run(const char *csv_file, hp_simd_mode_t simd, const char *label) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = simd,
        .buffer_capacity = 64 * 1024
    };

    hp_transcoder_t tc;
    if (hp_transcoder_init(&tc, &cfg) != HP_OK) {
        fprintf(stderr, "Init failed for %s\n", label);
        return;
    }

    FILE *in_f = fopen(csv_file, "rb");
    FILE *out_f = fopen("bench_out.tmp", "wb");
    if (!in_f || !out_f) {
        if (in_f) fclose(in_f);
        if (out_f) fclose(out_f);
        hp_transcoder_free(&tc);
        return;
    }

    hp_error_t err = hp_transcoder_run(&tc, in_f, out_f);
    fclose(in_f);
    fclose(out_f);
    remove("bench_out.tmp");

    if (err != HP_OK) {
        fprintf(stderr, "Run failed for %s with code %d\n", label, err);
        hp_transcoder_free(&tc);
        return;
    }

    double mib_in = (double)tc.stats.bytes_in / (1024.0 * 1024.0);
    double throughput_mibs = tc.stats.elapsed_seconds > 0.0 ? (mib_in / tc.stats.elapsed_seconds) : 0.0;
    double throughput_gbs = throughput_mibs / 1024.0;
    double mrows_sec = tc.stats.elapsed_seconds > 0.0 ? ((double)tc.stats.rows_read / 1e6 / tc.stats.elapsed_seconds) : 0.0;
    size_t peak_rss = get_peak_rss_bytes();
    double peak_rss_mb = (double)peak_rss / (1024.0 * 1024.0);

    printf("  %-12s | %7.2f s | %8.2f MiB/s | %6.3f GB/s | %6.3f Mrows/s | %6.2f MB RSS\n",
           label, tc.stats.elapsed_seconds, throughput_mibs, throughput_gbs, mrows_sec, peak_rss_mb);

    hp_transcoder_free(&tc);
}

int main(int argc, char **argv) {
    size_t rows = 500000; /* 500k rows default for quick run, user can supply N */
    if (argc > 1) {
        rows = (size_t)strtoull(argv[1], NULL, 10);
    }

    const char *test_csv = "bench_input.csv";
    generate_test_csv(test_csv, rows);

    printf("\n=========================================================================================\n");
    printf(" HyperPipe SIMD Streaming Transcoder Benchmark (AMD Zen 4 Ryzen 5 7600X)\n");
    printf("=========================================================================================\n");
    printf("  Kernel       |  Elapsed  |   Throughput  |   Throughput |    Row Rate   | Peak Memory RSS\n");
    printf("---------------+-----------+---------------+--------------+---------------+----------------\n");

    run_benchmark_run(test_csv, HP_SIMD_SCALAR, "Scalar");
    run_benchmark_run(test_csv, HP_SIMD_AVX2,   "AVX2");
    run_benchmark_run(test_csv, HP_SIMD_AVX512, "AVX-512");

    printf("=========================================================================================\n");
    remove(test_csv);
    return 0;
}
