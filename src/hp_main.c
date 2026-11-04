#include "hyperpipe/hp_transcoder.h"
#include "hyperpipe/hp_parallel.h"
#include <errno.h>
#include <stdarg.h>
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
        "  --group-by COLUMNS        one output row per distinct value of COLUMNS\n"
        "  --agg FUNCTIONS           per group: count, count(col), sum(col),\n"
        "                            avg(col), min(col), max(col) (default count)\n"
        "  --sort COLUMNS            sort rows, or groups, by COLUMNS; -col descends\n"
        "  --memory SIZE             memory for sorting before it uses temporary\n"
        "                            files, e.g. 2G (default 512M)\n"
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
        stdout);
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

static void complain(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("hyperpipe: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- command line ---------------------------------------------------------- */

typedef struct options_s {
    hp_transcoder_config_t cfg;
    const char *input_path;
    const char *output_path;
    int jobs;                  /* 0: auto */
    bool show_stats;
} options_t;

typedef struct choice_s {
    const char *name;
    int value;
} choice_t;

static const choice_t in_formats[] = {
    { "auto", HP_FORMAT_AUTO }, { "csv", HP_FORMAT_CSV }, { "tsv", HP_FORMAT_TSV },
    { "ndjson", HP_FORMAT_NDJSON }, { NULL, 0 },
};
static const choice_t out_formats[] = {
    { "csv", HP_FORMAT_CSV }, { "tsv", HP_FORMAT_TSV }, { "ndjson", HP_FORMAT_NDJSON },
    { NULL, 0 },
};
static const choice_t error_modes[] = {
    { "fail", HP_ON_ERROR_FAIL }, { "skip", HP_ON_ERROR_SKIP },
    { "quarantine", HP_ON_ERROR_QUARANTINE }, { NULL, 0 },
};
static const choice_t simd_modes[] = {
    { "auto", HP_SIMD_AUTO }, { "avx512", HP_SIMD_AVX512 }, { "avx2", HP_SIMD_AVX2 },
    { "scalar", HP_SIMD_SCALAR }, { NULL, 0 },
};

static bool is_option(const char *arg, const char *short_name, const char *long_name) {
    return (short_name && strcmp(arg, short_name) == 0) || strcmp(arg, long_name) == 0;
}

/* The value after the option at argv[*i], or NULL when it is missing */
static const char *option_value(int argc, char **argv, int *i) {
    if (*i + 1 < argc) return argv[++*i];
    complain("%s needs a value", argv[*i]);
    return NULL;
}

static bool parse_choice(const char *option, const char *value, const choice_t *choices, int *out) {
    if (!value) return false;
    for (const choice_t *c = choices; c->name; c++) {
        if (strcmp(value, c->name) == 0) {
            *out = c->value;
            return true;
        }
    }
    complain("%s: unknown value '%s'", option, value);
    return false;
}

static bool parse_count(const char *option, const char *value, uint64_t min, uint64_t *out) {
    if (!value) return false;
    char *end = NULL;
    errno = 0;
    unsigned long long n = strtoull(value, &end, 10);
    if (value[0] == '-' || end == value || *end != '\0' || errno == ERANGE || n < min) {
        complain("%s: invalid number '%s'", option, value);
        return false;
    }
    *out = n;
    return true;
}

/* A byte count with an optional K, M or G suffix */
static bool parse_size(const char *option, const char *value, uint64_t *out) {
    if (!value) return false;
    char *end = NULL;
    errno = 0;
    unsigned long long n = strtoull(value, &end, 10);
    unsigned shift = 0;
    if (end != value && *end) {
        switch (*end | 32) {
            case 'k': shift = 10; break;
            case 'm': shift = 20; break;
            case 'g': shift = 30; break;
            default: shift = 64;
        }
        end++;
    }
    if (value[0] == '-' || end == value || *end != '\0' || errno == ERANGE || n == 0 ||
        shift == 64 || n > (UINT64_MAX >> shift)) {
        complain("%s: invalid size '%s'", option, value);
        return false;
    }
    *out = (uint64_t)n << shift;
    return true;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* A lone positional argument is the input when it names a file, or when it
 * cannot be a filter: every condition has a space or one of = < > !. */
static bool looks_like_input(const char *arg) {
    return file_exists(arg) || !strpbrk(arg, " \t=<>!");
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Returns 0 to run, otherwise the exit status (help and version exit 0) */
static int parse_args(int argc, char **argv, options_t *o) {
    hp_transcoder_config_t *cfg = &o->cfg;
    const char *positional[2];
    int n_positional = 0;
    int v;
    uint64_t n;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (is_option(a, "-h", "--help")) {
            print_usage();
            return -1;
        } else if (is_option(a, "-v", "--version")) {
            print_version();
            return -1;
        } else if (is_option(a, "-i", "--input")) {
            if (!(o->input_path = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, "-o", "--output")) {
            if (!(o->output_path = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, NULL, "--from")) {
            if (!parse_choice(a, option_value(argc, argv, &i), in_formats, &v)) return 1;
            cfg->in_format = (hp_format_t)v;
        } else if (is_option(a, NULL, "--to")) {
            if (!parse_choice(a, option_value(argc, argv, &i), out_formats, &v)) return 1;
            cfg->out_format = (hp_format_t)v;
        } else if (is_option(a, NULL, "--select")) {
            if (!(cfg->select_spec = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, NULL, "--where")) {
            if (!(cfg->where_spec = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, NULL, "--on-error")) {
            if (!parse_choice(a, option_value(argc, argv, &i), error_modes, &v)) return 1;
            cfg->on_error = (hp_on_error_t)v;
        } else if (is_option(a, NULL, "--quarantine")) {
            if (!(cfg->quarantine_path = option_value(argc, argv, &i))) return 1;
            cfg->on_error = HP_ON_ERROR_QUARANTINE;
        } else if (is_option(a, NULL, "--head") || is_option(a, NULL, "--limit")) {
            if (!parse_count(a, option_value(argc, argv, &i), 0, &cfg->limit_rows)) return 1;
        } else if (is_option(a, NULL, "--count")) {
            cfg->count_only = true;
        } else if (is_option(a, NULL, "--gzip")) {
            cfg->gzip_out = true;
        } else if (is_option(a, NULL, "--group-by")) {
            if (!(cfg->group_by_spec = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, NULL, "--agg")) {
            if (!(cfg->agg_spec = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, NULL, "--sort")) {
            if (!(cfg->sort_spec = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, NULL, "--memory")) {
            if (!parse_size(a, option_value(argc, argv, &i), &n)) return 1;
            cfg->sort_memory = (size_t)n;
        } else if (is_option(a, NULL, "--summary")) {
            cfg->summary = true;
        } else if (is_option(a, NULL, "--schema")) {
            if (!(cfg->schema_path = option_value(argc, argv, &i))) return 1;
        } else if (is_option(a, NULL, "--jobs")) {
            const char *value = option_value(argc, argv, &i);
            if (value && strcmp(value, "auto") == 0) {
                o->jobs = 0;
            } else if (parse_count(a, value, 1, &n)) {
                o->jobs = n > 1024 ? 1024 : (int)n;   /* hp_parallel_run caps it further */
            } else {
                return 1;
            }
        } else if (is_option(a, NULL, "--simd")) {
            if (!parse_choice(a, option_value(argc, argv, &i), simd_modes, &v)) return 1;
            cfg->simd_mode = (hp_simd_mode_t)v;
        } else if (is_option(a, NULL, "--buffer-size")) {
            if (!parse_count(a, option_value(argc, argv, &i), 1, &n)) return 1;
            cfg->buffer_capacity = (size_t)n;
        } else if (is_option(a, NULL, "--stats")) {
            o->show_stats = true;
        } else if (a[0] == '-' && a[1] != '\0') {
            complain("unknown option '%s' (see --help)", a);
            return 1;
        } else if (n_positional < 2) {
            positional[n_positional++] = a;
        } else {
            complain("unexpected argument '%s'", a);
            return 1;
        }
    }

    /* Positional arguments are FILE then FILTER; either may also come from
     * its option */
    for (int k = 0; k < n_positional; k++) {
        const char *a = positional[k];
        bool as_input = !o->input_path &&
                        (cfg->where_spec || n_positional - k > 1 || looks_like_input(a));
        if (as_input) {
            o->input_path = a;
        } else if (!cfg->where_spec) {
            cfg->where_spec = a;
        } else {
            complain("unexpected argument '%s'", a);
            return 1;
        }
    }

    if (o->output_path && ends_with(o->output_path, ".gz")) cfg->gzip_out = true;

    if (cfg->count_only && o->output_path) {
        complain("--count prints only the row count; it cannot be combined with -o");
        return 1;
    }
    if (cfg->summary && (cfg->count_only || o->output_path || cfg->gzip_out)) {
        complain("--summary cannot be combined with --count, -o or --gzip");
        return 1;
    }
    const bool grouped = cfg->group_by_spec || cfg->agg_spec;
    if (grouped && (cfg->select_spec || cfg->summary)) {
        complain("--group-by and --agg choose the output columns; they cannot be "
                 "combined with --select or --summary");
        return 1;
    }
    if (cfg->sort_spec && cfg->summary) {
        complain("--sort cannot be combined with --summary");
        return 1;
    }
    if (o->jobs > 1) {
        if (!o->input_path) {
            complain("--jobs needs an input file; stdin cannot be split");
            return 1;
        }
        /* With groups or sorting, --head is applied after the merge */
        if ((cfg->limit_rows > 0 && !grouped && !cfg->sort_spec) || cfg->summary) {
            complain("--jobs cannot be combined with --head or --summary");
            return 1;
        }
    }
    return 0;
}

static bool check_agg(const char *group_by, const char *fns) {
    static hp_agg_t agg;
    if (!group_by && !fns) return true;
    hp_error_t err = hp_agg_init(&agg, group_by, fns);
    if (err == HP_ERR_AGG) {
        complain("invalid --agg: %s", agg.error);
        fprintf(stderr, "  %s\n  %*s^\n", fns, (int)agg.error_at, "");
    } else if (err != HP_OK) {
        complain("--group-by: more than %d columns, or a name longer than 127 bytes",
                 HP_AGG_MAX_KEYS);
    }
    return err == HP_OK;
}

static bool check_sort(const char *spec) {
    static hp_sorter_t sorter;
    if (!spec) return true;
    hp_error_t err = hp_sorter_init(&sorter, spec, 0, 0);
    hp_sorter_free(&sorter);
    if (err != HP_OK) {
        complain("--sort: expected comma-separated columns, at most %d, each up to "
                 "127 bytes", HP_SORT_MAX_KEYS);
    }
    return err == HP_OK;
}

static bool check_select(const char *spec) {
    static hp_projection_t projection;
    if (!spec || hp_projection_init(&projection, spec) == HP_OK) return true;
    complain("--select: more than %d columns, or a name longer than 127 bytes", HP_MAX_COLUMNS);
    return false;
}

/* A filter error points at the place in the filter where parsing stopped */
static bool check_filter(const char *where) {
    static hp_filter_t filter;
    if (!where || hp_filter_init(&filter, where) == HP_OK) return true;
    complain("invalid filter: %s", filter.error);
    fprintf(stderr, "  %s\n  %*s^\n", where, (int)filter.error_at, "");
    return false;
}

static void report_error(hp_error_t err, const hp_transcoder_stats_t *st) {
    if (err == HP_ERR_COLUMN_NOT_FOUND && st->error_column[0]) {
        complain("no column '%s' in the input", st->error_column);
    } else if (st->error_record) {
        complain("%s at record %llu, byte %llu", hp_strerror(err),
                 (unsigned long long)st->error_record, (unsigned long long)st->error_offset);
    } else {
        complain("%s", hp_strerror(err));
    }
}

/* ---- main ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    use_binary_stdio();

    options_t o;
    memset(&o, 0, sizeof(o));
    o.cfg.buffer_capacity = HP_DEFAULT_BUFFER_CAPACITY;

    int status = parse_args(argc, argv, &o);
    if (status != 0) return status < 0 ? 0 : status;
    if (!check_select(o.cfg.select_spec) || !check_filter(o.cfg.where_spec) ||
        !check_agg(o.cfg.group_by_spec, o.cfg.agg_spec) || !check_sort(o.cfg.sort_spec)) {
        return 1;
    }

    /* --jobs auto takes the parallel path only where it applies and pays off;
     * an explicit --jobs N says when it does not apply */
    const bool jobs_explicit = (o.jobs != 0);
    o.cfg.sort_threads = o.jobs;   /* --jobs bounds the sort's threads too */
    if (!jobs_explicit) {
        const bool merged = o.cfg.group_by_spec || o.cfg.agg_spec || o.cfg.sort_spec;
        o.jobs = (o.input_path && (o.cfg.limit_rows == 0 || merged) && !o.cfg.summary)
                     ? hp_parallel_auto_jobs(o.input_path) : 1;
    }

    FILE *in_f = stdin;
    if (o.input_path && !(in_f = fopen(o.input_path, "rb"))) {
        complain("cannot open '%s': %s", o.input_path, strerror(errno));
        return 1;
    }
    FILE *out_f = stdout;
    if (o.output_path && !(out_f = fopen(o.output_path, "wb"))) {
        complain("cannot create '%s': %s", o.output_path, strerror(errno));
        if (in_f != stdin) fclose(in_f);
        return 1;
    }
    setvbuf(in_f, NULL, _IONBF, 0);
    setvbuf(out_f, NULL, _IONBF, 0);

    hp_error_t err = HP_ERR_UNSUPPORTED;
    hp_transcoder_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    if (o.jobs > 1) {
        err = hp_parallel_run(&o.cfg, o.input_path, out_f, o.jobs, &stats);
        /* gzip input cannot be split, CSV skip/quarantine runs need the
         * sequential resynchronization, and an unparsable first record
         * belongs to the fault-tolerance mode: run single-threaded */
        if (err == HP_ERR_UNSUPPORTED && jobs_explicit) {
            complain("--jobs does not apply to gzip input, CSV with --on-error "
                     "skip or quarantine, or an unparsable first record; "
                     "running single-threaded");
        }
    }

    if (err == HP_ERR_UNSUPPORTED) {
        hp_transcoder_t tc;
        err = hp_transcoder_init(&tc, &o.cfg);
        if (err == HP_OK) {
            err = hp_transcoder_run(&tc, in_f, out_f);
            stats = tc.stats;
            hp_transcoder_free(&tc);
        }
    }

    if (o.cfg.count_only) printf("%llu\n", (unsigned long long)stats.rows_emitted);
    if (o.show_stats) print_perf_stats(&stats);
    if (err != HP_OK) report_error(err, &stats);

    if (in_f != stdin) fclose(in_f);
    if (out_f != stdout) fclose(out_f);
    return (err == HP_OK) ? 0 : 1;
}
