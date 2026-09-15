/* THE ONE SHAPE BOTH ENGINES ARE DRIVEN THROUGH.
 *
 * The stage benchmark compares two parsers on the same bytes, so neither may
 * get a driver of its own: `stage_runner.c` owns argument handling, document
 * loading and the receipt, and each engine supplies only the parse itself.
 * Loading and the receipt sit outside the measured call edges, so what the
 * driver reads back is the parse and nothing the harness did around it.
 */
#ifndef MARKDOWN_CORE_BENCH_STAGE_RUNNER_H
#define MARKDOWN_CORE_BENCH_STAGE_RUNNER_H

#include <stddef.h>

/* What the parse is asked to prove it did. The driver compares both engines'
 * receipts for one document: equal byte counts mean the comparison really ran
 * on the same input, and a nonzero tree means neither engine returned early. */
typedef struct bench_receipt {
    size_t bytes;
    size_t root_children;
} bench_receipt;

/* The engine's name as it appears in the report. */
const char *bench_engine_name(void);

/* Parse one complete document. Returns 0 on success, nonzero when the engine
 * reports failure; the harness then exits nonzero rather than reporting a
 * measurement of a parse that did not happen. */
int bench_parse_document(const char *source, size_t length, bench_receipt *receipt);

#endif
