/* Real data: the NASA access log (data/nasa_server_access_log.csv; a 5K-row
 * sample ships with the source, data/fetch_real_dataset.py fetches the full
 * 1.89M rows). Checks that all engines agree on it, that memory stays
 * bounded, and that a filter with a projection behaves on real values. */

#include "tap.h"
#include "hyperpipe/hp_transcoder.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <psapi.h>
    static size_t peak_rss(void) {
        PROCESS_MEMORY_COUNTERS pmc;
        return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.PeakWorkingSetSize : 0;
    }
#else
    #include <sys/resource.h>
    static size_t peak_rss(void) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (size_t)ru.ru_maxrss * 1024;
    }
#endif

static const char *dataset;
static uint64_t dataset_rows;

static const char *find_dataset(void) {
    static const char *const candidates[] = {
        "data/nasa_server_access_log.csv",
        "../data/nasa_server_access_log.csv",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

static FILE *transcode(const hp_transcoder_config_t *cfg, hp_transcoder_stats_t *stats) {
    FILE *in = fopen(dataset, "rb");
    FILE *out = tmpfile();
    CHECK(in && out);
    hp_transcoder_t tc;
    CHECK(hp_transcoder_init(&tc, cfg) == HP_OK);
    CHECK(hp_transcoder_run(&tc, in, out) == HP_OK);
    *stats = tc.stats;
    hp_transcoder_free(&tc);
    fclose(in);
    rewind(out);
    return out;
}

static bool same_contents(FILE *a, FILE *b) {
    static char ba[1 << 16], bb[1 << 16];
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), a);
        size_t nb = fread(bb, 1, sizeof(bb), b);
        if (na != nb || memcmp(ba, bb, na) != 0) return false;
        if (na == 0) return true;
    }
}

static void test_engines_agree(void) {
    static const hp_simd_mode_t engines[] = { HP_SIMD_SCALAR, HP_SIMD_AVX2, HP_SIMD_AVX512 };
    FILE *ref = NULL;
    for (size_t i = 0; i < sizeof(engines) / sizeof(engines[0]); i++) {
        hp_transcoder_config_t cfg = {
            .in_format = HP_FORMAT_CSV,
            .out_format = HP_FORMAT_NDJSON,
            .simd_mode = engines[i],
            .buffer_capacity = 64 * 1024,
        };
        hp_transcoder_stats_t stats;
        FILE *out = transcode(&cfg, &stats);
        if (i == 0) {
            ref = out;
            dataset_rows = stats.rows_read;
            continue;
        }
        CHECK(stats.rows_read == dataset_rows);
        CHECK(same_contents(ref, out));
        rewind(ref);
        fclose(out);
    }
    fclose(ref);
}

/* Streaming keeps memory flat whatever the input size; the bound includes
 * the process baseline and the system DLLs. */
static void test_memory_stays_bounded(void) {
    double mib = (double)peak_rss() / (1024.0 * 1024.0);
    printf("# peak RSS %.2f MiB\n", mib);
#if defined(__SANITIZE_ADDRESS__)
    printf("# not checked: AddressSanitizer's shadow memory dominates the RSS\n");
#else
    CHECK(mib < 8.0);
#endif
}

static void test_filter_and_projection(void) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_CSV,
        .select_spec = "host,status,bytes",
        .where_spec = "status == 200, bytes > 5000",
        .buffer_capacity = 64 * 1024,
    };
    hp_transcoder_stats_t stats;
    fclose(transcode(&cfg, &stats));
    CHECK(stats.rows_read == dataset_rows);
    CHECK(stats.rows_emitted > dataset_rows / 4);
}

int main(void) {
    dataset = find_dataset();
    if (!dataset) return tap_skip_all("data/nasa_server_access_log.csv not found");
    TAP_RUN(test_engines_agree);
    TAP_RUN(test_memory_stays_bounded);
    TAP_RUN(test_filter_and_projection);
    return tap_done();
}
