/* Directive attribute scanner complexity suite.
 *
 * Measures parse time for directive attribute inputs across widely separated
 * sizes and asserts near-linear scaling with relative ratios only; no absolute
 * wall-clock thresholds.
 *
 *   complexity_runner --list
 *   complexity_runner --case NAME
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static const size_t SCALING_SIZES[] = {4096, 65536, 1048576, 16777216};
#define SCALING_STEPS (sizeof(SCALING_SIZES) / sizeof(SCALING_SIZES[0]))
#define SCALING_REPEATS 3
#define MIN_SAMPLE_NS 25000000ULL
/* A linear parser has constant time per input byte.  Judge only the endpoints:
 * adjacent ratios remain sensitive to a scheduler pause in one short sample,
 * while the 4096x range leaves orders of magnitude between this allowance and
 * quadratic growth.  Intermediate timings remain useful diagnostics. */
static const double MAX_NORMALIZED_SLOWDOWN = 4.0;

typedef char *(*cc_builder)(size_t size, size_t *length);

static char *cc_quoted_value(size_t size, size_t *length) {
    char *value = ts_repeat("a", size, NULL);
    char *input = NULL;
    if (!value)
        return NULL;
    *length = 8 + size + 2;
    input = (char *)malloc(*length + 1);
    if (input)
        snprintf(input, *length + 1, ":x{key=\"%s\"}", value);
    free(value);
    return input;
}

static char *cc_backslashes(size_t size, size_t *length) {
    char *value = ts_repeat("\\", size, NULL);
    char *input = NULL;
    if (!value)
        return NULL;
    *length = 8 + size + 2;
    input = (char *)malloc(*length + 1);
    if (input)
        snprintf(input, *length + 1, ":x{key=\"%s\"}", value);
    free(value);
    return input;
}

static char *cc_unclosed_quoted(size_t size, size_t *length) {
    char *value = ts_repeat("a", size, NULL);
    char *input = NULL;
    if (!value)
        return NULL;
    *length = 8 + size;
    input = (char *)malloc(*length + 1);
    if (input)
        snprintf(input, *length + 1, ":x{key=\"%s", value);
    free(value);
    return input;
}

static char *cc_unclosed_backslashes(size_t size, size_t *length) {
    char *value = ts_repeat("\\", size, NULL);
    char *input = NULL;
    if (!value)
        return NULL;
    *length = 8 + size;
    input = (char *)malloc(*length + 1);
    if (input)
        snprintf(input, *length + 1, ":x{key=\"%s", value);
    free(value);
    return input;
}

static char *cc_attributes(size_t size, size_t *length, int duplicates) {
    size_t attribute_count = size / 24 ? size / 24 : 1;
    size_t capacity = attribute_count * 24 + 16;
    char *input = (char *)malloc(capacity);
    size_t written = 0;
    size_t index;
    if (!input)
        return NULL;
    /* Each attribute needs at most strlen(" k<20 digits>=v") < 24 bytes, so
     * the initial capacity always suffices. */
    written += (size_t)snprintf(input + written, capacity - written, ":x{");
    for (index = 0; index < attribute_count; index++)
        written += (size_t)snprintf(input + written, capacity - written, "%sk%zu=v", index ? " " : "",
                                    duplicates ? index % 64 : index);
    written += (size_t)snprintf(input + written, capacity - written, "}");
    *length = written;
    return input;
}

static char *cc_unique_attributes(size_t size, size_t *length) { return cc_attributes(size, length, 0); }

static char *cc_duplicate_attributes(size_t size, size_t *length) { return cc_attributes(size, length, 1); }

typedef struct cc_case_entry {
    const char *name;
    cc_builder build;
} cc_case_entry;

static const cc_case_entry CC_CASES[] = {
    {"valid_long_quoted_value", cc_quoted_value},       {"valid_consecutive_backslashes", cc_backslashes},
    {"unclosed_long_quoted_value", cc_unclosed_quoted}, {"unclosed_backslash_value", cc_unclosed_backslashes},
    {"many_unique_attributes", cc_unique_attributes},   {"many_duplicate_attributes", cc_duplicate_attributes},
};

static int cc_measure(const char *input, size_t length, double *seconds) {
    double samples[SCALING_REPEATS];
    int repeat;
    markdown_core_parse_options options;
    ts_ast_options_none(&options);
    if (ts_ast_enable(&options, "directive") != 0)
        return -1;
    for (repeat = 0; repeat < SCALING_REPEATS; repeat++) {
        uint64_t started;
        uint64_t elapsed;
        size_t iterations = 0;
        started = ts_monotonic_ns();
        do {
            markdown_core_document *document = ts_ast_parse((const uint8_t *)input, length, &options);
            if (!document)
                return -1;
            markdown_core_document_free(document);
            iterations++;
            elapsed = ts_monotonic_ns() - started;
        } while (elapsed < MIN_SAMPLE_NS);
        samples[repeat] = (double)elapsed / (1e9 * (double)iterations);
    }
    /* median of three */
    {
        double a = samples[0], b = samples[1], c = samples[2];
        double high = a > b ? (a > c ? a : c) : (b > c ? b : c);
        double low = a < b ? (a < c ? a : c) : (b < c ? b : c);
        *seconds = a + b + c - high - low;
    }
    return 0;
}

static int cc_run(const cc_case_entry *entry) {
    size_t lengths[SCALING_STEPS];
    double timings[SCALING_STEPS];
    size_t step;
    int failed = 0;

    for (step = 0; step < SCALING_STEPS; step++) {
        size_t length = 0;
        char *input = entry->build(SCALING_SIZES[step], &length);
        if (!input) {
            fprintf(stderr, "cannot build input for %s\n", entry->name);
            return -1;
        }
        if (cc_measure(input, length, &timings[step]) != 0) {
            fprintf(stderr, "conversion failed for %s\n", entry->name);
            free(input);
            return -1;
        }
        lengths[step] = length;
        free(input);
    }

    {
        double input_growth = (double)lengths[SCALING_STEPS - 1] / (double)lengths[0];
        double time_growth = timings[SCALING_STEPS - 1] / timings[0];
        if (time_growth > input_growth * MAX_NORMALIZED_SLOWDOWN)
            failed = 1;
    }

    printf("%s ... %s (", entry->name, failed ? "[FAILED non-linear scaling]" : "[PASSED]");
    for (step = 0; step < SCALING_STEPS; step++)
        printf("%s%zu bytes: %.6fs", step ? ", " : "", lengths[step], timings[step]);
    printf(")\n");
    return failed ? -1 : 0;
}

int main(int argc, char **argv) {
    const char *case_name = NULL;
    int list_only = 0;
    size_t i;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0)
            list_only = 1;
        else if (strcmp(argv[i], "--case") == 0 && i + 1 < (size_t)argc)
            case_name = argv[++i];
        else {
            fputs("usage: complexity_runner [--list] [--case NAME]\n", stderr);
            return 2;
        }
    }

    if (list_only) {
        for (i = 0; i < sizeof(CC_CASES) / sizeof(CC_CASES[0]); i++)
            puts(CC_CASES[i].name);
        return 0;
    }
    if (!case_name) {
        fputs("usage: complexity_runner [--list] [--case NAME]\n", stderr);
        return 2;
    }
    for (i = 0; i < sizeof(CC_CASES) / sizeof(CC_CASES[0]); i++) {
        if (strcmp(CC_CASES[i].name, case_name) == 0)
            return cc_run(&CC_CASES[i]) == 0 ? 0 : 1;
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
