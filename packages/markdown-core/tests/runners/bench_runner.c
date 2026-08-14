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
    document = markdown_core_document_new(mc_sv((const uint8_t *)input, length), NULL, &error);
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

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int bench_measure(
    const char *name,
    const char *input,
    size_t length,
    const bench_options *options,
    double *median_ms
) {
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
            " workload_version=1 bytes=%zu warmup=%d repeats=%d median_ns=%.0f peak_rss_kib=%ld\n",
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

static char *build_large_table(const bench_options *options, size_t scale, size_t *length) {
    static const char prefix[] = "| h1 | h2 | h3 | h4 | h5 | h6 | h7 | h8 |\n"
                                 "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
    static const char row[] = "| alpha \\| beta | gamma delta | epsilon zeta | eta theta | iota kappa |"
                              " lambda mu | nu xi | omicron pi |\n";
    const size_t prefix_length = sizeof(prefix) - 1;
    const size_t row_length = sizeof(row) - 1;
    char *input;
    size_t i;
    (void)options;
    if (scale > (SIZE_MAX - prefix_length - 1) / row_length) {
        return NULL;
    }
    *length = prefix_length + row_length * scale;
    input = (char *)malloc(*length + 1);
    if (!input) {
        return NULL;
    }
    memcpy(input, prefix, prefix_length);
    for (i = 0; i < scale; i++) {
        memcpy(input + prefix_length + i * row_length, row, row_length);
    }
    input[*length] = 0;
    return input;
}

static int workload_large_table(const bench_options *options) {
    static const size_t scales[] = {10000, 20000, 40000};
    return bench_doubling("large_table", options, build_large_table, scales, 3);
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

/* --- append baseline (streaming plan P0.2) -------------------------------
 *
 * The per-tick cost of consuming a stream: every append re-parses the
 * bytes-so-far, so a tick costs one full parse plus one whole-tree diff of
 * the prefix.
 *
 * Shapes follow the plan's list; each is measured at doubling prefix
 * checkpoints. A burst of token-sized, non-line-aligned ticks (3-8 byte
 * strides) runs at each checkpoint — a full trace at these sizes is
 * quadratic and would measure nothing extra. The doubling assertion holds
 * because a tick IS a full parse of the prefix: super-linear growth here is
 * a parser regression, not a streaming property. */

typedef char *(*append_shape_build)(size_t target, size_t *length);

static char *build_append_prose(size_t target, size_t *length) {
    static const char unit[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod\n"
                               "tempor incididunt ut labore et dolore magna aliqua ut enim ad minim.\n\n";
    return ts_repeat(unit, target / (sizeof(unit) - 1) + 1, length);
}

static char *build_append_nested_list(size_t target, size_t *length) {
    static const char unit[] = "- alpha item one\n  - beta item two\n    - gamma item three\n"
                               "      - delta item four\n- epsilon item five\n\n";
    return ts_repeat(unit, target / (sizeof(unit) - 1) + 1, length);
}

static char *build_append_fence(size_t target, size_t *length) {
    static const char unit[] = "let value = compute(index) + offset; // streamed code line\n";
    char *body = ts_repeat(unit, target / (sizeof(unit) - 1) + 1, length);
    char *input;
    if (!body) {
        return NULL;
    }
    /* One growing, never-closed fence: the shape the plan calls memcpy-speed
     * for the warm path, full-parse speed today. */
    input = (char *)malloc(*length + 5);
    if (!input) {
        free(body);
        return NULL;
    }
    memcpy(input, "```\n", 4);
    memcpy(input + 4, body, *length);
    input[*length + 4] = 0;
    *length += 4;
    free(body);
    return input;
}

static char *build_append_giant_paragraph(size_t target, size_t *length) {
    /* No blank line anywhere: one paragraph that never closes. */
    static const char unit[] = "words keep arriving and the paragraph never ends because no blank line\n";
    return ts_repeat(unit, target / (sizeof(unit) - 1) + 1, length);
}

/* Distinct labels, so density stresses the definition table rather than one
 * bucket's duplicate chain. */
static char *build_append_footnote_dense(size_t target, size_t *length) {
    size_t capacity = target + 128;
    char *input = (char *)malloc(capacity + 1);
    size_t used = 0;
    size_t n = 0;
    if (!input) {
        return NULL;
    }
    while (used < target) {
        int wrote = snprintf(
            input + used,
            capacity - used,
            "Mention[^n%zu] rides along.\n\n[^n%zu]: The matching note body.\n\n",
            n,
            n
        );
        if (wrote <= 0 || (size_t)wrote >= capacity - used) {
            break;
        }
        used += (size_t)wrote;
        n++;
    }
    input[used] = 0;
    *length = used;
    return input;
}

static char *build_append_references_appendix(size_t target, size_t *length) {
    size_t capacity = target + 128;
    char *input = (char *)malloc(capacity + 1);
    size_t used = 0;
    size_t n = 0;
    if (!input) {
        return NULL;
    }
    /* Consecutive definitions with no blank line: one growing paragraph the
     * harvest re-consumes whole every tick — the plan's named quadratic. */
    while (used < target) {
        int wrote = snprintf(input + used, capacity - used, "[r%zu]: /url/%zu\n", n, n);
        if (wrote <= 0 || (size_t)wrote >= capacity - used) {
            break;
        }
        used += (size_t)wrote;
        n++;
    }
    input[used] = 0;
    *length = used;
    return input;
}

/* One measured append tick through the REAL mutation: hand only the chunk
 * over and swap the handle. Only `append` is timed; releasing the
 * predecessor stays outside the window. */
static int append_tick(markdown_core_document **document, const char *chunk, size_t length, uint64_t *nanoseconds) {
    markdown_core_error *error = NULL;
    uint64_t started = ts_monotonic_ns();
    markdown_core_document *successor =
        markdown_core_document_append(*document, mc_sv((const uint8_t *)chunk, length), &error);
    uint64_t elapsed = ts_monotonic_ns() - started;
    if (!successor) {
        markdown_core_error_free(error);
        return -1;
    }
    markdown_core_document_free(*document);
    *document = successor;
    if (nanoseconds) {
        *nanoseconds = elapsed;
    }
    return 0;
}

#define APPEND_TICKS_PER_CHECKPOINT 5
#define APPEND_WARMUP_TICKS 1
#define APPEND_CHECKPOINTS 5

static int bench_append_shape(const char *name, append_shape_build build, const bench_options *options) {
    static const size_t checkpoints[APPEND_CHECKPOINTS] =
        {256 * 1024, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024};
    const size_t steps = APPEND_CHECKPOINTS;
    size_t text_length = 0;
    char *text = build(checkpoints[steps - 1] + 4096, &text_length);
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    ts_prng prng;
    double medians_ms[APPEND_CHECKPOINTS];
    double ratios[APPEND_CHECKPOINTS - 1];
    size_t step;
    int failed = 0;

    (void)options;
    if (!text || text_length < checkpoints[steps - 1]) {
        fprintf(stderr, "%s: cannot build input\n", name);
        free(text);
        return -1;
    }
    ts_prng_seed(&prng, UINT64_C(0xa99e4dba5e11e5) ^ (uint64_t)name[0]);
    document = markdown_core_document_new(mc_sv(NULL, 0), NULL, &error);
    if (!document) {
        markdown_core_error_free(error);
        free(text);
        return -1;
    }

    size_t sent = 0;
    for (step = 0; step < steps && !failed; step++) {
        uint64_t samples[APPEND_TICKS_PER_CHECKPOINT];
        size_t offset = checkpoints[step];
        char case_name[128];
        int tick;

        /* Fast-forward to the checkpoint with one jump chunk, then run the
         * burst: warmup ticks settle the allocator at this working-set size
         * before anything is recorded. */
        if (append_tick(&document, text + sent, offset - sent, NULL) != 0) {
            failed = 1;
            break;
        }
        sent = offset;
        for (tick = -APPEND_WARMUP_TICKS; tick < APPEND_TICKS_PER_CHECKPOINT; tick++) {
            uint64_t *slot = tick < 0 ? NULL : &samples[tick];
            size_t step_bytes = 3 + (size_t)(ts_prng_next(&prng) % 6);
            if (step_bytes > text_length - sent) {
                step_bytes = text_length - sent;
            }
            if (append_tick(&document, text + sent, step_bytes, slot) != 0) {
                failed = 1;
                break;
            }
            sent += step_bytes;
        }
        if (failed) {
            break;
        }
        qsort(samples, APPEND_TICKS_PER_CHECKPOINT, sizeof(samples[0]), compare_u64);
        medians_ms[step] = (double)samples[APPEND_TICKS_PER_CHECKPOINT / 2] / 1e6;
        snprintf(case_name, sizeof(case_name), "append_%s@%zu", name, checkpoints[step]);
        printf(
            "benchmark case=%s bytes=%zu repeats=%d warmup=%d median_ms=%.3f\n",
            case_name,
            checkpoints[step],
            APPEND_TICKS_PER_CHECKPOINT,
            APPEND_WARMUP_TICKS,
            medians_ms[step]
        );
    }

    /* The gate is the MEDIAN of the adjacent-doubling growth ratios, not any
     * single pair: an allocator size-class or cache-level transition puts one
     * step's ratio far above its neighbours while every other interval stays
     * linear, and a two-point ratio cannot tell that from an asymptote — the
     * footnote-renumber complexity gate failed exactly this way before it
     * moved to the same median form (docs/specs/test-architecture.md).
     * Sustained super-linear growth moves every interval, so it moves the
     * median; an isolated transition cannot. */
    if (!failed) {
        double sorted[APPEND_CHECKPOINTS - 1];
        double median_ratio;
        for (step = 1; step < steps; step++) {
            double floor_ms = medians_ms[step - 1] > 0.0005 ? medians_ms[step - 1] : 0.0005;
            ratios[step - 1] = medians_ms[step] / floor_ms;
        }
        memcpy(sorted, ratios, sizeof(sorted));
        qsort(sorted, steps - 1, sizeof(sorted[0]), compare_double);
        median_ratio = sorted[(steps - 1) / 2];
        if (median_ratio > BENCH_MAX_DOUBLING_RATIO) {
            fprintf(
                stderr,
                "%s: median per-tick doubling ratio %.2f exceeds %.2f\n",
                name,
                median_ratio,
                BENCH_MAX_DOUBLING_RATIO
            );
            failed = 1;
        }
    }

    markdown_core_document_free(document);
    free(text);
    return failed ? -1 : 0;
}

static int workload_append_baseline(const bench_options *options) {
    int failed = 0;
    failed |= bench_append_shape("prose", build_append_prose, options) != 0;
    failed |= bench_append_shape("nested_list", build_append_nested_list, options) != 0;
    failed |= bench_append_shape("fence", build_append_fence, options) != 0;
    failed |= bench_append_shape("footnote_dense", build_append_footnote_dense, options) != 0;
    failed |= bench_append_shape("giant_paragraph", build_append_giant_paragraph, options) != 0;
    failed |= bench_append_shape("references_appendix", build_append_references_appendix, options) != 0;
    if (!failed) {
        printf("append_baseline peak_rss_kib=%ld\n", peak_rss_kib());
    }
    return failed ? -1 : 0;
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
    {"large_table", workload_large_table},
    {"adversarial", workload_adversarial},
    {"append_baseline", workload_append_baseline},
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
