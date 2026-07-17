/* Benchmark workloads (CTest label: benchmark).
 *
 * Every workload is deterministic and offline: inputs come from the tracked
 * sample documents or are synthesized in-process; nothing is downloaded or
 * written to the source tree.  Timings are reported for trend tracking; the
 * only assertions are completion and relative scaling ratios across doubling
 * input sizes — never absolute wall-clock thresholds.
 *
 *   bench_runner --list
 *   bench_runner --workload NAME [--samples DIR] [--repeats N] [--warmup N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <markdown_core.h>

#include "test_support.h"

#define BENCH_MAX_REPEATS 32
#define BENCH_DEFAULT_REPEATS 5
#define BENCH_DEFAULT_WARMUP 1
/* Adjacent doubling steps may regress at most this factor before the
 * workload fails; generous enough to absorb scheduler noise while still
 * catching super-linear blowups. */
static const double BENCH_MAX_DOUBLING_RATIO = 4.0;

typedef struct bench_options {
    const char *samples_dir;
    int repeats;
    int warmup;
} bench_options;

static const char *const BENCH_SAMPLES[] = {
    "block-bq-flat.md",  "block-bq-nested.md",   "block-code.md",          "block-fences.md",    "block-heading.md",
    "block-hr.md",       "block-html.md",        "block-lheading.md",      "block-list-flat.md", "block-list-nested.md",
    "block-ref-flat.md", "block-ref-nested.md",  "directive.md",           "inline-autolink.md", "inline-backticks.md",
    "inline-em-flat.md", "inline-em-nested.md",  "inline-em-worst.md",     "inline-entity.md",   "inline-escape.md",
    "inline-html.md",    "inline-links-flat.md", "inline-links-nested.md", "inline-newlines.md", "lorem1.md",
    "rawtabs.md",
};

static char *load_sample(const bench_options *options, const char *name, size_t *length) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", options->samples_dir, name);
    return (char *)ts_read_file(path, length);
}

/* Concatenates every tracked sample once; the result is the deterministic
 * building block for the large-document workloads. */
static char *build_sample_block(const bench_options *options, size_t *length) {
    char *block = NULL;
    size_t block_length = 0;
    size_t i;
    for (i = 0; i < sizeof(BENCH_SAMPLES) / sizeof(BENCH_SAMPLES[0]); i++) {
        size_t sample_length = 0;
        char *sample = load_sample(options, BENCH_SAMPLES[i], &sample_length);
        char *grown;
        if (!sample) {
            free(block);
            return NULL;
        }
        grown = (char *)realloc(block, block_length + sample_length + 2);
        if (!grown) {
            free(sample);
            free(block);
            return NULL;
        }
        block = grown;
        memcpy(block + block_length, sample, sample_length);
        block_length += sample_length;
        block[block_length++] = '\n';
        block[block_length] = 0;
        free(sample);
    }
    *length = block_length;
    return block;
}

static char *repeat_block(const char *block, size_t block_length, size_t times, size_t *length) {
    char *buffer = (char *)malloc(block_length * times + 1);
    size_t i;
    if (!buffer) {
        return NULL;
    }
    for (i = 0; i < times; i++) {
        memcpy(buffer + i * block_length, block, block_length);
    }
    buffer[block_length * times] = 0;
    *length = block_length * times;
    return buffer;
}

static int bench_parse_once(const char *input, size_t length, uint64_t *nanoseconds) {
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    uint64_t started = ts_monotonic_ns();
    document = markdown_core_document_parse((const uint8_t *)input, length, NULL, &error);
    *nanoseconds = ts_monotonic_ns() - started;
    if (!document) {
        markdown_core_error_free(error);
        return -1;
    }
    markdown_core_document_free(document);
    return 0;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int
bench_measure(const char *name, const char *input, size_t length, const bench_options *options, double *median_ms) {
    uint64_t samples[BENCH_MAX_REPEATS];
    uint64_t elapsed;
    int i;
    int repeats = options->repeats;
    if (repeats > BENCH_MAX_REPEATS) {
        repeats = BENCH_MAX_REPEATS;
    }
    for (i = 0; i < options->warmup; i++) {
        if (bench_parse_once(input, length, &elapsed) != 0) {
            fprintf(stderr, "%s: parse failed\n", name);
            return -1;
        }
    }
    for (i = 0; i < repeats; i++) {
        if (bench_parse_once(input, length, &samples[i]) != 0) {
            fprintf(stderr, "%s: parse failed\n", name);
            return -1;
        }
    }
    qsort(samples, (size_t)repeats, sizeof(samples[0]), compare_u64);
    *median_ms = (double)samples[repeats / 2] / 1e6;
    printf(
        "benchmark case=%s bytes=%zu repeats=%d warmup=%d median_ms=%.3f\n",
        name,
        length,
        repeats,
        options->warmup,
        *median_ms
    );
    return 0;
}

/* Measures the same generator at doubling scales and asserts the relative
 * growth stays under BENCH_MAX_DOUBLING_RATIO per step. */
static int bench_doubling(
    const char *name,
    const bench_options *options,
    char *(*build)(const bench_options *options, size_t scale, size_t *length),
    const size_t *scales,
    size_t steps
) {
    double previous_ms = 0.0;
    size_t step;
    int failed = 0;
    for (step = 0; step < steps; step++) {
        size_t length = 0;
        char *input = build(options, scales[step], &length);
        double median_ms = 0.0;
        char case_name[128];
        if (!input) {
            fprintf(stderr, "%s: cannot build input\n", name);
            return -1;
        }
        snprintf(case_name, sizeof(case_name), "%s@%zu", name, scales[step]);
        if (bench_measure(case_name, input, length, options, &median_ms) != 0) {
            free(input);
            return -1;
        }
        free(input);
        if (step > 0) {
            double floor_ms = previous_ms > 0.0005 ? previous_ms : 0.0005;
            if (median_ms / floor_ms > BENCH_MAX_DOUBLING_RATIO) {
                fprintf(
                    stderr,
                    "%s: scaling ratio %.2f exceeds %.2f at scale %zu\n",
                    name,
                    median_ms / floor_ms,
                    BENCH_MAX_DOUBLING_RATIO,
                    scales[step]
                );
                failed = 1;
            }
        }
        previous_ms = median_ms;
    }
    return failed ? -1 : 0;
}

/* Workloads ---------------------------------------------------------------- */

static int workload_representative(const bench_options *options) {
    size_t i;
    for (i = 0; i < sizeof(BENCH_SAMPLES) / sizeof(BENCH_SAMPLES[0]); i++) {
        size_t sample_length = 0;
        char *sample = load_sample(options, BENCH_SAMPLES[i], &sample_length);
        size_t input_length = 0;
        char *input;
        double median_ms;
        int result;
        if (!sample) {
            fprintf(stderr, "cannot read sample %s\n", BENCH_SAMPLES[i]);
            return -1;
        }
        input = repeat_block(sample, sample_length, 200, &input_length);
        free(sample);
        if (!input) {
            return -1;
        }
        result = bench_measure(BENCH_SAMPLES[i], input, input_length, options, &median_ms);
        free(input);
        if (result != 0) {
            return -1;
        }
    }
    return 0;
}

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

static int workload_binding_baseline(const bench_options *options) {
    static const char unit[] = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
    size_t length = 0;
    char *input = repeat_block(unit, sizeof(unit) - 1, 2000, &length);
    double median_ms = 0.0;
    int result;
    if (!input) {
        return -1;
    }
    result = bench_measure("binding_baseline", input, length, options, &median_ms);
    free(input);
    if (result == 0) {
        printf(
            "baseline runtime=c boundary=native_parse workload=representative_large"
            " bytes=%zu warmup=%d repeats=%d median_ns=%.0f peak_rss_kib=%ld\n",
            length,
            options->warmup,
            options->repeats,
            median_ms * 1e6,
            peak_rss_kib()
        );
    }
    return result;
}

static char *build_large_document(const bench_options *options, size_t scale, size_t *length) {
    size_t block_length = 0;
    char *block = build_sample_block(options, &block_length);
    char *input;
    if (!block) {
        return NULL;
    }
    input = repeat_block(block, block_length, scale, length);
    free(block);
    return input;
}

static int workload_large_document(const bench_options *options) {
    /* The x512 step concatenates the sample block to roughly the size of the
     * retired 11MB Pro Git corpus, keeping large-input coverage equivalent. */
    static const size_t scales[] = {128, 256, 512};
    return bench_doubling("large_document", options, build_large_document, scales, 3);
}

static char *build_deep_nesting(const bench_options *options, size_t scale, size_t *length) {
    (void)options;
    char *quotes = ts_repeat("> ", scale, length);
    char *input;
    if (!quotes) {
        return NULL;
    }
    input = (char *)malloc(*length + 2);
    if (!input) {
        free(quotes);
        return NULL;
    }
    memcpy(input, quotes, *length);
    input[*length] = 'a';
    input[*length + 1] = 0;
    *length += 1;
    free(quotes);
    return input;
}

static int workload_deep_nesting(const bench_options *options) {
    static const size_t scales[] = {8192, 16384, 32768};
    return bench_doubling("deep_nesting", options, build_deep_nesting, scales, 3);
}

static char *build_extension_document(const bench_options *options, size_t scale, size_t *length) {
    size_t sample_length = 0;
    char *sample = load_sample(options, "directive.md", &sample_length);
    char *input;
    if (!sample) {
        return NULL;
    }
    input = repeat_block(sample, sample_length, scale, length);
    free(sample);
    return input;
}

static int workload_extensions(const bench_options *options) {
    static const size_t scales[] = {100, 200, 400};
    return bench_doubling("extensions", options, build_extension_document, scales, 3);
}

static char *build_adversarial_links(const bench_options *options, size_t scale, size_t *length) {
    (void)options;
    return ts_repeat("[a](b", scale, length);
}

static char *build_adversarial_emphasis(const bench_options *options, size_t scale, size_t *length) {
    (void)options;
    return ts_repeat("*a_ ", scale, length);
}

static int workload_adversarial(const bench_options *options) {
    static const size_t scales[] = {16384, 32768, 65536};
    if (bench_doubling("adversarial_links", options, build_adversarial_links, scales, 3) != 0) {
        return -1;
    }
    return bench_doubling("adversarial_emphasis", options, build_adversarial_emphasis, scales, 3);
}

typedef struct bench_workload {
    const char *name;
    int (*run)(const bench_options *options);
} bench_workload;

static const bench_workload WORKLOADS[] = {
    {"binding_baseline", workload_binding_baseline},
    {"representative", workload_representative},
    {"large_document", workload_large_document},
    {"deep_nesting", workload_deep_nesting},
    {"extensions", workload_extensions},
    {"adversarial", workload_adversarial},
};

int main(int argc, char **argv) {
    bench_options options;
    const char *workload_name = NULL;
    int list_only = 0;
    size_t i;

    options.samples_dir = NULL;
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
        } else {
            fputs(
                "usage: bench_runner --list | --workload NAME [--samples DIR]"
                " [--repeats N] [--warmup N]\n",
                stderr
            );
            return 2;
        }
    }

    if (list_only) {
        for (i = 0; i < sizeof(WORKLOADS) / sizeof(WORKLOADS[0]); i++) {
            puts(WORKLOADS[i].name);
        }
        return 0;
    }
    if (!workload_name || !options.samples_dir || options.repeats < 1 || options.warmup < 0) {
        fputs(
            "usage: bench_runner --list | --workload NAME [--samples DIR]"
            " [--repeats N] [--warmup N]\n",
            stderr
        );
        return 2;
    }
    for (i = 0; i < sizeof(WORKLOADS) / sizeof(WORKLOADS[0]); i++) {
        if (strcmp(WORKLOADS[i].name, workload_name) == 0) {
            return WORKLOADS[i].run(&options) == 0 ? 0 : 1;
        }
    }
    fprintf(stderr, "unknown workload: %s\n", workload_name);
    return 2;
}
