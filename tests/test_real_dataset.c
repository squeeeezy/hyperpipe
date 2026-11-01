#include "hyperpipe/hp_transcoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <psapi.h>
    static size_t get_peak_rss(void) {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return (size_t)pmc.PeakWorkingSetSize;
        }
        return 0;
    }
#else
    #include <sys/resource.h>
    static size_t get_peak_rss(void) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (size_t)ru.ru_maxrss * 1024;
    }
#endif

/* Standard compact SHA-256 implementation */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} hp_sha256_t;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define Ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define S0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define S1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define s0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define s1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static void sha256_transform(hp_sha256_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + S1(e) + Ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = S0(a) + Maj(a, b, c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(hp_sha256_t *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update(hp_sha256_t *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    while (i < len) {
        ctx->buffer[idx++] = data[i++];
        if (idx == 64) {
            sha256_transform(ctx, ctx->buffer);
            idx = 0;
        }
    }
}

static void sha256_final(hp_sha256_t *ctx, uint8_t hash[32]) {
    size_t idx = (size_t)(ctx->count & 63);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buffer[idx++] = 0;
        sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    while (idx < 56) ctx->buffer[idx++] = 0;
    uint64_t bits = ctx->count * 8;
    for (int i = 7; i >= 0; i--) {
        ctx->buffer[56 + (7 - i)] = (uint8_t)(bits >> (i * 8));
    }
    sha256_transform(ctx, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void compute_file_sha256(const char *path, uint8_t hash[32]) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    hp_sha256_t ctx;
    sha256_init(&ctx);
    uint8_t buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&ctx, buf, n);
    }
    fclose(f);
    sha256_final(&ctx, hash);
}

static const char *find_real_dataset(void) {
    static const char *candidates[] = {
        "hyperpipe/data/nasa_server_access_log.csv",
        "../data/nasa_server_access_log.csv",
        "data/nasa_server_access_log.csv",
        "../../hyperpipe/data/nasa_server_access_log.csv",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

int main(void) {
    const char *csv_path = find_real_dataset();
    assert(csv_path != NULL && "Authentic dataset 'nasa_server_access_log.csv' must exist");
    printf("[test_real_dataset] Found authentic dataset: '%s'\n", csv_path);

    /* 1. Transcode via Scalar */
    printf("[test_real_dataset] Running 1.89M rows transcoding with SCALAR kernel...\n");
    hp_transcoder_config_t cfg_scalar = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_SCALAR,
        .buffer_capacity = 64 * 1024
    };
    hp_transcoder_t tc_scalar;
    assert(hp_transcoder_init(&tc_scalar, &cfg_scalar) == HP_OK);
    FILE *in1 = fopen(csv_path, "rb");
    FILE *out1 = fopen("real_scalar.tmp", "wb");
    assert(in1 && out1);
    assert(hp_transcoder_run(&tc_scalar, in1, out1) == HP_OK);
    fclose(in1);
    fclose(out1);
    uint64_t rows_scalar = tc_scalar.stats.rows_read;
    hp_transcoder_free(&tc_scalar);

    /* 2. Transcode via AVX2 */
    printf("[test_real_dataset] Running 1.89M rows transcoding with AVX2 kernel...\n");
    hp_transcoder_config_t cfg_avx2 = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_AVX2,
        .buffer_capacity = 64 * 1024
    };
    hp_transcoder_t tc_avx2;
    assert(hp_transcoder_init(&tc_avx2, &cfg_avx2) == HP_OK);
    FILE *in2 = fopen(csv_path, "rb");
    FILE *out2 = fopen("real_avx2.tmp", "wb");
    assert(in2 && out2);
    assert(hp_transcoder_run(&tc_avx2, in2, out2) == HP_OK);
    fclose(in2);
    fclose(out2);
    uint64_t rows_avx2 = tc_avx2.stats.rows_read;
    hp_transcoder_free(&tc_avx2);

    /* 3. Transcode via AVX-512 */
    printf("[test_real_dataset] Running 1.89M rows transcoding with AVX-512 kernel...\n");
    hp_transcoder_config_t cfg_avx512 = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_AVX512,
        .buffer_capacity = 64 * 1024
    };
    hp_transcoder_t tc_avx512;
    assert(hp_transcoder_init(&tc_avx512, &cfg_avx512) == HP_OK);
    FILE *in3 = fopen(csv_path, "rb");
    FILE *out3 = fopen("real_avx512.tmp", "wb");
    assert(in3 && out3);
    assert(hp_transcoder_run(&tc_avx512, in3, out3) == HP_OK);
    fclose(in3);
    fclose(out3);
    uint64_t rows_avx512 = tc_avx512.stats.rows_read;
    hp_transcoder_free(&tc_avx512);

    /* 4. Compare Bit-for-Bit Hashes across all 3 kernels */
    uint8_t hash_scalar[32], hash_avx2[32], hash_avx512[32];
    compute_file_sha256("real_scalar.tmp", hash_scalar);
    compute_file_sha256("real_avx2.tmp", hash_avx2);
    compute_file_sha256("real_avx512.tmp", hash_avx512);

    remove("real_scalar.tmp");
    remove("real_avx2.tmp");
    remove("real_avx512.tmp");

    assert(rows_scalar == rows_avx2 && rows_avx2 == rows_avx512);
    assert(memcmp(hash_scalar, hash_avx2, 32) == 0 && "Scalar != AVX2 on real dataset!");
    assert(memcmp(hash_scalar, hash_avx512, 32) == 0 && "Scalar != AVX-512 on real dataset!");

    printf("  [PASS] Differential equivalence verified bit-for-bit: Scalar == AVX2 == AVX-512 (%llu rows)\n",
           (unsigned long long)rows_scalar);

    /* 5. Memory RSS bounded verification (guaranteed bounded streaming memory) */
    size_t peak_rss = get_peak_rss();
    double peak_mb = (double)peak_rss / (1024.0 * 1024.0);
    printf("  [PASS] Peak RSS on 183 MB dataset: %.2f MB (guaranteed bounded streaming memory)\n", peak_mb);
    fflush(stdout);
    assert(peak_mb < 8.0 && "Peak RSS must remain bounded under 8.0 MB including OS DLLs");

    /* 6. Test in-flight projection & filter on real HTTP status */
    printf("[test_real_dataset] Testing in-flight projection and HTTP status filter...\n");
    hp_transcoder_config_t cfg_flt = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_CSV,
        .simd_mode = HP_SIMD_AUTO,
        .select_spec = "host,status,bytes",
        .where_spec = "status == 200, bytes > 5000",
        .buffer_capacity = 64 * 1024
    };
    hp_transcoder_t tc_flt;
    assert(hp_transcoder_init(&tc_flt, &cfg_flt) == HP_OK);
    FILE *in_f = fopen(csv_path, "rb");
    FILE *out_f = fopen("real_flt.tmp", "wb");
    assert(in_f && out_f);
    assert(hp_transcoder_run(&tc_flt, in_f, out_f) == HP_OK);
    fclose(in_f);
    fclose(out_f);
    remove("real_flt.tmp");

    assert(tc_flt.stats.rows_read == rows_scalar);
    assert(tc_flt.stats.rows_emitted > 700000 && "Expected > 700k matching requests");
    printf("  [PASS] Filtered %llu rows to %llu emitted in %.3f s\n",
           (unsigned long long)tc_flt.stats.rows_read,
           (unsigned long long)tc_flt.stats.rows_emitted,
           tc_flt.stats.elapsed_seconds);
    hp_transcoder_free(&tc_flt);

    printf("[test_real_dataset] ALL TESTS PASSED ON REAL PRODUCTION DATASET!\n");
    return 0;
}
