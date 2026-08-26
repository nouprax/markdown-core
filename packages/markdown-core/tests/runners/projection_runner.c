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
 */
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
static markdown_core_parser *pr_parser_new(void) {
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES);
    if (!parser) {
        return NULL;
    }
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
 * flags, content bytes and the content-to-source run. If a derivation writes
 * any of it, two fingerprints taken around the derivation differ. */
static int pr_fingerprint(markdown_core_node *root, markdown_core_strbuf *out) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;
    if (!iter) {
        return -1;
    }
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        char header[160];
        snprintf(
            header,
            sizeof(header),
            "%u|%u|%d:%d..%d:%d|%d|%d+%d|%u:",
            (unsigned)node->type,
            (unsigned)node->flags,
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
        first = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
        second = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
        if (first) {
            first_dump = pr_dump(first, &first_length);
        }
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

        if (pr_fingerprint(parser->root, &before) != 0) {
            example_failed = 1;
        }
        first = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
        other = markdown_core_parser_derive_tree(parser, empty_map, 0);
        again = markdown_core_parser_derive_tree(parser, parser->refmap, 0);
        if (!example_failed && pr_fingerprint(parser->root, &after) != 0) {
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
    {"projection_slope", case_projection_slope, 0},
};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    const char *spec = NULL;
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
        } else {
            fputs("usage: projection_runner [--list] --case NAME --spec FILE\n", stderr);
            return 2;
        }
    }
    if (!case_name) {
        fputs("usage: projection_runner [--list] --case NAME [--spec FILE]\n", stderr);
        return 2;
    }
    for (i = 0; i < sizeof(PR_CASES) / sizeof(PR_CASES[0]); i++) {
        if (strcmp(PR_CASES[i].name, case_name) == 0) {
            int failed;
            if (PR_CASES[i].needs_spec) {
                if (!spec) {
                    fprintf(stderr, "case %s requires --spec FILE\n", case_name);
                    return 2;
                }
                if (ts_spec_load(spec, &file) != 0) {
                    fprintf(stderr, "projection_runner: cannot load %s\n", spec);
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
