/* The CST/AST split's own gates (Stage 1, §12).
 *
 * Every case here drives the raw parser -- feed, finish, and the parser's
 * internal state -- so, like stream_runner, this links the static engine
 * rather than the facade. The corpus is the same fixture set the goldens pin,
 * so every example is a case for free.
 *
 *   projection_runner --case NAME --spec FILE
 *
 * closed_after_finish: a FINISHED tree contains no block still carrying
 * MARKDOWN_CORE_NODE__OPEN. §12.8 Q3 measured the violation: every table cell
 * and the header row shipped open, because `make_block` sets the flag,
 * `finalize` is the only clearer, and neither is ever on the open spine. The
 * flag is the closed signal Stage 1 schedules projections on, so an open block
 * in a finished tree is a lie about completeness.
 *
 * borrow_across_feed: T19's gate, documented at its definition.
 *
 * projection_key: T3 and T4's gate, documented at its definition.
 *
 * dump_boundaries / feed_loop: the A/B oracle and the clock for a change to
 * the projection, documented at their definitions. Both also take `--md FILE`
 * or `--md-dir DIR` -- raw markdown, one case per file -- because the real
 * corpus is not a fixture file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

/* The `--md-dir` corpus walk, same split as corpus_guard's: the Windows CRT
 * has no dirent.h. */
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "test_support.h"

#include <markdown_core.h>

#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "syntax_extension.h"

#include "ast_internal.h"
#include "iterator.h"
#include "map.h"
#include "node.h"
#include "parser.h"
#include "references.h"

/* Every extension and the footnote option, because the point is coverage of
 * every node KIND a parse can put in a tree, not agreement with any golden. */
/* `--no-cache` turns the projection cache (T9) off for every parser a case
 * builds -- the control side of T11's measurement. */
static int pr_no_cache = 0;

static markdown_core_parser *pr_parser_new(void) {
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES);
    if (!parser) {
        return NULL;
    }
    parser->no_projection_cache = pr_no_cache != 0;
    if (!markdown_core_core_extensions_attach(
            parser,
            MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK | MARKDOWN_CORE_CORE_EXTENSION_TASKLIST |
                MARKDOWN_CORE_CORE_EXTENSION_FORMULA | MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE
        )) {
        markdown_core_parser_free(parser);
        return NULL;
    }
    return parser;
}

static markdown_core_node *pr_parse(const char *text, size_t length) {
    markdown_core_parser *parser = pr_parser_new();
    markdown_core_node *root;
    if (!parser) {
        return NULL;
    }
    markdown_core_parser_feed(parser, text, length);
    root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return root;
}

static int pr_count_open_blocks(markdown_core_node *root, int example, size_t *nodes_seen) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;
    int open_nodes = 0;
    if (!iter) {
        return -1;
    }
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        (*nodes_seen)++;
        if (node->flags & MARKDOWN_CORE_NODE__OPEN) {
            fprintf(
                stderr,
                "example %d: %s at %d:%d is still open after finish\n",
                example,
                markdown_core_node_get_type_string(node),
                node->start_line,
                node->start_column
            );
            open_nodes++;
        }
    }
    markdown_core_iter_free(iter);
    return open_nodes;
}

static int case_closed_after_finish(const ts_spec_file *file) {
    size_t index;
    size_t nodes_seen = 0;
    int failures = 0;
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        markdown_core_node *root = pr_parse(test_case->markdown, test_case->markdown_length);
        int open_nodes;
        if (!root) {
            fprintf(stderr, "example %d: parse failed\n", test_case->example);
            failures++;
            continue;
        }
        open_nodes = pr_count_open_blocks(root, test_case->example, &nodes_seen);
        if (open_nodes != 0) {
            failures++;
        }
        markdown_core_node_free(root);
    }
    printf(
        "closed after finish: %zu/%zu examples clean, %zu nodes\n",
        file->count - (size_t)failures,
        file->count,
        nodes_seen
    );
    return failures ? -1 : 0;
}

static uint8_t *pr_dump(markdown_core_node *root, size_t *length) {
    markdown_core_document facade;
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    memset(&facade, 0, sizeof(facade));
    facade.root = root;
    if (!markdown_core_document_dump(&facade, &dump, length, &error)) {
        markdown_core_error_free(error);
        return NULL;
    }
    return dump;
}

/* A stable serialization of everything the CST states: node identity, place,
 * flags, content bytes and the content-to-source run -- and the SIZE of the
 * parser's mark vector, which is CST state no node owns: a projection that
 * minted marks into it grew it (F21), and every open block's later marks
 * then landed outside their run. If a derivation writes any of it, two
 * fingerprints taken around the derivation differ. */
static int pr_fingerprint(markdown_core_parser *parser, markdown_core_strbuf *out) {
    markdown_core_node *root = parser->root;
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;
    char marks[48];
    if (!iter) {
        return -1;
    }
    snprintf(marks, sizeof(marks), "marks=%u\n", (unsigned)parser->line_marks_size);
    markdown_core_strbuf_puts(out, marks);
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        char header[160];
        snprintf(
            header,
            sizeof(header),
            "%u#%u|%u|%d:%d..%d:%d|%d|%d+%d|%u:",
            (unsigned)node->type,
            (unsigned)node->identifier,
            /* The cache (T9) hangs a holder on a CST block and says so in a
             * flag. That is bookkeeping about the block, not a statement the
             * CST makes, so it is outside what a derivation must not write. */
            (unsigned)(node->flags & ~MARKDOWN_CORE_NODE__CACHE_OWNER),
            node->start_line,
            node->start_column,
            node->end_line,
            node->end_column,
            node->internal_offset,
            node->content_mark,
            node->content_mark_count,
            (unsigned)node->content.size
        );
        markdown_core_strbuf_puts(out, header);
        if (node->content.size) {
            markdown_core_strbuf_put(out, node->content.ptr, node->content.size);
        }
        markdown_core_strbuf_putc(out, '\n');
    }
    markdown_core_iter_free(iter);
    return out->oom ? -1 : 0;
}

/* Two projections of one CST at one refmap generation are byte-identical --
 * the gate §0's item states for "make process_inlines callable more than
 * once". Derived BEFORE finish, so the CST here still carries its open spine:
 * repeatability is proved over open blocks too, not only over the closed tree
 * `finish` projects. */
static int case_double_projection(const ts_spec_file *file) {
    size_t index;
    int failures = 0;
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        markdown_core_parser *parser = pr_parser_new();
        markdown_core_node *first;
        markdown_core_node *second;
        uint8_t *first_dump = NULL;
        uint8_t *second_dump = NULL;
        size_t first_length = 0;
        size_t second_length = 0;
        if (!parser) {
            fprintf(stderr, "example %d: parser allocation failed\n", test_case->example);
            failures++;
            continue;
        }
        markdown_core_parser_feed(parser, test_case->markdown, test_case->markdown_length);
        /* Each tree is dumped BEFORE the next derivation runs. Dumping both at
         * the end let a second derivation that wrote into the list the two
         * trees share (T9, F22) change both dumps alike, and the gate agreed
         * with itself while the tree was wrong. */
        first = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (first) {
            first_dump = pr_dump(first, &first_length);
        }
        second = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (second) {
            second_dump = pr_dump(second, &second_length);
        }
        if (!first_dump || !second_dump) {
            fprintf(stderr, "example %d: derivation or dump failed\n", test_case->example);
            failures++;
        } else if (first_length != second_length || memcmp(first_dump, second_dump, first_length) != 0) {
            fprintf(stderr, "example %d: two projections of one CST differ\n", test_case->example);
            fprintf(
                stderr,
                "  first:\n%.*s  second:\n%.*s",
                (int)first_length,
                (const char *)first_dump,
                (int)second_length,
                (const char *)second_dump
            );
            failures++;
        }
        markdown_core_dump_free(first_dump);
        markdown_core_dump_free(second_dump);
        if (first) {
            markdown_core_node_free(first);
        }
        if (second) {
            markdown_core_node_free(second);
        }
        markdown_core_parser_free(parser);
    }
    printf("double projection: %zu/%zu examples agree\n", file->count - (size_t)failures, file->count);
    return failures ? -1 : 0;
}

/* The CST is independent of the refmap: project it against the document's own
 * map, against an EMPTY map, and against the document's map again -- the CST's
 * fingerprint never moves, and the first and third projections are
 * byte-identical. The middle projection is what §0's acceptance calls
 * "projecting one CST against two different maps": its result legitimately
 * differs where references resolve, and what is asserted is that deriving it
 * poisoned nothing. */
static int case_refmap_independence(const ts_spec_file *file) {
    size_t index;
    int failures = 0;
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        markdown_core_parser *parser = pr_parser_new();
        markdown_core_map *empty_map;
        markdown_core_node *first;
        markdown_core_node *other;
        markdown_core_node *again;
        uint8_t *first_dump = NULL;
        uint8_t *again_dump = NULL;
        size_t first_length = 0;
        size_t again_length = 0;
        markdown_core_strbuf before;
        markdown_core_strbuf after;
        int example_failed = 0;
        if (!parser) {
            fprintf(stderr, "example %d: parser allocation failed\n", test_case->example);
            failures++;
            continue;
        }
        empty_map = markdown_core_reference_map_new(parser->mem);
        markdown_core_strbuf_init(parser->mem, &before, 0);
        markdown_core_strbuf_init(parser->mem, &after, 0);
        markdown_core_parser_feed(parser, test_case->markdown, test_case->markdown_length);

        if (pr_fingerprint(parser, &before) != 0) {
            example_failed = 1;
        }
        /* Each tree is dumped BEFORE the next derivation runs, the ordering
         * F22 forced on `projection_double` -- a later derivation that wrote
         * into a shared list would otherwise change both dumps alike and the
         * gate would agree with itself (landing review). */
        first = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (first) {
            first_dump = pr_dump(first, &first_length);
        }
        other = markdown_core_parser_derive_tree(parser, empty_map);
        again = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (again) {
            again_dump = pr_dump(again, &again_length);
        }
        if (!example_failed && pr_fingerprint(parser, &after) != 0) {
            example_failed = 1;
        }
        if (example_failed || !first_dump || !other || !again_dump) {
            fprintf(stderr, "example %d: derivation, dump or fingerprint failed\n", test_case->example);
            failures++;
        } else if (before.size != after.size || memcmp(before.ptr, after.ptr, before.size) != 0) {
            fprintf(stderr, "example %d: a derivation WROTE the CST\n", test_case->example);
            failures++;
        } else if (first_length != again_length || memcmp(first_dump, again_dump, first_length) != 0) {
            fprintf(
                stderr,
                "example %d: projecting against another map poisoned the next projection\n",
                test_case->example
            );
            failures++;
        }
        markdown_core_dump_free(first_dump);
        markdown_core_dump_free(again_dump);
        if (first) {
            markdown_core_node_free(first);
        }
        if (other) {
            markdown_core_node_free(other);
        }
        if (again) {
            markdown_core_node_free(again);
        }
        markdown_core_strbuf_free(&before);
        markdown_core_strbuf_free(&after);
        markdown_core_map_free(empty_map);
        markdown_core_parser_free(parser);
    }
    printf("refmap independence: %zu/%zu examples agree\n", file->count - (size_t)failures, file->count);
    return failures ? -1 : 0;
}

/* CRITERION 2's SECOND BOUND (§12.4, §12.10 D): a projection costs
 * O(what is projected) -- flat ns per content byte across document sizes,
 * gated the same way the construction slope gate is: two endpoints, and the
 * time growth normalized by the size growth must not exceed the bound.
 *
 * The bound is NOT the construction gates' 4.0, and the difference is what
 * each number was calibrated against. Theirs sits just under a MEASURED
 * regression (the qsort path's 4.442x); this gate has no bad reading to sit
 * under -- what it rejects is super-linearity, which at a 256x size span
 * reads >= 100x -- and it does have a measured HEALTHY reading to sit above:
 * CI's shared macOS runner reported 4.114x (18.6 -> 76.7 ns/byte) on the same
 * build a local machine measured at 1.336x, which is a memory-hierarchy and
 * VM-noise regime, not an algorithm. 8.0 clears the observed noise and still
 * fails any real growth term by an order of magnitude. */
#define PR_SLOPE_SMALL 65536
#define PR_SLOPE_LARGE 16777216
#define PR_SLOPE_REPEATS 3
#define PR_MIN_SAMPLE_NS 25000000ULL
static const double PR_MAX_NORMALIZED_SLOWDOWN = 8.0;

/* Representative prose: emphasis, an inline link, code, and a reference that
 * resolves against the map, so the projection does the work a real document
 * asks of it. */
static char *pr_slope_document(size_t target, size_t *length) {
    static const char paragraph[] =
        "Some *emphasis* with a [link](https://example.com/a) and `code` and a [ref] in prose.\n\n";
    static const char definition[] = "[ref]: https://example.com/definition\n";
    size_t unit = sizeof(paragraph) - 1;
    size_t count = target / unit;
    size_t total = count * unit + sizeof(definition) - 1;
    char *input = (char *)malloc(total + 1);
    size_t i;
    if (!input) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memcpy(input + i * unit, paragraph, unit);
    }
    memcpy(input + count * unit, definition, sizeof(definition) - 1);
    input[total] = '\0';
    *length = total;
    return input;
}

static int pr_slope_measure(const char *input, size_t length, double *seconds) {
    double samples[PR_SLOPE_REPEATS];
    int repeat;
    markdown_core_parser *parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    /* Deriving one CST over and over is exactly what the projection cache
     * (T9) serves, and served, the projection is the whole-CST clone -- an
     * O(bytes) copy whose ns/byte is a memory-hierarchy fact (1.1 in cache,
     * 12.5 out of it, measured) rather than an algorithm. This gate is about
     * the RE-PROJECTION being linear in what it projects, so it measures with
     * the cache off; the cached regime's bound is T15's. */
    parser->no_projection_cache = true;
    markdown_core_parser_feed(parser, input, length);
    for (repeat = 0; repeat < PR_SLOPE_REPEATS; repeat++) {
        uint64_t started;
        uint64_t elapsed = 0;
        unsigned iterations = 0;
        started = ts_monotonic_ns();
        do {
            markdown_core_node *derived = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!derived) {
                markdown_core_parser_free(parser);
                return -1;
            }
            markdown_core_node_free(derived);
            iterations++;
            elapsed = ts_monotonic_ns() - started;
        } while (elapsed < PR_MIN_SAMPLE_NS);
        samples[repeat] = (double)elapsed / (1e9 * (double)iterations);
        if (iterations == 1) {
            markdown_core_parser_free(parser);
            *seconds = samples[repeat];
            return 0;
        }
    }
    markdown_core_parser_free(parser);
    {
        double a = samples[0], b = samples[1], c = samples[2];
        double high = a > b ? (a > c ? a : c) : (b > c ? b : c);
        double low = a < b ? (a < c ? a : c) : (b < c ? b : c);
        *seconds = a + b + c - high - low;
    }
    return 0;
}

static int case_projection_slope(const ts_spec_file *file) {
    static const size_t sizes[] = {PR_SLOPE_SMALL, PR_SLOPE_LARGE};
    size_t lengths[2];
    double timings[2];
    size_t step;
    (void)file;
    for (step = 0; step < 2; step++) {
        size_t length = 0;
        char *input = pr_slope_document(sizes[step], &length);
        if (!input) {
            fputs("cannot build slope input\n", stderr);
            return -1;
        }
        if (pr_slope_measure(input, length, &timings[step]) != 0) {
            fputs("projection failed while measuring\n", stderr);
            free(input);
            return -1;
        }
        lengths[step] = length;
        free(input);
    }
    {
        double input_growth = (double)lengths[1] / (double)lengths[0];
        double time_growth = timings[1] / timings[0];
        double normalized_slowdown = time_growth / input_growth;
        int failed = normalized_slowdown > PR_MAX_NORMALIZED_SLOWDOWN;
        printf(
            "projection slope: %zu bytes: %.6fs (%.3f ns/byte), %zu bytes: %.6fs (%.3f ns/byte), normalized "
            "slowdown %.3fx%s\n",
            lengths[0],
            timings[0],
            timings[0] * 1e9 / (double)lengths[0],
            lengths[1],
            timings[1],
            timings[1] * 1e9 / (double)lengths[1],
            normalized_slowdown,
            failed ? " [NON-FLAT]" : ""
        );
        return failed ? -1 : 0;
    }
}

/* T19's gate (docs/STREAMING.md §5): A BORROW HELD ACROSS A FEED THAT
 * REPLACES IT STILL READS. The engine hands out no borrow yet -- that is T9 --
 * so this case plays T9's part with T19's primitives. After every line it
 * derives, moves each leaf block's children into a holder and borrows them
 * back; where a leaf's subtree dumps as it did at the previous boundary it
 * borrows the PREVIOUS holder instead, so one list is aliased by two live
 * trees. Each boundary then asserts what a consumer could observe:
 *
 *   1. the borrowed tree dumps as the plain derivation did;
 *   2. the previous tree still does after the new borrow was taken -- the
 *      walk out of a shared list must land in ITS borrower, not the newest;
 *   3. still does after the cache released its hold;
 *   4. the new tree still does after the previous borrower was freed.
 *
 * The parser runs on a counting allocator and an example must end with zero
 * live allocations: that is the leak ledger, because LSan is not available
 * under the macOS ASan preset. */
static long pr_live_allocations = 0;

static void *pr_counting_calloc(size_t count, size_t size) {
    void *block = calloc(count, size);
    if (block) {
        pr_live_allocations++;
    }
    return block;
}

static void *pr_counting_realloc(void *block, size_t size) {
    void *grown = realloc(block, size);
    if (grown && !block) {
        pr_live_allocations++;
    }
    return grown;
}

static void pr_counting_free(void *block) {
    if (block) {
        pr_live_allocations--;
    }
    free(block);
}

static markdown_core_mem PR_COUNTING_MEM = {pr_counting_calloc, pr_counting_realloc, pr_counting_free};

/* One boundary's tree and the holders it borrows from, index-aligned with
 * its leaf blocks in walk order. */
typedef struct pr_generation {
    markdown_core_node *tree;
    uint8_t *expected;
    size_t expected_length;
    markdown_core_holder **holders;
    uint8_t **subtrees;
    size_t *subtree_lengths;
    size_t count;
} pr_generation;

static void pr_generation_clear(pr_generation *generation) {
    size_t i;
    if (generation->subtrees) {
        for (i = 0; i < generation->count; i++) {
            markdown_core_dump_free(generation->subtrees[i]);
        }
    }
    free(generation->holders);
    free(generation->subtrees);
    free(generation->subtree_lengths);
    markdown_core_dump_free(generation->expected);
    memset(generation, 0, sizeof(*generation));
}

/* A block whose children are all inline -- vacuously so for a block with
 * none, which is what puts the empty list under test as well. */
static int pr_is_leaf_block(markdown_core_node *node) {
    markdown_core_child_cursor cursor;
    markdown_core_node *child;
    if (!MARKDOWN_CORE_NODE_BLOCK_P(node)) {
        return 0;
    }
    for (child = markdown_core_child_first(node, &cursor); child;
        child = markdown_core_child_after(node, child, &cursor)) {
        if (!MARKDOWN_CORE_NODE_INLINE_P(child)) {
            return 0;
        }
    }
    return 1;
}

static int pr_collect_leaf_blocks(markdown_core_node *root, markdown_core_node ***out, size_t *count) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;
    markdown_core_node **items = NULL;
    size_t n = 0, cap = 0;
    if (!iter) {
        return -1;
    }
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER || !pr_is_leaf_block(node)) {
            continue;
        }
        if (n == cap) {
            markdown_core_node **grown;
            cap = cap ? cap * 2 : 16;
            grown = (markdown_core_node **)realloc(items, cap * sizeof(*items));
            if (!grown) {
                free(items);
                markdown_core_iter_free(iter);
                return -1;
            }
            items = grown;
        }
        items[n++] = node;
    }
    markdown_core_iter_free(iter);
    *out = items;
    *count = n;
    return 0;
}

static int pr_dump_equals(markdown_core_node *root, const uint8_t *expected, size_t expected_length) {
    size_t length = 0;
    uint8_t *dump = pr_dump(root, &length);
    int equal;
    if (!dump) {
        return 0;
    }
    equal = length == expected_length && memcmp(dump, expected, length) == 0;
    markdown_core_dump_free(dump);
    return equal;
}

static int pr_borrow_boundary(markdown_core_parser *parser, pr_generation *prev, int example, int boundary) {
    pr_generation cur;
    markdown_core_node **leaves = NULL;
    size_t i;
    int failed = 0;

    memset(&cur, 0, sizeof(cur));
    cur.tree = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!cur.tree) {
        fprintf(stderr, "example %d boundary %d: derivation failed\n", example, boundary);
        return -1;
    }
    cur.expected = pr_dump(cur.tree, &cur.expected_length);
    if (!cur.expected || pr_collect_leaf_blocks(cur.tree, &leaves, &cur.count) != 0) {
        fprintf(stderr, "example %d boundary %d: dump or leaf walk failed\n", example, boundary);
        markdown_core_node_free(cur.tree);
        pr_generation_clear(&cur);
        return -1;
    }
    cur.holders = (markdown_core_holder **)calloc(cur.count ? cur.count : 1, sizeof(*cur.holders));
    cur.subtrees = (uint8_t **)calloc(cur.count ? cur.count : 1, sizeof(*cur.subtrees));
    cur.subtree_lengths = (size_t *)calloc(cur.count ? cur.count : 1, sizeof(*cur.subtree_lengths));
    if (!cur.holders || !cur.subtrees || !cur.subtree_lengths) {
        fputs("out of memory\n", stderr);
        free(leaves);
        markdown_core_node_free(cur.tree);
        pr_generation_clear(&cur);
        return -1;
    }

    /* Every leaf borrows: the previous boundary's holder when its subtree is
     * unchanged, otherwise a fresh holder that takes the leaf's own children.
     * The "cache" is the set `cur.holders`, holding each holder once. */
    for (i = 0; i < cur.count; i++) {
        markdown_core_node *leaf = leaves[i];
        cur.subtrees[i] = pr_dump(leaf, &cur.subtree_lengths[i]);
        if (!cur.subtrees[i]) {
            failed = 1;
            break;
        }
        if (prev->tree && i < prev->count && prev->subtree_lengths[i] == cur.subtree_lengths[i] &&
            memcmp(prev->subtrees[i], cur.subtrees[i], cur.subtree_lengths[i]) == 0) {
            while (leaf->first_child) {
                markdown_core_node_free(leaf->first_child);
            }
            cur.holders[i] = prev->holders[i];
        } else {
            cur.holders[i] = markdown_core_holder_new(parser->mem);
            if (!cur.holders[i]) {
                failed = 1;
                break;
            }
            /* Pure pointer moves since #153 retired the store-time copy;
             * nothing can fail here, and the creation hold is the slot's. */
            markdown_core_holder_take_children(cur.holders[i], leaf);
        }
        markdown_core_node_borrow_children(leaf, cur.holders[i]);
    }
    free(leaves);
    if (failed) {
        /* Unwind WITHOUT publishing: a generation with NULL slots must never
         * reach the caller's release loop (landing review). Freeing the tree
         * drops every borrow the built slots took; each fresh slot then holds
         * exactly the one hold the cache took, and a slot aliasing the
         * previous boundary's holder took none. `prev` stays as it was, and
         * the caller's cleanup releases it as the last good generation. */
        size_t built;
        fprintf(stderr, "example %d boundary %d: could not build the borrow\n", example, boundary);
        markdown_core_node_free(cur.tree);
        cur.tree = NULL;
        for (built = 0; built < cur.count; built++) {
            int aliased = prev->tree && built < prev->count && cur.holders[built] == prev->holders[built];
            if (cur.holders[built] && !aliased) {
                markdown_core_holder_release(cur.holders[built]);
            }
        }
        pr_generation_clear(&cur);
        return -1;
    }

    if (!failed && !pr_dump_equals(cur.tree, cur.expected, cur.expected_length)) {
        fprintf(stderr, "example %d boundary %d: the borrowed tree reads differently\n", example, boundary);
        failed = 1;
    }
    if (!failed && prev->tree && !pr_dump_equals(prev->tree, prev->expected, prev->expected_length)) {
        fprintf(
            stderr,
            "example %d boundary %d: the previous borrow stopped reading once the new one was taken\n",
            example,
            boundary
        );
        failed = 1;
    }

    /* The cache evicts what this boundary did not re-share. */
    for (i = 0; i < prev->count; i++) {
        if (i >= cur.count || cur.holders[i] != prev->holders[i]) {
            markdown_core_holder_release(prev->holders[i]);
        }
    }
    if (!failed && prev->tree && !pr_dump_equals(prev->tree, prev->expected, prev->expected_length)) {
        fprintf(
            stderr,
            "example %d boundary %d: the previous borrow stopped reading once the cache let go\n",
            example,
            boundary
        );
        failed = 1;
    }
    if (prev->tree) {
        markdown_core_node_free(prev->tree);
        prev->tree = NULL;
    }
    if (!failed && !pr_dump_equals(cur.tree, cur.expected, cur.expected_length)) {
        fprintf(
            stderr,
            "example %d boundary %d: the tree stopped reading once the previous borrower was freed\n",
            example,
            boundary
        );
        failed = 1;
    }

    pr_generation_clear(prev);
    *prev = cur;
    return failed ? -1 : 0;
}

static int case_borrow_across_feed(const ts_spec_file *file) {
    size_t index;
    size_t boundaries = 0;
    int failures = 0;
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        const char *text = test_case->markdown;
        size_t length = test_case->markdown_length;
        markdown_core_parser *parser;
        pr_generation prev;
        size_t start = 0;
        size_t i;
        int boundary = 0;
        int failed = 0;

        pr_live_allocations = 0;
        parser = markdown_core_parser_new_with_mem(
            MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES,
            &PR_COUNTING_MEM
        );
        if (!parser || !markdown_core_core_extensions_attach(
                           parser,
                           MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                               MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK | MARKDOWN_CORE_CORE_EXTENSION_TASKLIST |
                               MARKDOWN_CORE_CORE_EXTENSION_FORMULA | MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE
                       )) {
            fprintf(stderr, "example %d: parser allocation failed\n", test_case->example);
            if (parser) {
                markdown_core_parser_free(parser);
            }
            failures++;
            continue;
        }
        /* This case plays the cache's part itself, so the engine's (T9)
         * stays out of its way. */
        parser->no_projection_cache = true;
        memset(&prev, 0, sizeof(prev));
        for (i = 0; i < length && !failed; i++) {
            if (text[i] == '\n') {
                markdown_core_parser_feed(parser, text + start, i - start + 1);
                start = i + 1;
                failed = pr_borrow_boundary(parser, &prev, test_case->example, ++boundary) != 0;
            }
        }
        if (!failed && start < length) {
            markdown_core_parser_feed(parser, text + start, length - start);
            failed = pr_borrow_boundary(parser, &prev, test_case->example, ++boundary) != 0;
        }
        boundaries += (size_t)boundary;

        /* The cache empties; the last borrow alone keeps every list alive. */
        for (i = 0; i < prev.count; i++) {
            markdown_core_holder_release(prev.holders[i]);
        }
        if (!failed && prev.tree && !pr_dump_equals(prev.tree, prev.expected, prev.expected_length)) {
            fprintf(stderr, "example %d: the last borrow stopped reading once the cache emptied\n", test_case->example);
            failed = 1;
        }
        if (prev.tree) {
            markdown_core_node_free(prev.tree);
        }
        pr_generation_clear(&prev);
        markdown_core_parser_free(parser);
        if (pr_live_allocations != 0) {
            fprintf(
                stderr,
                "example %d: %ld allocation(s) still live after every hold was released\n",
                test_case->example,
                pr_live_allocations
            );
            failed = 1;
        }
        if (failed) {
            failures++;
        }
    }
    printf(
        "borrow across feed: %zu/%zu examples agree over %zu boundaries\n",
        file->count - (size_t)failures,
        file->count,
        boundaries
    );
    return failures ? -1 : 0;
}

/* THE A/B ORACLE FOR A CHANGE TO THE PROJECTION. Feeds every case one line
 * at a time, derives after every line and finishes at the end, and prints
 * each tree's canonical dump under a stable header -- twice per boundary,
 * once with every extension and once with the comment strip added, because
 * the strip is a tail pass with its own removal path. Two builds run over
 * the same corpus and `diff` their output: a refactor of the projection is
 * correct when the diff is empty, and the diff names the first boundary that
 * moved when it is not. It is not a ctest row -- its oracle is another
 * build -- so it prints and judges nothing. */
static int pr_dump_boundaries_with(const ts_spec_case *test_case, int options, const char *label) {
    markdown_core_parser *parser = markdown_core_parser_new(options);
    const char *text = test_case->markdown;
    if (parser) {
        parser->no_projection_cache = pr_no_cache != 0;
    }
    size_t length = test_case->markdown_length;
    size_t start = 0, i;
    int boundary = 0;
    markdown_core_node *tree;
    uint8_t *dump;
    size_t dump_length;

    if (!parser || !markdown_core_core_extensions_attach(
                       parser,
                       MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                           MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK | MARKDOWN_CORE_CORE_EXTENSION_TASKLIST |
                           MARKDOWN_CORE_CORE_EXTENSION_FORMULA | MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE
                   )) {
        if (parser) {
            markdown_core_parser_free(parser);
        }
        return -1;
    }
    for (i = 0; i <= length; i++) {
        if (i < length && text[i] != '\n') {
            continue;
        }
        if (i == length && start == length) {
            break;
        }
        markdown_core_parser_feed(parser, text + start, (i < length ? i + 1 : length) - start);
        start = i + 1;
        boundary++;
        tree = markdown_core_parser_derive_tree(parser, parser->refmap);
        dump = tree ? pr_dump(tree, &dump_length) : NULL;
        printf("== example %d %s boundary %d\n", test_case->example, label, boundary);
        if (dump) {
            fwrite(dump, 1, dump_length, stdout);
        } else {
            puts("<derivation failed>");
        }
        markdown_core_dump_free(dump);
        if (tree) {
            markdown_core_node_free(tree);
        }
    }
    tree = markdown_core_parser_finish(parser);
    dump = tree ? pr_dump(tree, &dump_length) : NULL;
    printf("== example %d %s finish\n", test_case->example, label);
    if (dump) {
        fwrite(dump, 1, dump_length, stdout);
    } else {
        puts("<finish failed>");
    }
    markdown_core_dump_free(dump);
    if (tree) {
        markdown_core_node_free(tree);
    }
    markdown_core_parser_free(parser);
    return 0;
}

static int case_dump_boundaries(const ts_spec_file *file) {
    size_t index;
    int failures = 0;
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        if (pr_dump_boundaries_with(test_case, MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES, "plain") != 0 ||
            pr_dump_boundaries_with(
                test_case,
                MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS,
                "strip"
            ) != 0) {
            fprintf(stderr, "example %d: parser allocation failed\n", test_case->example);
            failures++;
        }
    }
    return failures ? -1 : 0;
}

/* THE CLOCK. What a Session's feed loop costs: every case fed one line at a
 * time with a derivation after every line and a finish at the end, nothing
 * dumped. `--repeats N` runs the whole corpus N times and reports the
 * fastest, which is the aggregation F1 and F11 use; the boundary count is
 * printed so two runs can be seen to have done the same work. The benchmark
 * ctest registration also collects the median of the repeats as a
 * pr-metrics row (#148), so the engine-side feed clock is tracked alongside
 * the bindings' feed loops. */
static int pr_repeats = 1;

static long pr_peak_rss_kib(void) {
#if defined(_WIN32)
    return -1;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
#endif
}

static int pr_compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int case_feed_loop(const ts_spec_file *file) {
    size_t index;
    int repeat;
    uint64_t best_ns = 0;
    uint64_t *repeat_ns;
    uint64_t median_ns;
    size_t total_bytes = 0;
    size_t boundaries = 0;
    size_t hits = 0, misses = 0;
    ts_bench_pin_allocator();
    repeat_ns = (uint64_t *)calloc((size_t)pr_repeats, sizeof(uint64_t));
    if (!repeat_ns) {
        fputs("feed loop: cannot allocate repeat samples\n", stderr);
        return -1;
    }
    for (repeat = 0; repeat < pr_repeats; repeat++) {
        uint64_t started = ts_monotonic_ns();
        uint64_t elapsed;
        size_t seen = 0;
        for (index = 0; index < file->count; index++) {
            const ts_spec_case *test_case = &file->cases[index];
            markdown_core_parser *parser = pr_parser_new();
            const char *text = test_case->markdown;
            size_t length = test_case->markdown_length;
            size_t start = 0, i;
            markdown_core_node *tree;
            if (!parser) {
                fprintf(stderr, "example %d: parser allocation failed\n", test_case->example);
                free(repeat_ns);
                return -1;
            }
            for (i = 0; i <= length; i++) {
                if (i < length && text[i] != '\n') {
                    continue;
                }
                if (i == length && start == length) {
                    break;
                }
                markdown_core_parser_feed(parser, text + start, (i < length ? i + 1 : length) - start);
                start = i + 1;
                seen++;
                tree = markdown_core_parser_derive_tree(parser, parser->refmap);
                if (tree) {
                    markdown_core_node_free(tree);
                }
            }
            /* Read before `finish`, which resets the parser and its ledger;
             * the finish projection itself is therefore not counted. */
            if (repeat == 0) {
                hits += parser->cache_hits;
                misses += parser->cache_misses;
                total_bytes += length;
            }
            tree = markdown_core_parser_finish(parser);
            if (tree) {
                markdown_core_node_free(tree);
            }
            markdown_core_parser_free(parser);
        }
        elapsed = ts_monotonic_ns() - started;
        repeat_ns[repeat] = elapsed;
        if (repeat == 0 || elapsed < best_ns) {
            best_ns = elapsed;
        }
        boundaries = seen;
    }
    printf(
        "feed loop: %zu documents, %zu boundaries, min of %d: %.2f ms; cache hits %zu, misses %zu (%.1f%%)\n",
        file->count,
        boundaries,
        pr_repeats,
        (double)best_ns / 1e6,
        hits,
        misses,
        hits + misses ? 100.0 * (double)hits / (double)(hits + misses) : 0.0
    );
    qsort(repeat_ns, (size_t)pr_repeats, sizeof(repeat_ns[0]), pr_compare_u64);
    median_ns = repeat_ns[pr_repeats / 2];
    free(repeat_ns);
    /* The human line above keeps the min the F-gates aggregate; the metrics
     * row carries the median like every other collected benchmark. */
    printf(
        "baseline runtime=c boundary=native_feed_loop workload=spec_corpus"
        " workload_version=1 bytes=%zu warmup=0 repeats=%d median_ns=%llu peak_rss_kib=%ld\n",
        total_bytes,
        pr_repeats,
        (unsigned long long)median_ns,
        pr_peak_rss_kib()
    );
    return 0;
}

/* MSVC deprecates the POSIX name under /WX (fallback_runner's fb_strdup is
 * the same answer). */
static char *pr_strdup(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

/* One case per `.md` file, in name order, so two runs see the same corpus in
 * the same order. */
static int pr_case_from_file(const char *path, int example, ts_spec_case *out) {
    size_t length = 0;
    uint8_t *bytes = ts_read_file(path, &length);
    if (!bytes) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->markdown = (char *)bytes;
    out->markdown_length = length;
    out->section = pr_strdup(path);
    out->example = example;
    return 0;
}

static int pr_name_compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Keep a copy of `name` when it ends in `.md`; both walk arms below filter
 * through here, so the Windows pattern match never widens the corpus. */
static int pr_push_md_name(char ***names, size_t *count, size_t *cap, const char *name) {
    size_t n = strlen(name);
    char *copy;
    if (n < 4 || strcmp(name + n - 3, ".md") != 0) {
        return 0;
    }
    if (*count == *cap) {
        char **grown;
        size_t grown_cap = *cap ? *cap * 2 : 64;
        grown = (char **)realloc(*names, grown_cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        *names = grown;
        *cap = grown_cap;
    }
    copy = pr_strdup(name);
    if (!copy) {
        return -1;
    }
    (*names)[(*count)++] = copy;
    return 0;
}

static int pr_load_md_dir(const char *dir, ts_spec_file *out) {
    char **names = NULL;
    size_t count = 0, cap = 0, i;
    int failed = 0;
#if defined(_WIN32)
    {
        char pattern[4096];
        WIN32_FIND_DATAA entry;
        HANDLE handle;
        snprintf(pattern, sizeof(pattern), "%s\\*.md", dir);
        handle = FindFirstFileA(pattern, &entry);
        if (handle == INVALID_HANDLE_VALUE) {
            return -1;
        }
        do {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                pr_push_md_name(&names, &count, &cap, entry.cFileName) != 0) {
                failed = 1;
                break;
            }
        } while (FindNextFileA(handle, &entry));
        FindClose(handle);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *entry;
        if (!d) {
            return -1;
        }
        while ((entry = readdir(d)) != NULL) {
            if (pr_push_md_name(&names, &count, &cap, entry->d_name) != 0) {
                failed = 1;
                break;
            }
        }
        closedir(d);
    }
#endif
    if (failed) {
        for (i = 0; i < count; i++) {
            free(names[i]);
        }
        free(names);
        return -1;
    }
    qsort(names, count, sizeof(*names), pr_name_compare);
    out->cases = (ts_spec_case *)calloc(count ? count : 1, sizeof(ts_spec_case));
    out->count = 0;
    if (!out->cases) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (pr_case_from_file(path, (int)i + 1, &out->cases[out->count]) == 0) {
            out->count++;
        }
        free(names[i]);
    }
    free(names);
    return 0;
}

/* T3 AND T4's GATE: THE CACHE KEY IS SOUND. Feeds every case one line at a
 * time, derives after every line, pairs each CST block with its derived block
 * (pre-order over blocks; the strip is off, so nothing is removed and the two
 * skeletons stay one to one), and keeps every CST block's reading of the key
 * -- its write stamp and the two map generations -- beside the dump of its
 * derived subtree. At the next boundary, for every block seen before:
 *
 *   1. an unchanged key MUST mean an unchanged subtree -- a stale key here
 *      is exactly a wrong tree served from the cache T9 builds on it;
 *   2. a CLOSED block's stamp must never move again -- the invariant the
 *      spine stamp rests on, and the one that gives a closed block its hit.
 *
 * It also reports what the key would buy: the share of block observations
 * whose key was unchanged (T9's hit-rate ceiling on this corpus) and, among
 * the changed, the share whose subtree had not in fact moved (the key's
 * imprecision -- spurious, never wrong). */
typedef struct pr_key_entry {
    const markdown_core_node *block;
    uint32_t stamp;
    size_t refgen;
    size_t footgen;
    int closed;
    uint8_t *dump;
    size_t dump_length;
} pr_key_entry;

typedef struct pr_key_set {
    pr_key_entry *items;
    size_t count;
} pr_key_set;

static void pr_key_set_clear(pr_key_set *set) {
    size_t i;
    for (i = 0; i < set->count; i++) {
        markdown_core_dump_free(set->items[i].dump);
    }
    free(set->items);
    set->items = NULL;
    set->count = 0;
}

static const pr_key_entry *pr_key_find(const pr_key_set *set, const markdown_core_node *block) {
    size_t i;
    for (i = 0; i < set->count; i++) {
        if (set->items[i].block == block) {
            return &set->items[i];
        }
    }
    return NULL;
}

/* Pre-order over BLOCK nodes only: a leaf block's inline children are not
 * descended into, and a block's siblings are blocks. */
static int pr_collect_blocks(markdown_core_node *root, markdown_core_node ***out, size_t *count) {
    markdown_core_node **items = NULL;
    size_t n = 0, cap = 0;
    markdown_core_iter walk;
    markdown_core_event_type ev_type;
    markdown_core_iter_init(&walk, root);
    while ((ev_type = markdown_core_iter_next(&walk)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *cur = markdown_core_iter_get_node(&walk);
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        if (!MARKDOWN_CORE_NODE_BLOCK_P(cur)) {
            /* An inline-class child (a directive's label) roots no blocks. */
            markdown_core_iter_skip_children(&walk);
            continue;
        }
        if (n == cap) {
            markdown_core_node **grown;
            cap = cap ? cap * 2 : 32;
            grown = (markdown_core_node **)realloc(items, cap * sizeof(*items));
            if (!grown) {
                free(items);
                markdown_core_iter_deinit(&walk);
                return -1;
            }
            items = grown;
        }
        items[n++] = cur;
    }
    *out = items;
    *count = n;
    return 0;
}

typedef struct pr_key_stats {
    size_t observations;
    size_t revisits;
    size_t unchanged_key;
    size_t changed_key;
    size_t spurious;
} pr_key_stats;

static int pr_key_boundary(
    markdown_core_parser *parser,
    pr_key_set *prev,
    pr_key_stats *stats,
    int example,
    int boundary
) {
    bufsize_t marks_before = parser->line_marks_size;
    markdown_core_node *tree = markdown_core_parser_derive_tree(parser, parser->refmap);
    markdown_core_node **cst = NULL, **derived = NULL;
    size_t cst_count = 0, derived_count = 0, i;
    pr_key_set cur;
    int failed = 0;
    size_t refgen = parser->refmap->generation;
    size_t footgen = parser->footnote_defs->generation;

    if (!tree) {
        fprintf(stderr, "example %d boundary %d: derivation failed\n", example, boundary);
        return -1;
    }
    /* Derive-then-feed is this case's shape, and it is the shape in which a
     * derivation that grew the mark vector (F21) corrupts the NEXT line's
     * positions -- so the growth is asserted here, at every boundary. */
    if (parser->line_marks_size != marks_before) {
        fprintf(
            stderr,
            "example %d boundary %d: the derivation grew the content-to-source map (%u -> %u marks)\n",
            example,
            boundary,
            (unsigned)marks_before,
            (unsigned)parser->line_marks_size
        );
        markdown_core_node_free(tree);
        return -1;
    }
    if (pr_collect_blocks(parser->root, &cst, &cst_count) != 0 ||
        pr_collect_blocks(tree, &derived, &derived_count) != 0) {
        fputs("out of memory\n", stderr);
        free(cst);
        markdown_core_node_free(tree);
        return -1;
    }
    if (cst_count != derived_count) {
        fprintf(
            stderr,
            "example %d boundary %d: %zu CST blocks but %zu derived blocks\n",
            example,
            boundary,
            cst_count,
            derived_count
        );
        free(cst);
        free(derived);
        markdown_core_node_free(tree);
        return -1;
    }
    cur.items = (pr_key_entry *)calloc(cst_count ? cst_count : 1, sizeof(*cur.items));
    cur.count = 0;
    if (!cur.items) {
        fputs("out of memory\n", stderr);
        free(cst);
        free(derived);
        markdown_core_node_free(tree);
        return -1;
    }
    for (i = 0; i < cst_count; i++) {
        pr_key_entry *entry = &cur.items[cur.count];
        const pr_key_entry *seen;
        entry->block = cst[i];
        entry->stamp = cst[i]->stamp;
        entry->refgen = refgen;
        entry->footgen = footgen;
        entry->closed = (cst[i]->flags & MARKDOWN_CORE_NODE__OPEN) == 0;
        entry->dump = pr_dump(derived[i], &entry->dump_length);
        if (!entry->dump) {
            failed = 1;
            break;
        }
        cur.count++;
        stats->observations++;
        seen = pr_key_find(prev, cst[i]);
        if (!seen) {
            continue;
        }
        stats->revisits++;
        if (seen->closed && seen->stamp != entry->stamp) {
            fprintf(
                stderr,
                "example %d boundary %d: a CLOSED %s at %d:%d was written (stamp %u -> %u)\n",
                example,
                boundary,
                markdown_core_node_get_type_string(cst[i]),
                cst[i]->start_line,
                cst[i]->start_column,
                seen->stamp,
                entry->stamp
            );
            failed = 1;
        }
        if (seen->stamp == entry->stamp && seen->refgen == refgen && seen->footgen == footgen) {
            stats->unchanged_key++;
            if (seen->dump_length != entry->dump_length || memcmp(seen->dump, entry->dump, entry->dump_length) != 0) {
                fprintf(
                    stderr,
                    "example %d boundary %d: STALE KEY -- %s at %d:%d kept stamp %u and generation %zu/%zu but its "
                    "projection moved:\n  before:\n%.*s  after:\n%.*s",
                    example,
                    boundary,
                    markdown_core_node_get_type_string(cst[i]),
                    cst[i]->start_line,
                    cst[i]->start_column,
                    entry->stamp,
                    refgen,
                    footgen,
                    (int)seen->dump_length,
                    (const char *)seen->dump,
                    (int)entry->dump_length,
                    (const char *)entry->dump
                );
                failed = 1;
            }
        } else {
            stats->changed_key++;
            if (seen->dump_length == entry->dump_length && memcmp(seen->dump, entry->dump, entry->dump_length) == 0) {
                stats->spurious++;
            }
        }
    }
    free(cst);
    free(derived);
    markdown_core_node_free(tree);
    pr_key_set_clear(prev);
    *prev = cur;
    return failed ? -1 : 0;
}

static int case_projection_key(const ts_spec_file *file) {
    size_t index;
    size_t boundaries = 0;
    pr_key_stats stats;
    int failures = 0;
    memset(&stats, 0, sizeof(stats));
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        markdown_core_parser *parser = pr_parser_new();
        const char *text = test_case->markdown;
        size_t length = test_case->markdown_length;
        pr_key_set prev;
        /* The key is sound only if a block RE-PROJECTED under an unchanged
         * key projects the same; served from the cache (T9) it trivially
         * would, so the cache is off here. */
        if (parser) {
            parser->no_projection_cache = true;
        }
        size_t start = 0, i;
        int boundary = 0;
        int failed = 0;
        if (!parser) {
            fprintf(stderr, "example %d: parser allocation failed\n", test_case->example);
            failures++;
            continue;
        }
        prev.items = NULL;
        prev.count = 0;
        for (i = 0; i <= length && !failed; i++) {
            if (i < length && text[i] != '\n') {
                continue;
            }
            if (i == length && start == length) {
                break;
            }
            markdown_core_parser_feed(parser, text + start, (i < length ? i + 1 : length) - start);
            start = i + 1;
            failed = pr_key_boundary(parser, &prev, &stats, test_case->example, ++boundary) != 0;
        }
        boundaries += (size_t)boundary;
        pr_key_set_clear(&prev);
        markdown_core_parser_free(parser);
        if (failed) {
            failures++;
        }
    }
    printf(
        "projection key: %zu/%zu examples sound over %zu boundaries; %zu block observations, %zu revisits, key "
        "unchanged %zu (%.1f%%), changed %zu of which %zu spurious (%.1f%%)\n",
        file->count - (size_t)failures,
        file->count,
        boundaries,
        stats.observations,
        stats.revisits,
        stats.unchanged_key,
        stats.revisits ? 100.0 * (double)stats.unchanged_key / (double)stats.revisits : 0.0,
        stats.changed_key,
        stats.spurious,
        stats.changed_key ? 100.0 * (double)stats.spurious / (double)stats.changed_key : 0.0
    );
    return failures ? -1 : 0;
}

/* T2 AND T5'S GATE: IDENTITY IS TOTAL, UNIQUE, PROJECTION-STABLE AND NEVER
 * RESURRECTED (docs/STREAMING.md §4 D4, F11). Feeds every case one line at a
 * time and derives TWICE after every line, asserting five things:
 *
 *   1. no node in a derived tree -- block OR inline -- carries identity 0: a
 *      lost mint, carry or numbering fails closed, and this is where it
 *      surfaces;
 *   2. no two blocks in one derivation share an identity, and no two
 *      SIBLINGS anywhere do -- which is the uniqueness any single ForEach
 *      needs, inline ordinals included;
 *   3. two projections of one unwritten CST name every node identically --
 *      identity is a fact about the CST, not about the derivation
 *      (closes F4);
 *   4. a block identity that left the tree never comes back. Inline
 *      ordinals are positional within their block and are not tracked here:
 *      a re-parsed block reusing ordinal 1 is the SAME slot continuing, not
 *      a resurrection.
 *
 * The finish tree joins the same asserts: it is the last projection, taken in
 * place on the CST, and the consumer joins it against the boundary trees. */
typedef struct pr_id_ledger {
    uint32_t *ids;
    size_t count;
    size_t cap;
} pr_id_ledger;

static int pr_id_ledger_has(const pr_id_ledger *ledger, uint32_t id) {
    size_t i;
    for (i = 0; i < ledger->count; i++) {
        if (ledger->ids[i] == id) {
            return 1;
        }
    }
    return 0;
}

static int pr_id_ledger_add(pr_id_ledger *ledger, uint32_t id) {
    if (ledger->count == ledger->cap) {
        uint32_t *grown;
        ledger->cap = ledger->cap ? ledger->cap * 2 : 32;
        grown = (uint32_t *)realloc(ledger->ids, ledger->cap * sizeof(*ledger->ids));
        if (!grown) {
            return -1;
        }
        ledger->ids = grown;
    }
    ledger->ids[ledger->count++] = id;
    return 0;
}

/* Every node in the tree, ENTER order, inline nodes included. */
static int pr_collect_all(markdown_core_node *root, markdown_core_node ***out, size_t *count) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;
    markdown_core_node **items = NULL;
    size_t n = 0, cap = 0;
    if (!iter) {
        return -1;
    }
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        if (n == cap) {
            markdown_core_node **grown;
            cap = cap ? cap * 2 : 64;
            grown = (markdown_core_node **)realloc(items, cap * sizeof(*items));
            if (!grown) {
                free(items);
                markdown_core_iter_free(iter);
                return -1;
            }
            items = grown;
        }
        items[n++] = markdown_core_iter_get_node(iter);
    }
    markdown_core_iter_free(iter);
    *out = items;
    *count = n;
    return 0;
}

static int pr_identity_boundary(
    markdown_core_parser *parser,
    markdown_core_node *tree,
    pr_id_ledger *alive,
    pr_id_ledger *dead,
    int example,
    const char *boundary,
    size_t *blocks_seen
) {
    markdown_core_node *second = NULL;
    markdown_core_node **blocks = NULL;
    markdown_core_node **all = NULL, **all_again = NULL;
    size_t count = 0, all_count = 0, all_again_count = 0, i, j;
    pr_id_ledger cur;
    int failed = 0;

    memset(&cur, 0, sizeof(cur));
    if (pr_collect_blocks(tree, &blocks, &count) != 0 || pr_collect_all(tree, &all, &all_count) != 0) {
        fputs("out of memory\n", stderr);
        free(blocks);
        return -1;
    }
    /* 1 and 2: total over every node, blocks unique in the derivation,
     * siblings unique everywhere. */
    for (i = 0; i < all_count && !failed; i++) {
        markdown_core_node *first_sibling, *later_sibling;
        if (all[i]->identifier == 0) {
            fprintf(
                stderr,
                "example %d %s: %s at %d:%d has no identity\n",
                example,
                boundary,
                markdown_core_node_get_type_string(all[i]),
                all[i]->start_line,
                all[i]->start_column
            );
            failed = 1;
        }
        markdown_core_child_cursor first_cursor;
        for (first_sibling = markdown_core_child_first(all[i], &first_cursor); first_sibling && !failed;
            first_sibling = markdown_core_child_after(all[i], first_sibling, &first_cursor)) {
            markdown_core_child_cursor later_cursor = first_cursor;
            for (later_sibling = markdown_core_child_after(all[i], first_sibling, &later_cursor);
                later_sibling && !failed;
                later_sibling = markdown_core_child_after(all[i], later_sibling, &later_cursor)) {
                if (first_sibling->identifier == later_sibling->identifier) {
                    fprintf(
                        stderr,
                        "example %d %s: two children of a %s share identity %u (%s and %s)\n",
                        example,
                        boundary,
                        markdown_core_node_get_type_string(all[i]),
                        (unsigned)first_sibling->identifier,
                        markdown_core_node_get_type_string(first_sibling),
                        markdown_core_node_get_type_string(later_sibling)
                    );
                    failed = 1;
                }
            }
        }
    }
    for (i = 0; i < count && !failed; i++) {
        for (j = i + 1; j < count && !failed; j++) {
            if (blocks[i]->identifier == blocks[j]->identifier) {
                fprintf(
                    stderr,
                    "example %d %s: two blocks share id %u (%s at %d:%d, %s at %d:%d)\n",
                    example,
                    boundary,
                    (unsigned)blocks[i]->identifier,
                    markdown_core_node_get_type_string(blocks[i]),
                    blocks[i]->start_line,
                    blocks[i]->start_column,
                    markdown_core_node_get_type_string(blocks[j]),
                    blocks[j]->start_line,
                    blocks[j]->start_column
                );
                failed = 1;
            }
        }
    }
    /* 3: the second projection names every node -- inline included -- as the
     * first did. `parser` is NULL for the finish tree, which has no second
     * projection to take. CACHE OFF for this one derivation: with it on,
     * every hit aliases the first tree's lists and the inline comparison is
     * a node against itself (landing review) -- a fresh projection re-parses
     * and re-numbers, which is what "two projections name every node
     * identically" must actually mean. */
    if (!failed && parser) {
        bool cache_was_off = parser->no_projection_cache;
        parser->no_projection_cache = true;
        second = markdown_core_parser_derive_tree(parser, parser->refmap);
        parser->no_projection_cache = cache_was_off;
        if (!second || pr_collect_all(second, &all_again, &all_again_count) != 0) {
            fprintf(stderr, "example %d %s: second derivation failed\n", example, boundary);
            failed = 1;
        } else if (all_again_count != all_count) {
            fprintf(
                stderr,
                "example %d %s: %zu nodes in one projection, %zu in the next\n",
                example,
                boundary,
                all_count,
                all_again_count
            );
            failed = 1;
        } else {
            for (i = 0; i < all_count; i++) {
                if (all[i]->identifier != all_again[i]->identifier || all[i]->type != all_again[i]->type) {
                    fprintf(
                        stderr,
                        "example %d %s: two projections name a %s at %d:%d differently (%u vs %u)\n",
                        example,
                        boundary,
                        markdown_core_node_get_type_string(all[i]),
                        all[i]->start_line,
                        all[i]->start_column,
                        (unsigned)all[i]->identifier,
                        (unsigned)all_again[i]->identifier
                    );
                    failed = 1;
                    break;
                }
            }
        }
    }
    /* 4: nothing this boundary shows was ever declared dead, and whatever the
     * previous boundary showed that this one does not is dead from here on. */
    for (i = 0; i < count && !failed; i++) {
        if (pr_id_ledger_has(dead, blocks[i]->identifier)) {
            fprintf(
                stderr,
                "example %d %s: dead id %u came back as %s at %d:%d\n",
                example,
                boundary,
                (unsigned)blocks[i]->identifier,
                markdown_core_node_get_type_string(blocks[i]),
                blocks[i]->start_line,
                blocks[i]->start_column
            );
            failed = 1;
        }
        if (!failed && pr_id_ledger_add(&cur, blocks[i]->identifier) != 0) {
            fputs("out of memory\n", stderr);
            failed = 1;
        }
    }
    if (!failed) {
        for (i = 0; i < alive->count; i++) {
            if (!pr_id_ledger_has(&cur, alive->ids[i]) && pr_id_ledger_add(dead, alive->ids[i]) != 0) {
                fputs("out of memory\n", stderr);
                failed = 1;
                break;
            }
        }
    }
    *blocks_seen += all_count;
    free(blocks);
    free(all);
    free(all_again);
    if (second) {
        markdown_core_node_free(second);
    }
    free(alive->ids);
    *alive = cur;
    return failed ? -1 : 0;
}

static int case_block_identity(const ts_spec_file *file) {
    size_t index;
    size_t boundaries = 0;
    size_t blocks_seen = 0;
    int failures = 0;
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        markdown_core_parser *parser = pr_parser_new();
        const char *text = test_case->markdown;
        size_t length = test_case->markdown_length;
        markdown_core_node *tree;
        pr_id_ledger alive, dead;
        size_t start = 0, i;
        int boundary = 0;
        int failed = 0;
        char label[32];
        if (!parser) {
            fprintf(stderr, "example %d: parser allocation failed\n", test_case->example);
            failures++;
            continue;
        }
        memset(&alive, 0, sizeof(alive));
        memset(&dead, 0, sizeof(dead));
        for (i = 0; i <= length && !failed; i++) {
            if (i < length && text[i] != '\n') {
                continue;
            }
            if (i == length && start == length) {
                break;
            }
            markdown_core_parser_feed(parser, text + start, (i < length ? i + 1 : length) - start);
            start = i + 1;
            snprintf(label, sizeof(label), "boundary %d", ++boundary);
            tree = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!tree) {
                fprintf(stderr, "example %d %s: derivation failed\n", test_case->example, label);
                failed = 1;
                break;
            }
            failed = pr_identity_boundary(parser, tree, &alive, &dead, test_case->example, label, &blocks_seen) != 0;
            markdown_core_node_free(tree);
        }
        boundaries += (size_t)boundary;
        if (!failed) {
            tree = markdown_core_parser_finish(parser);
            if (!tree) {
                fprintf(stderr, "example %d finish: projection failed\n", test_case->example);
                failed = 1;
            } else {
                failed =
                    pr_identity_boundary(NULL, tree, &alive, &dead, test_case->example, "finish", &blocks_seen) != 0;
                markdown_core_node_free(tree);
            }
        }
        free(alive.ids);
        free(dead.ids);
        markdown_core_parser_free(parser);
        if (failed) {
            failures++;
        }
    }
    printf(
        "block identity: %zu/%zu examples sound over %zu boundaries, %zu node observations\n",
        file->count - (size_t)failures,
        file->count,
        boundaries,
        blocks_seen
    );
    return failures ? -1 : 0;
}

/* T5'S SECOND HALF, AND D4'S TWO RULED FORKS, AS SHAPES (§4 D4). Each
 * document below crosses one of the identity-moving events, and the assert
 * says which node the consumer keeps: a retype keeps the id (setext,
 * paragraph -> table, the formula promotion); a split leaves the id on what
 * the reader already had (the table's lead paragraph, a surviving paragraph
 * losing its definitions); a death bequeaths it to the firstborn definition.
 * Recorded per boundary as the derived root's direct children. */
typedef struct ti_block {
    uint16_t type;
    uint32_t id;
} ti_block;

#define TI_MAX_CHILDREN 8
#define TI_MAX_RECORDS 8

typedef struct ti_record {
    ti_block items[TI_MAX_CHILDREN];
    size_t count;
} ti_record;

static int ti_snapshot(markdown_core_node *root, ti_record *out) {
    markdown_core_child_cursor cursor;
    markdown_core_node *child;
    out->count = 0;
    for (child = markdown_core_child_first(root, &cursor); child;
        child = markdown_core_child_after(root, child, &cursor)) {
        if (out->count == TI_MAX_CHILDREN) {
            return -1;
        }
        out->items[out->count].type = child->type;
        out->items[out->count].id = child->identifier;
        out->count++;
    }
    return 0;
}

static int ti_run(const char *text, ti_record *records, size_t *count) {
    markdown_core_parser *parser = pr_parser_new();
    size_t length = strlen(text);
    size_t start = 0, i;
    markdown_core_node *tree;
    int failed = 0;

    *count = 0;
    if (!parser) {
        return -1;
    }
    for (i = 0; i <= length && !failed; i++) {
        if (i < length && text[i] != '\n') {
            continue;
        }
        if (i == length && start == length) {
            break;
        }
        markdown_core_parser_feed(parser, text + start, (i < length ? i + 1 : length) - start);
        start = i + 1;
        tree = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!tree || *count == TI_MAX_RECORDS || ti_snapshot(tree, &records[*count]) != 0) {
            failed = 1;
        } else {
            (*count)++;
        }
        if (tree) {
            markdown_core_node_free(tree);
        }
    }
    if (!failed) {
        tree = markdown_core_parser_finish(parser);
        if (!tree || *count == TI_MAX_RECORDS || ti_snapshot(tree, &records[*count]) != 0) {
            failed = 1;
        } else {
            (*count)++;
        }
        if (tree) {
            markdown_core_node_free(tree);
        }
    }
    markdown_core_parser_free(parser);
    return failed ? -1 : 0;
}

static int ti_expect(
    const char *name,
    const ti_record *record,
    const char *when,
    const ti_block *expected,
    size_t expected_count
) {
    size_t i;
    if (record->count != expected_count) {
        fprintf(
            stderr,
            "%s at %s: %zu top-level blocks where %zu were expected\n",
            name,
            when,
            record->count,
            expected_count
        );
        return -1;
    }
    for (i = 0; i < expected_count; i++) {
        if (record->items[i].type != expected[i].type || record->items[i].id != expected[i].id) {
            fprintf(
                stderr,
                "%s at %s: child %zu is type %u id %u where type %u id %u was expected\n",
                name,
                when,
                i,
                (unsigned)record->items[i].type,
                (unsigned)record->items[i].id,
                (unsigned)expected[i].type,
                (unsigned)expected[i].id
            );
            return -1;
        }
    }
    return 0;
}

static int case_block_identity_transitions(const ts_spec_file *file) {
    ti_record r[TI_MAX_RECORDS];
    size_t n;
    int failures = 0;
    (void)file;

    /* A setext retype keeps the id: the underline rewrites the node in place. */
    {
        if (ti_run("text\n===\n", r, &n) != 0 || n < 2) {
            fputs("setext: run failed\n", stderr);
            failures++;
        } else {
            uint32_t x = r[0].items[0].id;
            ti_block p[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, x}};
            ti_block h[] = {{MARKDOWN_CORE_NODE_HEADING, x}};
            if (x == 0 || ti_expect("setext", &r[0], "boundary 1", p, 1) != 0 ||
                ti_expect("setext", &r[n - 1], "finish", h, 1) != 0) {
                failures++;
            }
        }
    }
    /* A paragraph -> table retype with no lead keeps the id the same way. */
    {
        if (ti_run("a|b\n-|-\n", r, &n) != 0 || n < 2) {
            fputs("table retype: run failed\n", stderr);
            failures++;
        } else {
            uint32_t x = r[0].items[0].id;
            ti_block p[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, x}};
            ti_block t[] = {{MARKDOWN_CORE_NODE_TABLE, x}};
            if (x == 0 || ti_expect("table retype", &r[0], "boundary 1", p, 1) != 0 ||
                ti_expect("table retype", &r[n - 1], "finish", t, 1) != 0) {
                failures++;
            }
        }
    }
    /* Fork 1: the lead paragraph keeps the id; the table is the new element. */
    {
        if (ti_run("Intro\na|b\n-|-\n", r, &n) != 0 || n < 3) {
            fputs("lead split: run failed\n", stderr);
            failures++;
        } else {
            uint32_t x = r[0].items[0].id;
            uint32_t y = r[n - 1].count == 2 ? r[n - 1].items[1].id : 0;
            ti_block p[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, x}};
            ti_block split[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, x}, {MARKDOWN_CORE_NODE_TABLE, y}};
            if (x == 0 || y == 0 || y == x || ti_expect("lead split", &r[0], "boundary 1", p, 1) != 0 ||
                ti_expect("lead split", &r[1], "boundary 2", p, 1) != 0 ||
                ti_expect("lead split", &r[n - 1], "finish", split, 2) != 0) {
                failures++;
            }
        }
    }
    /* Fork 3, death: the paragraph became the definition, so the definition
     * keeps the paragraph's id. */
    {
        if (ti_run("[a]: /url\n\nafter\n", r, &n) != 0 || n < 2) {
            fputs("definition death: run failed\n", stderr);
            failures++;
        } else {
            uint32_t x = r[0].items[0].id;
            uint32_t z = r[n - 1].count == 2 ? r[n - 1].items[1].id : 0;
            ti_block p[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, x}};
            ti_block fin[] = {{MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, x}, {MARKDOWN_CORE_NODE_PARAGRAPH, z}};
            if (x == 0 || z == 0 || z == x || ti_expect("definition death", &r[0], "boundary 1", p, 1) != 0 ||
                ti_expect("definition death", &r[n - 1], "finish", fin, 2) != 0) {
                failures++;
            }
        }
    }
    /* Fork 3, survival: the visible text keeps the id; the definition is a
     * fresh birth. */
    {
        if (ti_run("[a]: /url\ntext\n\nafter\n", r, &n) != 0 || n < 2) {
            fputs("definition survival: run failed\n", stderr);
            failures++;
        } else {
            uint32_t x = r[0].items[0].id;
            uint32_t y = r[n - 1].count == 3 ? r[n - 1].items[0].id : 0;
            uint32_t z = r[n - 1].count == 3 ? r[n - 1].items[2].id : 0;
            ti_block p[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, x}};
            ti_block fin[] = {
                {MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, y},
                {MARKDOWN_CORE_NODE_PARAGRAPH, x},
                {MARKDOWN_CORE_NODE_PARAGRAPH, z}
            };
            if (x == 0 || y == 0 || y == x || z == 0 || y == z ||
                ti_expect("definition survival", &r[0], "boundary 1", p, 1) != 0 ||
                ti_expect("definition survival", &r[n - 1], "finish", fin, 3) != 0) {
                failures++;
            }
        }
    }
    /* Fork 3, several definitions from one death: the firstborn inherits and
     * the rest are births. */
    {
        if (ti_run("[a]: /1\n[b]: /2\n\nafter\n", r, &n) != 0 || n < 2) {
            fputs("definition firstborn: run failed\n", stderr);
            failures++;
        } else {
            uint32_t x = r[0].items[0].id;
            uint32_t y = r[n - 1].count == 3 ? r[n - 1].items[1].id : 0;
            uint32_t z = r[n - 1].count == 3 ? r[n - 1].items[2].id : 0;
            ti_block p[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, x}};
            ti_block fin[] = {
                {MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, x},
                {MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, y},
                {MARKDOWN_CORE_NODE_PARAGRAPH, z}
            };
            if (x == 0 || y == 0 || y == x || z == 0 ||
                ti_expect("definition firstborn", &r[0], "boundary 1", p, 1) != 0 ||
                ti_expect("definition firstborn", &r[n - 1], "finish", fin, 3) != 0) {
                failures++;
            }
        }
    }
    /* Inline identity (§4 D4, amended): two inlines with the same content in
     * one block are distinguishable, and an append to the block leaves the
     * inlines the reader already had with the ordinals they already had. */
    {
        markdown_core_parser *parser = pr_parser_new();
        uint32_t first_pass[4], second_pass[4];
        size_t first_count = 0, second_count = 0;
        markdown_core_node *tree;
        int ok = parser != NULL;
        if (ok) {
            markdown_core_parser_feed(parser, "x [a](u) y [a](u)\n", 18);
            tree = markdown_core_parser_derive_tree(parser, parser->refmap);
            ok = tree != NULL;
            if (ok) {
                markdown_core_node **nodes = NULL;
                size_t node_count = 0, i;
                ok = pr_collect_all(tree, &nodes, &node_count) == 0;
                for (i = 0; ok && i < node_count; i++) {
                    if (nodes[i]->type == MARKDOWN_CORE_NODE_LINK && first_count < 4) {
                        first_pass[first_count++] = nodes[i]->identifier;
                    }
                }
                free(nodes);
                markdown_core_node_free(tree);
            }
        }
        if (ok) {
            markdown_core_parser_feed(parser, "more\n", 5);
            tree = markdown_core_parser_finish(parser);
            ok = tree != NULL;
            if (ok) {
                markdown_core_node **nodes = NULL;
                size_t node_count = 0, i;
                ok = pr_collect_all(tree, &nodes, &node_count) == 0;
                for (i = 0; ok && i < node_count; i++) {
                    if (nodes[i]->type == MARKDOWN_CORE_NODE_LINK && second_count < 4) {
                        second_pass[second_count++] = nodes[i]->identifier;
                    }
                }
                free(nodes);
                markdown_core_node_free(tree);
            }
        }
        if (parser) {
            markdown_core_parser_free(parser);
        }
        if (!ok || first_count != 2 || second_count != 2 || first_pass[0] == 0 || first_pass[0] == first_pass[1] ||
            first_pass[0] != second_pass[0] || first_pass[1] != second_pass[1]) {
            fprintf(
                stderr,
                "inline identity: %zu then %zu links, ids %u/%u then %u/%u -- want two distinct, both stable\n",
                first_count,
                second_count,
                first_count > 0 ? (unsigned)first_pass[0] : 0,
                first_count > 1 ? (unsigned)first_pass[1] : 0,
                second_count > 0 ? (unsigned)second_pass[0] : 0,
                second_count > 1 ? (unsigned)second_pass[1] : 0
            );
            failures++;
        }
    }
    /* Fork 2: the formula promotion carries the id across every projection. */
    {
        if (ti_run("$$e$$\n\nafter\n", r, &n) != 0 || n < 2 || r[0].count != 1) {
            fputs("formula promotion: run failed\n", stderr);
            failures++;
        } else {
            uint32_t x = r[0].items[0].id;
            uint32_t z = r[n - 1].count == 2 ? r[n - 1].items[1].id : 0;
            ti_block fin[] = {{MARKDOWN_CORE_NODE_FORMULA_BLOCK, x}, {MARKDOWN_CORE_NODE_PARAGRAPH, z}};
            if (x == 0 || z == 0 || z == x || ti_expect("formula promotion", &r[n - 1], "finish", fin, 2) != 0) {
                failures++;
            }
        }
    }

    printf("block identity transitions: %s\n", failures ? "shapes moved" : "8/8 shapes hold");
    return failures ? -1 : 0;
}

/* THE CACHE KEY'S EXTENSION AXIS (T9, amended on the landing review): an
 * attach between a derivation and the next projection must read every cached
 * list as stale. Autolink's "*inlines" pass is skipped for a borrowed block,
 * so a hit minted before the attach would hand `finish` the un-autolinked
 * list, and the same feed without the intermediate derivation would disagree.
 * The control attached everything up front; the probe attaches autolink
 * between the derivation that fills the cache and the finish that would have
 * hit it, and the pre-attach derivation must DIFFER from the control -- a
 * text autolink cannot change would let this gate pass while proving
 * nothing. */
static const char PR_ATTACH_TEXT[] = "visit www.example.com today\n\nsecond paragraph\n";

static int case_attach_invalidation(const ts_spec_file *file) {
    static const unsigned PR_ATTACH_REST =
        MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
        MARKDOWN_CORE_CORE_EXTENSION_TASKLIST | MARKDOWN_CORE_CORE_EXTENSION_FORMULA |
        MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE;
    markdown_core_parser *probe_parser = NULL;
    markdown_core_node *control_root = NULL;
    markdown_core_node *probe_root = NULL;
    markdown_core_node *derived = NULL;
    uint8_t *control_dump = NULL;
    uint8_t *probe_dump = NULL;
    uint8_t *derived_dump = NULL;
    size_t control_length = 0;
    size_t probe_length = 0;
    size_t derived_length = 0;
    int failures = 1;
    (void)file;

    control_root = pr_parse(PR_ATTACH_TEXT, sizeof(PR_ATTACH_TEXT) - 1);
    if (!control_root || !(control_dump = pr_dump(control_root, &control_length))) {
        fputs("attach invalidation: control parse failed\n", stderr);
        goto done;
    }

    probe_parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES);
    if (!probe_parser) {
        fputs("attach invalidation: parser allocation failed\n", stderr);
        goto done;
    }
    probe_parser->no_projection_cache = pr_no_cache != 0;
    if (!markdown_core_core_extensions_attach(probe_parser, PR_ATTACH_REST)) {
        fputs("attach invalidation: attach failed\n", stderr);
        goto done;
    }
    markdown_core_parser_feed(probe_parser, PR_ATTACH_TEXT, sizeof(PR_ATTACH_TEXT) - 1);
    derived = markdown_core_parser_derive_tree(probe_parser, probe_parser->refmap);
    if (!derived || !(derived_dump = pr_dump(derived, &derived_length))) {
        fputs("attach invalidation: derivation failed\n", stderr);
        goto done;
    }
    if (derived_length == control_length && memcmp(derived_dump, control_dump, control_length) == 0) {
        fputs("attach invalidation: the probe text does not exercise autolink\n", stderr);
        goto done;
    }
    if (!markdown_core_core_extensions_attach(probe_parser, MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK)) {
        fputs("attach invalidation: attach failed\n", stderr);
        goto done;
    }
    probe_root = markdown_core_parser_finish(probe_parser);
    if (!probe_root || !(probe_dump = pr_dump(probe_root, &probe_length))) {
        fputs("attach invalidation: finish failed\n", stderr);
        goto done;
    }
    if (probe_length != control_length || memcmp(probe_dump, control_dump, control_length) != 0) {
        fputs("attach invalidation: a hit outlived the attach\n", stderr);
        fprintf(
            stderr,
            "  control:\n%.*s  probe:\n%.*s",
            (int)control_length,
            (const char *)control_dump,
            (int)probe_length,
            (const char *)probe_dump
        );
        goto done;
    }
    failures = 0;

done:
    markdown_core_dump_free(control_dump);
    markdown_core_dump_free(probe_dump);
    markdown_core_dump_free(derived_dump);
    if (control_root) {
        markdown_core_node_free(control_root);
    }
    if (probe_root) {
        markdown_core_node_free(probe_root);
    }
    if (derived) {
        markdown_core_node_free(derived);
    }
    if (probe_parser) {
        markdown_core_parser_free(probe_parser);
    }
    printf("attach invalidation: %s\n", failures ? "stale list served" : "attach re-projects");
    return failures ? -1 : 0;
}

/* THE SHARING GATE (#161, D9): a hit is the RETAINED NODE ITSELF, so two
 * derivations of an unwritten CST must hand back the SAME PHYSICAL node for
 * a closed paragraph -- pointer equality, not value equality -- and both
 * trees must read correctly while alive and free independently afterwards.
 * Red without retention (a fresh clone per feed compares unequal), red
 * without the per-tree hold (a use-after-free under the sanitizers). */
static const markdown_core_node *sg_first_shared_block(markdown_core_node *root) {
    markdown_core_children cursor = markdown_core_node_children(root);
    for (; cursor.child; cursor = markdown_core_children_next(cursor)) {
        if (cursor.child->flags & MARKDOWN_CORE_NODE__SHARED) {
            return cursor.child;
        }
    }
    return NULL;
}

static int case_node_sharing(const ts_spec_file *file) {
    static const char SG_TEXT[] = "the first paragraph\n\nthe second paragraph\n\n";
    markdown_core_parser *parser;
    markdown_core_node *first = NULL;
    markdown_core_node *second = NULL;
    const markdown_core_node *first_shared;
    const markdown_core_node *second_shared;
    int failures = 0;
    (void)file;

    if (pr_no_cache) {
        printf("node sharing: skipped under --no-cache\n");
        return 0;
    }
    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    markdown_core_parser_feed(parser, SG_TEXT, sizeof(SG_TEXT) - 1);
    first = markdown_core_parser_derive_tree(parser, parser->refmap);
    second = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!first || !second) {
        fputs("node sharing: derivation failed\n", stderr);
        failures++;
    } else {
        first_shared = sg_first_shared_block(first);
        second_shared = sg_first_shared_block(second);
        if (!first_shared || first_shared != second_shared) {
            fputs("node sharing: two reads of an unwritten CST did not hand back the same node\n", stderr);
            failures++;
        }
        if (!failures) {
            size_t first_length = 0;
            size_t second_length = 0;
            uint8_t *first_dump = pr_dump(first, &first_length);
            uint8_t *second_dump = pr_dump(second, &second_length);
            if (!first_dump || !second_dump || first_length != second_length ||
                memcmp(first_dump, second_dump, first_length) != 0) {
                fputs("node sharing: the two trees dump differently\n", stderr);
                failures++;
            }
            markdown_core_dump_free(first_dump);
            markdown_core_dump_free(second_dump);
        }
        /* THE CONSUMER'S FREE IS NOT A RELEASE (review-found, P1): the
         * tree's one hold belongs to the vector entry, so a direct free
         * of the shared block -- and of a node inside it, and every
         * structural mutation against either -- must be a refused no-op.
         * Asserted the hard way: mutate, re-dump, then the frees below
         * and a fresh derive must still serve the retained node. Before
         * the fix the free here released the holder's hold a second
         * time and the interior free destroyed a listed inline, so the
         * re-dump or the teardown died under ASan. */
        if (!failures) {
            markdown_core_node *mut = (markdown_core_node *)first_shared;
            markdown_core_node *interior = markdown_core_node_first_child(mut);
            markdown_core_node *stray = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
            size_t baseline_length = 0;
            uint8_t *baseline = pr_dump(first, &baseline_length);
            markdown_core_node_free(mut);
            markdown_core_node_unlink(mut);
            if (interior) {
                markdown_core_node_free(interior);
                markdown_core_node_unlink(interior);
            }
            if (stray) {
                if (markdown_core_node_append_child(mut, stray) ||
                    (interior && markdown_core_node_insert_after(interior, stray)) ||
                    markdown_core_node_replace(interior ? interior : mut, stray)) {
                    fputs("node sharing: a structural mutation reached the shared projection\n", stderr);
                    failures++;
                }
                markdown_core_node_free(stray);
            }
            /* Content is as frozen as structure: a write through any
             * setter would show in every tree at once, so each answers
             * 0 for a shared node, and the trim walks away untaken. The
             * dumps against the baseline are the proof either way. */
            if ((interior && markdown_core_node_set_literal(interior, "rewritten")) ||
                markdown_core_node_set_string_content(mut, "rewritten") ||
                markdown_core_node_set_type(mut, MARKDOWN_CORE_NODE_HEADING)) {
                fputs("node sharing: a content setter reached the shared projection\n", stderr);
                failures++;
            }
            markdown_core_node_unput(mut, 3);
            if (!failures && baseline) {
                size_t first_length = 0;
                size_t second_length = 0;
                uint8_t *first_dump = pr_dump(first, &first_length);
                uint8_t *second_dump = pr_dump(second, &second_length);
                if (!first_dump || first_length != baseline_length ||
                    memcmp(first_dump, baseline, baseline_length) != 0 || !second_dump ||
                    second_length != baseline_length || memcmp(second_dump, baseline, baseline_length) != 0) {
                    fputs("node sharing: the trees dump differently after refused mutations\n", stderr);
                    failures++;
                }
                markdown_core_dump_free(first_dump);
                markdown_core_dump_free(second_dump);
            }
            markdown_core_dump_free(baseline);
        }
    }
    /* Free in store order first, then the survivor must still read. */
    markdown_core_node_free(first);
    if (!failures && second) {
        size_t length = 0;
        uint8_t *dump = pr_dump(second, &length);
        if (!dump || length == 0) {
            fputs("node sharing: the surviving tree stopped reading after the first was freed\n", stderr);
            failures++;
        }
        markdown_core_dump_free(dump);
    }
    markdown_core_node_free(second);
    /* Both consumers gone, the cache's own hold must still serve: a fresh
     * derive hits the retained node. Before the fix the mutation block's
     * stolen release left the holder destroyed by the frees above, and
     * this derive read it freed. */
    if (!failures) {
        size_t hits_before = parser->cache_hits;
        markdown_core_node *third = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!third || parser->cache_hits < hits_before + 1) {
            fputs("node sharing: the cache's hold did not survive its consumers\n", stderr);
            failures++;
        }
        if (third) {
            markdown_core_node_free(third);
        }
    }
    markdown_core_parser_free(parser);
    printf("node sharing: %s\n", failures ? "the clone is back" : "a hit is the retained node itself");
    return failures ? -1 : 0;
}

/* THE HOOK-ONCE GATE (F25, review-found three times): a name-selected hook
 * runs at the projection that RECORDS a block, never at a derive hit (the
 * shared EXIT used to re-queue every stored block per feed), and exactly
 * ONCE per stored block at the seal -- finish hands back the CST shell, and
 * the hook must reproduce its node-level effect there; zero lost the cached
 * mutation, and the historical double queue ran it twice. A counting AND
 * mutating hook watches all three: the recording derive runs it once per
 * paragraph, a second derive adds nothing, finish adds exactly one per
 * stored block, and the sealed tree dumps identically to the hit derive's
 * below the document line (the document node's own scope end differs by
 * design: open at a derive, final at the seal). Skipped under --no-cache,
 * where every projection records and the counts grow by design. */
static size_t ho_hook_runs;

/* Counts AND mutates in place (no replacement, so the block still stores):
 * the retype is the node-level effect whose loss at the seal the gate must
 * see -- a count alone cannot tell a reproduced shell from a stale one. */
static void ho_counting_hook(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block
) {
    (void)extension;
    (void)parser;
    ho_hook_runs++;
    if (markdown_core_node_set_type(*block, MARKDOWN_CORE_NODE_HEADING)) {
        markdown_core_node_set_heading_level(*block, 3);
    }
}

static const markdown_core_syntax_extension HO_COUNTING_EXTENSION = {
    .name = "hook_once_probe",
    .postprocess_block_func = ho_counting_hook,
    .postprocess_blocks = "paragraph\0",
};

static int case_hook_once(const ts_spec_file *file) {
    static const char HO_TEXT[] = "the first paragraph\n\nthe second paragraph\n\n";
    markdown_core_parser *parser;
    markdown_core_node *first = NULL;
    markdown_core_node *second = NULL;
    markdown_core_node *sealed = NULL;
    size_t after_recording;
    int failures = 0;
    (void)file;

    if (pr_no_cache) {
        printf("hook once: skipped under --no-cache\n");
        return 0;
    }
    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    ho_hook_runs = 0;
    if (!markdown_core_parser_attach_syntax_extension(parser, &HO_COUNTING_EXTENSION)) {
        fputs("hook once: the counting extension did not attach\n", stderr);
        markdown_core_parser_free(parser);
        return -1;
    }
    markdown_core_parser_feed(parser, HO_TEXT, sizeof(HO_TEXT) - 1);
    first = markdown_core_parser_derive_tree(parser, parser->refmap);
    after_recording = ho_hook_runs;
    if (!first || after_recording != 2) {
        fprintf(
            stderr,
            "hook once: the recording projection ran the hook %zu times for 2 paragraphs\n",
            after_recording
        );
        failures++;
    }
    second = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!second || ho_hook_runs != after_recording) {
        fprintf(stderr, "hook once: a derive hit re-ran the hook (%zu -> %zu)\n", after_recording, ho_hook_runs);
        failures++;
    }
    sealed = markdown_core_parser_finish(parser);
    if (!sealed || ho_hook_runs != after_recording + 2) {
        fprintf(
            stderr,
            "hook once: the seal ran the hook %zu times over %zu for 2 stored blocks -- one each reproduces "
            "the shell's node, zero loses the cached mutation, two each was the double queue\n",
            sealed ? ho_hook_runs - after_recording : (size_t)0,
            after_recording
        );
        failures++;
    }
    /* The seal's answer IS the derive's answer below the document line: the
     * hit derive served the retained node with the retype baked in, and the
     * seal reproduced the same retype on the CST shell. The document node's
     * own scope end differs by design -- open at a derive, final at the
     * seal -- so the compare starts past each dump's first line. A seal
     * that skipped the hook still dumps paragraphs here. */
    if (!failures && second && sealed) {
        size_t second_length = 0;
        size_t sealed_length = 0;
        uint8_t *second_dump = pr_dump(second, &second_length);
        uint8_t *sealed_dump = pr_dump(sealed, &sealed_length);
        const uint8_t *second_body = second_dump ? memchr(second_dump, '\n', second_length) : NULL;
        const uint8_t *sealed_body = sealed_dump ? memchr(sealed_dump, '\n', sealed_length) : NULL;
        size_t second_body_length = second_body ? second_length - (size_t)(second_body - second_dump) : 0;
        size_t sealed_body_length = sealed_body ? sealed_length - (size_t)(sealed_body - sealed_dump) : 0;
        if (!second_body || !sealed_body || second_body_length != sealed_body_length ||
            memcmp(second_body, sealed_body, second_body_length) != 0) {
            fputs("hook once: the sealed tree lost the cached node-level mutation\n", stderr);
            failures++;
        }
        markdown_core_dump_free(second_dump);
        markdown_core_dump_free(sealed_dump);
    }
    if (first) {
        markdown_core_node_free(first);
    }
    if (second) {
        markdown_core_node_free(second);
    }
    if (sealed) {
        markdown_core_node_free(sealed);
    }
    markdown_core_parser_free(parser);
    printf("hook once: %s\n", failures ? "a hit still runs hooks" : "hooks run where the store is");
    return failures ? -1 : 0;
}

/* The document's spine-memo entry (F27 generalized the doc memo into the
 * per-container table); NULL when no run is recorded for the root. */
static markdown_core_child_memo *cm_root_memo(markdown_core_parser *parser) {
    size_t i;
    for (i = 0; i < parser->spine_memo_size; i++) {
        if (parser->spine_memos[i].container == parser->root) {
            return parser->spine_memos[i].memo;
        }
    }
    return NULL;
}

static int cm_dump_contains(const uint8_t *dump, size_t length, const char *needle) {
    size_t n = strlen(needle);
    size_t i;
    if (!dump || length < n) {
        return 0;
    }
    for (i = 0; i + n <= length; i++) {
        if (memcmp(dump + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* THE STABLE-PREFIX MEMO's gate (#161, F25), read through the internal
 * fields, because the memo has no public face and its failure modes -- a
 * stale prefix served, a hold given back twice, an OPEN block recorded --
 * do not all show in a rendering. Six acts: the run records and serves
 * (the recorded nodes themselves, one boundary per tree, the hit ledger
 * moving by the whole run and the miss ledger not at all); it extends
 * while older trees keep the smaller boundary they consumed, through
 * every free order; an OPEN block never enters -- recorded open, the
 * lines it takes after would be lost by every later consumer, since the
 * write clock covers exactly the spine the memo skips; a consulted
 * generation's move stales the run whole, the fallback resolves what the
 * run would have served stale, and the record REBUILDS while the old
 * tree keeps the old memo and the old answer alive; the parser dies
 * before the last consumer; and --no-cache builds nothing. */
static int case_child_memo(const ts_spec_file *file) {
    static const char CM_P1[] = "the first paragraph\n\n";
    static const char CM_P2[] = "the second bears *emphasis*\n\n";
    static const char CM_P3[] = "the third paragraph\n\n";
    static const char CM_P4[] = "the fourth paragraph\n\n";
    static const char CM_OPEN[] = "omega grows\n";
    static const char CM_CLOSE[] = "and keeps growing\n\n";
    static const char CM_REF[] = "see [x] for details\n\n";
    static const char CM_DEF[] = "[x]: /url\n\n";
    markdown_core_parser *parser = NULL;
    markdown_core_node *t1 = NULL;
    markdown_core_node *t2 = NULL;
    markdown_core_node *t3 = NULL;
    markdown_core_node *t4 = NULL;
    markdown_core_node *t5 = NULL;
    markdown_core_node *t6 = NULL;
    markdown_core_node *t7 = NULL;
    markdown_core_node *t8 = NULL;
    markdown_core_node *t9 = NULL;
    uint8_t *baseline2 = NULL;
    uint8_t *baseline7 = NULL;
    uint8_t *baseline8 = NULL;
    uint8_t *baseline9 = NULL;
    size_t baseline2_len = 0;
    size_t baseline7_len = 0;
    size_t baseline8_len = 0;
    size_t baseline9_len = 0;
    int failures = 0;
    (void)file;

    if (pr_no_cache) {
        puts("child memo: skipped under --no-cache (the memo is the cache's)");
        return 0;
    }
    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }

    /* Act 1: three closed paragraphs record a run; the next tree consumes
     * it whole. */
    markdown_core_parser_feed(parser, CM_P1, sizeof(CM_P1) - 1);
    markdown_core_parser_feed(parser, CM_P2, sizeof(CM_P2) - 1);
    markdown_core_parser_feed(parser, CM_P3, sizeof(CM_P3) - 1);
    t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!t1) {
        fputs("child memo: first derivation failed\n", stderr);
        failures++;
    } else {
        if (t1->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) {
            fputs("child memo: the recording tree claims a prefix no memo served\n", stderr);
            failures++;
        }
        if (!cm_root_memo(parser) || cm_root_memo(parser)->count != 3) {
            fprintf(
                stderr,
                "child memo: the run recorded %zu of 3 closed blocks\n",
                cm_root_memo(parser) ? cm_root_memo(parser)->count : (size_t)0
            );
            failures++;
        }
    }
    if (!failures) {
        size_t hits_before = parser->cache_hits;
        size_t misses_before = parser->cache_misses;
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t2) {
            fputs("child memo: consuming derivation failed\n", stderr);
            failures++;
        } else {
            size_t i;
            if (!(t2->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) || t2->link.memo_ref->boundary != 3 ||
                t2->link.memo_ref->memo != cm_root_memo(parser)) {
                fputs("child memo: the consumer does not carry the memo's boundary\n", stderr);
                failures++;
            }
            /* The extension-owned payload arm stays NULL (review-found):
             * a document-selected name hook receives this node, and the
             * attach path trusts any non-NULL `as.opaque` as a payload --
             * an integer boundary there would be dereferenced and later
             * handed to `opaque_free_func`. */
            if (t2->as.opaque != NULL) {
                fputs("child memo: the consumer's document carries a fake extension payload\n", stderr);
                failures++;
            }
            if (parser->cache_hits - hits_before != 3 || parser->cache_misses != misses_before) {
                fprintf(
                    stderr,
                    "child memo: the consume moved the ledger by %zu hits and %zu misses, not 3 and 0\n",
                    parser->cache_hits - hits_before,
                    parser->cache_misses - misses_before
                );
                failures++;
            }
            for (i = 0; !failures && i < 3; i++) {
                if (t2->children.vec[i] != t1->children.vec[i]) {
                    fputs("child memo: the consumed prefix is not the recorded nodes themselves\n", stderr);
                    failures++;
                }
            }
            baseline2 = pr_dump(t2, &baseline2_len);
            if (!failures && baseline2) {
                size_t first_len = 0;
                uint8_t *first_dump = pr_dump(t1, &first_len);
                if (!first_dump || first_len != baseline2_len || memcmp(first_dump, baseline2, first_len) != 0) {
                    fputs("child memo: recorder and consumer dump differently\n", stderr);
                    failures++;
                }
                if (first_dump) {
                    markdown_core_dump_free(first_dump);
                }
            }
        }
    }

    /* Act 2: the memo extends; each tree keeps the boundary IT consumed
     * while the memo's count moves on. */
    if (!failures) {
        markdown_core_parser_feed(parser, CM_P4, sizeof(CM_P4) - 1);
        t3 = markdown_core_parser_derive_tree(parser, parser->refmap);
        t4 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t3 || !t4) {
            fputs("child memo: extending derivations failed\n", stderr);
            failures++;
        } else if (!(t3->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) || !(t4->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) ||
                   t3->link.memo_ref->boundary != 3 || t4->link.memo_ref->boundary != 4 || t3->children.count != 4 ||
                   !cm_root_memo(parser) || cm_root_memo(parser)->count != 4) {
            fputs("child memo: the extension moved the boundaries with the count\n", stderr);
            failures++;
        } else if (t4->children.vec[0] != t3->children.vec[0] || t4->children.vec[3] != t3->children.vec[3]) {
            fputs("child memo: the extended run is not the same nodes\n", stderr);
            failures++;
        }
    }
    /* The free order: the widest boundary first, the recorder, the middle
     * consumer -- each must give back exactly the holds its own boundary
     * says -- and the first consumer then reads on alone. */
    if (t4) {
        markdown_core_node_free(t4);
        t4 = NULL;
    }
    if (t1) {
        markdown_core_node_free(t1);
        t1 = NULL;
    }
    if (t3) {
        markdown_core_node_free(t3);
        t3 = NULL;
    }
    if (!failures && t2 && baseline2) {
        size_t len = 0;
        uint8_t *dump = pr_dump(t2, &len);
        if (!dump || len != baseline2_len || memcmp(dump, baseline2, len) != 0) {
            fputs("child memo: the surviving consumer stopped reading after its siblings died\n", stderr);
            failures++;
        }
        if (dump) {
            markdown_core_dump_free(dump);
        }
    }
    if (t2) {
        markdown_core_node_free(t2);
        t2 = NULL;
    }

    /* Act 2b: an edit below the boundary DISSOLVES the tree's memo hold
     * into the per-entry holds it stood in for (review-found): a prepend
     * shifts the run, and a fixed boundary would make the free walk skip
     * the new child and release a shifted entry the tree never held. The
     * PARSER's memo must ride through untouched: the next derivation
     * still consumes it whole. */
    if (!failures) {
        markdown_core_node *edited = markdown_core_parser_derive_tree(parser, parser->refmap);
        markdown_core_node *fresh = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
        markdown_core_node *after = NULL;
        if (!edited || !fresh) {
            fputs("child memo: the dissolve act could not build its pieces\n", stderr);
            failures++;
        } else if (!markdown_core_node_prepend_child(edited, fresh)) {
            fputs("child memo: the derived document refused a prepend\n", stderr);
            failures++;
        } else {
            fresh = NULL;
            if ((edited->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) || edited->children.count != 5 ||
                !(edited->children.vec[1]->flags & MARKDOWN_CORE_NODE__SHARED)) {
                fputs("child memo: the edit below the boundary did not dissolve the run\n", stderr);
                failures++;
            }
        }
        if (fresh) {
            markdown_core_node_free(fresh);
        }
        if (edited) {
            markdown_core_node_free(edited);
        }
        if (!failures) {
            after = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!after || !(after->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) || after->link.memo_ref->boundary != 4) {
                fputs("child memo: the dissolve reached the parser's memo\n", stderr);
                failures++;
            }
            if (after) {
                markdown_core_node_free(after);
            }
        }
    }

    /* Act 3: an OPEN block never enters the run. */
    if (!failures) {
        markdown_core_parser_feed(parser, CM_OPEN, sizeof(CM_OPEN) - 1);
        t5 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t5) {
            fputs("child memo: open-block derivation failed\n", stderr);
            failures++;
        } else if (!cm_root_memo(parser) || cm_root_memo(parser)->count != 4) {
            fprintf(
                stderr,
                "child memo: the open block was recorded (count %zu, not 4)\n",
                cm_root_memo(parser) ? cm_root_memo(parser)->count : (size_t)0
            );
            failures++;
        } else if (t5->children.count != 5) {
            fputs("child memo: the open block fell out of the derived tree\n", stderr);
            failures++;
        }
        markdown_core_parser_feed(parser, CM_CLOSE, sizeof(CM_CLOSE) - 1);
        t6 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t6) {
            fputs("child memo: closing derivation failed\n", stderr);
            failures++;
        } else {
            size_t len = 0;
            uint8_t *dump = pr_dump(t6, &len);
            if (!cm_dump_contains(dump, len, "keeps growing")) {
                fputs("child memo: the closed block lost the line fed while it was open\n", stderr);
                failures++;
            }
            if (dump) {
                markdown_core_dump_free(dump);
            }
            if (!cm_root_memo(parser) || cm_root_memo(parser)->count != 5) {
                fputs("child memo: the closed block did not extend the run\n", stderr);
                failures++;
            }
        }
        if (t5) {
            markdown_core_node_free(t5);
            t5 = NULL;
        }
        if (t6) {
            markdown_core_node_free(t6);
            t6 = NULL;
        }
    }

    /* Act 4: a consulted generation's move stales the run whole; the
     * fallback resolves; the record rebuilds; the old tree keeps the old
     * memo and the old answer. */
    if (!failures) {
        markdown_core_parser_feed(parser, CM_REF, sizeof(CM_REF) - 1);
        t7 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t7 || !(t7->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) || t7->link.memo_ref->memo != cm_root_memo(parser) ||
            cm_root_memo(parser)->count != 6) {
            fputs("child memo: the consulting block did not extend the run\n", stderr);
            failures++;
        } else {
            baseline7 = pr_dump(t7, &baseline7_len);
            markdown_core_parser_feed(parser, CM_DEF, sizeof(CM_DEF) - 1);
            t8 = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!t8) {
                fputs("child memo: post-definition derivation failed\n", stderr);
                failures++;
            } else {
                baseline8 = pr_dump(t8, &baseline8_len);
                if (t8->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) {
                    fputs("child memo: a stale run was consumed past a definition's arrival\n", stderr);
                    failures++;
                }
                if (!baseline7 || !baseline8 ||
                    (baseline7_len == baseline8_len && memcmp(baseline7, baseline8, baseline7_len) == 0)) {
                    fputs("child memo: the fallback did not resolve the reference\n", stderr);
                    failures++;
                }
                /* The rebuilt run reaches 7, one PAST the prose: the
                 * arrived definition lives on as a ReferenceDefinition
                 * NODE (node.h -- the mdast model), an inline-less leaf
                 * that used to cap every run at its index and now
                 * enrolls with the bare leaves (F27), so the rebuild
                 * records it too. */
                if (!cm_root_memo(parser) || cm_root_memo(parser) == t7->link.memo_ref->memo ||
                    cm_root_memo(parser)->count != 7) {
                    fputs("child memo: the record did not rebuild after the move\n", stderr);
                    failures++;
                }
                if (t8->children.count != 7 || !(t8->children.vec[6]->flags & MARKDOWN_CORE_NODE__SHARED)) {
                    fputs("child memo: the definition node did not enroll with the bare leaves\n", stderr);
                    failures++;
                }
                if (!failures && baseline7) {
                    size_t relen = 0;
                    uint8_t *redump = pr_dump(t7, &relen);
                    if (!redump || relen != baseline7_len || memcmp(redump, baseline7, relen) != 0) {
                        fputs("child memo: the old tree lost its answer to the rebuild\n", stderr);
                        failures++;
                    }
                    if (redump) {
                        markdown_core_dump_free(redump);
                    }
                }
            }
        }
    }

    /* Act 5: the parser dies first; the last consumer holds the rebuilt
     * memo, and through it every entry, alone. */
    if (!failures) {
        t9 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t9 || !(t9->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) || t9->link.memo_ref->boundary != 7) {
            fputs("child memo: the rebuilt run did not serve\n", stderr);
            failures++;
        } else {
            baseline9 = pr_dump(t9, &baseline9_len);
            if (!baseline9 || !baseline8 || baseline9_len != baseline8_len ||
                memcmp(baseline9, baseline8, baseline9_len) != 0) {
                fputs("child memo: the rebuilt run serves a different answer than its recorder\n", stderr);
                failures++;
            }
        }
    }
    if (t8) {
        markdown_core_node_free(t8);
        t8 = NULL;
    }
    if (t7) {
        markdown_core_node_free(t7);
        t7 = NULL;
    }
    markdown_core_parser_free(parser);
    parser = NULL;
    if (!failures && t9 && baseline9) {
        size_t len = 0;
        uint8_t *dump = pr_dump(t9, &len);
        if (!dump || len != baseline9_len || memcmp(dump, baseline9, len) != 0) {
            fputs("child memo: the last consumer stopped reading when the parser died\n", stderr);
            failures++;
        }
        if (dump) {
            markdown_core_dump_free(dump);
        }
    }
    if (t9) {
        markdown_core_node_free(t9);
        t9 = NULL;
    }

    /* Act 6: the cache's switch is the memo's switch. */
    if (!failures) {
        markdown_core_parser *bare = pr_parser_new();
        if (!bare) {
            failures++;
        } else {
            markdown_core_node *only;
            bare->no_projection_cache = true;
            markdown_core_parser_feed(bare, CM_P1, sizeof(CM_P1) - 1);
            markdown_core_parser_feed(bare, CM_P2, sizeof(CM_P2) - 1);
            only = markdown_core_parser_derive_tree(bare, bare->refmap);
            if (only) {
                markdown_core_node_free(only);
            }
            only = markdown_core_parser_derive_tree(bare, bare->refmap);
            if (only) {
                markdown_core_node_free(only);
            }
            if (cm_root_memo(bare)) {
                fputs("child memo: --no-cache still built a memo\n", stderr);
                failures++;
            }
            markdown_core_parser_free(bare);
        }
    }

    /* Act 7: a bare leaf with a NAME HOOK is retained too (F27): the
     * formula extension declares code_block for its promotion, so the
     * fence runs a tail -- and the store at the tail's end now keeps what
     * the hook declined to replace, the promotion-memo shape T9's
     * amendment named. Without it the fence missed every derivation and
     * capped every run at its index. */
    if (!failures) {
        static const char CM_FENCED[] = "before the fence\n\n```c\nint x;\n```\n\nafter the fence\n\n";
        markdown_core_parser *fenced = pr_parser_new();
        if (!fenced) {
            failures++;
        } else {
            markdown_core_node *first = NULL;
            markdown_core_node *second = NULL;
            size_t misses_before;
            markdown_core_parser_feed(fenced, CM_FENCED, sizeof(CM_FENCED) - 1);
            first = markdown_core_parser_derive_tree(fenced, fenced->refmap);
            misses_before = fenced->cache_misses;
            second = markdown_core_parser_derive_tree(fenced, fenced->refmap);
            if (!first || !second || fenced->cache_misses != misses_before || !cm_root_memo(fenced) ||
                cm_root_memo(fenced)->count != 3 || !(second->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) ||
                second->link.memo_ref->boundary != 3) {
                fputs("child memo: the hooked fence is not retained, and caps the run\n", stderr);
                failures++;
            }
            if (first) {
                markdown_core_node_free(first);
            }
            if (second) {
                markdown_core_node_free(second);
            }
            markdown_core_parser_free(fenced);
        }
    }

    if (t1) {
        markdown_core_node_free(t1);
    }
    if (t2) {
        markdown_core_node_free(t2);
    }
    if (t3) {
        markdown_core_node_free(t3);
    }
    if (t4) {
        markdown_core_node_free(t4);
    }
    if (t5) {
        markdown_core_node_free(t5);
    }
    if (t6) {
        markdown_core_node_free(t6);
    }
    if (t7) {
        markdown_core_node_free(t7);
    }
    if (t8) {
        markdown_core_node_free(t8);
    }
    if (t9) {
        markdown_core_node_free(t9);
    }
    if (parser) {
        markdown_core_parser_free(parser);
    }
    if (baseline2) {
        markdown_core_dump_free(baseline2);
    }
    if (baseline7) {
        markdown_core_dump_free(baseline7);
    }
    if (baseline8) {
        markdown_core_dump_free(baseline8);
    }
    if (baseline9) {
        markdown_core_dump_free(baseline9);
    }
    printf("child memo: %s\n", failures ? "the run does not hold" : "one hold and a memcpy serve the closed prefix");
    return failures ? -1 : 0;
}

static size_t cr_list_hook_runs;

/* Counts and leaves the node in place: a name hook on a CONTAINER must
 * not cost the container its retention (review-found) -- the identity
 * across derivations is the discriminator, the count proves the hit ran
 * no tail. */
static void cr_list_hook(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block
) {
    (void)extension;
    (void)parser;
    (void)block;
    cr_list_hook_runs++;
}

static const markdown_core_syntax_extension CR_LIST_EXTENSION = {
    .name = "container_retention_probe",
    .postprocess_block_func = cr_list_hook,
    .postprocess_blocks = "list\0",
};

static size_t cr_prune_hook_runs;

/* Edits INSIDE the block it is handed -- the contract's own words -- by
 * removing any nested list from the handed list's items. Before the
 * store moved off the tail, the inner list was already FROZEN when this
 * ran on the outer one, and the removal was a silently refused no-op
 * (review-found). */
static void cr_pruning_hook(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block
) {
    markdown_core_children item = markdown_core_node_children(*block);
    (void)extension;
    (void)parser;
    cr_prune_hook_runs++;
    for (; item.child; item = markdown_core_children_next(item)) {
        markdown_core_children inner = markdown_core_node_children(item.child);
        for (; inner.child; inner = markdown_core_children_next(inner)) {
            markdown_core_node *candidate = (markdown_core_node *)inner.child;
            if (markdown_core_node_get_type(candidate) == MARKDOWN_CORE_NODE_LIST) {
                markdown_core_node_unlink(candidate);
                markdown_core_node_free(candidate);
                break;
            }
        }
    }
}

static const markdown_core_syntax_extension CR_PRUNING_EXTENSION = {
    .name = "container_prune_probe",
    .postprocess_block_func = cr_pruning_hook,
    .postprocess_blocks = "list\0",
};

static size_t cr_third_hook_runs;

/* Removes the FIRST item once three exist -- the cross-feed inside edit
 * (review-found): while the list stays open its items must stay fresh,
 * or the third feed's hook meets a frozen first item and the removal is
 * a silently refused no-op. */
static void cr_third_item_hook(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block
) {
    size_t count = 0;
    markdown_core_children item = markdown_core_node_children(*block);
    (void)extension;
    (void)parser;
    cr_third_hook_runs++;
    for (; item.child; item = markdown_core_children_next(item)) {
        count++;
    }
    if (count >= 3) {
        markdown_core_node *first = (markdown_core_node *)markdown_core_node_children(*block).child;
        markdown_core_node_unlink(first);
        markdown_core_node_free(first);
    }
}

static const markdown_core_syntax_extension CR_THIRD_EXTENSION = {
    .name = "container_third_probe",
    .postprocess_block_func = cr_third_item_hook,
    .postprocess_blocks = "list\0",
};

/* Reaches PAST the closed first item to the paragraph inside it -- the
 * deep cross-feed edit (review-found): the item closes on the second
 * feed but the hooked list above it stays open, and a climb that
 * stopped at the first closed ancestor stored the paragraph anyway,
 * handing the third feed's removal a frozen target. */
static void cr_deep_target_hook(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block
) {
    size_t count = 0;
    markdown_core_children item = markdown_core_node_children(*block);
    (void)extension;
    (void)parser;
    for (; item.child; item = markdown_core_children_next(item)) {
        count++;
    }
    if (count >= 3) {
        const markdown_core_node *first = markdown_core_node_children(*block).child;
        markdown_core_node *inside = (markdown_core_node *)markdown_core_node_children(first).child;
        if (inside) {
            markdown_core_node_unlink(inside);
            markdown_core_node_free(inside);
        }
    }
}

static const markdown_core_syntax_extension CR_DEEP_EXTENSION = {
    .name = "container_deep_probe",
    .postprocess_block_func = cr_deep_target_hook,
    .postprocess_blocks = "list\0",
};

/* Removes the block it is handed -- the contract's other allowance, and
 * the one that frees a whole subtree mid-drain: the sweep must never
 * read what lived under it (review-found). */
static void cr_removing_hook(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block
) {
    markdown_core_node *doomed = *block;
    (void)extension;
    (void)parser;
    markdown_core_node_unlink(doomed);
    markdown_core_node_free(doomed);
    *block = NULL;
}

static const markdown_core_syntax_extension CR_REMOVING_EXTENSION = {
    .name = "container_removal_probe",
    .postprocess_block_func = cr_removing_hook,
    .postprocess_blocks = "list\0",
};

/* CONTAINER RETENTION's gate (#161, F27): a CLOSED container is one
 * retainable value -- the hit is the retained ARRAY node itself, its
 * subtree never entered -- keyed on its stamp with the consulted bits OR'd
 * up from its entries, so a definition's arrival re-derives exactly the
 * containers whose subtrees asked while the rest keep serving by
 * identity. Four acts: a closed list serves pointer-identical across
 * derivations; the closed ITEMS of a still-open list already serve while
 * the list itself stays fresh; a consulted move re-derives the asking
 * container per-child (the non-asking sibling still hits) and resolves;
 * and every free order unwinds, the parser first included. */
static int case_container_retention(const ts_spec_file *file) {
    static const char CR_LIST[] = "- alpha one\n- beta two\n- gamma three\n\nclosing paragraph\n\n";
    static const char CR_ITEM1[] = "- first item here\n";
    static const char CR_ITEM2[] = "- second item here\n";
    static const char CR_ITEM3[] = "- third item here\n";
    static const char CR_REFLIST[] = "- plain item text\n- see [x] label\n\nafter the list\n\n";
    static const char CR_DEF[] = "[x]: /url\n\n";
    markdown_core_parser *parser = NULL;
    markdown_core_node *t1 = NULL;
    markdown_core_node *t2 = NULL;
    int failures = 0;
    (void)file;

    if (pr_no_cache) {
        puts("container retention: skipped under --no-cache");
        return 0;
    }

    /* Act 1: the closed list is ONE retained value. */
    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    markdown_core_parser_feed(parser, CR_LIST, sizeof(CR_LIST) - 1);
    t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
    t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!t1 || !t2) {
        fputs("container retention: derivations failed\n", stderr);
        failures++;
    } else {
        markdown_core_node *first_list = t1->children.vec[0];
        markdown_core_node *second_list = t2->children.vec[0];
        if (first_list != second_list || !(second_list->flags & MARKDOWN_CORE_NODE__SHARED) ||
            !MARKDOWN_CORE_NODE_ARRAY_P(second_list) || second_list->children.count != 3) {
            fputs("container retention: two reads of a closed list are not the same node\n", stderr);
            failures++;
        }
        if (!failures) {
            size_t len1 = 0;
            size_t len2 = 0;
            uint8_t *d1 = pr_dump(t1, &len1);
            uint8_t *d2 = pr_dump(t2, &len2);
            if (!d1 || !d2 || len1 != len2 || memcmp(d1, d2, len1) != 0) {
                fputs("container retention: the retained list dumps differently\n", stderr);
                failures++;
            }
            if (d1) {
                markdown_core_dump_free(d1);
            }
            if (d2) {
                markdown_core_dump_free(d2);
            }
        }
    }
    /* Free order both ways, the parser last here. */
    if (t1) {
        markdown_core_node_free(t1);
        t1 = NULL;
    }
    if (t2) {
        markdown_core_node_free(t2);
        t2 = NULL;
    }
    markdown_core_parser_free(parser);
    parser = NULL;

    /* Act 2: the still-open list's CLOSED items already serve. */
    if (!failures) {
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        markdown_core_parser_feed(parser, CR_ITEM1, sizeof(CR_ITEM1) - 1);
        markdown_core_parser_feed(parser, CR_ITEM2, sizeof(CR_ITEM2) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        markdown_core_parser_feed(parser, CR_ITEM3, sizeof(CR_ITEM3) - 1);
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1 || !t2) {
            fputs("container retention: open-list derivations failed\n", stderr);
            failures++;
        } else {
            markdown_core_node *open1 = t1->children.vec[0];
            markdown_core_node *open2 = t2->children.vec[0];
            if ((open1->flags & MARKDOWN_CORE_NODE__SHARED) || (open2->flags & MARKDOWN_CORE_NODE__SHARED)) {
                fputs("container retention: an OPEN list was retained\n", stderr);
                failures++;
            } else if (!MARKDOWN_CORE_NODE_ARRAY_P(open1) || !MARKDOWN_CORE_NODE_ARRAY_P(open2) ||
                       open1->children.count != 2 || open2->children.count != 3) {
                fputs("container retention: the open lists lost their items\n", stderr);
                failures++;
            } else if (open2->children.vec[0] != open1->children.vec[0] ||
                       !(open1->children.vec[0]->flags & MARKDOWN_CORE_NODE__SHARED)) {
                fputs("container retention: a closed item is not the same node across derivations\n", stderr);
                failures++;
            } else if (!(open2->flags & MARKDOWN_CORE_NODE__MEMO_PREFIX) || open2->link.memo_ref->boundary != 1) {
                /* The open LIST consumes its own spine memo (F27): the
                 * first derivation recorded its one closed item, and the
                 * second entered it as a memcpy under one hold -- the
                 * boundary is the tree's own, not the memo's count. */
                fputs("container retention: the open list did not consume its spine memo\n", stderr);
                failures++;
            }
        }
        /* The PARSER dies first this time; the trees must keep reading. */
        markdown_core_parser_free(parser);
        parser = NULL;
        if (!failures && t2) {
            size_t len = 0;
            uint8_t *dump = pr_dump(t2, &len);
            if (!dump || len == 0) {
                fputs("container retention: the tree stopped reading after the parser died\n", stderr);
                failures++;
            }
            if (dump) {
                markdown_core_dump_free(dump);
            }
        }
        if (t2) {
            markdown_core_node_free(t2);
            t2 = NULL;
        }
        if (t1) {
            markdown_core_node_free(t1);
            t1 = NULL;
        }
    }

    /* Act 3: a consulted move re-derives the asking container and
     * resolves; the non-asking sibling still serves by identity. */
    if (!failures) {
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        markdown_core_parser_feed(parser, CR_REFLIST, sizeof(CR_REFLIST) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        markdown_core_parser_feed(parser, CR_DEF, sizeof(CR_DEF) - 1);
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1 || !t2) {
            fputs("container retention: consulted derivations failed\n", stderr);
            failures++;
        } else {
            markdown_core_node *stale_list = t1->children.vec[0];
            markdown_core_node *fresh_list = t2->children.vec[0];
            markdown_core_node *closing1 = t1->children.vec[1];
            markdown_core_node *closing2 = t2->children.vec[1];
            size_t len1 = 0;
            size_t len2 = 0;
            uint8_t *d1 = pr_dump(t1, &len1);
            uint8_t *d2 = pr_dump(t2, &len2);
            if (stale_list == fresh_list) {
                fputs("container retention: the asking list survived the definition's arrival\n", stderr);
                failures++;
            }
            if (fresh_list->children.count != 2 || fresh_list->children.vec[0] != stale_list->children.vec[0]) {
                fputs("container retention: the non-asking item did not serve by identity\n", stderr);
                failures++;
            }
            if (closing1 != closing2) {
                fputs("container retention: the non-asking sibling was re-derived\n", stderr);
                failures++;
            }
            if (!d1 || !d2 || (len1 == len2 && memcmp(d1, d2, len1) == 0)) {
                fputs("container retention: the definition's arrival resolved nothing\n", stderr);
                failures++;
            }
            if (d1) {
                markdown_core_dump_free(d1);
            }
            if (d2) {
                markdown_core_dump_free(d2);
            }
        }
        if (t1) {
            markdown_core_node_free(t1);
            t1 = NULL;
        }
        if (t2) {
            markdown_core_node_free(t2);
            t2 = NULL;
        }
        markdown_core_parser_free(parser);
        parser = NULL;
    }

    /* Act 4: a container whose entry CANNOT be shared -- a directive
     * block's CST-resident label rides the derivation arena -- is merely
     * unstored, never a holder that references arena memory. The
     * discriminating order: derive, FREE (that derivation's arena dies
     * with the tree), derive again, and READ -- a store that ignored the
     * unshared entry would now serve a pointer into the dead arena. */
    if (!failures) {
        static const char CR_DIRECTIVE[] = "::note[with a *label* here]{k=v}\n\nafter block\n\n";
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        markdown_core_parser_feed(parser, CR_DIRECTIVE, sizeof(CR_DIRECTIVE) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1) {
            fputs("container retention: directive derivation failed\n", stderr);
            failures++;
        } else {
            size_t len1 = 0;
            uint8_t *baseline = pr_dump(t1, &len1);
            markdown_core_node_free(t1);
            t1 = NULL;
            t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!t2 || !baseline) {
                fputs("container retention: directive re-derivation failed\n", stderr);
                failures++;
            } else {
                size_t len2 = 0;
                uint8_t *dump = pr_dump(t2, &len2);
                if (!dump || len1 != len2 || memcmp(baseline, dump, len1) != 0) {
                    fputs("container retention: the directive lost its label to a dead arena\n", stderr);
                    failures++;
                }
                if (dump) {
                    markdown_core_dump_free(dump);
                }
            }
            if (baseline) {
                markdown_core_dump_free(baseline);
            }
            if (t2) {
                markdown_core_node_free(t2);
                t2 = NULL;
            }
        }
        markdown_core_parser_free(parser);
        parser = NULL;
    }

    /* Act 5: a name hook on the container does not cost it retention
     * (review-found): the extension queues the closed list for tail
     * work, and the tail's end stores what the hook left in place --
     * its unswept items first, at the ancestor's own tail where the
     * subtree is provably alive, then the list itself. The FIRST
     * derivation is the storing miss; every later one is the retained
     * node by identity, no tail, the count frozen at one. */
    if (!failures) {
        markdown_core_node *t3 = NULL;
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        if (!markdown_core_parser_attach_syntax_extension(parser, &CR_LIST_EXTENSION)) {
            fputs("container retention: could not attach the probe extension\n", stderr);
            markdown_core_parser_free(parser);
            return -1;
        }
        cr_list_hook_runs = 0;
        markdown_core_parser_feed(parser, CR_LIST, sizeof(CR_LIST) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        t3 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1 || !t2 || !t3) {
            fputs("container retention: hooked derivations failed\n", stderr);
            failures++;
        } else if (t1->children.vec[0] != t2->children.vec[0] || t2->children.vec[0] != t3->children.vec[0] ||
                   !(t3->children.vec[0]->flags & MARKDOWN_CORE_NODE__SHARED)) {
            fputs("container retention: a name hook cost the list its retention\n", stderr);
            failures++;
        } else if (cr_list_hook_runs != 1) {
            fprintf(
                stderr,
                "container retention: the list's hook ran %zu times over one storing miss and two hits\n",
                cr_list_hook_runs
            );
            failures++;
        }
        if (t1) {
            markdown_core_node_free(t1);
            t1 = NULL;
        }
        if (t2) {
            markdown_core_node_free(t2);
            t2 = NULL;
        }
        if (t3) {
            markdown_core_node_free(t3);
        }
        markdown_core_parser_free(parser);
        parser = NULL;
    }

    /* Act 6: a hook that REMOVES its container frees the whole subtree
     * mid-drain, and the sweep must not have kept a pointer into it
     * (review-found): the items were candidates with a hooked ancestor,
     * excluded exactly because this can happen. The sanitizer is the
     * judge; the dumps prove the removal itself projected cleanly. */
    if (!failures) {
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        if (!markdown_core_parser_attach_syntax_extension(parser, &CR_REMOVING_EXTENSION)) {
            fputs("container retention: could not attach the removal probe\n", stderr);
            markdown_core_parser_free(parser);
            return -1;
        }
        markdown_core_parser_feed(parser, CR_LIST, sizeof(CR_LIST) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1 || !t2) {
            fputs("container retention: removing derivations failed\n", stderr);
            failures++;
        } else if (t1->children.count != 1 || t2->children.count != 1) {
            fputs("container retention: the removed list left the wrong tree behind\n", stderr);
            failures++;
        } else {
            size_t len1 = 0;
            size_t len2 = 0;
            uint8_t *d1 = pr_dump(t1, &len1);
            uint8_t *d2 = pr_dump(t2, &len2);
            if (!d1 || !d2 || len1 != len2 || memcmp(d1, d2, len1) != 0) {
                fputs("container retention: the removals dump differently\n", stderr);
                failures++;
            }
            if (d1) {
                markdown_core_dump_free(d1);
            }
            if (d2) {
                markdown_core_dump_free(d2);
            }
        }
        if (t1) {
            markdown_core_node_free(t1);
            t1 = NULL;
        }
        if (t2) {
            markdown_core_node_free(t2);
            t2 = NULL;
        }
        markdown_core_parser_free(parser);
        parser = NULL;
    }

    /* Act 7: a hook EDITS INSIDE the block it is handed -- the
     * contract's own words -- and the edit must land: the outer list's
     * hook removes the nested list from its first item, which the old
     * tail-time store had already frozen by the time the outer hook ran
     * (review-found; the planted early pass reproduces the silent
     * refusal). The edited container then stores WITH the edit and
     * serves by identity, the hooks never re-run. */
    if (!failures) {
        static const char CR_NESTED[] = "- outer one\n  - inner a\n  - inner b\n- outer two\n\nclose\n\n";
        markdown_core_node *t3 = NULL;
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        if (!markdown_core_parser_attach_syntax_extension(parser, &CR_PRUNING_EXTENSION)) {
            fputs("container retention: could not attach the pruning probe\n", stderr);
            markdown_core_parser_free(parser);
            return -1;
        }
        cr_prune_hook_runs = 0;
        markdown_core_parser_feed(parser, CR_NESTED, sizeof(CR_NESTED) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        t3 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1 || !t2 || !t3) {
            fputs("container retention: pruning derivations failed\n", stderr);
            failures++;
        } else {
            markdown_core_node *outer = t1->children.vec[0];
            markdown_core_children item = markdown_core_node_children(outer);
            markdown_core_children inner;
            int lists_inside = 0;
            for (; item.child; item = markdown_core_children_next(item)) {
                for (inner = markdown_core_node_children(item.child); inner.child;
                    inner = markdown_core_children_next(inner)) {
                    if (markdown_core_node_get_type((markdown_core_node *)inner.child) == MARKDOWN_CORE_NODE_LIST) {
                        lists_inside++;
                    }
                }
            }
            if (lists_inside != 0) {
                fputs("container retention: the inside edit was silently refused\n", stderr);
                failures++;
            }
            if (t2->children.vec[0] != outer || t3->children.vec[0] != outer ||
                !(outer->flags & MARKDOWN_CORE_NODE__SHARED)) {
                fputs("container retention: the edited container did not serve by identity\n", stderr);
                failures++;
            }
            if (cr_prune_hook_runs != 2) {
                fprintf(
                    stderr,
                    "container retention: the pruning hook ran %zu times over one storing miss and two hits\n",
                    cr_prune_hook_runs
                );
                failures++;
            }
        }
        if (t1) {
            markdown_core_node_free(t1);
            t1 = NULL;
        }
        if (t2) {
            markdown_core_node_free(t2);
            t2 = NULL;
        }
        if (t3) {
            markdown_core_node_free(t3);
        }
        markdown_core_parser_free(parser);
        parser = NULL;
    }

    /* Act 8: the ancestor's future edit outranks retention
     * (review-found): a list hook that removes the first item once three
     * exist must find it EDITABLE on the third feed, so items under a
     * hooked OPEN list stay fresh per feed; once the list closes, the
     * hook's last word lands in the closing drain and the whole subtree
     * stores with it, serving by identity after. */
    if (!failures) {
        static const char CR_ONE[] = "- item one\n";
        static const char CR_TWO[] = "- item two\n";
        static const char CR_THREE[] = "- item three\n";
        static const char CR_CLOSER[] = "\nclosing paragraph\n\n";
        markdown_core_node *t3 = NULL;
        markdown_core_node *t4 = NULL;
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        if (!markdown_core_parser_attach_syntax_extension(parser, &CR_THIRD_EXTENSION)) {
            fputs("container retention: could not attach the third-item probe\n", stderr);
            markdown_core_parser_free(parser);
            return -1;
        }
        cr_third_hook_runs = 0;
        markdown_core_parser_feed(parser, CR_ONE, sizeof(CR_ONE) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        markdown_core_parser_feed(parser, CR_TWO, sizeof(CR_TWO) - 1);
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        markdown_core_parser_feed(parser, CR_THREE, sizeof(CR_THREE) - 1);
        t3 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1 || !t2 || !t3) {
            fputs("container retention: incremental hooked derivations failed\n", stderr);
            failures++;
        } else {
            markdown_core_node *open_list = t3->children.vec[0];
            if (!MARKDOWN_CORE_NODE_ARRAY_P(open_list) || open_list->children.count != 2) {
                fputs("container retention: the third feed's removal was silently refused\n", stderr);
                failures++;
            }
        }
        if (t1) {
            markdown_core_node_free(t1);
            t1 = NULL;
        }
        if (t2) {
            markdown_core_node_free(t2);
            t2 = NULL;
        }
        if (t3) {
            markdown_core_node_free(t3);
            t3 = NULL;
        }
        if (!failures) {
            markdown_core_parser_feed(parser, CR_CLOSER, sizeof(CR_CLOSER) - 1);
            t3 = markdown_core_parser_derive_tree(parser, parser->refmap);
            t4 = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!t3 || !t4) {
                fputs("container retention: closing hooked derivations failed\n", stderr);
                failures++;
            } else if (t3->children.vec[0] != t4->children.vec[0] ||
                       !(t4->children.vec[0]->flags & MARKDOWN_CORE_NODE__SHARED) ||
                       t4->children.vec[0]->children.count != 2) {
                fputs("container retention: the closed edited list did not serve by identity\n", stderr);
                failures++;
            }
            if (t3) {
                markdown_core_node_free(t3);
                t3 = NULL;
            }
            if (t4) {
                markdown_core_node_free(t4);
            }
        }
        markdown_core_parser_free(parser);
        parser = NULL;
    }

    /* Act 9: the climb past CLOSED intermediates (review-found): the
     * paragraph inside the first item sits beneath a closed item under
     * an open hooked list, and it must stay as editable as the item
     * itself -- a climb that stopped at the first closed ancestor stored
     * it on the second feed and silently refused the third feed's
     * removal. Once the list closes, the subtree stores WITH the
     * paragraph gone and serves by identity. */
    if (!failures) {
        static const char CR_ONE[] = "- item one\n";
        static const char CR_TWO[] = "- item two\n";
        static const char CR_THREE[] = "- item three\n";
        static const char CR_CLOSER[] = "\nclosing paragraph\n\n";
        markdown_core_node *t3 = NULL;
        markdown_core_node *t4 = NULL;
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        if (!markdown_core_parser_attach_syntax_extension(parser, &CR_DEEP_EXTENSION)) {
            fputs("container retention: could not attach the deep-target probe\n", stderr);
            markdown_core_parser_free(parser);
            return -1;
        }
        markdown_core_parser_feed(parser, CR_ONE, sizeof(CR_ONE) - 1);
        t1 = markdown_core_parser_derive_tree(parser, parser->refmap);
        markdown_core_parser_feed(parser, CR_TWO, sizeof(CR_TWO) - 1);
        t2 = markdown_core_parser_derive_tree(parser, parser->refmap);
        markdown_core_parser_feed(parser, CR_THREE, sizeof(CR_THREE) - 1);
        t3 = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!t1 || !t2 || !t3) {
            fputs("container retention: deep-target derivations failed\n", stderr);
            failures++;
        } else {
            markdown_core_node *open_list = t3->children.vec[0];
            const markdown_core_node *first_item = markdown_core_node_children(open_list).child;
            if (!MARKDOWN_CORE_NODE_ARRAY_P(open_list) || open_list->children.count != 3 || !first_item) {
                fputs("container retention: the deep-target list lost its shape\n", stderr);
                failures++;
            } else if (markdown_core_node_children(first_item).child != NULL) {
                fputs("container retention: the deep removal was silently refused\n", stderr);
                failures++;
            }
        }
        if (t1) {
            markdown_core_node_free(t1);
            t1 = NULL;
        }
        if (t2) {
            markdown_core_node_free(t2);
            t2 = NULL;
        }
        if (t3) {
            markdown_core_node_free(t3);
            t3 = NULL;
        }
        if (!failures) {
            markdown_core_parser_feed(parser, CR_CLOSER, sizeof(CR_CLOSER) - 1);
            t3 = markdown_core_parser_derive_tree(parser, parser->refmap);
            t4 = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!t3 || !t4) {
                fputs("container retention: deep-target closing derivations failed\n", stderr);
                failures++;
            } else {
                markdown_core_node *closed_list = t4->children.vec[0];
                const markdown_core_node *first_item = markdown_core_node_children(closed_list).child;
                if (t3->children.vec[0] != closed_list || !(closed_list->flags & MARKDOWN_CORE_NODE__SHARED) ||
                    closed_list->children.count != 3 || !first_item ||
                    markdown_core_node_children(first_item).child != NULL) {
                    fputs("container retention: the deep-edited list did not serve by identity\n", stderr);
                    failures++;
                }
            }
            if (t3) {
                markdown_core_node_free(t3);
                t3 = NULL;
            }
            if (t4) {
                markdown_core_node_free(t4);
            }
        }
        markdown_core_parser_free(parser);
        parser = NULL;
    }

    printf(
        "container retention: %s\n",
        failures ? "a closed container is not one value" : "a closed container serves whole, by identity"
    );
    return failures ? -1 : 0;
}

/* THE MAP-IMMUNITY REFINEMENT (#163): a map's generation takes part in the
 * cache key only for a block whose stored projection had something to ask
 * that map. Both halves are asserted: a definition arriving must still
 * re-derive the block that held the reference -- the tree resolves, which is
 * the correctness half and holds on any engine -- and it must NOT re-key the
 * prose block beside it, asserted on `parser->cache_hits` advancing across
 * the arrival, which is the half only the refinement can pass. A third block
 * hides its reference inside an inline directive's label (review-found): the
 * nested parse records candidacy on the label node, so the owning block must
 * inherit it, or the block stays immune and serves the unresolved tree
 * forever. Skipped under --no-cache, where there is nothing to hit. */
static int case_map_immunity(const ts_spec_file *file) {
    static const char PROSE[] = "plain prose paragraph\n\n";
    static const char REF[] = "see [x] here\n\n";
    static const char LABELED[] = "also :note[with [x] inside]\n\n";
    static const char DEF[] = "[x]: /url\n\n";
    markdown_core_parser *parser;
    markdown_core_node *first = NULL;
    markdown_core_node *second = NULL;
    markdown_core_node *third = NULL;
    size_t hits_before_second;
    size_t hits_before_third;
    size_t resolved = 0;
    int failures = 0;
    (void)file;

    if (pr_no_cache) {
        printf("map immunity: skipped under --no-cache\n");
        return 0;
    }
    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    markdown_core_parser_feed(parser, PROSE, sizeof(PROSE) - 1);
    markdown_core_parser_feed(parser, REF, sizeof(REF) - 1);
    markdown_core_parser_feed(parser, LABELED, sizeof(LABELED) - 1);
    first = markdown_core_parser_derive_tree(parser, parser->refmap);
    markdown_core_parser_feed(parser, DEF, sizeof(DEF) - 1);
    hits_before_second = parser->cache_hits;
    second = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!first || !second) {
        fputs("map immunity: derivation failed\n", stderr);
        failures++;
    } else {
        markdown_core_iter walk;
        markdown_core_event_type ev_type;
        markdown_core_iter_init(&walk, second);
        while ((ev_type = markdown_core_iter_next(&walk)) != MARKDOWN_CORE_EVENT_DONE) {
            if (ev_type == MARKDOWN_CORE_EVENT_ENTER &&
                markdown_core_iter_get_node(&walk)->type == MARKDOWN_CORE_NODE_LINK_REFERENCE) {
                resolved++;
            }
        }
        if (resolved < 2) {
            fprintf(
                stderr,
                "map immunity: the arriving definition resolved %zu of 2 references -- %s\n",
                resolved,
                resolved ? "the directive label's block stayed immune" : "no block re-derived"
            );
            failures++;
        }
        if (parser->cache_hits <= hits_before_second) {
            fprintf(
                stderr,
                "map immunity: the definition re-keyed the prose block (%zu hits, %zu misses)\n",
                parser->cache_hits,
                parser->cache_misses
            );
            failures++;
        }
    }
    hits_before_third = parser->cache_hits;
    third = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!failures && (!third || parser->cache_hits < hits_before_third + 3)) {
        fputs("map immunity: an unwritten CST did not serve its closed blocks\n", stderr);
        failures++;
    }
    markdown_core_node_free(first);
    markdown_core_node_free(second);
    markdown_core_node_free(third);
    markdown_core_parser_free(parser);
    printf("map immunity: %s\n", failures ? "the key is still global" : "only the asking block re-keys");
    return failures ? -1 : 0;
}

/* THE LABEL'S TAIL (landing review, F18): a directive's CST-resident label is
 * inline-class, so the walk never queues it, and its list silently missed
 * every content pass -- an unmatched `*` stayed three TEXT nodes and a
 * `www.` never became a link, where the whole-tree tail gave both their
 * passes. The owning block's tail now runs them; this gate pins the shape on
 * the finish path, the derive path, and a re-derivation (the label is not
 * cached, so its passes run on every projection and must be idempotent). */
static int lt_label_shape(markdown_core_node *root, const char *which) {
    markdown_core_iter walk;
    markdown_core_iter *iter = &walk;
    markdown_core_event_type ev_type;
    markdown_core_node *label = NULL;
    markdown_core_iter_init(iter, root);
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER && node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
            label = node;
            break;
        }
    }
    if (!label) {
        fprintf(stderr, "label tail (%s): no DirectiveLabel in the tree\n", which);
        return -1;
    }
    if (!label->first_child || label->first_child->type != MARKDOWN_CORE_NODE_TEXT || !label->first_child->next ||
        label->first_child->next->type != MARKDOWN_CORE_NODE_LINK || label->first_child->next->next) {
        fprintf(
            stderr,
            "label tail (%s): the label is not [consolidated text, autolink] -- its passes did not run\n",
            which
        );
        return -1;
    }
    return 0;
}

static int case_label_tail(const ts_spec_file *file) {
    static const char LT_TEXT[] = "::note[a *cat sees www.example.com]{k=v}\n\nafter\n";
    markdown_core_parser *parser;
    markdown_core_node *finished;
    markdown_core_node *first;
    markdown_core_node *second;
    int failures = 0;
    (void)file;

    finished = pr_parse(LT_TEXT, sizeof(LT_TEXT) - 1);
    if (!finished) {
        fputs("label tail: parse failed\n", stderr);
        return -1;
    }
    failures += lt_label_shape(finished, "finish") != 0;
    markdown_core_node_free(finished);

    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    markdown_core_parser_feed(parser, LT_TEXT, sizeof(LT_TEXT) - 1);
    first = markdown_core_parser_derive_tree(parser, parser->refmap);
    second = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!first || !second) {
        fputs("label tail: derivation failed\n", stderr);
        failures++;
    } else {
        failures += lt_label_shape(first, "derive") != 0;
        failures += lt_label_shape(second, "re-derive") != 0;
    }
    if (first) {
        markdown_core_node_free(first);
    }
    if (second) {
        markdown_core_node_free(second);
    }
    markdown_core_parser_free(parser);
    printf("label tail: %s\n", failures ? "the label missed its passes" : "the label gets every pass");
    return failures ? -1 : 0;
}

/* T15 -- THE REACTIVE-LOOP BOUND, as counters rather than clocks. The
 * projection side of a feed carries no term in the document already fed:
 * `cache_misses` counts exactly the content blocks a projection re-parses
 * (the cost F12 measured), so over a stream of independent blocks the
 * per-feed miss delta must sit FLAT while the hit delta grows with the
 * document -- a miss delta that moves with fed size names state being
 * re-derived. Two terms are carved out and STATED rather than folded in
 * (§6): the whole-CST clone every derivation pays by design, and the
 * binding-side copy-out, which lives above this runner. The third term --
 * a definition's arrival re-keys the whole document (F19) -- is asserted
 * AS that term, so a change in its shape fails here instead of hiding. */
static int case_feed_bound(const ts_spec_file *file) {
    enum { FB_BLOCKS = 256, FB_WARMUP = 16 };
    markdown_core_parser *parser;
    size_t misses_before;
    size_t hits_before;
    size_t min_misses = (size_t)-1;
    size_t max_misses = 0;
    size_t first_hits_delta = 0;
    size_t last_hits_delta = 0;
    char line[64];
    int i;
    int failures = 0;
    (void)file;

    if (pr_no_cache) {
        puts("feed bound: skipped under --no-cache (the bound is the cache's)");
        return 0;
    }
    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    for (i = 0; i < FB_BLOCKS; i++) {
        markdown_core_node *derived;
        snprintf(line, sizeof(line), "paragraph number %d with some text\n\n", i);
        markdown_core_parser_feed(parser, line, strlen(line));
        misses_before = parser->cache_misses;
        hits_before = parser->cache_hits;
        derived = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!derived) {
            fputs("feed bound: derivation failed\n", stderr);
            markdown_core_parser_free(parser);
            return -1;
        }
        markdown_core_node_free(derived);
        if (i >= FB_WARMUP) {
            size_t misses = parser->cache_misses - misses_before;
            if (misses < min_misses) {
                min_misses = misses;
            }
            if (misses > max_misses) {
                max_misses = misses;
            }
            if (i == FB_WARMUP) {
                first_hits_delta = parser->cache_hits - hits_before;
            }
            last_hits_delta = parser->cache_hits - hits_before;
        }
    }
    if (max_misses != min_misses) {
        fprintf(
            stderr,
            "feed bound: misses per feed moved %zu..%zu -- the projection side carries a term in the fed document\n",
            min_misses,
            max_misses
        );
        failures++;
    }
    if (last_hits_delta <= first_hits_delta) {
        fprintf(
            stderr,
            "feed bound: hits per feed did not grow (%zu -> %zu) -- the cache is not serving\n",
            first_hits_delta,
            last_hits_delta
        );
        failures++;
    }
    {
        static const char DEF[] = "[zz]: /u\n\n";
        markdown_core_node *derived;
        size_t misses;
        markdown_core_parser_feed(parser, DEF, sizeof(DEF) - 1);
        misses_before = parser->cache_misses;
        derived = markdown_core_parser_derive_tree(parser, parser->refmap);
        if (!derived) {
            fputs("feed bound: derivation failed\n", stderr);
            markdown_core_parser_free(parser);
            return -1;
        }
        markdown_core_node_free(derived);
        misses = parser->cache_misses - misses_before;
        /* #163 closed F19's whole-document term: none of the 256 prose
         * blocks consulted a map, so the arrival may re-key none of them.
         * The other direction -- a block that DID ask re-keys -- is
         * `map_immunity`'s to pin. */
        if (misses >= FB_BLOCKS) {
            fprintf(
                stderr,
                "feed bound: a definition's arrival re-keyed %zu of %d blocks that consulted no map (#163)\n",
                misses,
                FB_BLOCKS
            );
            failures++;
        }
    }
    markdown_core_parser_free(parser);
    /* THE SAME BOUND ON THE SHAPE THE MEMO CANNOT SERVE (F26, review-asked):
     * one tight list fed an item per line keeps the document's only child
     * OPEN for the whole stream, so the stable-prefix memo records nothing
     * and every derivation still walks the accumulated skeleton -- the
     * carved-out clone term stated above, and #161's still-open remainder
     * for container prefixes. What must hold on this shape TODAY is T15's
     * bound on the projection side: an item already fed is never re-parsed,
     * so misses per feed sit flat -- the open spine's constant, not a term
     * in the list -- while hits grow with it. */
    {
        enum { NB_ITEMS = 128, NB_WARMUP = 16 };
        size_t nested_min = (size_t)-1;
        size_t nested_max = 0;
        size_t nested_first_hits = 0;
        size_t nested_last_hits = 0;
        parser = pr_parser_new();
        if (!parser) {
            return -1;
        }
        for (i = 0; i < NB_ITEMS; i++) {
            markdown_core_node *derived;
            snprintf(line, sizeof(line), "- item %d with some text\n", i);
            markdown_core_parser_feed(parser, line, strlen(line));
            misses_before = parser->cache_misses;
            hits_before = parser->cache_hits;
            derived = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!derived) {
                fputs("feed bound: nested derivation failed\n", stderr);
                markdown_core_parser_free(parser);
                return -1;
            }
            markdown_core_node_free(derived);
            if (i >= NB_WARMUP) {
                size_t misses = parser->cache_misses - misses_before;
                if (misses < nested_min) {
                    nested_min = misses;
                }
                if (misses > nested_max) {
                    nested_max = misses;
                }
                if (i == NB_WARMUP) {
                    nested_first_hits = parser->cache_hits - hits_before;
                }
                nested_last_hits = parser->cache_hits - hits_before;
            }
        }
        markdown_core_parser_free(parser);
        if (nested_max != nested_min) {
            fprintf(
                stderr,
                "feed bound: nested misses per feed moved %zu..%zu -- an open container's fed items are being "
                "re-parsed\n",
                nested_min,
                nested_max
            );
            failures++;
        }
        if (nested_last_hits <= nested_first_hits) {
            fprintf(
                stderr,
                "feed bound: nested hits per feed did not grow (%zu -> %zu) -- the cache is not serving under an "
                "open container\n",
                nested_first_hits,
                nested_last_hits
            );
            failures++;
        }
        printf(
            "feed bound: nested holds -- misses/feed flat at %zu under one open list over %d feeds, hits/feed %zu "
            "-> %zu\n",
            nested_min,
            NB_ITEMS - NB_WARMUP,
            nested_first_hits,
            nested_last_hits
        );
    }
    printf(
        "feed bound: %s -- misses/feed flat at %zu over %d feeds, hits/feed %zu -> %zu; carved out and accepted "
        "(§6): the whole-CST clone per feed and the binding copy-out\n",
        failures ? "MOVED" : "holds",
        min_misses,
        FB_BLOCKS - FB_WARMUP,
        first_hits_delta,
        last_hits_delta
    );
    return failures ? -1 : 0;
}

/* T16 -- RESIDENT MEMORY across a long stream, measured rather than assumed,
 * and the bound stated so it is not later mistaken for a defect (§6):
 * every block keeps its content bytes for life, the parser keeps the
 * normalized source, and the cache keeps one projected list per closed
 * block, so residency is O(bytes fed) with a constant factor -- THE GATE
 * IS THE FACTOR. `ru_maxrss` is a peak: the tree handed back per feed is
 * freed before the next, so the peak is the resident state plus one live
 * projection, which is the consumer's own shape. Closes F9. */
static int case_resident_memory(const ts_spec_file *file) {
#if defined(_WIN32)
    (void)file;
    puts("resident memory: skipped (no getrusage on this host)");
    return 0;
#else
    enum { RM_BLOCKS = 16384, RM_DERIVE_EVERY = 64 };
    static const char RM_FILLER[] = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do "
                                    "eiusmod tempor incididunt ut labore et dolore magna aliqua ut "
                                    "enim ad minim veniam quis nostrud exercitation ullamco laboris";
    struct rusage usage;
    long baseline_kb;
    long final_kb;
    size_t fed = 0;
    char line[512];
    int i;
    markdown_core_parser *parser;
    double factor;
    (void)file;

    getrusage(RUSAGE_SELF, &usage);
    baseline_kb = usage.ru_maxrss;
    parser = pr_parser_new();
    if (!parser) {
        return -1;
    }
    for (i = 0; i < RM_BLOCKS; i++) {
        int n = snprintf(line, sizeof(line), "block %d: %s\n\n", i, RM_FILLER);
        markdown_core_parser_feed(parser, line, (size_t)n);
        fed += (size_t)n;
        if (i % RM_DERIVE_EVERY == RM_DERIVE_EVERY - 1) {
            markdown_core_node *derived = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!derived) {
                fputs("resident memory: derivation failed\n", stderr);
                markdown_core_parser_free(parser);
                return -1;
            }
            markdown_core_node_free(derived);
        }
    }
    getrusage(RUSAGE_SELF, &usage);
    final_kb = usage.ru_maxrss;
    markdown_core_parser_free(parser);
#if defined(__APPLE__)
    /* ru_maxrss is bytes on Darwin and kilobytes on Linux. */
    baseline_kb /= 1024;
    final_kb /= 1024;
#endif
    factor = ((double)(final_kb - baseline_kb) * 1024.0) / (double)fed;
    printf(
        "resident memory: %.2fx of the %zu bytes fed stays resident (%ld KiB -> %ld KiB peak); the bound is "
        "O(bytes fed) -- the CST keeps every block's content, the parser the normalized source, the cache one "
        "list per closed block\n",
        factor,
        fed,
        baseline_kb,
        final_kb
    );
    if (factor > 24.0) {
        fprintf(stderr, "resident memory: %.2fx exceeds the 24x tripwire\n", factor);
        return -1;
    }
    return 0;
#endif
}

/* T17 -- CARRIED EXTENSION STATE, gated structurally instead of through a
 * golden (closes F8). The boundary derivation goes through the clone; the
 * finish projection is taken in place on the CST with no clone at all --
 * so a field of a block's extension payload that stops surviving the
 * clone shows up as the SAME closed block dumping differently at the last
 * boundary than at finish, joined on the identity T2 mints. Run twice:
 * cache off (a pure clone-projection against the in-place one) and cache
 * on (the served-list path against the same). A document whose FINALIZE
 * mints definitions -- a dying paragraph's, a footnote's -- is skipped:
 * those move the maps between the two projections, resolution may then
 * legitimately move a closed block's rendering, and the generations are
 * unreadable afterwards because finish's reset renews the maps. The
 * finalize-minted definitions are counted STRUCTURALLY, as definition
 * nodes the finish tree has that the boundary tree does not. The skip
 * count is asserted a minority so the gate cannot go vacuous.
 *
 * One block is exempt BY THE RULING: the boundary tree's final top-level
 * block. The formula promotion's `closed` lags one line deliberately
 * (§1's backward-reach table), so the block at the spine's very end can
 * read closed while its reinterpretation is still owed to the next line
 * -- at finish that line has spoken. Every block the stream has actually
 * left behind is compared. */
typedef struct cs_entry {
    uint32_t id;
    uint8_t *dump;
    size_t length;
} cs_entry;

static int cs_collect(markdown_core_node *root, int exempt_tail, cs_entry **out, size_t *count) {
    markdown_core_iter walk;
    markdown_core_iter *iter = &walk;
    markdown_core_event_type ev_type;
    markdown_core_node *tail = exempt_tail ? markdown_core_child_back(root) : NULL;
    size_t cap = 0;
    *out = NULL;
    *count = 0;
    markdown_core_iter_init(iter, root);
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER && tail && node == tail) {
            markdown_core_iter_skip_children(iter);
            continue;
        }
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER || !MARKDOWN_CORE_NODE_BLOCK_P(node) ||
            (node->flags & MARKDOWN_CORE_NODE__OPEN) || node->identifier == 0) {
            continue;
        }
        if (*count == cap) {
            size_t grown = cap ? cap * 2 : 32;
            cs_entry *entries = (cs_entry *)realloc(*out, grown * sizeof(cs_entry));
            if (!entries) {
                return -1;
            }
            *out = entries;
            cap = grown;
        }
        (*out)[*count].id = node->identifier;
        (*out)[*count].dump = pr_dump(node, &(*out)[*count].length);
        if (!(*out)[*count].dump) {
            return -1;
        }
        (*count)++;
    }
    return 0;
}

static void cs_free(cs_entry *entries, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        markdown_core_dump_free(entries[i].dump);
    }
    free(entries);
}

static size_t cs_definition_count(markdown_core_node *root) {
    markdown_core_iter walk;
    markdown_core_iter *iter = &walk;
    markdown_core_event_type ev_type;
    size_t count = 0;
    markdown_core_iter_init(iter, root);
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER && (node->type == MARKDOWN_CORE_NODE_REFERENCE_DEFINITION ||
                                                        node->type == MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION)) {
            count++;
        }
    }
    return count;
}

static int case_carried_state(const ts_spec_file *file) {
    size_t index;
    int cache_off;
    int failures = 0;
    size_t compared = 0;
    size_t skipped = 0;
    size_t documents = 0;
    size_t multi_block = 0;

    for (cache_off = 0; cache_off <= 1; cache_off++) {
        for (index = 0; index < file->count; index++) {
            const ts_spec_case *test_case = &file->cases[index];
            markdown_core_parser *parser = pr_parser_new();
            markdown_core_node *derived;
            markdown_core_node *finished;
            cs_entry *at_boundary = NULL;
            cs_entry *at_finish = NULL;
            size_t boundary_count = 0;
            size_t finish_count = 0;
            size_t boundary_definitions;
            size_t i;
            size_t j;
            if (!parser) {
                return -1;
            }
            parser->no_projection_cache = cache_off || pr_no_cache;
            documents++;
            markdown_core_parser_feed(parser, test_case->markdown, test_case->markdown_length);
            derived = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!derived || cs_collect(derived, 1, &at_boundary, &boundary_count) != 0) {
                fprintf(stderr, "example %d: boundary derivation failed\n", test_case->example);
                cs_free(at_boundary, boundary_count);
                if (derived) {
                    markdown_core_node_free(derived);
                }
                markdown_core_parser_free(parser);
                return -1;
            }
            boundary_definitions = cs_definition_count(derived);
            if (markdown_core_node_child_count(derived) > 1) {
                multi_block++;
            }
            markdown_core_node_free(derived);
            finished = markdown_core_parser_finish(parser);
            if (!finished || cs_collect(finished, 0, &at_finish, &finish_count) != 0) {
                fprintf(stderr, "example %d: finish failed\n", test_case->example);
                cs_free(at_boundary, boundary_count);
                cs_free(at_finish, finish_count);
                if (finished) {
                    markdown_core_node_free(finished);
                }
                markdown_core_parser_free(parser);
                return -1;
            }
            if (cs_definition_count(finished) != boundary_definitions) {
                skipped++;
            } else {
                for (i = 0; i < boundary_count; i++) {
                    int found = 0;
                    for (j = 0; j < finish_count; j++) {
                        if (at_finish[j].id != at_boundary[i].id) {
                            continue;
                        }
                        found = 1;
                        if (at_finish[j].length != at_boundary[i].length ||
                            memcmp(at_finish[j].dump, at_boundary[i].dump, at_boundary[i].length) != 0) {
                            fprintf(
                                stderr,
                                "example %d (cache %s): closed block %u dumps differently at finish\n",
                                test_case->example,
                                cache_off ? "off" : "on",
                                at_boundary[i].id
                            );
                            failures++;
                        }
                        break;
                    }
                    if (!found) {
                        fprintf(
                            stderr,
                            "example %d (cache %s): closed block %u vanished at finish\n",
                            test_case->example,
                            cache_off ? "off" : "on",
                            at_boundary[i].id
                        );
                        failures++;
                    }
                    compared++;
                }
            }
            cs_free(at_boundary, boundary_count);
            cs_free(at_finish, finish_count);
            markdown_core_node_free(finished);
            markdown_core_parser_free(parser);
        }
    }
    /* Vacuity is judged structurally: a corpus of one-block documents has
     * nothing but spine tails, and the tail is the one ruled exemption. */
    if ((compared == 0 && multi_block > 0) || skipped * 2 > documents) {
        fprintf(
            stderr,
            "carried state: %zu blocks compared over %zu multi-block documents, %zu of %zu skipped -- vacuous\n",
            compared,
            multi_block,
            skipped,
            documents
        );
        failures++;
    }
    printf(
        "carried state: %s -- %zu closed blocks agree between the cloned boundary projection and the in-place "
        "finish, %zu resolution-moved documents skipped\n",
        failures ? "DIVERGED" : "holds",
        compared,
        skipped
    );
    return failures ? -1 : 0;
}

/* One session on this thread, its documents freed on another: the #153
 * threading contract. The producer feeds line by line and derives after
 * every line -- holders store and hit, frozen buffers are retained across
 * trees, a late definition re-stamps the maps and forces re-stores -- and
 * hands each derived tree to a consumer through a small bounded queue. The
 * consumer walks every text literal (reading the shared frozen bytes) and
 * frees the tree, concurrently with the producer's next feeds and with the
 * session's own death. The TSan lane is the judge; the byte sum only keeps
 * the reads alive. Raw native threads on purpose, as concurrency_runner's
 * comment explains for the facade cases. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
typedef HANDLE pr_thread;
typedef CRITICAL_SECTION pr_mutex;
typedef CONDITION_VARIABLE pr_cond;
#define PR_THREAD_RETURN unsigned __stdcall
#define PR_THREAD_RESULT 0u
static int pr_thread_spawn(pr_thread *handle, unsigned(__stdcall *entry)(void *), void *argument) {
    uintptr_t raw = _beginthreadex(NULL, 0, entry, argument, 0, NULL);
    if (!raw) {
        return 1;
    }
    *handle = (HANDLE)raw;
    return 0;
}
static void pr_thread_join(pr_thread handle) {
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
}
static void pr_mutex_init(pr_mutex *m) { InitializeCriticalSection(m); }
static void pr_cond_init(pr_cond *c) { InitializeConditionVariable(c); }
static void pr_lock(pr_mutex *m) { EnterCriticalSection(m); }
static void pr_unlock(pr_mutex *m) { LeaveCriticalSection(m); }
static void pr_signal(pr_cond *c) { WakeAllConditionVariable(c); }
static void pr_wait(pr_cond *c, pr_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
#else
#include <pthread.h>
typedef pthread_t pr_thread;
typedef pthread_mutex_t pr_mutex;
typedef pthread_cond_t pr_cond;
#define PR_THREAD_RETURN void *
#define PR_THREAD_RESULT NULL
static int pr_thread_spawn(pr_thread *handle, void *(*entry)(void *), void *argument) {
    return pthread_create(handle, NULL, entry, argument) != 0;
}
static void pr_thread_join(pr_thread handle) { pthread_join(handle, NULL); }
static void pr_mutex_init(pr_mutex *m) { pthread_mutex_init(m, NULL); }
static void pr_cond_init(pr_cond *c) { pthread_cond_init(c, NULL); }
static void pr_lock(pr_mutex *m) { pthread_mutex_lock(m); }
static void pr_unlock(pr_mutex *m) { pthread_mutex_unlock(m); }
static void pr_signal(pr_cond *c) { pthread_cond_broadcast(c); }
static void pr_wait(pr_cond *c, pr_mutex *m) { pthread_cond_wait(c, m); }
#endif

enum { SDQ_CAPACITY = 4, SDQ_ROUNDS = 6 };

typedef struct {
    pr_mutex lock;
    pr_cond changed;
    markdown_core_node *slots[SDQ_CAPACITY];
    int head, count;
    int done;
    unsigned long long consumed;
} sd_queue;

static void sd_push(sd_queue *q, markdown_core_node *tree) {
    pr_lock(&q->lock);
    while (q->count == SDQ_CAPACITY) {
        pr_wait(&q->changed, &q->lock);
    }
    q->slots[(q->head + q->count) % SDQ_CAPACITY] = tree;
    q->count++;
    pr_signal(&q->changed);
    pr_unlock(&q->lock);
}

static PR_THREAD_RETURN sd_consumer(void *argument) {
    sd_queue *q = (sd_queue *)argument;
    for (;;) {
        markdown_core_node *tree;
        markdown_core_iter walk;
        markdown_core_event_type ev;
        unsigned long long sum = 0;
        pr_lock(&q->lock);
        while (q->count == 0 && !q->done) {
            pr_wait(&q->changed, &q->lock);
        }
        if (q->count == 0) {
            pr_unlock(&q->lock);
            break;
        }
        tree = q->slots[q->head];
        q->head = (q->head + 1) % SDQ_CAPACITY;
        q->count--;
        pr_signal(&q->changed);
        pr_unlock(&q->lock);

        markdown_core_iter_init(&walk, tree);
        while ((ev = markdown_core_iter_next(&walk)) != MARKDOWN_CORE_EVENT_DONE) {
            markdown_core_node *cur = markdown_core_iter_get_node(&walk);
            if (ev == MARKDOWN_CORE_EVENT_ENTER && cur->type == MARKDOWN_CORE_NODE_TEXT && cur->as.literal.len > 0) {
                sum += (unsigned long long)cur->as.literal.data[0] + (unsigned long long)cur->as.literal.len;
            }
        }
        markdown_core_node_free(tree);
        pr_lock(&q->lock);
        q->consumed += sum;
        pr_unlock(&q->lock);
    }
    return PR_THREAD_RESULT;
}

static int case_session_documents(const ts_spec_file *file) {
    static const char *const SD_LINES[] = {
        "# Shared *heading* with `code`\n",
        "\n",
        "A paragraph with [ref][label] and a footnote[^fn] plus **strong**.\n",
        "\n",
        "Another paragraph, plain but long enough to carry several words.\n",
        "\n",
        "[label]: /url \"title\"\n",
        "\n",
        "[^fn]: the footnote body arrives late.\n",
        "\n",
    };
    enum { SD_LINE_COUNT = sizeof(SD_LINES) / sizeof(SD_LINES[0]) };
    sd_queue queue;
    pr_thread consumer;
    int failures = 0;
    int round;
    (void)file;

    memset(&queue, 0, sizeof(queue));
    pr_mutex_init(&queue.lock);
    pr_cond_init(&queue.changed);
    if (pr_thread_spawn(&consumer, sd_consumer, &queue)) {
        fputs("session_documents: could not spawn the consumer\n", stderr);
        return -1;
    }

    for (round = 0; round < SDQ_ROUNDS && !failures; round++) {
        markdown_core_parser *parser = pr_parser_new();
        markdown_core_node *tree;
        size_t i;
        if (!parser) {
            failures = 1;
            break;
        }
        for (i = 0; i < SD_LINE_COUNT; i++) {
            markdown_core_parser_feed(parser, SD_LINES[i], strlen(SD_LINES[i]));
            tree = markdown_core_parser_derive_tree(parser, parser->refmap);
            if (!tree) {
                failures = 1;
                break;
            }
            sd_push(&queue, tree);
        }
        tree = markdown_core_parser_finish(parser);
        if (tree) {
            sd_push(&queue, tree);
        } else {
            failures = 1;
        }
        /* The session dies while the consumer may still hold its documents:
         * this free racing those frees is the contract under test. */
        markdown_core_parser_free(parser);
    }

    pr_lock(&queue.lock);
    queue.done = 1;
    pr_signal(&queue.changed);
    pr_unlock(&queue.lock);
    pr_thread_join(consumer);
    if (queue.consumed == 0) {
        fputs("session_documents: the consumer read nothing\n", stderr);
        failures = 1;
    }
    return failures ? -1 : 0;
}

typedef struct pr_case_entry {
    const char *name;
    int (*run)(const ts_spec_file *file);
    int needs_spec;
} pr_case_entry;

static const pr_case_entry PR_CASES[] = {
    {"closed_after_finish", case_closed_after_finish, 1},
    {"double_projection", case_double_projection, 1},
    {"refmap_independence", case_refmap_independence, 1},
    {"borrow_across_feed", case_borrow_across_feed, 1},
    {"projection_key", case_projection_key, 1},
    {"block_identity", case_block_identity, 1},
    {"block_identity_transitions", case_block_identity_transitions, 0},
    {"attach_invalidation", case_attach_invalidation, 0},
    {"map_immunity", case_map_immunity, 0},
    {"node_sharing", case_node_sharing, 0},
    {"hook_once", case_hook_once, 0},
    {"child_memo", case_child_memo, 0},
    {"container_retention", case_container_retention, 0},
    {"label_tail", case_label_tail, 0},
    {"feed_bound", case_feed_bound, 0},
    {"resident_memory", case_resident_memory, 0},
    {"carried_state", case_carried_state, 1},
    {"dump_boundaries", case_dump_boundaries, 1},
    {"feed_loop", case_feed_loop, 1},
    {"session_documents", case_session_documents, 0},
    {"projection_slope", case_projection_slope, 0},
};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    const char *spec = NULL;
    const char *md = NULL;
    const char *md_dir = NULL;
    ts_spec_file file;
    size_t i;
    int arg;

    for (arg = 1; arg < argc; arg++) {
        if (strcmp(argv[arg], "--list") == 0) {
            for (i = 0; i < sizeof(PR_CASES) / sizeof(PR_CASES[0]); i++) {
                puts(PR_CASES[i].name);
            }
            return 0;
        } else if (strcmp(argv[arg], "--case") == 0 && arg + 1 < argc) {
            case_name = argv[++arg];
        } else if (strcmp(argv[arg], "--spec") == 0 && arg + 1 < argc) {
            spec = argv[++arg];
        } else if (strcmp(argv[arg], "--md") == 0 && arg + 1 < argc) {
            md = argv[++arg];
        } else if (strcmp(argv[arg], "--md-dir") == 0 && arg + 1 < argc) {
            md_dir = argv[++arg];
        } else if (strcmp(argv[arg], "--repeats") == 0 && arg + 1 < argc) {
            pr_repeats = atoi(argv[++arg]);
        } else if (strcmp(argv[arg], "--no-cache") == 0) {
            pr_no_cache = 1;
        } else {
            fputs("usage: projection_runner [--list] --case NAME (--spec FILE | --md FILE | --md-dir DIR)\n", stderr);
            return 2;
        }
    }
    if (!case_name || pr_repeats < 1) {
        fputs("usage: projection_runner [--list] --case NAME (--spec FILE | --md FILE | --md-dir DIR)\n", stderr);
        return 2;
    }
    for (i = 0; i < sizeof(PR_CASES) / sizeof(PR_CASES[0]); i++) {
        if (strcmp(PR_CASES[i].name, case_name) == 0) {
            int failed;
            if (PR_CASES[i].needs_spec) {
                if (spec) {
                    if (ts_spec_load(spec, &file) != 0) {
                        fprintf(stderr, "projection_runner: cannot load %s\n", spec);
                        return 2;
                    }
                } else if (md) {
                    file.cases = (ts_spec_case *)calloc(1, sizeof(ts_spec_case));
                    file.count = 1;
                    if (!file.cases || pr_case_from_file(md, 1, &file.cases[0]) != 0) {
                        fprintf(stderr, "projection_runner: cannot load %s\n", md);
                        return 2;
                    }
                } else if (md_dir) {
                    if (pr_load_md_dir(md_dir, &file) != 0) {
                        fprintf(stderr, "projection_runner: cannot load %s\n", md_dir);
                        return 2;
                    }
                } else {
                    fprintf(stderr, "case %s requires --spec FILE, --md FILE or --md-dir DIR\n", case_name);
                    return 2;
                }
                failed = PR_CASES[i].run(&file) != 0;
                ts_spec_free(&file);
            } else {
                failed = PR_CASES[i].run(NULL) != 0;
            }
            printf("%s %s\n", case_name, failed ? "[FAILED]" : "[PASSED]");
            return failed;
        }
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
