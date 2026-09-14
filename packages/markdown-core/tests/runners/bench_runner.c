/* Opt-in local benchmark workloads: the timing lane (CTest label: benchmark).
 *
 * Every workload is deterministic and offline: inputs come from the tracked
 * sample documents or are synthesized in-process by the shared workload
 * generators (tests/support/bench_workloads.c), and each input is identified
 * by its SHA-256. The parse and the free of a document are timed separately
 * through the public facade -- the one entry every consumer runs -- and every
 * sample is reported, with the minimum, the median, throughput from the
 * bytes and time per node from the tree. Timings and relative scaling ratios
 * are measurements only. They never decide correctness or process status;
 * hosted-runner load and platform variance make wall-clock assertions an
 * unreliable regression oracle. The deterministic lane is work_runner.
 *
 * A runner built with MARKDOWN_CORE_BENCH_CMARK links the pinned cmark
 * oracle and, with --reference cmark, times its parse and free of the same
 * bytes beside the engine's. --instructions parses and frees each case
 * exactly once and reports nothing else, so that a run under callgrind
 * counts the instructions of that one parse; --dry-run builds the input and
 * parses nothing, the baseline such a count subtracts
 * (scripts/benchmark-instructions.mjs drives both).
 *
 * Every timed case also reports the minor page faults the measured parses
 * took, per parse: a parse whose arena blocks the allocator gave back to
 * the system faults every page in again, and the kernel zeroes it, which
 * no instruction count shows. --allocator retain asks glibc, through
 * mallopt, to keep freed memory (no mmap for blocks below 32 MB, no trim of
 * the heap top, a 64 MB top pad) so that the same measurement shows the
 * parse without those faults: the share of the time that is memory being
 * handed back and re-faulted, on any workload, beside the default. It is a
 * measurement of the process's allocator, not a setting the library makes.
 * Both readings reach --json too: the allocator with the run, the faults
 * with each case.
 *
 *   bench_runner --list [--workload NAME --samples DIR]
 *   bench_runner --workload NAME --samples DIR [--case NAME] [--repeats N]
 *                [--warmup N] [--json FILE] [--source-sha SHA]
 *                [--reference cmark] [--instructions [--implementation core|cmark]]
 *                [--dry-run] [--allocator default|retain]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <markdown_core.h>
#ifdef MARKDOWN_CORE_BENCH_CMARK
#include <cmark.h>
#endif

#include "bench_workloads.h"
#include "test_support.h"

#define BENCH_MAX_REPEATS 32
#define BENCH_DEFAULT_REPEATS 5
#define BENCH_DEFAULT_WARMUP 1

typedef struct bench_options {
    const char *samples_dir;
    const char *json_path;
    const char *source_sha;
    /* Only this case of the workload, when set. */
    const char *case_name;
    /* The reference implementation timed beside the engine; NULL for none. */
    const char *reference;
    /* The one implementation --instructions runs: "core" or "cmark". */
    const char *implementation;
    /* The process allocator's retention: "default" or "retain" (see above). */
    const char *allocator;
    int repeats;
    int warmup;
    int instructions;
    int dry_run;
} bench_options;

typedef struct bench_sample {
    uint64_t parse_ns, free_ns;
} bench_sample;

typedef struct bench_run {
    const bench_options *options;
    const char *workload;
    FILE *json;
    int json_cases;
    /* Cases the --case filter let through. */
    int matched;
    /* The previous case of the doubling series being measured. */
    const char *series;
    uint64_t series_previous_ns;
} bench_run;

static long peak_rss_kib(void) {
#ifndef _WIN32
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
#else
    return -1;
#endif
}

/* The process's minor page faults so far, or -1 where they cannot be read. */
static long minor_faults(void) {
#ifndef _WIN32
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return usage.ru_minflt;
#else
    return -1;
#endif
}

static int count_node(const markdown_core_node *node, void *context) {
    (void)node;
    (*(size_t *)context)++;
    return 0;
}

/* One parse and one free, timed apart; `nodes` is counted between them on
 * request, outside both windows. */
static int bench_parse_once(const char *input, size_t length, bench_sample *sample, size_t *nodes) {
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    uint64_t started = ts_monotonic_ns(), parsed, released;
    document = markdown_core_document_parse((const uint8_t *)input, length, &error);
    parsed = ts_monotonic_ns();
    if (!document) {
        markdown_core_error_free(error);
        return -1;
    }
    if (nodes) {
        *nodes = 0;
        ts_ast_walk(markdown_core_document_root(document), count_node, nodes);
    }
    released = ts_monotonic_ns();
    markdown_core_document_free(document);
    sample->parse_ns = parsed - started;
    sample->free_ns = ts_monotonic_ns() - released;
    return 0;
}

#ifdef MARKDOWN_CORE_BENCH_CMARK
/* The reference: cmark's parse and free of the same bytes, timed apart the
 * same way. */
static int bench_reference_once(const char *input, size_t length, bench_sample *sample) {
    uint64_t started = ts_monotonic_ns(), parsed, released;
    cmark_node *document = cmark_parse_document(input, length, CMARK_OPT_DEFAULT);
    parsed = ts_monotonic_ns();
    if (!document) {
        return -1;
    }
    released = ts_monotonic_ns();
    cmark_node_free(document);
    sample->parse_ns = parsed - started;
    sample->free_ns = ts_monotonic_ns() - released;
    return 0;
}
#define BENCH_REFERENCE_VERSION CMARK_VERSION_STRING
#else
static int bench_reference_once(const char *input, size_t length, bench_sample *sample) {
    (void)input;
    (void)length;
    (void)sample;
    return -1;
}
#define BENCH_REFERENCE_VERSION ""
#endif

static int bench_once(const bench_options *options, const char *input, size_t length, bench_sample *sample) {
    return options->implementation && strcmp(options->implementation, "cmark") == 0
               ? bench_reference_once(input, length, sample)
               : bench_parse_once(input, length, sample, NULL);
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static void order_statistics(const bench_sample *samples, int count, int free_lane, uint64_t *minimum,
                             uint64_t *median) {
    uint64_t values[BENCH_MAX_REPEATS];
    int i;
    for (i = 0; i < count; i++) {
        values[i] = free_lane ? samples[i].free_ns : samples[i].parse_ns;
    }
    qsort(values, (size_t)count, sizeof(values[0]), compare_u64);
    *minimum = values[0];
    *median = values[count / 2];
}

static int measure_case(const bench_case *input, void *context) {
    bench_run *run = (bench_run *)context;
    const bench_options *options = run->options;
    bench_sample samples[BENCH_MAX_REPEATS], reference[BENCH_MAX_REPEATS];
    bench_sample warm;
    size_t nodes = 0;
    uint64_t min_parse, median_parse, min_free, median_free;
    uint64_t reference_min_parse = 0, reference_median_parse = 0, reference_min_free = 0, reference_median_free = 0;
    long rss_before = peak_rss_kib(), rss_after;
    long faults_before, faults_after;
    double mb_per_s, ns_per_node, faults_per_parse;
    int repeats = options->repeats > BENCH_MAX_REPEATS ? BENCH_MAX_REPEATS : options->repeats;
    int i;

    if (options->case_name && strcmp(input->name, options->case_name) != 0) {
        return 0;
    }
    run->matched++;
    if (options->dry_run || options->instructions) {
        /* The instruction lane: the input is built either way; one parse
         * and free, or none, is the whole difference between the two runs. */
        if (!options->dry_run && bench_once(options, input->data, input->length, &warm) != 0) {
            fprintf(stderr, "%s: parse failed\n", input->name);
            return 1;
        }
        printf("instructions case=%s implementation=%s bytes=%zu parses=%d sha256=%s\n", input->name,
               options->implementation ? options->implementation : "core", input->length, options->dry_run ? 0 : 1,
               input->sha256);
        return 0;
    }

    for (i = 0; i < options->warmup; i++) {
        if (bench_parse_once(input->data, input->length, &warm, NULL) != 0) {
            fprintf(stderr, "%s: parse failed\n", input->name);
            return 1;
        }
    }
    /* The tree is counted here, outside the fault window and outside every
     * reported sample: the walk would otherwise charge its own first touch
     * -- of the walker's code and of the tree it reads -- to the parse whose
     * faults are reported, which with one repeat is the whole measurement. */
    if (bench_parse_once(input->data, input->length, &warm, &nodes) != 0) {
        fprintf(stderr, "%s: parse failed\n", input->name);
        return 1;
    }
    faults_before = minor_faults();
    for (i = 0; i < repeats; i++) {
        if (bench_parse_once(input->data, input->length, &samples[i], NULL) != 0) {
            fprintf(stderr, "%s: parse failed\n", input->name);
            return 1;
        }
    }
    faults_after = minor_faults();
    rss_after = peak_rss_kib();
    order_statistics(samples, repeats, 0, &min_parse, &median_parse);
    order_statistics(samples, repeats, 1, &min_free, &median_free);
    if (options->reference) {
        for (i = 0; i < options->warmup; i++) {
            if (bench_reference_once(input->data, input->length, &warm) != 0) {
                fprintf(stderr, "%s: %s parse failed\n", input->name, options->reference);
                return 1;
            }
        }
        for (i = 0; i < repeats; i++) {
            if (bench_reference_once(input->data, input->length, &reference[i]) != 0) {
                fprintf(stderr, "%s: %s parse failed\n", input->name, options->reference);
                return 1;
            }
        }
        order_statistics(reference, repeats, 0, &reference_min_parse, &reference_median_parse);
        order_statistics(reference, repeats, 1, &reference_min_free, &reference_median_free);
    }
    mb_per_s = min_parse ? (double)input->length / ((double)min_parse / 1e9) / 1e6 : 0.0;
    ns_per_node = nodes ? (double)min_parse / (double)nodes : 0.0;
    /* The faults of the measured parses and frees together, per parse; the
     * warmup took the first touch of every page the process keeps. */
    faults_per_parse =
        faults_before >= 0 && faults_after >= 0 ? (double)(faults_after - faults_before) / (double)repeats : -1.0;

    printf("benchmark case=%s bytes=%zu nodes=%zu repeats=%d warmup=%d min_parse_ns=%llu median_parse_ns=%llu"
           " min_free_ns=%llu median_free_ns=%llu mb_per_s=%.2f ns_per_node=%.1f rss_delta_kib=%ld"
           " minor_faults_per_parse=%.1f allocator=%s sha256=%s\n",
           input->name, input->length, nodes, repeats, options->warmup, (unsigned long long)min_parse,
           (unsigned long long)median_parse, (unsigned long long)min_free, (unsigned long long)median_free, mb_per_s,
           ns_per_node, rss_before >= 0 && rss_after >= 0 ? rss_after - rss_before : -1L, faults_per_parse,
           options->allocator ? options->allocator : "default", input->sha256);
    printf("benchmark samples case=%s parse_ns=", input->name);
    for (i = 0; i < repeats; i++) {
        printf("%s%llu", i ? "," : "", (unsigned long long)samples[i].parse_ns);
    }
    printf(" free_ns=");
    for (i = 0; i < repeats; i++) {
        printf("%s%llu", i ? "," : "", (unsigned long long)samples[i].free_ns);
    }
    printf("\n");
    if (options->reference) {
        /* The same bytes through the reference, and the engine's minimum
         * over the reference's: a measurement to read, never a gate. */
        printf("benchmark reference=%s version=%s case=%s min_parse_ns=%llu median_parse_ns=%llu min_free_ns=%llu"
               " median_free_ns=%llu parse_ratio=%.3f\n",
               options->reference, BENCH_REFERENCE_VERSION, input->name, (unsigned long long)reference_min_parse,
               (unsigned long long)reference_median_parse, (unsigned long long)reference_min_free,
               (unsigned long long)reference_median_free,
               reference_min_parse ? (double)min_parse / (double)reference_min_parse : 0.0);
    }

    /* A doubling series reports the ratio of adjacent minimums. A reviewer
     * may use it to design a deterministic invariant or a controlled
     * experiment, but the ratio itself is not a test assertion. */
    if (input->step > 1 && run->series && strcmp(run->series, input->generator) == 0) {
        uint64_t floor_ns = run->series_previous_ns > 500 ? run->series_previous_ns : 500;
        printf("benchmark scaling=%s scale=%zu adjacent_ratio=%.3f\n", input->generator, input->scale,
               (double)min_parse / (double)floor_ns);
    }
    run->series = input->step ? input->generator : NULL;
    run->series_previous_ns = min_parse;

    if (strcmp(input->name, "binding_baseline") == 0) {
        /* The PR benchmark's one fixed operation; the first eight fields are
         * its published contract, the rest are the same measurement's detail. */
        printf("metric runtime=c workload=representative_large workload_version=1"
               " bytes=%zu warmup=%d repeats=%d median_ns=%llu peak_rss_kib=%ld"
               " min_ns=%llu free_median_ns=%llu nodes=%zu input_sha256=%s\n",
               input->length, options->warmup, repeats, (unsigned long long)median_parse, peak_rss_kib(),
               (unsigned long long)min_parse, (unsigned long long)median_free, nodes, input->sha256);
    }

    if (run->json) {
        fprintf(run->json,
                "%s    {\n      \"name\": \"%s\",\n      \"generator\": \"%s\",\n      \"parameters\": \"%s\",\n"
                "      \"scale\": %zu,\n      \"bytes\": %zu,\n      \"inputSha256\": \"%s\",\n      \"nodes\": %zu,\n"
                "      \"minParseNs\": %llu,\n      \"medianParseNs\": %llu,\n      \"minFreeNs\": %llu,\n"
                "      \"medianFreeNs\": %llu,\n      \"mbPerSecond\": %.3f,\n      \"nsPerNode\": %.3f,\n"
                "      \"rssDeltaKiB\": %ld,\n      \"minorFaultsPerParse\": %.1f,\n      \"samples\": [",
                run->json_cases ? ",\n" : "", input->name, input->generator, input->parameters, input->scale,
                input->length, input->sha256, nodes, (unsigned long long)min_parse, (unsigned long long)median_parse,
                (unsigned long long)min_free, (unsigned long long)median_free, mb_per_s, ns_per_node,
                rss_before >= 0 && rss_after >= 0 ? rss_after - rss_before : -1L, faults_per_parse);
        for (i = 0; i < repeats; i++) {
            fprintf(run->json, "%s{\"parseNs\": %llu, \"freeNs\": %llu}", i ? ", " : "",
                    (unsigned long long)samples[i].parse_ns, (unsigned long long)samples[i].free_ns);
        }
        fprintf(run->json, "]");
        if (options->reference) {
            fprintf(run->json,
                    ",\n      \"reference\": {\"implementation\": \"%s\", \"version\": \"%s\", \"minParseNs\": %llu,"
                    " \"medianParseNs\": %llu, \"minFreeNs\": %llu, \"medianFreeNs\": %llu}",
                    options->reference, BENCH_REFERENCE_VERSION, (unsigned long long)reference_min_parse,
                    (unsigned long long)reference_median_parse, (unsigned long long)reference_min_free,
                    (unsigned long long)reference_median_free);
        }
        fprintf(run->json, "\n    }");
        run->json_cases++;
    }
    return 0;
}

static int run_workload(const char *workload, const bench_options *options) {
    bench_run run;
    int result;
    memset(&run, 0, sizeof(run));
    run.options = options;
    run.workload = workload;
    if (options->json_path) {
        run.json = fopen(options->json_path, "wb");
        if (!run.json) {
            fprintf(stderr, "cannot write %s\n", options->json_path);
            return 1;
        }
        fprintf(run.json,
                "{\n  \"schema\": 2,\n  \"runtime\": \"c\",\n  \"lane\": \"timing\",\n  \"sourceSha\": \"%s\",\n"
                "  \"workload\": \"%s\",\n  \"workloadVersion\": %d,\n  \"warmup\": %d,\n  \"repeats\": %d,\n"
                "  \"allocator\": \"%s\",\n  \"cases\": [\n",
                options->source_sha ? options->source_sha : "", workload, bench_workload_version(workload),
                options->warmup, options->repeats > BENCH_MAX_REPEATS ? BENCH_MAX_REPEATS : options->repeats,
                options->allocator ? options->allocator : "default");
    }
    result = bench_workload_visit(workload, options->samples_dir, measure_case, &run);
    if (result == -2) {
        fprintf(stderr, "unknown workload: %s\n", workload);
    } else if (result == -1) {
        fprintf(stderr, "%s: cannot build input\n", workload);
    } else if (result == 0 && options->case_name && !run.matched) {
        fprintf(stderr, "unknown case: %s\n", options->case_name);
        result = -2;
    }
    if (run.json) {
        fprintf(run.json, "\n  ]\n}\n");
        fclose(run.json);
    }
    return result == 0 ? 0 : (result == -2 ? 2 : 1);
}

static int list_case(const bench_case *input, void *context) {
    (void)context;
    puts(input->name);
    return 0;
}

static int usage(void) {
    fputs("usage: bench_runner --list [--workload NAME --samples DIR]\n"
          "       bench_runner --workload NAME --samples DIR [--case NAME] [--repeats N] [--warmup N]\n"
          "                    [--json FILE] [--source-sha SHA] [--reference cmark]\n"
          "                    [--instructions [--implementation core|cmark]] [--dry-run]\n"
          "                    [--allocator default|retain]\n",
          stderr);
    return 2;
}

/* --allocator retain: glibc keeps what the parses free. Blocks below 32 MB
 * (the most mallopt allows) come from the heap rather than their own
 * mapping, the heap top is never trimmed, and it grows 64 MB at a time, so
 * a parse after a free runs on pages already in the process. Elsewhere the
 * lane is refused: the measurement is of glibc's retention. */
static int retain_allocator(void) {
#if defined(__GLIBC__)
    if (!mallopt(M_MMAP_THRESHOLD, 32 << 20) || !mallopt(M_TRIM_THRESHOLD, 512 << 20) ||
        !mallopt(M_TOP_PAD, 64 << 20)) {
        fputs("--allocator retain: mallopt refused the retention settings\n", stderr);
        return 2;
    }
    return 0;
#else
    fputs("--allocator retain needs glibc's mallopt; this process allocator has no retention setting\n", stderr);
    return 2;
#endif
}

int main(int argc, char **argv) {
    bench_options options;
    const char *workload_name = NULL;
    int list_only = 0;
    size_t i;

    memset(&options, 0, sizeof(options));
    options.repeats = BENCH_DEFAULT_REPEATS;
    options.warmup = BENCH_DEFAULT_WARMUP;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--workload") == 0 && i + 1 < (size_t)argc) {
            workload_name = argv[++i];
        } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < (size_t)argc) {
            options.samples_dir = argv[++i];
        } else if (strcmp(argv[i], "--repeats") == 0 && i + 1 < (size_t)argc) {
            options.repeats = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < (size_t)argc) {
            options.warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < (size_t)argc) {
            options.json_path = argv[++i];
        } else if (strcmp(argv[i], "--source-sha") == 0 && i + 1 < (size_t)argc) {
            options.source_sha = argv[++i];
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < (size_t)argc) {
            options.case_name = argv[++i];
        } else if (strcmp(argv[i], "--reference") == 0 && i + 1 < (size_t)argc) {
            options.reference = argv[++i];
        } else if (strcmp(argv[i], "--implementation") == 0 && i + 1 < (size_t)argc) {
            options.implementation = argv[++i];
        } else if (strcmp(argv[i], "--instructions") == 0) {
            options.instructions = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            options.dry_run = 1;
        } else if (strcmp(argv[i], "--allocator") == 0 && i + 1 < (size_t)argc) {
            options.allocator = argv[++i];
        } else {
            return usage();
        }
    }

    if (list_only) {
        if (workload_name) {
            if (!options.samples_dir) {
                return usage();
            }
            return bench_workload_visit(workload_name, options.samples_dir, list_case, NULL) == 0 ? 0 : 1;
        }
        for (i = 0; i < bench_workload_count(); i++) {
            puts(bench_workload_name(i));
        }
        return 0;
    }
    if (!workload_name || !options.samples_dir || options.repeats < 1 || options.warmup < 0) {
        return usage();
    }
    if (options.reference && strcmp(options.reference, "cmark") != 0) {
        fprintf(stderr, "unknown reference: %s (only cmark)\n", options.reference);
        return 2;
    }
    if (options.implementation && strcmp(options.implementation, "core") != 0 &&
        strcmp(options.implementation, "cmark") != 0) {
        fprintf(stderr, "unknown implementation: %s (core or cmark)\n", options.implementation);
        return 2;
    }
#ifndef MARKDOWN_CORE_BENCH_CMARK
    if (options.reference || (options.implementation && strcmp(options.implementation, "cmark") == 0)) {
        fputs("this bench_runner was built without the cmark reference (MARKDOWN_CORE_BENCH_CMARK=ON)\n", stderr);
        return 2;
    }
#endif
    if (options.allocator && strcmp(options.allocator, "default") != 0 && strcmp(options.allocator, "retain") != 0) {
        fprintf(stderr, "unknown allocator: %s (default or retain)\n", options.allocator);
        return 2;
    }
    if (options.allocator && strcmp(options.allocator, "retain") == 0) {
        int refused = retain_allocator();
        if (refused) {
            return refused;
        }
    }
    return run_workload(workload_name, &options);
}
