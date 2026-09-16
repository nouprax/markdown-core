/* THE ONE SHAPE BOTH ATTRIBUTE BASELINES ARE DRIVEN THROUGH.
 *
 * `{#lane .stage k="callgrind"}` and `<x id="lane" class="stage"
 * k="callgrind">` are the same job spelled twice: scan a bracketed run, split
 * it into an identifier, a class run and key/value records, and hand back what
 * was found. No Markdown parser implements the second spelling and no HTML
 * parser implements the first, so neither can be measured on the other's
 * bytes -- which is why this is a separate comparison from the document stage
 * benchmark rather than a fourth engine in it.
 *
 * What makes it a comparison and not two numbers is that the two baselines are
 * required to RECOVER THE SAME ATTRIBUTES. Each writes what it found in one
 * canonical form and the driver compares those, so a baseline that skipped a
 * record, kept a value raw, or stopped early fails the run instead of posting
 * a flattering count.
 *
 * The census is written on a separate pass, deliberately. Folding it into the
 * measured one would put a formatting loop inside the cost being compared --
 * and the two baselines hold classes differently (this parser as one chunk per
 * class, an HTML tokenizer as one `class` value), so that loop would not even
 * be the same loop on both sides.
 */
#ifndef MARKDOWN_CORE_BENCH_ATTRIBUTE_RUNNER_H
#define MARKDOWN_CORE_BENCH_ATTRIBUTE_RUNNER_H

#include <stddef.h>
#include <stdio.h>

/* What the recovery is asked to prove it did. The driver compares both
 * baselines' receipts: equal counts mean neither stopped early, and a zero
 * count means the input was not the input either of them was given. */
typedef struct attribute_receipt {
    size_t lists;
    size_t values;
} attribute_receipt;

/* The baseline's name as it appears in the report. */
const char *bench_baseline_name(void);

/* Recover every attribute list in the buffer.
 *
 * With `census` NULL this is the measured path and writes nothing. With a
 * stream it additionally writes one line per list in the canonical form
 *
 *   list <index> id=<anchor> class=<classes, single-spaced> <name>=<value>...
 *
 * where an absent anchor and an empty class run are written as empty values,
 * so the two baselines' output is comparable line for line rather than as a
 * count. Returns 0 on success.
 */
int bench_parse_attributes(const char *source, size_t length, attribute_receipt *receipt, FILE *census);

#endif
