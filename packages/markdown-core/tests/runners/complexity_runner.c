/* Parser complexity suite.
 *
 * Measures parse time for directive attribute inputs at 4 KiB and 128 MiB,
 * then compares time per input byte.  The 32768x span makes n log n growth
 * visible without relying on absolute wall-clock thresholds.
 *
 *   complexity_runner --list
 *   complexity_runner --case NAME
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static const size_t SCALING_SIZES[] = {4096, 134217728};
#define SCALING_STEPS (sizeof(SCALING_SIZES) / sizeof(SCALING_SIZES[0]))
#define SCALING_REPEATS 3
#define MIN_SAMPLE_NS 25000000ULL
/* A linear parser has constant asymptotic work per input byte, but millions of
 * parsed nodes cross allocator and cache regimes that a 4 KiB sample does not.
 * The inherited qsort path measured 4.442x across these endpoints; reject at
 * 4.0x to catch that observed regression without treating memory hierarchy as
 * an algorithmic proof. Probe/collision tests enforce the hash-path bound. */
static const double MAX_NORMALIZED_SLOWDOWN = 4.0;

typedef char *(*cc_builder)(size_t size, size_t *length);

static char *cc_quoted_value(size_t size, size_t *length) {
    char *value = ts_repeat("a", size, NULL);
    char *input = NULL;
    if (!value) {
        return NULL;
    }
    *length = 8 + size + 2;
    input = (char *)malloc(*length + 1);
    if (input) {
        snprintf(input, *length + 1, ":x{key=\"%s\"}", value);
    }
    free(value);
    return input;
}

static char *cc_backslashes(size_t size, size_t *length) {
    char *value = ts_repeat("\\", size, NULL);
    char *input = NULL;
    if (!value) {
        return NULL;
    }
    *length = 8 + size + 2;
    input = (char *)malloc(*length + 1);
    if (input) {
        snprintf(input, *length + 1, ":x{key=\"%s\"}", value);
    }
    free(value);
    return input;
}

static char *cc_unclosed_quoted(size_t size, size_t *length) {
    char *value = ts_repeat("a", size, NULL);
    char *input = NULL;
    if (!value) {
        return NULL;
    }
    *length = 8 + size;
    input = (char *)malloc(*length + 1);
    if (input) {
        snprintf(input, *length + 1, ":x{key=\"%s", value);
    }
    free(value);
    return input;
}

static char *cc_unclosed_backslashes(size_t size, size_t *length) {
    char *value = ts_repeat("\\", size, NULL);
    char *input = NULL;
    if (!value) {
        return NULL;
    }
    *length = 8 + size;
    input = (char *)malloc(*length + 1);
    if (input) {
        snprintf(input, *length + 1, ":x{key=\"%s", value);
    }
    free(value);
    return input;
}

static char *cc_attributes(size_t size, size_t *length, int duplicates) {
    size_t attribute_count = size / 24 ? size / 24 : 1;
    size_t capacity = attribute_count * 24 + 16;
    char *input = (char *)malloc(capacity);
    size_t written = 0;
    size_t index;
    if (!input) {
        return NULL;
    }
    /* Each attribute needs at most strlen(" k<20 digits>=v") < 24 bytes, so
     * the initial capacity always suffices. */
    written += (size_t)snprintf(input + written, capacity - written, ":x{");
    for (index = 0; index < attribute_count; index++) {
        written += (size_t)snprintf(
            input + written,
            capacity - written,
            "%sk%zu=v",
            index ? " " : "",
            duplicates ? index % 64 : index
        );
    }
    written += (size_t)snprintf(input + written, capacity - written, "}");
    *length = written;
    return input;
}

static char *cc_unique_attributes(size_t size, size_t *length) { return cc_attributes(size, length, 0); }

static char *cc_duplicate_attributes(size_t size, size_t *length) { return cc_attributes(size, length, 1); }

static char *cc_references(size_t size, size_t *length, int duplicates) {
    size_t reference_count = size / 32 ? size / 32 : 1;
    size_t capacity = reference_count * 32 + 16;
    char *input = (char *)malloc(capacity);
    size_t written = 0;
    size_t index;
    if (!input) {
        return NULL;
    }
    for (index = 0; index < reference_count; index++) {
        written +=
            (size_t)snprintf(input + written, capacity - written, "[k%zu]: /u\n", duplicates ? index % 64 : index);
    }
    written += (size_t)snprintf(input + written, capacity - written, "\n[k0]\n");
    *length = written;
    return input;
}

static char *cc_unique_references(size_t size, size_t *length) { return cc_references(size, length, 0); }

static char *cc_duplicate_references(size_t size, size_t *length) { return cc_references(size, length, 1); }

/* Long Unicode labels: case folding is the dominant per-label cost of the one
 * key construction (#125), and folding rewrites these bytes for real -- 'ß'
 * widens to "ss" -- so the doubling gate watches the fold-heavy path stay
 * linear rather than timing an ASCII shortcut. */
static char *cc_unicode_references(size_t size, size_t *length) {
    static const char unicode_stem[] = "\xc3\x84\xc3\x96\xc3\x9c\xc3\x9f-\xc3\x84\xc3\x96\xc3\x9c\xc3\x9f-"
                                       "\xc3\x84\xc3\x96\xc3\x9c\xc3\x9f-\xc3\x84\xc3\x96\xc3\x9c\xc3\x9f-"
                                       "\xc3\x84\xc3\x96\xc3\x9c\xc3\x9f-\xc3\x84\xc3\x96\xc3\x9c\xc3\x9f";
    size_t reference_count = size / 96 ? size / 96 : 1;
    size_t capacity = reference_count * 96 + sizeof(unicode_stem) + 32;
    char *input = (char *)malloc(capacity);
    size_t written = 0;
    size_t index;
    if (!input) {
        return NULL;
    }
    for (index = 0; index < reference_count; index++) {
        written += (size_t)snprintf(input + written, capacity - written, "[%s-k%zu]: /u\n", unicode_stem, index);
    }
    written += (size_t)snprintf(input + written, capacity - written, "\n[%s-k0]\n", unicode_stem);
    *length = written;
    return input;
}

/* D9's second gate, and it is what Step 9b.2 was not allowed to give up.
 *
 * Resolving a reference USED TO COPY the definition's destination and title
 * into the node, so one definition with a long destination, referenced many
 * times, turned a small document into a large tree. The only thing bounding
 * that was `max_ref_size`, a running budget of `max(100000, input size)` bytes
 * summed over successful lookups, after which lookups simply failed — and that
 * budget was D9 itself, because it made whether a reference resolves depend on
 * how many resolved before it. Deleting it with nothing in its place measured
 * 68.7 GB of output from 1 MiB of input.
 *
 * Since 9b.2 a reference NAMES its definition: it carries the association it
 * was written with and no destination at all, so there is nothing to copy,
 * nothing to charge, and no budget. This gate stays because the bound must
 * stay STATED — and it counts what the tree actually stores now, the
 * association on every reference kind as well as the resource on every inline
 * link, so it cannot pass by measuring a field the model no longer has.
 *
 * Measured as a multiple of input size rather than an absolute, because the
 * point is the ratio. */
static const double MAX_REFERENCE_EXPANSION = 8.0;

typedef struct cc_expansion {
    size_t bytes;
} cc_expansion;

static int cc_expansion_visit(const markdown_core_node *node, void *context) {
    cc_expansion *total = (cc_expansion *)context;
    markdown_core_string first;
    markdown_core_string second;
    markdown_core_optional_string title;
    if (markdown_core_node_link_properties(node, &first, &title) ||
        markdown_core_node_image_properties(node, &first, &title)) {
        total->bytes += first.length + (title.has_value ? title.value.length : 0);
    } else if (markdown_core_node_association(node, &first, &second)) {
        total->bytes += first.length + second.length;
    }
    return 0;
}

/* One definition with a long destination, referenced until the input reaches
 * `size`. Every reference that resolves copies the destination, so the output
 * grows as (references x destination length) while the input grows as
 * (references x label length). */
static char *cc_reference_expansion(size_t size, size_t *length) {
    const size_t destination_length = 1024;
    char *destination = ts_repeat("u", destination_length, NULL);
    size_t reference_count = size / 8 ? size / 8 : 1;
    size_t capacity = destination_length + 16 + reference_count * 8 + 16;
    char *input = NULL;
    size_t written = 0;
    size_t index;
    if (!destination) {
        return NULL;
    }
    input = (char *)malloc(capacity);
    if (!input) {
        free(destination);
        return NULL;
    }
    written += (size_t)snprintf(input + written, capacity - written, "[a]: /%s\n\n", destination);
    free(destination);
    for (index = 0; index < reference_count; index++) {
        written += (size_t)snprintf(input + written, capacity - written, "[a]\n\n");
    }
    *length = written;
    return input;
}

static int cc_run_expansion(const char *name) {
    size_t length = 0;
    char *input = cc_reference_expansion(SCALING_SIZES[0] * 256, &length);
    markdown_core_parse_options options;
    cc_expansion total = {0};
    markdown_core_document *document;
    double ratio;
    int failed;

    if (!input) {
        fprintf(stderr, "cannot build input for %s\n", name);
        return -1;
    }
    ts_ast_options_none(&options);
    document = ts_ast_parse((const uint8_t *)input, length, &options);
    if (!document) {
        fprintf(stderr, "conversion failed for %s\n", name);
        free(input);
        return -1;
    }
    if (ts_ast_walk(markdown_core_document_semantic(document), cc_expansion_visit, &total) != 0) {
        markdown_core_document_free(document);
        free(input);
        return -1;
    }
    markdown_core_document_free(document);
    ratio = (double)total.bytes / (double)length;
    failed = ratio > MAX_REFERENCE_EXPANSION;
    printf(
        "%s ... %s (%zu input bytes, %zu bytes of resource and association payload, %.3fx)\n",
        name,
        failed ? "[FAILED reference expansion]" : "[PASSED]",
        length,
        total.bytes,
        ratio
    );
    free(input);
    return failed ? -1 : 0;
}

typedef struct cc_case_entry {
    const char *name;
    cc_builder build;
    /* WHETHER THE CASE READS THE TREE IT PARSED. Every case here parsed and
     * freed and nothing else, so a facade accessor could be quadratic and this
     * suite would report linear scaling -- which is exactly what happened:
     * `markdown_core_node_directive_attribute_at` walked from the head on every
     * index, and a 6.8 MB directive took 269 s to dump while `many_unique_
     * attributes` passed. Parsing is not the only thing that has to scale. */
    int reads_attributes;
} cc_case_entry;

static const cc_case_entry CC_CASES[] = {
    {"valid_long_quoted_value", cc_quoted_value, 0},
    {"valid_consecutive_backslashes", cc_backslashes, 0},
    {"unclosed_long_quoted_value", cc_unclosed_quoted, 0},
    {"unclosed_backslash_value", cc_unclosed_backslashes, 0},
    {"many_unique_attributes", cc_unique_attributes, 0},
    {"many_duplicate_attributes", cc_duplicate_attributes, 0},
    {"many_unique_references", cc_unique_references, 0},
    {"many_duplicate_references", cc_duplicate_references, 0},
    {"many_unicode_references", cc_unicode_references, 0},
    {"read_unique_attributes", cc_unique_attributes, 1},
    {"read_duplicate_attributes", cc_duplicate_attributes, 1},
};

/* Cases measured by output size rather than by time. */
static const char *const CC_EXPANSION_CASES[] = {"reference_expansion_bound"};

/* Reads every attribute of every directive through the PUBLIC accessor, which
 * is what a dump and all three bindings do. The values are discarded; the point
 * is that reading n of them costs O(n) and not O(n^2). */
static void cc_read_attributes(markdown_core_document *document) {
    const markdown_core_node *root = markdown_core_document_semantic(document);
    const markdown_core_node *paragraph = root ? markdown_core_node_get_first_child(root) : NULL;
    const markdown_core_node *node;
    for (node = paragraph ? markdown_core_node_get_first_child(paragraph) : NULL; node;
        node = markdown_core_node_get_next_sibling(node)) {
        markdown_core_string directive_name;
        bool has_attributes = false;
        size_t count = 0;
        size_t index;
        if (!markdown_core_node_directive_properties(node, &directive_name, &has_attributes, &count)) {
            continue;
        }
        for (index = 0; index < count; index++) {
            markdown_core_string name;
            markdown_core_string value;
            if (!markdown_core_node_directive_attribute_at(node, index, &name, &value)) {
                break;
            }
        }
    }
}

static int cc_measure(const char *input, size_t length, double *seconds, int reads_attributes) {
    double samples[SCALING_REPEATS];
    int repeat;
    markdown_core_parse_options options;
    ts_ast_options_none(&options);
    if (ts_ast_enable(&options, "directive") != 0) {
        return -1;
    }
    for (repeat = 0; repeat < SCALING_REPEATS; repeat++) {
        uint64_t started;
        uint64_t elapsed;
        size_t iterations = 0;
        started = ts_monotonic_ns();
        do {
            markdown_core_document *document = ts_ast_parse((const uint8_t *)input, length, &options);
            if (!document) {
                return -1;
            }
            if (reads_attributes) {
                cc_read_attributes(document);
            }
            markdown_core_document_free(document);
            iterations++;
            elapsed = ts_monotonic_ns() - started;
        } while (elapsed < MIN_SAMPLE_NS);
        samples[repeat] = (double)elapsed / (1e9 * (double)iterations);
        /* A single parse already gives a long, stable sample for the large
         * endpoint.  Avoid tripling 128 MiB work on slow/sanitized builds. */
        if (iterations == 1) {
            *seconds = samples[repeat];
            return 0;
        }
    }
    /* Median of three for short samples where scheduler noise matters. */
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
        if (cc_measure(input, length, &timings[step], entry->reads_attributes) != 0) {
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
        double normalized_slowdown = time_growth / input_growth;
        if (normalized_slowdown > MAX_NORMALIZED_SLOWDOWN) {
            failed = 1;
        }
        printf("%s ... %s (", entry->name, failed ? "[FAILED non-linear scaling]" : "[PASSED]");
        for (step = 0; step < SCALING_STEPS; step++) {
            printf("%s%zu bytes: %.6fs", step ? ", " : "", lengths[step], timings[step]);
        }
        printf(", normalized slowdown: %.3fx)\n", normalized_slowdown);
    }
    return failed ? -1 : 0;
}

int main(int argc, char **argv) {
    const char *case_name = NULL;
    int list_only = 0;
    size_t i;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < (size_t)argc) {
            case_name = argv[++i];
        } else {
            fputs("usage: complexity_runner [--list] [--case NAME]\n", stderr);
            return 2;
        }
    }

    if (list_only) {
        for (i = 0; i < sizeof(CC_CASES) / sizeof(CC_CASES[0]); i++) {
            puts(CC_CASES[i].name);
        }
        for (i = 0; i < sizeof(CC_EXPANSION_CASES) / sizeof(CC_EXPANSION_CASES[0]); i++) {
            puts(CC_EXPANSION_CASES[i]);
        }
        return 0;
    }
    if (!case_name) {
        fputs("usage: complexity_runner [--list] [--case NAME]\n", stderr);
        return 2;
    }
    for (i = 0; i < sizeof(CC_CASES) / sizeof(CC_CASES[0]); i++) {
        if (strcmp(CC_CASES[i].name, case_name) == 0) {
            return cc_run(&CC_CASES[i]) == 0 ? 0 : 1;
        }
    }
    for (i = 0; i < sizeof(CC_EXPANSION_CASES) / sizeof(CC_EXPANSION_CASES[0]); i++) {
        if (strcmp(CC_EXPANSION_CASES[i], case_name) == 0) {
            return cc_run_expansion(CC_EXPANSION_CASES[i]) == 0 ? 0 : 1;
        }
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
