#include "hyperpipe/hp_transcoder.h"
#include "hyperpipe/hp_parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HP_VERSION "1.5.0"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "HyperPipe v%s - High-Performance Streaming Transcoder & Filter\n"
        "Usage: %s [FILE] [FILTER_EXPR] [OPTIONS]\n\n"
        "Positional Arguments:\n"
        "  FILE                      Input file path (default: stdin; .gz is transparently decompressed)\n"
        "  FILTER_EXPR               Natural predicate (e.g. \"status == 200 and bytes > 5000\")\n\n"
        "Options:\n"
        "  -i, --input <file>        Explicit input file path\n"
        "  -o, --output <file>       Output file path (default: stdout; a .gz suffix enables gzip)\n"
        "  --from <auto|csv|tsv|ndjson> Input format (default: auto-sniff)\n"
        "  --to <csv|tsv|ndjson>     Output format (default: ndjson for csv/tsv, csv for ndjson)\n"
        "  --select <columns>        Comma-separated columns to project\n"
        "  --where <predicate>       Explicit filter predicate\n"
        "  --count                   Print the number of matching rows to stdout (no row output)\n"
        "  --head <N>, --limit <N>   Stop after N emitted rows\n"
        "  --summary                 Print per-column statistics (type, present, empty,\n"
        "                            min/max, mean) instead of row output\n"
        "  --schema <file>           Declared column types, one \"name,type\" per line\n"
        "                            (int hints accelerate numeric filter predicates)\n"
        "  --jobs <N>                Parallel chunked pipeline for plain CSV/TSV/NDJSON files\n"
        "                            input (2-64); other inputs run single-threaded\n"
        "  --gzip                    Compress the output with gzip (libz is loaded at runtime)\n"
        "  --on-error <mode>         Fault tolerance: fail (default), skip, quarantine\n"
        "  --quarantine <file>       Destination file for quarantined malformed rows\n"
        "  --simd <mode>             SIMD engine: auto (default), avx512, avx2, scalar\n"
        "  --buffer-size <bytes>     Streaming ring buffer size (default: 524288)\n"
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

static void print_perf_stats(const hp_transcoder_stats_t *st) {
    double mib_in = (double)st->bytes_in / (1024.0 * 1024.0);
    double mib_out = (double)st->bytes_out / (1024.0 * 1024.0);
    double sec = st->elapsed_seconds > 0.000001 ? st->elapsed_seconds : 0.000001;
    fprintf(stderr, "\n=== HyperPipe Performance Profile ===\n");
    fprintf(stderr, "Processed: %llu rows (%.2f MiB in, %.2f MiB out)\n",
            (unsigned long long)st->rows_read, mib_in, mib_out);
    fprintf(stderr, "Emitted:   %llu rows\n", (unsigned long long)st->rows_emitted);
    if (st->rows_dropped > 0) {
        fprintf(stderr, "Dropped:   %llu rows (resynchronized via fault-tolerance)\n",
                (unsigned long long)st->rows_dropped);
    }
    fprintf(stderr, "Elapsed:   %.4f s (%.2f MiB/s, %.3f Mrows/s)\n",
            sec, mib_in / sec, ((double)st->rows_read / 1e6) / sec);
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
    uint64_t limit_rows = 0;
    bool show_stats = false;
    bool count_only = false;
    bool gzip_out = false;
    bool summary_mode = false;
    const char *schema_path = NULL;
    int jobs = 0;

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
        } else if (strcmp(argv[i], "--head") == 0 || strcmp(argv[i], "--limit") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            char *end = NULL;
            limit_rows = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0') {
                fprintf(stderr, "Error: invalid row limit: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--count") == 0) {
            count_only = true;
        } else if (strcmp(argv[i], "--gzip") == 0) {
            gzip_out = true;
        } else if (strcmp(argv[i], "--summary") == 0) {
            summary_mode = true;
        } else if (strcmp(argv[i], "--schema") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            schema_path = argv[i];
        } else if (strcmp(argv[i], "--jobs") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            char *end = NULL;
            long n = strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || n < 0) {
                fprintf(stderr, "Error: invalid --jobs value: %s\n", argv[i]);
                return 1;
            }
            jobs = (int)n;
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

    /* A .gz output suffix implies --gzip regardless of option order */
    if (output_path) {
        size_t olen = strlen(output_path);
        if (olen >= 3 && output_path[olen-1] == 'z' && output_path[olen-2] == 'g' && output_path[olen-3] == '.') {
            gzip_out = true;
        }
    }

    if (count_only && output_path) {
        fprintf(stderr, "Error: --count prints the row count to stdout and produces no row output; omit -o/--output\n");
        return 1;
    }

    if (summary_mode && (count_only || output_path || gzip_out)) {
        fprintf(stderr, "Error: --summary prints its statistics table to stdout; it cannot be combined with --count, -o/--output or --gzip\n");
        return 1;
    }

    if (jobs > 1) {
        if (!input_path) {
            fprintf(stderr, "Error: --jobs requires a regular input file (stdin is not seekable)\n");
            return 1;
        }
        if (limit_rows > 0) {
            fprintf(stderr, "Error: --head/--limit is not supported with --jobs\n");
            return 1;
        }
        if (summary_mode) {
            fprintf(stderr, "Error: --summary is not supported with --jobs\n");
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

    /* Default output format is resolved inside hp_transcoder_run() AFTER the
     * input format has been sniffed: NDJSON input -> CSV output, everything
     * else -> NDJSON output. Resolving it here (before sniffing) would freeze
     * the output format on the CLI value instead of the detected one. */

    hp_transcoder_config_t config = {
        .in_format = in_fmt,
        .out_format = out_fmt,
        .simd_mode = simd_mode,
        .on_error = on_error,
        .quarantine_path = quarantine_path,
        .select_spec = select_spec,
        .where_spec = where_spec,
        .buffer_capacity = buf_cap,
        .limit_rows = limit_rows,
        .count_only = count_only,
        .gzip_out = gzip_out,
        .summary = summary_mode,
        .schema_path = schema_path,
    };

    hp_error_t err = HP_OK;
    bool ran_parallel = false;
    hp_transcoder_stats_t parallel_stats;
    memset(&parallel_stats, 0, sizeof(parallel_stats));

    if (jobs > 1) {
        err = hp_parallel_run(&config, input_path, out_f, jobs, &parallel_stats);
        if (err == HP_ERR_UNSUPPORTED) {
            /* Compressed input cannot be seek-split by workers; a first
             * line the reference parsers reject may need the
             * single-stream fault-tolerance mode. Either way the
             * single-threaded pipeline owns the run. */
            fprintf(stderr, "Note: --jobs fell back to the single-threaded pipeline (compressed input or unparsable first line)\n");
        } else {
            ran_parallel = true;
        }
    }

    if (!ran_parallel) {
        hp_transcoder_t transcoder;
        err = hp_transcoder_init(&transcoder, &config);
        if (err != HP_OK) {
            fprintf(stderr, "Error: failed to initialize transcoder (code %d)\n", err);
            if (in_f != stdin) fclose(in_f);
            if (out_f != stdout) fclose(out_f);
            return 1;
        }

        err = hp_transcoder_run(&transcoder, in_f, out_f);
        if (err != HP_OK) {
            if (err == HP_ERR_GZIP_UNAVAILABLE) {
                fprintf(stderr, "Error: gzip support requires libz, which was not found on this system\n");
            } else {
                fprintf(stderr, "Error: transcoding execution failed (code %d)\n", err);
            }
        }

        if (count_only) {
            printf("%llu\n", (unsigned long long)transcoder.stats.rows_emitted);
        }

        if (show_stats) {
            print_perf_stats(&transcoder.stats);
        }

        hp_transcoder_free(&transcoder);
    } else {
        if (count_only) {
            printf("%llu\n", (unsigned long long)parallel_stats.rows_emitted);
        }

        if (show_stats) {
            print_perf_stats(&parallel_stats);
        }
    }

    if (in_f != stdin) fclose(in_f);
    if (out_f != stdout) fclose(out_f);

    return (err == HP_OK) ? 0 : 1;
}
