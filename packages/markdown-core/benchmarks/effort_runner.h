#ifndef BENCH_EFFORT_RUNNER_H
#define BENCH_EFFORT_RUNNER_H

#include <stddef.h>
#include <stdint.h>

/* The SAME machine-level entry contract for both adapters. Native descriptor
 * construction belongs inside operation(), including cmark's larger cache.
 * No native parser state, recognized tokens or allocation are supplied free. */
typedef enum { EFFORT_COPY, EFFORT_TRIM, EFFORT_UNESCAPE, EFFORT_WHITESPACE, EFFORT_CODE, EFFORT_CLOSER } effort_op;
typedef struct {
    const unsigned char *input;
    unsigned char *data;
    int32_t length, size, capacity;
    int32_t start, ticks, position, result, cache[81];
    int scanned;
} effort_state;

#if defined(__GNUC__) || defined(__clang__)
#define EFFORT_NOINLINE __attribute__((noinline))
#else
#define EFFORT_NOINLINE
#endif

EFFORT_NOINLINE void bench_effort_operation(effort_state *state, effort_op operation);

#endif
