#ifndef HYPERPIPE_HP_PARALLEL_H
#define HYPERPIPE_HP_PARALLEL_H

#include "hp_transcoder.h"
#include <stdio.h>

/* --jobs N: parallel chunked pipeline.
 *
 * The input file is split into N chunks at quote-aware line boundaries (a
 * boundary is a newline outside a quoted field, so no record ever straddles
 * two chunks). One worker thread per chunk runs the full transcoder
 * pipeline - parse, filter, project, serialize - into a bounded page queue;
 * the coordinator drains the queues strictly in chunk order, which restores
 * the exact byte stream of the single-threaded run.
 *
 * Scope (deliberate, see docs/ARCHITECTURE.md 7):
 *   - CSV, TSV and NDJSON input. NDJSON runs lock the schema to the first
 *     line, the same rule CSV output follows. gzip-compressed input reports
 *     HP_ERR_UNSUPPORTED so the caller falls back to the single-threaded path.
 *   - The input must be a regular seekable file.
 *   - Runs on Windows and POSIX (see src/hp_thread.h).
 *   - Memory stays bounded: each worker holds one ring buffer plus at most
 *     a few pages of queued output.
 *   - gzip output with --jobs produces concatenated gzip members, which
 *     every gzip decoder (including this project's own reader) accepts. */

hp_error_t hp_parallel_run(const hp_transcoder_config_t *base_cfg,
                           const char *input_path,
                           FILE *out_stream,
                           int jobs,
                           hp_transcoder_stats_t *stats_out);

#endif /* HYPERPIPE_HP_PARALLEL_H */
