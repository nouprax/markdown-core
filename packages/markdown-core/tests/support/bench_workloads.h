#ifndef MARKDOWN_CORE_BENCH_WORKLOADS_H
#define MARKDOWN_CORE_BENCH_WORKLOADS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The benchmark workloads, shared by the timing lane (bench_runner, on the
 * public facade) and the work-invariant lane (work_runner, on the
 * diagnostics engine). Every input is deterministic and offline: it comes
 * from a tracked sample or is generated in memory, never downloaded or
 * written to the source tree, and its bytes are identified by a SHA-256
 * digest so two measurements of "the same workload" can prove they read
 * the same input. A workload's version changes whenever its inputs do. */

typedef struct bench_case {
    const char *workload;
    int version;
    /* Unique within the workload; a doubling series is `name@scale`. */
    char name[128];
    const char *generator;
    /* The generator's parameters as `key=value` pairs. */
    char parameters[128];
    /* The doubling scale and the case's index in its series; 0 and 0 for a
     * case that stands alone. */
    size_t scale, step;
    const char *data; /* NUL-terminated one byte past `length` */
    size_t length;
    char sha256[65]; /* lowercase hex */
} bench_case;

/* Returns non-zero to stop the visit; that value is returned by the visit. */
typedef int (*bench_case_visitor)(const bench_case *input, void *context);

size_t bench_workload_count(void);
const char *bench_workload_name(size_t index);
/* 0 when the workload does not exist. */
int bench_workload_version(const char *workload);
/* Builds every case of `workload` in order and calls `visit` with each; the
 * input is released after the call. Returns the visitor's first non-zero
 * result, -1 when a case cannot be built (a sample cannot be read, or
 * allocation fails), -2 for an unknown workload, 0 otherwise. */
int bench_workload_visit(const char *workload, const char *samples_dir, bench_case_visitor visit, void *context);

/* The same, with the sample workload's replication factor overridden:
 * `copies` of each tracked sample instead of its own 200. Zero keeps the
 * workload's factor. Only the sample-based workload replicates a file on
 * disk, so only it reads this; a generated workload's scale is its own.
 *
 * The instruction lane reads a case twice, once replicated and once at
 * `copies = 1`: replication measures throughput, and the unreplicated read
 * is the only one where a parse's fixed cost is not amortized away. */
int bench_workload_visit_copies(const char *workload, const char *samples_dir, size_t copies, bench_case_visitor visit,
                                void *context);

/* SHA-256 of `length` bytes, as 64 lowercase hex digits plus a NUL. */
void bench_sha256_hex(const void *data, size_t length, char hex[65]);

#ifdef __cplusplus
}
#endif

#endif
