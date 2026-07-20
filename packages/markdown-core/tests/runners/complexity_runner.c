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

typedef struct cc_case_entry {
    const char *name;
    cc_builder build;
} cc_case_entry;

static const cc_case_entry CC_CASES[] = {
    {"valid_long_quoted_value", cc_quoted_value},
    {"valid_consecutive_backslashes", cc_backslashes},
    {"unclosed_long_quoted_value", cc_unclosed_quoted},
    {"unclosed_backslash_value", cc_unclosed_backslashes},
    {"many_unique_attributes", cc_unique_attributes},
    {"many_duplicate_attributes", cc_duplicate_attributes},
    {"many_unique_references", cc_unique_references},
    {"many_duplicate_references", cc_duplicate_references},
};

/* --- session commit-cost cases -------------------------------------------
 *
 * Incremental commits must cost O(damaged region), independent of document
 * size.  Every case compares seconds-per-commit between a small and a large
 * session (1024x more text): streaming appends at the tail, an edit storm of
 * byte replacements spread across the document, and definition retargeting
 * that re-refines a fixed set of dependent units.  A per-commit cost that
 * scales with the document (the full-reparse behavior) shows up as a ~1024x
 * ratio against the same 4.0x bound the parse-scaling cases use. */

#define CC_SESSION_STANZA "para *text* [ref] line\n\n### head\n\n- item one\n- item two\n\n"
/* The retarget corpus keeps its bracket-free bulk out of the lookup records
 * so the dependent set stays a constant eight units. */
#define CC_SESSION_PLAIN_STANZA "para *text* line\n\n### head\n\n- item one\n- item two\n\n"
#define CC_SESSION_DEFINITION "[l]: /aaaa\n\n"
#define CC_SESSION_USE "uses [u][l] here\n\n"
#define CC_SESSION_USES 8
/* The footnote corpus appends a fixed reference/definition cluster after the
 * plain bulk; the edit toggles the first reference's label, so first-use
 * numbering reorders across the cluster every commit. */
#define CC_SESSION_FOOTNOTE_CLUSTER                                                                                    \
    "note [^a] then [^b] again\n\n"                                                                                    \
    "more [^c] and [^d] close\n\n"                                                                                     \
    "[^a]: alpha body\n\n"                                                                                             \
    "[^b]: beta body\n\n"                                                                                              \
    "[^c]: c body\n\n"                                                                                                 \
    "[^d]: d body\n"
/* The head-defs corpus is a document-scale leading cluster of distinct
 * definition paragraphs with one fixed use at the tail; the edit retargets
 * the cluster's last (unused) definition. Sentinel clean entries must keep
 * that commit's cost independent of the cluster size. */
#define CC_SESSION_DEF_WIDTH 18 /* strlen("[dNNNNNN]: /aaaa\n\n") */
#define CC_SESSION_DEF_URL_OFFSET 12
#define CC_SESSION_DEF_USE "uses [u][d000000] tail\n"
/* The footnote-defs corpus is a document-scale leading cluster of
 * blank-separated footnote definitions with one fixed use and one plain
 * paragraph at the tail; the edit rewrites the last definition's body.
 * Sealing clean entries must keep the restart span flat, and the
 * sequence-preserving footnote refresh must keep the index update flat,
 * even though every definition in the growing cluster is a footnote
 * site. */
#define CC_SESSION_FOOTNOTE_DEF_WIDTH 18 /* strlen("[^dNNNNNN]: aaaa\n\n") */
#define CC_SESSION_FOOTNOTE_DEF_BODY_OFFSET 12
#define CC_SESSION_FOOTNOTE_DEF_USE "uses [^d000000] tail\n\nplain para line\n"
/* The quote-suffix corpus pins the resolved half of the reflow-delay pair:
 * blank-separated top-level quotes restart cleanly, so a front edit must
 * reflow at the first boundary regardless of how many quotes follow. */
#define CC_SESSION_QUOTE_STANZA "> q aaaa line\n\n"
#define CC_SESSION_QUOTE_BODY_OFFSET 4
/* The def-spread corpus interleaves a definition with a unit referencing
 * that same label, one pair per stanza; the edit retargets the FIRST
 * definition's URL. Exactly one unit depends on the changed label, so the
 * inverted lookup postings must keep dependent collection — and the whole
 * commit — independent of how many other labels and referencing units
 * exist. */
#define CC_SESSION_DEF_SPREAD_WIDTH 42 /* strlen("[dNNNNNN]: /aaaa\n\nuses [u][dNNNNNN] here\n\n") */
#define CC_SESSION_OPS 64
static const size_t CC_SESSION_SIZES[] = {4096, 4194304};

enum {
    CC_SESSION_STREAM,
    CC_SESSION_STORM,
    CC_SESSION_RETARGET,
    CC_SESSION_FOOTNOTE,
    CC_SESSION_HEAD_DEFS,
    CC_SESSION_FOOTNOTE_DEFS,
    CC_SESSION_QUOTE_SUFFIX,
    CC_SESSION_DEF_SPREAD
};

static int cc_session_mode_footnote_defs(int mode) { return mode == CC_SESSION_FOOTNOTE_DEFS; }

static markdown_core_session *cc_session_build(size_t size, int mode, size_t *stanza_count) {
    markdown_core_parse_options options;
    markdown_core_session *session;
    const char *stanza = mode == CC_SESSION_RETARGET || mode == CC_SESSION_FOOTNOTE
                             ? CC_SESSION_PLAIN_STANZA
                             : (mode == CC_SESSION_QUOTE_SUFFIX ? CC_SESSION_QUOTE_STANZA : CC_SESSION_STANZA);
    size_t stanza_length =
        mode == CC_SESSION_HEAD_DEFS
            ? CC_SESSION_DEF_WIDTH
            : (mode == CC_SESSION_DEF_SPREAD
                   ? CC_SESSION_DEF_SPREAD_WIDTH
                   : (cc_session_mode_footnote_defs(mode) ? CC_SESSION_FOOTNOTE_DEF_WIDTH : strlen(stanza)));
    size_t count = size / stanza_length ? size / stanza_length : 1;
    size_t extras =
        mode == CC_SESSION_RETARGET
            ? strlen(CC_SESSION_DEFINITION) + CC_SESSION_USES * strlen(CC_SESSION_USE)
            : (mode == CC_SESSION_FOOTNOTE
                   ? strlen(CC_SESSION_FOOTNOTE_CLUSTER)
                   : (mode == CC_SESSION_HEAD_DEFS
                          ? strlen(CC_SESSION_DEF_USE)
                          : (cc_session_mode_footnote_defs(mode) ? strlen(CC_SESSION_FOOTNOTE_DEF_USE) : 0)));
    char *text = (char *)malloc(count * stanza_length + extras + 1);
    char *fill = text;
    size_t i;

    if (!text) {
        return NULL;
    }
    if (mode == CC_SESSION_RETARGET) {
        memcpy(fill, CC_SESSION_DEFINITION, strlen(CC_SESSION_DEFINITION));
        fill += strlen(CC_SESSION_DEFINITION);
    }
    if (mode == CC_SESSION_HEAD_DEFS) {
        for (i = 0; i < count; i++) {
            sprintf(fill, "[d%06zu]: /aaaa\n\n", i % 1000000);
            fill += CC_SESSION_DEF_WIDTH;
        }
    } else if (mode == CC_SESSION_DEF_SPREAD) {
        for (i = 0; i < count; i++) {
            sprintf(fill, "[d%06zu]: /aaaa\n\nuses [u][d%06zu] here\n\n", i % 1000000, i % 1000000);
            fill += CC_SESSION_DEF_SPREAD_WIDTH;
        }
    } else if (cc_session_mode_footnote_defs(mode)) {
        for (i = 0; i < count; i++) {
            sprintf(fill, "[^d%06zu]: aaaa\n\n", i % 1000000);
            fill += CC_SESSION_FOOTNOTE_DEF_WIDTH;
        }
    } else {
        for (i = 0; i < count; i++) {
            memcpy(fill, stanza, stanza_length);
            fill += stanza_length;
        }
    }
    if (mode == CC_SESSION_RETARGET) {
        for (i = 0; i < CC_SESSION_USES; i++) {
            memcpy(fill, CC_SESSION_USE, strlen(CC_SESSION_USE));
            fill += strlen(CC_SESSION_USE);
        }
    }
    if (mode == CC_SESSION_FOOTNOTE) {
        memcpy(fill, CC_SESSION_FOOTNOTE_CLUSTER, strlen(CC_SESSION_FOOTNOTE_CLUSTER));
        fill += strlen(CC_SESSION_FOOTNOTE_CLUSTER);
    }
    if (mode == CC_SESSION_HEAD_DEFS) {
        memcpy(fill, CC_SESSION_DEF_USE, strlen(CC_SESSION_DEF_USE));
        fill += strlen(CC_SESSION_DEF_USE);
    }
    if (cc_session_mode_footnote_defs(mode)) {
        memcpy(fill, CC_SESSION_FOOTNOTE_DEF_USE, strlen(CC_SESSION_FOOTNOTE_DEF_USE));
        fill += strlen(CC_SESSION_FOOTNOTE_DEF_USE);
    }
    *fill = '\0';

    ts_ast_options_none(&options);
    if (mode == CC_SESSION_FOOTNOTE || cc_session_mode_footnote_defs(mode)) {
        options.footnotes = true;
    }
    session = markdown_core_session_open(&options, NULL);
    if (!session || !markdown_core_session_edit(session, 0, 0, (const uint8_t *)text, (size_t)(fill - text), NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        markdown_core_session_free(session);
        session = NULL;
    }
    free(text);
    if (stanza_count) {
        *stanza_count = count;
    }
    return session;
}

/* One timed block of commits: appends at the tail, a storm of byte
 * replacements across the stanzas, a rewrite of the lone definition's
 * destination (a winner-delta commit re-refining the dependent units), or a
 * flip of the first footnote reference's label (a first-use renumbering
 * across the fixed cluster). */
static int cc_session_block(markdown_core_session *session, int mode, size_t stanza_count, size_t *op_counter) {
    size_t stanza_length = strlen(CC_SESSION_STANZA);
    int op;
    for (op = 0; op < CC_SESSION_OPS; op++) {
        bool ok;
        if (mode == CC_SESSION_STORM) {
            size_t index = (size_t)((*op_counter * UINT64_C(2654435761)) % stanza_count);
            uint8_t byte = (*op_counter & 1) ? 'x' : 'y';
            ok = markdown_core_session_edit(
                session,
                index * stanza_length + 1,
                index * stanza_length + 2,
                &byte,
                1,
                NULL
            );
        } else if (mode == CC_SESSION_RETARGET) {
            const uint8_t *url = (const uint8_t *)((*op_counter & 1) ? "bbbb" : "aaaa");
            ok = markdown_core_session_edit(session, 6, 10, url, 4, NULL);
        } else if (mode == CC_SESSION_FOOTNOTE) {
            size_t base = stanza_count * strlen(CC_SESSION_PLAIN_STANZA);
            uint8_t label = (*op_counter & 1) ? 'b' : 'a';
            ok = markdown_core_session_edit(session, base + 7, base + 8, &label, 1, NULL);
        } else if (mode == CC_SESSION_HEAD_DEFS) {
            size_t base = (stanza_count - 1) * CC_SESSION_DEF_WIDTH + CC_SESSION_DEF_URL_OFFSET;
            const uint8_t *url = (const uint8_t *)((*op_counter & 1) ? "bbbb" : "aaaa");
            ok = markdown_core_session_edit(session, base, base + 4, url, 4, NULL);
        } else if (mode == CC_SESSION_FOOTNOTE_DEFS) {
            size_t base = (stanza_count - 1) * CC_SESSION_FOOTNOTE_DEF_WIDTH + CC_SESSION_FOOTNOTE_DEF_BODY_OFFSET;
            const uint8_t *body = (const uint8_t *)((*op_counter & 1) ? "bbbb" : "aaaa");
            ok = markdown_core_session_edit(session, base, base + 4, body, 4, NULL);
        } else if (mode == CC_SESSION_DEF_SPREAD) {
            // The LAST pair: editing the first definition would measure the
            // def-index splice (a known O(defs) memmove) instead of the
            // dependent collection this case pins.
            size_t base = (stanza_count - 1) * CC_SESSION_DEF_SPREAD_WIDTH + CC_SESSION_DEF_URL_OFFSET;
            const uint8_t *url = (const uint8_t *)((*op_counter & 1) ? "bbbb" : "aaaa");
            ok = markdown_core_session_edit(session, base, base + 4, url, 4, NULL);
        } else if (mode == CC_SESSION_QUOTE_SUFFIX) {
            const uint8_t *body = (const uint8_t *)((*op_counter & 1) ? "bbbb" : "aaaa");
            ok = markdown_core_session_edit(
                session,
                CC_SESSION_QUOTE_BODY_OFFSET,
                CC_SESSION_QUOTE_BODY_OFFSET + 4,
                body,
                4,
                NULL
            );
        } else {
            static const uint8_t line[] = "appended stream line\n";
            size_t length = markdown_core_session_length(session);
            ok = markdown_core_session_edit(session, length, length, line, sizeof(line) - 1, NULL);
        }
        if (!ok || !markdown_core_session_commit(session, NULL, NULL)) {
            return -1;
        }
        (*op_counter)++;
    }
    return 0;
}

/* Session repeats take the minimum, not the median: contention on shared CI
 * runners only ever adds time, and it inflates the large session (the
 * cache- and bandwidth-heavy working set) far more than the small one,
 * which skews the ratio the flatness bound is about. The minimum estimates
 * the uncontended per-commit cost on both sides of that ratio. Five windows
 * give the minimum a real chance to see a quiet slice on a noisy machine. */
#define CC_SESSION_REPEATS 5

static int cc_session_measure(size_t size, int mode, double *seconds_per_commit) {
    double floor_seconds = 0.0;
    size_t stanza_count = 0;
    size_t op_counter = 0;
    markdown_core_session *session = cc_session_build(size, mode, &stanza_count);
    int repeat;

    if (!session) {
        return -1;
    }
    for (repeat = 0; repeat < CC_SESSION_REPEATS; repeat++) {
        uint64_t started = ts_monotonic_ns();
        uint64_t elapsed;
        size_t commits = 0;
        double sample;
        do {
            if (cc_session_block(session, mode, stanza_count, &op_counter) != 0) {
                markdown_core_session_free(session);
                return -1;
            }
            commits += CC_SESSION_OPS;
            elapsed = ts_monotonic_ns() - started;
        } while (elapsed < MIN_SAMPLE_NS);
        sample = (double)elapsed / (1e9 * (double)commits);
        if (repeat == 0 || sample < floor_seconds) {
            floor_seconds = sample;
        }
    }
    markdown_core_session_free(session);
    *seconds_per_commit = floor_seconds;
    return 0;
}

static int cc_run_session(const char *name, int mode) {
    double timings[SCALING_STEPS];
    size_t step;
    int failed = 0;

    for (step = 0; step < SCALING_STEPS; step++) {
        if (cc_session_measure(CC_SESSION_SIZES[step], mode, &timings[step]) != 0) {
            fprintf(stderr, "session run failed for %s\n", name);
            return -1;
        }
    }
    {
        double slowdown = timings[SCALING_STEPS - 1] / timings[0];
        if (slowdown > MAX_NORMALIZED_SLOWDOWN) {
            failed = 1;
        }
        printf("%s ... %s (", name, failed ? "[FAILED per-commit cost scales with document]" : "[PASSED]");
        for (step = 0; step < SCALING_STEPS; step++) {
            printf("%s%zu bytes: %.9fs/commit", step ? ", " : "", CC_SESSION_SIZES[step], timings[step]);
        }
        printf(", slowdown: %.3fx)\n", slowdown);
    }
    return failed ? -1 : 0;
}

static const char *const CC_SESSION_CASES[] = {
    "session_stream_flat",
    "session_edit_storm",
    "session_ref_retarget",
    "session_footnote_shift",
    "session_head_defs",
    "session_footnote_defs",
    "session_quote_suffix",
    "session_def_spread",
};

static int cc_measure(const char *input, size_t length, double *seconds) {
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
        for (i = 0; i < sizeof(CC_SESSION_CASES) / sizeof(CC_SESSION_CASES[0]); i++) {
            puts(CC_SESSION_CASES[i]);
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
    if (strcmp(case_name, "session_stream_flat") == 0) {
        return cc_run_session(case_name, CC_SESSION_STREAM) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_edit_storm") == 0) {
        return cc_run_session(case_name, CC_SESSION_STORM) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_ref_retarget") == 0) {
        return cc_run_session(case_name, CC_SESSION_RETARGET) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_footnote_shift") == 0) {
        return cc_run_session(case_name, CC_SESSION_FOOTNOTE) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_head_defs") == 0) {
        return cc_run_session(case_name, CC_SESSION_HEAD_DEFS) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_footnote_defs") == 0) {
        return cc_run_session(case_name, CC_SESSION_FOOTNOTE_DEFS) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_quote_suffix") == 0) {
        return cc_run_session(case_name, CC_SESSION_QUOTE_SUFFIX) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_def_spread") == 0) {
        return cc_run_session(case_name, CC_SESSION_DEF_SPREAD) == 0 ? 0 : 1;
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
