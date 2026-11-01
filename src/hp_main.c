#include "hyperpipe/hp_transcoder.h"
#include <stdio.h>
#include <string.h>

#define HP_VERSION "1.1.0"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "HyperPipe v%s - High-Performance Streaming Transcoder & Filter\n"
        "Usage: %s [FILE] [FILTER_EXPR] [OPTIONS]\n\n"
        "Positional Arguments:\n"
        "  FILE                      Input file path (default: stdin)\n"
        "  FILTER_EXPR               Natural predicate (e.g. \"status == 200 and bytes > 5000\")\n\n"
        "Options:\n"
        "  -i, --input <file>        Explicit input file path\n"
        "  -o, --output <file>       Output file path (default: stdout)\n"
        "  --from <auto|csv|tsv|ndjson> Input format (default: auto-sniff)\n"
        "  --to <csv|tsv|ndjson>     Output format (default: ndjson for csv/tsv, csv for ndjson)\n"
        "  --select <columns>        Comma-separated columns to project\n"
        "  --where <predicate>       Explicit filter predicate\n"
        "  --on-error <mode>         Fault tolerance: fail (default), skip, quarantine\n"
        "  --quarantine <file>       Destination file for quarantined malformed rows\n"
        "  --simd <mode>             SIMD engine: auto (default), avx512, avx2, scalar\n"
        "  --buffer-size <bytes>     Streaming ring buffer size (default: 65536)\n"
        "  --stats                   Output performance metrics to stderr\n"
        "  -v, --version             Display version and host CPU capabilities\n"
        "  -h, --help                Display this help message\n",
        HP_VERSION, prog);
}

static void print_version(void) {
    hp_cpu_caps_t caps = hp_detect_cpu_caps();
    printf("HyperPipe v%s (pure C11 vector engine)\n", HP_VERSION);
    printf("Host CPU Capabilities:\n");
    printf("  AVX-512 (F/BW/VL): %s (OS state: %s)\n",
           (caps.has_avx512f && caps.has_avx512bw && caps.has_avx512vl) ? "YES" : "NO",
           caps.os_avx512_supported ? "YES" : "NO");
    printf("  AVX2:             %s (OS state: %s)\n",
           caps.has_avx2 ? "YES" : "NO",
           caps.os_avx_supported ? "YES" : "NO");
    printf("  BMI2:             %s\n", caps.has_bmi2 ? "YES" : "NO");
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *output_path = NULL;
    const char *select_spec = NULL;
    const char *where_spec = NULL;
    const char *quarantine_path = NULL;
    hp_format_t in_fmt = HP_FORMAT_AUTO;
    hp_format_t out_fmt = HP_FORMAT_AUTO;
    hp_on_error_t on_error = HP_ON_ERROR_FAIL;
    hp_simd_mode_t simd_mode = HP_SIMD_AUTO;
    size_t buf_cap = HP_DEFAULT_BUFFER_CAPACITY;
    bool show_stats = false;
    bool user_specified_out_fmt = false;

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
            if (strcmp(argv[i], "auto") == 0) in_fmt = HP_FORMAT_AUTO;
            else if (strcmp(argv[i], "csv") == 0) in_fmt = HP_FORMAT_CSV;
            else if (strcmp(argv[i], "tsv") == 0) in_fmt = HP_FORMAT_TSV;
            else if (strcmp(argv[i], "ndjson") == 0) in_fmt = HP_FORMAT_NDJSON;
            else { fprintf(stderr, "Error: unknown input format: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--to") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            user_specified_out_fmt = true;
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
        } else if (strcmp(argv[i], "--on-error") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            if (strcmp(argv[i], "fail") == 0) on_error = HP_ON_ERROR_FAIL;
            else if (strcmp(argv[i], "skip") == 0) on_error = HP_ON_ERROR_SKIP;
            else if (strcmp(argv[i], "quarantine") == 0) on_error = HP_ON_ERROR_QUARANTINE;
            else { fprintf(stderr, "Error: unknown on-error mode: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--quarantine") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            quarantine_path = argv[i];
            on_error = HP_ON_ERROR_QUARANTINE;
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
        } else if (argv[i][0] != '-') {
            /* Positional argument detection: either filename or filter query */
            if (input_path == NULL && (strstr(argv[i], ".csv") || strstr(argv[i], ".tsv") ||
                                       strstr(argv[i], ".json") || strstr(argv[i], ".log") ||
                                       strstr(argv[i], ".txt") || fopen(argv[i], "r") != NULL)) {
                input_path = argv[i];
            } else if (where_spec == NULL) {
                where_spec = argv[i];
            } else if (input_path == NULL) {
                input_path = argv[i];
            } else {
                fprintf(stderr, "Error: unexpected argument: %s\n", argv[i]);
                return 1;
            }
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
            fprintf(stderr, "Error: failed to open output file '%s'\n", output_path);
            if (in_f != stdin) fclose(in_f);
            return 1;
        }
    }
    if (in_f) setvbuf(in_f, NULL, _IONBF, 0);
    if (out_f) setvbuf(out_f, NULL, _IONBF, 0);

    /* Configure default output format if not specified */
    if (!user_specified_out_fmt) {
        if (in_fmt == HP_FORMAT_NDJSON) {
            out_fmt = HP_FORMAT_CSV;
        } else {
            out_fmt = HP_FORMAT_NDJSON;
        }
    }

    hp_transcoder_config_t config = {
        .in_format = in_fmt,
        .out_format = out_fmt,
        .simd_mode = simd_mode,
        .on_error = on_error,
        .quarantine_path = quarantine_path,
        .select_spec = select_spec,
        .where_spec = where_spec,
        .buffer_capacity = buf_cap,
    };

    hp_transcoder_t transcoder;
    hp_error_t err = hp_transcoder_init(&transcoder, &config);
    if (err != HP_OK) {
        fprintf(stderr, "Error: failed to initialize transcoder (code %d)\n", err);
        if (in_f != stdin) fclose(in_f);
        if (out_f != stdout) fclose(out_f);
        return 1;
    }

    err = hp_transcoder_run(&transcoder, in_f, out_f);
    if (err != HP_OK) {
        fprintf(stderr, "Error: transcoding execution failed (code %d)\n", err);
    }

    if (show_stats) {
        double mib_in = (double)transcoder.stats.bytes_in / (1024.0 * 1024.0);
        double mib_out = (double)transcoder.stats.bytes_out / (1024.0 * 1024.0);
        double sec = transcoder.stats.elapsed_seconds > 0.000001 ? transcoder.stats.elapsed_seconds : 0.000001;
        fprintf(stderr, "\n=== HyperPipe Performance Profile ===\n");
        fprintf(stderr, "Processed: %llu rows (%.2f MiB in, %.2f MiB out)\n",
                (unsigned long long)transcoder.stats.rows_read, mib_in, mib_out);
        fprintf(stderr, "Emitted:   %llu rows\n", (unsigned long long)transcoder.stats.rows_emitted);
        if (transcoder.stats.rows_dropped > 0) {
            fprintf(stderr, "Dropped:   %llu rows (resynchronized via fault-tolerance)\n",
                    (unsigned long long)transcoder.stats.rows_dropped);
        }
        fprintf(stderr, "Elapsed:   %.4f s (%.2f MiB/s, %.3f Mrows/s)\n",
                sec, mib_in / sec, ((double)transcoder.stats.rows_read / 1e6) / sec);
    }

    hp_transcoder_free(&transcoder);
    if (in_f != stdin) fclose(in_f);
    if (out_f != stdout) fclose(out_f);

    return (err == HP_OK) ? 0 : 1;
}
