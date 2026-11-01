#include "hyperpipe/hp_transcoder.h"
#include <stdio.h>
#include <string.h>

#define HP_VERSION "1.0.0"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "HyperPipe v%s - High-Performance Streaming Transcoder\n"
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  -i, --input <file>        Input file path (default: stdin)\n"
        "  -o, --output <file>       Output file path (default: stdout)\n"
        "  --from <csv|tsv|ndjson>   Input format (default: csv)\n"
        "  --to <csv|tsv|ndjson>     Output format (default: ndjson)\n"
        "  --select <columns>        Comma-separated columns to project\n"
        "  --where <predicate>       Predicate filter (e.g. \"age >= 18, status == active\")\n"
        "  --simd <mode>             SIMD mode: auto, avx512, avx2, scalar (default: auto)\n"
        "  --buffer-size <bytes>     Streaming ring buffer size (default: 65536)\n"
        "  --stats                   Output performance statistics to stderr\n"
        "  -v, --version             Display version and host CPU capabilities\n"
        "  -h, --help                Display this help message\n",
        HP_VERSION, prog);
}

static void print_version(void) {
    hp_cpu_caps_t caps = hp_detect_cpu_caps();
    printf("HyperPipe v%s (pure C11 SIMD engine)\n", HP_VERSION);
    printf("Host CPU Capabilities:\n");
    printf("  AVX-512 (F/BW/VL): %s (OS support: %s)\n",
           (caps.has_avx512f && caps.has_avx512bw && caps.has_avx512vl) ? "YES" : "NO",
           caps.os_avx512_supported ? "YES" : "NO");
    printf("  AVX2:             %s (OS support: %s)\n",
           caps.has_avx2 ? "YES" : "NO",
           caps.os_avx_supported ? "YES" : "NO");
    printf("  BMI2:             %s\n", caps.has_bmi2 ? "YES" : "NO");
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *output_path = NULL;
    const char *select_spec = NULL;
    const char *where_spec = NULL;
    hp_format_t in_fmt = HP_FORMAT_CSV;
    hp_format_t out_fmt = HP_FORMAT_NDJSON;
    hp_simd_mode_t simd_mode = HP_SIMD_AUTO;
    size_t buf_cap = HP_DEFAULT_BUFFER_CAPACITY;
    bool show_stats = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            input_path = argv[i];
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            output_path = argv[i];
        } else if (strcmp(argv[i], "--from") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            if (strcmp(argv[i], "csv") == 0) in_fmt = HP_FORMAT_CSV;
            else if (strcmp(argv[i], "tsv") == 0) in_fmt = HP_FORMAT_TSV;
            else if (strcmp(argv[i], "ndjson") == 0) in_fmt = HP_FORMAT_NDJSON;
            else { fprintf(stderr, "Error: unknown input format: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--to") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            if (strcmp(argv[i], "csv") == 0) out_fmt = HP_FORMAT_CSV;
            else if (strcmp(argv[i], "tsv") == 0) out_fmt = HP_FORMAT_TSV;
            else if (strcmp(argv[i], "ndjson") == 0) out_fmt = HP_FORMAT_NDJSON;
            else { fprintf(stderr, "Error: unknown output format: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--select") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            select_spec = argv[i];
        } else if (strcmp(argv[i], "--where") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            where_spec = argv[i];
        } else if (strcmp(argv[i], "--simd") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            if (strcmp(argv[i], "auto") == 0) simd_mode = HP_SIMD_AUTO;
            else if (strcmp(argv[i], "avx512") == 0) simd_mode = HP_SIMD_AVX512;
            else if (strcmp(argv[i], "avx2") == 0) simd_mode = HP_SIMD_AVX2;
            else if (strcmp(argv[i], "scalar") == 0) simd_mode = HP_SIMD_SCALAR;
            else { fprintf(stderr, "Error: unknown SIMD mode: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--buffer-size") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            buf_cap = (size_t)strtoull(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--stats") == 0) {
            show_stats = true;
        } else {
            fprintf(stderr, "Error: unrecognized option: %s\n", argv[i]);
            return 1;
        }
    }

    FILE *in_f = stdin;
    if (input_path) {
        in_f = fopen(input_path, "rb");
        if (!in_f) {
            fprintf(stderr, "Error: failed to open input file '%s'\n", input_path);
            return 1;
        }
    }

    FILE *out_f = stdout;
    if (output_path) {
        out_f = fopen(output_path, "wb");
        if (!out_f) {
            if (in_f != stdin) fclose(in_f);
            fprintf(stderr, "Error: failed to open output file '%s'\n", output_path);
            return 1;
        }
    }

    hp_transcoder_config_t cfg = {
        .in_format = in_fmt,
        .out_format = out_fmt,
        .simd_mode = simd_mode,
        .select_spec = select_spec,
        .where_spec = where_spec,
        .buffer_capacity = buf_cap
    };

    hp_transcoder_t tc;
    hp_error_t err = hp_transcoder_init(&tc, &cfg);
    if (err != HP_OK) {
        fprintf(stderr, "Error: transcoder initialization failed (%d)\n", err);
        if (in_f != stdin) fclose(in_f);
        if (out_f != stdout) fclose(out_f);
        return 1;
    }

    err = hp_transcoder_run(&tc, in_f, out_f);
    if (err != HP_OK) {
        fprintf(stderr, "Error: transcoding failed with error code %d\n", err);
    }

    if (show_stats) {
        double mib_in = (double)tc.stats.bytes_in / (1024.0 * 1024.0);
        double mib_out = (double)tc.stats.bytes_out / (1024.0 * 1024.0);
        double throughput_mibs = tc.stats.elapsed_seconds > 0.0 ? (mib_in / tc.stats.elapsed_seconds) : 0.0;
        double throughput_gbs = throughput_mibs / 1024.0;
        double mrows_sec = tc.stats.elapsed_seconds > 0.0 ? ((double)tc.stats.rows_read / 1e6 / tc.stats.elapsed_seconds) : 0.0;

        fprintf(stderr, "\n--- HyperPipe Execution Statistics ---\n");
        fprintf(stderr, "Active SIMD kernel: %s\n", g_hp_dispatch.active_mode_name);
        fprintf(stderr, "Rows processed:     %llu (emitted: %llu)\n",
                (unsigned long long)tc.stats.rows_read, (unsigned long long)tc.stats.rows_emitted);
        fprintf(stderr, "Input data:         %.2f MiB (Output: %.2f MiB)\n", mib_in, mib_out);
        fprintf(stderr, "Elapsed time:       %.4f s\n", tc.stats.elapsed_seconds);
        fprintf(stderr, "Throughput:         %.2f MiB/s (%.3f GB/s)\n", throughput_mibs, throughput_gbs);
        fprintf(stderr, "Row rate:           %.3f Million rows/s\n", mrows_sec);
        fprintf(stderr, "--------------------------------------\n");
    }

    hp_transcoder_free(&tc);
    if (in_f != stdin) fclose(in_f);
    if (out_f != stdout) fclose(out_f);

    return (err == HP_OK) ? 0 : 1;
}
