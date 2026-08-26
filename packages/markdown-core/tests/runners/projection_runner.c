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
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

#include <markdown_core.h>

#include "markdown-core.h"
#include "markdown-core-extensions.h"

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
        first = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
        if (first) {
            first_dump = pr_dump(first, &first_length);
        }
        second = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
        first = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
        other = markdown_core_parser_derive_tree(parser, empty_map, 0);
        again = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
        if (!example_failed && pr_fingerprint(parser, &after) != 0) {
            example_failed = 1;
        }
        if (first) {
            first_dump = pr_dump(first, &first_length);
        }
        if (again) {
            again_dump = pr_dump(again, &again_length);
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
            markdown_core_node *derived = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
    for (i = 0; i < generation->count; i++) {
        markdown_core_dump_free(generation->subtrees[i]);
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
    markdown_core_node *child;
    if (!MARKDOWN_CORE_NODE_BLOCK_P(node)) {
        return 0;
    }
    for (child = node->first_child; child; child = child->next) {
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
    cur.tree = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
            if (!markdown_core_holder_take_children(cur.holders[i], leaf)) {
                markdown_core_holder_release(cur.holders[i]);
                failed = 1;
                break;
            }
            markdown_core_holder_hold(cur.holders[i]);
        }
        markdown_core_node_borrow_children(leaf, cur.holders[i]);
    }
    free(leaves);
    if (failed) {
        fprintf(stderr, "example %d boundary %d: could not build the borrow\n", example, boundary);
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
        tree = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
 * printed so two runs can be seen to have done the same work. */
static int pr_repeats = 1;

static int case_feed_loop(const ts_spec_file *file) {
    size_t index;
    int repeat;
    uint64_t best_ns = 0;
    size_t boundaries = 0;
    size_t hits = 0, misses = 0;
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
                tree = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
                if (tree) {
                    markdown_core_node_free(tree);
                }
            }
            /* Read before `finish`, which resets the parser and its ledger;
             * the finish projection itself is therefore not counted. */
            if (repeat == 0) {
                hits += parser->cache_hits;
                misses += parser->cache_misses;
            }
            tree = markdown_core_parser_finish(parser);
            if (tree) {
                markdown_core_node_free(tree);
            }
            markdown_core_parser_free(parser);
        }
        elapsed = ts_monotonic_ns() - started;
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
    return 0;
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
    out->section = strdup(path);
    out->example = example;
    return 0;
}

static int pr_name_compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int pr_load_md_dir(const char *dir, ts_spec_file *out) {
    DIR *d = opendir(dir);
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0, cap = 0, i;
    if (!d) {
        return -1;
    }
    while ((entry = readdir(d)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n < 4 || strcmp(entry->d_name + n - 3, ".md") != 0) {
            continue;
        }
        if (count == cap) {
            char **grown;
            cap = cap ? cap * 2 : 64;
            grown = (char **)realloc(names, cap * sizeof(*names));
            if (!grown) {
                closedir(d);
                return -1;
            }
            names = grown;
        }
        names[count++] = strdup(entry->d_name);
    }
    closedir(d);
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
    markdown_core_node *cur = root;
    while (cur) {
        if (n == cap) {
            markdown_core_node **grown;
            cap = cap ? cap * 2 : 32;
            grown = (markdown_core_node **)realloc(items, cap * sizeof(*items));
            if (!grown) {
                free(items);
                return -1;
            }
            items = grown;
        }
        items[n++] = cur;
        if (cur->first_child && MARKDOWN_CORE_NODE_BLOCK_P(cur->first_child)) {
            cur = cur->first_child;
            continue;
        }
        while (cur != root && cur->next == NULL) {
            cur = cur->parent;
        }
        cur = (cur == root) ? NULL : cur->next;
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
    markdown_core_node *tree = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
    markdown_core_node **blocks = NULL, **again = NULL;
    markdown_core_node **all = NULL, **all_again = NULL;
    size_t count = 0, again_count = 0, all_count = 0, all_again_count = 0, i, j;
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
        for (first_sibling = all[i]->first_child; first_sibling && !failed; first_sibling = first_sibling->next) {
            for (later_sibling = first_sibling->next; later_sibling && !failed; later_sibling = later_sibling->next) {
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
     * projection to take. */
    if (!failed && parser) {
        second = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
    free(again);
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
            tree = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
    markdown_core_node *child;
    out->count = 0;
    for (child = root->first_child; child; child = child->next) {
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
        tree = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
            tree = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
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
    {"dump_boundaries", case_dump_boundaries, 1},
    {"feed_loop", case_feed_loop, 1},
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
