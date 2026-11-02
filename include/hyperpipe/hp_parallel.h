#ifndef HYPERPIPE_HP_PARALLEL_H
#define HYPERPIPE_HP_PARALLEL_H

#include "hp_transcoder.h"
#include <stdio.h>

/* --jobs N: parallel chunked pipeline.
 *
 * The input file is cut into chunks of whole records (a record ends at a
 * newline outside quotes, so none straddles two chunks). Worker threads run
 * the full transcoder pipeline - parse, filter, project, serialize - on one
 * chunk at a time, and the output is assembled strictly in chunk order,
 * which reproduces the exact byte stream of the single-threaded run.
 * See src/hp_parallel.c for how chunks are found and scheduled.
 *
 * Scope:
 *   - CSV, TSV and NDJSON input. NDJSON runs lock the schema to the first
 *     line, the same rule CSV output follows.
 *   - HP_ERR_UNSUPPORTED, with the single-threaded path as the fallback, for
 *     gzip input, for CSV/TSV with --on-error skip or quarantine, and for a
 *     first record the parsers reject.
 *   - The input must be a regular seekable file.
 *   - Runs on Windows and POSIX (see src/hp_thread.h).
 *   - Memory stays bounded: at most jobs + 2 chunks (a few MiB of input
 *     each) are in flight, whatever the input size.
 *   - gzip output with --jobs produces concatenated gzip members, which
 *     every gzip decoder (including this project's own reader) accepts. */

/* Job count for --jobs auto: one per processor (at most 16) for a regular
 * file large enough to amortize the threads, otherwise 1. */
int hp_parallel_auto_jobs(const char *input_path);

hp_error_t hp_parallel_run(const hp_transcoder_config_t *base_cfg,
                           const char *input_path,
                           FILE *out_stream,
                           int jobs,
                           hp_transcoder_stats_t *stats_out);

#endif /* HYPERPIPE_HP_PARALLEL_H */
