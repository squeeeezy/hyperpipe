#include "hyperpipe/hp_transcoder.h"
#include "hyperpipe/hp_parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
#endif

#define HP_VERSION "2.0.0"

/* The Windows CRT opens stdin and stdout in text mode: output "\n" becomes
 * "\r\n" and input stops at the first 0x1A byte, which breaks byte-exact
 * output and gzip streams on pipes. */
static void use_binary_stdio(void) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void print_usage(void) {
    fputs(
        "usage: hyperpipe [FILE] [FILTER] [options]\n"
        "\n"
        "Filter, project and convert CSV, TSV and NDJSON. Reads FILE or stdin;\n"
        "the input format is detected and gzip input is decompressed.\n"
        "\n"
        "  FILTER                    e.g. \"status == 200 and bytes > 5000\"\n"
        "  -i, --input FILE          input file\n"
        "  -o, --output FILE         output file (default stdout; .gz compresses)\n"
        "  --from auto|csv|tsv|ndjson  input format (default auto)\n"
        "  --to csv|tsv|ndjson       output format (default: NDJSON for CSV/TSV\n"
        "                            input, CSV for NDJSON input)\n"
        "  --select COLUMNS          comma-separated columns to output\n"
        "  --where FILTER            filter, when not given as an argument\n"
        "  --count                   print the number of matching rows\n"
        "  --head N, --limit N       stop after N rows\n"
        "  --summary                 print per-column statistics instead of rows\n"
        "  --schema FILE             column types, one \"name,int|string\" per line\n"
        "  --jobs N|auto             threads for file input (default auto: one per\n"
        "                            processor for files of 16 MiB or more)\n"
        "  --gzip                    compress the output (needs zlib at run time)\n"
        "  --on-error fail|skip|quarantine  malformed records (default fail)\n"
        "  --quarantine FILE         where quarantined records go\n"
        "  --simd auto|avx512|avx2|scalar   vector engine (default auto)\n"
        "  --buffer-size BYTES       input buffer, the largest record (default 524288)\n"
        "  --stats                   print throughput to stderr\n"
        "  -v, --version             print version and CPU features\n"
        "  -h, --help                print this help\n",
        stderr);
}

static void print_version(void) {
    hp_cpu_caps_t caps = hp_detect_cpu_caps();
    bool avx512 = caps.has_avx512f && caps.has_avx512bw && caps.has_avx512vl && caps.os_avx512_supported;
    bool avx2 = caps.has_avx2 && caps.os_avx_supported;
    printf("hyperpipe %s\n", HP_VERSION);
    printf("cpu: avx512 %s, avx2 %s, bmi2 %s\n",
           avx512 ? "yes" : "no", avx2 ? "yes" : "no", caps.has_bmi2 ? "yes" : "no");
}

static void print_perf_stats(const hp_transcoder_stats_t *st) {
    double mib_in = (double)st->bytes_in / (1024.0 * 1024.0);
    double mib_out = (double)st->bytes_out / (1024.0 * 1024.0);
    double sec = st->elapsed_seconds > 1e-6 ? st->elapsed_seconds : 1e-6;
    fprintf(stderr, "rows: %llu read, %llu written, %llu dropped\n",
            (unsigned long long)st->rows_read, (unsigned long long)st->rows_emitted,
            (unsigned long long)st->rows_dropped);
    fprintf(stderr, "data: %.2f MiB in, %.2f MiB out\n", mib_in, mib_out);
    fprintf(stderr, "time: %.4f s, %.0f MiB/s, %.2f M rows/s\n",
            sec, mib_in / sec, (double)st->rows_read / 1e6 / sec);
}

int main(int argc, char **argv) {
    use_binary_stdio();

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
    int jobs = 0;              /* 0: auto */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
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
            if (strcmp(argv[i], "auto") == 0) {
                jobs = 0;
            } else if (end == argv[i] || *end != '\0' || n < 1) {
                fprintf(stderr, "Error: invalid --jobs value: %s\n", argv[i]);
                return 1;
            } else {
                jobs = (int)n;
            }
        } else if (strcmp(argv[i], "--simd") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            if (strcmp(argv[i], "auto") == 0) simd_mode = HP_SIMD_AUTO;
            else if (strcmp(argv[i], "avx512") == 0) simd_mode = HP_SIMD_AVX512;
            else if (strcmp(argv[i], "avx2") == 0) simd_mode = HP_SIMD_AVX2;
            else if (strcmp(argv[i], "scalar") == 0) simd_mode = HP_SIMD_SCALAR;
            else { fprintf(stderr, "Error: unknown SIMD mode: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--buffer-size") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: missing argument for %s\n", argv[i-1]); return 1; }
            char *end = NULL;
            unsigned long long n = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || n == 0) {
                fprintf(stderr, "Error: invalid --buffer-size value: %s\n", argv[i]);
                return 1;
            }
            buf_cap = (size_t)n;
        } else if (strcmp(argv[i], "--stats") == 0) {
            show_stats = true;
        } else if (argv[i][0] != '-') {
            /* Positional argument detection: either filename or filter query */
            if (input_path == NULL && (strstr(argv[i], ".csv") || strstr(argv[i], ".tsv") ||
                                       strstr(argv[i], ".json") || strstr(argv[i], ".log") ||
                                       strstr(argv[i], ".txt") || file_exists(argv[i]))) {
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

    /* Auto mode only picks the parallel path where it applies and pays off;
     * an explicit --jobs N reports what keeps it from applying. */
    const bool jobs_explicit = (jobs != 0);
    if (!jobs_explicit) {
        jobs = (input_path && limit_rows == 0 && !summary_mode) ? hp_parallel_auto_jobs(input_path) : 1;
    }

    if (jobs > 1 && jobs_explicit) {
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
            /* gzip input cannot be split, CSV skip/quarantine runs need the
             * sequential resynchronization, and an unparsable first record
             * belongs to the fault-tolerance mode: run single-threaded. */
            if (jobs_explicit) {
                fprintf(stderr, "Note: --jobs does not apply to this input "
                                "(gzip, CSV with --on-error skip/quarantine, or unparsable first record); "
                                "running single-threaded\n");
            }
        } else {
            ran_parallel = true;
        }
    }

    if (!ran_parallel) {
        hp_transcoder_t transcoder;
        err = hp_transcoder_init(&transcoder, &config);
        if (err != HP_OK) {
            fprintf(stderr, "hyperpipe: %s\n", hp_strerror(err));
            if (in_f != stdin) fclose(in_f);
            if (out_f != stdout) fclose(out_f);
            return 1;
        }

        err = hp_transcoder_run(&transcoder, in_f, out_f);

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

    if (err != HP_OK) fprintf(stderr, "hyperpipe: %s\n", hp_strerror(err));

    if (in_f != stdin) fclose(in_f);
    if (out_f != stdout) fclose(out_f);

    return (err == HP_OK) ? 0 : 1;
}
