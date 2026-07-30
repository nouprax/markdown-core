/* Parser complexity suite.
 *
 * Measures parse time for adversarial parser inputs, then compares time
 * per input byte. Scanner/map/reference cases span 4 KiB to 128 MiB;
 * delimiter-dense cases use a smaller 4 KiB to 64 KiB span so the regression
 * test does not require millions of AST nodes. Every parse-scaling endpoint
 * is warmed once before adaptive timed sampling. Both spans expose nonlinear
 * growth without relying on absolute wall-clock thresholds. Scope-table
 * materialization separately walks committed deep trees through the public
 * batch API, pinning the first-scope-use path to linear growth. Ordered delta
 * materialization measures a deep touched path independently of parsing,
 * rejecting per-node ancestor walks.
 *
 *   complexity_runner --list
 *   complexity_runner --case NAME
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static const size_t SCALING_SIZES[] = {4096, 134217728};
static const size_t DELIMITER_SCALING_SIZES[] = {4096, 65536};
#define SCALING_STEPS (sizeof(SCALING_SIZES) / sizeof(SCALING_SIZES[0]))
#define SCALING_REPEATS 3
#define MIN_SAMPLE_NS 25000000ULL
/* A linear parser has constant asymptotic work per input byte, but millions of
 * parsed nodes cross allocator and cache regimes that a 4 KiB sample does not.
 * The inherited qsort path measured 4.442x across these endpoints; reject at
 * 4.0x to catch that observed regression without treating memory hierarchy as
 * an algorithmic proof. Probe/collision tests enforce the hash-path bound. */
static const double MAX_NORMALIZED_SLOWDOWN = 4.0;
/* Scope depth grows 8x below, so a quadratic materializer normalizes to
 * roughly 8x. The linear batch path measures about 2.0-2.5x after allocator
 * and cache effects; an independent 4.0x ceiling separates that signal from
 * parser thresholds that may evolve for unrelated workloads. */
static const double MAX_SCOPE_NORMALIZED_SLOWDOWN = 4.0;
static const double MAX_DELTA_ORDER_NORMALIZED_SLOWDOWN = 4.0;

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

static char *cc_emphasis_then_closers(size_t size, size_t *length) {
    size_t count = size / 3 ? size / 3 : 1;
    char *input = (char *)malloc(count * 3 + 1);
    size_t i;
    if (!input) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        input[i * 2] = '*';
        input[i * 2 + 1] = 'a';
    }
    memset(input + count * 2, ']', count);
    *length = count * 3;
    input[*length] = '\0';
    return input;
}

static char *cc_nested_directive_labels(size_t size, size_t *length) {
    size_t count = size / 4 ? size / 4 : 1;
    char *input = (char *)malloc(count * 4 + 1);
    size_t i;
    if (!input) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memcpy(input + i * 3, ":x[", 3);
    }
    memset(input + count * 3, ']', count);
    *length = count * 4;
    input[*length] = '\0';
    return input;
}

static char *cc_email_autolinks(size_t size, size_t *length) {
    static const char token[] = "a@b.c ";
    size_t count = size / (sizeof(token) - 1) ? size / (sizeof(token) - 1) : 1;
    char *input = (char *)malloc(count * (sizeof(token) - 1) + 1);
    size_t i;
    if (!input) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memcpy(input + i * (sizeof(token) - 1), token, sizeof(token) - 1);
    }
    *length = count * (sizeof(token) - 1);
    input[*length] = '\0';
    return input;
}

static char *cc_unclosed_cross_references(size_t size, size_t *length) {
    size_t count = size / 5 ? size / 5 : 1;
    char *input = (char *)malloc(count * 5 + 1);
    size_t i;
    if (!input) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memcpy(input + i * 5, "[[![[", 5);
    }
    *length = count * 5;
    input[*length] = '\0';
    return input;
}

static char *cc_balanced_nested_cross_links(size_t size, size_t *length) {
    size_t count = size > 1 ? (size - 1) / 4 : 1;
    char *input = (char *)malloc(count * 4 + 2);
    size_t i;
    if (!input) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memcpy(input + i * 2, "[[", 2);
    }
    input[count * 2] = 'x';
    for (i = 0; i < count; i++) {
        memcpy(input + count * 2 + 1 + i * 2, "]]", 2);
    }
    *length = count * 4 + 1;
    input[*length] = '\0';
    return input;
}

typedef struct cc_case_entry {
    const char *name;
    cc_builder build;
    const size_t *sizes;
    const char *option;
} cc_case_entry;

static const cc_case_entry CC_CASES[] = {
    {"valid_long_quoted_value", cc_quoted_value, SCALING_SIZES, "directive"},
    {"valid_consecutive_backslashes", cc_backslashes, SCALING_SIZES, "directive"},
    {"unclosed_long_quoted_value", cc_unclosed_quoted, SCALING_SIZES, "directive"},
    {"unclosed_backslash_value", cc_unclosed_backslashes, SCALING_SIZES, "directive"},
    {"many_unique_attributes", cc_unique_attributes, SCALING_SIZES, "directive"},
    {"many_duplicate_attributes", cc_duplicate_attributes, SCALING_SIZES, "directive"},
    {"many_unique_references", cc_unique_references, SCALING_SIZES, "directive"},
    {"many_duplicate_references", cc_duplicate_references, SCALING_SIZES, "directive"},
    {"directive_closers_after_emphasis", cc_emphasis_then_closers, DELIMITER_SCALING_SIZES, "directive"},
    {"nested_directive_label_closers", cc_nested_directive_labels, DELIMITER_SCALING_SIZES, "directive"},
    {"many_email_autolinks", cc_email_autolinks, DELIMITER_SCALING_SIZES, "autolink"},
    {"unclosed_cross_references", cc_unclosed_cross_references, DELIMITER_SCALING_SIZES, "cross-links-and-embeds"},
    {"balanced_nested_cross_links", cc_balanced_nested_cross_links, DELIMITER_SCALING_SIZES, "cross-link"},
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
            snprintf(fill, CC_SESSION_DEF_WIDTH + 1, "[d%06zu]: /aaaa\n\n", i % 1000000);
            fill += CC_SESSION_DEF_WIDTH;
        }
    } else if (mode == CC_SESSION_DEF_SPREAD) {
        for (i = 0; i < count; i++) {
            snprintf(
                fill,
                CC_SESSION_DEF_SPREAD_WIDTH + 1,
                "[d%06zu]: /aaaa\n\nuses [u][d%06zu] here\n\n",
                i % 1000000,
                i % 1000000
            );
            fill += CC_SESSION_DEF_SPREAD_WIDTH;
        }
    } else if (cc_session_mode_footnote_defs(mode)) {
        for (i = 0; i < count; i++) {
            snprintf(fill, CC_SESSION_FOOTNOTE_DEF_WIDTH + 1, "[^d%06zu]: aaaa\n\n", i % 1000000);
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

/* A first-reference label flip renumbers every later footnote. This scales
 * the number of query-only changed nodes, rather than unrelated document
 * bulk, so the normalized ratio catches quadratic index-diff deduplication. */
static const size_t CC_FOOTNOTE_RENUMBER_COUNTS[] = {256, 4096};
#define CC_FOOTNOTE_RENUMBER_STEPS (sizeof(CC_FOOTNOTE_RENUMBER_COUNTS) / sizeof(CC_FOOTNOTE_RENUMBER_COUNTS[0]))
#define CC_FOOTNOTE_RENUMBER_LABEL_OFFSET 10

static markdown_core_session *cc_footnote_renumber_build(size_t count) {
    markdown_core_parse_options options;
    markdown_core_session *session = NULL;
    size_t capacity;
    size_t length = 0;
    size_t i;
    char *text;

    if (count > (SIZE_MAX - 1) / 64) {
        return NULL;
    }
    capacity = count * 64 + 1;
    text = (char *)malloc(capacity);
    if (!text) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        length += (size_t)snprintf(text + length, capacity - length, "r [^n%06zu]\n\n", i);
    }
    for (i = 0; i < count; i++) {
        length += (size_t)snprintf(text + length, capacity - length, "[^n%06zu]: body\n\n", i);
    }

    ts_ast_options_none(&options);
    options.footnotes = true;
    session = markdown_core_session_open(&options, NULL);
    if (!session || !markdown_core_session_edit(session, 0, 0, (const uint8_t *)text, length, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        markdown_core_session_free(session);
        session = NULL;
    }
    free(text);
    return session;
}

static int cc_footnote_renumber_measure(size_t count, double *seconds_per_commit) {
    double floor_seconds = 0.0;
    size_t op_counter = 0;
    markdown_core_session *session = cc_footnote_renumber_build(count);
    int repeat;

    if (!session) {
        return -1;
    }
    for (repeat = 0; repeat < CC_SESSION_REPEATS; repeat++) {
        uint64_t started = ts_monotonic_ns();
        uint64_t elapsed;
        size_t commits = 0;
        do {
            const uint8_t label = (op_counter & 1) ? '0' : '1';
            markdown_core_delta *changes = NULL;
            size_t changed;
            if (!markdown_core_session_edit(
                    session,
                    CC_FOOTNOTE_RENUMBER_LABEL_OFFSET,
                    CC_FOOTNOTE_RENUMBER_LABEL_OFFSET + 1,
                    &label,
                    1,
                    NULL
                ) ||
                !markdown_core_session_commit(session, &changes, NULL)) {
                markdown_core_delta_free(changes);
                markdown_core_session_free(session);
                return -1;
            }
            changed = markdown_core_delta_changed(changes, NULL);
            markdown_core_delta_free(changes);
            if (changed < count) {
                markdown_core_session_free(session);
                return -1;
            }
            op_counter++;
            commits++;
            elapsed = ts_monotonic_ns() - started;
        } while (elapsed < MIN_SAMPLE_NS);
        {
            double sample = (double)elapsed / (1e9 * (double)commits);
            if (repeat == 0 || sample < floor_seconds) {
                floor_seconds = sample;
            }
        }
    }
    markdown_core_session_free(session);
    *seconds_per_commit = floor_seconds;
    return 0;
}

static int cc_run_footnote_renumber(const char *name) {
    double timings[CC_FOOTNOTE_RENUMBER_STEPS];
    size_t step;
    int failed = 0;

    for (step = 0; step < CC_FOOTNOTE_RENUMBER_STEPS; step++) {
        if (cc_footnote_renumber_measure(CC_FOOTNOTE_RENUMBER_COUNTS[step], &timings[step]) != 0) {
            fprintf(stderr, "session run failed for %s\n", name);
            return -1;
        }
    }
    {
        double count_growth = (double)CC_FOOTNOTE_RENUMBER_COUNTS[CC_FOOTNOTE_RENUMBER_STEPS - 1] /
                              (double)CC_FOOTNOTE_RENUMBER_COUNTS[0];
        double normalized_slowdown = timings[CC_FOOTNOTE_RENUMBER_STEPS - 1] / timings[0] / count_growth;
        if (normalized_slowdown > MAX_NORMALIZED_SLOWDOWN) {
            failed = 1;
        }
        printf("%s ... %s (", name, failed ? "[FAILED non-linear diff scaling]" : "[PASSED]");
        for (step = 0; step < CC_FOOTNOTE_RENUMBER_STEPS; step++) {
            printf("%s%zu footnotes: %.9fs/commit", step ? ", " : "", CC_FOOTNOTE_RENUMBER_COUNTS[step], timings[step]);
        }
        printf(", normalized slowdown: %.3fx)\n", normalized_slowdown);
    }
    return failed ? -1 : 0;
}

static const size_t CC_DEEP_DEPTHS[] = {2048, 16384};
#define CC_DEEP_STEPS (sizeof(CC_DEEP_DEPTHS) / sizeof(CC_DEEP_DEPTHS[0]))

static markdown_core_session *cc_scope_build(size_t depth) {
    markdown_core_session *session = NULL;
    size_t length;
    size_t index;
    char *text;
    if (depth > (SIZE_MAX - 6) / 2) {
        return NULL;
    }
    length = depth * 2 + 5;
    text = (char *)malloc(length + 1);
    if (!text) {
        return NULL;
    }
    for (index = 0; index < depth; index++) {
        text[index * 2] = '>';
        text[index * 2 + 1] = ' ';
    }
    memcpy(text + depth * 2, "leaf\n", 5);
    text[length] = '\0';
    session = markdown_core_session_open(NULL, NULL);
    if (!session || !markdown_core_session_edit(session, 0, 0, (const uint8_t *)text, length, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        markdown_core_session_free(session);
        session = NULL;
    }
    free(text);
    return session;
}

static int cc_scope_measure(size_t depth, double *seconds_per_materialization) {
    markdown_core_session *session = cc_scope_build(depth);
    const markdown_core_document *document;
    double samples[SCALING_REPEATS];
    int repeat;
    if (!session) {
        return -1;
    }
    document = markdown_core_session_document(session);
    if (!document) {
        markdown_core_session_free(session);
        return -1;
    }
    for (repeat = 0; repeat < SCALING_REPEATS; repeat++) {
        uint64_t started = ts_monotonic_ns();
        uint64_t elapsed;
        size_t iterations = 0;
        do {
            markdown_core_scope_entry *entries = NULL;
            size_t count = 0;
            size_t index;
            uint64_t checksum = 0;
            if (!markdown_core_document_scope_table(document, &entries, &count, NULL)) {
                markdown_core_session_free(session);
                return -1;
            }
            for (index = 0; index < count; index++) {
                checksum += (uint64_t)entries[index].scope.start.line + (uint64_t)entries[index].scope.end.line;
            }
            markdown_core_scope_table_free(entries);
            if (checksum == 0) {
                markdown_core_session_free(session);
                return -1;
            }
            iterations++;
            elapsed = ts_monotonic_ns() - started;
        } while (elapsed < MIN_SAMPLE_NS);
        samples[repeat] = (double)elapsed / (1e9 * (double)iterations);
    }
    markdown_core_session_free(session);
    {
        double a = samples[0], b = samples[1], c = samples[2];
        double high = a > b ? (a > c ? a : c) : (b > c ? b : c);
        double low = a < b ? (a < c ? a : c) : (b < c ? b : c);
        *seconds_per_materialization = a + b + c - high - low;
    }
    return 0;
}

static int cc_run_scope_materialization(const char *name) {
    double timings[CC_DEEP_STEPS];
    size_t step;
    int failed = 0;
    for (step = 0; step < CC_DEEP_STEPS; step++) {
        if (cc_scope_measure(CC_DEEP_DEPTHS[step], &timings[step]) != 0) {
            fprintf(stderr, "scope materialization failed for %s\n", name);
            return -1;
        }
    }
    {
        double depth_growth = (double)CC_DEEP_DEPTHS[CC_DEEP_STEPS - 1] / (double)CC_DEEP_DEPTHS[0];
        double normalized_slowdown = timings[CC_DEEP_STEPS - 1] / timings[0] / depth_growth;
        if (normalized_slowdown > MAX_SCOPE_NORMALIZED_SLOWDOWN) {
            failed = 1;
        }
        printf("%s ... %s (", name, failed ? "[FAILED non-linear scope scaling]" : "[PASSED]");
        for (step = 0; step < CC_DEEP_STEPS; step++) {
            printf("%sdepth %zu: %.9fs/materialization", step ? ", " : "", CC_DEEP_DEPTHS[step], timings[step]);
        }
        printf(", normalized slowdown: %.3fx)\n", normalized_slowdown);
    }
    return failed ? -1 : 0;
}

static int cc_delta_order_build(
    size_t depth,
    markdown_core_session **session_output,
    markdown_core_delta **changes_output
) {
    markdown_core_session *session = NULL;
    markdown_core_delta *changes = NULL;
    size_t length;
    size_t index;
    char *text;
    const uint8_t replacement = 'b';

    *session_output = NULL;
    *changes_output = NULL;
    if (depth > (SIZE_MAX - 3) / 2) {
        return -1;
    }
    length = depth * 2 + 2;
    text = (char *)malloc(length + 1);
    if (!text) {
        return -1;
    }
    for (index = 0; index < depth; ++index) {
        text[index * 2] = '>';
        text[index * 2 + 1] = ' ';
    }
    memcpy(text + depth * 2, "a\n", 2);
    text[length] = '\0';

    session = markdown_core_session_open(NULL, NULL);
    if (!session || !markdown_core_session_edit(session, 0, 0, (const uint8_t *)text, length, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL) ||
        !markdown_core_session_edit(session, depth * 2, depth * 2 + 1, &replacement, 1, NULL) ||
        !markdown_core_session_commit(session, &changes, NULL)) {
        free(text);
        markdown_core_delta_free(changes);
        markdown_core_session_free(session);
        return -1;
    }
    free(text);
    *session_output = session;
    *changes_output = changes;
    return 0;
}

static int cc_delta_order_measure(size_t depth, double *seconds_per_ordering) {
    markdown_core_session *session;
    markdown_core_delta *changes;
    double samples[SCALING_REPEATS];
    int repeat;

    if (cc_delta_order_build(depth, &session, &changes) != 0) {
        return -1;
    }
    for (repeat = 0; repeat < SCALING_REPEATS; ++repeat) {
        uint64_t started = ts_monotonic_ns();
        uint64_t elapsed;
        size_t iterations = 0;
        do {
            markdown_core_delta_entry *entries = NULL;
            size_t count = 0;
            size_t index;
            uint64_t checksum = 0;
            if (!markdown_core_session_ordered_delta_entries(session, changes, &entries, &count, NULL) ||
                count < depth) {
                markdown_core_delta_entries_free(entries);
                markdown_core_delta_free(changes);
                markdown_core_session_free(session);
                return -1;
            }
            for (index = 0; index < count; ++index) {
                checksum += entries[index].id + entries[index].parent + entries[index].change;
            }
            markdown_core_delta_entries_free(entries);
            if (checksum == 0) {
                markdown_core_delta_free(changes);
                markdown_core_session_free(session);
                return -1;
            }
            ++iterations;
            elapsed = ts_monotonic_ns() - started;
        } while (elapsed < MIN_SAMPLE_NS);
        samples[repeat] = (double)elapsed / (1e9 * (double)iterations);
    }
    markdown_core_delta_free(changes);
    markdown_core_session_free(session);
    {
        double a = samples[0], b = samples[1], c = samples[2];
        double high = a > b ? (a > c ? a : c) : (b > c ? b : c);
        double low = a < b ? (a < c ? a : c) : (b < c ? b : c);
        *seconds_per_ordering = a + b + c - high - low;
    }
    return 0;
}

static int cc_run_delta_ordering(const char *name) {
    double timings[CC_DEEP_STEPS];
    size_t step;
    int failed = 0;

    for (step = 0; step < CC_DEEP_STEPS; ++step) {
        if (cc_delta_order_measure(CC_DEEP_DEPTHS[step], &timings[step]) != 0) {
            fprintf(stderr, "delta ordering failed for %s\n", name);
            return -1;
        }
    }
    {
        double depth_growth = (double)CC_DEEP_DEPTHS[CC_DEEP_STEPS - 1] / (double)CC_DEEP_DEPTHS[0];
        double normalized_slowdown = timings[CC_DEEP_STEPS - 1] / timings[0] / depth_growth;
        if (normalized_slowdown > MAX_DELTA_ORDER_NORMALIZED_SLOWDOWN) {
            failed = 1;
        }
        printf("%s ... %s (", name, failed ? "[FAILED non-linear delta ordering]" : "[PASSED]");
        for (step = 0; step < CC_DEEP_STEPS; ++step) {
            printf("%sdepth %zu: %.9fs/ordering", step ? ", " : "", CC_DEEP_DEPTHS[step], timings[step]);
        }
        printf(", normalized slowdown: %.3fx)\n", normalized_slowdown);
    }
    return failed ? -1 : 0;
}

static const char *const CC_SESSION_CASES[] = {
    "session_stream_flat",
    "session_edit_storm",
    "session_ref_retarget",
    "session_footnote_shift",
    "session_footnote_renumber",
    "session_head_defs",
    "session_footnote_defs",
    "session_quote_suffix",
    "session_def_spread",
    "session_scope_materialization",
    "session_delta_ordering",
};

static int cc_measure(const char *input, size_t length, const char *option, double *seconds) {
    double samples[SCALING_REPEATS];
    int repeat;
    markdown_core_parse_options options;
    ts_ast_options_none(&options);
    if (ts_ast_enable(&options, option) != 0) {
        return -1;
    }
    /* Warm every endpoint once outside the timer so the adaptive sampling
     * policy never compares a hot short endpoint with a cold long endpoint. */
    {
        markdown_core_document *document = ts_ast_parse((const uint8_t *)input, length, &options);
        if (!document) {
            return -1;
        }
        markdown_core_document_free(document);
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
        /* Classify from the first post-warmup bucket. A single parse already
         * gives a long, stable sample for the large endpoint; later one-parse
         * buckets are scheduler outliers handled by the median below. */
        if (repeat == 0 && iterations == 1) {
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
        char *input = entry->build(entry->sizes[step], &length);
        if (!input) {
            fprintf(stderr, "cannot build input for %s\n", entry->name);
            return -1;
        }
        if (cc_measure(input, length, entry->option, &timings[step]) != 0) {
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
    if (strcmp(case_name, "session_footnote_renumber") == 0) {
        return cc_run_footnote_renumber(case_name) == 0 ? 0 : 1;
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
    if (strcmp(case_name, "session_scope_materialization") == 0) {
        return cc_run_scope_materialization(case_name) == 0 ? 0 : 1;
    }
    if (strcmp(case_name, "session_delta_ordering") == 0) {
        return cc_run_delta_ordering(case_name) == 0 ? 0 : 1;
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
