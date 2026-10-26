/* THE GENERATED INPUTS the oracle runners share
 * (docs/plans/2026-10-23-lsp-completion-engine.md, section 8). */
#ifndef MARKDOWN_CORE_TESTS_SAMPLES_H
#define MARKDOWN_CORE_TESTS_SAMPLES_H

#include <stdbool.h>
#include <stddef.h>

/* Calls `check` with every generated input, NUL-terminated, its length,
 * which a NUL it holds does not end, and its number:
 * its outer construct, inner construct and atom, two digits each, counted
 * from 1. False when an input could not be allocated. */
bool samples_each(void (*check)(void *context, const char *source, size_t length, int number), void *context);

#endif
