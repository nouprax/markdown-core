#include "autolink_scanners.h"
#include "code_block_scanners.h"
#include "comment_scanners.h"
#include "footnote_scanners.h"
#include "formula_scanners.h"
#include "heading_scanners.h"
#include "html_scanners.h"
#include "link_scanners.h"
#include "table_scanners.h"
#include "text_scanners.h"
#include <stdio.h>
#include "table.h"
#include "autolink.h"
#include "formula.h"
#include "directive.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#if !defined(NDEBUG) && !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "alloc.h"
#include "node_type.h"
#include "node.h"
#include "buffer.h"
#include "parser.h"
#include "element.h"
#include "markdown-core-elements.h"
#include "document.h"
#include "../../elements/attributes.h"
#include "sink.h"
#include "block_identifier.h"
#include "link.h"
#include "callout.h"
#include "citation.h"
#include "comment.h"
#include "cross_link.h"
#include "definition_list.h"
#include "embedded.h"
#include "footnote.h"
#include "heading_state.h"
#include "heading.h"
#include "list.h"
#include "properties.h"
#include "specimen.h"
#include "text.h"

#include <markdown_core.h>

#include "ast_internal.h"
#include "block_internal.h"
#include "inline_internal.h"

#include "harness.h"
#include "cplusplus.h"

/* The child of `node` at `index`, or NULL past its children. */
static markdown_core_node *child_at(const markdown_core_node *node, size_t index) {
    return index < node->child_count ? node->children[index] : NULL;
}

/* `child`, a node nothing holds, at the end of `parent`'s children when
 * `parent` can contain it: 1, or 0 having appended nothing. */
static int append_child(markdown_core_node *parent, markdown_core_node *child) {
    return markdown_core_node_can_contain_type(parent, (markdown_core_node_type)child->kind) &&
           markdown_core_node_push_child(NULL, parent, child);
}

/* The last child of `node`, or NULL when it has none. */
static markdown_core_node *last_child_of(const markdown_core_node *node) {
    return node->child_count ? node->children[node->child_count - 1] : NULL;
}

/* `node`'s last child, out of its children: the caller holds it. The array
 * goes with the last of them, as a node with no children has none. */
static markdown_core_node *take_last_child(markdown_core_node *node) {
    markdown_core_node *child = node->children[--node->child_count];
    if (!node->child_count) {
        markdown_core_node_pool_bytes_free(NULL, node->children);
        node->children = NULL;
    }
    return child;
}

/* A DEPTH-FIRST WALK over a subtree's children arrays: every node yields one
 * ENTER and one EXIT with its descendants' events between them, and DONE
 * follows the root's EXIT. Node-valued fields are independent roots the walk
 * never discovers. A node may be freed at its EXIT: the walk never reads it
 * again. */
typedef struct {
    markdown_core_node *node;
    uint32_t next;
} tree_frame;

typedef struct {
    markdown_core_node *root, *node;
    tree_frame *frames;
    size_t depth, capacity;
    bool started;
} tree_iter;

static tree_iter *tree_iter_new(markdown_core_node *root) {
    tree_iter *iter = calloc(1, sizeof(*iter));
    if (!iter) {
        abort();
    }
    iter->root = root;
    return iter;
}

static void tree_iter_free(tree_iter *iter) {
    if (iter) {
        free(iter->frames);
        free(iter);
    }
}

static void tree_iter_enter(tree_iter *iter, markdown_core_node *node) {
    if (iter->depth == iter->capacity) {
        iter->capacity = iter->capacity ? iter->capacity * 2 : 16;
        iter->frames = realloc(iter->frames, iter->capacity * sizeof(*iter->frames));
        if (!iter->frames) {
            abort();
        }
    }
    iter->frames[iter->depth++] = (tree_frame){node, 0};
    iter->node = node;
}

static markdown_core_event_type tree_iter_next(tree_iter *iter) {
    if (!iter->started) {
        iter->started = true;
        tree_iter_enter(iter, iter->root);
        return MARKDOWN_CORE_EVENT_ENTER;
    }
    if (!iter->depth) {
        iter->node = NULL;
        return MARKDOWN_CORE_EVENT_DONE;
    }
    tree_frame *top = &iter->frames[iter->depth - 1];
    if (top->next < top->node->child_count) {
        tree_iter_enter(iter, top->node->children[top->next++]);
        return MARKDOWN_CORE_EVENT_ENTER;
    }
    iter->node = top->node;
    iter->depth--;
    return MARKDOWN_CORE_EVENT_EXIT;
}

static markdown_core_node *tree_iter_get_node(const tree_iter *iter) { return iter->node; }

/* The node whose children the walk's current node is among, or NULL for the
 * root: the frame below the node's own at its ENTER, the top one at its EXIT,
 * which already took the node off. */
static markdown_core_node *tree_iter_parent(const tree_iter *iter) {
    size_t below = iter->depth && iter->frames[iter->depth - 1].node == iter->node ? 1 : 0;
    return iter->depth > below ? iter->frames[iter->depth - 1 - below].node : NULL;
}

/* The node whose children `node` is among in the tree under `root`, or NULL
 * when it is the root or not in the tree. */
static markdown_core_node *parent_in(markdown_core_node *root, const markdown_core_node *node) {
    tree_iter *iter = tree_iter_new(root);
    markdown_core_node *parent = NULL;
    for (markdown_core_event_type event; (event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE;) {
        if (event == MARKDOWN_CORE_EVENT_ENTER && tree_iter_get_node(iter) == node) {
            parent = tree_iter_parent(iter);
            break;
        }
    }
    tree_iter_free(iter);
    return parent;
}

/* A FACADE CALL THE CASE EXPECTS TO SUCCEED. Any other status names the call
 * and aborts the suite, so no case reads an out-parameter the call did not
 * write; the failure boundary of each call is the facade suite's to test. */
static void expect_ok(markdown_core_status status, const char *call) {
    if (status != MARKDOWN_CORE_OK) {
        fprintf(stderr, "%s answered status %d\n", call, (int)status);
        abort();
    }
}
#define EXPECT_OK(call) expect_ok((call), #call)

/* The document the facade parses from `length` bytes of `source`, or NULL
 * when the parse fails. */
static markdown_core_document *facade_parse_in(const void *source, size_t length, markdown_core_text_unit unit) {
    markdown_core_document *document;
    return markdown_core_document_parse_in((const uint8_t *)source, length, unit, &document) == MARKDOWN_CORE_OK
               ? document
               : NULL;
}

static markdown_core_document *facade_parse(const void *source, size_t length) {
    return facade_parse_in(source, length, MARKDOWN_CORE_TEXT_UNIT_UTF8);
}

/* The facade's answer for a node of the kind each accessor reads, or an index
 * below its count. */
typedef markdown_core_status (*facade_node_field)(const markdown_core_node *, const markdown_core_node **);
typedef markdown_core_status (*facade_metadata_field)(const markdown_core_node *,
                                                      const markdown_core_metadata_value **);

static const markdown_core_node *facade_field(const markdown_core_node *node, facade_node_field accessor) {
    const markdown_core_node *value;
    EXPECT_OK(accessor(node, &value));
    return value;
}

/* The facade's slice for a node of the kind each accessor reads. */
typedef markdown_core_status (*facade_nodes_field)(const markdown_core_node *, markdown_core_nodes *);

static markdown_core_nodes facade_nodes(const markdown_core_node *node, facade_nodes_field accessor) {
    markdown_core_nodes value;
    EXPECT_OK(accessor(node, &value));
    return value;
}

/* The child the facade answers for `node` at `index`, or NULL past its
 * children. */
static const markdown_core_node *facade_child(const markdown_core_node *node, size_t index) {
    markdown_core_nodes children = markdown_core_node_children(node);
    return index < children.count ? children.items[index] : NULL;
}

static const markdown_core_metadata_value *facade_metadata(const markdown_core_node *node,
                                                           facade_metadata_field accessor) {
    const markdown_core_metadata_value *value;
    EXPECT_OK(accessor(node, &value));
    return value;
}

#define FACADE_ANSWER(type, name, accessor)                                                                            \
    static type name(const markdown_core_node *node) {                                                                 \
        type value;                                                                                                    \
        EXPECT_OK(accessor(node, &value));                                                                             \
        return value;                                                                                                  \
    }
FACADE_ANSWER(markdown_core_string, facade_literal, markdown_core_node_literal)
FACADE_ANSWER(markdown_core_destination, facade_destination, markdown_core_node_destination)
FACADE_ANSWER(markdown_core_optional_string, facade_title, markdown_core_node_title)
FACADE_ANSWER(markdown_core_optional_string, facade_cross_label, markdown_core_node_cross_label)
FACADE_ANSWER(markdown_core_optional_string, facade_list_item_marker, markdown_core_node_list_item_marker)
FACADE_ANSWER(markdown_core_optional_string, facade_footnote_label, markdown_core_footnote_label)
FACADE_ANSWER(markdown_core_referent, facade_referent, markdown_core_citation_referent)
FACADE_ANSWER(int32_t, facade_heading_level, markdown_core_node_heading_level)
FACADE_ANSWER(const markdown_core_dimensions *, facade_dimensions, markdown_core_node_dimensions)
#undef FACADE_ANSWER

static markdown_core_metadata_scalar facade_scalar(const markdown_core_metadata_value *value) {
    markdown_core_metadata_scalar scalar;
    EXPECT_OK(markdown_core_metadata_value_scalar(value, &scalar));
    return scalar;
}

static size_t facade_item_count(const markdown_core_metadata_value *value) {
    size_t count;
    EXPECT_OK(markdown_core_metadata_value_item_count(value, &count));
    return count;
}

static markdown_core_metadata_list_item facade_item_at(const markdown_core_metadata_value *value, size_t index) {
    markdown_core_metadata_list_item item;
    EXPECT_OK(markdown_core_metadata_value_item_at(value, index, &item));
    return item;
}

static markdown_core_table_column facade_column_at(const markdown_core_node *node, size_t index) {
    markdown_core_table_column column;
    EXPECT_OK(markdown_core_node_table_column_at(node, index, &column));
    return column;
}

static markdown_core_string facade_class_at(const markdown_core_node *node, size_t index) {
    markdown_core_string value;
    EXPECT_OK(markdown_core_node_attribute_class_at(node, index, &value));
    return value;
}

static const markdown_core_node *facade_footnote_at(const markdown_core_document *document, size_t index) {
    const markdown_core_node *node;
    EXPECT_OK(markdown_core_document_footnote_at(document, index, &node));
    return node;
}

static const markdown_core_node *facade_specimen_at(const markdown_core_document *document, size_t index) {
    const markdown_core_node *node;
    EXPECT_OK(markdown_core_document_specimen_at(document, index, &node));
    return node;
}

static const markdown_core_node *facade_node_at(const markdown_core_document *document, markdown_core_position position,
                                                const uint8_t *source, size_t length) {
    const markdown_core_node *node;
    EXPECT_OK(markdown_core_document_node_at(document, position, source, length, &node));
    return node;
}

static const char *facade_kind_name(markdown_core_node_kind kind) {
    const char *name;
    EXPECT_OK(markdown_core_node_kind_name(kind, &name));
    return name;
}

/* The parse record of `element` in the parse `parser` runs, as `type`: how a
 * test reads an element's work, from outside the dialect. */
#define ELEMENT_STATE(parser, type, element) ((type *)markdown_core_parser_instance((parser), &(element))->state)

/* WHERE A NODE IS, as editor coordinates. A parsed tree is published: each
 * node holds its extent, and its scope is a query over the tree and the
 * source its root was parsed from, in UTF-8 columns. Each parse this file
 * runs records its root and source, and freeing that root ends the record,
 * so every record names a live tree: a node reads against the recorded tree
 * that holds it, through its field roots too, and a node no recorded tree
 * holds has no scope. */
typedef struct {
    const markdown_core_node *root;
    const char *source;
    size_t length;
} test_parse;

static test_parse *test_parses;
static size_t test_parse_count, test_parse_capacity;

static markdown_core_node *test_remember_parse(markdown_core_node *root, const char *source, size_t length) {
    if (!root) {
        return root;
    }
    if (test_parse_count == test_parse_capacity) {
        size_t capacity = test_parse_capacity ? test_parse_capacity * 2 : 1;
        test_parse *parses = realloc(test_parses, capacity * sizeof(*parses));
        if (!parses) {
            fprintf(stderr, "no memory to record a parse\n");
            abort();
        }
        test_parses = parses;
        test_parse_capacity = capacity;
    }
    test_parses[test_parse_count++] = (test_parse){root, source, length};
    return root;
}

/* Frees a node, ending the record of the parse it is the root of. */
static void test_free_node(markdown_core_node *node) {
    size_t kept = 0;
    for (size_t i = 0; i < test_parse_count; i++) {
        if (test_parses[i].root != node) {
            test_parses[kept++] = test_parses[i];
        }
    }
    test_parse_count = kept;
    markdown_core_node_free(node);
}

#define markdown_core_parse_document(source, length)                                                                   \
    test_remember_parse(markdown_core_parse_document((source), (length)), (source), (length))
#define markdown_core_parse_document_with_setup(source, length, setup, context)                                        \
    test_remember_parse(markdown_core_parse_document_with_setup((source), (length), (setup), (context)), (source),     \
                        (length))
#define markdown_core_node_free(node) test_free_node(node)

/* The bounds of a node's scopes, which it releases: the first one's start
 * and the last one's end. */
static markdown_core_scope scope_bounds(markdown_core_scope *scopes, size_t count) {
    markdown_core_scope scope = {{0, 0}, {0, 0}};
    if (count) {
        scope = (markdown_core_scope){scopes[0].start, scopes[count - 1].end};
    }
    markdown_core_scopes_free(scopes);
    return scope;
}

static markdown_core_scope test_node_scope(const markdown_core_node *node) {
    for (size_t i = test_parse_count; i-- > 0;) {
        const test_parse *parse = &test_parses[i];
        markdown_core_scope *scopes;
        size_t count;
        if (markdown_core_tree_scope(parse->root, node, (const uint8_t *)parse->source, parse->length,
                                     MARKDOWN_CORE_TEXT_UNIT_UTF8, &scopes, &count)) {
            return scope_bounds(scopes, count);
        }
    }
    return scope_bounds(NULL, 0);
}

#define SCOPE(node) test_node_scope(node)
#define START_LINE(node) (SCOPE(node).start.line)
#define START_COLUMN(node) (SCOPE(node).start.column)
#define END_LINE(node) (SCOPE(node).end.line)
#define END_COLUMN(node) (SCOPE(node).end.column)

/* A published document's node carries only its extent; its scope is a query
 * over the document and the source it was parsed from. */
static markdown_core_scope document_scope_of(const markdown_core_document *document, const markdown_core_node *node,
                                             const char *source) {
    markdown_core_scope *scopes = NULL;
    size_t count = 0;
    EXPECT_OK(markdown_core_document_scope(document, node, (const uint8_t *)source, strlen(source), &scopes, &count));
    return scope_bounds(scopes, count);
}

/* Literal access for engine tests goes through the installed facade, which
 * reads a node without materializing or changing anything. A code block's
 * literal is one of its block properties there; everything else with bytes of
 * its own answers `markdown_core_node_literal`. */
static bool literal_of(const markdown_core_node *node, markdown_core_string *literal) {
    markdown_core_optional_string info, language;
    bool fenced, closed;
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        EXPECT_OK(markdown_core_node_code_block_properties(node, &info, &language, literal, &fenced, &closed));
        return true;
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        *literal = facade_literal(node);
        return true;
    default:
        return false;
    }
}

static bool strings_equal(markdown_core_string a, markdown_core_string b) {
    return a.length == b.length && (!a.length || memcmp(a.data, b.data, a.length) == 0);
}

static bool literal_is(const markdown_core_node *node, const char *expected) {
    markdown_core_string literal, wanted;
    wanted.data = (const uint8_t *)expected;
    wanted.length = strlen(expected);
    return literal_of(node, &literal) && strings_equal(literal, wanted);
}

static void LITERAL_EQ(test_batch_runner *runner, const markdown_core_node *node, const char *expected, const char *msg,
                       ...) {
    char message[512];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(message, sizeof(message), msg, ap);
    va_end(ap);
    markdown_core_string literal;
    if (!node || !literal_of(node, &literal)) {
        OK(runner, 0, "%s (the node has no literal)", message);
        return;
    }
    OK(runner, literal_is(node, expected), "%s (got \"%.*s\", expected \"%s\")", message, (int)literal.length,
       (const char *)literal.data, expected);
}

/* A formula node's literal, a view of bytes its node owns, against `expected`. */
static void FORMULA_LITERAL_EQ(test_batch_runner *runner, const markdown_core_node *formula, const char *expected,
                               const char *msg) {
    const markdown_core_chunk literal = markdown_core_elements_formula_literal(formula);
    const size_t length = strlen(expected);
    OK(runner, (size_t)literal.len == length && (!length || memcmp(literal.data, expected, length) == 0),
       "%s (got \"%.*s\", expected \"%s\")", msg, (int)literal.len, (const char *)literal.data, expected);
}

/* Appends `node`'s literal to `buffer`, or nothing when it has none. */
static void put_literal(markdown_core_strbuf *buffer, const markdown_core_node *node) {
    markdown_core_string literal;
    if (literal_of(node, &literal) && literal.length) {
        markdown_core_strbuf_put(buffer, literal.data, (bufsize_t)literal.length);
    }
}

/* Replaces the literal of a node built by hand. Parsing is the only producer of
 * literals in the library; this is the test fixture's way of building one. */
static int set_literal(markdown_core_node *node, const char *content) {
    switch (node ? node->kind : MARKDOWN_CORE_NODE_NONE) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return markdown_core_chunk_set_cstr(&node->as.html_block->literal, content);
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        return markdown_core_chunk_set_cstr(node->as.literal, content);
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_set_cstr(&node->as.code->literal, content);
    default:
        return 0;
    }
}

#define UTF8_REPL "\xEF\xBF\xBD"

/* THE TEST BINARY'S ALLOCATOR, SUBSTITUTED AT THE LINK.
 *
 * `core/alloc.c` defines these three and nothing else, so defining them here
 * keeps that archive member from ever being pulled and every allocation the
 * library makes lands in this file. See `core/alloc.h` for why the seam is the
 * linker rather than a swappable global.
 *
 * There is ONE allocator per binary, so the three separate probes this file
 * used to install per object -- a refusing one for strbuf, a counting one for
 * node payloads, a watching one for a single marker -- are one allocator with
 * three independent behaviours. Each stays inert until a test arms it.
 *
 * `payload_live` is ARMED rather than always counting, and that is the whole
 * difference from the per-object allocators. It used to count allocations made
 * through one `markdown_core_mem` and nothing else, so `payload_live == 0` read
 * as "the objects this test made are all gone". Counting every allocation in
 * the process instead would fold in whatever else happens to be alive, so the
 * counter runs only between `payload_probe_arm` and `payload_probe_disarm`,
 * where the test allocates and frees everything it asserts about. */
static size_t payload_allocations, payload_fail_at, payload_live;
/* Calls into the free seam while counting, a null pointer included: the count
 * for a release that has nothing to release. */
static size_t payload_releases;
static int payload_counting;
static int payload_fill_fresh;
static int strbuf_refuse_next;
static void *marker_to_free;
static int marker_free_count;

/* Counts calls, for the tests that assert a speculative probe allocated
 * nothing it would throw away. */
static size_t text_allocation_calls;
static int text_counting;

/* Accounts BYTES, for the test that asserts long scalar text needs no index
 * storage. It prefixes every allocation with its size, which is why it must be
 * armed only around a CLOSED region: a pointer allocated before arming and
 * freed while armed would be read through a header that is not there. The one
 * test that uses it arms immediately before its parse and disarms after the
 * document is freed; the source buffer it builds is allocated and freed
 * outside that window. */
typedef union {
    size_t size;
    long double alignment;
    void *pointer;
} properties_allocation;
static size_t properties_live_bytes, properties_peak_bytes;
static int properties_counting, properties_force_moves;

static void properties_account(size_t old_size, size_t new_size) {
    properties_live_bytes = properties_live_bytes - old_size + new_size;
    if (properties_live_bytes > properties_peak_bytes) {
        properties_peak_bytes = properties_live_bytes;
    }
}

static void properties_probe_arm(void) {
    properties_live_bytes = properties_peak_bytes = 0;
    properties_counting = 1;
}

static void properties_probe_disarm(void) {
    if (!properties_peak_bytes) {
        fprintf(stderr, "byte probe disarmed having accounted no bytes: the region measures nothing\n");
        abort();
    }
    properties_counting = 0;
}

/* ARMING IS CHECKED AT RUNTIME, NOT BY READING THE SOURCE.
 *
 * An armed region exists to measure a parse, and a parse always allocates. So
 * a region that ends having observed NOTHING was not armed around the thing it
 * meant to measure -- armed too late, disarmed too early, or wrapped around
 * the wrong statement -- and every assertion that follows it compares zero
 * against zero. That is the failure `grid_opening_memory` shipped with, and no
 * audit over the source text catches it in general: a check that the arming
 * call appears in the same function is satisfied by arming AFTER the parse.
 *
 * So the probes check themselves. Disarming a region that counted nothing
 * aborts, which cannot be arranged around by moving a line. */
static void payload_probe_arm(void) {
    payload_allocations = payload_fail_at = payload_live = payload_releases = 0;
    payload_counting = 1;
}

static void payload_probe_disarm(void) {
    if (!payload_allocations) {
        fprintf(stderr, "payload probe disarmed having counted no allocation: the region measures nothing\n");
        abort();
    }
    payload_counting = 0;
}

typedef struct {
    size_t allocations, releases;
} payload_probe_calls;

/* Callbacks borrow the surrounding test's armed region. Never let a snapshot
 * silently observe disabled counters, even in an NDEBUG test binary. */
static payload_probe_calls payload_probe_snapshot(void) {
    if (!payload_counting) {
        fprintf(stderr, "payload snapshot requested outside an armed region\n");
        abort();
    }
    return (payload_probe_calls){payload_allocations, payload_releases};
}

/* Refusals are counted the same way whichever entry point asks, because a
 * constructor that grows a buffer and one that allocates a record are the same
 * ordinal to a sweep that refuses the Nth. */
static int allocation_refused(void) {
    if (strbuf_refuse_next) {
        strbuf_refuse_next = 0;
        return 1;
    }
    if (payload_counting && ++payload_allocations == payload_fail_at) {
        return 1;
    }
    return 0;
}

void *markdown_core_alloc(size_t count, size_t size) {
    void *pointer;
    if (allocation_refused()) {
        return NULL;
    }
    text_allocation_calls += text_counting;
    if (properties_counting) {
        properties_allocation *allocation;
        size_t bytes;
        if (count && size > (SIZE_MAX - sizeof(*allocation)) / count) {
            return NULL;
        }
        bytes = count * size;
        allocation = calloc(1, sizeof(*allocation) + bytes);
        if (!allocation) {
            return NULL;
        }
        allocation->size = bytes;
        properties_account(0, bytes);
        return allocation + 1;
    }
    pointer = calloc(count, size);
    if (payload_counting) {
        payload_live += pointer != NULL;
    }
    return pointer;
}

void *markdown_core_realloc(void *pointer, size_t size) {
    int fresh = pointer == NULL;
    void *result;
    if (allocation_refused()) {
        return NULL;
    }
    text_allocation_calls += text_counting;
    if (properties_counting) {
        properties_allocation *allocation;
        size_t old_size;
        if (!size) {
            markdown_core_free(pointer);
            return NULL;
        }
        if (size > SIZE_MAX - sizeof(*allocation)) {
            return NULL;
        }
        allocation = pointer ? (properties_allocation *)pointer - 1 : NULL;
        old_size = allocation ? allocation->size : 0;
        if (properties_force_moves) {
            properties_allocation *grown = malloc(sizeof(*grown) + size);
            if (!grown) {
                return NULL;
            }
            if (allocation) {
                memcpy(grown + 1, allocation + 1, old_size < size ? old_size : size);
                free(allocation);
            }
            allocation = grown;
        } else {
            allocation = realloc(allocation, sizeof(*allocation) + size);
        }
        if (!allocation) {
            return NULL;
        }
        allocation->size = size;
        properties_account(old_size, size);
        return allocation + 1;
    }
    result = realloc(pointer, size);
    if (fresh && result && payload_fill_fresh) {
        memset(result, 0xa5, size);
    }
    if (payload_counting) {
        payload_live += result != NULL && fresh;
    }
    return result;
}

void markdown_core_free(void *pointer) {
    if (pointer != NULL && pointer == marker_to_free) {
        marker_free_count++;
        marker_to_free = NULL;
    }
    if (properties_counting) {
        if (pointer) {
            properties_allocation *allocation = (properties_allocation *)pointer - 1;
            properties_account(allocation->size, 0);
            free(allocation);
        }
        return;
    }
    if (payload_counting) {
        payload_live -= pointer != NULL;
        payload_releases++;
    }
    free(pointer);
}

typedef struct probe_setup {
    const markdown_core_element *const *elements;
    size_t count;
} probe_setup;

static bool attach_probes(markdown_core_dialect_builder *builder, void *context) {
    const probe_setup *setup = (const probe_setup *)context;
    size_t i;

    for (i = 0; i < setup->count; i++) {
        if (!markdown_core_dialect_builder_attach(builder, setup->elements[i])) {
            return false;
        }
    }
    return true;
}

/* A document-lifecycle owner a test registers: a copy of the core document
 * element and of its lifecycle, one hook of which the test replaces. */
typedef struct {
    markdown_core_element element;
    markdown_core_document_ops document;
} document_owner;

/* `owner` becomes a copy of the core document element, so it keeps every
 * document hook and the size of the document's record, its lifecycle the
 * copy in `owner->document`, named `name`. */
static void copy_owner(document_owner *owner, const char *name) {
    owner->element = MARKDOWN_CORE_ELEMENT_DOCUMENT;
    owner->document = *MARKDOWN_CORE_ELEMENT_DOCUMENT.document;
    owner->element.name = name;
    owner->element.document = &owner->document;
}

/* A test reaches its live instance the way registration allows: by registering
 * a document-lifecycle owner whose `init` is replaced; that hook begins the
 * core lifecycle on its own instance before doing anything else. */
static bool attach_document_owner(markdown_core_dialect_builder *builder, document_owner *owner, const char *name,
                                  void (*init)(const markdown_core_element_instance *, markdown_core_parser *)) {
    copy_owner(owner, name);
    owner->document.init = init;
    return markdown_core_dialect_builder_attach(builder, &owner->element);
}

/* A test reads the finished parse the same way: as the document-lifecycle
 * owner, whose `finish` -- given here -- runs the core one on its own
 * instance before it reads anything; its other hooks are the core's. */
static bool attach_document_finisher(markdown_core_dialect_builder *builder, document_owner *owner, const char *name,
                                     void (*finish)(const markdown_core_element_instance *, markdown_core_parser *)) {
    copy_owner(owner, name);
    owner->document.finish = finish;
    return markdown_core_dialect_builder_attach(builder, &owner->element);
}

static markdown_core_node *parse_with_probes(const char *source, size_t length,
                                             const markdown_core_element *const *elements, size_t element_count) {
    probe_setup setup = {elements, element_count};
    return markdown_core_parse_document_with_setup(source, length, attach_probes, &setup);
}

static const markdown_core_node_type node_types[] = {
    MARKDOWN_CORE_NODE_DOCUMENT,       MARKDOWN_CORE_NODE_CALLOUT,    MARKDOWN_CORE_NODE_LIST,
    MARKDOWN_CORE_NODE_LIST_ITEM,      MARKDOWN_CORE_NODE_CODE_BLOCK, MARKDOWN_CORE_NODE_HTML_BLOCK,
    MARKDOWN_CORE_NODE_COMMENT_BLOCK,  MARKDOWN_CORE_NODE_PARAGRAPH,  MARKDOWN_CORE_NODE_HEADING,
    MARKDOWN_CORE_NODE_THEMATIC_BREAK, MARKDOWN_CORE_NODE_TEXT,       MARKDOWN_CORE_NODE_SOFT_BREAK,
    MARKDOWN_CORE_NODE_LINE_BREAK,     MARKDOWN_CORE_NODE_CODE,       MARKDOWN_CORE_NODE_HTML,
    MARKDOWN_CORE_NODE_COMMENT,        MARKDOWN_CORE_NODE_EMPHASIS,   MARKDOWN_CORE_NODE_STRONG,
    MARKDOWN_CORE_NODE_LINK,           MARKDOWN_CORE_NODE_EMBEDDED};
static const char *const node_type_names[] = {
    "document",  "callout", "list",           "list_item", "code_block", "html_block", "comment_block",
    "paragraph", "heading", "thematic_break", "text",      "soft_break", "line_break", "code",
    "html",      "comment", "emphasis",       "strong",    "link",       "embedded"};
static const int num_node_types = sizeof(node_types) / sizeof(*node_types);

static void test_md_paragraph_text(test_batch_runner *runner, const char *markdown, const char *expected_text,
                                   const char *msg);

static void test_md_paragraph_bytes(test_batch_runner *runner, const char *markdown, size_t markdown_length,
                                    const char *expected_text, const char *msg);

static markdown_core_node *parse(const char *source);

static void test_content(test_batch_runner *runner, markdown_core_node_type type, unsigned int *allowed_content);

static void test_valid_char(test_batch_runner *runner, const char *utf8, const char *msg);

/* The element types continue these two sequences, so listing them here means
 * the existing contiguity assertions pin every one of the nine values AND make
 * a collision or a gap impossible. Until Step 3.1 they were globals filled in
 * by runtime registration in whatever order `core_elements_registration`
 * called the `create_*` functions, and nothing in the repository asserted a
 * single one of them. */
static void node_type_values(test_batch_runner *runner) {
    static const markdown_core_node_type block_types[] = {
        MARKDOWN_CORE_NODE_DOCUMENT,     MARKDOWN_CORE_NODE_CALLOUT,       MARKDOWN_CORE_NODE_LIST,
        MARKDOWN_CORE_NODE_LIST_ITEM,    MARKDOWN_CORE_NODE_CODE_BLOCK,    MARKDOWN_CORE_NODE_HTML_BLOCK,
        MARKDOWN_CORE_NODE_PARAGRAPH,    MARKDOWN_CORE_NODE_HEADING,       MARKDOWN_CORE_NODE_THEMATIC_BREAK,
        MARKDOWN_CORE_NODE_FOOTNOTE,     MARKDOWN_CORE_NODE_TABLE,         MARKDOWN_CORE_NODE_TABLE_ROW,
        MARKDOWN_CORE_NODE_TABLE_CELL,   MARKDOWN_CORE_NODE_FORMULA_BLOCK, MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
        MARKDOWN_CORE_NODE_COMMENT_BLOCK};
    static const markdown_core_node_type inline_types[] = {
        MARKDOWN_CORE_NODE_TEXT,           MARKDOWN_CORE_NODE_SOFT_BREAK,
        MARKDOWN_CORE_NODE_LINE_BREAK,     MARKDOWN_CORE_NODE_CODE,
        MARKDOWN_CORE_NODE_HTML,           MARKDOWN_CORE_NODE_EMPHASIS,
        MARKDOWN_CORE_NODE_STRONG,         MARKDOWN_CORE_NODE_LINK,
        MARKDOWN_CORE_NODE_EMBEDDED,       MARKDOWN_CORE_NODE_CITE,
        MARKDOWN_CORE_NODE_STRIKETHROUGH,  MARKDOWN_CORE_NODE_FORMULA,
        MARKDOWN_CORE_NODE_DIRECTIVE,      MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
        MARKDOWN_CORE_NODE_COMMENT,        MARKDOWN_CORE_NODE_CITATION,
        MARKDOWN_CORE_NODE_CROSS_LINK,     MARKDOWN_CORE_NODE_MARK,
        MARKDOWN_CORE_NODE_CROSS_EMBEDDED, MARKDOWN_CORE_NODE_INSERTION};

    for (size_t i = 0; i < sizeof(block_types) / sizeof(*block_types); ++i) {
        INT_EQ(runner, block_types[i] & MARKDOWN_CORE_NODE_TYPE_MASK, MARKDOWN_CORE_NODE_TYPE_BLOCK,
               "block node type class %zu", i);
        INT_EQ(runner, block_types[i] & MARKDOWN_CORE_NODE_VALUE_MASK, i + 1, "block node type value %zu", i);
    }

    for (size_t i = 0; i < sizeof(inline_types) / sizeof(*inline_types); ++i) {
        INT_EQ(runner, inline_types[i] & MARKDOWN_CORE_NODE_TYPE_MASK, MARKDOWN_CORE_NODE_TYPE_INLINE,
               "inline node type class %zu", i);
        INT_EQ(runner, inline_types[i] & MARKDOWN_CORE_NODE_VALUE_MASK, i + 1, "inline node type value %zu", i);
    }
}

static void constructor(test_batch_runner *runner) {
    for (int i = 0; i < num_node_types; ++i) {
        markdown_core_node_type type = node_types[i];
        markdown_core_node *node = markdown_core_node_new(type);
        OK(runner, node != NULL, "new type %d", type);
        INT_EQ(runner, node->kind, type, "kind %d", type);
        STR_EQ(runner, markdown_core_node_get_type_string(node), node_type_names[i], "get_type_string %d", type);

        switch (node->kind) {
        case MARKDOWN_CORE_NODE_HEADING:
            INT_EQ(runner, node->as.heading->level, 1, "default heading level is 1");
            break;

        case MARKDOWN_CORE_NODE_LIST:
            INT_EQ(runner, node->as.list->flavor, MARKDOWN_CORE_LIST_FLAVOR_BULLET, "default list flavor is bullet");
            INT_EQ(runner, node->as.list->start, 0, "default list start is 0");
            INT_EQ(runner, node->as.list->tight, 0, "default list is loose");
            break;

        default:
            break;
        }

        markdown_core_node_free(node);
    }
}

/* A parsed tree read back through the installed facade: the facade is the one
 * reader, so what these assert is what every binding sees. */
static void accessors(test_batch_runner *runner) {
    static const char markdown[] = "## Header\n"
                                   "\n"
                                   "* Item 1\n"
                                   "* Item 2\n"
                                   "\n"
                                   "2. Item 1\n"
                                   "\n"
                                   "3. Item 2\n"
                                   "\n"
                                   "``` lang\n"
                                   "fenced\n"
                                   "```\n"
                                   "    code\n"
                                   "\n"
                                   "<div>html</div>\n"
                                   "\n"
                                   "[link](url 'title')\n";

    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1);
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start;
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    bool tight = false;
    markdown_core_optional_string info, language;
    markdown_core_string literal;
    bool fenced = false, closed = false;

    markdown_core_node *heading = child_at(doc, 0);
    OK(runner, facade_heading_level(heading) == 2, "heading level");

    markdown_core_node *bullet_list = child_at(doc, 1);
    EXPECT_OK(markdown_core_node_list_properties(bullet_list, &flavor, &start, &variant, &delimiter, &tight));
    OK(runner, flavor == MARKDOWN_CORE_LIST_FLAVOR_BULLET && !start.has_value && tight, "tight bullet list");

    markdown_core_node *ordered_list = child_at(doc, 2);
    EXPECT_OK(markdown_core_node_list_properties(ordered_list, &flavor, &start, &variant, &delimiter, &tight));
    OK(runner,
       flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED && start.has_value && start.value == 2 &&
           delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD && !tight,
       "loose ordered list from 2 with a period");

    markdown_core_node *fenced_code = child_at(doc, 3);
    EXPECT_OK(markdown_core_node_code_block_properties(fenced_code, &info, &language, &literal, &fenced, &closed));
    OK(runner, fenced && closed && info.has_value && info.value.length == 4 && memcmp(info.value.data, "lang", 4) == 0,
       "closed fenced code with its info string");
    LITERAL_EQ(runner, fenced_code, "fenced\n", "fenced code literal");

    markdown_core_node *code = child_at(doc, 4);
    EXPECT_OK(markdown_core_node_code_block_properties(code, &info, &language, &literal, &fenced, &closed));
    OK(runner, !fenced && !info.has_value, "indented code has no fence");
    LITERAL_EQ(runner, code, "code\n", "indented code literal");

    static const char unclosed_markdown[] = "``` lang\n"
                                            "unclosed\n";
    markdown_core_node *unclosed_doc = markdown_core_parse_document(unclosed_markdown, sizeof(unclosed_markdown) - 1);
    EXPECT_OK(markdown_core_node_code_block_properties(child_at(unclosed_doc, 0), &info, &language, &literal, &fenced,
                                                       &closed));
    OK(runner, fenced && !closed, "an unclosed fence reports open");
    markdown_core_node_free(unclosed_doc);

    markdown_core_node *html = child_at(doc, 5);
    LITERAL_EQ(runner, html, "<div>html</div>\n", "html block literal");

    markdown_core_node *paragraph = child_at(doc, 6);
    markdown_core_scope scope = SCOPE(paragraph);
    OK(runner, scope.start.line == 17 && scope.start.column == 1 && scope.end.line == 17, "paragraph scope");

    markdown_core_node *link = child_at(paragraph, 0);
    markdown_core_destination destination = facade_destination(link);
    markdown_core_optional_string title = facade_title(link);
    OK(runner,
       destination.kind == MARKDOWN_CORE_DESTINATION_URL && destination.url.length == 3 &&
           memcmp(destination.url.data, "url", 3) == 0,
       "a parsed link's destination is read through the facade");
    OK(runner, title.has_value && title.value.length == 5 && memcmp(title.value.data, "title", 5) == 0,
       "a parsed link's title is read through the facade");
    LITERAL_EQ(runner, child_at(link, 0), "link", "link text literal");

    markdown_core_node_free(doc);
}

static markdown_core_node *parse(const char *source) { return markdown_core_parse_document(source, strlen(source)); }

static void formula_element_accessors(test_batch_runner *runner) {
    markdown_core_node *doc = parse("Inline $x+y$ end.\n");
    markdown_core_node *paragraph = child_at(doc, 0);
    markdown_core_node *formula = child_at(paragraph, 1);

    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "formula type string");
    FORMULA_LITERAL_EQ(runner, formula, "x+y", "formula inline literal");
    INT_EQ(runner, markdown_core_elements_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
           "formula inline mode is embedded");
    INT_EQ(runner, markdown_core_elements_set_formula_literal(formula, "z"), 1, "set formula literal succeeds");
    FORMULA_LITERAL_EQ(runner, formula, "z", "formula literal setter updates payload");
    markdown_core_elements_set_formula_mode(formula, MARKDOWN_CORE_FORMULA_MODE_STANDALONE);
    INT_EQ(runner, markdown_core_elements_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula mode setter updates mode");
    markdown_core_node_free(doc);

    doc = parse("$$\nx+y\n$$\n");
    formula = child_at(doc, 0);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "standalone formula block type string");
    FORMULA_LITERAL_EQ(runner, formula, "x+y", "standalone formula block literal");
    INT_EQ(runner, markdown_core_elements_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula block mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("Display $$a+b$$ end.\n");
    paragraph = child_at(doc, 0);
    formula = child_at(paragraph, 1);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "standalone formula inline type string");
    FORMULA_LITERAL_EQ(runner, formula, "a+b", "standalone formula inline literal");
    INT_EQ(runner, markdown_core_elements_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula inline mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("Inline \\\\(x+y\\\\) end.\n");
    paragraph = child_at(doc, 0);
    formula = child_at(paragraph, 1);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "LaTeX embedded formula inline type string");
    FORMULA_LITERAL_EQ(runner, formula, "x+y", "LaTeX embedded formula inline literal");
    INT_EQ(runner, markdown_core_elements_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
           "LaTeX formula inline mode is embedded");
    markdown_core_node_free(doc);

    doc = parse("Display \\\\[x+y\\\\] end.\n");
    paragraph = child_at(doc, 0);
    formula = child_at(paragraph, 1);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula",
           "LaTeX standalone formula inline type string");
    FORMULA_LITERAL_EQ(runner, formula, "x+y", "LaTeX standalone formula inline literal");
    INT_EQ(runner, markdown_core_elements_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "LaTeX formula inline mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("\\\\[\nx+y\n\\\\]\n");
    formula = child_at(doc, 0);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "LaTeX standalone formula block type string");
    FORMULA_LITERAL_EQ(runner, formula, "x+y", "LaTeX standalone formula block literal");
    INT_EQ(runner, markdown_core_elements_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "LaTeX formula block mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("```formula\nx+y\n```\n");
    formula = child_at(doc, 0);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "formula fence becomes standalone block");
    FORMULA_LITERAL_EQ(runner, formula, "x+y", "formula fence literal is trimmed");
    markdown_core_node_free(doc);
}

/* Reads one attribute and compares it, so a sequence can be asserted without a
 * serialization to compare against -- which is what the deleted JSON was
 * doing for these tests. */
static void attribute_eq(test_batch_runner *runner, markdown_core_node *node, size_t index, const char *name,
                         const char *value, const char *message) {
    markdown_core_string actual_name, actual_value;
    EXPECT_OK(markdown_core_node_attribute_record_at(node, index, &actual_name, &actual_value));
    OK(runner,
       actual_name.length == strlen(name) && memcmp(actual_name.data, name, actual_name.length) == 0 &&
           actual_value.length == strlen(value) && memcmp(actual_value.data, value, actual_value.length) == 0,
       message);
}

/* The anchor `node` shows; none when it could not be read. */
static markdown_core_optional_string anchor_of(const markdown_core_node *node) {
    markdown_core_optional_string anchor = {0};
    if (markdown_core_node_anchor(node, &anchor) != MARKDOWN_CORE_OK) {
        return (markdown_core_optional_string){0};
    }
    return anchor;
}
static void directive_element_accessors(test_batch_runner *runner) {
    /* `:-a[]` was the input here until Step 7, and it is not a directive: a
     * name may not BEGIN with a hyphen or underscore any more than it may end
     * with one. `class` is also the one name whose repeats accumulate now, so
     * the three of them are one value rather than the last one. The sequence
     * keeps the first occurrence of each name in source order. */
    markdown_core_node *doc = parse(":a[]{id=first muted=true title=\"My Video\" bare= dup=first dup=last "
                                    "class=red class=green class=blue id=123}\n");
    markdown_core_node *paragraph = child_at(doc, 0);
    markdown_core_node *directive = child_at(paragraph, 0);
    markdown_core_node *label = markdown_core_directive_label(directive);

    STR_EQ(runner, markdown_core_node_get_type_string(directive), "directive", "directive inline type string");
    OK(runner, label != NULL && label->kind == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
       "an explicit directive label is a typed node field");
    OK(runner, child_at(directive, 0) == NULL, "an inline directive label is not a content child");
    STR_EQ(runner, markdown_core_elements_get_directive_name(directive), "a", "directive name getter");
    markdown_core_optional_string anchor = anchor_of(directive);
    OK(runner, anchor.has_value && anchor.value.length == 3 && memcmp(anchor.value.data, "123", 3) == 0,
       "last ID wins");
    INT_EQ(runner, (int)markdown_core_node_attribute_record_count(directive), 5, "every record occurrence survives");
    INT_EQ(runner, (int)markdown_core_node_attribute_class_count(directive), 3, "class assignments append words");
    attribute_eq(runner, directive, 0, "muted", "true", "first record");
    attribute_eq(runner, directive, 1, "title", "My Video", "quoted value");
    attribute_eq(runner, directive, 2, "bare", "", "empty assignment");
    attribute_eq(runner, directive, 3, "dup", "first", "first duplicate");
    attribute_eq(runner, directive, 4, "dup", "last", "last duplicate");

    INT_EQ(runner, markdown_core_elements_set_directive_name(directive, "next_name-2"), 1,
           "set directive name succeeds");
    STR_EQ(runner, markdown_core_elements_get_directive_name(directive), "next_name-2",
           "directive name setter updates payload");

    INT_EQ(runner, anchor_of(paragraph).has_value, 0, "a non-directive node has no attribute container");
    INT_EQ(runner, (int)markdown_core_node_attribute_record_count(paragraph), 0,
           "a non-directive node has no attributes to count");
    markdown_core_node_free(doc);

    /* A block directive has two independent node-valued relations: `label`
     * and block content. The ordinary cmark iterator follows only the content
     * child tree; callers can start a separate walk at the label field root. */
    doc = parse(":::note[Title]\nBody\n:::\n");
    directive = child_at(doc, 0);
    label = markdown_core_directive_label(directive);
    paragraph = child_at(directive, 0);
    OK(runner, label != NULL && paragraph != NULL && paragraph->kind == MARKDOWN_CORE_NODE_PARAGRAPH,
       "a block directive exposes label and content as distinct relations");
    OK(runner, directive->child_count == 1 && child_at(directive, 0) == paragraph,
       "the label is not mixed into the block content list");
    {
        markdown_core_node *expected[] = {directive, paragraph, child_at(paragraph, 0)};
        size_t entered = 0;
        tree_iter *iter = tree_iter_new(directive);
        markdown_core_event_type event;
        while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            if (event == MARKDOWN_CORE_EVENT_ENTER) {
                OK(runner,
                   entered < sizeof(expected) / sizeof(expected[0]) && tree_iter_get_node(iter) == expected[entered],
                   "iterator preserves structural child order");
                entered++;
            }
        }
        INT_EQ(runner, (int)entered, (int)(sizeof(expected) / sizeof(expected[0])),
               "iterator visits only directive content nodes");
        tree_iter_free(iter);
    }
    {
        markdown_core_node *expected[] = {label, child_at(label, 0)};
        size_t entered = 0;
        tree_iter *iter = tree_iter_new(label);
        markdown_core_event_type event;
        while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            if (event == MARKDOWN_CORE_EVENT_ENTER) {
                OK(runner,
                   entered < sizeof(expected) / sizeof(expected[0]) && tree_iter_get_node(iter) == expected[entered],
                   "a walk rooted at the label follows its own child tree");
                entered++;
            }
        }
        INT_EQ(runner, (int)entered, (int)(sizeof(expected) / sizeof(expected[0])),
               "the label tree is independently iterable");
        tree_iter_free(iter);
    }
    markdown_core_node_free(doc);

    /* Missing and authored-empty containers have the same public value. */
    doc = parse(":plain[] :empty{}\n");
    paragraph = child_at(doc, 0);
    directive = child_at(paragraph, 0);
    for (int i = 0; i < 2; i++) {
        OK(runner, !anchor_of(directive).has_value, "empty value has no anchor");
        INT_EQ(runner, (int)markdown_core_node_attribute_class_count(directive), 0, "empty classes");
        INT_EQ(runner, (int)markdown_core_node_attribute_record_count(directive), 0, "empty records");
        if (i == 0) {
            directive = child_at(paragraph, 2);
        }
    }
    markdown_core_node_free(doc);
}

static void create_tree(test_batch_runner *runner) {
    markdown_core_node *doc = markdown_core_node_new(MARKDOWN_CORE_NODE_DOCUMENT);

    markdown_core_node *p = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    OK(runner, append_child(doc, p), "append paragraph");
    OK(runner, doc->child_count == 1 && child_at(doc, 0) == p, "the document holds the paragraph");

    markdown_core_node *str1 = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    set_literal(str1, "Hello, ");
    markdown_core_node *emph = markdown_core_node_new(MARKDOWN_CORE_NODE_EMPHASIS);
    markdown_core_node *str2 = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    set_literal(str2, "world");
    markdown_core_node *str3 = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    set_literal(str3, "!");
    OK(runner, append_child(p, str1), "append str1");
    OK(runner, append_child(p, emph), "append emph");
    OK(runner, append_child(emph, str2), "append str2");
    OK(runner, append_child(p, str3), "append str3");
    markdown_core_node *orphan = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    markdown_core_node *block = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    OK(runner, !append_child(str1, orphan) && !str1->child_count, "a text node contains nothing");
    OK(runner, !append_child(emph, block) && emph->child_count == 1, "an inline contains no block");
    markdown_core_node_free(orphan);
    markdown_core_node_free(block);

    // Built tree: p -> [str1 "Hello, ", emph(str2 "world"), str3 "!"]
    OK(runner, p->child_count == 3 && child_at(p, 0) == str1 && child_at(p, 1) == emph && child_at(p, 2) == str3,
       "the paragraph holds its children in order");
    OK(runner, emph->child_count == 1 && child_at(emph, 0) == str2, "emph contains str2");
    LITERAL_EQ(runner, str1, "Hello, ", "str1 literal");
    LITERAL_EQ(runner, str2, "world", "str2 literal");
    LITERAL_EQ(runner, str3, "!", "str3 literal");

    markdown_core_node_free(doc);
}

typedef struct {
    bool accepts;
    size_t calls;
} attachment_policy;

/* The policy the case under test installs; containment hooks take no context
 * of their own. */
static attachment_policy *attachment_current_policy;

static int attachment_can_contain(const markdown_core_element *element, markdown_core_node *node,
                                  markdown_core_node_type child_kind) {
    (void)element;
    (void)node;
    attachment_policy *policy = attachment_current_policy;
    policy->calls++;
    return child_kind == MARKDOWN_CORE_NODE_TEXT && policy->accepts;
}

static const markdown_core_contain_ops ATTACHMENT_POLICY_CONTAIN = {
    .allows = attachment_can_contain,
};

static const markdown_core_element ATTACHMENT_POLICY = {
    .name = "attachment-policy",
    .contain = &ATTACHMENT_POLICY_CONTAIN,
};

/* A containment policy decides an append once, before the parent's children
 * change: a refused child is not appended, and an accepted one joins the end. */
static void attachment_containment(test_batch_runner *runner) {
    for (int accepted = 0; accepted <= 1; accepted++) {
        markdown_core_node *parent = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
        markdown_core_node *anchor = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
        markdown_core_node *child = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
        append_child(parent, anchor);
        attachment_policy policy = {accepted != 0, 0};
        parent->element = &ATTACHMENT_POLICY;
        attachment_current_policy = &policy;

        INT_EQ(runner, append_child(parent, child), accepted, "append honors containment before changing ownership");
        INT_EQ(runner, policy.calls, 1, "containment is validated exactly once");
        OK(runner,
           accepted ? parent->child_count == 2 && child_at(parent, 0) == anchor && child_at(parent, 1) == child
                    : parent->child_count == 1 && child_at(parent, 0) == anchor,
           "the parent's children change only when the policy accepts");
        attachment_current_policy = NULL;
        parent->element = NULL;
        if (!accepted) {
            markdown_core_node_free(child);
        }
        markdown_core_node_free(parent);
    }
}

void hierarchy(test_batch_runner *runner) {
    markdown_core_node *bquote1 = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);
    markdown_core_node *bquote2 = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);
    markdown_core_node *bquote3 = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);

    OK(runner, append_child(bquote1, bquote2), "append bquote2");
    OK(runner, append_child(bquote2, bquote3), "append bquote3");

    markdown_core_node_free(bquote1);

    unsigned int list_item_flag[] = {MARKDOWN_CORE_NODE_LIST_ITEM, 0};
    unsigned int top_level_blocks[] = {
        MARKDOWN_CORE_NODE_CALLOUT,    MARKDOWN_CORE_NODE_LIST,           MARKDOWN_CORE_NODE_CODE_BLOCK,
        MARKDOWN_CORE_NODE_HTML_BLOCK, MARKDOWN_CORE_NODE_COMMENT_BLOCK,  MARKDOWN_CORE_NODE_PARAGRAPH,
        MARKDOWN_CORE_NODE_HEADING,    MARKDOWN_CORE_NODE_THEMATIC_BREAK, 0};
    unsigned int all_inlines[] = {MARKDOWN_CORE_NODE_TEXT,
                                  MARKDOWN_CORE_NODE_SOFT_BREAK,
                                  MARKDOWN_CORE_NODE_LINE_BREAK,
                                  MARKDOWN_CORE_NODE_CODE,
                                  MARKDOWN_CORE_NODE_HTML,
                                  MARKDOWN_CORE_NODE_COMMENT,
                                  MARKDOWN_CORE_NODE_EMPHASIS,
                                  MARKDOWN_CORE_NODE_STRONG,
                                  MARKDOWN_CORE_NODE_LINK,
                                  MARKDOWN_CORE_NODE_EMBEDDED,
                                  0};

    test_content(runner, MARKDOWN_CORE_NODE_DOCUMENT, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_CALLOUT, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_LIST, list_item_flag);
    test_content(runner, MARKDOWN_CORE_NODE_LIST_ITEM, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_CODE_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_COMMENT_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_PARAGRAPH, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_HEADING, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_THEMATIC_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_TEXT, 0);
    test_content(runner, MARKDOWN_CORE_NODE_SOFT_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_LINE_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_CODE, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML, 0);
    test_content(runner, MARKDOWN_CORE_NODE_COMMENT, 0);
    test_content(runner, MARKDOWN_CORE_NODE_EMPHASIS, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_STRONG, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_LINK, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_EMBEDDED, all_inlines);
}

static void test_content(test_batch_runner *runner, markdown_core_node_type type, unsigned int *allowed_content) {
    markdown_core_node *node = markdown_core_node_new(type);

    for (int i = 0; i < num_node_types; ++i) {
        markdown_core_node_type child_type = node_types[i];
        markdown_core_node *child = markdown_core_node_new(child_type);

        int got = append_child(node, child);
        int expected = 0;
        if (allowed_content) {
            for (unsigned int *p = allowed_content; *p; ++p) {
                expected |= *p == (unsigned int)child_type;
            }
        }

        INT_EQ(runner, got, expected, "add %d as child of %d", child_type, type);

        markdown_core_node_free(got ? take_last_child(node) : child);
    }

    markdown_core_node_free(node);
}

static void parser(test_batch_runner *runner) {
    test_md_paragraph_text(runner, "No newline", "No newline", "document without trailing newline");
}

static void utf8(test_batch_runner *runner) {
    test_valid_char(runner, "\x01", "valid utf8 01");
    test_valid_char(runner, "\x7F", "valid utf8 7F");
    test_valid_char(runner, "\xC2\x80", "valid utf8 C280");
    test_valid_char(runner, "\xDF\xBF", "valid utf8 DFBF");
    test_valid_char(runner, "\xE0\xA0\x80", "valid utf8 E0A080");
    test_valid_char(runner, "\xED\x9F\xBF", "valid utf8 ED9FBF");
    test_valid_char(runner, "\xEF\xBF\xBE", "valid utf8 U+FFFE noncharacter");
    test_valid_char(runner, "\xEF\xBF\xBF", "valid utf8 U+FFFF noncharacter");
    test_valid_char(runner, "\xF0\x90\x80\x80", "valid utf8 F0908080");
    test_valid_char(runner, "\xF4\x8F\xBF\xBF", "valid utf8 F48FBFBF");

    // Test string containing null character
    static const char string_with_null[] = "((((\0))))";
    test_md_paragraph_bytes(runner, string_with_null, sizeof(string_with_null) - 1, "((((" UTF8_REPL "))))",
                            "utf8 with U+0000");

    // Test NUL followed by newline
    static const char string_with_nul_lf[] = "```\n\0\n```\n";
    markdown_core_node *doc = markdown_core_parse_document(string_with_nul_lf, sizeof(string_with_nul_lf) - 1);
    markdown_core_node *code_block = child_at(doc, 0);
    INT_EQ(runner, code_block->kind, MARKDOWN_CORE_NODE_CODE_BLOCK, "utf8 with \\0\\n parses a code block");
    LITERAL_EQ(runner, code_block, UTF8_REPL "\n", "utf8 with \\0\\n");
    markdown_core_node_free(doc);

    // A leading U+FEFF is an ordinary character: it is text, not a marker to skip.
    static const char string_with_bom[] = "\xef\xbb\xbf# Hello\n";
    doc = markdown_core_parse_document(string_with_bom, sizeof(string_with_bom) - 1);
    markdown_core_node *paragraph = child_at(doc, 0);
    INT_EQ(runner, paragraph->kind, MARKDOWN_CORE_NODE_PARAGRAPH, "a leading U+FEFF keeps its line a paragraph");
    LITERAL_EQ(runner, child_at(paragraph, 0), "\xef\xbb\xbf# Hello", "a leading U+FEFF stays in the text");
    markdown_core_node_free(doc);
}

static void test_valid_char(test_batch_runner *runner, const char *utf8, const char *msg) {
    char buf[20];
    char expected[30];

    snprintf(buf, sizeof(buf), "((((%s))))", utf8);
    snprintf(expected, sizeof(expected), "((((%s))))", utf8);
    test_md_paragraph_text(runner, buf, expected, msg);
}

static void line_endings(test_batch_runner *runner) {
    // Test list with different line endings
    static const char list_with_endings[] = "- a\n- b\r\n- c\r- d";
    static const char *const expected_items[] = {"a", "b", "c", "d"};
    markdown_core_node *doc = markdown_core_parse_document(list_with_endings, sizeof(list_with_endings) - 1);
    markdown_core_node *list = child_at(doc, 0);
    markdown_core_node *item = child_at(list, 0);
    INT_EQ(runner, list->kind, MARKDOWN_CORE_NODE_LIST, "list with different line endings parses one list");
    for (size_t i = 0; i < 4; i++) {
        OK(runner, item != NULL, "list item %zu exists", i);
        if (item) {
            markdown_core_node *paragraph = child_at(item, 0);
            LITERAL_EQ(runner, child_at(paragraph, 0), expected_items[i], "list item %zu text", i);
            item = child_at(list, i + 1);
        }
    }
    OK(runner, item == NULL, "list has exactly four items");
    markdown_core_node_free(doc);

    // A CRLF line ending is a SoftBreak between the two texts.
    static const char crlf_lines[] = "line\r\nline\r\n";
    doc = markdown_core_parse_document(crlf_lines, sizeof(crlf_lines) - 1);
    markdown_core_node *paragraph = child_at(doc, 0);
    markdown_core_node *middle = child_at(paragraph, 1);
    LITERAL_EQ(runner, child_at(paragraph, 0), "line", "crlf line splits into text");
    INT_EQ(runner, middle->kind, MARKDOWN_CORE_NODE_SOFT_BREAK, "crlf endings produce a softbreak");
    LITERAL_EQ(runner, child_at(paragraph, 2), "line", "crlf trailing text follows the softbreak");
    markdown_core_node_free(doc);

    static const char no_line_ending[] = "```\nline\n```";
    doc = markdown_core_parse_document(no_line_ending, sizeof(no_line_ending) - 1);
    markdown_core_node *code_block = child_at(doc, 0);
    INT_EQ(runner, code_block->kind, MARKDOWN_CORE_NODE_CODE_BLOCK, "fenced code block with no final newline parses");
    LITERAL_EQ(runner, code_block, "line\n", "fenced code block with no final newline");
    markdown_core_node_free(doc);
}

static void numeric_entities(test_batch_runner *runner) {
    test_md_paragraph_text(runner, "&#0;", UTF8_REPL, "Invalid numeric entity 0");
    test_md_paragraph_text(runner, "&#55295;", "\xED\x9F\xBF", "Valid numeric entity 0xD7FF");
    test_md_paragraph_text(runner, "&#xD800;", UTF8_REPL, "Invalid numeric entity 0xD800");
    test_md_paragraph_text(runner, "&#xDFFF;", UTF8_REPL, "Invalid numeric entity 0xDFFF");
    test_md_paragraph_text(runner, "&#57344;", "\xEE\x80\x80", "Valid numeric entity 0xE000");
    test_md_paragraph_text(runner, "&#x10FFFF;", "\xF4\x8F\xBF\xBF", "Valid numeric entity 0x10FFFF");
    test_md_paragraph_text(runner, "&#x110000;", UTF8_REPL, "Invalid numeric entity 0x110000");
    test_md_paragraph_text(runner, "&#9999999;", UTF8_REPL, "Seven-digit decimal entity is recognized");
    test_md_paragraph_text(runner, "&#99999999;", "&#99999999;", "Eight-digit decimal entity stays literal");
    test_md_paragraph_text(runner, "&#87654321;", "&#87654321;", "CommonMark long decimal entity stays literal");
    test_md_paragraph_text(runner, "&#x80000000;", "&#x80000000;", "Eight-digit hexadecimal entity stays literal");
    test_md_paragraph_text(runner, "&#xFFFFFFFF;", "&#xFFFFFFFF;", "Long hexadecimal entity stays literal");

    test_md_paragraph_text(runner, "&#;", "&#;", "Min decimal entity length");
    test_md_paragraph_text(runner, "&#x;", "&#x;", "Min hexadecimal entity length");
    test_md_paragraph_text(runner, "&#999999999;", "&#999999999;", "Overlong decimal entity stays literal");
    test_md_paragraph_text(runner, "&#x000000041;", "&#x000000041;", "Overlong hexadecimal entity stays literal");
    test_md_paragraph_text(runner, "a*\xC2\xA3*b", "a*\xC2\xA3*b",
                           "Unicode symbols are punctuation for emphasis flanking");
}

/* M0: an HTML comment is a `Comment` node of the dialect -- inline for the
 * token, block for an HTML block that opened with `<!--` and whose end line
 * held only whitespace after the first `-->` -- and its literal is the bytes
 * between the delimiters. Nothing strips it; the text around an inline
 * comment stays split where the comment sits. */
static void comment_nodes(test_batch_runner *runner) {
    static const char markdown[] = "before <!-- hidden --> after <br>\n"
                                   "\n"
                                   "<!-- block\n"
                                   "hidden -->\n"
                                   "\n"
                                   "<div>raw</div>\n"
                                   "\n"
                                   "<!-- a --> b\n"
                                   "\n"
                                   "<!-->\n";

    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1);
    markdown_core_node *paragraph = child_at(doc, 0);
    markdown_core_node *text = child_at(paragraph, 0);
    markdown_core_node *comment = child_at(paragraph, 1);
    markdown_core_node *after = child_at(paragraph, 2);
    markdown_core_node *inline_html = child_at(paragraph, 3);
    markdown_core_node *block_comment = child_at(doc, 1);
    markdown_core_node *block_html = child_at(doc, 2);
    markdown_core_node *trailing = child_at(doc, 3);
    markdown_core_node *empties = child_at(doc, 4);
    markdown_core_node *empty;

    LITERAL_EQ(runner, text, "before ", "text before an inline comment");
    INT_EQ(runner, comment->kind, MARKDOWN_CORE_NODE_COMMENT, "inline comment type");
    LITERAL_EQ(runner, comment, " hidden ", "inline comment literal excludes delimiters");
    STR_EQ(runner, markdown_core_node_get_type_string(comment), "comment", "inline comment type string");
    LITERAL_EQ(runner, after, " after ", "text after an inline comment");
    INT_EQ(runner, inline_html->kind, MARKDOWN_CORE_NODE_HTML, "a tag stays inline HTML");
    LITERAL_EQ(runner, inline_html, "<br>", "inline HTML literal");

    INT_EQ(runner, block_comment->kind, MARKDOWN_CORE_NODE_COMMENT_BLOCK, "block comment type");
    LITERAL_EQ(runner, block_comment, " block\nhidden ",
               "block comment literal keeps its line ending and excludes delimiters");
    STR_EQ(runner, markdown_core_node_get_type_string(block_comment), "comment_block", "block comment type string");
    INT_EQ(runner, START_LINE(block_comment), 3, "block comment starts on its opener line");
    INT_EQ(runner, END_LINE(block_comment), 4, "block comment ends on its closer line");
    INT_EQ(runner, END_COLUMN(block_comment), 10, "block comment ends at its closer");

    INT_EQ(runner, block_html->kind, MARKDOWN_CORE_NODE_HTML_BLOCK, "a div stays an HTML block");
    LITERAL_EQ(runner, block_html, "<div>raw</div>\n", "HTML block literal");
    INT_EQ(runner, trailing->kind, MARKDOWN_CORE_NODE_HTML_BLOCK,
           "non-whitespace after the closer keeps the HTML block");
    LITERAL_EQ(runner, trailing, "<!-- a --> b\n", "HTML block literal as written");

    INT_EQ(runner, empties->kind, MARKDOWN_CORE_NODE_COMMENT_BLOCK, "`<!-->` opens a block comment");
    LITERAL_EQ(runner, empties, "", "`<!-->` as a block has an empty literal");
    OK(runner, child_at(empties, 0) == NULL, "a block comment is a leaf");

    markdown_core_node_free(doc);

    doc = markdown_core_parse_document("a <!--> b <!---> c <!----> d\n", 29);
    paragraph = child_at(doc, 0);
    empty = child_at(paragraph, 1);
    INT_EQ(runner, empty->kind, MARKDOWN_CORE_NODE_COMMENT, "`<!-->` is an inline comment");
    LITERAL_EQ(runner, empty, "", "`<!-->` has an empty literal");
    INT_EQ(runner, START_COLUMN(empty), 3, "`<!-->` starts at its `<`");
    INT_EQ(runner, END_COLUMN(empty), 7, "`<!-->` ends at its `>`");
    empty = child_at(paragraph, 3);
    INT_EQ(runner, empty->kind, MARKDOWN_CORE_NODE_COMMENT, "`<!--->` is an inline comment");
    LITERAL_EQ(runner, empty, "", "`<!--->` has an empty literal");
    empty = child_at(paragraph, 5);
    INT_EQ(runner, empty->kind, MARKDOWN_CORE_NODE_COMMENT, "`<!---->` is an inline comment");
    LITERAL_EQ(runner, empty, "", "`<!---->` has an empty literal");
    markdown_core_node_free(doc);

    /* The line splitter is the inherited one: a CR, an LF, or a CRLF ends a
     * line, and every block receives its lines LF-terminated, so every
     * literal in the tree -- a code block's, an HTML block's, a comment's --
     * holds LF where the source held CR or CRLF. cmark 0.31.2 stores the same
     * bytes for the same input. A comment that kept the CR bytes alone would
     * be the one literal out of step with its neighbours. */
    {
        static const char crlf[] = "a <!--x\r\ny--> b\r\n\r\n<!--\r\nx\r\n-->\r\n\r\n```\r\nx\r\n```\r\n";
        static const char cr[] = "a <!--x\ry--> b\r\r<!--\rx\r-->\r";
        markdown_core_node *code;
        doc = markdown_core_parse_document(crlf, sizeof(crlf) - 1);
        paragraph = child_at(doc, 0);
        comment = child_at(paragraph, 1);
        block_comment = child_at(doc, 1);
        code = child_at(doc, 2);
        INT_EQ(runner, comment->kind, MARKDOWN_CORE_NODE_COMMENT, "CRLF: inline comment");
        LITERAL_EQ(runner, comment, "x\ny", "CRLF inside an inline comment is stored as LF");
        INT_EQ(runner, END_LINE(comment), 2, "CRLF: the inline comment ends on line 2");
        INT_EQ(runner, block_comment->kind, MARKDOWN_CORE_NODE_COMMENT_BLOCK, "CRLF: block comment");
        LITERAL_EQ(runner, block_comment, "\nx\n", "CRLF inside a block comment is stored as LF");
        INT_EQ(runner, END_LINE(block_comment), 6, "CRLF: the block comment ends on its closer line");
        INT_EQ(runner, code->kind, MARKDOWN_CORE_NODE_CODE_BLOCK, "CRLF: code block");
        LITERAL_EQ(runner, code, "x\n", "CRLF inside a code block is stored as LF too");
        markdown_core_node_free(doc);

        doc = markdown_core_parse_document(cr, sizeof(cr) - 1);
        paragraph = child_at(doc, 0);
        comment = child_at(paragraph, 1);
        block_comment = child_at(doc, 1);
        LITERAL_EQ(runner, comment, "x\ny", "a lone CR inside an inline comment is stored as LF");
        LITERAL_EQ(runner, block_comment, "\nx\n", "a lone CR inside a block comment is stored as LF");
        INT_EQ(runner, END_LINE(block_comment), 6, "CR: the block comment ends on its closer line");
        markdown_core_node_free(doc);
    }
}

/* Parses and asserts the document is a single paragraph whose concatenated
 * Text literals equal `expected_text`.  This replaces the retired
 * markdown_to_html comparisons: AST literals carry raw bytes, without HTML
 * escaping. */
static void test_md_paragraph_bytes(test_batch_runner *runner, const char *markdown, size_t markdown_length,
                                    const char *expected_text, const char *msg) {
    markdown_core_node *doc = markdown_core_parse_document(markdown, markdown_length);
    markdown_core_node *paragraph = child_at(doc, 0);
    char text[4096] = "";
    size_t length = 0;
    markdown_core_node *child;

    if (paragraph->kind != MARKDOWN_CORE_NODE_PARAGRAPH || child_at(doc, 1) != NULL) {
        OK(runner, 0, "%s (document is a single paragraph)", msg);
        markdown_core_node_free(doc);
        return;
    }
    for (uint32_t index = 0; index < paragraph->child_count; index++) {
        child = paragraph->children[index];
        markdown_core_string literal;
        size_t literal_length;
        if (child->kind != MARKDOWN_CORE_NODE_TEXT || !literal_of(child, &literal)) {
            OK(runner, 0, "%s (paragraph contains only text)", msg);
            markdown_core_node_free(doc);
            return;
        }
        literal_length = literal.length;
        if (length + literal_length + 1 > sizeof(text)) {
            OK(runner, 0, "%s (text fits the harness buffer)", msg);
            markdown_core_node_free(doc);
            return;
        }
        memcpy(text + length, literal.data, literal_length);
        length += literal_length;
        text[length] = '\0';
    }
    STR_EQ(runner, text, expected_text, "%s", msg);
    markdown_core_node_free(doc);
}

static void test_md_paragraph_text(test_batch_runner *runner, const char *markdown, const char *expected_text,
                                   const char *msg) {
    test_md_paragraph_bytes(runner, markdown, strlen(markdown), expected_text, msg);
}

static void test_crlf_line_ending(test_batch_runner *runner) {
    const char *source = "line1\r\nline2\r\n";
    markdown_core_node *document = markdown_core_parse_document(source, strlen(source));
    OK(runner, child_at(document, 1) == NULL, "document has one paragraph");
    markdown_core_node_free(document);
}

static void test_pathological_parse_completion(test_batch_runner *runner, const char *pattern, size_t repetitions,
                                               const char *msg) {
    size_t pattern_length = strlen(pattern);
    size_t input_length = pattern_length * repetitions;
    char *input = (char *)malloc(input_length);
    markdown_core_node *document;

    OK(runner, input != NULL, "%s (input allocation succeeds)", msg);
    if (!input) {
        return;
    }
    for (size_t i = 0; i < repetitions; ++i) {
        memcpy(input + i * pattern_length, pattern, pattern_length);
    }

    document = markdown_core_parse_document(input, input_length);
    OK(runner, document != NULL, "%s (parse succeeds)", msg);
    markdown_core_node_free(document);
    free(input);
}

static void test_pathological_regressions(test_batch_runner *runner) {
    test_pathological_parse_completion(runner, "[a](b", 50000, "unclosed inline destination");
    test_pathological_parse_completion(runner, "[a](<b", 50000, "unclosed pointy inline destination");
}

/* Parses through the read-only facade and compares the canonical AST dump,
 * which carries every node's scope, byte-for-byte.  This replaces the
 * retired sourcepos XML renderer assertions. */
static void test_facade_dump(test_batch_runner *runner, const char *markdown, const char *expected_dump,
                             const char *msg) {
    markdown_core_document *document;
    uint8_t *dump = NULL;
    size_t dump_length = 0;

    document = facade_parse((const uint8_t *)markdown, strlen(markdown));
    if (!document) {
        OK(runner, 0, "%s (facade parse succeeds)", msg);
        return;
    }
    if (markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)markdown,
                                    strlen(markdown), &dump, &dump_length) != MARKDOWN_CORE_OK) {
        OK(runner, 0, "%s (facade dump succeeds)", msg);
        markdown_core_document_free(document);
        return;
    }
    STR_EQ(runner, (const char *)dump, expected_dump, "%s", msg);
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);
}

// An element that declines to open a block must answer NULL. The parser
// offers each attached element a turn in attach order and stops at the first
// non-NULL answer, so an element that returns the parent container on a
// decline takes away the turn of every element attached after it -- and
// `table` used to do exactly that, on every path including "there is no table
// here". Enabling tables then stopped a directive block from interrupting a
// paragraph.
//
// The parse always uses the dialect's fixed order.
static void element_decline_yields_turn(test_batch_runner *runner) {
    /* Test the decline contract directly, so the fixed attach order cannot
     * hide a table matcher that wrongly returns its unchanged parent. */
    markdown_core_parser parser = {0};
    /* An instance with no record: a declining hook reads none. */
    const markdown_core_element_instance table = {.element = &MARKDOWN_CORE_ELEMENT_TABLE};
    markdown_core_node *candidate = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    markdown_core_strbuf_puts(&candidate->content, "text\n");
    unsigned char line[] = ":::note\n";
    OK(runner, MARKDOWN_CORE_ELEMENT_TABLE.opens->open(&table, 0, &parser, candidate, line, sizeof(line) - 1) == NULL,
       "a non-table line yields no block, independently of attach order");
    markdown_core_node_free(candidate);

    static const char *const markdown = "text\n:::note\nbody\n:::\n";
    markdown_core_node *doc = parse(markdown);
    OK(runner, doc != NULL, "the full dialect parses the element conflict");
    if (!doc) {
        return;
    }

    markdown_core_node *paragraph = child_at(doc, 0);
    markdown_core_node *block = paragraph ? child_at(doc, 1) : NULL;
    INT_EQ(runner, paragraph->kind, MARKDOWN_CORE_NODE_PARAGRAPH, "the lead paragraph survives table declining");
    OK(runner, block != NULL, "the directive interrupts the ordinary paragraph");
    STR_EQ(runner, block ? markdown_core_node_get_type_string(block) : "", "directive_block",
           "a declining table does not swallow the directive block");
    markdown_core_node_free(doc);
}

/* D33. `process_emphasis` used to choose its arm by the delimiter's BYTE:
 *
 *     if (element)                       ... else
 *     if (delim_char == '*' || '_')        ... else
 *     if (delim_char == '\'' || '"')        ...
 *
 * where `element` was "the first attached element whose dispatch set
 * contains this byte". A delimiter matching none of the three left `closer`
 * exactly where it was, fell into the removal below, freed it, and read it
 * again on the next turn -- ASan `heap-use-after-free`, READ of size 8 in
 * `process_emphasis`. With `can_open` set nothing freed it and the loop never
 * ended at all.
 *
 * No in-tree element reaches it, because each pushes a tag it also declares.
 * The public push does not care: it is one call from any element, and the two
 * descriptors below are the two ways to make it. The first pushes a real rule
 * with a NULL owner, so nothing can handle it; the second pushes a rule outside
 * the enum, which would also index `openers_bottom` out of bounds.
 *
 * They are `static const` descriptors, like every element since 3.4. A test
 * may still build one -- what 3.4 removed is the ability to REGISTER one, look
 * one up by name, or mutate one after the fact. */
static markdown_core_node *stray_delimiter_push(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                                unsigned char character, markdown_core_delimiter_rule rule) {
    markdown_core_node *node;

    (void)parser;
    if (character != '@') {
        return NULL;
    }
    markdown_core_inline_state_advance_offset(inline_state);
    node = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    if (!node) {
        return NULL;
    }
    set_literal(node, "@");
    int offset = markdown_core_inline_state_get_offset(inline_state);
    markdown_core_inline_state_place(inline_state, node, offset - 1, offset - 1);
    markdown_core_inline_state_push_delimiter(inline_state, NULL, rule, 0, 1, node);
    return node;
}

static markdown_core_node *stray_unowned_match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node *parent, unsigned char character,
                                               markdown_core_inline_state *inline_state) {
    (void)self;
    (void)parent;
    return stray_delimiter_push(parser, inline_state, character, MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH);
}

static markdown_core_node *stray_unnamed_match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node *parent, unsigned char character,
                                               markdown_core_inline_state *inline_state) {
    (void)self;
    (void)parent;
    return stray_delimiter_push(parser, inline_state, character, (markdown_core_delimiter_rule)200);
}

static const markdown_core_inline_ops STRAY_UNOWNED_INLINES = {
    .match = stray_unowned_match,
    .terminates = "@",
    .dispatch = "@",
};

static const markdown_core_element STRAY_UNOWNED = {
    .name = "stray-unowned",
    .inlines = &STRAY_UNOWNED_INLINES,
};
static const markdown_core_inline_ops STRAY_UNNAMED_INLINES = {
    .match = stray_unnamed_match,
    .terminates = "@",
    .dispatch = "@",
};

static const markdown_core_element STRAY_UNNAMED = {
    .name = "stray-unnamed",
    .inlines = &STRAY_UNNAMED_INLINES,
};

static void stray_delimiter_parse(test_batch_runner *runner, const markdown_core_element *element, const char *what) {
    const char *input = "a @ b @ c\n";
    markdown_core_node *document = parse_with_probes(input, strlen(input), &element, 1);

    OK(runner, document != NULL, "a delimiter with %s still finishes the parse", what);
    markdown_core_node_free(document);
}

static void stray_delimiter(test_batch_runner *runner) {
    stray_delimiter_parse(runner, &STRAY_UNOWNED, "a rule and no owner");
    stray_delimiter_parse(runner, &STRAY_UNNAMED, "a rule outside the enum");
}

typedef struct dispatch_observation {
    char calls[16];
    size_t count;
} dispatch_observation;

static markdown_core_node *observe_dispatch(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                            markdown_core_node *parent, unsigned char character,
                                            markdown_core_inline_state *inline_state) {
    (void)parent;
    (void)character;
    dispatch_observation *observation = parser->context;
    if (observation->count + 1 < sizeof(observation->calls)) {
        observation->calls[observation->count++] = self->element->name[0];
    }
    if (self->element->name[0] == 'c') {
        markdown_core_inline_state_advance_offset(inline_state);
    }
    return NULL;
}

static bool attach_dispatch_observers(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    /* Attach the fallback first, repeat a set member, and include a disjoint
     * scanner. These are grammar/ordering contracts, independent of the index. */
    static const markdown_core_inline_ops fallback_inlines = {
        .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
        .match = observe_dispatch,
        .terminates = "!",
        .dispatch = "!",
    };
    static const markdown_core_element fallback = {
        .name = "fallback-observer",
        .inlines = &fallback_inlines,
    };
    static const markdown_core_inline_ops decline_inlines = {
        .match = observe_dispatch,
        .terminates = "!",
        .dispatch = "!!",
    };
    static const markdown_core_element decline = {
        .name = "decline-observer",
        .inlines = &decline_inlines,
    };
    static const markdown_core_inline_ops consume_inlines = {
        .match = observe_dispatch,
        .terminates = "!",
        .dispatch = "!",
    };
    static const markdown_core_element consume = {
        .name = "consume-observer",
        .inlines = &consume_inlines,
    };
    static const markdown_core_inline_ops disjoint_inlines = {
        .match = observe_dispatch,
        .terminates = "?",
        .dispatch = "?",
    };
    static const markdown_core_element disjoint = {
        .name = "unrelated-observer",
        .inlines = &disjoint_inlines,
    };
    return markdown_core_dialect_builder_attach(builder, &fallback) &&
           markdown_core_dialect_builder_attach(builder, &decline) &&
           markdown_core_dialect_builder_attach(builder, &consume) &&
           markdown_core_dialect_builder_attach(builder, &disjoint);
}

static bool attach_precedence_observers(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    /* Precedences past both ends of the named range, a tie at one of them
     * attached apart, and the named values, all attached out of order. Every
     * owner declines without consuming, so each is asked exactly once. */
    static const markdown_core_inline_ops late_inlines = {
        .precedence = (markdown_core_inline_precedence)2,
        .match = observe_dispatch,
        .terminates = "%",
        .dispatch = "%",
    };
    static const markdown_core_element late = {
        .name = "y-late-observer",
        .inlines = &late_inlines,
    };
    static const markdown_core_inline_ops fallback_inlines = {
        .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
        .match = observe_dispatch,
        .terminates = "%",
        .dispatch = "%",
    };
    static const markdown_core_element fallback = {
        .name = "fallback-observer",
        .inlines = &fallback_inlines,
    };
    static const markdown_core_inline_ops later_inlines = {
        .precedence = (markdown_core_inline_precedence)2,
        .match = observe_dispatch,
        .terminates = "%",
        .dispatch = "%%",
    };
    static const markdown_core_element later = {
        .name = "z-late-observer",
        .inlines = &later_inlines,
    };
    static const markdown_core_inline_ops ordinary_inlines = {
        .match = observe_dispatch,
        .terminates = "%",
        .dispatch = "%",
    };
    static const markdown_core_element ordinary = {
        .name = "default-observer",
        .inlines = &ordinary_inlines,
    };
    static const markdown_core_inline_ops token_inlines = {
        .precedence = MARKDOWN_CORE_INLINE_TOKEN,
        .match = observe_dispatch,
        .terminates = "%",
        .dispatch = "%",
    };
    static const markdown_core_element token = {
        .name = "token-observer",
        .inlines = &token_inlines,
    };
    static const markdown_core_inline_ops early_inlines = {
        .precedence = (markdown_core_inline_precedence)-2,
        .match = observe_dispatch,
        .terminates = "%",
        .dispatch = "%",
    };
    static const markdown_core_element early = {
        .name = "early-observer",
        .inlines = &early_inlines,
    };
    return markdown_core_dialect_builder_attach(builder, &late) &&
           markdown_core_dialect_builder_attach(builder, &fallback) &&
           markdown_core_dialect_builder_attach(builder, &later) &&
           markdown_core_dialect_builder_attach(builder, &ordinary) &&
           markdown_core_dialect_builder_attach(builder, &token) &&
           markdown_core_dialect_builder_attach(builder, &early);
}

/* A declared block-start gate is a PROMISE ABOUT A NEGATIVE: the dispatcher
 * skips the hook for every byte the gate leaves out, so a byte wrongly left out
 * is not a slow parse, it is a construct that silently stops existing. Nothing
 * in the output can show it -- the element simply never runs.
 *
 * So the law is checked against the implementation rather than against a
 * fixture: for every byte the gate excludes, the hook itself must agree by
 * returning NULL. The gate may be wider than the grammar, which only costs a
 * call; it may never be narrower.
 *
 * The parent here is a DOCUMENT, so no `relaxed_containers` bit is live and
 * the byte set alone decides -- a gate that leans on a container kind is
 * checked for the case where that container is absent, which is the case that
 * loses documents. */
static const markdown_core_node_type OBSERVER_ABSENT_KINDS[] = {MARKDOWN_CORE_NODE_CODE_BLOCK, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type OBSERVER_PRESENT_KINDS[] = {MARKDOWN_CORE_NODE_TEXT, MARKDOWN_CORE_NODE_NONE};

/* THE GATE SKIPS A STEP. A step declares the kinds it acts on apart
 * from the kinds it is asked at. The gate reads the former, so a document
 * that produced none of them never asks the step at all -- not at any of its
 * paragraphs -- while a document that produced one asks it at every event it
 * was projected to. Counted, not timed, because skipping is invisible in
 * the output. */
static int step_asked[2];

static markdown_core_finish_result count_step_absent(const markdown_core_element_instance *self,
                                                     markdown_core_parser *parser, markdown_core_node *node,
                                                     markdown_core_node *parent, markdown_core_event_type event,
                                                     void **state) {
    (void)self;
    (void)parser;
    (void)node;
    (void)event;
    (void)parent;
    (void)state;
    step_asked[0]++;
    return MARKDOWN_CORE_FINISH_CONTINUE;
}

static markdown_core_finish_result count_step_present(const markdown_core_element_instance *self,
                                                      markdown_core_parser *parser, markdown_core_node *node,
                                                      markdown_core_node *parent, markdown_core_event_type event,
                                                      void **state) {
    (void)self;
    (void)parser;
    (void)node;
    (void)event;
    (void)parent;
    (void)state;
    step_asked[1]++;
    return MARKDOWN_CORE_FINISH_CONTINUE;
}

static const markdown_core_node_type STEP_AT_PARAGRAPH[] = {MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_NONE};

static bool attach_step_observers(markdown_core_dialect_builder *builder, void *context) {
    /* Both steps are asked at a Paragraph's EXIT; one acts on a kind this
     * document cannot contain, the other on the Text every paragraph holds. */
    static const markdown_core_finish_ops absent_finish = {
        .step = count_step_absent,
        .acts = OBSERVER_ABSENT_KINDS,
        .exits = STEP_AT_PARAGRAPH,
    };
    static const markdown_core_element absent = {
        .name = "step-absent-observer",
        .finish = &absent_finish,
    };
    static const markdown_core_finish_ops present_finish = {
        .step = count_step_present,
        .acts = OBSERVER_PRESENT_KINDS,
        .exits = STEP_AT_PARAGRAPH,
    };
    static const markdown_core_element present = {
        .name = "step-present-observer",
        .finish = &present_finish,
    };
    (void)context;
    return markdown_core_dialect_builder_attach(builder, &absent) &&
           markdown_core_dialect_builder_attach(builder, &present);
}

static void finish_step_skips_absent_kinds(test_batch_runner *runner) {
    static const char source[] = "just some prose\n\nand a second paragraph\n";
    markdown_core_node *doc;

    step_asked[0] = 0;
    step_asked[1] = 0;
    doc = markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_step_observers, NULL);
    OK(runner, doc != NULL, "a document with observing finish steps parses");
    INT_EQ(runner, step_asked[0], 0, "a step whose acted-on kinds never occurred is asked at no paragraph");
    INT_EQ(runner, step_asked[1], 2, "a step whose acted-on kind occurred is asked at each paragraph's EXIT once");
    markdown_core_node_free(doc);
}

/* THE GATE IS READ AT THE EVENT. Completion parses each block's inline
 * content as it reaches the block, so a kind the step acts on may first be
 * made after completion began; the gate opens when it is. A step asked at a
 * Paragraph's EXIT that acts on inline Code is asked at the paragraph that
 * holds the first Code and at every paragraph after it, and not at the
 * paragraphs before it -- where no Code existed and the step could have
 * found none. A scope kind's ENTER and EXIT are delivered whatever the gate
 * says, so the extent the step tracks is always in step with the tree. */
static const markdown_core_node_type OBSERVER_LATE_KINDS[] = {MARKDOWN_CORE_NODE_CODE, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type SCOPE_AT_LINK[] = {MARKDOWN_CORE_NODE_LINK, MARKDOWN_CORE_NODE_NONE};

static bool attach_late_step_observers(markdown_core_dialect_builder *builder, void *context) {
    static const markdown_core_finish_ops late_finish = {
        .step = count_step_absent,
        .acts = OBSERVER_LATE_KINDS,
        .exits = STEP_AT_PARAGRAPH,
    };
    static const markdown_core_element late = {
        .name = "step-late-observer",
        .finish = &late_finish,
    };
    static const markdown_core_finish_ops scoped_finish = {
        .step = count_step_present,
        .acts = OBSERVER_ABSENT_KINDS,
        .scopes = SCOPE_AT_LINK,
    };
    static const markdown_core_element scoped = {
        .name = "step-scoped-observer",
        .finish = &scoped_finish,
    };
    (void)context;
    return markdown_core_dialect_builder_attach(builder, &late) &&
           markdown_core_dialect_builder_attach(builder, &scoped);
}

static void finish_step_gate_opens_when_its_kind_is_made(test_batch_runner *runner) {
    static const char source[] = "prose first\n\nthen `code` in [a link](u)\n\nand prose again\n";
    markdown_core_node *doc;

    step_asked[0] = 0;
    step_asked[1] = 0;
    doc = markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_late_step_observers, NULL);
    OK(runner, doc != NULL, "a document whose acted-on kind appears in its second paragraph parses");
    INT_EQ(runner, step_asked[0], 2,
           "a step acting on a kind first made by the walk is asked from that paragraph's EXIT on: not at the "
           "first paragraph, at the second and the third");
    INT_EQ(runner, step_asked[1], 2,
           "a step tracking a Link's extent receives the Link's ENTER and EXIT whatever its gate says");
    markdown_core_node_free(doc);
}

/* A declared kind set is written with a macro, because a descriptor cannot call
 * a function, and the macro cannot tell a block kind from an inline one -- the
 * field it fills says which. So check every set the dialect declares against
 * the runtime helpers, which can: a Formula bit parked in `.blocks` would make
 * the gate skip a step in documents that do contain formulas. */
typedef struct {
    size_t gated;
    size_t violations;
    int first_bad_byte;
    const char *first_bad_element;
} gate_sweep;

/* Sweep the dialect the engine itself selected and sealed, from inside the
 * instance -- a document owner's lifecycle begins it -- rather than selecting
 * a second copy of the dialect: only the parse transaction may do that, and
 * audit-element-attach-order holds the line. The probe parser each hook is
 * asked with reads that same sealed dialect. */
static void sweep_block_gates(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->init(self, parser);
    gate_sweep *sweep = parser->context;
    const markdown_core_element_instance *instances = parser->dialect->instances;
    size_t element_count = parser->dialect->element_count;

    /* A byte the gate does not admit must be one the hook rejects, whatever
     * container it is asked in and whether or not a paragraph is open: a scan
     * hook is asked from inside a definition too (definition_list's), and a
     * setext underline is a heading only under a paragraph. A blank line, which
     * no gate can name, is asked of no gated scan hook, so each must reject it
     * as well. Line endings are left out of the byte sweep -- a line made of
     * them is not a line -- and covered by the blank probe. */
    static const markdown_core_node_type containers[] = {MARKDOWN_CORE_NODE_DOCUMENT, MARKDOWN_CORE_NODE_DEFINITION};
    for (size_t i = 0; i < element_count; i++) {
        const markdown_core_element_instance *instance = &instances[i];
        static const markdown_core_open_ops none = {0};
        const markdown_core_open_ops *opens = instance->element->opens ? instance->element->opens : &none;
        const struct {
            const char *family;
            const char *bytes;
        } gates[] = {{"open", opens->open ? opens->open_needs.bytes : NULL},
                     {"scan", opens->scan ? opens->scan_needs.bytes : NULL},
                     {"interrupt", opens->interrupt ? opens->interrupt_needs.bytes : NULL}};
        for (size_t g = 0; g < sizeof(gates) / sizeof(*gates); g++) {
            if (!gates[g].bytes) {
                continue;
            }
            sweep->gated++;
            for (int byte = 0; byte < 256; byte++) {
                unsigned char line[8];
                bool blank = byte == '\n';
                if (byte == '\r' || byte == 0 || (!blank && strchr(gates[g].bytes, byte))) {
                    continue;
                }
                line[0] = (unsigned char)byte;
                line[1] = blank ? 0 : (unsigned char)byte;
                line[2] = blank ? 0 : (unsigned char)byte;
                line[3] = '\n';
                for (size_t c = 0; c < sizeof(containers) / sizeof(*containers); c++) {
                    for (int paragraph = 0; paragraph < 2; paragraph++) {
                        /* The instance's dialect, whose instances hold the element state. */
                        markdown_core_parser probe = {.dialect = parser->dialect};
                        markdown_core_node *parent = markdown_core_node_new(containers[c]);
                        markdown_core_chunk input = {line, blank ? 1 : 4, 0};
                        bool claimed = false;
                        if (!parent) {
                            continue;
                        }
                        if (gates[g].family[0] == 'o') {
                            claimed = opens->open(instance, 0, &probe, parent, line, input.len) != NULL;
                        } else if (gates[g].family[0] == 's') {
                            block_start_context context = {.container = parent,
                                                           .input = &input,
                                                           .first = 0,
                                                           .column = 1,
                                                           .indent = 0,
                                                           .paragraph = paragraph != 0,
                                                           .lazy = false,
                                                           .all_matched = true,
                                                           .depth = 1};
                            block_start start = {0};
                            claimed = opens->scan(instance, &probe, &context, &start);
                        } else {
                            claimed = opens->interrupt(instance, &probe, parent, &input, false) != NULL;
                        }
                        if (claimed) {
                            sweep->violations++;
                            if (sweep->first_bad_byte < 0) {
                                sweep->first_bad_byte = byte;
                                sweep->first_bad_element = instance->element->name;
                            }
                        }
                        markdown_core_node_free(parent);
                    }
                }
            }
        }
    }
}

static bool begin_with_block_gate_sweep(markdown_core_dialect_builder *builder, void *context) {
    static document_owner document;
    (void)context;
    return attach_document_owner(builder, &document, "block-gate-sweeper", sweep_block_gates);
}

typedef struct {
    size_t declared;
    size_t malformed;
    size_t without_hook;
} kind_set_sweep;

static bool sweep_finish_kind_sets(markdown_core_dialect_builder *builder, void *context) {
    kind_set_sweep *sweep = context;
    size_t element_count;
    const markdown_core_element *const *elements = markdown_core_dialect_builder_elements(builder, &element_count);

    for (size_t i = 0; i < element_count; i++) {
        const markdown_core_finish_ops *finish = elements[i]->finish;
        if (!finish || (!finish->acts && !finish->exits && !finish->scopes)) {
            continue;
        }
        /* All of a step's lists -- the kinds it acts on, the kinds it is
         * asked at and the kinds whose extent it tracks -- are declarations
         * of kinds and obey the law. */
        const markdown_core_node_type *lists[] = {finish->acts, finish->exits, finish->scopes};
        sweep->declared++;
        if (!finish->step) {
            sweep->without_hook++;
        }
        for (size_t list = 0; list < sizeof(lists) / sizeof(*lists); list++) {
            for (const markdown_core_node_type *kind = lists[list]; kind && *kind; kind++) {
                /* Exactly one namespace must claim the kind. This is checkable
                 * only because the declaration keeps the KIND: block and
                 * inline values collide once masked -- masked 12 is a TableRow
                 * and an Embedded alike -- so a bit set reconstructed from a
                 * stored mask would agree with whichever namespace the reader
                 * already assumed, and a Formula written where a block kind
                 * belongs would pass. */
                uint32_t as_block = markdown_core_node_block_kind_bit(*kind);
                uint32_t as_inline = markdown_core_node_inline_kind_bit(*kind);
                markdown_core_node_kind_set set = {0, 0};

                if ((as_block == 0) == (as_inline == 0)) {
                    sweep->malformed++;
                    continue;
                }
                markdown_core_node_kind_set_add(&set, *kind);
                if (set.blocks != as_block || set.inlines != as_inline) {
                    sweep->malformed++;
                }
            }
        }
    }
    return true;
}

/* A step declares the kinds it acts on, and the engine skips it where a
 * document has produced none of them. A kind written into the wrong namespace
 * would skip the documents that DO feed the step, silently, so the
 * declarations are checked for well-formedness -- and separately, above, for
 * actually taking effect. */
static void finish_kind_sets_are_well_formed(test_batch_runner *runner) {
    kind_set_sweep sweep = {0, 0, 0};
    static const char probe_source[] = "probe\n";
    markdown_core_node *probe_doc =
        markdown_core_parse_document_with_setup(probe_source, sizeof(probe_source) - 1, sweep_finish_kind_sets, &sweep);

    OK(runner, sweep.declared > 0, "at least one finish step declares its kinds for this law to bind");
    INT_EQ(runner, (int)sweep.without_hook, 0, "only a step declares the kinds it acts on");
    INT_EQ(runner, (int)sweep.malformed, 0, "every declared finish kind belongs to exactly one namespace");
    markdown_core_node_free(probe_doc);
}

static void block_gate_admits_every_opener(test_batch_runner *runner) {
    gate_sweep sweep = {0, 0, -1, NULL};
    static const char probe_source[] = "probe\n";
    markdown_core_node *probe_doc = markdown_core_parse_document_with_setup(probe_source, sizeof(probe_source) - 1,
                                                                            begin_with_block_gate_sweep, &sweep);
    size_t gated = sweep.gated, violations = sweep.violations;
    int first_bad_byte = sweep.first_bad_byte;
    const char *first_bad_element = sweep.first_bad_element;
    markdown_core_node_free(probe_doc);

    OK(runner, gated >= 12, "every scan and interrupt hook, and the gated openers, declare a gate: gated=%zu", gated);
    OK(runner, violations == 0, "no gated opener claims a line its gate excludes");
    if (violations) {
        fprintf(stderr, "element %s opened a block on byte 0x%02x, which its gate excludes\n",
                first_bad_element ? first_bad_element : "?", (unsigned)first_bad_byte);
    }

    /* The sweep above offers ONE line, so it can only bind a grammar that one
     * line decides. A block whose opener needs a LATER line is invisible to
     * it -- and that is not hypothetical: a pipe table is introduced by its
     * header row, whose first bytes are arbitrary prose, and the delimiter row
     * that makes it a table is on the next line. Gating such an opener on the
     * bytes of its first line deletes the construct while every single-line
     * probe still passes. Pin the shape here so the sweep is never mistaken
     * for the whole law. */
    static const char *const pipe_table = "Right | Left\n--- | ---\n12 | 12\n";
    markdown_core_node *doc = parse(pipe_table);
    OK(runner, doc != NULL, "a pipe table with a prose header parses");
    if (doc) {
        markdown_core_node *first = child_at(doc, 0);
        STR_EQ(runner, first ? markdown_core_node_get_type_string(first) : "", "table",
               "a block opened by a later line survives first-line gating");
        markdown_core_node_free(doc);
    }
}

static void inline_dispatch_ownership(test_batch_runner *runner) {
    const char source[] = "`!` ! tail";
    dispatch_observation observation = {0};
    markdown_core_node *root =
        markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_dispatch_observers, &observation);
    OK(runner, root != NULL, "overlapping inline owners complete the parse");
    STR_EQ(runner, observation.calls, "dc",
           "each matching owner runs once in precedence order; consuming NULL commits the token");
    if (root) {
        markdown_core_node *code = child_at(child_at(root, 0), 0);
        INT_EQ(runner, code->kind, MARKDOWN_CORE_NODE_CODE, "protected tokens retain their contents");
        LITERAL_EQ(runner, code, "!", "dispatch does not inspect an opaque token body");
        LITERAL_EQ(runner, child_at(child_at(root, 0), 1), "  tail", "consumed input is not offered to fallbacks");
    }
    markdown_core_node_free(root);
}

/* The dispatch order is a function of every precedence value a descriptor
 * can hold, not only the three named ones: an owner outside that range is
 * asked in its place, and never leaves its reserved slot empty. */
static void inline_dispatch_orders_every_precedence(test_batch_runner *runner) {
    const char source[] = "a % b\n";
    dispatch_observation observation = {0};
    markdown_core_node *root =
        markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_precedence_observers, &observation);
    OK(runner, root != NULL, "owners at unnamed precedences complete the parse");
    STR_EQ(runner, observation.calls, "etdfyz", "owners are asked in ascending precedence, ties in descriptor order");
    if (root) {
        LITERAL_EQ(runner, child_at(child_at(root, 0), 0), "a % b", "a byte every owner declines stays text");
    }
    markdown_core_node_free(root);
}

/* A1. An allocation failure is a fact about the write that failed, not a
 * property the buffer keeps. `markdown_core_strbuf_clear` used not to lift
 * `oom`, and `markdown_core_strbuf_detach` was the only operation in the engine
 * that did -- so a buffer cleared and reused went on silently dropping every
 * later write WITH THE ALLOCATOR WORKING AGAIN.
 *
 * This is a property test rather than a parse test on purpose. Measured at
 * 3a.3: reverting the lift alone leaves `correctness` at 69/69 and both
 * allocation-failure sweeps green, because the parser's reusable buffers
 * report at the transaction and abandon the parse before the reuse. The lift
 * removes the class by construction, and nothing else can see it. */

static void strbuf_growth_preserves_termination(test_batch_runner *runner) {
    const bufsize_t initial[] = {0, 1, 32};
    for (size_t i = 0; i < sizeof(initial) / sizeof(*initial); i++) {
        payload_probe_arm();
        payload_fill_fresh = 1;
        markdown_core_strbuf buf;
        markdown_core_strbuf_init(&buf, initial[i]);
        OK(runner, !buf.oom && !buf.size && buf.ptr[0] == 0,
           "initialization itself establishes a terminated empty string");
        markdown_core_strbuf_grow(&buf, 64);
        OK(runner, !buf.oom && buf.ptr[0] == 0 && buf.size == 0,
           "fresh nonzero-filled storage is an empty terminated string");
        markdown_core_strbuf_puts(&buf, "value");
        markdown_core_strbuf_grow(&buf, 256);
        STR_EQ(runner, (char *)buf.ptr, "value", "nonempty growth preserves bytes and terminator");
        unsigned char *before = buf.ptr;
        bufsize_t capacity = buf.asize;
        strbuf_refuse_next = 1;
        markdown_core_strbuf_grow(&buf, 1024);
        OK(runner, buf.oom && buf.ptr == before && buf.asize == capacity,
           "failed growth preserves storage and reports loss");
        STR_EQ(runner, (char *)buf.ptr, "value", "failed growth preserves the terminated value");
        markdown_core_strbuf_clear(&buf);
        markdown_core_strbuf_grow(&buf, 1024);
        OK(runner, !buf.oom && buf.ptr[buf.size] == 0, "clear and empty owned growth remain terminated");
        unsigned char *empty = markdown_core_strbuf_detach(&buf);
        OK(runner, empty && empty[0] == 0, "detaching the empty owned buffer returns an empty string");
        markdown_core_free(empty);
        empty = markdown_core_strbuf_detach(&buf);
        OK(runner, empty && empty[0] == 0, "detaching the sentinel returns an owned empty string");
        markdown_core_free(empty);
        markdown_core_strbuf_free(&buf);
        payload_fill_fresh = 0;
        INT_EQ(runner, payload_live, 0, "every buffer allocation is released");
        payload_probe_disarm();
    }
}

/* A literal an edit leaves pending (node.h, A LITERAL, LAZY) joins on its
 * first read; a read whose join is refused storage writes none of its
 * answers, and the next read joins it. */
static void lazy_literal_failure_writes_nothing(test_batch_runner *runner) {
    static char text[400 * 9 + 16];
    size_t size = (size_t)sprintf(text, "```\n");
    for (int i = 0; i < 400; i++) {
        size += (size_t)sprintf(text + size, "line %03d\n", i);
    }
    markdown_core_session *session;
    const markdown_core_document *document;
    EXPECT_OK(markdown_core_session_new((const uint8_t *)text, size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session));
    EXPECT_OK(markdown_core_session_flush(session, &document));
    const markdown_core_position at = {200, 8}, end = {200, 9};
    EXPECT_OK(markdown_core_session_edit(session, at, end, (const uint8_t *)"x", 1, &document));
    const markdown_core_node *code = markdown_core_node_children(markdown_core_document_root(document)).items[0];
    const markdown_core_string kept = {(const uint8_t *)"kept", 4};
    const markdown_core_optional_string none = {true, kept};
    markdown_core_optional_string info = none, language = none;
    markdown_core_string literal = kept;
    bool fenced = false, closed = false;
    strbuf_refuse_next = 1;
    INT_EQ(runner, markdown_core_node_code_block_properties(code, &info, &language, &literal, &fenced, &closed),
           MARKDOWN_CORE_ALLOCATION_FAILED, "a refused join of a pending literal fails the read");
    OK(runner,
       literal.data == kept.data && info.value.data == kept.data && language.value.data == kept.data && !fenced &&
           !closed,
       "a failed read writes none of its answers");
    EXPECT_OK(markdown_core_node_code_block_properties(code, &info, &language, &literal, &fenced, &closed));
    OK(runner, literal.length == 400 * 9 && !memcmp(literal.data + 198 * 9, "line 19x\n", 9),
       "the next read joins the literal as the edited text has it");
    markdown_core_session_close(session);
}

/* An empty source anchors at its position (4.4): an indented code block
 * whose one line holds only a stray continuation byte has none of it, and
 * the fence an edit opens over its position continues it. */
static void void_source_continues(test_batch_runner *runner) {
    static const char text[] = "```\t`\n```\n```\n    \xa7\n`";
    markdown_core_session *session;
    const markdown_core_document *document;
    EXPECT_OK(
        markdown_core_session_new((const uint8_t *)text, sizeof(text) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session));
    EXPECT_OK(markdown_core_session_flush(session, &document));
    markdown_core_nodes children = markdown_core_node_children(markdown_core_document_root(document));
    const markdown_core_extent empty =
        children.count == 4 ? markdown_core_node_extent(children.items[2]) : (markdown_core_extent){0, 1};
    const uint64_t id = children.count == 4 ? markdown_core_node_id(children.items[2]) : 0;
    OK(runner, empty.span == 0, "the stray byte's code block has an empty source");
    const markdown_core_position at = {1, 4}, end = {1, 6};
    EXPECT_OK(markdown_core_session_edit(session, at, end, NULL, 0, &document));
    children = markdown_core_node_children(markdown_core_document_root(document));
    OK(runner, children.count == 2 && markdown_core_node_id(children.items[1]) == id,
       "the fence over the empty source's position continues it");
    markdown_core_session_close(session);
}

static void strbuf_failure_is_a_transaction(test_batch_runner *runner) {
    markdown_core_strbuf buf;

    markdown_core_strbuf_init(&buf, 0);
    strbuf_refuse_next = 1;
    markdown_core_strbuf_put(&buf, (const unsigned char *)"hello", 5);
    INT_EQ(runner, buf.oom, 1, "a refused growth poisons the buffer");
    INT_EQ(runner, buf.size, 0, "a refused growth writes nothing");

    markdown_core_strbuf_clear(&buf);
    INT_EQ(runner, buf.oom, 0, "clearing the buffer lifts the failure with the content it described");

    markdown_core_strbuf_put(&buf, (const unsigned char *)"world", 5);
    INT_EQ(runner, buf.oom, 0, "the next write succeeds with the allocator working again");
    INT_EQ(runner, buf.size, 5, "the next write lands");
    STR_EQ(runner, markdown_core_strbuf_cstr(&buf), "world", "and it lands intact");

    markdown_core_strbuf_free(&buf);
}

/* A4. `bufsize_t` is int32_t, and every append went through
 * `markdown_core_strbuf_grow(buf, buf->size + add)`. Two things were wrong and
 * either alone is enough:
 *
 *   the sum is undefined behaviour past INT32_MAX, and wraps NEGATIVE;
 *   `grow` answered a negative target with "already big enough" -- silently,
 *   because its `assert(target_size > 0)` compiles out under NDEBUG.
 *
 * `put` then memmoved `add` bytes into a buffer that had not grown. Measured
 * before the fix, by direct call: SIGSEGV, status 139, writing 1,073,741,833
 * bytes into an eight-byte allocation.
 *
 * The forged `size` below is the largest a legitimate buffer may hold -- the
 * cap is INT32_MAX/2 -- so this is the state the normalized-line scratch can
 * reach after an embedded NUL in a 1.07 GiB line, and the put is the remaining
 * segment. It is forged because constructing that source costs 2 GiB. */
static void strbuf_overflow(test_batch_runner *runner) {
    markdown_core_strbuf buf;
    unsigned char data[16] = {0};

    markdown_core_strbuf_init(&buf, 0);
    markdown_core_strbuf_grow(&buf, -1);
    INT_EQ(runner, buf.oom, 1, "a negative grow target poisons the buffer");
    INT_EQ(runner, buf.asize, 0, "a negative grow target allocates nothing");
    markdown_core_strbuf_free(&buf);

    markdown_core_strbuf_init(&buf, 0);
    markdown_core_strbuf_grow(&buf, 0);
    INT_EQ(runner, buf.oom, 1, "a zero grow target poisons the buffer");
    markdown_core_strbuf_free(&buf);

    /* The overflow itself. Without the fix this line does not return. */
    markdown_core_strbuf_init(&buf, 8);
    buf.size = (bufsize_t)(INT32_MAX / 2);
    markdown_core_strbuf_put(&buf, data, (bufsize_t)(INT32_MAX / 2) + 10);
    INT_EQ(runner, buf.oom, 1, "an append whose length overflows the size sum poisons instead of writing");
    INT_EQ(runner, buf.size, (bufsize_t)(INT32_MAX / 2), "the refused append moves nothing");
    buf.size = 0;
    markdown_core_strbuf_free(&buf);

    /* And the guard is not over-tight: an ordinary large append still works. */
    markdown_core_strbuf_init(&buf, 0);
    for (int i = 0; i < 4096; i++) {
        markdown_core_strbuf_put(&buf, data, (bufsize_t)sizeof(data));
    }
    INT_EQ(runner, buf.oom, 0, "4096 ordinary appends do not poison");
    INT_EQ(runner, buf.size, 4096 * (bufsize_t)sizeof(data), "4096 ordinary appends all landed");
    markdown_core_strbuf_free(&buf);
}

static void source_pos(test_batch_runner *runner) {
    static const char markdown[] = "# Hi *there*.\n"
                                   "\n"
                                   "Hello &ldquo; <http://www.google.com>\n"
                                   "there `hi` -- [okay](www.google.com (ok)).\n"
                                   "\n"
                                   "> 1. Okay.\n"
                                   ">    Sure.\n"
                                   ">\n"
                                   "> 2. Yes, okay.\n"
                                   ">    ![ok](hi \"yes\")\n";

    test_facade_dump(
        runner, markdown,
        "Document scope=1:1..10:20 anchor=null attributes={} children=3\n"
        "├── Heading scope=1:1..1:13 anchor=\"hi-there\" attributes={} level=1 children=3\n"
        "│   ├── Text scope=1:3..1:5 anchor=null attributes={} literal=\"Hi \" children=0\n"
        "│   ├── Emphasis scope=1:6..1:12 anchor=null attributes={} children=1\n"
        "│   │   └── Text scope=1:7..1:11 anchor=null attributes={} literal=\"there\" children=0\n"
        "│   └── Text scope=1:13..1:13 anchor=null attributes={} literal=\".\" children=0\n"
        "├── Paragraph scope=3:1..4:42 anchor=null attributes={} children=8\n"
        "│   ├── Text scope=3:1..3:14 anchor=null attributes={} literal=\"Hello “ \" children=0\n"
        "│   ├── Link scope=3:15..3:37 anchor=null attributes={} dest=url(\"http://www.google.com\") "
        "title=null children=1\n"
        "│   │   └── Text scope=3:16..3:36 anchor=null attributes={} literal=\"http://www.google.com\" "
        "children=0\n"
        "│   ├── SoftBreak scope=3:38..4:0 anchor=null attributes={} children=0\n"
        "│   ├── Text scope=4:1..4:6 anchor=null attributes={} literal=\"there \" children=0\n"
        "│   ├── Code scope=4:7..4:10 anchor=null attributes={} literal=\"hi\" children=0\n"
        "│   ├── Text scope=4:11..4:14 anchor=null attributes={} literal=\" -- \" children=0\n"
        "│   ├── Link scope=4:15..4:41 anchor=null attributes={} dest=url(\"www.google.com\") title=\"ok\" "
        "children=1\n"
        "│   │   └── Text scope=4:16..4:19 anchor=null attributes={} literal=\"okay\" children=0\n"
        "│   └── Text scope=4:42..4:42 anchor=null attributes={} literal=\".\" children=0\n"
        "└── Callout scope=6:1..10:20 anchor=null attributes={} variant=null collapsed=null children=1\n"
        "    └── List scope=6:3..10:20 anchor=null attributes={} flavor=ordered start=1 variant=decimal "
        "delimiter=period tight=false children=2\n"
        "        ├── ListItem scope=6:3..8:1 anchor=null attributes={} marker=null children=1\n"
        "        │   └── Paragraph scope=6:6..7:0,7:6..7:10 anchor=null attributes={} children=3\n"
        "        │       ├── Text scope=6:6..6:10 anchor=null attributes={} literal=\"Okay.\" children=0\n"
        "        │       ├── SoftBreak scope=6:11..7:0 anchor=null attributes={} children=0\n"
        "        │       └── Text scope=7:6..7:10 anchor=null attributes={} literal=\"Sure.\" children=0\n"
        "        └── ListItem scope=9:3..10:20 anchor=null attributes={} marker=null children=1\n"
        "            └── Paragraph scope=9:6..10:0,10:6..10:20 anchor=null attributes={} children=3\n"
        "                ├── Text scope=9:6..9:15 anchor=null attributes={} literal=\"Yes, okay.\" children=0\n"
        "                ├── SoftBreak scope=9:16..10:0 anchor=null attributes={} children=0\n"
        "                └── Embedded scope=10:6..10:20 anchor=null attributes={} dest=url(\"hi\") title=\"yes\" "
        "dimensions=null "
        "children=1\n"
        "                    └── Text scope=10:8..10:9 anchor=null attributes={} literal=\"ok\" children=0\n",
        "scopes are as expected");
}

static void source_pos_inlines(test_batch_runner *runner) {
    test_facade_dump(runner,
                     "*first*\n"
                     "second\n",
                     "Document scope=1:1..2:6 anchor=null attributes={} children=1\n"
                     "└── Paragraph scope=1:1..2:6 anchor=null attributes={} children=3\n"
                     "    ├── Emphasis scope=1:1..1:7 anchor=null attributes={} children=1\n"
                     "    │   └── Text scope=1:2..1:6 anchor=null attributes={} literal=\"first\" children=0\n"
                     "    ├── SoftBreak scope=1:8..2:0 anchor=null attributes={} children=0\n"
                     "    └── Text scope=2:1..2:6 anchor=null attributes={} literal=\"second\" children=0\n",
                     "closed emphasis scopes are as expected");
    test_facade_dump(runner,
                     "*first\n"
                     "second*\n",
                     "Document scope=1:1..2:7 anchor=null attributes={} children=1\n"
                     "└── Paragraph scope=1:1..2:7 anchor=null attributes={} children=1\n"
                     "    └── Emphasis scope=1:1..2:7 anchor=null attributes={} children=3\n"
                     "        ├── Text scope=1:2..1:6 anchor=null attributes={} literal=\"first\" children=0\n"
                     "        ├── SoftBreak scope=1:7..2:0 anchor=null attributes={} children=0\n"
                     "        └── Text scope=2:1..2:6 anchor=null attributes={} literal=\"second\" children=0\n",
                     "multiline emphasis scopes are as expected");
}

/* THE CITATION MODEL's values (M4). A defined call is a one-item `Cite`
 * whose `Citation` names the footnote by the normalized label without its
 * caret and carries empty affixes; every definition, a losing duplicate and
 * an unreferenced one included, is a `Footnote` block where it was written,
 * and the document lists them in source order; and every value accessor
 * refuses a node that is not its owner and a value that is missing. */
static void citation_and_footnote_values(test_batch_runner *runner) {
    static const char markdown[] = "[^Note] [^b] [c](/inline)\n"
                                   "\n"
                                   "[^note]: first\n"
                                   "\n"
                                   "[^note]: second\n"
                                   "\n"
                                   "[^b]: kept\n"
                                   "\n"
                                   "[^unused]: still here\n";
    static const char *const labels[] = {"note", "note", "b", "unused"};
    static const int32_t lines[] = {3, 5, 7, 9};
    const size_t length = sizeof(markdown) - 1;
    markdown_core_document *document;
    const markdown_core_node *root;
    const markdown_core_node *paragraph;
    const markdown_core_node *cite;
    const markdown_core_node *item;
    const markdown_core_node *footnote;
    markdown_core_referent referent;
    markdown_core_scope scope;
    markdown_core_optional_string label;
    size_t count = 0;

    document = facade_parse((const uint8_t *)markdown, length);
    if (!document) {
        OK(runner, 0, "citation corpus parses");
        return;
    }
    root = markdown_core_document_root(document);
    INT_EQ(runner, (int)markdown_core_node_children(root).count, 5, "every definition is a block where it was written");
    paragraph = facade_child(root, 0);
    cite = facade_child(paragraph, 0);
    INT_EQ(runner, markdown_core_node_get_kind(cite), MARKDOWN_CORE_KIND_CITE, "a defined call is a Cite");
    INT_EQ(runner, (int)markdown_core_node_children(cite).count, 0, "a cite has no children");
    markdown_core_nodes citations = facade_nodes(cite, markdown_core_node_cite_citations);
    item = citations.count ? citations.items[0] : NULL;
    OK(runner, item != NULL, "a cite holds an item");
    OK(runner, citations.count == 1, "an inherited call holds exactly one item");
    referent = facade_referent(item);
    INT_EQ(runner, referent.kind, MARKDOWN_CORE_REFERENT_FOOTNOTE, "an inherited call names a footnote");
    OK(runner, referent.label.length == 4 && memcmp(referent.label.data, "note", 4) == 0 && referent.note == NULL,
       "the label is the normalized label without the caret");
    OK(runner, referent.key.length == 0 && referent.key.data == NULL && referent.mode == 0,
       "the footnote branch zeroes the bib fields");
    OK(runner,
       facade_nodes(item, markdown_core_citation_prefix).count == 0 &&
           facade_nodes(item, markdown_core_citation_suffix).count == 0,
       "an inherited call has empty affixes");
    OK(runner, ((scope = document_scope_of(document, cite, markdown)).start.line > 0), "the cite has a scope");
    OK(runner, scope.start.line == 1 && scope.start.column == 1 && scope.end.line == 1 && scope.end.column == 7,
       "the cite covers the brackets");
    OK(runner, ((scope = document_scope_of(document, item, markdown)).start.line > 0), "the item has a scope");
    OK(runner, scope.start.line == 1 && scope.start.column == 2 && scope.end.line == 1 && scope.end.column == 6,
       "the item covers the caret and the label");

    INT_EQ(runner, (int)markdown_core_document_footnote_count(document), 4,
           "the winner, its duplicate, the referenced, and the unreferenced definitions are footnotes");
    for (size_t at = 1; (footnote = facade_child(root, at)); at++) {
        OK(runner, count < 4 && facade_footnote_at(document, count) == footnote,
           "the document lists footnote %zu in source order", count);
        label = facade_footnote_label(footnote);
        OK(runner,
           count < 4 && label.has_value && label.value.length == strlen(labels[count]) &&
               memcmp(label.value.data, labels[count], label.value.length) == 0,
           "footnote %zu carries the expected label", count);
        OK(runner,
           count < 4 && ((scope = document_scope_of(document, footnote, markdown)).start.line > 0) &&
               scope.start.line == lines[count] && scope.start.column == 1,
           "footnote %zu starts where it was written", count);
        count++;
    }
    INT_EQ(runner, (int)count, 4, "four footnotes follow the paragraph");
    footnote = facade_child(root, 1);
    OK(runner,
       markdown_core_document_footnote_for(document, referent.label) == footnote &&
           markdown_core_document_footnote_for(document, (markdown_core_string){(const uint8_t *)"missing", 7}) == NULL,
       "a label finds the first definition in source order, and no other");
    OK(runner,
       facade_nodes(footnote, markdown_core_footnote_content).count != 0 &&
           markdown_core_node_get_kind(facade_nodes(footnote, markdown_core_footnote_content).items[0]) ==
               MARKDOWN_CORE_KIND_PARAGRAPH,
       "a footnote's content is its block content");
    markdown_core_document_free(document);
}

static void link_resource_lifecycle(test_batch_runner *runner) {
    /* M2: a direct link, image or autolink states its destination and title
     * in a resource the parser creates; a reference occurrence names its
     * definition by label and has none. Nothing else writes one: the
     * engine's url and title setters left with the model. A node built by
     * hand, or converted into a link, has no resource rather than reading
     * another arm's bytes as a resource pointer. */
    markdown_core_node *paragraph = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    markdown_core_node *link = markdown_core_node_new(MARKDOWN_CORE_NODE_LINK);
    markdown_core_node *image = markdown_core_node_new(MARKDOWN_CORE_NODE_EMBEDDED);
    markdown_core_node *converted = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);

    OK(runner, append_child(paragraph, link), "hand-built link joins a paragraph");
    OK(runner, append_child(paragraph, image), "hand-built image joins a paragraph");
    OK(runner, append_child(paragraph, converted), "text joins a paragraph");

    OK(runner, link->as.link->resource == NULL, "a hand-built link has no resource");
    OK(runner, image->as.link->resource == NULL, "a hand-built image has no resource");

    OK(runner, set_literal(converted, "~~"), "the text to convert has a literal");
    INT_EQ(runner, markdown_core_node_set_kind(converted, paragraph, MARKDOWN_CORE_NODE_LINK),
           MARKDOWN_CORE_NODE_SET_KIND_OK, "set_kind converts text into a link");
    OK(runner, converted->as.link->resource == NULL, "a converted link starts without a resource");
    INT_EQ(runner, markdown_core_node_set_kind(converted, paragraph, MARKDOWN_CORE_NODE_TEXT),
           MARKDOWN_CORE_NODE_SET_KIND_OK, "set_kind converts the link back");
    LITERAL_EQ(runner, converted, "", "converting back starts the literal empty");

    markdown_core_node_free(paragraph);

    /* An occurrence of a definition names it by label and states no title;
     * the Reference states the destination and title, and a direct link
     * with the same bytes states its own. */
    static const char markdown[] = "[a]: /shared \"t\"\n\n[A] [a] [d](/shared \"t\")\n";
    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1);
    markdown_core_node *reference = child_at(doc, 0);
    markdown_core_node *first = child_at(child_at(doc, 1), 0);
    markdown_core_node *second = child_at(child_at(doc, 1), 2);
    markdown_core_node *direct = child_at(child_at(doc, 1), 4);
    INT_EQ(runner, markdown_core_node_get_kind(reference), MARKDOWN_CORE_KIND_REFERENCE,
           "the definition is a Reference where it was written");
    markdown_core_destination destination = facade_destination(reference);
    OK(runner,
       destination.kind == MARKDOWN_CORE_DESTINATION_URL && destination.url.length == 7 &&
           !memcmp(destination.url.data, "/shared", 7) && facade_title(reference).has_value,
       "the Reference states the destination and title");
    for (int i = 0; i < 2; i++) {
        destination = facade_destination(i ? second : first);
        OK(runner,
           destination.kind == MARKDOWN_CORE_DESTINATION_REFERENCE && destination.label.length == 1 &&
               destination.label.data[0] == 'a' && !facade_title(i ? second : first).has_value,
           "occurrence %d names the normalized label and states no title", i);
    }
    destination = facade_destination(direct);
    OK(runner, destination.kind == MARKDOWN_CORE_DESTINATION_URL && facade_title(direct).has_value,
       "a direct link with the same bytes states its own");
    markdown_core_node_free(doc);
}

/* A REFERENCE'S ATTRIBUTES ARE ITS OWN, and an occurrence's are the ones
 * written on it: nothing merges them. */
static void reference_attribute_lifecycle(test_batch_runner *runner) {
    const char source[] = "[r][] [r][]{#own .same k=2}\n\n[r]: /u {#definition .same k=1 k=1}\n";
    markdown_core_node *root = markdown_core_parse_document(source, sizeof(source) - 1);
    markdown_core_node *first = child_at(child_at(root, 0), 0);
    markdown_core_node *second = child_at(child_at(root, 0), 2);
    markdown_core_node *reference = child_at(root, 1);
    INT_EQ(runner, markdown_core_node_get_kind(reference), MARKDOWN_CORE_KIND_REFERENCE, "the definition is a node");
    INT_EQ(runner, markdown_core_node_attribute_class_count(first), 0, "a bare occurrence has no attributes");
    OK(runner, !anchor_of(first).has_value, "a bare occurrence has no anchor");
    INT_EQ(runner, markdown_core_node_attribute_class_count(second), 1, "an occurrence keeps its own classes");
    INT_EQ(runner, markdown_core_node_attribute_record_count(second), 1, "an occurrence keeps its own records");
    attribute_eq(runner, second, 0, "k", "2", "the occurrence's own declaration");
    INT_EQ(runner, markdown_core_node_attribute_record_count(reference), 2, "the Reference keeps duplicates");
    attribute_eq(runner, reference, 0, "k", "1", "the Reference's first declaration");
    attribute_eq(runner, reference, 1, "k", "1", "the Reference's duplicate declaration");
    markdown_core_optional_string anchor = anchor_of(reference);
    OK(runner, anchor.has_value && anchor.value.length == 10 && !memcmp(anchor.value.data, "definition", 10),
       "the Reference has the anchor the definition states");
    markdown_core_node_free(root);
}

typedef struct {
    char prefix;
    long double value;
} payload_float_alignment;
typedef struct {
    char prefix;
    int64_t value;
} payload_integer_alignment;

/* THE POOL'S CLAIMS, in numbers the tree cannot show. A parse's nodes come
 * from slabs of many slots, so a run of constructions is one allocation; a
 * slot released into the pool is handed out again before another is taken
 * from a slab, so the storage a parse holds is bounded by its peak, not by
 * how many nodes it made; and a slab is freed by the last slot that leaves
 * it, whichever release does that, so a node outlives the pool that made it. */
static void node_slots_come_from_slabs_and_go_back_to_the_pool(test_batch_runner *runner) {
    enum { LIMIT = 4096 };
    static markdown_core_node *taken[LIMIT];
    markdown_core_node_pool pool = {0};
    payload_probe_arm();

    /* Take slots until the third slab arrives: the first two slabs say how
     * many slots one holds, and that the count is the slab's, not the run's. */
    size_t count = 0, per_slab = 0, second_slab_at = 0;
    while (count < LIMIT) {
        size_t before = payload_allocations;
        markdown_core_node *node = markdown_core_node_pool_new(&pool, MARKDOWN_CORE_NODE_TEXT, NULL);
        OK(runner, node != NULL, "a slot is taken");
        if (!node) {
            break;
        }
        taken[count++] = node;
        if (payload_allocations != before) {
            if (before == 0) {
                continue;
            }
            if (!per_slab) {
                per_slab = count - 1;
                second_slab_at = count;
            } else {
                break;
            }
        }
    }
    OK(runner, per_slab > 100, "a slab holds many slots: %zu", per_slab);
    INT_EQ(runner, count - second_slab_at, per_slab, "every slab holds the same number of slots");
    INT_EQ(runner, payload_allocations, 3, "taking %zu nodes made three allocations", count);
    for (size_t i = 1; i < count; i++) {
        OK(runner, taken[i]->as.literal && taken[i]->kind == MARKDOWN_CORE_NODE_TEXT,
           "each slot carries a zeroed node with its record");
    }

    /* Released into the pool, slots are reused before a slab is touched, and
     * the reused slot is the zeroed one a constructor expects. */
    size_t allocations = payload_allocations;
    markdown_core_node *reused[8];
    for (size_t i = 0; i < 8; i++) {
        reused[i] = taken[count - 1 - i];
        OK(runner, set_literal(reused[i], "bytes"), "a node owns a literal before release");
        markdown_core_node_pool_release(&pool, reused[i]);
    }
    count -= 8;
    INT_EQ(runner, payload_allocations, allocations + 8, "the literals were the only allocations");
    allocations = payload_allocations;
    for (size_t i = 0; i < 8; i++) {
        markdown_core_node *node = markdown_core_node_pool_new(&pool, MARKDOWN_CORE_NODE_PARAGRAPH, NULL);
        OK(runner, node != NULL, "a released slot is taken again");
        size_t j = 0;
        while (j < 8 && reused[j] != node) {
            j++;
        }
        OK(runner, j < 8, "the slot taken is one released into the pool");
        OK(runner,
           node && node->kind == MARKDOWN_CORE_NODE_PARAGRAPH && !node->as.data && !markdown_core_scaffold_next(node) &&
               !node->children && !node->child_count && node->content.size == 0,
           "a reused slot is zeroed before it is a node again");
        taken[count++] = node;
    }
    INT_EQ(runner, payload_allocations, allocations, "reusing released slots makes no allocator call");

    /* A record too large for the slot is storage of its size the node owns,
     * cut from the slabs beside the nodes and given back to the pool with
     * the node, under the rule copies and kind conversion use too. A pool of
     * its own keeps the slabs above holding nodes alone. */
    markdown_core_node_pool records = {0};
    allocations = payload_allocations;
    markdown_core_node *large = markdown_core_node_pool_new(&records, MARKDOWN_CORE_NODE_METADATA, NULL);
    OK(runner, large && large->node_data_allocation && large->as.data == large->node_data_allocation,
       "a record that does not fit the slot is owned through node_data_allocation");
    INT_EQ(runner, payload_allocations, allocations + 1, "the node and its out-of-slot record are cut from one slab");
    void *record = large ? large->node_data_allocation : NULL;
    size_t releases = payload_releases;
    markdown_core_node_pool_release(&records, large);
    large = markdown_core_node_pool_new(&records, MARKDOWN_CORE_NODE_METADATA, NULL);
    OK(runner, large && large->node_data_allocation == record, "the record given back is the next one of its size");
    INT_EQ(runner, payload_allocations, allocations + 1, "taking them again makes no allocator call");
    INT_EQ(runner, payload_releases, releases, "giving them back frees nothing");
    markdown_core_node_pool_dispose(&records);
    markdown_core_node_free(large);
    INT_EQ(runner, payload_releases, releases + 1, "the slab is freed with its last node and record");

    /* Refusing the slab refuses the node and leaves the pool usable. */
    while (count < LIMIT) {
        size_t before = payload_allocations;
        payload_fail_at = before + 1;
        markdown_core_node *node = markdown_core_node_pool_new(&pool, MARKDOWN_CORE_NODE_TEXT, NULL);
        payload_fail_at = 0;
        if (!node) {
            INT_EQ(runner, payload_allocations, before + 1, "the refused allocation was the slab's");
            node = markdown_core_node_pool_new(&pool, MARKDOWN_CORE_NODE_TEXT, NULL);
            OK(runner, node != NULL, "the pool takes a slab once the allocator allows one");
            taken[count++] = node;
            break;
        }
        taken[count++] = node;
    }

    /* The pool disposed, the nodes stand: each slab is freed by the last
     * slot to leave it, and not before. */
    releases = payload_releases;
    markdown_core_node_pool_dispose(&pool);
    INT_EQ(runner, payload_releases, releases, "disposing the pool frees no slab that still has a node in it");
    size_t freed_slabs = 0;
    for (size_t i = 0; i < count; i++) {
        releases = payload_releases;
        markdown_core_node_free(taken[i]);
        if (payload_releases != releases) {
            freed_slabs++;
            OK(runner, (i + 1) % per_slab == 0 || i + 1 == count,
               "a slab is freed by the last of its slots: node %zu of %zu per slab", i + 1, per_slab);
        }
    }
    OK(runner, freed_slabs >= 3, "every slab was freed by a node's release: %zu", freed_slabs);
    INT_EQ(runner, payload_live, 0, "slots, slabs and records are all released");
    payload_probe_disarm();
}

static void node_reuse_initializes_the_active_record(test_batch_runner *runner) {
    const struct {
        markdown_core_node_type kind;
        size_t bytes;
    } cases[] = {{MARKDOWN_CORE_NODE_PARAGRAPH, 0},
                 {MARKDOWN_CORE_NODE_TEXT, sizeof(markdown_core_chunk)},
                 {MARKDOWN_CORE_NODE_LIST, sizeof(markdown_core_list)},
                 {MARKDOWN_CORE_NODE_HEADING, sizeof(markdown_core_heading)},
                 {MARKDOWN_CORE_NODE_METADATA, sizeof(markdown_core_metadata_fields)}};
    markdown_core_node_pool pool = {0};
    payload_probe_arm();
    payload_fill_fresh = 1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        markdown_core_node *dirty = markdown_core_node_pool_new(&pool, MARKDOWN_CORE_NODE_LIST, NULL);
        void *record = dirty->as.data;
        OK(runner, !dirty->node_data_allocation, "the dirty record occupies retained slot storage");
        markdown_core_node_pool_release(&pool, dirty);
        /* A released slot is retained by the pool. Use a DIFFERENT poison
         * from fresh allocations, so omitted initialization cannot compare
         * equal by accident. Keep the pool's idle free-list link, which it
         * keeps in the released slot's first bytes (slab.h). */
        unsigned char *slot = (unsigned char *)markdown_core_node_slot_of(dirty);
        void *link;
        memcpy(&link, slot, sizeof(link));
        memset(slot, 0x5a, offsetof(markdown_core_node_slot, node) + sizeof(*dirty));
        memcpy(slot, &link, sizeof(link));
        memset(record, 0x5a, sizeof(markdown_core_list));
        markdown_core_node *node = markdown_core_node_pool_new(&pool, cases[i].kind, NULL);
        markdown_core_node *control = markdown_core_node_new(cases[i].kind);
        OK(runner, node == dirty && control, "a dirty slot is reused for kind %d", cases[i].kind);
        OK(runner,
           !markdown_core_scaffold_parent(node) && !markdown_core_scaffold_prev(node) &&
               !markdown_core_scaffold_next(node) && !markdown_core_scaffold_first(node) &&
               !markdown_core_scaffold_last(node) && !node->children && !node->child_count && !node->id &&
               !node->place.start && !node->place.end && !node->flags && !node->element &&
               node->content.ptr == markdown_core_strbuf__initbuf && !node->content.size && !node->content.asize &&
               !node->content.oom,
           "all node state is initialized independently of its former kind");
        OK(runner, cases[i].bytes ? !memcmp(node->as.data, control->as.data, cases[i].bytes) : !node->as.data,
           "the active record has the same complete initialization as a fresh node");
        markdown_core_node_pool_release(&pool, node);
        markdown_core_node_free(control);
    }
    markdown_core_node_pool_dispose(&pool);
    payload_fill_fresh = 0;
    INT_EQ(runner, payload_live, 0, "dirty-slot reuse and external records release every allocation");
    payload_probe_disarm();
}

/* RESOURCES ARE SLOTS TOO (slab.h), and they outlive the pool that made
 * them the way nodes do: a run of them is a few slab allocations, disposing
 * the pool frees no slab a resource is still in, and the last resource out of
 * a slab frees it. */
static void resource_slots_come_from_slabs_and_outlive_the_pool(test_batch_runner *runner) {
    enum { COUNT = 256 };
    static markdown_core_resource *taken[COUNT];
    markdown_core_node_pool pool = {0};
    payload_probe_arm();
    for (size_t i = 0; i < COUNT; i++) {
        taken[i] =
            markdown_core_resource_new(&pool, markdown_core_chunk_literal("/u"), markdown_core_optional_chunk_absent());
        OK(runner, taken[i] && !taken[i]->title.has_value && taken[i]->url.len == 2, "resource %zu is taken", i);
    }
    OK(runner, payload_allocations > 0 && payload_allocations <= COUNT / 32, "%d resources took %zu allocations", COUNT,
       payload_allocations);
    size_t releases = payload_releases;
    markdown_core_node_pool_dispose(&pool);
    INT_EQ(runner, payload_releases, releases, "disposing the pool frees no slab a resource is still in");
    for (size_t i = 0; i < COUNT; i++) {
        markdown_core_resource_free(NULL, taken[i]);
    }
    INT_EQ(runner, payload_live, 0, "every slab went with the last resource in it");
    payload_probe_disarm();
}

static markdown_core_node *first_of_kind(markdown_core_node *root, markdown_core_node_type kind);

/* EVERY NODE OF ONE KIND IN AN ENGINE TREE, in source order: the content and
 * every field root the engine's own field visitor names, an inline note's
 * included. The caller frees the array. */
typedef struct {
    markdown_core_node **nodes;
    size_t count, capacity;
} node_list;

static int node_list_push(markdown_core_node **slot, void *context) {
    node_list *list = context;
    if (!slot || !*slot) {
        return 1;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        markdown_core_node **nodes = realloc(list->nodes, capacity * sizeof(*nodes));
        if (!nodes) {
            return 0;
        }
        list->nodes = nodes;
        list->capacity = capacity;
    }
    list->nodes[list->count++] = *slot;
    return 1;
}

/* Every node of `kind`, in canonical walk order, read from the relations
 * alone: the observers that ask run before the tree is published, so its
 * nodes have no runs for the canonical walk to read yet. */
typedef struct {
    markdown_core_relation_cursor cursor;
    markdown_core_relation relation;
    size_t next;
} kind_frame;

static node_list nodes_of_kind(markdown_core_node *root, markdown_core_node_type kind) {
    node_list found = {0};
    if (root->kind == kind) {
        node_list_push(&root, &found);
    }
    size_t depth = 1, capacity = 16;
    kind_frame *frames = malloc(capacity * sizeof(*frames));
    frames[0] = (kind_frame){0};
    markdown_core_relations_begin(&frames[0].cursor, root);
    while (depth) {
        kind_frame *frame = &frames[depth - 1];
        if (frame->next == frame->relation.count) {
            if (!markdown_core_relations_next(&frame->cursor, &frame->relation)) {
                depth--;
            } else {
                frame->next = 0;
            }
            continue;
        }
        markdown_core_node *node = frame->relation.items[frame->next++];
        if (!node) {
            continue;
        }
        if (node->kind == kind) {
            node_list_push(&node, &found);
        }
        if (depth == capacity) {
            capacity *= 2;
            frames = realloc(frames, capacity * sizeof(*frames));
        }
        frames[depth] = (kind_frame){0};
        markdown_core_relations_begin(&frames[depth++].cursor, node);
    }
    free(frames);
    return found;
}

/* Every footnote, definitions and inline notes alike, in source order, which
 * the canonical walk order is for footnotes. */
static node_list footnotes_of(markdown_core_node *root) { return nodes_of_kind(root, MARKDOWN_CORE_NODE_FOOTNOTE); }

/* A footnote's label as a C string, or NULL for an inline note. */
static const char *footnote_label_of(const markdown_core_node *note) {
    return note->as.footnote->label.has_value ? (const char *)note->as.footnote->label.value.data : NULL;
}

/* A REFERENCE OCCURRENCE A HEADING DECLARES names the heading's label, and
 * the document resolves the label to the heading; the heading owns its
 * computed anchor. */
static void heading_reference_resolves_to_the_heading(test_batch_runner *runner) {
    const char *source = "# Straße and more\n\n[STRASSE AND MORE] and [straße and more]\n\n# Untitled [x]\n";
    markdown_core_document *document = facade_parse((const uint8_t *)source, strlen(source));
    OK(runner, document != NULL, "heading reference document parses");
    if (!document) {
        return;
    }
    markdown_core_node *heading = first_of_kind(document->root, MARKDOWN_CORE_NODE_HEADING);
    markdown_core_node *paragraph = child_at(document->root, 1);
    markdown_core_node *links[] = {child_at(paragraph, 0), last_child_of(paragraph)};
    for (int i = 0; i < 2; i++) {
        markdown_core_destination destination = facade_destination(links[i]);
        OK(runner,
           destination.kind == MARKDOWN_CORE_DESTINATION_REFERENCE && destination.label.length == 16 &&
               !memcmp(destination.label.data, "strasse and more", 16),
           "occurrence %d names the case-folded label", i);
        OK(runner, markdown_core_document_reference_for(document, destination.label) == heading,
           "the label resolves to the heading");
    }
    STR_EQ(runner, (const char *)heading->attributes.anchor.data, "straße-and-more", "the anchor is the slug");
    OK(runner, heading->attributes.anchor.alloc, "the heading owns its anchor");
    INT_EQ(runner, markdown_core_document_reference_label_count(document), 1,
           "only the label a heading's text declares resolves");
    markdown_core_document_free(document);
}

/* A `class=` RUN SPLITS WHERE HTML SPLITS A `class` ATTRIBUTE: at tab, line
 * feed, form feed, carriage return and space, written or decoded from a
 * character reference, and nowhere else. Vertical tab and non-ASCII spaces --
 * NBSP, EM SPACE, IDEOGRAPHIC SPACE, raw or referenced -- stay inside a class. */
static void class_runs_split_on_ascii_white_space(test_batch_runner *runner) {
    const char *source = "[x]{class=\"a b\tc&#10;d&#12;e&#13;f g\xC2\xA0h i\xE3\x80\x80j k&nbsp;l "
                         "m&#x3000;n o&#11;p q\xE2\x80\x83r  s\"}\n";
    const char *expected[] = {"a",          "b",
                              "c",          "d",
                              "e",          "f",
                              "g\xC2\xA0h", "i\xE3\x80\x80j",
                              "k\xC2\xA0l", "m\xE3\x80\x80n",
                              "o\vp",       "q\xE2\x80\x83r",
                              "s"};
    const size_t count = sizeof(expected) / sizeof(expected[0]);
    markdown_core_document *document = facade_parse((const uint8_t *)source, strlen(source));
    OK(runner, document != NULL, "class run document parses");
    if (!document) {
        return;
    }
    markdown_core_node *span = first_of_kind(document->root, MARKDOWN_CORE_NODE_SPAN);
    OK(runner, span != NULL, "the attributes make a span");
    if (span) {
        INT_EQ(runner, (int)markdown_core_node_attribute_class_count(span), (int)count, "one class per separated run");
        for (size_t i = 0; i < count; i++) {
            markdown_core_string class = facade_class_at(span, i);
            OK(runner, class.length == strlen(expected[i]) && !memcmp(class.data, expected[i], class.length),
               "class %zu is %s", i, expected[i]);
        }
    }
    markdown_core_document_free(document);
}

/* THE UNICODE CLASSES ARE UNICODE 17'S. One table answers every class, so
 * these pin the version: characters P and S gained after the older tables the
 * punctuation predicates once used, one that left P for S, and the values
 * that are not scalars at all. */
static void unicode_classes_are_unicode_17(test_batch_runner *runner) {
    static const struct {
        int32_t scalar;
        int whitespace, punctuation, punctuation_or_symbol, letter, number, mark;
    } cases[] = {{0x20, 1, 0, 0, 0, 0, 0},     {0x0B, 0, 0, 0, 0, 0, 0},    {0x85, 0, 0, 0, 0, 0, 0},
                 {0x3000, 1, 0, 0, 0, 0, 0},   {'!', 0, 1, 1, 0, 0, 0},     {'$', 0, 1, 1, 0, 0, 0},
                 {'a', 0, 0, 0, 1, 0, 0},      {'7', 0, 0, 0, 0, 1, 0},     {0xA7, 0, 1, 1, 0, 0, 0},
                 {0x166D, 0, 0, 1, 0, 0, 0},   {0x1B4E, 0, 1, 1, 0, 0, 0},  {0x2FFC, 0, 0, 1, 0, 0, 0},
                 {0x4E00, 0, 0, 0, 1, 0, 0},   {0x20000, 0, 0, 0, 1, 0, 0}, {0x0661, 0, 0, 0, 0, 1, 0},
                 {0x2167, 0, 0, 0, 0, 1, 0},   {'~', 0, 1, 1, 0, 0, 0},     {0x7F, 0, 0, 0, 0, 0, 0},
                 {0x80, 0, 0, 0, 0, 0, 0},     {0xA0, 1, 0, 0, 0, 0, 0},    {0xD800, 0, 0, 0, 0, 0, 0},
                 {0x110000, 0, 0, 0, 0, 0, 0}, {-1, 0, 0, 0, 0, 0, 0},      {0x0301, 0, 0, 0, 0, 0, 1},
                 {0x093E, 0, 0, 0, 0, 0, 1},   {0x20DD, 0, 0, 0, 0, 0, 1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int32_t scalar = cases[i].scalar;
        uint8_t classes = markdown_core_utf8proc_classes(scalar);
        OK(runner,
           !!(classes & MARKDOWN_CORE_UNICODE_WHITESPACE) == cases[i].whitespace &&
               !!(classes & MARKDOWN_CORE_UNICODE_PUNCTUATION) == cases[i].punctuation &&
               !!(classes & MARKDOWN_CORE_UNICODE_PUNCTUATION_OR_SYMBOL) == cases[i].punctuation_or_symbol &&
               !!(classes & MARKDOWN_CORE_UNICODE_LETTER) == cases[i].letter &&
               !!(classes & MARKDOWN_CORE_UNICODE_NUMBER) == cases[i].number &&
               !!(classes & MARKDOWN_CORE_UNICODE_MARK) == cases[i].mark,
           "U+%04X has its Unicode 17 classes", (unsigned)scalar);
    }
    /* Emoji outside ASCII, whose keycap bases stay ordinary characters, and
     * the decimal digits among the numbers. */
    OK(runner,
       (markdown_core_utf8proc_classes(0x1F389) & MARKDOWN_CORE_UNICODE_EMOJI) &&
           (markdown_core_utf8proc_classes(0xA9) & MARKDOWN_CORE_UNICODE_EMOJI) &&
           !(markdown_core_utf8proc_classes('#') & MARKDOWN_CORE_UNICODE_EMOJI) &&
           !(markdown_core_utf8proc_classes('7') & MARKDOWN_CORE_UNICODE_EMOJI),
       "emoji are the Emoji property outside ASCII");
    OK(runner,
       (markdown_core_utf8proc_classes('7') & MARKDOWN_CORE_UNICODE_DECIMAL) &&
           (markdown_core_utf8proc_classes(0x0661) & MARKDOWN_CORE_UNICODE_DECIMAL) &&
           !(markdown_core_utf8proc_classes(0x2167) & MARKDOWN_CORE_UNICODE_DECIMAL) &&
           !(markdown_core_utf8proc_classes(0xB2) & MARKDOWN_CORE_UNICODE_DECIMAL),
       "decimal digits are Nd");
    /* U+1B4E BALINESE INVERTED CARIK SIAKI became punctuation in Unicode 16,
     * so it keeps `a**` from opening strong emphasis, as a quote would. */
    const char *source = "a**\xE1\xAD\x8E"
                         "foo\xE1\xAD\x8E**b\n";
    markdown_core_document *document = facade_parse((const uint8_t *)source, strlen(source));
    OK(runner, document != NULL, "flanking document parses");
    if (document) {
        OK(runner, first_of_kind(document->root, MARKDOWN_CORE_NODE_STRONG) == NULL,
           "Unicode 17 punctuation decides flanking");
        markdown_core_document_free(document);
    }
}

/* A scalar's UTF-8 encoding, written straight from the definition so the
 * decoder is checked against the standard and not against itself. */
static int utf8_encode_scalar(int32_t scalar, uint8_t out[4]) {
    if (scalar < 0x80) {
        out[0] = (uint8_t)scalar;
        return 1;
    }
    if (scalar < 0x800) {
        out[0] = (uint8_t)(0xC0 | scalar >> 6);
        out[1] = (uint8_t)(0x80 | (scalar & 0x3F));
        return 2;
    }
    if (scalar < 0x10000) {
        out[0] = (uint8_t)(0xE0 | scalar >> 12);
        out[1] = (uint8_t)(0x80 | ((scalar >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (scalar & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | scalar >> 18);
    out[1] = (uint8_t)(0x80 | ((scalar >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((scalar >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (scalar & 0x3F));
    return 4;
}

/* THE DECODER IS UTF-8'S DEFINITION on every scalar. Each one, encoded at the
 * end of an exact-size heap block so that a read past it is a sanitizer
 * error, decodes to itself with its encoded width, and that width is also
 * the one its lead byte gives. Every proper prefix of an encoding is a
 * character cut off by its range: the rest of the range, with no scalar. So
 * is a continuation byte where a character should begin. */
static void utf8_decode_reads_every_scalar(test_batch_runner *runner) {
    uint8_t *block = malloc(4);
    int wrong = 0, cut_wrong = 0, stray_wrong = 0;
    for (int32_t scalar = 0; scalar < 0x110000; scalar++) {
        if (scalar >= 0xD800 && scalar < 0xE000) {
            continue;
        }
        uint8_t bytes[4];
        int width = utf8_encode_scalar(scalar, bytes);
        memcpy(block + 4 - width, bytes, (size_t)width);
        int32_t decoded = -1;
        int got = markdown_core_utf8proc_decode(block + 4 - width, width, &decoded);
        wrong += got != width || decoded != scalar || markdown_core_utf8proc_width(bytes[0]) != width;
        for (int cut = 1; cut < width; cut++) {
            memcpy(block + 4 - cut, bytes, (size_t)cut);
            got = markdown_core_utf8proc_decode(block + 4 - cut, cut, &decoded);
            cut_wrong += got != cut || decoded != MARKDOWN_CORE_NO_SCALAR;
        }
    }
    for (int byte = 0x80; byte < 0xC0; byte++) {
        int32_t decoded = -1;
        block[3] = (uint8_t)byte;
        stray_wrong += markdown_core_utf8proc_decode(block + 3, 1, &decoded) != 1 || decoded != MARKDOWN_CORE_NO_SCALAR;
    }
    free(block);
    INT_EQ(runner, wrong, 0, "every scalar decodes to itself with its encoded width");
    INT_EQ(runner, cut_wrong, 0, "every cut encoding is the rest of its range, with no scalar");
    INT_EQ(runner, stray_wrong, 0, "a continuation byte alone is one byte with no scalar");
}

/* THE DECODER IS TOTAL on any bytes, which is all it promises outside UTF-8:
 * its width is the lead byte's width clamped to the range, its result does
 * not depend on the bytes after the character, and its scalar is never
 * negative and below 0x400000. A scalar below U+110000 encodes in no more
 * bytes than it was decoded from, which is what the anchor's reservation
 * rests on. Every lead byte meets every range of one to four bytes, each in
 * an exact-size heap block, followed by bytes chosen at the boundaries of the
 * byte classes, and is decoded again alone in a block of exactly its own
 * bytes; a seeded walk over random bytes then checks that every step moves
 * forward and that the walk ends exactly at the end. */
static void utf8_decode_is_total(test_batch_runner *runner) {
    static const uint8_t second[] = {0x00, 0x0A, 0x20, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF,
                                     0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF};
    static const uint8_t later[] = {0x00, 0x41, 0x80, 0xBF, 0xC2, 0xFF};
    const size_t seconds = sizeof(second), laters = sizeof(later);
    int cases = 0, wrong = 0;
    for (int lead = 0; lead < 256; lead++) {
        for (int len = 1; len <= 4; len++) {
            size_t combinations = len == 1 ? 1 : seconds;
            for (int k = 2; k < len; k++) {
                combinations *= laters;
            }
            for (size_t combination = 0; combination < combinations; combination++) {
                uint8_t *bytes = malloc((size_t)len);
                size_t rest = combination;
                bytes[0] = (uint8_t)lead;
                if (len > 1) {
                    bytes[1] = second[rest % seconds];
                    rest /= seconds;
                }
                for (int k = 2; k < len; k++) {
                    bytes[k] = later[rest % laters];
                    rest /= laters;
                }
                int32_t scalar = -1;
                int width = markdown_core_utf8proc_decode(bytes, len, &scalar);
                int expected = markdown_core_utf8proc_width((uint8_t)lead) < len
                                   ? markdown_core_utf8proc_width((uint8_t)lead)
                                   : len;
                /* The same character alone, in a block of exactly its own
                 * bytes: the same result, and no read past its range. */
                uint8_t *own = malloc((size_t)expected);
                memcpy(own, bytes, (size_t)expected);
                int32_t own_scalar = -1;
                int own_width = markdown_core_utf8proc_decode(own, expected, &own_scalar);
                free(own);
                uint8_t encoded[4];
                int ok =
                    width == expected && own_width == width && own_scalar == scalar && scalar >= 0 && scalar < 0x400000;
                if (lead < 0x80) {
                    ok &= scalar == lead;
                } else if (lead < 0xC0 || markdown_core_utf8proc_width((uint8_t)lead) > len) {
                    ok &= scalar == MARKDOWN_CORE_NO_SCALAR;
                } else if (scalar < MARKDOWN_CORE_NO_SCALAR) {
                    ok &= utf8_encode_scalar(scalar, encoded) <= width;
                }
                wrong += !ok;
                cases++;
                free(bytes);
            }
        }
    }
    OK(runner, cases > 100000, "%d lead-and-range cases", cases);
    INT_EQ(runner, wrong, 0, "every width is the lead's clamped to its range, and every scalar is bounded");

    uint64_t state = UINT64_C(0x9E3779B97F4A7C15);
    int walks_wrong = 0;
    for (int walk = 0; walk < 4096; walk++) {
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        bufsize_t len = (bufsize_t)(1 + (state >> 33) % 64);
        uint8_t *bytes = malloc((size_t)len);
        for (bufsize_t at = 0; at < len; at++) {
            state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
            bytes[at] = (uint8_t)(state >> 56);
        }
        bufsize_t at = 0;
        while (at < len) {
            int32_t scalar;
            int width = markdown_core_utf8proc_decode(bytes + at, len - at, &scalar);
            if (width < 1 || width > len - at) {
                walks_wrong++;
                break;
            }
            at += width;
        }
        walks_wrong += at != len;
        free(bytes);
    }
    INT_EQ(runner, walks_wrong, 0, "every walk over random bytes moves forward and ends at its end");
}

/* THE CLASS LOOKUP IS TOTAL over int32_t. Every scalar's classes are its
 * page's block entry; page 0, U+0000..U+00FF, is laid at offset 0 and read
 * without its entry. Surrogates, MARKDOWN_CORE_NO_SCALAR, everything a
 * decoder can produce past it, and the negative values have no class. */
static void unicode_classes_are_total(test_batch_runner *runner) {
    int staged = markdown_core_unicode_pages[0] == 0;
    for (int32_t scalar = 0; scalar < 0x110000; scalar++) {
        staged &= markdown_core_utf8proc_classes(scalar) ==
                  markdown_core_unicode_blocks[markdown_core_unicode_pages[scalar >> 8] + (scalar & 255)];
    }
    OK(runner, staged, "every scalar's classes are its page's block entry");
    int none = 1;
    for (int32_t scalar = 0xD800; scalar < 0xE000; scalar++) {
        none &= markdown_core_utf8proc_classes(scalar) == 0;
    }
    for (int32_t scalar = MARKDOWN_CORE_NO_SCALAR; scalar < 0x400000; scalar++) {
        none &= markdown_core_utf8proc_classes(scalar) == 0;
    }
    none &= markdown_core_utf8proc_classes(-1) == 0 && markdown_core_utf8proc_classes(INT32_MIN) == 0 &&
            markdown_core_utf8proc_classes(INT32_MAX) == 0;
    OK(runner, none, "values that are not scalars have no class");
    int ascii = 1;
    for (int byte = 0; byte < 0x80; byte++) {
        uint8_t b = (uint8_t)byte;
        int alnum = (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
        ascii &= markdown_core_utf8proc_alnum_width(&b, 1) == alnum;
    }
    OK(runner, ascii && markdown_core_utf8proc_alnum_width((const uint8_t *)"a", 0) == 0,
       "an ASCII byte is alphanumeric exactly when it is a letter or digit, and an empty range is not");
}

/* AN ANCHOR IMAGES ONLY SCALARS. Bytes that are not a character, a lead cut
 * off by the end of the literal, every surrogate and every value past
 * U+10FFFF that four bytes can spell have no image, so an anchor is UTF-8
 * whatever its heading's bytes were. */
static void anchor_images_only_scalars(test_batch_runner *runner) {
    markdown_core_strbuf anchor = MARKDOWN_CORE_BUF_INIT();
    int empty = 1;
    for (int32_t scalar = 0xD800; scalar < 0xE000; scalar++) {
        const uint8_t bytes[3] = {(uint8_t)(0xE0 | scalar >> 12), (uint8_t)(0x80 | ((scalar >> 6) & 0x3F)),
                                  (uint8_t)(0x80 | (scalar & 0x3F))};
        markdown_core_strbuf_clear(&anchor);
        markdown_core_utf8proc_anchor(&anchor, bytes, 3);
        empty &= anchor.size == 0;
    }
    OK(runner, empty, "no surrogate has an anchor image");
    static const char *const others[] = {"\xF4\x90\x80\x80", "\xF7\xBF\xBF\xBF", "\xFF\xFF\xFF\xFF", "\x80",
                                         "\xBF\xBF",         "\xE4\xB8",         "\xF0\x9F\x98"};
    for (size_t i = 0; i < sizeof(others) / sizeof(*others); i++) {
        markdown_core_strbuf_clear(&anchor);
        markdown_core_utf8proc_anchor(&anchor, (const uint8_t *)others[i], (bufsize_t)strlen(others[i]));
        INT_EQ(runner, (int)anchor.size, 0, "bytes %zu that are no scalar have no anchor image", i);
    }
    markdown_core_strbuf_clear(&anchor);
    markdown_core_utf8proc_anchor(&anchor,
                                  (const uint8_t *)"A\x80"
                                                   "B\xE4",
                                  4);
    STR_EQ(runner, (const char *)anchor.ptr, "ab", "the characters around them keep theirs");
    markdown_core_strbuf_free(&anchor);
}

/* WHITESPACE IS A SPACE, A TAB OR A LINE ENDING (docs/specs/dialect.md), and
 * no other byte: not vertical tab, form feed or another control, and no byte
 * of a multi-byte character, so no Unicode space. */
static void whitespace_is_space_tab_and_line_ending(test_batch_runner *runner) {
    int exact = 1;
    for (int c = 0; c < 256; c++) {
        const int space_or_tab = c == ' ' || c == '\t';
        const int line_end = c == '\n' || c == '\r';
        exact &= !!markdown_core_is_space_or_tab((unsigned char)c) == space_or_tab &&
                 !!markdown_core_is_line_end((unsigned char)c) == line_end &&
                 !!markdown_core_is_whitespace((unsigned char)c) == (space_or_tab || line_end);
    }
    OK(runner, exact, "every byte's whitespace class");

    /* A byte-class table names the same bytes as the predicates. */
    static const uint8_t classes[256] = {MARKDOWN_CORE_SPACE_OR_TAB_BYTES(1), MARKDOWN_CORE_LINE_END_BYTES(2)};
    static const uint8_t whitespace[256] = {MARKDOWN_CORE_WHITESPACE_BYTES(1)};
    int tables = 1;
    for (int c = 0; c < 256; c++) {
        tables &= (classes[c] == 1) == !!markdown_core_is_space_or_tab((unsigned char)c) &&
                  (classes[c] == 2) == !!markdown_core_is_line_end((unsigned char)c) &&
                  (whitespace[c] == 1) == !!markdown_core_is_whitespace((unsigned char)c);
    }
    OK(runner, tables, "byte-class tables name the whitespace bytes");

    static const struct {
        const char *text;
        bufsize_t end;
    } separators[] = {{"", 0},        {"x", 0},        {" \tx", 2}, {" \n x", 3}, {"\r\n\tx", 3}, {"\r x", 2},
                      {" \n\n x", 2}, {" \n \n x", 3}, {"\r\r", 1}, {"\v x", 0},  {"\f\n", 0},    {"\xC2\xA0x", 0}};
    for (size_t i = 0; i < sizeof(separators) / sizeof(separators[0]); i++) {
        const unsigned char *text = (const unsigned char *)separators[i].text;
        INT_EQ(runner, (int)markdown_core_skip_spaces_and_line_end(text, 0, (bufsize_t)strlen(separators[i].text)),
               (int)separators[i].end, "separator %zu ends at %d", i, (int)separators[i].end);
    }

    static const struct {
        const char *text;
        int blank;
    } rests[] = {{"", 1}, {" \t", 1}, {" \n x", 1}, {"\t\rx", 1}, {" x", 0}, {"\v\n", 0}, {"\f", 0}, {"\xC2\xA0", 0}};
    for (size_t i = 0; i < sizeof(rests) / sizeof(rests[0]); i++) {
        const unsigned char *text = (const unsigned char *)rests[i].text;
        OK(runner, !!markdown_core_is_blank_to_line_end(text, 0, (bufsize_t)strlen(rests[i].text)) == rests[i].blank,
           "rest %zu is %sblank", i, rests[i].blank ? "" : "not ");
    }
}

/* THE ROSTER'S RECORDS ARE CARVED FROM BLOCKS IT OWNS: a run of
 * declarations costs allocations logarithmic in its length -- the blocks, the
 * log's array and the index each double -- rather than one each, and all of
 * them go with the roster. */
static void roster_records_are_carved_from_its_blocks(test_batch_runner *runner) {
    enum { COUNT = 4096 };
    payload_probe_arm();
    markdown_core_roster roster;
    markdown_core_roster_init(&roster);
    char label[32];
    bool declared = true;
    for (size_t i = 0; i < COUNT; i++) {
        snprintf(label, sizeof(label), "label %zu", i);
        declared = declared &&
                   markdown_core_roster_declare(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, (const unsigned char *)label,
                                                (bufsize_t)strlen(label), (uint32_t)i);
    }
    OK(runner, declared && roster.count == COUNT, "every declaration is a record");
    /* Twelve doublings reach COUNT; each of the three structures doubles
     * at most once per doubling. */
    OK(runner, payload_allocations <= 3 * 13, "%d records took %zu allocations", COUNT, payload_allocations);
    bool failed = false;
    markdown_core_chunk lookup = {(unsigned char *)"LABEL  4095", 11, 0};
    const markdown_core_roster_record *found =
        markdown_core_roster_find_label(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, &lookup, &failed);
    markdown_core_chunk name = found ? markdown_core_roster_label(found) : (markdown_core_chunk){0};
    OK(runner, !failed && name.len == 10 && !memcmp(name.data, "label 4095", 10),
       "a carved record is found by its label's normal form");
    markdown_core_roster_free(&roster);
    INT_EQ(runner, payload_live, 0, "the roster's blocks, log and index are all released");
    payload_probe_disarm();
}

/* Two byte strings in byte order, the shorter first on a common prefix,
 * written apart from the index's own order. */
static int key_bytes_order(const char *a, size_t a_size, const char *b, size_t b_size) {
    const size_t common = a_size < b_size ? a_size : b_size;
    const int order = common ? memcmp(a, b, common) : 0;
    return order ? order : (a_size > b_size) - (a_size < b_size);
}

static int key_model_order(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    return key_bytes_order(x, strlen(x), y, strlen(y));
}

static markdown_core_key_entry key_of(const char *key) {
    const bufsize_t size = (bufsize_t)strlen(key);
    const unsigned char *bytes = (const unsigned char *)key;
    return (markdown_core_key_entry){markdown_core_key_head(bytes, size), bytes, size, 0, NULL};
}

/* THE ORDERED INDEX AGREES WITH A SORTED MODEL whichever order the keys
 * arrive in -- rising, falling or scattered, through splits at every level:
 * one entry per key, each key's entry and rank, each rank's entry, a walk
 * from any key, a missing key's rank. Keys share heads, so the bytes past the
 * eighth and the lengths decide among them too. An insertion whose storage
 * cannot be had leaves the tree as it was, and the free releases every
 * node. */
static void key_tree_matches_a_sorted_model(test_batch_runner *runner) {
    enum { COUNT = 3000, ORDERS = 3 };
    static char keys[COUNT][32];
    static const char *sorted[COUNT];
    for (size_t i = 0; i < COUNT; i++) {
        if (i % 3 == 0) {
            snprintf(keys[i], sizeof(keys[i]), "%zu", i);
        } else if (i % 3 == 1) {
            snprintf(keys[i], sizeof(keys[i]), "headings/%zu", i * 7);
        } else {
            snprintf(keys[i], sizeof(keys[i]), "headings/%zu/%zu", i, i % 11);
        }
        sorted[i] = keys[i];
    }
    qsort(sorted, COUNT, sizeof(*sorted), key_model_order);
    payload_probe_arm();
    for (size_t order = 0; order < ORDERS; order++) {
        markdown_core_key_tree tree = {0};
        bool ok = true;
        for (size_t step = 0; step < COUNT && ok; step++) {
            const size_t i = order == 0 ? step : order == 1 ? COUNT - 1 - step : step * 1999 % COUNT;
            const markdown_core_key_entry key = key_of(keys[i]);
            bool made = false;
            markdown_core_key_entry *entry = markdown_core_key_tree_entry(&tree, &key, &made);
            ok = entry && made && entry->key == key.key;
            if (ok) {
                entry->value = keys[i];
                entry = markdown_core_key_tree_entry(&tree, &key, &made);
                ok = entry && !made && entry->value == keys[i];
            }
        }
        OK(runner, ok && tree.count == COUNT, "order %zu: every key has one entry", order);
        bool agrees = true;
        for (size_t rank = 0; rank < COUNT && agrees; rank++) {
            const markdown_core_key_entry key = key_of(sorted[rank]);
            size_t found_rank = SIZE_MAX;
            const markdown_core_key_entry *found = markdown_core_key_tree_find(&tree, &key, &found_rank);
            agrees = found && found->value == sorted[rank] && found_rank == rank &&
                     markdown_core_key_tree_at(&tree, rank)->value == sorted[rank];
            char missing[40];
            snprintf(missing, sizeof(missing), "%s~", sorted[rank]);
            const markdown_core_key_entry absent = key_of(missing);
            agrees = agrees && !markdown_core_key_tree_find(&tree, &absent, &found_rank);
            const size_t expected = rank + 1 < COUNT && key_model_order(&sorted[rank + 1], &(const char *){missing}) < 0
                                        ? SIZE_MAX
                                        : rank + 1;
            agrees = agrees && (expected == SIZE_MAX || found_rank == expected);
        }
        OK(runner, agrees, "order %zu: each key's entry and rank, and each rank's entry, agree", order);
        bool walks = true;
        for (size_t from = 0; from < COUNT && walks; from += 97) {
            const markdown_core_key_entry key = key_of(sorted[from]);
            markdown_core_key_tree_cursor cursor;
            markdown_core_key_tree_seek(&tree, &key, &cursor);
            size_t at = from;
            for (const markdown_core_key_entry *entry; (entry = markdown_core_key_tree_next(&cursor)); at++) {
                walks = walks && at < COUNT && entry->value == sorted[at];
            }
            walks = walks && at == COUNT;
        }
        OK(runner, walks, "order %zu: a walk from any key visits the rest in order", order);
        /* A key past every other, which splits the rightmost node at each
         * level that is full, refused at each allocation it makes. */
        bool kept = true;
        for (size_t fail = 1; fail <= 4 && kept; fail++) {
            const markdown_core_key_entry key = key_of("~");
            payload_fail_at = payload_allocations + fail;
            bool made = false;
            const markdown_core_key_entry *entry = markdown_core_key_tree_entry(&tree, &key, &made);
            payload_fail_at = 0;
            if (entry) {
                kept = kept && made && tree.count == COUNT + 1;
                break;
            }
            kept = tree.count == COUNT && markdown_core_key_tree_at(&tree, COUNT - 1)->value == sorted[COUNT - 1];
        }
        OK(runner, kept, "order %zu: a refused insertion leaves the tree as it was", order);
        markdown_core_key_tree_free(&tree);
    }
    markdown_core_key_tree numbers = {0};
    bool ok = true;
    for (uint32_t i = 0; i < COUNT && ok; i++) {
        const markdown_core_key_entry key = markdown_core_key_number(COUNT - i);
        bool made = false;
        markdown_core_key_entry *entry = markdown_core_key_tree_entry(&numbers, &key, &made);
        ok = entry && made;
        if (ok) {
            entry->extra = COUNT - i;
        }
    }
    for (uint32_t rank = 0; rank < COUNT && ok; rank++) {
        ok = markdown_core_key_tree_at(&numbers, rank)->extra == rank + 1;
    }
    OK(runner, ok, "numbers are keys in numeric order");
    markdown_core_key_tree_free(&numbers);
    INT_EQ(runner, payload_live, 0, "the trees' nodes are all released");
    payload_probe_disarm();
}

/* THE KINDS ARE SEPARATE LOOKUP SPACES, and a key's records answer for the
 * earliest of them: a label declared as a reference is not a footnote, and
 * a key declared twice starts where its earlier declaration starts. */
static void roster_kinds_and_starts(test_batch_runner *runner) {
    markdown_core_roster roster;
    markdown_core_roster_init(&roster);
    const unsigned char *x = (const unsigned char *)"x";
    bool failed = false;
    OK(runner, markdown_core_roster_declare(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, x, 1, 40),
       "a reference is declared");
    OK(runner, !markdown_core_roster_find(&roster, MARKDOWN_CORE_ROSTER_FOOTNOTE, x, 1, &failed),
       "a reference label is no footnote");
    OK(runner, !markdown_core_roster_find(&roster, MARKDOWN_CORE_ROSTER_SPECIMEN, x, 1, &failed), "nor a specimen");
    const markdown_core_roster_record *record =
        markdown_core_roster_find(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, x, 1, &failed);
    OK(runner,
       record && !markdown_core_roster_starts_before(&roster, record, 40) &&
           markdown_core_roster_starts_before(&roster, record, 41),
       "a declaration starts where its definition starts");
    OK(runner,
       markdown_core_roster_declare(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, x, 1, 90) &&
           markdown_core_roster_declare(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, x, 1, 10),
       "the key is declared twice more");
    record = markdown_core_roster_find(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, x, 1, &failed);
    OK(runner,
       record && record->start == 10 && markdown_core_roster_starts_before(&roster, record, 11) &&
           !markdown_core_roster_starts_before(&roster, record, 10),
       "the key's earliest declaration answers");
    markdown_core_roster_mark mark = markdown_core_roster_mark_now(&roster);
    OK(runner, markdown_core_roster_declare(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, x, 1, 2),
       "a declaration after the mark");
    markdown_core_roster_rollback(&roster, mark);
    record = markdown_core_roster_find(&roster, MARKDOWN_CORE_ROSTER_REFERENCE, x, 1, &failed);
    OK(runner, record && record->start == 10 && !markdown_core_roster_starts_before(&roster, record, 10),
       "a rollback takes the earlier start back with the record");
    OK(runner, !failed, "and no lookup failed");
    markdown_core_roster_free(&roster);
}

/* ROLLBACK RESTORES THE LOG AND THE INDEX EXACTLY. A model -- each key's
 * declarations that stand, as a list of where they start, and every step
 * that changed one, which a rollback undoes newest first -- is driven
 * through the same random declarations, withdrawals, marks and rollbacks,
 * and after a random third of the steps every key of a random set of the
 * kinds is compared: whether it is declared, where its earliest declaration
 * starts and how many of its declarations stand. A withdrawal withdraws the
 * declaration that starts where it does, or the key's last when none does.
 * The index takes a kind's records only when a lookup or a withdrawal of
 * that kind asks, so the steps between two comparisons, and the kinds a
 * comparison skips, leave records it never took, and the rollbacks pop
 * records it took and records it did not. Labels come from a small set, so
 * keys are declared again and again, and from a growing one, so the index
 * grows and removals cross the slots growth moved. A rollback to the first
 * mark releases every block the records were carved from. */
typedef struct {
    uint32_t *starts;
    size_t count, capacity;
} roster_model_key;

/* A step of the model: what it declared, or the start it withdrew. */
typedef struct {
    unsigned kind, label;
    uint32_t start;
    bool withdraws;
} roster_model_step;

static void roster_label_text(unsigned label, char *text, size_t size) { snprintf(text, size, "k%u", label); }

/* The start the model's key withdraws for a withdrawal at `start`: that
 * start when a declaration stands there, else the latest. */
static size_t roster_model_withdrawn(const roster_model_key *key, uint32_t start) {
    size_t latest = 0;
    for (size_t i = 0; i < key->count; i++) {
        if (key->starts[i] == start) {
            return i;
        }
        latest = key->starts[i] > key->starts[latest] ? i : latest;
    }
    return latest;
}

static void roster_rollback_matches_a_model(test_batch_runner *runner) {
    enum { STEPS = 6000, KINDS = 4, SHARED = 23, MARKS = 16 };
    payload_probe_arm();
    markdown_core_roster roster;
    markdown_core_roster_init(&roster);
    roster_model_step *model = malloc(STEPS * sizeof(*model));
    roster_model_key *keys = calloc((size_t)KINDS * (SHARED + STEPS), sizeof(*keys));
    size_t model_count = 0, fresh = SHARED, mark_count = 0;
    markdown_core_roster_mark marks[MARKS];
    size_t model_marks[MARKS];
    uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
    bool agrees = true;
    size_t untaken = 0, taken = 0, withdrawals = 0;
    for (size_t step = 0; step < STEPS && agrees; step++) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        unsigned choice = (unsigned)(random % 100);
        if (choice < 6 && mark_count < MARKS) {
            model_marks[mark_count] = model_count;
            marks[mark_count++] = markdown_core_roster_mark_now(&roster);
        } else if (choice < 10 && mark_count) {
            size_t to = (size_t)(random >> 32) % mark_count;
            for (size_t popped = model_marks[to]; popped < roster.count; popped++) {
                const bool indexed = popped < roster.spaces[roster.records[popped]->key[0]].cursor;
                taken += indexed;
                untaken += !indexed;
            }
            markdown_core_roster_rollback(&roster, marks[to]);
            while (model_count > model_marks[to]) {
                const roster_model_step *undone = &model[--model_count];
                roster_model_key *key = &keys[undone->kind * (SHARED + STEPS) + undone->label];
                if (undone->withdraws) {
                    key->starts[key->count++] = undone->start;
                } else {
                    for (size_t i = key->count; i-- > 0;) {
                        if (key->starts[i] == undone->start) {
                            key->starts[i] = key->starts[--key->count];
                            break;
                        }
                    }
                }
            }
            mark_count = to;
        } else {
            roster_model_step record = {(unsigned)((random >> 40) % KINDS),
                                        choice < 70 ? (unsigned)((random >> 20) % SHARED) : (unsigned)fresh++,
                                        (uint32_t)(random >> 44), false};
            roster_model_key *key = &keys[record.kind * (SHARED + STEPS) + record.label];
            /* A declared key is withdrawn about a third of the times a
             * shared label comes up, so declarations stand and fall again
             * and again. */
            record.withdraws = choice < 30 && key->count > 0;
            char text[16];
            roster_label_text(record.label, text, sizeof(text));
            const markdown_core_roster_kind kind =
                (markdown_core_roster_kind)(MARKDOWN_CORE_ROSTER_REFERENCE + record.kind);
            agrees = record.withdraws ? markdown_core_roster_withdraw(&roster, kind, (const unsigned char *)text,
                                                                      (bufsize_t)strlen(text), record.start)
                                      : markdown_core_roster_declare(&roster, kind, (const unsigned char *)text,
                                                                     (bufsize_t)strlen(text), record.start);
            if (record.withdraws) {
                const size_t at = roster_model_withdrawn(key, record.start);
                /* The step keeps the start it withdrew, which its undoing
                 * declares again. */
                record.start = key->starts[at];
                key->starts[at] = key->starts[--key->count];
            } else {
                if (key->count == key->capacity) {
                    key->capacity = key->capacity ? 2 * key->capacity : 4;
                    key->starts = realloc(key->starts, key->capacity * sizeof(*key->starts));
                }
                key->starts[key->count++] = record.start;
            }
            withdrawals += record.withdraws;
            model[model_count++] = record;
        }
        agrees = agrees && roster.count == model_count;
        const bool compare = (random >> 52) % 3 == 0 || step == STEPS - 1;
        /* The kinds compared: a random set, so one kind's records are taken
         * while another's wait; every kind at the last step. */
        const unsigned compared = step == STEPS - 1 ? (1u << KINDS) - 1 : (unsigned)(random >> 56) % (1u << KINDS);
        for (unsigned kind = 0; kind < KINDS && agrees && compare; kind++) {
            if (!(compared & (1u << kind))) {
                continue;
            }
            const markdown_core_roster_kind compared_kind =
                (markdown_core_roster_kind)(MARKDOWN_CORE_ROSTER_REFERENCE + kind);
            for (unsigned label = 0; label < fresh && agrees; label++) {
                const roster_model_key *key = &keys[kind * (SHARED + STEPS) + label];
                /* Only a label some step declared can answer; the rest are
                 * sampled every few steps, which is enough to see a key
                 * left behind. */
                if (!key->count && label >= SHARED && (label + step) % 64) {
                    continue;
                }
                uint32_t earliest = UINT32_MAX;
                for (size_t i = 0; i < key->count; i++) {
                    earliest = key->starts[i] < earliest ? key->starts[i] : earliest;
                }
                char text[16];
                roster_label_text(label, text, sizeof(text));
                bool failed = false;
                const markdown_core_roster_record *found = markdown_core_roster_find(
                    &roster, compared_kind, (const unsigned char *)text, (bufsize_t)strlen(text), &failed);
                const markdown_core_key_index_slot *slot =
                    found ? markdown_core_key_index_find(&roster.index, found->key, found->key_len) : NULL;
                agrees =
                    !failed && (found != NULL) == (key->count > 0) &&
                    (!found || (found->start == earliest && !found->withdraws && slot && slot->extra == key->count));
                if (!agrees) {
                    fprintf(stderr, "roster diverges at step %zu on kind %u label %u\n", step, kind, label);
                }
            }
        }
    }
    OK(runner, agrees, "the roster answers as the model after declarations, withdrawals, marks and rollbacks");
    OK(runner, withdrawals, "declarations were withdrawn (%zu)", withdrawals);
    OK(runner, untaken && taken, "rollbacks popped records the index took and records it never took (%zu, %zu)", taken,
       untaken);
    if (mark_count == 0 || agrees) {
        markdown_core_roster_rollback(&roster, (markdown_core_roster_mark){0});
    }
    INT_EQ(runner, (int)roster.count, 0, "a rollback to the start empties the log");
    INT_EQ(runner, (int)roster.index.size, 0, "and the index");
    OK(runner, roster.blocks == NULL, "and releases every block");
    markdown_core_roster_free(&roster);
    INT_EQ(runner, payload_live, 0, "the roster releases everything it holds");
    payload_probe_disarm();
    for (size_t i = 0; i < (size_t)KINDS * (SHARED + STEPS); i++) {
        free(keys[i].starts);
    }
    free(keys);
    free(model);
}

/* Run from the instance's own beginning -- a document owner's lifecycle --
 * because what it measures is the fresh parser's storage. */
static void inspect_lazy_block_content(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->init(self, parser);
    test_batch_runner *runner = parser->context;
    OK(runner, parser->root->content.ptr == markdown_core_strbuf__initbuf,
       "the document has no allocated content buffer");
    const markdown_core_node_type kinds[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_THEMATIC_BREAK,
                                             MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_DEFINITION_LIST};
    /* Blocks open and take content on the line the driver visits; no input
     * has begun, so stand on an empty line at the start of the source. */
    parser->input.lines.front.values[0] = (markdown_core_input_line){0};
    parser->input.first_line = parser->at.line_number;
    parser->input.lines.front.count = 1;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) {
        size_t before = payload_probe_snapshot().allocations;
        markdown_core_node *node = markdown_core_parser_add_child_validated(parser, parser->root, kinds[i], 1);
        OK(runner, node && node->content.ptr == markdown_core_strbuf__initbuf && !node->content.asize,
           "every empty block starts with the same borrowed empty content");
        INT_EQ(runner, payload_probe_snapshot().allocations, before,
               "a block fits the existing slab without a content allocation");
        if (kinds[i] == MARKDOWN_CORE_NODE_PARAGRAPH) {
            /* Warm mapping storage independently so this measures content
             * ownership, not the parser's shared source-map vector. */
            OK(runner, markdown_core_parser_mark_content(parser, node, 1, 1), "the content mapping is available");
            before = payload_probe_snapshot().allocations;
            markdown_core_chunk empty = {(unsigned char *)"", 0, 0};
            markdown_core_block_add_line(node, &empty, parser);
            INT_EQ(runner, payload_probe_snapshot().allocations, before, "an empty write acquires no storage");
            markdown_core_chunk first = {(unsigned char *)"first write\n", 12, 0};
            markdown_core_block_add_line(node, &first, parser);
            INT_EQ(runner, payload_probe_snapshot().allocations, before + 1,
                   "the first write alone acquires content storage");
            markdown_core_chunk second = {(unsigned char *)"second write\n", 13, 0};
            markdown_core_block_add_line(node, &second, parser);
            INT_EQ(runner, payload_probe_snapshot().allocations, before + 1,
                   "streaming content retains its established initial reservation");
            STR_EQ(runner, (char *)node->content.ptr, "first write\nsecond write\n",
                   "incremental writes preserve the complete value");
            unsigned char bytes[256];
            memset(bytes, 'x', sizeof(bytes));
            bytes[0] = '\t';
            markdown_core_chunk large = {bytes, sizeof(bytes), 0};
            for (int partial_tab = 0; partial_tab < 2; partial_tab++) {
                markdown_core_strbuf_free(&node->content);
                node->content_map = (markdown_core_content_map){0};
                parser->at.offset = 0;
                parser->at.column = 1;
                parser->at.partially_consumed_tab = partial_tab;
                before = payload_probe_snapshot().allocations;
                markdown_core_block_add_line(node, &large, parser);
                INT_EQ(runner, payload_probe_snapshot().allocations, before + 1,
                       "the complete first write, including tab expansion, needs one allocation");
                INT_EQ(runner, node->content.size, sizeof(bytes) + (partial_tab ? 2 : 0),
                       "first-write reservation preserves expanded content length");
                OK(runner, !memcmp(node->content.ptr + (partial_tab ? 3 : 1), bytes + 1, sizeof(bytes) - 1),
                   "first-write reservation preserves the copied source bytes");
                if (partial_tab) {
                    OK(runner, !memcmp(node->content.ptr, "   ", 3), "the partial tab writes its remaining spaces");
                }
            }
            parser->at.offset = 0;
            parser->at.column = 0;
            parser->at.partially_consumed_tab = false;
        }
        /* The probe block leaves the builder stack and the root's children
         * as it is released: the root is the deepest open block again. */
        parser->at.current = parser->root;
        markdown_core_node_array_pop(parser->root->children, &parser->root->child_count);
        markdown_core_parser_release_node(parser, node);
    }
    parser->input.lines.front.count = 0;
}

static bool begin_with_lazy_block_inspection(markdown_core_dialect_builder *builder, void *context) {
    static document_owner document;
    (void)context;
    return attach_document_owner(builder, &document, "lazy-block-inspector", inspect_lazy_block_content);
}

static void block_content_storage_follows_writes(test_batch_runner *runner) {
    payload_probe_arm();
    markdown_core_node *root = markdown_core_parse_document_with_setup("", 0, begin_with_lazy_block_inspection, runner);
    OK(runner, root != NULL, "probing empty block storage preserves the parse");
    markdown_core_node_free(root);
    INT_EQ(runner, payload_live, 0, "lazy content and parser resources are released");
    payload_probe_disarm();
}

static void node_payload_lifecycle(test_batch_runner *runner) {
    payload_probe_arm();
    INT_EQ(runner, sizeof(markdown_core_node_data), sizeof(void *),
           "all payload arms share one pointer-sized node slot");
    static const markdown_core_node_type extra_types[] = {
        MARKDOWN_CORE_NODE_METADATA,      MARKDOWN_CORE_NODE_INSERTION,       MARKDOWN_CORE_NODE_CROSS_EMBEDDED,
        MARKDOWN_CORE_NODE_MARK,          MARKDOWN_CORE_NODE_CROSS_LINK,      MARKDOWN_CORE_NODE_CITE,
        MARKDOWN_CORE_NODE_CITATION,      MARKDOWN_CORE_NODE_FOOTNOTE,        MARKDOWN_CORE_NODE_SPECIMEN,
        MARKDOWN_CORE_NODE_TABLE,         MARKDOWN_CORE_NODE_TABLE_ROW,       MARKDOWN_CORE_NODE_TABLE_CELL,
        MARKDOWN_CORE_NODE_STRIKETHROUGH, MARKDOWN_CORE_NODE_FORMULA,         MARKDOWN_CORE_NODE_FORMULA_BLOCK,
        MARKDOWN_CORE_NODE_DIRECTIVE,     MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK, MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
    };
    size_t extra_count = sizeof(extra_types) / sizeof(*extra_types);
    for (size_t i = 0; i < (size_t)num_node_types + extra_count; i++) {
        markdown_core_node_type type = i < (size_t)num_node_types ? node_types[i] : extra_types[i - num_node_types];
        payload_allocations = payload_fail_at = 0;
        markdown_core_node *node = markdown_core_node_new(type);
        OK(runner, node != NULL, "type %u constructs with its default fields", (unsigned)type);
        size_t total = payload_allocations;
        /* One cell holds the node and its record, or the record is owned
         * apart from the cell as a replacement would be: the release path
         * has one rule for both, and the second allocation is the proof
         * that the rule is being exercised for the kinds that need it. */
        INT_EQ(runner, total, node->node_data_allocation ? 2 : 1,
               "node and initial typed record are one allocation, or two when the record does not fit the cell");
        if (node->as.data) {
            OK(runner,
               (uintptr_t)node->as.data % offsetof(payload_float_alignment, value) == 0 &&
                   (uintptr_t)node->as.data % offsetof(payload_integer_alignment, value) == 0,
               "initial typed record has scalar alignment");
        }
        markdown_core_node_free(node);
        INT_EQ(runner, payload_live, 0, "type %u releases every constructor allocation", (unsigned)type);
        for (size_t fail = 1; fail <= total; fail++) {
            payload_allocations = 0;
            payload_fail_at = fail;
            node = markdown_core_node_new(type);
            OK(runner, node == NULL, "type %u never publishes a partial payload", (unsigned)type);
            if (node) {
                markdown_core_node_free(node);
            }
            INT_EQ(runner, payload_live, 0, "failed constructor releases all acquired allocations");
        }
    }
    payload_fail_at = 0;
    markdown_core_node *parent = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    markdown_core_node *text = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    OK(runner, append_child(parent, text), "text joins its parent");
    OK(runner, set_literal(text, "retained"), "text owns a literal");
    markdown_core_chunk *original_payload = text->as.literal;
    size_t before = payload_live;
    payload_fail_at = payload_allocations + 1;
    INT_EQ(runner, markdown_core_node_set_kind(text, parent, MARKDOWN_CORE_NODE_TEXT), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "setting the current kind preserves its data without allocation");
    INT_EQ(runner, payload_allocations + 1, payload_fail_at, "setting the current kind allocates nothing");
    INT_EQ(runner, markdown_core_node_set_kind(text, parent, MARKDOWN_CORE_NODE_CROSS_EMBEDDED),
           MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED, "conversion reports replacement allocation failure");
    OK(runner,
       text->kind == MARKDOWN_CORE_NODE_TEXT && text->as.literal == original_payload && child_at(parent, 0) == text,
       "failed kind conversion preserves data, identity, and tree links");
    LITERAL_EQ(runner, text, "retained", "failed retyping retains owned bytes");
    INT_EQ(runner, payload_live, before, "failed retyping neither frees nor leaks an allocation");
    /* Inline replacement needs no allocation, even when the next allocation
     * is armed to fail. Its cell is already owned by this node. */
    payload_fail_at = payload_allocations + 1;
    size_t inline_attempts = payload_allocations;
    INT_EQ(runner, markdown_core_node_set_kind(text, parent, MARKDOWN_CORE_NODE_LINK), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "successful kind conversion installs new defaults");
    OK(runner, text->as.link && !text->as.link->resource && !text->node_data_allocation,
       "converted link reuses its cell with empty defaults");
    INT_EQ(runner, payload_allocations, inline_attempts, "inline replacement does not attempt allocation");
    size_t attempts = payload_allocations;
    markdown_core_link *original_link = text->as.link;
    payload_fail_at = attempts + 1;
    INT_EQ(runner, markdown_core_node_set_kind(text, parent, MARKDOWN_CORE_NODE_CODE_BLOCK),
           MARKDOWN_CORE_NODE_SET_KIND_REJECTED, "containment rejects a block in a paragraph");
    INT_EQ(runner, payload_allocations, attempts, "invalid containment allocates nothing");
    OK(runner, text->kind == MARKDOWN_CORE_NODE_LINK && text->as.link == original_link && child_at(parent, 0) == text,
       "containment rejection preserves kind, data, identity, and tree links");
    payload_fail_at = 0;
    OK(runner,
       markdown_core_node_set_kind(text, parent, MARKDOWN_CORE_NODE_STRONG) == MARKDOWN_CORE_NODE_SET_KIND_OK &&
           !text->as.data,
       "a fieldless kind releases the old payload without creating an empty record");

    markdown_core_node *empty = markdown_core_node_new(MARKDOWN_CORE_NODE_EMPHASIS);
    OK(runner, append_child(parent, empty), "a fieldless node joins the parent");
    INT_EQ(runner, markdown_core_node_set_kind(empty, parent, MARKDOWN_CORE_NODE_CROSS_LINK),
           MARKDOWN_CORE_NODE_SET_KIND_OK, "a node constructed without fields acquires an inline replacement record");
    OK(runner, !empty->node_data_allocation, "a previously unused cell record becomes active");
    markdown_core_destination destination = facade_destination(empty);
    markdown_core_optional_string label = facade_cross_label(empty);
    OK(runner, destination.path.length == 0 && !destination.anchor.has_value && !label.has_value,
       "converted cross link establishes ordinary empty and absent defaults");

    markdown_core_node *cite = markdown_core_node_new(MARKDOWN_CORE_NODE_CITE);
    markdown_core_node *item = markdown_core_node_new(MARKDOWN_CORE_NODE_CITATION);
    markdown_core_node *prefix = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    OK(runner, append_child(parent, cite), "cite joins its parent");
    OK(runner, append_child(cite, item), "the citation joins its cite");
    item->as.citation->prefix = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    append_child(item->as.citation->prefix, prefix);
    OK(runner, set_literal(prefix, "prefix"), "citation owns an affix subtree");
    payload_fail_at = payload_allocations + 1;
    before = payload_live;
    OK(runner,
       markdown_core_node_set_kind(cite, parent, MARKDOWN_CORE_NODE_CROSS_EMBEDDED) ==
               MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED &&
           child_at(cite, 0) == item && child_at(item->as.citation->prefix, 0) == prefix,
       "failed retyping preserves children and node-valued fields");
    INT_EQ(runner, payload_live, before, "failed retyping leaves the owned subtree alive");
    payload_fail_at = 0;
    INT_EQ(runner, markdown_core_node_set_kind(cite, parent, MARKDOWN_CORE_NODE_TEXT), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "kind conversion releases the old owned subtrees");
    markdown_core_node_free(parent);
    INT_EQ(runner, payload_live, 0, "conversion and destruction release payloads, fields, and affixes exactly once");
    payload_probe_disarm();
}

static void kind_conversion_reuses_cell_storage(test_batch_runner *runner) {
    for (int pooled = 0; pooled < 2; pooled++) {
        payload_probe_arm();
        markdown_core_node_pool pool = {0};
        markdown_core_node_pool *source = pooled ? &pool : NULL;
        markdown_core_node *parent = markdown_core_node_pool_new(source, MARKDOWN_CORE_NODE_PARAGRAPH, NULL);
        markdown_core_node *node = markdown_core_node_pool_new(source, MARKDOWN_CORE_NODE_TEXT, NULL);
        OK(runner, append_child(parent, node), "the convertible node joins its parent");
        void *cell_record = node->as.data;
        markdown_core_node_pool_dispose(&pool);
        OK(runner, set_literal(node, "owned text"), "the old inline record owns bytes");

        size_t attempts = payload_allocations;
        payload_fail_at = attempts + 1;
        INT_EQ(runner, markdown_core_node_set_kind(node, parent, MARKDOWN_CORE_NODE_LINK),
               MARKDOWN_CORE_NODE_SET_KIND_OK,
               "inline replacement succeeds with allocation refused, even after pool disposal");
        OK(runner, node->as.data == cell_record && !node->node_data_allocation && !node->as.link->resource,
           "inline replacement reuses the original record address with new defaults");
        INT_EQ(runner, payload_allocations, attempts, "inline replacement makes no allocator call");
        INT_EQ(runner, markdown_core_node_set_kind(node, parent, MARKDOWN_CORE_NODE_CROSS_EMBEDDED),
               MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED, "external replacement can still fail atomically");
        OK(runner, node->kind == MARKDOWN_CORE_NODE_LINK && node->as.data == cell_record && child_at(parent, 0) == node,
           "failed external replacement retains the inline record and tree links");

        payload_fail_at = 0;
        INT_EQ(runner, markdown_core_node_set_kind(node, parent, MARKDOWN_CORE_NODE_CROSS_EMBEDDED),
               MARKDOWN_CORE_NODE_SET_KIND_OK, "a record exceeding capacity obtains external storage");
        void *external = node->node_data_allocation;
        OK(runner, external && node->as.data == external && external != cell_record,
           "external allocation ownership is explicit");
        OK(runner, markdown_core_chunk_set_cstr(&node->as.cross_embedded->reference.path, "owned path"),
           "the external record owns a string");
        attempts = payload_allocations;
        payload_fail_at = attempts + 1;
        INT_EQ(runner, markdown_core_node_set_kind(node, parent, MARKDOWN_CORE_NODE_CROSS_EMBEDDED),
               MARKDOWN_CORE_NODE_SET_KIND_OK, "the same external kind preserves its record without allocation");
        OK(runner, node->node_data_allocation == external, "no-op conversion preserves external ownership");
        STR_EQ(runner, (char *)node->as.cross_embedded->reference.path.data, "owned path",
               "no-op conversion preserves external fields");

        size_t releases = payload_releases;
        INT_EQ(runner, markdown_core_node_set_kind(node, parent, MARKDOWN_CORE_NODE_TEXT),
               MARKDOWN_CORE_NODE_SET_KIND_OK, "external to inline conversion succeeds with allocation refused");
        INT_EQ(runner, payload_allocations, attempts, "returning to the cell makes no allocation");
        INT_EQ(runner, payload_releases, releases + 2, "the external record and its string are each released once");
        OK(runner, node->as.data == cell_record && !node->node_data_allocation && !node->as.literal->data,
           "returning to the cell reinitializes its former record bytes");
        INT_EQ(runner, markdown_core_node_set_kind(node, parent, MARKDOWN_CORE_NODE_STRONG),
               MARKDOWN_CORE_NODE_SET_KIND_OK, "a fieldless kind releases its active record without storage work");
        OK(runner, !node->as.data && !node->node_data_allocation, "a fieldless kind owns no record");
        payload_fail_at = 0;
        markdown_core_node_free(parent);
        INT_EQ(runner, payload_live, 0, "all record backing and detached slab storage is released");
        payload_probe_disarm();
    }
}

/* Element fields use the same nonrecursive ownership walk as typed fields.
 * The probe has two independent roots and ordinary children, so a destructor
 * that visits only the content chain cannot satisfy the lifetime contract. */
typedef struct {
    markdown_core_node *first, *second;
} owned_field_probe;
static size_t owned_field_releases, owned_field_uncleared;

static void owned_field_alloc(const markdown_core_element *element, markdown_core_node *node) {
    (void)element;
    node->opaque = markdown_core_alloc(1, sizeof(owned_field_probe));
}

static int owned_field_visit(const markdown_core_element *element, markdown_core_node *node,
                             markdown_core_owned_subtree_visitor visitor, void *context) {
    (void)element;
    owned_field_probe *fields = node->opaque;
    return !fields || (visitor(&fields->first, context) && visitor(&fields->second, context));
}

static void owned_field_free(const markdown_core_element *element, markdown_core_node *node) {
    (void)element;
    owned_field_probe *fields = node->opaque;
    owned_field_releases++;
    owned_field_uncleared += fields->first != NULL || fields->second != NULL;
    /* Clean up even on a failing test against the old destructor. */
    if (fields->first) {
        markdown_core_node_free(fields->first);
    }
    if (fields->second) {
        markdown_core_node_free(fields->second);
    }
    markdown_core_free(fields);
    node->opaque = NULL;
}

static const markdown_core_opaque_ops OWNED_FIELD_PROBE_OPAQUE = {
    .alloc = owned_field_alloc,
    .free = owned_field_free,
    .visit = owned_field_visit,
};

static const markdown_core_element OWNED_FIELD_PROBE = {
    .name = "owned-field-lifetime-probe",
    .opaque = &OWNED_FIELD_PROBE_OPAQUE,
};

static void element_owned_field_lifecycle(test_batch_runner *runner) {
    payload_probe_arm();
    payload_allocations = payload_fail_at = payload_live = 0;
    owned_field_releases = owned_field_uncleared = 0;
    markdown_core_node *root = NULL;
    const size_t depth = 4096;
    for (size_t i = 0; i < depth; i++) {
        markdown_core_node *owner = markdown_core_node_new_with_ext(MARKDOWN_CORE_NODE_PARAGRAPH, &OWNED_FIELD_PROBE);
        owned_field_probe *fields = owner->opaque;
        fields->first = root;
        fields->second = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
        append_child(owner, markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT));
        root = owner;
    }
    markdown_core_node *document = markdown_core_node_new(MARKDOWN_CORE_NODE_DOCUMENT);
    append_child(document, root);
    owned_field_probe *retained = root->opaque;
    markdown_core_node *retained_first = retained->first;
    OK(runner,
       markdown_core_node_set_kind(root, document, MARKDOWN_CORE_NODE_HEADING) == MARKDOWN_CORE_NODE_SET_KIND_OK &&
           root->opaque == retained && retained->first == retained_first,
       "kind conversion preserves the element's opaque state and owned fields");
    size_t before = payload_allocations;
    payload_fail_at = before + 1;
    markdown_core_node_free(document);
    INT_EQ(runner, payload_allocations, before, "element-owned tree destruction allocates nothing");
    INT_EQ(runner, owned_field_releases, depth, "each element payload is released exactly once");
    INT_EQ(runner, owned_field_uncleared, 0, "the shared destructor takes each field before releasing its payload");
    INT_EQ(runner, payload_live, 0, "deep owned fields and ordinary children release every allocation");
    payload_fail_at = 0;
    const char *source = ":a[label]\n";
    document = markdown_core_parse_document_with_setup(source, strlen(source), NULL, NULL);
    OK(runner, document != NULL, "directive with an owned label parses using the tracked allocator");
    if (document) {
        markdown_core_node *directive = child_at(child_at(document, 0), 0);
        markdown_core_node *label = markdown_core_directive_label(directive);
        OK(runner,
           label && markdown_core_node_set_kind(directive, child_at(document, 0), MARKDOWN_CORE_NODE_EMPHASIS) ==
                        MARKDOWN_CORE_NODE_SET_KIND_OK,
           "directive kind conversion preserves its element-owned label");
        before = payload_allocations;
        payload_fail_at = before + 1;
        markdown_core_node_free(document);
        INT_EQ(runner, payload_allocations, before, "converted directive destruction allocates nothing");
        INT_EQ(runner, payload_live, 0, "a converted directive releases fields hidden by its new kind");
        payload_fail_at = 0;
    }
    payload_probe_disarm();
}

typedef struct {
    markdown_core_node_type rejected_kind;
    size_t rejections;
    size_t allowed;
} conversion_policy;

/* The policy of the parse or case under test: a containment hook takes no
 * context of its own, and these run one at a time. */
static conversion_policy *conversion_current_policy;

static int conversion_can_contain(const markdown_core_element *element, markdown_core_node *node,
                                  markdown_core_node_type child_kind) {
    conversion_policy *policy = conversion_current_policy;
    (void)element;
    if (child_kind == policy->rejected_kind) {
        if (policy->allowed) {
            policy->allowed--;
            return true;
        }
        policy->rejections++;
        return false;
    }
    return node->kind == MARKDOWN_CORE_NODE_DOCUMENT
               ? MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_kind) && child_kind != MARKDOWN_CORE_NODE_LIST_ITEM
               : node->kind == MARKDOWN_CORE_NODE_PARAGRAPH && MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_kind);
}

/* A literal ! lets this probe set the parent policy before the following
 * delimiter, with every production element still in its fixed order. */
static markdown_core_node *conversion_match_inline(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, markdown_core_node *parent,
                                                   unsigned char character, markdown_core_inline_state *inline_state) {
    (void)character;
    (void)inline_state;
    parent->element = self->element;
    conversion_current_policy = parser->context;
    return NULL;
}

static const markdown_core_contain_ops CONVERSION_POLICY_CONTAIN = {
    .allows = conversion_can_contain,
};

static const markdown_core_inline_ops CONVERSION_POLICY_INLINES = {
    .match = conversion_match_inline,
    .dispatch = "!",
};

static const markdown_core_element CONVERSION_POLICY = {
    .name = "conversion-policy",
    .contain = &CONVERSION_POLICY_CONTAIN,
    .inlines = &CONVERSION_POLICY_INLINES,
};

/* The root's policy is set as the instance begins, by a document owner that
 * begins the core document's lifecycle first. */
static void begin_conversion_policy(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->init(self, parser);
    parser->root->element = &CONVERSION_POLICY;
    conversion_current_policy = parser->context;
}

static bool configure_conversion_policy(markdown_core_dialect_builder *builder, void *context) {
    static document_owner document;
    (void)context;
    return attach_document_owner(builder, &document, "conversion-policy-document", begin_conversion_policy) &&
           markdown_core_dialect_builder_attach(builder, &CONVERSION_POLICY);
}

static void kind_conversion_containment(test_batch_runner *runner) {
    payload_probe_arm();
    static const struct {
        markdown_core_node_type rejected_kind, retained_kind;
        const char *source, *retained_literal;
        /* The source also defines a footnote, which stays where it is
         * written, after the retained block. */
        bool defines;
    } cases[] = {
        {MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_PARAGRAPH, "| h |\n| - |\n", "| h |\n| - |", false},
        {MARKDOWN_CORE_NODE_HEADING, MARKDOWN_CORE_NODE_PARAGRAPH, "heading\n===\n", "heading\n===", false},
        {MARKDOWN_CORE_NODE_COMMENT_BLOCK, MARKDOWN_CORE_NODE_HTML_BLOCK, "<!-- body -->\n", "<!-- body -->\n", false},
        {MARKDOWN_CORE_NODE_STRIKETHROUGH, MARKDOWN_CORE_NODE_PARAGRAPH, "!~~text~~\n", "!~~text~~", false},
        {MARKDOWN_CORE_NODE_SPAN, MARKDOWN_CORE_NODE_PARAGRAPH, "![text]{}\n", "![text]{}", false},
        {MARKDOWN_CORE_NODE_LINK, MARKDOWN_CORE_NODE_PARAGRAPH, "! [text](u)\n", "! [text](u)", false},
        {MARKDOWN_CORE_NODE_LINK, MARKDOWN_CORE_NODE_PARAGRAPH, "! a@b.co\n", "! a@b.co", false},
        {MARKDOWN_CORE_NODE_EMBEDDED, MARKDOWN_CORE_NODE_PARAGRAPH, "![text](u)\n", "![text](u)", false},
        {MARKDOWN_CORE_NODE_CITE, MARKDOWN_CORE_NODE_PARAGRAPH, "!^[text]\n", "!^[text]", false},
        {MARKDOWN_CORE_NODE_CITE, MARKDOWN_CORE_NODE_PARAGRAPH, "![^n]\n\n[^n]: text\n", "![^n]", true},
        {MARKDOWN_CORE_NODE_EMPHASIS, MARKDOWN_CORE_NODE_PARAGRAPH, "!*text*\n", "!*text*", false},
        {MARKDOWN_CORE_NODE_STRONG, MARKDOWN_CORE_NODE_PARAGRAPH, "!**text**\n", "!**text**", false},
        {MARKDOWN_CORE_NODE_MARK, MARKDOWN_CORE_NODE_PARAGRAPH, "!==text==\n", "!==text==", false},
        {MARKDOWN_CORE_NODE_INSERTION, MARKDOWN_CORE_NODE_PARAGRAPH, "!++text++\n", "!++text++", false},
        {MARKDOWN_CORE_NODE_SUPERSCRIPT, MARKDOWN_CORE_NODE_PARAGRAPH, "!^text^\n", "!^text^", false},
        {MARKDOWN_CORE_NODE_SUBSCRIPT, MARKDOWN_CORE_NODE_PARAGRAPH, "!~text~\n", "!~text~", false},
        {MARKDOWN_CORE_NODE_FORMULA, MARKDOWN_CORE_NODE_PARAGRAPH, "!$text$\n", "!$text$", false},
        {MARKDOWN_CORE_NODE_FORMULA_BLOCK, MARKDOWN_CORE_NODE_CODE_BLOCK, "```formula\nx\n```\n", "x\n", false},

    };
    payload_fail_at = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        size_t live_before = payload_live;
        conversion_policy policy = {cases[i].rejected_kind, 0, 0};
        markdown_core_node *root = markdown_core_parse_document_with_setup(cases[i].source, strlen(cases[i].source),
                                                                           configure_conversion_policy, &policy);
        OK(runner, policy.rejections > 0, "case %zu exercises parent containment rejection", i);
        OK(runner, root != NULL, "case %zu declines conversion without failing the parse", i);
        if (!root) {
            continue;
        }
        markdown_core_node *block = child_at(root, 0);
        const markdown_core_node *after = block ? child_at(root, 1) : NULL;
        if (cases[i].defines && after && after->kind == MARKDOWN_CORE_NODE_FOOTNOTE) {
            after = child_at(root, 2);
        } else if (cases[i].defines) {
            after = block;
        }
        OK(runner, block && block->kind == cases[i].retained_kind && !after, "case %zu retains the original block", i);
        markdown_core_strbuf literal = MARKDOWN_CORE_BUF_INIT();
        if (block && (block->kind == MARKDOWN_CORE_NODE_HTML_BLOCK || block->kind == MARKDOWN_CORE_NODE_CODE_BLOCK)) {
            put_literal(&literal, block);
        } else if (block) {
            for (size_t child_index = 0; child_index < block->child_count; child_index++) {
                markdown_core_node *child = block->children[child_index];
                OK(runner, child->kind == MARKDOWN_CORE_NODE_TEXT || child->kind == MARKDOWN_CORE_NODE_SOFT_BREAK,
                   "case %zu retains text and line breaks", i);
                if (child->kind == MARKDOWN_CORE_NODE_TEXT) {
                    put_literal(&literal, child);
                } else if (child->kind == MARKDOWN_CORE_NODE_SOFT_BREAK) {
                    markdown_core_strbuf_putc(&literal, '\n');
                }
            }
        }
        STR_EQ(runner, (char *)literal.ptr, cases[i].retained_literal, "case %zu preserves authored content", i);
        markdown_core_strbuf_free(&literal);
        markdown_core_node_free(root);
        INT_EQ(runner, payload_live, live_before, "case %zu: rejected containers release every allocation", i);
    }
    payload_probe_disarm();
}

/* A byte offset and an indentation column are different coordinates. Splitting
 * byte advances anywhere, including inside a scalar, must compose identically;
 * a column advance can split a tab but must finish a whole scalar. */
static void block_cursor_coordinates(test_batch_runner *runner) {
    const char *markers[] = {"?", "é", "✓", "🚀", "́"};
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        char input[32];
        int length = snprintf(input, sizeof(input), "%s\t%s\t%sX", markers[i], markers[i], markers[i]);
        int width = (int)strlen(markers[i]);
        for (int split = 0; split <= length; split++) {
            markdown_core_parser cursor = {0};
            markdown_core_parser_advance_offset(&cursor, input, split, false);
            markdown_core_parser_advance_offset(&cursor, input, length - split, false);
            OK(runner, cursor.at.offset == length && cursor.at.column == 10 && !cursor.at.partially_consumed_tab,
               "byte advances compose across scalar and tab boundaries (%s, %d)", markers[i], split);
        }
        markdown_core_parser cursor = {0};
        markdown_core_parser_advance_offset(&cursor, input, 1, true);
        OK(runner, cursor.at.offset == width && cursor.at.column == 1, "one column consumes one whole scalar");
        markdown_core_parser_advance_offset(&cursor, input, 2, true);
        OK(runner, cursor.at.offset == width && cursor.at.column == 3 && cursor.at.partially_consumed_tab,
           "a tab after any scalar expands from its virtual column");
        markdown_core_parser_advance_offset(&cursor, input, 7, true);
        OK(runner, cursor.at.offset == length && cursor.at.column == 10 && !cursor.at.partially_consumed_tab,
           "resuming a partial tab preserves later scalar and tab coordinates");
    }
}

/* All block facts except the authored marker and byte coordinates must be
 * invariant under replacing a single-scalar marker with another UTF-8 width.
 * The generated trees have bounded depth and contain only lists and leaves. */
static bool task_block_facts_equal(markdown_core_node *a, markdown_core_node *b) {
    if (!a || !b) {
        return a == b;
    }
    if (a->kind != b->kind || a->child_count != b->child_count) {
        return false;
    }
    markdown_core_string a_literal, b_literal;
    bool a_has = literal_of(a, &a_literal), b_has = literal_of(b, &b_literal);
    if (a_has != b_has || (a_has && !strings_equal(a_literal, b_literal))) {
        return false;
    }
    if (a->kind == MARKDOWN_CORE_NODE_LIST &&
        (a->as.list->flavor != b->as.list->flavor || a->as.list->start != b->as.list->start ||
         a->as.list->delimiter.kind != b->as.list->delimiter.kind ||
         a->as.list->delimiter.closed != b->as.list->delimiter.closed ||
         a->as.list->variant.kind != b->as.list->variant.kind ||
         a->as.list->variant.lowercased != b->as.list->variant.lowercased || a->as.list->tight != b->as.list->tight)) {
        return false;
    }
    for (size_t i = 0; i < a->child_count; i++) {
        if (!task_block_facts_equal(a->children[i], b->children[i])) {
            return false;
        }
    }
    return true;
}

static void task_marker_tab_structure(test_batch_runner *runner) {
    const char *markers[] = {"é", "✓", "🚀", "́"};
    const char *separators[] = {" ", "\t", " \t", "\t ", "  ", " \t \t"};
    const char *padding[] = {" ", "\t", " \t", "\t ", " \t ", "\t\t"};
    const char *lists[] = {"-", "1.", "1)"};
    for (int indent = 0; indent < 4; indent++) {
        for (size_t sep = 0; sep < sizeof(separators) / sizeof(separators[0]); sep++) {
            for (size_t pad = 0; pad < sizeof(padding) / sizeof(padding[0]); pad++) {
                for (size_t list = 0; list < sizeof(lists) / sizeof(lists[0]); list++) {
                    char source[128];
                    int length = snprintf(source, sizeof(source), "%*s- [?]%s%s%schild\n", indent, "", separators[sep],
                                          lists[list], padding[pad]);
                    markdown_core_node *baseline = markdown_core_parse_document(source, length);
                    OK(runner, baseline != NULL, "ASCII task baseline parses");
                    for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); m++) {
                        length = snprintf(source, sizeof(source), "%*s- [%s]%s%s%schild\n", indent, "", markers[m],
                                          separators[sep], lists[list], padding[pad]);
                        markdown_core_node *actual = markdown_core_parse_document(source, length);
                        OK(runner, baseline && actual && task_block_facts_equal(baseline, actual),
                           "task marker width cannot change block facts (indent=%d sep=%zu pad=%zu list=%zu marker=%s)",
                           indent, sep, pad, list, markers[m]);
                        markdown_core_node_free(actual);
                    }
                    markdown_core_node_free(baseline);
                }
            }
        }
    }
}

static void task_marker_ownership(test_batch_runner *runner) {
    static const char original[] = "- [ ] open\n- [X] done\n- ordinary\n- [🚀] custom\n";
    char source[sizeof(original)];
    memcpy(source, original, sizeof(original));
    markdown_core_document *document = facade_parse((const uint8_t *)source, strlen(source));
    OK(runner, document != NULL, "task marker ownership document parses");
    if (!document) {
        return;
    }
    memset(source, '?', sizeof(source) - 1);
    markdown_core_node *item = child_at(child_at(document->root, 0), 0);
    markdown_core_optional_string marker = facade_list_item_marker(item);
    OK(runner, marker.has_value && marker.value.length == 1 && marker.value.data[0] == ' ',
       "incomplete marker survives input reuse");
    OK(runner, item->as.list->task_marker.value.alloc, "parsed task owns its marker bytes");
    marker = facade_list_item_marker(child_at(child_at(document->root, 0), 1));
    OK(runner, marker.has_value && marker.value.length == 1 && marker.value.data[0] == 'X',
       "completed marker retains authored case");
    marker = facade_list_item_marker(child_at(child_at(document->root, 0), 2));
    OK(runner, !marker.has_value && marker.value.data == NULL && marker.value.length == 0,
       "ordinary item has an absent marker");

    marker = facade_list_item_marker(child_at(child_at(document->root, 0), 3));
    OK(runner, marker.has_value && marker.value.length == 4 && memcmp(marker.value.data, "🚀", 4) == 0,
       "parsed custom marker survives input reuse with its complete UTF-8 spelling");
    uint8_t *dump = NULL;
    size_t length = 0;
    OK(runner,
       (markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)original,
                                    sizeof(original) - 1, &dump, &length) == MARKDOWN_CORE_OK),
       "UTF-8 marker document dumps");
    OK(runner, dump && strstr((const char *)dump, "marker=\"🚀\""), "dump preserves UTF-8 marker spelling");
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);

    item = markdown_core_node_new(MARKDOWN_CORE_NODE_LIST_ITEM);
    markdown_core_chunk bytes = markdown_core_chunk_literal("🚀");
    OK(runner, markdown_core_chunk_to_cstr(&bytes) != NULL, "custom marker allocates");
    item->as.list->task_marker = markdown_core_optional_chunk_present(bytes);
    marker_to_free = bytes.data;
    marker_free_count = 0;
    markdown_core_node_free(item);
    INT_EQ(runner, marker_free_count, 1, "destroying an item frees its owned marker exactly once");
}

/* SPECIMEN VALUES. A definition is a block where it was written: it owns its
 * authored label and explicit reset and its block content, the document lists
 * every definition in source order, and a reference names one by label. */
static void specimen_values(test_batch_runner *runner) {
    static const char source[] = "(5@étude) Body.\n(@) Anonymous.\n\n@étude\n";
    const size_t length = sizeof(source) - 1;
    markdown_core_document *document = facade_parse((const uint8_t *)source, length);
    markdown_core_optional_string label;
    markdown_core_optional_i64 start;
    OK(runner, document != NULL, "specimen document parses");
    if (!document) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *value = facade_child(root, 0);
    INT_EQ(runner, markdown_core_node_get_kind(value), MARKDOWN_CORE_KIND_SPECIMEN, "a definition is content");
    EXPECT_OK(markdown_core_specimen_properties(value, &label, &start));
    OK(runner, label.has_value && label.value.length == 6 && memcmp(label.value.data, "étude", 6) == 0,
       "specimen label retains owned UTF-8 bytes");
    OK(runner, start.has_value && start.value == 5, "specimen retains effective reset");
    OK(runner,
       markdown_core_node_get_kind(facade_nodes(value, markdown_core_specimen_content).items[0]) ==
           MARKDOWN_CORE_KIND_PARAGRAPH,
       "specimen retains its content relation");
    const markdown_core_node *anonymous = facade_child(root, 1);
    OK(runner, anonymous != NULL, "anonymous definition remains present");
    EXPECT_OK(markdown_core_specimen_properties(anonymous, &label, &start));
    OK(runner, !label.has_value && !start.has_value, "anonymous label and absent reset remain absent");
    INT_EQ(runner, (int)markdown_core_document_specimen_count(document), 2, "the document lists both definitions");
    OK(runner, facade_specimen_at(document, 0) == value && facade_specimen_at(document, 1) == anonymous,
       "the list is in source order");
    const markdown_core_node *citation =
        facade_nodes(facade_child(facade_child(root, 2), 0), markdown_core_node_cite_citations).items[0];
    markdown_core_referent referent = facade_referent(citation);
    OK(runner,
       referent.kind == MARKDOWN_CORE_REFERENT_SPECIMEN && referent.label.length == 6 && referent.key.data == NULL &&
           referent.mode == 0 && referent.note == NULL,
       "specimen reference uses only its own branch fields");
    OK(runner, markdown_core_document_specimen_for(document, referent.label) == value,
       "a reference's label finds its definition");
    uint8_t *dump = NULL;
    size_t dump_length = 0;
    OK(runner,
       (markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)source, length,
                                    &dump, &dump_length) == MARKDOWN_CORE_OK),
       "specimen document dumps");
    OK(runner,
       dump && strstr((const char *)dump, "label=\"étude\" start=5 children=1") &&
           strstr((const char *)dump, "label=null start=null children=1") &&
           strstr((const char *)dump, "referent=specimen(label=\"étude\")"),
       "dump carries only authored definition facts");
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);
}

/* Blank continuation belongs to the definition after ancestor prefixes have
 * been consumed. The same rule must hold during block-start lookahead. */
static void definition_blank_continuation(test_batch_runner *runner) {
    static const struct {
        const char *opening, *continuation, *blank;
    } containers[] = {
        {"", "", ""}, {"> ", "> ", ">"}, {"- ", "  ", "  "}, {"> - ", ">   ", ">"}, {"- > ", "  > ", "  >"}};
    static const char *markers[] = {"(@a) ", "[^a]: "};
    static const char *endings[] = {"\n", "\r\n", "\r"};
    static const char *whitespace[] = {"", " ", "   ", "\t"};
    for (size_t family = 0; family < 2; family++) {
        for (size_t shape = 0; shape < sizeof(containers) / sizeof(*containers); shape++) {
            for (size_t ending = 0; ending < 3; ending++) {
                for (size_t blank = 0; blank < 4; blank++) {
                    for (int definition_list = 0; definition_list <= 1; definition_list++) {
                        char source[512];
                        const char *eol = endings[ending];
                        const char *prefix = containers[shape].continuation;
                        const char *blank_prefix = containers[shape].blank;
                        const char *spaces = whitespace[blank];
                        int length = snprintf(source, sizeof(source), "%s%sfirst%s%s%s%s%s    %s%s",
                                              containers[shape].opening, markers[family], eol, blank_prefix, spaces,
                                              eol, prefix, definition_list ? "Term" : "second", eol);
                        if (definition_list) {
                            length += snprintf(source + length, sizeof(source) - (size_t)length, "%s%s%s%s    : body%s",
                                               blank_prefix, spaces, eol, prefix, eol);
                        }
                        snprintf(source + length, sizeof(source) - (size_t)length, "%s%s%s%soutside", blank_prefix,
                                 spaces, eol, prefix);
                        markdown_core_node *root = markdown_core_parse_document(source, strlen(source));
                        OK(runner, root != NULL, "definition continuation parses");
                        if (!root) {
                            continue;
                        }
                        markdown_core_node *value =
                            first_of_kind(root, family ? MARKDOWN_CORE_NODE_FOOTNOTE : MARKDOWN_CORE_NODE_SPECIMEN);
                        markdown_core_node *first = child_at(value, 0);
                        markdown_core_node *second = child_at(value, 1);
                        OK(runner, value && first && second && !child_at(value, 2),
                           "definition owns exactly two blocks: family=%zu shape=%zu ending=%zu blank=%zu list=%d",
                           family, shape, ending, blank, definition_list);
                        LITERAL_EQ(runner, child_at(first, 0), "first", "blank line ends the first paragraph");
                        INT_EQ(runner, second->kind,
                               definition_list ? MARKDOWN_CORE_NODE_DEFINITION_LIST : MARKDOWN_CORE_NODE_PARAGRAPH,
                               "continued body and lookahead retain the block kind");
                        if (second) {
                            INT_EQ(runner, START_LINE(second), 3, "continued block retains its source start");
                            INT_EQ(runner, END_LINE(second), definition_list ? 5 : 3,
                                   "unindented text stays outside the definition body");
                            if (!definition_list) {
                                LITERAL_EQ(runner, child_at(second, 0), "second",
                                           "continued paragraph retains its text");
                            }
                        }
                        markdown_core_node_free(root);
                    }
                }
            }
        }
    }
}

static void set_kind_keeps_element_data_beside_the_arm(test_batch_runner *runner) {
    /* Converting a formula to a link installs default link data and preserves
     * the element's opaque data. Destruction releases both exactly once. */
    static const char markdown[] = "$x$ tail\n";
    markdown_core_document *document = facade_parse((const uint8_t *)markdown, sizeof(markdown) - 1);
    markdown_core_node *formula;

    OK(runner, document != NULL, "the formula document parses");
    formula = child_at(child_at(document->root, 0), 0);
    INT_EQ(runner, markdown_core_node_get_kind(formula), MARKDOWN_CORE_KIND_FORMULA,
           "the paragraph opens with a formula");
    OK(runner, formula->opaque != NULL, "the formula's element owns per-node data");
    INT_EQ(runner, markdown_core_node_set_kind(formula, child_at(document->root, 0), MARKDOWN_CORE_NODE_LINK),
           MARKDOWN_CORE_NODE_SET_KIND_OK, "set_kind converts the formula into a link");
    OK(runner, formula->opaque != NULL, "the element's data stays with the node");
    OK(runner, formula->as.link->resource == NULL, "the converted link starts without a resource");
    markdown_core_document_free(document);
}

static void ref_source_pos(test_batch_runner *runner) {
    static const char markdown[] = "Let's try [reference] links.\n"
                                   "\n"
                                   "[reference]: https://github.com (GitHub)\n";

    /* M2: the occurrence is the Link that names its definition by label,
     * with its own scope; the definition is a Reference where it was
     * written, spanning it. */
    test_facade_dump(runner, markdown,
                     "Document scope=1:1..3:40 anchor=null attributes={} children=2\n"
                     "├── Paragraph scope=1:1..1:28 anchor=null attributes={} children=3\n"
                     "│   ├── Text scope=1:1..1:10 anchor=null attributes={} literal=\"Let's try \" children=0\n"
                     "│   ├── Link scope=1:11..1:21 anchor=null attributes={} dest=reference(\"reference\") "
                     "title=null children=1\n"
                     "│   │   └── Text scope=1:12..1:20 anchor=null attributes={} literal=\"reference\" children=0\n"
                     "│   └── Text scope=1:22..1:28 anchor=null attributes={} literal=\" links.\" children=0\n"
                     "└── Reference scope=3:1..3:40 anchor=null attributes={} label=\"reference\" "
                     "dest=url(\"https://github.com\") title=\"GitHub\" children=0\n",
                     "reference link scopes are as expected");
    markdown_core_node *root = parse("[r]: /u\n===\ntext\n");
    markdown_core_node *paragraph = root && child_at(root, 0) ? child_at(root, 1) : NULL;
    OK(runner,
       paragraph && paragraph->kind == MARKDOWN_CORE_NODE_PARAGRAPH && START_LINE(paragraph) == 2 &&
           END_LINE(paragraph) == 3 && child_at(paragraph, 0) && START_LINE(child_at(paragraph, 0)) == 2,
       "a consumed definition prefix still relocates content arriving after the setext probe");
    if (root) {
        markdown_core_node_free(root);
    }
}

static void autolink_source_pos(test_batch_runner *runner) {
    test_facade_dump(
        runner, "See www.example.com.\n",
        "Document scope=1:1..1:20 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:20 anchor=null attributes={} children=3\n"
        "    ├── Text scope=1:1..1:4 anchor=null attributes={} literal=\"See \" children=0\n"
        "    ├── Link scope=1:5..1:19 anchor=null attributes={} dest=url(\"http://www.example.com\") "
        "title=null children=1\n"
        "    │   └── Text scope=1:5..1:19 anchor=null attributes={} literal=\"www.example.com\" children=0\n"
        "    └── Text scope=1:20..1:20 anchor=null attributes={} literal=\".\" children=0\n",
        "www autolink scopes are as expected");
    test_facade_dump(
        runner, "See http://example.com.\n",
        "Document scope=1:1..1:23 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:23 anchor=null attributes={} children=3\n"
        "    ├── Text scope=1:1..1:4 anchor=null attributes={} literal=\"See \" children=0\n"
        "    ├── Link scope=1:5..1:22 anchor=null attributes={} dest=url(\"http://example.com\") title=null "
        "children=1\n"
        "    │   └── Text scope=1:5..1:22 anchor=null attributes={} literal=\"http://example.com\" children=0\n"
        "    └── Text scope=1:23..1:23 anchor=null attributes={} literal=\".\" children=0\n",
        "scheme autolink scopes are as expected");
    /* An autolink at column one leaves NO prefix. This assertion used to pin the
     * defect -- it asserted a `Text scope=0:0..0:0 anchor=null attributes={} literal=""` as expected
     * output, a child with no bytes and no position, and a paragraph that said
     * it had two children when it had one thing in it. 0a.14 removes the node;
     * unpinning the assertion is the fix, the same shape as D10's
     * `regression.txt` example 24 at 0a.2. */
    test_facade_dump(
        runner, "http://example.com\n",
        "Document scope=1:1..1:18 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:18 anchor=null attributes={} children=1\n"
        "    └── Link scope=1:1..1:18 anchor=null attributes={} dest=url(\"http://example.com\") title=null "
        "children=1\n"
        "        └── Text scope=1:1..1:18 anchor=null attributes={} literal=\"http://example.com\" children=0\n",
        "scheme autolink at column one scopes are as expected");
    test_facade_dump(
        runner, "Mail user@example.com now.\n",
        "Document scope=1:1..1:26 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:26 anchor=null attributes={} children=3\n"
        "    ├── Text scope=1:1..1:5 anchor=null attributes={} literal=\"Mail \" children=0\n"
        "    ├── Link scope=1:6..1:21 anchor=null attributes={} dest=url(\"mailto:user@example.com\") "
        "title=null children=1\n"
        "    │   └── Text scope=1:6..1:21 anchor=null attributes={} literal=\"user@example.com\" children=0\n"
        "    └── Text scope=1:22..1:26 anchor=null attributes={} literal=\" now.\" children=0\n",
        "email autolink scopes are as expected");
}

/* A node built by hand into a published tree: every published node has
 * runs, here the one `span` bytes long at its owner's start, from a pool of
 * their own that they outlive. */
static markdown_core_node *published_node_new(markdown_core_node_type type, uint32_t span) {
    markdown_core_node *node = markdown_core_node_new(type);
    markdown_core_node_pool pool = {0};
    markdown_core_runs *runs = markdown_core_node_pool_bytes(&pool, sizeof(*runs) + sizeof(markdown_core_run));
    markdown_core_node_pool_dispose(&pool);
    runs->count = 1;
    runs->items[0] = (markdown_core_run){0, span};
    node->source.runs = runs;
    return node;
}

static void table_values(test_batch_runner *runner) {
    const char source[] = "| a | b |\n| - | :-: |\n| c | d |\n| e | f |\n";
    markdown_core_document *document = facade_parse((const uint8_t *)source, sizeof(source) - 1);
    OK(runner, document != NULL, "table value fixture parses");
    if (!document) {
        return;
    }
    markdown_core_node *table = child_at(document->root, 0);
    size_t columns, head, content;
    EXPECT_OK(markdown_core_node_table_properties(table, &columns, &head, &content));
    OK(runner, columns == 2 && head == 1 && content == 2, "a pipe table has one head row and its body rows");
    markdown_core_table_column column = facade_column_at(table, 1);
    OK(runner, column.flow == MARKDOWN_CORE_FLOW_CENTER, "a column keeps its authored alignment");
    uint8_t *dump = NULL;
    size_t length = 0;
    OK(runner,
       markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)source,
                                   sizeof(source) - 1, &dump, &length) == MARKDOWN_CORE_OK,
       "table values dump");
    OK(runner, dump && strstr((const char *)dump, "columns=[none,center] children=3"),
       "the dump draws each column's alignment and counts the rows");
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);
}

/* Many contractions followed by many address splits must retain one linear
 * source map, rather than copying every unconsumed suffix for each link. */
static int observed_source_marks;
static void observe_source_marks(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->finish(self, parser);
    observed_source_marks = (int)parser->line_marks.count;
}
static bool attach_source_mark_observer(markdown_core_dialect_builder *builder, void *context) {
    static document_owner observer;
    (void)context;
    return attach_document_finisher(builder, &observer, "source-mark-observer", observe_source_marks);
}

static void table_source_map_growth(test_batch_runner *runner) {
    const char *unit = "\\| &amp; user@example.com ";
    size_t unit_length = strlen(unit);
    for (size_t count = 256; count <= 4096; count *= 4) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "| h |\n| - |\n| ");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, unit);
        }
        markdown_core_strbuf_puts(&source, "|\n");
        observed_source_marks = 0;
        markdown_core_node *root = markdown_core_parse_document_with_setup((const char *)source.ptr, source.size,
                                                                           attach_source_mark_observer, NULL);
        OK(runner, root != NULL, "mapped table with %zu address splits parses", count);
        if (root) {
            // Every source byte may contribute only a bounded number of runs,
            // even as the number of links and remaining runs both increase.
            OK(runner, observed_source_marks > 0 && (size_t)observed_source_marks <= 16 * count + 16,
               "source map storage is linear at %zu repeats (%d runs)", count, observed_source_marks);
            markdown_core_node *table = child_at(root, 0);
            markdown_core_node *cell = child_at(child_at(table, 1), 0);
            size_t links = 0;
            for (size_t node_index = 0; node_index < cell->child_count; node_index++) {
                markdown_core_node *node = cell->children[node_index];
                if (node->kind != MARKDOWN_CORE_NODE_LINK) {
                    continue;
                }
                INT_EQ(runner, START_COLUMN(node), 12 + links * unit_length, "address begins at its authored byte");
                INT_EQ(runner, END_COLUMN(node), 27 + links * unit_length, "address ends at its authored byte");
                links++;
            }
            INT_EQ(runner, links, count, "every address is retained");
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

static size_t metadata_populated_count(const markdown_core_node *metadata) {
    return (facade_metadata(metadata, markdown_core_metadata_name) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_title) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_subtitle) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_time) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_date) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_authors) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_keywords) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_abstract) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_state) != NULL) +
           (facade_metadata(metadata, markdown_core_metadata_comment) != NULL);
}

/* Whether the source wrote `value` as a scalar, and that scalar when it did. */
static bool metadata_scalar(const markdown_core_metadata_value *value, markdown_core_metadata_scalar *scalar) {
    if (!value || markdown_core_metadata_value_get_kind(value) != MARKDOWN_CORE_METADATA_SCALAR) {
        return false;
    }
    *scalar = facade_scalar(value);
    return true;
}

/* The items of `value` when the source wrote it as a list, and zero otherwise. */
static size_t metadata_list_count(const markdown_core_metadata_value *value) {
    return value && markdown_core_metadata_value_get_kind(value) == MARKDOWN_CORE_METADATA_LIST
               ? facade_item_count(value)
               : 0;
}

static void properties_values(test_batch_runner *runner) {
    const char *source = "---\n# ignored\nname: \"Note\"\nunknown: x\nnot YAML\n...\n"
                         "time: 9007199254740993\ndate: 2026-09-08\nauthors: [Ada, 2]\n"
                         "keywords: []\nabstract: |\n  one\n\n  two\nstate: true\ncomment:\n"
                         "name: duplicate\ntitle: A title\nsubtitle: A subtitle\n---\nbody\n";
    char *input = malloc(strlen(source) + 1);
    strcpy(input, source);
    markdown_core_document *document = facade_parse((const uint8_t *)input, strlen(input));
    memset(input, 0, strlen(input));
    free(input);
    OK(runner, document != NULL, "fixed metadata fields parse independently of source storage");
    if (!document) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *metadata = facade_field(root, markdown_core_node_document_metadata);
    INT_EQ(runner, metadata_populated_count(metadata), 10, "all ten named fields are present");
    markdown_core_metadata_scalar scalar;
    const markdown_core_metadata_value *record = facade_metadata(metadata, markdown_core_metadata_time);
    OK(runner,
       metadata_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_NUMBER &&
           scalar.value.string.length == 16 && !memcmp(scalar.value.string.data, "9007199254740993", 16),
       "exact numbers own their spelling without alias expansion");
    record = facade_metadata(metadata, markdown_core_metadata_abstract);
    OK(runner,
       metadata_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_TEXT &&
           scalar.value.string.length == 9 && !memcmp(scalar.value.string.data, "one\n\ntwo\n", 9),
       "literal prose keeps internal newlines and a clipped final newline");
    INT_EQ(runner, document_scope_of(document, metadata, source).end.line, 20, "metadata ends at the closing fence");
    INT_EQ(runner, document_scope_of(document, facade_child(root, 0), source).start.line, 21,
           "body stays outside metadata");
    markdown_core_document_free(document);
}

/* A NUL in a member reads as U+FFFD, as the Markdown parser reads it. */
static void properties_nul(test_batch_runner *runner) {
    static const char source[] = "---\ntitle: a\0b\n---\nbody\n";
    markdown_core_document *document = facade_parse((const uint8_t *)source, sizeof(source) - 1);
    OK(runner, document != NULL, "an envelope with a NUL parses");
    if (!document) {
        return;
    }
    const markdown_core_node *metadata =
        facade_field(markdown_core_document_root(document), markdown_core_node_document_metadata);
    markdown_core_metadata_scalar scalar;
    OK(runner,
       metadata_scalar(facade_metadata(metadata, markdown_core_metadata_title), &scalar) &&
           scalar.kind == MARKDOWN_CORE_METADATA_TEXT && scalar.value.string.length == 5 &&
           !memcmp(scalar.value.string.data,
                   "a\xEF\xBF\xBD"
                   "b",
                   5),
       "a NUL in a value reads as U+FFFD");
    markdown_core_document_free(document);
}

static void properties_source_boundaries(test_batch_runner *runner) {
    const char *prefixes[] = {"", "\t", "  ", " \t"};
    const char *endings[] = {"\n", "\r", "\r\n"};
    for (size_t ending = 0; ending < 3; ending++) {
        for (size_t prefix = 0; prefix < 4; prefix++) {
            for (size_t indent = 0; indent <= 2; indent += 2) {
                char original[256];
                snprintf(original, sizeof(original),
                         "---\nauthors:\n%*s- Ada\n%s# note\n%*s- Lin\n"
                         "abstract: | # header\n  # prose\n\n    name: inside\n\ncomment: |\n  done\nstate: "
                         "ready\n---\nbody\n",
                         (int)indent, "", prefixes[prefix], (int)indent, "");
                markdown_core_strbuf input = MARKDOWN_CORE_BUF_INIT();
                for (const char *c = original; *c; c++) {
                    if (*c == '\n') {
                        markdown_core_strbuf_puts(&input, endings[ending]);
                    } else {
                        markdown_core_strbuf_putc(&input, *c);
                    }
                }
                markdown_core_document *doc = facade_parse(input.ptr, input.size);
                const markdown_core_node *metadata =
                    facade_field(markdown_core_document_root(doc), markdown_core_node_document_metadata);
                INT_EQ(runner, metadata_populated_count(metadata), 4,
                       "comments never split a flat list or become records");
                const markdown_core_metadata_value *record = facade_metadata(metadata, markdown_core_metadata_authors);
                INT_EQ(runner, facade_item_count(record), 2, "both list entries survive every separation form");
                record = facade_metadata(metadata, markdown_core_metadata_abstract);
                markdown_core_metadata_scalar scalar;
                const char *expected = "# prose\n\n  name: inside\n";
                OK(runner,
                   metadata_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_TEXT &&
                       scalar.value.string.length == strlen(expected) &&
                       !memcmp(scalar.value.string.data, expected, strlen(expected)),
                   "literal indentation, hashes and line endings are text, not nested fields");
                markdown_core_document_free(doc);
                markdown_core_strbuf_free(&input);
            }
        }
    }
    const char *invalid[] = {"name: &a value",        "name: *a",
                             "name: !!str 1",         "name: |\n  text",
                             "abstract: >\n  text",   "abstract: |-\n  text",
                             "abstract: |2\n  text",  "abstract: |\n    a\n  b",
                             "name: 'one\n  two'",    "name: one\n  two",
                             "authors: [true]",       "authors: [[one]]",
                             "name: {nested: value}", "Name: uppercase",
                             "unknown: ignored",      "\"name\\n\": bad",
                             "name: \"bad\\x41\""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        char source[256];
        snprintf(source, sizeof(source), "---\n%s\nname: kept\n---\n", invalid[i]);
        markdown_core_document *doc = facade_parse((const uint8_t *)source, strlen(source));
        const markdown_core_node *metadata =
            facade_field(markdown_core_document_root(doc), markdown_core_node_document_metadata);
        const markdown_core_metadata_value *record = facade_metadata(metadata, markdown_core_metadata_name);
        markdown_core_metadata_scalar value;
        OK(runner,
           metadata_populated_count(metadata) == 1 && metadata_scalar(record, &value) &&
               value.kind == MARKDOWN_CORE_METADATA_TEXT && value.value.string.length == 4 &&
               !memcmp(value.value.string.data, "kept", 4),
           "unsupported member %zu is ignored without consuming the next valid field or reserving its name", i);
        markdown_core_document_free(doc);
    }
    const char *dash_text = "---\nauthors: - Ada\n- ignored\nauthors: [Lin]\nkeywords: - language # note\n"
                            "title: -\n---\nbody\n";
    markdown_core_document *dash_doc = facade_parse((const uint8_t *)dash_text, strlen(dash_text));
    const markdown_core_node *dash_metadata =
        facade_field(markdown_core_document_root(dash_doc), markdown_core_node_document_metadata);
    const markdown_core_metadata_value *dash_values[] = {
        facade_metadata(dash_metadata, markdown_core_metadata_authors),
        facade_metadata(dash_metadata, markdown_core_metadata_keywords),
        facade_metadata(dash_metadata, markdown_core_metadata_title)};
    const char *dash_expected[] = {"- Ada", "- language", "-"};
    INT_EQ(runner, metadata_populated_count(dash_metadata), 3, "field-line dashes assign text fields once");
    for (size_t i = 0; i < sizeof(dash_values) / sizeof(*dash_values); i++) {
        markdown_core_metadata_scalar value;
        OK(runner,
           metadata_scalar(dash_values[i], &value) && value.kind == MARKDOWN_CORE_METADATA_TEXT &&
               value.value.string.length == strlen(dash_expected[i]) &&
               !memcmp(value.value.string.data, dash_expected[i], strlen(dash_expected[i])),
           "a field-line dash stays text, distinct from a following-line list marker");
    }
    markdown_core_document_free(dash_doc);
    const struct {
        const char *source;
        bool closed;
    } bracketed[] = {{"authors: [\nstate: true\n]\n", true},
                     {"authors: [\n# ]\nstate: true\n]\n", true},
                     {"authors: [\n\"state: true ]\",\n[one]\n]\n", true},
                     {"authors: [\n{\nstate: true\n}\n]\n", true},
                     {"unknown: [\nstate: true\n]\n", true},
                     {"{\nstate: true\n}\n", true},
                     {"authors: [\nstate: true\n", false},
                     {"authors: [\n}\nstate: true\n", false},
                     {"{\n]\nstate: true\n", false},
                     {"authors: [\n\"unterminated\n]\nstate: false\n", false},
                     {"unknown: [\n...\nstate: true\n", false},
                     {"{\nstate: true\n", false}};
    for (size_t i = 0; i < sizeof(bracketed) / sizeof(*bracketed); i++) {
        char source[256];
        snprintf(source, sizeof(source), "---\nname: kept\n%sstate: ready\n---\nbody\n", bracketed[i].source);
        markdown_core_document *doc = facade_parse((const uint8_t *)source, strlen(source));
        const markdown_core_node *root = markdown_core_document_root(doc);
        const markdown_core_node *metadata = facade_field(root, markdown_core_node_document_metadata);
        const markdown_core_metadata_value *state = facade_metadata(metadata, markdown_core_metadata_state);
        markdown_core_metadata_scalar value;
        INT_EQ(runner, metadata_populated_count(metadata), bracketed[i].closed ? 2 : 1,
               "bracketed member %zu keeps its interior opaque", i);
        OK(runner,
           bracketed[i].closed ? metadata_scalar(state, &value) && value.kind == MARKDOWN_CORE_METADATA_TEXT &&
                                     value.value.string.length == 5 && !memcmp(value.value.string.data, "ready", 5)
                               : state == NULL,
           "only a closed bracketed member allows the next independent field");
        INT_EQ(runner, document_scope_of(doc, facade_child(root, 0), source).start.line,
               document_scope_of(doc, metadata, source).end.line + 1, "the closing fence always separates the body");
        markdown_core_document_free(doc);
    }
    const char *source =
        "---\n{\"name\":\"ignored\"}\nname: \"\\uD83D\\uDE80\"\nauthors: [Ada, 2]\nstate: false\n---\n";
    markdown_core_document *doc = facade_parse((const uint8_t *)source, strlen(source));
    const markdown_core_node *metadata =
        facade_field(markdown_core_document_root(doc), markdown_core_node_document_metadata);
    INT_EQ(runner, metadata_populated_count(metadata), 3, "only field lines produce metadata records");
    markdown_core_metadata_scalar value;
    OK(runner,
       metadata_scalar(facade_metadata(metadata, markdown_core_metadata_name), &value) &&
           value.kind == MARKDOWN_CORE_METADATA_TEXT && value.value.string.length == 4 &&
           !memcmp(value.value.string.data, "\xf0\x9f\x9a\x80", 4),
       "quoted Unicode surrogate pair decodes to a scalar");
    markdown_core_document_free(doc);
    const char *ordered[] = {"---\nname: one\nstate: ready\n---\n", "---\nstate: ready\nname: one\n---\n"};
    uint8_t *dumps[2] = {0};
    size_t lengths[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        doc = facade_parse((const uint8_t *)ordered[i], strlen(ordered[i]));
        OK(runner,
           (markdown_core_document_dump(doc, markdown_core_document_root(doc), (const uint8_t *)ordered[i],
                                        strlen(ordered[i]), &dumps[i], &lengths[i]) == MARKDOWN_CORE_OK),
           "named fields dump");
        markdown_core_document_free(doc);
    }
    OK(runner, dumps[0] && dumps[1] && lengths[0] == lengths[1] && !memcmp(dumps[0], dumps[1], lengths[0]),
       "field order does not change metadata or its envelope scope");
    markdown_core_dump_free(dumps[0]);
    markdown_core_dump_free(dumps[1]);
}

/* SEALING RESOLVES DECLARED PEERS. An element's `peers` become, in their
 * order, the instances the parser holds for them -- the same one the parser
 * answers from outside the dialect -- and NULL for one the dialect lacks. */
static const markdown_core_element PEER_ABSENT = {.name = "absent"};
static const markdown_core_element *const PROBE_PEERS[] = {&MARKDOWN_CORE_ELEMENT_TABLE, &PEER_ABSENT,
                                                           &MARKDOWN_CORE_ELEMENT_LINK, NULL};
static int peers_resolved;
static void observe_peers(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    peers_resolved = self->peers[0] == markdown_core_parser_instance(parser, &MARKDOWN_CORE_ELEMENT_TABLE) &&
                     self->peers[0] && self->peers[0]->element == &MARKDOWN_CORE_ELEMENT_TABLE && !self->peers[1] &&
                     self->peers[2] == markdown_core_parser_instance(parser, &MARKDOWN_CORE_ELEMENT_LINK) &&
                     self->peers[2] && !markdown_core_parser_instance(parser, &PEER_ABSENT);
}
static void peers_resolve_at_seal(test_batch_runner *runner) {
    static const markdown_core_element probe = {.dispose_parser = observe_peers, .peers = PROBE_PEERS};
    const markdown_core_element *elements[] = {&probe};
    peers_resolved = 0;
    markdown_core_node *root = parse_with_probes("text\n", 5, elements, 1);
    OK(runner, root != NULL && peers_resolved, "declared peers resolve in order, absent ones to NULL");
    markdown_core_node_free(root);
}

static size_t properties_decoded_bytes;
/* The owner's own instance read the Properties, so its record holds the work. */
static void observe_properties(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->finish(self, parser);
    properties_decoded_bytes = ((const markdown_core_properties_work *)self->state)->decoded_bytes;
}
static bool attach_properties_observer(markdown_core_dialect_builder *builder, void *context) {
    static document_owner observer;
    (void)context;
    return attach_document_finisher(builder, &observer, "properties-observer", observe_properties);
}
static void properties_member_work(test_batch_runner *runner) {
    const struct {
        const char *source;
        bool final_field;
    } units[] = {{"authors: [\\\"\nunknown: 1\n", false},
                 {"# comment\nnot YAML\n...\n", true},
                 {"abstract: &a [true]\ncomment: *a\n", true},
                 {"{bad: [true], unknown: 1}\n", true},
                 {"{\nunknown: 1\n", false},
                 {"unknown: x[\nb[c: 2\nnot YAML\n", true},
                 {"name: duplicate\n", true},
                 {"authors: [\nstate: true\n]\n", true},
                 {"{\nstate: true\n}\n", true}};
    for (size_t shape = 0; shape < sizeof(units) / sizeof(*units); shape++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, "---\n");
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, units[shape].source);
            }
            markdown_core_strbuf_puts(&source, "state: kept\n---\nbody\n");
            markdown_core_node *root = markdown_core_parse_document_with_setup((const char *)source.ptr, source.size,
                                                                               attach_properties_observer, NULL);
            OK(runner, root != NULL, "adversarial Properties shape %zu at %zu members parses", shape, count);
            OK(runner, properties_decoded_bytes <= (size_t)source.size, "disjoint members never retry a failed suffix");
            if (root) {
                markdown_core_node *metadata = root->as.document->metadata;
                const markdown_core_metadata_value *state = facade_metadata(metadata, markdown_core_metadata_state);
                markdown_core_metadata_scalar value;
                OK(runner,
                   units[shape].final_field
                       ? metadata_scalar(state, &value) && value.kind == MARKDOWN_CORE_METADATA_TEXT &&
                             value.value.string.length == 4 && !memcmp(value.value.string.data, "kept", 4)
                       : state == NULL,
                   "member ownership governs whether the final field is independent");
                OK(runner, metadata_populated_count(metadata) <= 10, "only named fields can be assigned");
                markdown_core_node_free(root);
            }
            markdown_core_strbuf_free(&source);
        }
    }
    for (size_t count = 128; count <= 65536; count *= 2) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "---\nauthors: [");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, i ? ",x" : "x");
        }
        markdown_core_strbuf_puts(&source, "]\n---\n");
        markdown_core_node *root = markdown_core_parse_document_with_setup((const char *)source.ptr, source.size,
                                                                           attach_properties_observer, NULL);
        OK(runner, root != NULL, "long flat list parses");
        if (root) {
            markdown_core_node *metadata = root->as.document->metadata;
            const markdown_core_metadata_value *record = facade_metadata(metadata, markdown_core_metadata_authors);
            OK(runner, record && record->kind == MARKDOWN_CORE_METADATA_LIST && record->as.list.count == count,
               "all items survive one general flat-list decoder");
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

/* Track live bytes, not RSS or allocation timing. The header preserves C99
 * fundamental alignment and lets realloc account for released capacity. */
static void properties_text_memory(test_batch_runner *runner) {
    const char *prefixes[] = {"---\nname: x", "---\nname: \"x", "---\nabstract: |\n  ", "---\nauthors:\n- x"};
    const char *suffixes[] = {"\n", "\"\n", "\n", "\n- second\n"};
    for (size_t shape = 0; shape < sizeof(prefixes) / sizeof(*prefixes); shape++) {
        for (size_t count = 65536; count <= 1048576; count *= 4) {
            size_t baseline = 0;
            const char *characters = "x{}[]";
            for (size_t character = 0; characters[character]; character++) {
                markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
                markdown_core_strbuf_puts(&source, prefixes[shape]);
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, characters[character]);
                }
                markdown_core_strbuf_puts(&source, suffixes[shape]);
                markdown_core_strbuf_puts(&source, "state: ready\n---\n");
                properties_probe_arm();
                markdown_core_node *root =
                    markdown_core_parse_document_with_setup((const char *)source.ptr, source.size, NULL, NULL);
                OK(runner, root != NULL, "long scalar shape %zu with %c parses", shape, characters[character]);
                if (root) {
                    markdown_core_node *metadata = root->as.document->metadata;
                    OK(runner, metadata_populated_count(metadata) == 2, "text and following field survive");
                    const markdown_core_metadata_value *record =
                        shape == 2   ? facade_metadata(metadata, markdown_core_metadata_abstract)
                        : shape == 3 ? facade_metadata(metadata, markdown_core_metadata_authors)
                                     : facade_metadata(metadata, markdown_core_metadata_name);
                    markdown_core_string text = {0};
                    if (shape == 3) {
                        if (metadata_list_count(record)) {
                            text = facade_item_at(record, 0).value;
                        }
                    } else {
                        markdown_core_metadata_scalar scalar;
                        if (metadata_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_TEXT) {
                            text = scalar.value.string;
                        }
                    }
                    size_t offset = shape == 2 ? 0 : 1;
                    bool intact = text.length == count + 1;
                    for (size_t i = 0; intact && i < count; i++) {
                        intact = text.data[offset + i] == characters[character];
                    }
                    OK(runner, intact, "brackets remain complete scalar text");
                    markdown_core_node_free(root);
                }
                properties_probe_disarm();
                if (!character) {
                    baseline = properties_peak_bytes;
                }
                OK(runner, properties_peak_bytes == baseline,
                   "text brackets allocate no index storage: shape %zu, %zu bytes, %c peak %zu baseline %zu", shape,
                   count, characters[character], properties_peak_bytes, baseline);
                INT_EQ(runner, properties_live_bytes, 0, "all tracked parse allocations are released");
                markdown_core_strbuf_free(&source);
            }
        }
    }
}

static markdown_core_string owned_metadata_string(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    memcpy(copy, text, length + 1);
    return (markdown_core_string){(const uint8_t *)copy, length};
}

static void universal_values(test_batch_runner *runner) {
    const char *source = "![x](/u) ![y](/u)";
    markdown_core_document *document = facade_parse((const uint8_t *)source, strlen(source));
    markdown_core_node *root = document->root;
    markdown_core_node *metadata = published_node_new(MARKDOWN_CORE_NODE_METADATA, 4);
    metadata->extent = (markdown_core_extent){0, 4};
    root->as.document->metadata = metadata;
    markdown_core_metadata_value *values[] = {&metadata->as.metadata->name,    &metadata->as.metadata->time,
                                              &metadata->as.metadata->date,    &metadata->as.metadata->title,
                                              &metadata->as.metadata->authors, &metadata->as.metadata->keywords};
    for (size_t i = 0; i < 6; i++) {
        markdown_core_metadata_value *value = values[i];
        /* Only active union members are initialized. Access and cleanup must
         * not read inactive storage, including absent field payloads. */
        memset(&value->as, 0xa5, sizeof(value->as));
        value->kind = i < 4 ? MARKDOWN_CORE_METADATA_SCALAR : MARKDOWN_CORE_METADATA_LIST;
        if (i < 4) {
            value->as.scalar.kind = (markdown_core_metadata_scalar_kind)i;
        } else {
            value->as.list.items = NULL;
            value->as.list.count = 0;
        }
    }
    memset(&metadata->as.metadata->subtitle.as, 0xa5, sizeof(metadata->as.metadata->subtitle.as));
    metadata->as.metadata->time.as.scalar.value.boolean = true;
    metadata->as.metadata->date.as.scalar.value.string = owned_metadata_string("9007199254740993");
    metadata->as.metadata->title.as.scalar.value.string = owned_metadata_string("中文\nquoted");
    markdown_core_metadata_value *list = &metadata->as.metadata->keywords;
    list->as.list.count = 2;
    list->as.list.items = calloc(2, sizeof(*list->as.list.items));
    list->as.list.items[0] =
        (markdown_core_metadata_list_item){MARKDOWN_CORE_METADATA_ITEM_NUMBER, owned_metadata_string("1.25")};
    list->as.list.items[1] =
        (markdown_core_metadata_list_item){MARKDOWN_CORE_METADATA_ITEM_TEXT, owned_metadata_string("")};
    OK(runner, facade_field(root, markdown_core_node_document_metadata) == metadata, "document owns metadata");
    INT_EQ(runner, metadata_populated_count(metadata), 6, "all metadata records retained");
    for (size_t i = 0; i < 6; i++) {
        const markdown_core_metadata_value *record = values[i];
        INT_EQ(runner, markdown_core_metadata_value_get_kind(record),
               i < 4 ? MARKDOWN_CORE_METADATA_SCALAR : MARKDOWN_CORE_METADATA_LIST, "metadata value tag retained");
        if (i < 4) {
            markdown_core_metadata_scalar scalar = facade_scalar(record);
            INT_EQ(runner, scalar.kind, i, "scalar tag retained");
            if (i == 1) {
                OK(runner, scalar.value.boolean, "boolean payload retained");
            } else if (i >= 2) {
                const char *expected = i == 2 ? "9007199254740993" : "中文\nquoted";
                OK(runner,
                   scalar.value.string.length == strlen(expected) &&
                       memcmp(scalar.value.string.data, expected, strlen(expected)) == 0,
                   "scalar string payload retained");
                OK(runner, scalar.value.string.data == record->as.scalar.value.string.data,
                   "scalar accessor borrows document string");
            }
            continue;
        }
        INT_EQ(runner, facade_item_count(record), i == 5 ? 2 : 0, "list item count retained");
        if (i == 5) {
            markdown_core_metadata_list_item item = facade_item_at(record, 0);
            OK(runner,
               item.kind == MARKDOWN_CORE_METADATA_ITEM_NUMBER && item.value.length == 4 &&
                   memcmp(item.value.data, "1.25", 4) == 0,
               "list number payload retained");
            OK(runner, item.value.data == list->as.list.items[0].value.data, "list accessor borrows document string");
            item = facade_item_at(record, 1);
            OK(runner, item.kind == MARKDOWN_CORE_METADATA_ITEM_TEXT && item.value.length == 0,
               "empty text list item retained");
        }
    }
    OK(runner, !facade_metadata(metadata, markdown_core_metadata_subtitle), "absent field differs from explicit null");
    OK(runner, facade_metadata(metadata, markdown_core_metadata_name) != NULL, "explicit null field is present");
    markdown_core_node *image = child_at(child_at(root, 0), 0);
    OK(runner, facade_dimensions(image) == NULL, "unsized image has no dimensions");
    image->as.link->dimensions.has_value = true;
    image->as.link->dimensions.value = (markdown_core_dimensions){640, {true, 480}};
    const markdown_core_dimensions *dimensions = facade_dimensions(image);
    OK(runner,
       dimensions && dimensions->width == 640 && dimensions->height.has_value && dimensions->height.value == 480,
       "dimensions preserve their owned value");
    uint8_t *dump = NULL;
    size_t length = 0;
    OK(runner,
       (markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)source,
                                    strlen(source), &dump, &length) == MARKDOWN_CORE_OK),
       "metadata and dimensions dump");
    OK(runner, strstr((const char *)dump, "date=scalar(number(\"9007199254740993\"))") != NULL,
       "decimal text never rounded");
    OK(runner, strstr((const char *)dump, "authors=list([])") != NULL, "empty list distinct from null");
    OK(runner, strstr((const char *)dump, "dimensions=(width=640,height=480)") != NULL, "typed dimensions dump");
    markdown_core_dump_free(dump);
    markdown_core_document_free(document); /* Sanitizers verify complete recursive ownership. */
}

/* Count visited source positions as well as verifying values. Repeated failed
 * candidates share one extent, so they cannot rescan each other's suffixes. */
typedef struct {
    size_t autolink_domains;
    size_t cross_link, opaque, delimiters, comment, lookahead, footnote_body, block_identifier, callout, dimensions;
    size_t definition_lists, citation_brace_bytes;
    size_t physical_lines, physical_capacity, physical_facts, physical_fact_capacity, normalized_lines;
    bool heading_collection_disposed;
    size_t attributes, anchors, definitions, whitespace, brackets, citations, list_markers, specimens;
    size_t inline_hooks;
    size_t properties_lines, physical_line_bytes, metadata_key_bytes, metadata_value_bytes;
    size_t delimiter_pushes;
    size_t pooled_delimiters;
    size_t content_mark_queries;
    size_t content_mark_probes;
    /* The runs the content-to-source map holds when the parse ends. */
    size_t line_marks;
} inline_work;
/* Read once the document is finished, by the document's owner, whose own
 * instance read the Properties. */
static void record_work(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    inline_work *work = parser->context;
    if (parser->error || !work) {
        return;
    }
    const markdown_core_properties_work *properties = self->state;
    const markdown_core_heading_state *headings =
        ELEMENT_STATE(parser, markdown_core_heading_state, MARKDOWN_CORE_ELEMENT_HEADING);
    work->cross_link = ELEMENT_STATE(parser, markdown_core_cross_link_work, MARKDOWN_CORE_ELEMENT_CROSS_LINK)->scan;
    work->autolink_domains =
        ELEMENT_STATE(parser, markdown_core_autolink_work, MARKDOWN_CORE_ELEMENT_AUTOLINK)->domains;
    work->opaque = parser->opaque_scan_work;
    work->delimiters = parser->delimiter_work;
    work->delimiter_pushes = parser->delimiter_pushes;
    work->content_mark_queries = parser->content_mark_queries;
    work->content_mark_probes = parser->content_mark_probes;
    work->line_marks = parser->line_marks.count;
    work->pooled_delimiters = 0;
    for (const delimiter *entry = parser->free_delimiters; entry; entry = entry->next) {
        work->pooled_delimiters++;
    }
    work->whitespace = ELEMENT_STATE(parser, markdown_core_text_work, MARKDOWN_CORE_ELEMENT_TEXT)->whitespace;
    work->inline_hooks = parser->inline_hook_work;
    work->properties_lines = properties->line_work;
    work->physical_line_bytes = parser->input_line_work;
    work->physical_lines = parser->input.lines.front.count;
    work->physical_capacity = parser->input.lines.front.ahead.capacity;
    work->physical_facts = parser->input.fact_count;
    work->physical_fact_capacity = parser->input.fact_capacity;
    work->normalized_lines = 0;
    for (size_t i = 0; i < parser->input.fact_count; i++) {
        work->normalized_lines += parser->input.facts[i].normalized != NULL;
    }
    work->metadata_key_bytes = properties->key_work;
    work->metadata_value_bytes = properties->decoded_bytes;
    work->brackets = ELEMENT_STATE(parser, markdown_core_bracket_work, MARKDOWN_CORE_ELEMENT_LINK)->work;
    work->citations = ELEMENT_STATE(parser, markdown_core_citation_work, MARKDOWN_CORE_ELEMENT_CITATION)->work;
    work->citation_brace_bytes =
        ELEMENT_STATE(parser, markdown_core_citation_work, MARKDOWN_CORE_ELEMENT_CITATION)->brace_bytes;
    work->specimens = ELEMENT_STATE(parser, markdown_core_specimen_state, MARKDOWN_CORE_ELEMENT_SPECIMEN)->work;
    work->list_markers = ELEMENT_STATE(parser, markdown_core_list_work, MARKDOWN_CORE_ELEMENT_LIST)->markers;
    work->comment = ELEMENT_STATE(parser, markdown_core_comment_work, MARKDOWN_CORE_ELEMENT_COMMENT)->scan;
    work->lookahead = parser->block_lookahead_work;
    work->block_identifier =
        ELEMENT_STATE(parser, markdown_core_block_identifier_work, MARKDOWN_CORE_ELEMENT_PARAGRAPH)->scan;
    work->callout = ELEMENT_STATE(parser, markdown_core_callout_work, MARKDOWN_CORE_ELEMENT_CALLOUT)->scan;
    work->dimensions =
        ELEMENT_STATE(parser, markdown_core_embedded_work, MARKDOWN_CORE_ELEMENT_EMBEDDED)->dimensions +
        ELEMENT_STATE(parser, markdown_core_cross_link_work, MARKDOWN_CORE_ELEMENT_CROSS_LINK)->dimensions;
    work->attributes = parser->attribute_scratch.work;
    work->anchors = headings->anchor_work;
    work->definitions = 0;
    for (size_t i = 0; i < parser->roster.count; i++) {
        work->definitions += parser->roster.records[i]->key[0] == MARKDOWN_CORE_ROSTER_REFERENCE;
    }
    work->heading_collection_disposed = headings->headings.front.values == NULL && headings->count == 0;
    work->footnote_body = parser->footnote_body_work;
    work->definition_lists =
        ELEMENT_STATE(parser, markdown_core_definition_list_work, MARKDOWN_CORE_ELEMENT_DEFINITION_LIST)->work;
}
static void record_inline_work(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->finish(self, parser);
    record_work(self, parser);
}
static bool attach_work_recorder(markdown_core_dialect_builder *builder) {
    static document_owner recorder;
    return attach_document_finisher(builder, &recorder, "work-recorder", record_inline_work);
}
static bool measure_inline_work(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    return attach_work_recorder(builder);
}

/* Depth and width vary independently. Construction transfers disjoint
 * subtrees, while the separate mutation tests retain all cycle checks. The
 * parser-boundary audit excludes arbitrary reparenting from these paths. */
static void deep_inline_construction(test_batch_runner *runner) {
    const size_t sizes[] = {1, 64, 1024, 8192};
    for (size_t shape = 0; shape < 2; shape++) {
        for (size_t d = 0; d < 4; d++) {
            for (size_t w = 0; w < 4; w++) {
                markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
                for (size_t i = 0; i < sizes[d]; i++) {
                    markdown_core_strbuf_puts(&source, shape ? "[" : "![");
                }
                for (size_t i = 0; i < sizes[w]; i++) {
                    markdown_core_strbuf_puts(&source, "a@b.co ");
                }
                for (size_t i = 0; i < sizes[d]; i++) {
                    markdown_core_strbuf_puts(&source, shape ? "]{}" : "](u)");
                }
                markdown_core_node *root = markdown_core_parse_document((char *)source.ptr, source.size);
                OK(runner, root != NULL, "depth/width construction succeeds");
                size_t containers = 0, links = 0;
                tree_iter *iter = tree_iter_new(root);
                markdown_core_event_type event;
                while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
                    if (event != MARKDOWN_CORE_EVENT_ENTER) {
                        continue;
                    }
                    markdown_core_node *node = tree_iter_get_node(iter);
                    containers += node->kind == (shape ? MARKDOWN_CORE_NODE_SPAN : MARKDOWN_CORE_NODE_EMBEDDED);
                    links += node->kind == MARKDOWN_CORE_NODE_LINK;
                }
                INT_EQ(runner, containers, sizes[d], "all nesting survives construction");
                INT_EQ(runner, links, sizes[w], "all independent autolinks survive construction");
                tree_iter_free(iter);
                markdown_core_node_free(root);
                markdown_core_strbuf_free(&source);
            }
        }
    }
}

static void autolink_domain_linear_work(test_batch_runner *runner) {
    const size_t segments[] = {8, 9, 10, 11, 64};
    const char *suffixes[] = {"_b\n", "_a.b\n", "_a.b.c\n"};
    for (size_t prefix = 0; prefix < 2; prefix++) {
        for (size_t n = 0; n < sizeof(segments) / sizeof(*segments); n++) {
            for (size_t suffix = 0; suffix < 3; suffix++) {
                markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
                markdown_core_strbuf_puts(&source, prefix ? "https://" : "www.");
                for (size_t i = 0; i < segments[n]; i++) {
                    markdown_core_strbuf_puts(&source, "a.");
                }
                markdown_core_strbuf_puts(&source, suffixes[suffix]);
                markdown_core_node *root = parse((const char *)source.ptr);
                OK(runner, root != NULL, "host suffix candidate parses");
                INT_EQ(runner, child_at(child_at(root, 0), 0)->kind,
                       suffix == 2 ? MARKDOWN_CORE_NODE_LINK : MARKDOWN_CORE_NODE_TEXT,
                       "the last two segments alone determine underscore rejection");
                markdown_core_node_free(root);
                markdown_core_strbuf_free(&source);
            }
        }
    }
    for (size_t n = 16; n <= 8192; n *= 2) {
        for (size_t valid_tail = 0; valid_tail < 2; valid_tail++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            for (size_t i = 0; i < n; i++) {
                markdown_core_strbuf_puts(&source, "www._");
            }
            markdown_core_strbuf_puts(&source, valid_tail ? "www.com\n" : "b\n");
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_setup((const char *)source.ptr, source.size,
                                                                               measure_inline_work, &work);
            OK(runner, root != NULL, "overlapping domain candidates parse");
            size_t links = 0;
            for (size_t node_index = 0; node_index < child_at(root, 0)->child_count; node_index++) {
                markdown_core_node *node = child_at(root, 0)->children[node_index];
                if (node->kind == MARKDOWN_CORE_NODE_LINK) {
                    links++;
                    STR_EQ(runner, markdown_core_chunk_to_cstr(&node->as.link->resource->url), "http://www.com",
                           "a candidate after the invalid underscore remains eligible");
                }
            }
            INT_EQ(runner, links, valid_tail, "host grammar is independent of segment count");
            OK(runner, work.autolink_domains <= 3 * (size_t)source.size,
               "overlapping failed hosts have bounded domain work");
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void ordered_numeral_ceiling(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    for (size_t i = 0; i < 999999; i++) {
        markdown_core_strbuf_putc(&source, 'M');
    }
    markdown_core_strbuf_puts(&source, "CMXCIX.  boundary\n");
    inline_work work = {0};
    markdown_core_node *root =
        markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
    OK(runner,
       root && child_at(root, 0)->kind == MARKDOWN_CORE_NODE_LIST && child_at(root, 0)->as.list->start == 999999999,
       "Roman numeral accepts the exact nine-digit ceiling");
    OK(runner, work.list_markers <= 4 * (size_t)source.size, "ceiling accumulation inspects bounded source");
    if (root) {
        markdown_core_node_free(root);
    }
    static const char *const overflow[] = {"M", "CCCCCCCCCC", "CCCCCCCCCXXXXXXXXXX", "CCCCCCCCCXXXXXXXXXIIIIIIIIII"};
    for (size_t i = 0; i < sizeof(overflow) / sizeof(*overflow); i++) {
        markdown_core_strbuf_truncate(&source, 999999);
        markdown_core_strbuf_puts(&source, overflow[i]);
        markdown_core_strbuf_puts(&source, ".  overflow\n");
        root = markdown_core_parse_document((char *)source.ptr, source.size);
        OK(runner, root && child_at(root, 0)->kind == MARKDOWN_CORE_NODE_PARAGRAPH,
           "Roman M/C/X/I accumulation rejects overflow without wrapping: case=%zu", i);
        if (root) {
            markdown_core_node_free(root);
        }
    }
    markdown_core_strbuf_free(&source);
}

static void definition_list_linear_work(test_batch_runner *runner) {
    static const char *const units[] = {"Term\n: body\n\n", ": body\n", "literal\n\n", "Term\n\n\n: body\n\n",
                                        "[x]: /u\n: body\n\n"};
    for (size_t shape = 0; shape < sizeof(units) / sizeof(*units); shape++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, "First\n: body\n");
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, units[shape]);
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "definition body/term input parses: shape=%zu count=%zu", shape, count);
            OK(runner, work.definition_lists + work.lookahead <= 32 * (size_t)source.size,
               "definition lookahead is bounded: shape=%zu count=%zu work=%zu", shape, count,
               work.definition_lists + work.lookahead);
            if (root) {
                markdown_core_node_free(root);
            }
            markdown_core_strbuf_free(&source);
        }
    }
    for (size_t depth = 32; depth <= 1024; depth *= 2) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "T\n");
        for (size_t i = 0; i < depth; i++) {
            for (size_t j = 0; j < i; j++) {
                markdown_core_strbuf_puts(&source, "  ");
            }
            markdown_core_strbuf_puts(&source, ": T\n");
        }
        for (size_t i = 0; i < depth * 2; i++) {
            markdown_core_strbuf_putc(&source, '\n');
        }
        for (size_t i = 0; i < depth; i++) {
            markdown_core_strbuf_puts(&source, "  ");
        }
        markdown_core_strbuf_puts(&source, "last\n");
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "nested definition ownership parses and releases: depth=%zu", depth);
        OK(runner, work.definition_lists + work.lookahead <= 32 * (size_t)source.size,
           "nested definition blank runs keep bounded work: depth=%zu", depth);
        size_t definitions = 0;
        tree_iter *iter = root ? tree_iter_new(root) : NULL;
        if (iter) {
            markdown_core_event_type event;
            while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
                if (event == MARKDOWN_CORE_EVENT_ENTER &&
                    tree_iter_get_node(iter)->kind == MARKDOWN_CORE_NODE_DEFINITION) {
                    definitions++;
                }
            }
            tree_iter_free(iter);
        }
        INT_EQ(runner, definitions, depth, "one association per committed nested term");
        if (root) {
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

static void citation_linear_work(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *unit, *close, *suffix;
    } cases[] = {
        {"", "@a [", "]", ""},    {"# [", "@a [", "]", "]\n"},
        {"", "[@a ", "]", ""},    {"[", "*pre* @a [@b [x]]; ", "", "@z]"},
        {"", "@{", "", " key}"},  {"", "@{a", "}", ""},
        {"", "@a [x] ", "", ""},  {"", "(@label) body\n\n", "", "@label"},
        {"", "M", "", ".  body"}, {"[", "@a [", "]", ";]"},
        {"", "[@a;] ", "", ""},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].close);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "citation dependency chains parse: case=%zu count=%zu", c, count);
            OK(runner, work.citations <= 20 * (size_t)source.size,
               "citation scans and resolutions are linear: case=%zu size=%d work=%zu", c, source.size, work.citations);
            OK(runner, work.brackets <= 4 * (size_t)source.size && work.delimiters <= 20 * (size_t)source.size,
               "citation ranges share bounded delimiter reduction: case=%zu count=%zu", c, count);
            OK(runner, work.list_markers <= 20 * (size_t)source.size && work.specimens <= 20 * (size_t)source.size,
               "list marker scans are bounded: case=%zu count=%zu", c, count);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void citation_sparse_brace_storage(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *suffix;
        char fill;
    } cases[] = {{"", " @{key}", 'x'},
                 {"@{key} ", "", 'x'},
                 {"@{key} `", "`", '{'},
                 {"@{broken ", " @{key}", 'x'},
                 {"@{key} <i title=\"", "\">", '{'}};
    for (size_t shape = 0; shape < sizeof(cases) / sizeof(*cases); shape++) {
        for (size_t length = 256; length <= 1048576; length *= 16) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[shape].prefix);
            for (size_t i = 0; i < length; i++) {
                markdown_core_strbuf_putc(&source, cases[shape].fill);
            }
            markdown_core_strbuf_puts(&source, cases[shape].suffix);
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_setup((char *)source.ptr, (size_t)source.size,
                                                                               measure_inline_work, &work);
            OK(runner, root != NULL, "sparse braced citations parse: shape=%zu length=%zu", shape, length);
            OK(runner, work.citation_brace_bytes <= 256,
               "brace index storage is independent of ordinary/opaque bytes: shape=%zu length=%zu bytes=%zu", shape,
               length, work.citation_brace_bytes);
            OK(runner, work.citations <= 20 * (size_t)source.size, "sparse index keeps linear scan work");
            size_t citations = 0;
            markdown_core_node *paragraph = child_at(root, 0);
            for (size_t node_index = 0; node_index < paragraph->child_count; node_index++) {
                markdown_core_node *node = paragraph->children[node_index];
                citations += node->kind == MARKDOWN_CORE_NODE_CITE;
            }
            INT_EQ(runner, citations, 1,
                   "valid inner key survives sparse text, opaque braces, and an invalid outer key");
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void cross_link_linear_work(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *unit, *suffix;
    } cases[] = {
        {"", "!", "[[a]]"},
        {"", "[", "a]]"},
        {"[[a]]", "]", ""},
        {"[[a", "#h", "]]"},
        {"[[", "^", "]]"},
        {"[[a|", "|", "]]"},
        {"![[a|", "9", "]]"},
        {"![[a|1x", "9", "]]"},
        {"![[a|", "|", "2147483647x1]]"},
        {"![[a|", "\\|", "2147483647x1]]"},
        {"", "![[a|caption|1x2]] ", ""},
        {"", "![[a|caption|1x2[", "]]"},
        {"![[", "a", ""},
        {"![[", "a", "]"},
        {"", "![[a[", "]]"},
        {"", "[[a#|x]]", ""},
        {"", "[[a\\|b]] ![[#^id|]] ", ""},
        {"$", "![[a|", "$]]"},
        {"", "$x ", ""},
        {"\\\\(", "[[a|", "\\\\)]]"},
        {"", "\\\\(a ", ""},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial cross links parse successfully");
            OK(runner, work.cross_link <= 3 * (size_t)source.size,
               "cross-link scanner inspects disjoint bodies: case=%zu size=%d work=%zu", c, source.size,
               work.cross_link);
            OK(runner, work.opaque <= 4 * (size_t)source.size,
               "opaque delimiter searches are linear: case=%zu size=%d work=%zu", c, source.size, work.opaque);
            OK(runner, work.dimensions <= 3 * (size_t)source.size,
               "dimension recognition is bounded: case=%zu size=%d work=%zu", c, source.size, work.dimensions);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* Nested bodies are never rescanned for emptiness and close in one bracket
 * operation. The document order follows opening positions, not close order. */
static void inline_footnote_linear_work(test_batch_runner *runner) {
    static const struct {
        const char *left, *middle, *right;
        int notes_per_unit;
    } cases[] = {
        {"^", "", "", 0},        {"[", "x", "]", 0},      {"^[", "x", "]", 1},  {"^[", "", "", 0},
        {"]", "", "", 0},        {"^[ \t ] ", "", "", 0}, {"^[a] ", "", "", 1}, {"^[a [x](u) ", "z", " b]", 1},
        {"^[a ", "^[]", "]", 1},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].left);
            }
            markdown_core_strbuf_puts(&source, cases[c].middle);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].right);
            }
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_setup(
                (const char *)source.ptr, (size_t)source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial inline footnotes parse");
            OK(runner, work.footnote_body <= (size_t)source.size,
               "nonblank evidence inspects disjoint source ranges: case=%zu size=%d work=%zu", c, source.size,
               work.footnote_body);
            if (root) {
                node_list notes = footnotes_of(root);
                node_list citations = nodes_of_kind(root, MARKDOWN_CORE_NODE_CITATION);
                size_t owned = 0;
                for (size_t i = 0; i < citations.count; i++) {
                    owned += citations.nodes[i]->as.citation->note != NULL;
                }
                bool unlabeled = owned == notes.count;
                for (size_t i = 0; i < notes.count; i++) {
                    unlabeled &= footnote_label_of(notes.nodes[i]) == NULL;
                }
                OK(runner, unlabeled, "every inline note is an unlabeled Footnote its citation owns");
                INT_EQ(runner, notes.count, count * cases[c].notes_per_unit,
                       "all and only completed notes are retained");
                free(citations.nodes);
                free(notes.nodes);
                markdown_core_node_free(root);
            }
            markdown_core_strbuf_free(&source);
        }
    }
    /* An authored label is only a label: an inline note never takes one, so
     * no label an author writes is reserved against the notes. */
    static const char authored[] = "^[x]\n\n[^inline-1]: authored\n";
    markdown_core_node *root = markdown_core_parse_document(authored, sizeof(authored) - 1);
    OK(runner, root != NULL, "an authored inline-looking label parses");
    if (root) {
        node_list notes = footnotes_of(root);
        OK(runner,
           notes.count == 2 && footnote_label_of(notes.nodes[0]) == NULL && footnote_label_of(notes.nodes[1]) &&
               !strcmp(footnote_label_of(notes.nodes[1]), "inline-1"),
           "the note stays unlabeled and the definition keeps its authored label");
        free(notes.nodes);
        markdown_core_node_free(root);
    }
}

/* Definitions, closing brackets, and deferred fields commit in different
 * orders. All must already be registered before finalization, including
 * notes whose enclosing candidate fails or becomes a link or image. */
static void footnote_registration(test_batch_runner *runner) {
    static const struct {
        const char *source;
        size_t count;
        const char *labels[6];
    } cases[] = {
        {"^[outer ^[inner]] :d[^[label]] ^[last]\n\n[^inline-1]: ^[body]\n",
         6,
         {NULL, NULL, NULL, NULL, "inline-1", NULL}},
        {"^[unclosed ^[inner]", 1, {NULL}},
        {"[^a]: ^[body]\n\n[^a ^[x]] [^[link]](u) ![^[image]](v)\n", 5, {"a", NULL, NULL, NULL, NULL}},
        {"`^[code]` $^[formula]$ %%^[comment]%% <!-- ^[html] --> <i data-x=\"^[token]\">\n", 0, {NULL}},
        {"[[^[target]]]\n", 1, {NULL}},
        {"[^a]: first\n[^a]: duplicate\n", 2, {"a", "a"}},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        inline_work work = {0};
        markdown_core_node *root = markdown_core_parse_document_with_setup(cases[c].source, strlen(cases[c].source),
                                                                           measure_inline_work, &work);
        OK(runner, root != NULL, "footnote registration boundaries parse: case=%zu", c);
        if (root) {
            node_list notes = footnotes_of(root);
            for (size_t i = 0; i < notes.count && i < cases[c].count; i++) {
                const char *label = footnote_label_of(notes.nodes[i]);
                OK(runner, cases[c].labels[i] ? label && !strcmp(label, cases[c].labels[i]) : label == NULL,
                   "case=%zu footnote %zu carries its label where it was written", c, i);
            }
            INT_EQ(runner, notes.count, cases[c].count, "each committed note is one Footnote in the tree");
            free(notes.nodes);
            markdown_core_node_free(root);
        }
    }
}

/* A standalone formula is promoted to a FormulaBlock only where the paragraph
 * wrapping it has a parent to take the substitute. As a definition term it is
 * the root of an owned subtree, which has none: the promotion is skipped and
 * the term keeps the inline Formula. It used to be attempted and its missing
 * parent reported to the caller as a failed allocation. */
static void standalone_formula_as_owned_root(test_batch_runner *runner) {
    markdown_core_node *root = parse("$$x$$\n: Definition body\n");
    OK(runner, root != NULL, "a formula-only definition term parses");
    if (!root) {
        return;
    }
    markdown_core_node *found = NULL;
    tree_iter *iter = tree_iter_new(root);
    markdown_core_event_type event;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event == MARKDOWN_CORE_EVENT_ENTER && node->kind == MARKDOWN_CORE_NODE_DEFINITION) {
            found = node->as.definition->term;
            break;
        }
    }
    tree_iter_free(iter);
    OK(runner, found != NULL, "the definition carries a term");
    if (found) {
        INT_EQ(runner, found->kind, MARKDOWN_CORE_NODE_PARAGRAPH, "the term root is still its owner's paragraph");
        OK(runner, child_at(found, 0) && child_at(found, 0)->kind == MARKDOWN_CORE_NODE_FORMULA,
           "the term holds the inline formula");
    }
    markdown_core_node_free(root);
}

/* The caller may remove every footnote from the finished tree: definitions
 * from the content, and inline notes from their citations. Returns how many
 * it freed. */
static size_t remove_footnotes(markdown_core_node *root) {
    size_t removed = 0;
    node_list notes = footnotes_of(root);
    node_list cites = nodes_of_kind(root, MARKDOWN_CORE_NODE_CITE);
    /* Detach every note from its owner first, so freeing one never reaches
     * another through it: an inline note from its citation, and a definition
     * from the children of the block that holds it. */
    for (size_t i = 0; i < cites.count; i++) {
        for (size_t at = 0; at < cites.nodes[i]->child_count; at++) {
            cites.nodes[i]->children[at]->as.citation->note = NULL;
        }
    }
    tree_iter *iter = tree_iter_new(root);
    for (markdown_core_event_type event; (event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE;) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event == MARKDOWN_CORE_EVENT_ENTER) {
            uint32_t kept = 0;
            for (uint32_t at = 0; at < node->child_count; at++) {
                if (node->children[at]->kind != MARKDOWN_CORE_NODE_FOOTNOTE) {
                    node->children[kept++] = node->children[at];
                }
            }
            node->child_count = kept;
        }
    }
    tree_iter_free(iter);
    for (size_t i = 0; i < notes.count; i++) {
        markdown_core_node_free(notes.nodes[i]);
        removed++;
    }
    free(cites.nodes);
    free(notes.nodes);
    return removed;
}

static void footnote_removal(test_batch_runner *runner) {
    static const char source[] = "^[outer ^[inner]] :d[^[label]]\n\n[^n]: ^[body]\n";
    markdown_core_node *root = parse(source);
    OK(runner, root != NULL, "a document with nested footnotes parses");
    if (root) {
        INT_EQ(runner, remove_footnotes(root), 5, "the caller reaches every definition and inline note");
        markdown_core_node *cite = child_at(child_at(root, 0), 0);
        node_list notes = footnotes_of(root);
        OK(runner,
           notes.count == 0 && child_at(cite, 0)->kind == MARKDOWN_CORE_NODE_CITATION &&
               !child_at(cite, 0)->as.citation->note,
           "no footnote remains and a Cite holds only its citations");
        free(notes.nodes);
        markdown_core_node *note = markdown_core_node_new(MARKDOWN_CORE_NODE_FOOTNOTE);
        OK(runner, !append_child(cite, note), "Cite containment rejects a Footnote child");
        markdown_core_node_free(note);
        markdown_core_node_free(root);
    }
}

/* Attribute recognition work is totalled in the parse's attribute workspace,
 * so the probes run against a real parser, as the directive's instance. */
static void probe_directive_openers(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->init(self, parser);
    test_batch_runner *runner = parser->context;
    const markdown_core_element_instance *directive =
        markdown_core_parser_instance(parser, &MARKDOWN_CORE_ELEMENT_DIRECTIVE);
    text_counting = 1;
    const char *directives[] = {"::: {.a k=1} junk\n", "::: {.a k=1}\n", "::: classname junk\n", "::: classname\n",
                                "::: {.a k=1\n"};
    for (size_t i = 0; i < sizeof(directives) / sizeof(*directives); i++) {
        markdown_core_chunk input = {(unsigned char *)directives[i], (bufsize_t)strlen(directives[i]), 0};
        size_t work = parser->attribute_scratch.work;
        text_allocation_calls = 0;
        int matched = MARKDOWN_CORE_ELEMENT_DIRECTIVE.opens->probe(directive, parser, &input, 0, 0, NULL);
        INT_EQ(runner, matched, i == 1 || i == 3, "directive probe validates the complete opener");
        OK(runner, text_allocation_calls <= (i == 2 || i == 3 ? 0 : 1),
           "directive probes allocate no semantic attributes");
        if (i != 2 && i != 3) {
            OK(runner, parser->attribute_scratch.work > work, "probe attribute recognition work is accounted");
        }
    }
}

static bool begin_with_directive_probes(markdown_core_dialect_builder *builder, void *context) {
    static document_owner document;
    (void)context;
    return attach_document_owner(builder, &document, "directive-prober", probe_directive_openers);
}

/* Recognition may allocate its shared suffix index, but never semantic
 * attribute strings or row cells that would be thrown away by a probe. */
static void speculative_probe_allocations(test_batch_runner *runner) {
    text_counting = 1;
    markdown_core_node *root = markdown_core_parse_document_with_setup("", 0, begin_with_directive_probes, runner);
    OK(runner, root != NULL, "directive probes run inside a parse");
    markdown_core_node_free(root);
    /* Instances with no record: these paths read none. */
    const markdown_core_element_instance table_instance = {.element = &MARKDOWN_CORE_ELEMENT_TABLE};
    const markdown_core_element_instance autolink_instance = {.element = &MARKDOWN_CORE_ELEMENT_AUTOLINK};
    const char *rows[] = {"| a | b |\n", "| a \\| b | c |\n", "\n"};
    for (size_t i = 0; i < sizeof(rows) / sizeof(*rows); i++) {
        markdown_core_parser parser = {0};
        /* A pipe table: one with no dash runs. */
        markdown_core_table value = {0};
        markdown_core_node table = {.kind = MARKDOWN_CORE_NODE_TABLE, .opaque = &value};
        text_allocation_calls = 0;
        int matched = MARKDOWN_CORE_ELEMENT_TABLE.lines->matches(&table_instance, &parser, (unsigned char *)rows[i],
                                                                 (int)strlen(rows[i]), &table);
        INT_EQ(runner, matched, i != 2, "table continuation grammar is retained");
        INT_EQ(runner, text_allocation_calls, 0, "table continuation allocates no temporary geometry");
    }
    {
        markdown_core_parser parser = {0};
        markdown_core_node *paragraph = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
        markdown_core_strbuf_sets(&paragraph->content, "| a | b |\n");
        unsigned char delimiter[] = "| --- |\n";
        text_allocation_calls = 0;
        markdown_core_node *table = MARKDOWN_CORE_ELEMENT_TABLE.opens->open(&table_instance, 0, &parser, paragraph,
                                                                            delimiter, sizeof(delimiter) - 1);
        OK(runner, table == NULL && paragraph->kind == MARKDOWN_CORE_NODE_PARAGRAPH,
           "a mismatched header remains a paragraph");
        INT_EQ(runner, text_allocation_calls, 0, "mismatched pipe header allocates no row or cell geometry");
        markdown_core_node_free(paragraph);
    }
    for (size_t length = 64; length <= 65536; length *= 4) {
        markdown_core_parser parser = {0};
        markdown_core_node *paragraph = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
        markdown_core_node *text = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
        char *source = malloc(length + 1);
        memset(source, 'a', length);
        source[length - 1] = '@'; /* A failed candidate, as well as ordinary text. */
        source[length] = 0;
        set_literal(text, source);
        /* The Text sits in its paragraph's inline tree under construction. */
        markdown_core_scaffold_append(paragraph, text);
        const unsigned char *original = text->as.literal->data;
        void *state = NULL;
        text_allocation_calls = 0;
        /* The step as completion asks it: at the Text's EXIT, outside a
         * Link, with a fresh per-root state word. */
        markdown_core_finish_result result = MARKDOWN_CORE_ELEMENT_AUTOLINK.finish->step(
            &autolink_instance, &parser, text, paragraph, MARKDOWN_CORE_EVENT_EXIT, &state);
        INT_EQ(runner, result, MARKDOWN_CORE_FINISH_CONTINUE, "a failed email scan leaves the Text in place");
        OK(runner, text->as.literal->data == original, "a failed email scan retains the original owned buffer");
        OK(runner, markdown_core_scaffold_first(paragraph) == text && !markdown_core_scaffold_next(text),
           "failed email scan preserves the tree");
        INT_EQ(runner, text_allocation_calls, 0, "a no-hit step allocates nothing: it has no walk of its own");
        markdown_core_scaffold_unlink(text);
        markdown_core_node_free(text);
        markdown_core_node_free(paragraph);
        free(source);
    }
    text_counting = 0;
}

/* Known-literal runs occupy the same text slice as surrounding prose.
 * Their allocation count equals ordinary text with the same whitespace and byte length;
 * scanning each run once also bounds work independently of node allocation. */
static void literal_text_allocations(test_batch_runner *runner) {
    text_counting = 1;
    static const struct {
        const char *prefix, *unit;
    } cases[] = {
        {"a", "@@@ "},
        {"a", "-; "},
        {"a", "a+"},
        {"a", "+a"},
        {"a", "a=b"},
        {"a", " + "},
        {"a", " ++ "},
        {"a", " === "},
        {"a", " _ "},
        {"a", "a_b"},
        {"a", " * "},
        {"a", "a+b=c_d"},
        {"a", "α_β"},
        // Close-only runs without an earlier opener, including odd/long runs.
        {"a", "C++ "},
        {"a", "x+++ "},
        {"a", "x++++ "},
        {"a", "x== "},
        {"a", "x=== "},
        {"a", "x* "},
        {"a", "x** "},
        {"a", "x*** "},
        {"a", "x_ "},
        {"a", "x__ "},
        {"a", "x___ "},
        {"a", "x* x_ x== C++ "},
        // An opener of another rule cannot make these closers eligible.
        {"*other ", "C++ "},
        {"++other ", "x== "},
        {"==other ", "x_ "},
        {"_other ", "x* "},
        // Bracket completion removes both unmatched and consumed openers.
        {"[++open](/u) a", "C++ "},
        {"[++x++](/u) a", "C++ "},
        {"[==open](/u) a", "x== "},
        {"[==x==](/u) a", "x== "},
        {"[*open](/u) a", "x* "},
        {"[*x*](/u) a", "x* "},
        {"[_open](/u) a", "x_ "},
        {"[_x_](/u) a", "x_ "},
    };
    for (size_t size = 1024; size <= 1048576; size *= 2) {
        for (size_t shape = 0; shape < sizeof(cases) / sizeof(*cases); shape++) {
            size_t prefix_length = strlen(cases[shape].prefix);
            size_t width = strlen(cases[shape].unit);
            size_t repeats = size / width;
            size_t length = prefix_length + repeats * width + 1;
            char *source = malloc(length);
            size_t ordinary_allocations = 0;
            size_t ordinary_work = 0;
            int ordinary_text_start = 0;
            for (size_t pass = 0; pass < 2; pass++) {
                memset(source, 'a', length);
                memcpy(source, cases[shape].prefix, prefix_length);
                source[length - 1] = 'z';
                for (size_t i = 0; i < repeats; i++) {
                    memcpy(source + prefix_length + i * width, cases[shape].unit, width);
                }
                if (!pass) {
                    for (size_t i = prefix_length; i < length; i++) {
                        if (source[i] != ' ') {
                            source[i] = 'a';
                        }
                    }
                    source[length - 1] = 'z';
                }
                text_allocation_calls = 0;
                inline_work work = {0};
                markdown_core_node *root =
                    markdown_core_parse_document_with_setup(source, length, measure_inline_work, &work);
                OK(runner, root != NULL, "literal text parses: shape=%zu size=%zu", shape, length);
                if (!pass) {
                    ordinary_allocations = text_allocation_calls;
                    ordinary_work = work.delimiters;
                } else {
                    INT_EQ(runner, text_allocation_calls, ordinary_allocations,
                           "known-literal runs allocate only for their surrounding text slice");
                }
                size_t delimiter_bytes = 0;
                for (size_t i = prefix_length; i < length; i++) {
                    if (strchr("*_+=", source[i])) {
                        delimiter_bytes++;
                    }
                }
                INT_EQ(runner, work.delimiters, ordinary_work + delimiter_bytes,
                       "known-literal delimiter runs are scanned once without matching work");
                if (root) {
                    markdown_core_node *text = last_child_of(child_at(root, 0));
                    if (!pass) {
                        ordinary_text_start = START_COLUMN(text);
                    }
                    size_t text_offset = (size_t)ordinary_text_start - 1;
                    OK(runner,
                       text->kind == MARKDOWN_CORE_NODE_TEXT &&
                           text->as.literal->len == (bufsize_t)(length - text_offset) &&
                           memcmp(text->as.literal->data, source + text_offset, length - text_offset) == 0,
                       "one Text retains every literal byte");
                    OK(runner,
                       START_LINE(text) == 1 && START_COLUMN(text) == ordinary_text_start && END_LINE(text) == 1 &&
                           END_COLUMN(text) == (int)length,
                       "one Text retains the complete source scope");
                    markdown_core_node_free(root);
                }
            }
            free(source);
        }
    }
    text_counting = 0;
}

typedef struct {
    const char *left, *middle, *right;
    size_t nodes_per_unit;
} paired_delimiter_case;

static void paired_delimiter_linear_work(test_batch_runner *runner, markdown_core_node_type kind,
                                         const paired_delimiter_case *cases, size_t case_count) {
    for (size_t c = 0; c < case_count; c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].left);
            }
            markdown_core_strbuf_puts(&source, cases[c].middle);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].right);
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial paired-delimiter runs parse successfully");
            OK(runner,
               work.delimiters <= 8 * (size_t)source.size && work.attributes <= 16 * (size_t)source.size &&
                   work.whitespace <= 2 * (size_t)source.size && work.brackets <= 8 * (size_t)source.size,
               "shared inline work is linear: case=%zu size=%d delimiters=%zu attributes=%zu whitespace=%zu "
               "brackets=%zu",
               c, source.size, work.delimiters, work.attributes, work.whitespace, work.brackets);
            size_t nodes = 0;
            tree_iter *iter = tree_iter_new(root);
            markdown_core_event_type event;
            while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
                if (event == MARKDOWN_CORE_EVENT_ENTER && tree_iter_get_node(iter)->kind == kind) {
                    nodes++;
                }
            }
            tree_iter_free(iter);
            size_t expected = count * cases[c].nodes_per_unit;
            INT_EQ(runner, nodes, expected, "pairwise matching preserves the semantic paired-delimiter count");
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void mark_linear_work(test_batch_runner *runner) {
    static const paired_delimiter_case cases[] = {
        {"=", "", "", 0},                                       // one maximal run: no empty mark
        {"==", "x", "==", 1},                                   // one nested pair per repeat
        {"==a ", "", "", 0},                                    // unmatched openers
        {" a==", "", "", 0},                                    // unmatched closers
        {"==a* ", "", "", 0},                                   // failed searches across another rule
        {"==a* ", "", " b==", 1},                               // mixed nested runs
        {"==a===b== ", "", "", 1},                              // odd leftovers must never match
        {"==*a*== ", "", "", 1},                                // parsed child ownership
        {"==a====b== ", "", "", 2},                             // adjacent marks
        {"[==a==](/u) ", "", "", 1},                            // separate inline containers
        {"<i title=\"==hidden==\">==*body*==</i> ", "", "", 1}, // token opacity, live body
        {"==<i title=\"==hidden==\">body</i>== ", "", "", 1},   // tag delimiters stay owned
        {"==a %%==b%% c== ", "", "", 1},                        // comment cannot close the mark
    };
    paired_delimiter_linear_work(runner, MARKDOWN_CORE_NODE_MARK, cases, sizeof(cases) / sizeof(*cases));
}

static void insertion_linear_work(test_batch_runner *runner) {
    static const paired_delimiter_case cases[] = {
        {"+", "", "", 0},                                       // one maximal run: no empty insertion
        {"++", "x", "++", 1},                                   // one nested pair per repeat
        {"++a ", "", "", 0},                                    // unmatched openers
        {" a++", "", "", 0},                                    // unmatched closers
        {"++a* ", "", "", 0},                                   // failed searches across another rule
        {"++a* ", "", " b++", 1},                               // mixed nested runs
        {"++a+++b++ ", "", "", 1},                              // odd leftovers must never match
        {"++*a*++ ", "", "", 1},                                // parsed child ownership
        {"++a++++b++ ", "", "", 2},                             // adjacent insertions
        {"[++a++](/u) ", "", "", 1},                            // separate inline containers
        {"<i title=\"++hidden++\">++*body*++</i> ", "", "", 1}, // token opacity, live body
        {"++<i title=\"++hidden++\">body</i>++ ", "", "", 1},   // tag delimiters stay owned
        {"++a %%++b%% c++ ", "", "", 1},                        // comment cannot close the insertion
    };
    paired_delimiter_linear_work(runner, MARKDOWN_CORE_NODE_INSERTION, cases, sizeof(cases) / sizeof(*cases));
}

/* P5/P6: nesting, unmatched candidates, empty pairs and raw whitespace
 * all use the same bracket/delimiter operations. Bounds count work, not time. */
static void span_and_script_linear_work(test_batch_runner *runner) {
    static const paired_delimiter_case spans[] = {
        {"[a ", "x", "]{.c}", 1}, {"[", "x", "]{}", 1}, {"[a]{} ", "", "", 1},     {"[a]{k=\"", "", "", 0},
        {"[a]{.c ", "", "", 0},   {"[", "", "", 0},     {"[a^b", "x", "]{.c}", 1}, {"[a~b", "x", "]{.c}", 1},
    };
    static const paired_delimiter_case superscripts[] = {
        {"^^", "", "", 1},
        {"^^x^y^ ", "", "", 2},
        {"*^^* ", "", "", 1},
        {"^a^ ", "", "", 1},
        {"^a^b^c^ ", "", "", 2},
        {"^a ", "", "", 0},
        {"^a\t", "", "", 0},
        {"^a\xe2\x80\x83"
         "b^ ",
         "", "", 1},            /* EM SPACE is not whitespace */
        {"^a\vb^ ", "", "", 1}, /* nor is vertical tab */
        {"^*a\\ b*^ ", "", "", 1},
        {"[^a^]{} ", "", "", 1},
        {"^[^a^] ", "", "", 0}, /* detached notes are visited by the value walker */
        {"^a ", "", "]", 0},    /* unmatched brackets cannot rescan the delimiter stack */
        {"^a`x y`b^ ", "", "", 1},
        {"^a%%x y%%b^ ", "", "", 1},
        {"^a&#32;b^ ", "", "", 1},
        {"^:d[:n[a b]]^ ", "", "", 0},
        {"^:d[:n[a\\ b]]^ ", "", "", 1},
        {"^:d[`a b`]^ ", "", "", 1},
        {"^:d[a&#32;b]^ ", "", "", 1},
        {"^a[ ", "x", "]{}^ ", 0}, /* each closed scope retains one boundary */
        {"^a* ", "x", " b*^ ", 0},
        {"^a*^b^ ", "x", " c*^ ", 1},
        {"^a~~b c~~^ ", "", "", 0},
        {"^a[==b c==]{}^ ", "", "", 0},
        {"^a[**b&#32;c**]{}^ ", "", "", 1},
    };
    static const paired_delimiter_case subscripts[] = {
        {"~", "", "", 0},
        {"~~~", "", "", 0},
        {"~a~ ", "", "", 1},
        {"~a~~b~ ", "", "", 1},
        {"~a ", "", "", 0},
        {"~^a^~ ", "", "", 1},
        {"~*a\\ b*~ ", "", "", 1},
        {"~:d[:n[a b]]~ ", "", "", 0},
        {"~:d[:n[a\\ b]]~ ", "", "", 1},
        {"~a[ ", "x", "]{}~ ", 0},
        {"~a~~b c~~z~ ", "", "", 0},
        {"~a[**b&#32;c**]{}~ ", "", "", 1},
    };
    paired_delimiter_linear_work(runner, MARKDOWN_CORE_NODE_SPAN, spans, sizeof(spans) / sizeof(*spans));
    paired_delimiter_linear_work(runner, MARKDOWN_CORE_NODE_SUPERSCRIPT, superscripts,
                                 sizeof(superscripts) / sizeof(*superscripts));
    paired_delimiter_linear_work(runner, MARKDOWN_CORE_NODE_SUBSCRIPT, subscripts,
                                 sizeof(subscripts) / sizeof(*subscripts));

    /* A suspended heading resumes with the same bracket/delimiter state.
     * Projection visits nested formatting once and reserves a later Span id. */
    for (size_t count = 128; count <= 8192; count *= 2) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "# ");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_putc(&source, '[');
        }
        markdown_core_strbuf_puts(&source, "^a\\ b^ ~x~");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, "]{}");
        }
        markdown_core_strbuf_puts(&source, "\n\n[owner]{#a-b-x}\n");
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "deep Span/script headings parse successfully");
        if (root) {
            STR_EQ(runner, (const char *)child_at(root, 0)->attributes.anchor.data, "a-b-x-1",
                   "heading projection includes script bodies and reserves the later Span anchor");
            OK(runner,
               work.anchors < 25 * (size_t)source.size && work.brackets <= 8 * (size_t)source.size &&
                   work.delimiters <= 8 * (size_t)source.size,
               "heading projection and mixed bracket/delimiter ownership stay linear");
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

static size_t count_kind(markdown_core_node *root, markdown_core_node_type kind) {
    size_t found = 0;
    tree_iter *iter = tree_iter_new(root);
    markdown_core_event_type event;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (event == MARKDOWN_CORE_EVENT_ENTER && tree_iter_get_node(iter)->kind == kind) {
            found++;
        }
    }
    tree_iter_free(iter);
    return found;
}

/* Eligibility must preserve future openers, opener units and bracket scope.
 * In particular, an opener count is not a count of available delimiter units. */
static void core_delimiter_stack_eligibility(test_batch_runner *runner) {
    static const struct {
        const char *marker;
        markdown_core_node_type kind;
    } rules[] = {
        {"*", MARKDOWN_CORE_NODE_EMPHASIS},
        {"_", MARKDOWN_CORE_NODE_EMPHASIS},
        {"==", MARKDOWN_CORE_NODE_MARK},
        {"++", MARKDOWN_CORE_NODE_INSERTION},
    };
    static const struct {
        const char *format;
        size_t nodes;
    } cases[] = {
        {"x%s %sy%s", 1},           // Literal closer before a later valid opener.
        {"%s%sa%s b%s", 2},         // One opener run supplies two separate closers.
        {"%s[x](/u)%s", 1},         // An outer opener survives bracket completion.
        {"[%sx](/u) y%s %sz%s", 1}, // An inner opener is discarded at the bracket.
        {"(%s!x%s)", 1},            // A run with both roles can open an empty stack.
    };
    for (size_t rule = 0; rule < sizeof(rules) / sizeof(*rules); rule++) {
        for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
            const char *marker = rules[rule].marker;
            char source[128];
            snprintf(source, sizeof(source), cases[c].format, marker, marker, marker, marker);
            markdown_core_node *root = markdown_core_parse_document(source, strlen(source));
            OK(runner, root != NULL, "core delimiter eligibility parses: %s", source);
            if (root) {
                INT_EQ(runner, count_kind(root, rules[rule].kind), cases[c].nodes,
                       "core delimiter eligibility preserves matching: %s", source);
                markdown_core_node_free(root);
            }
        }
    }
}

/* O3: the inline `%%` scanner. Every failed closer search is cached under the
 * comment's rule, so a run of signs that never closes, a body that crosses
 * another construct's bytes, and a comment beside every earlier opaque
 * construct all cost work proportional to the input. */
static void comment_inline_linear_work(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *unit, *suffix;
        size_t comments_per_unit; /* comments per unit, or 0 when the count is asserted separately */
    } cases[] = {
        {"", "%%a%% ", "", 1},                // closed comments
        {"", "%%%%", "", 1},                  // adjacent empty comments
        {"", "%", "", 0},                     // one maximal run: pairs close, at most one sign is text
        {"", "%%%a", "", 0},                  // the leftover of each run opens the next candidate
        {"%%", "a", "", 0},                   // one opener, one long failed search
        {"", "%% a", "", 0},                  // openers whose closers are the next openers
        {"", "\\%%a", "", 0},                 // escaped signs never open
        {"", "$%%$ ", "", 0},                 // formula bodies own their signs
        {"", "`%%` ", "", 0},                 // code spans own their signs
        {"", "[[%%]] ", "", 0},               // cross links own their signs
        {"", "%%[[a", "", 0},                 // comment bodies own brackets
        {"", "==%%a%%== ", "", 1},            // marks around comments
        {"", "%%a%%%%b%% <!-- c -->", "", 3}, // both grammars side by side, three Comment nodes
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial percent runs parse successfully");
            OK(runner, work.comment + work.opaque <= 4 * (size_t)source.size,
               "comment scanner work is linear: case=%zu size=%d comment=%zu opaque=%zu", c, source.size, work.comment,
               work.opaque);
            if (cases[c].comments_per_unit) {
                INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_COMMENT), count * cases[c].comments_per_unit,
                       "every closed comment is one Comment node");
            }
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* O3: the block form's lookahead. A candidate scans forward under the open
 * containers' prefixes; nested containers each carrying a candidate that never
 * closes are the shape that would rescan every line once per container, and
 * blank runs inside nested list items are the shape that would visit every
 * blank line once per candidate. The recorded work counts lines visited and
 * prefix bytes matched, and stays within a constant of the input size. */
static void comment_block_linear_work(test_batch_runner *runner) {
    for (int shape = 0; shape < 6; shape++) {
        for (size_t depth = 16; depth <= 128; depth *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            size_t expected_comments = 0;
            size_t expected_paragraphs = 0;
            size_t i;
            switch (shape) {
            case 0: /* nested block quotes, each opening a candidate, no closer anywhere */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 0; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "> ");
                    }
                    markdown_core_strbuf_puts(&source, "%%\n");
                }
                expected_paragraphs = depth;
                break;
            case 1: /* nested block quotes whose deepest candidate closes; the outer ones cannot */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 0; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "> ");
                    }
                    markdown_core_strbuf_puts(&source, "%%\n");
                }
                for (size_t j = 0; j < depth; j++) {
                    markdown_core_strbuf_puts(&source, "> ");
                }
                markdown_core_strbuf_puts(&source, "x\n");
                for (size_t j = 0; j < depth; j++) {
                    markdown_core_strbuf_puts(&source, "> ");
                }
                markdown_core_strbuf_puts(&source, "%%\n");
                expected_comments = 1;
                expected_paragraphs = depth - 1;
                break;
            case 2: /* nested list items, each opening a candidate, then a long blank run */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 1; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "  ");
                    }
                    markdown_core_strbuf_puts(&source, "- %%\n");
                }
                for (i = 0; i < depth * depth; i++) {
                    markdown_core_strbuf_puts(&source, "\n");
                }
                expected_paragraphs = depth;
                break;
            case 3: /* nested list items with blank runs between non-blank continuation lines */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 1; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "  ");
                    }
                    markdown_core_strbuf_puts(&source, "- %%\n");
                }
                for (i = 0; i < depth; i++) {
                    for (size_t j = 0; j < depth; j++) {
                        markdown_core_strbuf_puts(&source, "\n");
                    }
                    for (size_t j = 0; j < depth; j++) {
                        markdown_core_strbuf_puts(&source, "  ");
                    }
                    markdown_core_strbuf_puts(&source, "x\n");
                }
                expected_paragraphs = depth + depth;
                break;
            default: /* many candidates in sibling items: each scan ends at its item's end */
                for (i = 0; i < depth * depth; i++) {
                    markdown_core_strbuf_puts(&source, "- %%\n  a\n");
                }
                expected_paragraphs = depth * depth;
                break;
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "nested block comment candidates parse successfully");
            OK(runner, work.lookahead <= 4 * (size_t)source.size,
               "block lookahead work is linear: shape=%d size=%d work=%zu", shape, source.size, work.lookahead);
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_COMMENT_BLOCK), expected_comments,
                   "only a closed candidate is a block comment: shape=%d depth=%zu", shape, depth);
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_PARAGRAPH), expected_paragraphs,
                   "a failed candidate's line is paragraph text: shape=%d depth=%zu", shape, depth);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* A transaction's line facts outlive the text they were read from: a
 * session's text moves when it grows, and the next commit reads the facts an
 * earlier one recorded. Nested candidates over a blank run are the shape
 * whose later scans step over the run the first scan recorded. A session's
 * first commit reads every line but the last from a copy of the text that is
 * then overwritten, and the finalize's commit reads the whole text from
 * another copy; the document is the one parsed whole. */
static void line_facts_survive_moved_text(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    const size_t depth = 16;
    for (size_t i = 1; i <= depth; i++) {
        for (size_t j = 1; j < i; j++) {
            markdown_core_strbuf_puts(&source, "  ");
        }
        markdown_core_strbuf_puts(&source, "- %%\n");
    }
    for (size_t i = 0; i < depth; i++) {
        for (size_t j = 0; j < depth; j++) {
            markdown_core_strbuf_puts(&source, "\n");
        }
        for (size_t j = 0; j < depth; j++) {
            markdown_core_strbuf_puts(&source, "  ");
        }
        markdown_core_strbuf_puts(&source, "x\n");
    }
    const size_t size = (size_t)source.size;
    size_t last_line = size - 1;
    while (source.ptr[last_line - 1] != '\n') {
        last_line--;
    }
    char *before = malloc(size), *after = malloc(size);
    markdown_core_parser *parser = markdown_core_core_parser(NULL, NULL);
    markdown_core_node_pool pool = {0};
    markdown_core_revision revision = {.pool = &pool};
    markdown_core_node *moved = NULL;
    if (before && after && parser) {
        memcpy(before, source.ptr, size);
        markdown_core_parser_start(parser, &revision);
        markdown_core_parser_commit(parser, before, last_line, false);
        memcpy(after, source.ptr, size);
        memset(before, 'x', size);
        markdown_core_parser_commit(parser, after, size, true);
        moved = markdown_core_parser_finalize(parser);
    }
    markdown_core_node *whole = markdown_core_parse_document((const char *)source.ptr, size);
    OK(runner, moved && whole, "both parses succeed");
    if (moved && whole) {
        INT_EQ(runner, count_kind(moved, MARKDOWN_CORE_NODE_PARAGRAPH), count_kind(whole, MARKDOWN_CORE_NODE_PARAGRAPH),
               "the commits after the text moved read the paragraphs a whole parse reads");
        INT_EQ(runner, count_kind(moved, MARKDOWN_CORE_NODE_LIST_ITEM), count_kind(whole, MARKDOWN_CORE_NODE_LIST_ITEM),
               "the commits after the text moved read the items a whole parse reads");
    }
    if (moved) {
        markdown_core_node_pool_release(&pool, moved);
    }
    markdown_core_node_free(whole);
    if (parser) {
        markdown_core_parser_destroy(parser);
    }
    markdown_core_node_pool_dispose(&pool);
    free(before);
    free(after);
    markdown_core_strbuf_free(&source);
}

/* O3: the `%%` forms produce the `Comment` kind M0 added, on the engine's own
 * accessors, with the type strings and positions the HTML forms have. */
static void percent_comment_nodes(test_batch_runner *runner) {
    static const char markdown[] = "a %%b%% c %%%% %%x\r\ny%% d\r\n"
                                   "\r\n"
                                   "%%\r\n"
                                   "  block\r\n"
                                   "\r\n"
                                   "%%\t\r\n"
                                   "\r\n"
                                   "> %%\r\n"
                                   "> q\r\n"
                                   "> %%\r\n"
                                   "\r\n"
                                   "%%\r\n"
                                   "open\r\n";
    /* The engine entry with the dialect attached: `%%` is an element's
     * syntax, unlike the HTML comment of `comment_nodes` above. */
    markdown_core_node *doc = markdown_core_parse_document_with_setup(markdown, sizeof(markdown) - 1, NULL, NULL);
    markdown_core_node *paragraph = child_at(doc, 0);
    markdown_core_node *comment = child_at(paragraph, 1);
    markdown_core_node *empty = child_at(paragraph, 3);
    markdown_core_node *spanning = child_at(paragraph, 5);
    markdown_core_node *block = child_at(doc, 1);
    markdown_core_node *quote = child_at(doc, 2);
    markdown_core_node *quoted = child_at(quote, 0);
    markdown_core_node *open = child_at(doc, 3);

    INT_EQ(runner, comment->kind, MARKDOWN_CORE_NODE_COMMENT, "inline `%%` comment type");
    STR_EQ(runner, markdown_core_node_get_type_string(comment), "comment", "inline `%%` comment type string");
    LITERAL_EQ(runner, comment, "b", "inline literal excludes the delimiters");
    INT_EQ(runner, START_COLUMN(comment), 3, "inline scope starts at the opener");
    INT_EQ(runner, END_COLUMN(comment), 7, "inline scope ends at the closer");
    INT_EQ(runner, empty->kind, MARKDOWN_CORE_NODE_COMMENT, "`%%%%` is a comment");
    LITERAL_EQ(runner, empty, "", "`%%%%` has an empty literal");
    LITERAL_EQ(runner, spanning, "x\ny", "a body spanning lines keeps one LF per line ending");
    INT_EQ(runner, START_LINE(spanning), 1, "a spanning body starts on its opener line");
    INT_EQ(runner, END_LINE(spanning), 2, "a spanning body ends on its closer line");
    INT_EQ(runner, END_COLUMN(spanning), 3, "a spanning body ends at its closer");

    INT_EQ(runner, block->kind, MARKDOWN_CORE_NODE_COMMENT_BLOCK, "block `%%` comment type");
    STR_EQ(runner, markdown_core_node_get_type_string(block), "comment_block", "block `%%` comment type string");
    LITERAL_EQ(runner, block, "  block\n\n",
               "block literal keeps indentation, blank lines, and LF line endings, and excludes both fences");
    INT_EQ(runner, START_LINE(block), 4, "block starts on its opener line");
    INT_EQ(runner, END_LINE(block), 7, "block ends on its closer line");
    INT_EQ(runner, END_COLUMN(block), 3, "block ends at its closer line's last byte");
    OK(runner, child_at(block, 0) == NULL, "a block comment is a leaf");

    INT_EQ(runner, quote->kind, MARKDOWN_CORE_NODE_CALLOUT, "the quoted form is inside its container");
    INT_EQ(runner, quoted->kind, MARKDOWN_CORE_NODE_COMMENT_BLOCK, "a quoted block comment");
    LITERAL_EQ(runner, quoted, "q\n", "a quoted literal has its prefix removed");
    INT_EQ(runner, START_COLUMN(quoted), 3, "a quoted block starts after the prefix");

    INT_EQ(runner, open->kind, MARKDOWN_CORE_NODE_PARAGRAPH, "an unclosed candidate is a paragraph");
    LITERAL_EQ(runner, child_at(open, 0), "%%", "an unmatched opener is text");
    OK(runner, child_at(doc, 4) == NULL, "nothing follows the unclosed candidate");

    markdown_core_node_free(doc);
}

static void cross_link_fields(test_batch_runner *runner) {
    const char *source = "[[ Note ]] [[Note|]] ![[#^block|raw *label*]]";
    markdown_core_document *doc = facade_parse((const uint8_t *)source, strlen(source));
    OK(runner, doc != NULL, "cross links parse through facade");
    const markdown_core_node *paragraph = facade_child(markdown_core_document_root(doc), 0);
    const markdown_core_node *node = facade_child(paragraph, 0);
    markdown_core_destination dest = facade_destination(node);
    markdown_core_optional_string label = facade_cross_label(node);
    OK(runner, dest.kind == MARKDOWN_CORE_DESTINATION_CROSS, "cross links produce the cross destination branch");
    OK(runner, dest.path.length == 6 && memcmp(dest.path.data, " Note ", 6) == 0 && !dest.anchor.has_value,
       "path bytes are preserved and anchor is absent");
    OK(runner, markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_CROSS_LINK && !label.has_value,
       "no separator means absent label");
    OK(runner, markdown_core_node_children(node).count == 0, "a cross link is an occurrence-owned leaf");
    node = facade_child(paragraph, 2);
    label = facade_cross_label(node);
    OK(runner, label.has_value && label.value.length == 0, "an authored empty label remains present");
    node = facade_child(paragraph, 4);
    OK(runner, markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_CROSS_EMBEDDED, "transclusion has its own kind");
    OK(runner,
       strcmp(facade_kind_name(MARKDOWN_CORE_KIND_CROSS_EMBEDDED), "CrossEmbedded") == 0 &&
           facade_dimensions(node) == NULL,
       "an unsized CrossEmbedded retains its kind");
    dest = facade_destination(node);
    OK(runner,
       dest.path.length == 0 && dest.anchor.has_value && dest.anchor.value.length == 5 &&
           memcmp(dest.anchor.value.data, "block", 5) == 0,
       "block punctuation is removed, current-document path is empty");
    markdown_core_document_free(doc);
}

static void attribute_linear_work(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *unit, *suffix;
        bool valid;
    } cases[] = {
        {"{", ".a k=1 ", "}", true},    {"{", "k=1 k=2 class='a a' ", "}", true},
        {"", "{#valid ", "\"}", false}, {"", "{k=bad ", "", false},
        {"", "{k=' ", "", false},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            markdown_core_attribute_scratch scratch = {0};
            markdown_core_attribute_parser parser = {.data = source.ptr, .length = source.size, .scratch = &scratch};
            size_t attempts = 0;
            for (bufsize_t at = 0; at < source.size; at++) {
                if (source.ptr[at] != '{') {
                    continue;
                }
                markdown_core_attributes value = {0};
                bufsize_t end = -1;
                int valid = markdown_core_attributes_parse(&parser, at, &value, &end);
                attempts++;
                INT_EQ(runner, valid, cases[c].valid, "attribute recognition is atomic");
                if (valid) {
                    INT_EQ(runner, end, source.size, "complete container consumed");
                    INT_EQ(runner, value.class_count, count * (c == 1 ? 2 : 1), "class occurrences retained");
                    INT_EQ(runner, value.record_count, count * (c == 1 ? 2 : 1), "duplicate records retained");
                    OK(runner, value.storage != NULL, "the retained values live in the value's one block");
                } else {
                    INT_EQ(runner, end, -1, "failed candidate never advances caller");
                    OK(runner, !value.classes && !value.records && !value.anchor.data && !value.storage,
                       "no partial attribute value escapes");
                }
                markdown_core_attributes_free(&value);
            }
            OK(runner, parser.work <= 12 * (size_t)source.size + attempts,
               "attribute work is linear: case=%zu size=%d work=%zu", c, source.size, parser.work);
            markdown_core_attribute_parser_free(&parser);
            markdown_core_attribute_scratch_free(&scratch);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* THE FORWARD RECOGNISER IS MEMOISED, and these are the shapes that defeat a
 * forward scan without the memo: every `{` is asked, and each answer must cost
 * the bytes its own member owns, not the bytes to the end of the extent. The
 * old reverse index was oblivious to them, which is why `attribute_linear_work`
 * never needed them. Recognition only -- a valid candidate here may carry a
 * value as long as the extent, and materialising every one would be quadratic
 * output, not quadratic recognition. */
static void attribute_recognition_is_memoised(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *unit, *suffix;
        /* Valid candidates: all `{`, only the last `{`, or none. */
        enum { NONE, LAST, ALL } valid;
    } cases[] = {
        /* Each `{` fails at the next `{` in O(1); the last closes. */
        {"", "{", "k=v}", LAST},
        /* Each opener's lookahead ends at the next unit's quote. */
        {"", "{k=\" ", "", NONE},
        /* Same, then the one failing lookahead per kind scans the tail once. */
        {"", "{k='", "\"\"\"\"\"\"\"\"", NONE},
        /* One lookahead decides a value of braces; no `}` is a candidate. */
        {"{k=\"", "}", "\"}", ALL},
        /* Every candidate's value is a suffix of the first's: memoised `=`. */
        {"", "{k=x", "}", ALL},
        /* A shared tail reached through a bare name and whitespace. */
        {"", "{a ", "{b=1}", LAST},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 4) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            markdown_core_attribute_parser parser = {.data = source.ptr, .length = source.size};
            size_t attempts = 0, valid = 0;
            bufsize_t last = -1;
            for (bufsize_t at = 0; at < source.size; at++) {
                if (source.ptr[at] != '{') {
                    continue;
                }
                bufsize_t end = markdown_core_attributes_end(&parser, at);
                attempts++;
                if (end) {
                    valid++;
                    INT_EQ(runner, end, source.size, "a valid candidate closes at the extent's end");
                }
                last = at;
            }
            size_t expected = cases[c].valid == ALL ? attempts : cases[c].valid == LAST ? 1 : 0;
            INT_EQ(runner, (int)valid, (int)expected, "memoised recognition answers as the grammar does: case=%zu", c);
            OK(runner, parser.work <= 12 * (size_t)source.size + attempts,
               "recognition from every candidate is linear: case=%zu size=%d work=%zu", c, source.size, parser.work);
            /* The memo invariants: asking again costs a constant, and a walk
             * that reaches known ground stops there. */
            size_t before = parser.work;
            markdown_core_attributes_end(&parser, last);
            OK(runner, parser.work - before <= 8, "a repeated query costs a constant: case=%zu extra=%zu", c,
               parser.work - before);
            markdown_core_attribute_parser_free(&parser);
            if (c == 4) {
                /* Ask the LAST candidate first on a fresh memo: it walks its
                 * own `k=` and the whole value; then the FIRST, whose value
                 * scan steps over every memoised `=` once. Then the middle
                 * one, which reaches its memoised `=` after one name byte. */
                markdown_core_attribute_parser fresh = {.data = source.ptr, .length = source.size};
                markdown_core_attributes_end(&fresh, last);
                markdown_core_attributes_end(&fresh, 0);
                before = fresh.work;
                markdown_core_attributes_end(&fresh, (bufsize_t)(count / 2) * 4);
                OK(runner, fresh.work - before <= 8,
                   "a walk reaching a memoised `=` costs only its own bytes: extra=%zu", fresh.work - before);
                markdown_core_attribute_parser_free(&fresh);
            }
            markdown_core_strbuf_free(&source);
        }
    }
}

/* THE MEMBERS OF MANY BRACES ON ONE STRETCH, read with a shared memo, are
 * what each brace reads alone: the shapes are the ones whose walks share a
 * value's scan, an escaped `=`, a quoted lookahead, a run of members, or a
 * walk that ends where an earlier one passed. */
static void open_members_are_memoised(test_batch_runner *runner) {
    static const char *const units[] = {"{k=",      "{k=a{k=a", "{a\\=x", "{k=\"\\\"", "{k=x a ",  "{#a {.b ",
                                        "{ - k=v ", "{k='",     "{k=x}{", "{a\n{b ",   "{k=\"x\" "};
    static const char *const suffixes[] = {"", "x", " b c", "\"", "=", "}"};
    for (size_t u = 0; u < sizeof(units) / sizeof(*units); u++) {
        for (size_t f = 0; f < sizeof(suffixes) / sizeof(*suffixes); f++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            for (size_t i = 0; i < 64; i++) {
                markdown_core_strbuf_puts(&source, units[u]);
            }
            markdown_core_strbuf_puts(&source, suffixes[f]);
            markdown_core_member_memo memo = {0};
            size_t braces = 0, agreed = 0;
            for (bufsize_t at = 0; at < source.size; at++) {
                if (source.ptr[at] != '{') {
                    continue;
                }
                braces++;
                markdown_core_field alone = {0}, shared = {0};
                const bool read = markdown_core_attributes_open_member(source.ptr, source.size, at, &alone, NULL);
                const bool memoised = markdown_core_attributes_open_member(source.ptr, source.size, at, &shared, &memo);
                agreed += read == memoised &&
                          (!read || (alone.slot == shared.slot && alone.start == shared.start &&
                                     alone.end == shared.end && alone.prefix_count == shared.prefix_count &&
                                     !memcmp(alone.prefix, shared.prefix, sizeof(alone.prefix))));
            }
            INT_EQ(runner, (int)agreed, (int)braces, "a memoised brace reads its member as alone: unit=%zu suffix=%zu",
                   u, f);
            OK(runner, memo.count <= braces, "the memo holds a field per brace at most: unit=%zu suffix=%zu", u, f);
            markdown_core_member_memo_free(&memo);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* A QUOTE-LED VALUE WITH NO CLOSER IS THE UNQUOTED VALUE THAT STARTS WITH
 * THE QUOTE, whatever the scratch holds when it is read. The reader decodes a
 * quote-led value as quoted and gives back what it staged when the container
 * ends first; the first member of a container read into a fresh scratch has
 * nothing staged before it, and one with no byte after its quote stages
 * nothing at all, so the scratch is still the empty sentinel there. */
static void attribute_unclosed_quote_is_unquoted(test_batch_runner *runner) {
    static const struct {
        const char *container, *name, *value;
        size_t records;
    } cases[] = {
        {"{x=\"}", "x", "\"", 1},         {"{x='}", "x", "'", 1},       {"{x=\"a}", "x", "\"a", 1},
        {"{x=\"a\\\"}", "x", "\"a\"", 1}, {"{a=1 x=\"}", "x", "\"", 2}, {"{#i .c x='\"}", "x", "'\"", 1},
        {"{x=\" y=1}", "y", "1", 2},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        markdown_core_attribute_scratch scratch = {0};
        markdown_core_attribute_parser parser = {.data = (const unsigned char *)cases[i].container,
                                                 .length = (bufsize_t)strlen(cases[i].container),
                                                 .scratch = &scratch};
        markdown_core_attributes value = {0};
        bufsize_t end = 0;
        int read = markdown_core_attributes_parse(&parser, 0, &value, &end);
        const markdown_core_record *last = read && value.record_count ? &value.records[value.record_count - 1] : NULL;
        OK(runner,
           read && end == parser.length && value.record_count == cases[i].records && last &&
               last->name.len == (bufsize_t)strlen(cases[i].name) &&
               memcmp(last->name.data, cases[i].name, (size_t)last->name.len) == 0 &&
               last->value.len == (bufsize_t)strlen(cases[i].value) &&
               memcmp(last->value.data, cases[i].value, (size_t)last->value.len) == 0 &&
               last->value.data[last->value.len] == 0,
           "a quote with no closer reads as the unquoted value it starts: %s", cases[i].container);
        markdown_core_attributes_free(&value);
        markdown_core_attribute_parser_free(&parser);
        markdown_core_attribute_scratch_free(&scratch);
    }
}

/* A REFUSED ALLOCATION FAILS THE READ, WHEREVER IT FALLS. A container is
 * staged into the scratch before it is laid out, and every path that stages
 * -- the `-` member, `#` and `.` runs, a bare name's `true`, a record's name,
 * a decoded value, a split class run, the member list, the value's block --
 * can be refused. Each is refused in turn, with a fresh scratch so the first
 * refusal falls on an empty buffer: the read fails with its loss reported,
 * publishes nothing, reads nothing it did not stage, and leaks nothing. */
static void attribute_reads_refuse_every_allocation(test_batch_runner *runner) {
    static const char *const containers[] = {
        "{class}",
        "{id}",
        "{-}",
        "{#a}",
        "{.b}",
        "{k=v}",
        "{k=\"a &amp; b\"}",
        "{class=\"a b\"}",
        "{x=\"}",
        "{#a .b - k=v bare class=\"c d\" id=e j=\"x\\\"y\"}",
    };
    for (size_t c = 0; c < sizeof(containers) / sizeof(*containers); c++) {
        for (size_t fail_at = 1;; fail_at++) {
            markdown_core_attribute_scratch scratch = {0};
            markdown_core_attribute_parser parser = {.data = (const unsigned char *)containers[c],
                                                     .length = (bufsize_t)strlen(containers[c]),
                                                     .scratch = &scratch};
            markdown_core_attributes value = {0};
            bufsize_t end = -1;
            payload_probe_arm();
            payload_fail_at = fail_at;
            int read = markdown_core_attributes_parse(&parser, 0, &value, &end);
            bool refused = payload_allocations >= fail_at;
            payload_fail_at = 0;
            if (refused) {
                OK(runner, !read && parser.oom && end == -1 && !value.storage && !value.anchor.data,
                   "a refused allocation fails the read and publishes nothing: %s at %zu", containers[c], fail_at);
            } else {
                OK(runner, read && !parser.oom, "the read succeeds with no refusal: %s", containers[c]);
            }
            markdown_core_attributes_free(&value);
            markdown_core_attribute_parser_free(&parser);
            markdown_core_attribute_scratch_free(&scratch);
            INT_EQ(runner, (int)payload_live, 0, "a refused read leaks nothing: %s at %zu", containers[c], fail_at);
            payload_probe_disarm();
            if (!refused) {
                break;
            }
        }
    }
}

/* A VALUE IS ONE ALLOCATION. A container is read into its extent's scratch
 * and then laid out -- records, classes and every string they name -- in one
 * block at its exact size, so a value costs one allocation whatever its
 * members. The scratch grows with the largest container the extent reads and
 * is reused by the next one, so reading the second container of an extent
 * allocates the value and nothing else. The negative control is the arena this
 * replaced: two vectors and an arena, at least three allocations per value and
 * more as they doubled, plus a decode buffer made and freed per parse. */
static void attribute_values_are_one_allocation(test_batch_runner *runner) {
    for (size_t count = 8; count <= 8192; count *= 4) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        for (size_t container = 0; container < 2; container++) {
            markdown_core_strbuf_puts(&source, "{#anchor ");
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, "k=v .c ");
            }
            markdown_core_strbuf_puts(&source, "} ");
        }
        size_t growth = 0;
        for (size_t capacity = 8; capacity < 16 * count; capacity += capacity / 2) {
            growth++;
        }
        markdown_core_attribute_scratch scratch = {0};
        markdown_core_attribute_parser parser = {.data = source.ptr, .length = source.size, .scratch = &scratch};
        markdown_core_attributes first = {0}, second = {0};
        bufsize_t end = 0, again = 0;
        payload_probe_arm();
        int read = markdown_core_attributes_parse(&parser, 0, &first, &end);
        size_t warming = payload_allocations;
        int reread = markdown_core_attributes_parse(&parser, end + 1, &second, &again);
        size_t value_allocations = payload_allocations - warming;
        OK(runner, read && reread && second.record_count == count && second.class_count == count,
           "both containers parse");
        OK(runner, warming <= 4 + 2 * growth, "the extent's scratch grows geometrically: members=%zu allocations=%zu",
           count, warming);
        INT_EQ(runner, (int)value_allocations, 1, "a value read with warm scratch is one allocation: members=%zu",
               count);
        OK(runner,
           second.storage != NULL && !second.anchor.alloc && !second.classes[0].alloc &&
               !second.records[0].name.alloc && second.anchor.len == 6 && second.anchor.data[6] == 0,
           "the strings live in the value's block, NUL-terminated, and own nothing themselves");
        markdown_core_attributes_free(&first);
        markdown_core_attributes_free(&second);
        markdown_core_attribute_parser_free(&parser);
        markdown_core_attribute_scratch_free(&scratch);
        payload_probe_disarm();
        INT_EQ(runner, (int)payload_live, 0,
               "freeing the values, the parser and its scratch releases every allocation");
        markdown_core_strbuf_free(&source);
    }
}

/* ORDERING IS LINEAR IN THE ENTRIES AND PASSES ONLY OVER KEY BYTES THAT DIFFER.
 * The shapes below are the ones a pass-skipping implementation can get wrong:
 * keys that differ in the top byte only, keys that differ in every byte, and
 * ties, which a stable ordering must leave in arrival order. An ordered input
 * is the identity and moves nothing. */
typedef struct source_entry {
    markdown_core_node *node;
    int serial;
} source_entry;

static int source_entries_ordered(const source_entry *entries, size_t count) {
    for (size_t i = 1; i < count; i++) {
        uint64_t previous = markdown_core_source_key(&entries[i - 1]);
        uint64_t current = markdown_core_source_key(&entries[i]);
        if (previous > current || (previous == current && entries[i - 1].serial > entries[i].serial)) {
            return 0;
        }
    }
    return 1;
}

static void source_entries_order_by_the_key_bytes_that_differ(test_batch_runner *runner) {
    enum { COUNT = 1000 };
    static markdown_core_node nodes[COUNT];
    source_entry entries[COUNT];
    static const struct {
        const char *name;
        int allocations;
        size_t passes;
    } shapes[] = {
        {"already ordered", 1, 0},
        {"top byte only", 2, 1},
        {"every byte", 2, 4},
        {"ties", 2, 1},
    };
    for (size_t shape = 0; shape < sizeof(shapes) / sizeof(*shapes); shape++) {
        uint32_t state = 0x9E3779B9u;
        for (int i = 0; i < COUNT; i++) {
            state = state * 1664525u + 1013904223u;
            uint32_t key;
            switch (shape) {
            case 0:
                key = (uint32_t)i;
                break;
            case 1:
                /* The key's top byte is the offset's bits 24..31, and seven of
                 * those bits vary: the keys differ in the top byte alone, with
                 * ties. */
                key = (uint32_t)((COUNT - i) % 127 + 1) << 24;
                break;
            case 2:
                key = state;
                break;
            default:
                key = (uint32_t)((i * 31) % 5);
                break;
            }
            nodes[i].place.start = key;
            entries[i] = (source_entry){&nodes[i], i};
        }
        markdown_core_source_order workspace = {0};
        payload_probe_arm();
        int ok =
            markdown_core_order_source_entries(&workspace, entries, COUNT, sizeof(*entries), markdown_core_source_key);
        INT_EQ(runner, workspace.work, COUNT * (1 + 2 * shapes[shape].passes),
               "ordering charges the key scan and only the radix passes performed");
        size_t allocations = payload_allocations;
        OK(runner,
           markdown_core_order_source_entries(&workspace, entries, COUNT, sizeof(*entries), markdown_core_source_key),
           "a second ordering reuses scratch");
        INT_EQ(runner, payload_allocations, allocations, "ordering allocates only on capacity growth");
        INT_EQ(runner, workspace.work, COUNT * (2 + 2 * shapes[shape].passes),
               "already ordered replay adds one key scan");
        markdown_core_source_order_dispose(&workspace);
        size_t live = payload_live;
        payload_probe_disarm();
        OK(runner, ok && source_entries_ordered(entries, COUNT), "%s: entries are ordered by key, ties by arrival",
           shapes[shape].name);
        INT_EQ(runner, (int)allocations, shapes[shape].allocations,
               "%s: the keys are computed once and an ordered input moves nothing", shapes[shape].name);
        INT_EQ(runner, (int)live, 0, "%s: the scratch is released", shapes[shape].name);
    }
    markdown_core_source_order workspace = {0};
    OK(runner,
       markdown_core_order_source_entries(&workspace, entries, 0, sizeof(*entries), markdown_core_source_key) &&
           markdown_core_order_source_entries(&workspace, entries, 1, sizeof(*entries), markdown_core_source_key),
       "fewer than two entries are ordered as they are");
}

/* A DELIMITER ENTRY IS ALLOCATED ONCE PER LIVE SLOT, NOT ONCE PER PUSH. The
 * pushes grow with the inline containers a document has; the entries the
 * parser holds at the end -- every entry it ever allocated, since a container
 * returns all of its entries when it is cleared -- do not. */
static void delimiter_entries_are_pooled_across_inline_containers(test_batch_runner *runner) {
    static const char paragraph[] = "*a* **b** _c_ d *e f* g\n\n";
    size_t pushes_per_paragraph = 0, pooled_for_one = 0, nodes_for_one = 0;
    for (size_t count = 1; count <= 64; count *= 8) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, paragraph);
        }
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((const char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "the document parses");
        size_t emphases = 0;
        tree_iter *iter = tree_iter_new(root);
        markdown_core_event_type ev;
        while ((ev = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            markdown_core_node *node = tree_iter_get_node(iter);
            if (ev == MARKDOWN_CORE_EVENT_ENTER &&
                (node->kind == MARKDOWN_CORE_NODE_EMPHASIS || node->kind == MARKDOWN_CORE_NODE_STRONG)) {
                emphases++;
            }
        }
        tree_iter_free(iter);
        if (count == 1) {
            pushes_per_paragraph = work.delimiter_pushes;
            pooled_for_one = work.pooled_delimiters;
            nodes_for_one = emphases;
            OK(runner, pushes_per_paragraph > 0 && pooled_for_one > 0 && nodes_for_one == 4,
               "one paragraph pushes entries and leaves them pooled: pushes=%zu pooled=%zu", pushes_per_paragraph,
               pooled_for_one);
        } else {
            INT_EQ(runner, (int)work.delimiter_pushes, (int)(count * pushes_per_paragraph),
                   "pushes grow with the paragraphs: count=%zu", count);
            INT_EQ(runner, (int)work.pooled_delimiters, (int)pooled_for_one, "the entries allocated do not: count=%zu",
                   count);
            INT_EQ(runner, (int)emphases, (int)(count * nodes_for_one), "a reused entry parses as a fresh one");
        }
        markdown_core_node_free(root);
        markdown_core_strbuf_free(&source);
    }
}

/* Every node's release visits its attribute value, and most values are empty. */
static void releasing_an_empty_attribute_value_makes_no_allocator_call(test_batch_runner *runner) {
    static const char container[] = "{#a .b k=v}";
    markdown_core_attribute_scratch scratch = {0};
    markdown_core_attribute_parser parser = {
        .data = (const unsigned char *)container, .length = sizeof(container) - 1, .scratch = &scratch};
    markdown_core_attributes value = {0};
    markdown_core_attributes empty = {0};
    bufsize_t end = 0;
    payload_probe_arm();
    OK(runner, markdown_core_attributes_parse(&parser, 0, &value, &end), "the container parses");
    markdown_core_attribute_parser_free(&parser);
    markdown_core_attribute_scratch_free(&scratch);
    size_t releases = payload_releases;
    markdown_core_attributes_free(&empty);
    INT_EQ(runner, (int)payload_releases, (int)releases, "an empty value is released without entering the allocator");
    markdown_core_attributes_free(&value);
    INT_EQ(runner, (int)payload_live, 0, "a value that owns strings releases them all");
    OK(runner, payload_releases > releases, "and does so through the allocator");
    payload_probe_disarm();
}

/* PLACING AN INLINE NODE PROBES THE SOURCE MAP A BOUNDED NUMBER OF TIMES,
 * however many lines its container has. The inline parser reads left to
 * right, so each placement is answered from the run the last one ended in or
 * the one after; a search of the container's whole run would cost log2 of
 * its lines per query, and the shape below has thousands of them. The
 * container is one paragraph whose every line carries several tokens, so the
 * probes per query are read where the search would be dearest. Placements
 * behind the cursor -- a link closed after its opener, a rewind -- are
 * answered by a search of the part behind it, so the bound is on the
 * average, not on each query, and the links here exercise that path too, as
 * a directive does, placed from its colon and then back at its label. */
static void inline_placement_probes_the_map_a_bounded_number_of_times(test_batch_runner *runner) {
    static const char line[] = "a *b* c [d](/e) f :g[h]{.i}\n";
    for (size_t lines = 64; lines <= 4096; lines *= 8) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        for (size_t i = 0; i < lines; i++) {
            markdown_core_strbuf_puts(&source, line);
        }
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((const char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "the paragraph parses");
        OK(runner, work.content_mark_queries >= 12 * lines, "every token asks the map: lines=%zu queries=%zu", lines,
           work.content_mark_queries);
        OK(runner, work.content_mark_probes <= 2 * work.content_mark_queries + 64,
           "a placement is answered from the cursor, not by a search of the container: lines=%zu queries=%zu "
           "probes=%zu",
           lines, work.content_mark_queries, work.content_mark_probes);
        markdown_core_node_free(root);
        markdown_core_strbuf_free(&source);
    }
}

/* CONSOLIDATION APPENDS NO RUN WHEN ITS OPERANDS ARE VIEWS. A Text that is a
 * verbatim copy of its source holds a slice of its container's runs, and the
 * siblings consolidation absorbs were placed left to right in that container,
 * so the merged Text is the slice from the first operand's first run to the
 * last operand's last: a paragraph whose literal asterisks and brackets split
 * it into several Texts ends the parse with exactly the runs its lines gave
 * the map, as a paragraph of the same lines with one Text does. An operand
 * that is not a view -- a decoded entity, whose literal is shorter than its
 * scope -- breaks the chain, and the merged map is materialized run by run.
 * The positions are the same either way; the api test's own dumps and the
 * corpus hold that, and this holds the mechanism. */
static void consolidation_keeps_a_view_when_its_operands_are_views(test_batch_runner *runner) {
    static const char split[] = "a_b_c[d]e\nf_g\n";
    static const char whole[] = "aaaaaaaaa\nfff\n";
    static const char decoded[] = "a&amp;b_c\nfff\n";
    inline_work split_work = {0}, whole_work = {0}, decoded_work = {0};
    markdown_core_node *split_root =
        markdown_core_parse_document_with_setup(split, sizeof(split) - 1, measure_inline_work, &split_work);
    markdown_core_node *whole_root =
        markdown_core_parse_document_with_setup(whole, sizeof(whole) - 1, measure_inline_work, &whole_work);
    markdown_core_node *decoded_root =
        markdown_core_parse_document_with_setup(decoded, sizeof(decoded) - 1, measure_inline_work, &decoded_work);
    OK(runner, split_root && whole_root && decoded_root, "the three paragraphs parse");
    INT_EQ(runner, count_kind(split_root, MARKDOWN_CORE_NODE_TEXT), 2,
           "the literal asterisks and brackets were merged into one Text per line");
    INT_EQ(runner, split_work.line_marks, whole_work.line_marks,
           "and merging views appended no run: %zu runs, as the one-Text paragraph's %zu", split_work.line_marks,
           whole_work.line_marks);
    OK(runner, decoded_work.line_marks > whole_work.line_marks,
       "a decoded operand materializes the merged map: %zu runs against %zu", decoded_work.line_marks,
       whole_work.line_marks);
    markdown_core_node_free(split_root);
    markdown_core_node_free(whole_root);
    markdown_core_node_free(decoded_root);
}

/* THE CURSOR'S FAST PATH AGREES WITH THE SPAN ON EVERY PLACEMENT. A node is
 * placed from the run the cursor names when both of its ends lie on that run;
 * otherwise the span resolves each end on its own. The two must give the same
 * positions for every pair of offsets a container has -- including a reversed
 * pair, which is how an empty field is placed ([x, x - 1]) and which puts the
 * two ends on two runs when x is a line's first byte -- from every run the
 * cursor can be on. The probe runs when the container's inlines are finished,
 * with the map complete and the state still alive. */
static int placement_disagreements, placements_checked;
static void probe_placements(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    (void)self;
    markdown_core_parser *parser = inline_state->owner_parser;
    markdown_core_node *owner = inline_state->owner;
    if (!owner || owner->kind != MARKDOWN_CORE_NODE_PARAGRAPH) {
        return;
    }
    int first = owner->content_map.first, last = first + owner->content_map.count - 1;
    for (int cursor = first; cursor <= last; cursor++) {
        for (int from = 0; from < inline_state->input.len; from++) {
            for (int to = -1; to < inline_state->input.len; to++) {
                markdown_core_node placed = {.kind = MARKDOWN_CORE_NODE_SOFT_BREAK};
                markdown_core_content_span span = {0};
                inline_state->mark_cursor = cursor;
                markdown_core_inline_seat_cursor(inline_state);
                markdown_core_inline_state_place(inline_state, &placed, from, to);
                markdown_core_parser_content_span(parser, &owner->content_map, from, to, &span, NULL);
                placements_checked++;
                if (placed.place.start != (uint32_t)span.start || (to >= 0 && placed.place.end != (uint32_t)span.end)) {
                    placement_disagreements++;
                }
            }
        }
    }
}

static void placement_from_the_cursor_agrees_with_the_span(test_batch_runner *runner) {
    static const markdown_core_inline_ops probe_inlines = {
        .finish = probe_placements,
    };
    static const markdown_core_element probe = {
        .name = "probe-placements",
        .inlines = &probe_inlines,
    };
    const markdown_core_element *elements[] = {&probe};
    static const char source[] = "ab\ncd\n> ef\n> gh\n\n- ij\n  kl\n";
    placement_disagreements = placements_checked = 0;
    markdown_core_node *root = parse_with_probes(source, sizeof(source) - 1, elements, 1);
    OK(runner, root != NULL, "the probe parses");
    OK(runner, placements_checked > 0, "the probe saw the paragraphs: checked=%d", placements_checked);
    INT_EQ(runner, placement_disagreements, 0,
           "every placement from every cursor, reversed pairs across runs included, matches the span");
    markdown_core_node_free(root);
}

/* A LINE WHOSE FIRST BYTE NO SCAN OWNER DECLARED REACHES NONE OF THEM. The
 * projection reads the line's key and asks only the owners listed under it;
 * `!` leads nothing, so the list and specimen scanners, whose work counters
 * count every byte they look at and which nothing else drives on this
 * document, never see the line. */
static void an_undeclared_first_byte_reaches_no_gated_scanner(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    for (int i = 0; i < 512; i++) {
        markdown_core_strbuf_puts(&source, "! prose that no block grammar starts on\n\n");
    }
    inline_work work = {0};
    markdown_core_node *root =
        markdown_core_parse_document_with_setup((const char *)source.ptr, source.size, measure_inline_work, &work);
    OK(runner, root != NULL, "the document parses");
    INT_EQ(runner, (int)work.list_markers, 0, "the list scanner is not asked about a line it cannot start");
    INT_EQ(runner, (int)work.specimens, 0, "nor the specimen scanner");
    markdown_core_node_free(root);
    markdown_core_strbuf_free(&source);
}

/* THE LIST SCANNER READS A NUMERAL BY THE CLASS ITS FIRST BYTE NAMES. The
 * scan gate must admit letters and digits -- fancy lists start on them -- so
 * every prose line that begins with a word reaches the numeral scan, and a
 * scan that read the whole alphanumeric run paid the word's length on every
 * such line. The grammar accepts one letter, roman letters of one case, or
 * decimal digits (at most nine), so the scan reads that class and stops at
 * its boundary: a prose word ends the probe on its second byte, a long digit
 * string on its eleventh, and every marker the grammar accepted or refused is
 * accepted or refused as before. */
static void the_list_scanner_reads_a_numeral_by_its_first_bytes_class(test_batch_runner *runner) {
    static const size_t lines = 256;
    static const struct {
        const char *lead;
        size_t ceiling;
    } shapes[] = {{"Supercalifragilisticexpialidocious", 4}, {"mississippi", 4}, {"1234567890123456789012345678", 14}};
    for (size_t s = 0; s < sizeof(shapes) / sizeof(*shapes); s++) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        for (size_t i = 0; i < lines; i++) {
            markdown_core_strbuf_puts(&source, shapes[s].lead);
            markdown_core_strbuf_puts(&source, " is not a list marker\n\n");
        }
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((const char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "shape %zu parses", s);
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_LIST), 0, "and opens no list: shape %zu", s);
        OK(runner, work.list_markers <= shapes[s].ceiling * lines,
           "the scan reads the first byte's class, not the word: %zu units for %zu lines of shape %zu",
           work.list_markers, lines, s);
        markdown_core_node_free(root);
        markdown_core_strbuf_free(&source);
    }

    /* The accepted set, and the refused set, are the grammar's. */
    static const struct {
        const char *source;
        int lists;
    } markers[] = {{"a. x\n", 1},          {"iv. x\n", 1},          {"MMXX. x\n", 1}, {"(c) x\n", 1},
                   {"123456789. x\n", 1},  {"i. x\nii. x\n", 1},    {"mix. x\n", 1},  {"#. x\n", 1},
                   {"x) y\n", 1},          {"3rd. x\n", 0},         {"ab. x\n", 0},   {"Iv. x\n", 0},
                   {"1234567890. x\n", 0}, {"12345678901. x\n", 0}, {"H. x\n", 0},    {"cat. x\n", 0}};
    for (size_t m = 0; m < sizeof(markers) / sizeof(*markers); m++) {
        markdown_core_node *root = markdown_core_parse_document(markers[m].source, strlen(markers[m].source));
        OK(runner, root != NULL, "marker %zu parses", m);
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_LIST), markers[m].lists,
               "the class scan keeps the grammar's answer for marker %zu", m);
        markdown_core_node_free(root);
    }
}

static void attribute_attachment_linear_work(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *unit, *suffix;
    } cases[] = {
        {"", "`x`{k=' ", "?}"},
        {"", "[x](/u){k=' ", "?}"},
        {"# ", "{#a ", "?}"},
        {"~~~lang ", "{#a ", "?}\nbody\n~~~"},
        {"", "[r]: /u {.c k=1}\n", "\n[r][]"},
        {"", "[r]: /u {k=' ", "?}\n"},
        {"", "`x`{.a .a k=1 k=2} ", ""},
        {"", "[r][]{.a k=2} ", "\n\n[r]: /u {#a .b k=1}\n"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 4096; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_setup((const char *)source.ptr, source.size,
                                                                               measure_inline_work, &work);
            OK(runner, root != NULL, "every attribute site parses adversarial inputs");
            OK(runner, work.attributes > 0 && work.attributes <= 20 * (size_t)source.size,
               "attribute attachment is linear: case=%zu bytes=%d work=%zu", c, source.size, work.attributes);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void heading_completion_invariants(test_batch_runner *runner) {
    static const struct {
        const char *source, *anchor;
    } cases[] = {
        {"# x\n\n# T {#x}\n", "x-1"},
        {"# x\n\n[r]: /u {#x}\n", "x-1"},
        {"# x\n\n[r][]{#y}\n\n[r]: /u {#x}\n", "x-1"},
        {"# x\n\n[^`a`{#x}]\n\n[^`a`{#x}]: note\n", "x"},
        {"# x\n\n:d[`a`{#x}]\n", "x-1"},
        {"# x\n\n:d[^[`a`{#x}]]\n", "x-1"},
        {"# x\n\n> [!note] `a`{#x}\n", "x-1"},
        {"# x\n\n[^n]: `a`{#x}\n", "x-1"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        markdown_core_node *root = parse(cases[i].source);
        OK(runner, root != NULL, "completed inline ownership preserves anchor reservations");
        if (root) {
            STR_EQ(runner, (char *)child_at(root, 0)->attributes.anchor.data, cases[i].anchor,
                   "only final effective anchors on emitted nodes reserve names");
        }
        markdown_core_node_free(root);
    }
    size_t anchor_work = 0;
    for (size_t count = 128; count <= 4096; count *= 2) {
        for (int referenced = 0; referenced < 2; referenced++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, "# Same\n\n");
            }
            if (referenced) {
                markdown_core_strbuf_puts(&source, "[Same] ![Same] [Same][]\n");
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "repeated heading declarations parse");
            INT_EQ(runner, work.definitions, count, "each heading creates its own implicit reference definition");
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "# Only\n\n");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, "Plain **paragraph**.\n\n");
        }
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "unrelated content does not add anchor work");
        if (!anchor_work) {
            anchor_work = work.anchors;
        }
        INT_EQ(runner, work.anchors, anchor_work, "anchor work is independent of unrelated AST size");
        markdown_core_node_free(root);
        markdown_core_strbuf_free(&source);
    }
}

static void heading_roster_invariants(test_batch_runner *runner) {
    for (size_t count = 128; count <= 8192; count *= 2) {
        for (int reserve = 0; reserve < 2; reserve++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, "# Same\n\n");
            }
            if (reserve) {
                markdown_core_strbuf_puts(&source, ":n{#same}\n\n");
                for (size_t i = 1; i <= count; i++) {
                    char declaration[80];
                    snprintf(declaration, sizeof(declaration), ":n{#same-%zu}\n\n", i);
                    markdown_core_strbuf_puts(&source, declaration);
                }
            }
            markdown_core_strbuf_puts(&source, "[Same] [Same][] [go][Same]\n");
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "heading roster handles dense future reservations");
            if (root) {
                markdown_core_node *node = child_at(root, 0);
                OK(runner, work.heading_collection_disposed, "the finished document holds no live heading parse state");
                for (size_t i = 0; i < count; i++, node = child_at(root, i)) {
                    char expected[80];
                    size_t ordinal = reserve ? count + i + 1 : i;
                    if (ordinal) {
                        snprintf(expected, sizeof(expected), "same-%zu", ordinal);
                    } else {
                        strcpy(expected, "same");
                    }
                    STR_EQ(runner, (const char *)node->attributes.anchor.data, expected,
                           "every generated suffix is the smallest available");
                }
                OK(runner, work.anchors > count && work.anchors < 25 * (size_t)source.size,
                   "roster work is linear in input and generated spelling bytes: %zu/%d", work.anchors, source.size);
                bool named = true;
                for (size_t node_index = 0; node_index < last_child_of(root)->child_count; node_index++) {
                    markdown_core_node *node = last_child_of(root)->children[node_index];
                    if (node->kind == MARKDOWN_CORE_NODE_LINK) {
                        markdown_core_link *link = node->as.link;
                        named =
                            named && !link->resource && link->label.len == 4 && !memcmp(link->label.data, "same", 4);
                    }
                }
                OK(runner, named, "all reference spellings name the heading's label");
                markdown_core_node_free(root);
            }
            markdown_core_strbuf_free(&source);
        }
    }
    /* The cursor may have live delimiters, backtick lookahead, and attribute
     * recognition state at the declaration boundary. It resumes exactly once. */
    for (size_t count = 128; count <= 4096; count *= 2) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, "# *`prefix` [Later]* {#same}\n\n");
        }
        markdown_core_strbuf_puts(&source, "# Later\n");
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "pending heading cursors retain their delimiter ownership");
        if (root) {
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_LINK), count, "every forward call resolves once");
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_EMPHASIS), count,
                   "every live delimiter pair resolves once");
            OK(runner, work.anchors + work.attributes + work.footnote_body < 40 * (size_t)source.size,
               "declaration parsing and finalization stay linear with pending cursors");
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

/* AN OCCURRENCE NAMES ITS TARGET BY LABEL: however long the heading's
 * anchor or a Reference's attributes, each occurrence holds the label alone,
 * and the attributes are the target's, never copied into it. */
static void reference_occurrences_hold_the_label_alone(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    const size_t count = 4096, length = 65536;
    for (int definition = 0; definition < 2; definition++) {
        markdown_core_strbuf_clear(&source);
        markdown_core_strbuf_puts(&source, definition ? "# x\n\n[Target]: /u {id=" : "# Target {id=");
        for (size_t i = 0; i < length; i++) {
            markdown_core_strbuf_putc(&source, 'a');
        }
        markdown_core_strbuf_puts(&source, " .c k=1}\n\n");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, "[Target] ");
        }
        markdown_core_node *root = parse((char *)source.ptr);
        OK(runner, root != NULL, "a long target parses");
        if (!root) {
            continue;
        }
        size_t references = 0;
        bool alone = true;
        for (size_t node_index = 0; node_index < last_child_of(root)->child_count; node_index++) {
            markdown_core_node *node = last_child_of(root)->children[node_index];
            if (node->kind != MARKDOWN_CORE_NODE_LINK) {
                continue;
            }
            alone = alone && !node->as.link->resource && node->as.link->label.len == 6 &&
                    !memcmp(node->as.link->label.data, "target", 6) && !node->attributes.anchor.len &&
                    !node->attributes.class_count && !node->attributes.record_count;
            references++;
        }
        INT_EQ(runner, references, count, "every occurrence resolves");
        OK(runner, alone, "every occurrence holds the label and none of the target's attributes");
        markdown_core_node_free(root);
    }
    markdown_core_strbuf_free(&source);
}

static void heading_label_length_boundary(test_batch_runner *runner) {
    for (size_t length = MAX_LINK_LABEL_LENGTH; length <= MAX_LINK_LABEL_LENGTH + 1; length++) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "# ");
        for (size_t i = 0; i < length; i++) {
            markdown_core_strbuf_putc(&source, 'a');
        }
        markdown_core_strbuf_puts(&source, "\n\n[");
        for (size_t i = 0; i < length; i++) {
            markdown_core_strbuf_putc(&source, 'a');
        }
        markdown_core_strbuf_puts(&source, "]\n");
        markdown_core_node *root = parse((char *)source.ptr);
        OK(runner, root != NULL, "heading labels respect the inherited raw-label length boundary");
        if (root) {
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_LINK), length == MAX_LINK_LABEL_LENGTH,
                   "declaration and occurrence use the same raw-label limit");
            INT_EQ(runner, anchor_of(child_at(root, 0)).value.length, length,
                   "label eligibility never limits generated heading anchors");
        }
        markdown_core_node_free(root);
        markdown_core_strbuf_free(&source);
    }
}

static size_t count_anchors(markdown_core_node *root) {
    size_t count = 0;
    tree_iter *iter = tree_iter_new(root);
    markdown_core_event_type event;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (event == MARKDOWN_CORE_EVENT_ENTER && tree_iter_get_node(iter)->attributes.anchor.len) {
            count++;
        }
    }
    tree_iter_free(iter);
    return count;
}

/* Long digit/pipe runs and nested successful images exercise the same bound:
 * no image closer rescans a nested label, including labels without pipes. */
static void image_dimension_linear_work(test_batch_runner *runner) {
    for (size_t count = 128; count <= 8192; count *= 2) {
        for (int shape = 0; shape < 6; shape++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            if (shape < 2) {
                markdown_core_strbuf_puts(&source, "![alt|");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, shape == 0 ? '9' : '|');
                }
                markdown_core_strbuf_puts(&source, "10](/i)\n");
            } else {
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_puts(&source, shape == 4 ? "![" : "![a|");
                }
                markdown_core_strbuf_puts(&source, "1");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_puts(&source, shape == 3 ? "](/i)|2" : "](/i)");
                }
                markdown_core_strbuf_putc(&source, '\n');
            }
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_setup(
                (const char *)source.ptr, (size_t)source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial image labels parse");
            OK(runner, work.dimensions > 0 && work.dimensions <= 3 * (size_t)source.size,
               "image dimension work is linear: shape=%d bytes=%d work=%zu", shape, source.size, work.dimensions);
            if (root) {
                size_t images = 0, sized = 0;
                tree_iter *iter = tree_iter_new(child_at(root, 0));
                for (markdown_core_event_type event; (event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE;) {
                    markdown_core_node *node = tree_iter_get_node(iter);
                    if (event == MARKDOWN_CORE_EVENT_ENTER && node->kind == MARKDOWN_CORE_NODE_EMBEDDED) {
                        images++;
                        sized += node->as.link->dimensions.has_value;
                    }
                }
                tree_iter_free(iter);
                INT_EQ(runner, images, shape < 2 ? 1 : count, "nested image depth is preserved");
                INT_EQ(runner, sized,
                       shape == 0   ? 0
                       : shape == 3 ? count
                                    : 1,
                       "only each image's own complete suffix sets dimensions");
            }
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* Each newly opened quote inspects one bounded prefix or its own type/title
 * separators. Deep quote chains and long failed type candidates must never
 * rescan the remainder of the document. */
static void callout_linear_work(test_batch_runner *runner) {
    for (size_t count = 128; count <= 8192; count *= 2) {
        for (int shape = 0; shape < 4; shape++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            if (shape == 0) {
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, '>');
                }
                markdown_core_strbuf_puts(&source, " [!deep]- ==title==\n");
            } else if (shape == 1) {
                markdown_core_strbuf_puts(&source, "> [!");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, 'a');
                }
                markdown_core_strbuf_puts(&source, ".] invalid\n");
            } else if (shape == 2) {
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_puts(&source, "> [!note]+ T\n\n");
                }
            } else {
                markdown_core_strbuf_puts(&source, "> [!note]-");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, ' ');
                }
                markdown_core_strbuf_puts(&source, "\n");
            }
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_setup(
                (const char *)source.ptr, (size_t)source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial callouts parse");
            OK(runner, work.callout > 0 && work.callout <= 2 * (size_t)source.size,
               "callout recognition is linear: shape=%d bytes=%d work=%zu", shape, source.size, work.callout);
            if (root && shape == 0) {
                markdown_core_node *node = child_at(root, 0);
                size_t depth = 1;
                while (child_at(node, 0)) {
                    node = child_at(node, 0);
                    depth++;
                }
                INT_EQ(runner, depth, count, "no quote depth truncation");
                STR_EQ(runner, (const char *)node->as.callout->variant.value.data, "deep", "deepest metadata retained");
                OK(runner,
                   node->as.callout->title && child_at(node->as.callout->title, 0)->kind == MARKDOWN_CORE_NODE_MARK,
                   "deep title parsed through shared inlines");
            }
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void block_identifier_linear_work(test_batch_runner *runner) {
    static const struct {
        const char *prefix, *unit, *suffix;
        size_t anchors_per_unit, anchors_at_end;
    } cases[] = {
        {"text #", "a", "#", 0, 1},
        {"text #", "a", "_#", 0, 0},
        {"text ", "#", "", 0, 0},
        {"text ", "#a# ", "#last#", 0, 1},
        {"text ", "#a# ", "invalid", 0, 0},
        {"", "text #id#\n\n", "", 1, 0},
        {"text\n", "#id#\n", "", 0, 1},
        {"text\n", "    #id#\n", "", 0, 0},
        {"", "> quote\n\n#id#\n\n", "", 1, 0},
        {"", "- item\n\n#id#\nnext\n\n", "", 0, 0},
        {"- item\n\n#id#\n", "\n", "next", 0, 1},
        {"", "lead #id#\n| h |\n| - |\n\n", "", 1, 0},
        {"", "- item\n\n[ref]: /x\n\n#id#\n\n", "", 0, 0},
        {"", "> - item\n>\n> [ref]: /x\n>\n> #id#\n>\n", "", 0, 0},
        {"- item\n\n[ref]: /x\n", "\n", "#id#", 0, 0},
        {"- item\n\n", "[ref]: /x\n\n", "#id#", 0, 0},
        {"- item\n\n", "\n", "#id#", 0, 1},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_setup((const char *)source.ptr, source.size,
                                                                               measure_inline_work, &work);
            OK(runner, root != NULL, "block identifier adversary parses: case=%zu count=%zu", c, count);
            INT_EQ(runner, count_anchors(root), count * cases[c].anchors_per_unit + cases[c].anchors_at_end,
                   "every placement and fallback retains its semantics: case=%zu count=%zu", c, count);
            OK(runner, work.block_identifier + work.lookahead <= 4 * (size_t)source.size,
               "identifier and boundary work is linear: case=%zu bytes=%d scanner=%zu lookahead=%zu", c, source.size,
               work.block_identifier, work.lookahead);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static markdown_core_node *seed_anchor(const markdown_core_element_instance *self, int indented,
                                       markdown_core_parser *parser, markdown_core_node *parent, unsigned char *input,
                                       int length) {
    (void)self;
    (void)indented;
    (void)parent;
    (void)input;
    (void)length;
    markdown_core_node *owner = markdown_core_parser_current(parser);
    if (parser->at.blank && owner->kind == MARKDOWN_CORE_NODE_PARAGRAPH) {
        markdown_core_node *holder = markdown_core_parser_parent(parser, owner);
        if (holder->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
            owner = holder;
        }
        if (!markdown_core_chunk_set_cstr(&owner->attributes.anchor, "existing")) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    return NULL;
}

static markdown_core_node *observe_definition_before_anchor(const markdown_core_element_instance *self, int indented,
                                                            markdown_core_parser *parser, markdown_core_node *parent,
                                                            unsigned char *input, int length) {
    (void)self;
    (void)indented;
    if (length - parser->at.first_nonspace >= 6 && memcmp(input + parser->at.first_nonspace, "#list#", 6) == 0) {
        markdown_core_node *previous = markdown_core_parser_last_child(parent);
        *(bool *)parser->context = previous && previous->kind == MARKDOWN_CORE_NODE_REFERENCE;
    }
    return NULL;
}

static bool observe_reference_definition_lifetime(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    static const markdown_core_open_ops observer_opens = {
        .open = observe_definition_before_anchor,
    };
    static const markdown_core_element observer = {
        .name = "reference-definition-lifetime",
        .opens = &observer_opens,
    };
    return markdown_core_dialect_builder_attach(builder, &observer);
}

/* `label`'s normal form into `normal`; false when it has none. */
static bool label_normal_form(markdown_core_strbuf *normal, const markdown_core_chunk *label) {
    markdown_core_strbuf_clear(normal);
    markdown_core_utf8proc_normalize_label(normal, label->data, label->len);
    return normal->size && !normal->oom;
}

/* THE REFERENCE-LABEL NORMAL FORM is one pass (utf8.c) that folds, trims and
 * collapses whitespace runs; these are the cases each former pass owned, and
 * the one the single reservation rests on: U+0390 folds two bytes to six, so a
 * label of nothing else is exactly three times its length. */
static void reference_label_normal_form(test_batch_runner *runner) {
    static const struct {
        const char *label, *normal;
    } cases[] = {
        {"  Foo\t\tBAR \n baz  ", "foo bar baz"},
        {"\r\nA\r\n", "a"},
        {"Stra\xC3\x9F"
         "e \xE1\xBA\x9E",
         "strasse ss"},
        {"\xE2\x84\xAA", "k"},
        {"\xCE\x90\xCE\x90", "\xCE\xB9\xCC\x88\xCC\x81\xCE\xB9\xCC\x88\xCC\x81"},
        {"A\x80 B\xBF", "a\x80 b\xBF"},
        {"C \xE4\xB8", "c \xE4\xB8"},
        {"\x0B\x0C", "\x0B\x0C"},
    };
    markdown_core_strbuf normal = MARKDOWN_CORE_BUF_INIT();
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        markdown_core_chunk label = markdown_core_chunk_literal(cases[i].label);
        OK(runner, label_normal_form(&normal, &label), "label %zu has a normal form", i);
        STR_EQ(runner, (const char *)normal.ptr, cases[i].normal, "label %zu normalizes in one pass", i);
    }
    markdown_core_chunk blank = markdown_core_chunk_literal(" \t\n ");
    OK(runner, !label_normal_form(&normal, &blank), "a whitespace-only label has no normal form");
    INT_EQ(runner, normal.size, 0, "and leaves the buffer empty");

    markdown_core_strbuf widest = MARKDOWN_CORE_BUF_INIT();
    for (size_t i = 0; i < 999; i++) {
        markdown_core_strbuf_puts(&widest, "\xCE\x90");
    }
    markdown_core_chunk label = {widest.ptr, widest.size, 0};
    OK(runner, label_normal_form(&normal, &label), "the widest fold has a normal form");
    INT_EQ(runner, normal.size, 3 * widest.size, "and it is exactly three times the label");
    OK(runner,
       normal.ptr[normal.size] == 0 && memcmp(normal.ptr, "\xCE\xB9\xCC\x88\xCC\x81", 6) == 0 &&
           memcmp(normal.ptr + normal.size - 6, "\xCE\xB9\xCC\x88\xCC\x81", 6) == 0,
       "every character folded, terminated");
    markdown_core_strbuf_free(&widest);
    markdown_core_strbuf_free(&normal);
}

/* A PROJECTION'S CONTENT STOPS AT THE BUFFER LIMIT. The label and anchor
 * projections write through a cursor bounded by the allocation, and when
 * growth oversized a buffer by half past MARKDOWN_CORE_STRBUF_LIMIT, an image
 * that appending would have refused landed past the limit, unpoisoned: a
 * 400 MiB label of U+0390, which folds from two bytes to six, reached that
 * state. Growth now stops at the limit and its terminator, so the room every
 * writer sees -- the cursor and the inline append alike -- ends at the limit.
 * The content below the limit is forged -- only the page the projections
 * write is touched -- because building such a label costs gigabytes. */
static void image_cursor_stops_at_buffer_limit(test_batch_runner *runner) {
    const bufsize_t limit = MARKDOWN_CORE_STRBUF_LIMIT;
    static const char fold[] = "\xCE\xB9\xCC\x88\xCC\x81";
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();
    markdown_core_strbuf_grow(&buf, limit / 3 * 2 + 64);
    OK(runner, !buf.oom && buf.asize - 1 == limit, "growth near the limit stops at the limit and its terminator");
    if (buf.oom) {
        markdown_core_strbuf_free(&buf);
        return;
    }

    buf.size = limit - 6;
    buf.ptr[buf.size] = '\0';
    markdown_core_utf8proc_normalize_label(&buf, (const uint8_t *)"\xCE\x90", 2);
    OK(runner, !buf.oom && buf.size == limit, "a fold that ends exactly at the limit lands");
    OK(runner, memcmp(buf.ptr + limit - 6, fold, 6) == 0 && buf.ptr[limit] == 0, "folded and terminated");

    buf.size = limit - 5;
    buf.ptr[buf.size] = '\0';
    markdown_core_utf8proc_normalize_label(&buf, (const uint8_t *)"\xCE\x90", 2);
    OK(runner, buf.oom, "a fold one byte past the limit poisons the buffer");
    OK(runner, buf.size == limit - 5 && buf.ptr[buf.size] == 0, "and leaves its content as it was");

    buf.oom = 0;
    buf.size = limit - 1;
    buf.ptr[buf.size] = '\0';
    markdown_core_utf8proc_anchor(&buf, (const uint8_t *)"AB", 2);
    OK(runner, buf.oom, "an anchor one byte past the limit poisons the buffer");
    OK(runner, buf.size == limit && buf.ptr[limit - 1] == 'a' && buf.ptr[limit] == 0,
       "at the byte where appending it would have been refused");

    /* The inline append reads the same room: an append that fits it fits the
     * limit, one that does not is refused, and a poisoned buffer takes none,
     * whatever room it has. */
    buf.oom = 0;
    buf.size = limit - 1;
    buf.ptr[buf.size] = '\0';
    markdown_core_strbuf_putc(&buf, 'x');
    OK(runner, !buf.oom && buf.size == limit && buf.ptr[limit - 1] == 'x' && buf.ptr[limit] == 0,
       "a byte appended at the last place the limit allows lands");
    markdown_core_strbuf_putc(&buf, 'y');
    OK(runner, buf.oom && buf.size == limit && buf.ptr[limit] == 0, "a byte past the limit poisons the buffer");
    buf.oom = 0;
    buf.size = limit - 2;
    buf.ptr[buf.size] = '\0';
    markdown_core_strbuf_put(&buf, (const unsigned char *)"abc", 3);
    OK(runner, buf.oom && buf.size == limit - 2 && buf.ptr[buf.size] == 0,
       "a run past the limit poisons the buffer and leaves its content");
    buf.size = 10;
    buf.ptr[buf.size] = '\0';
    markdown_core_strbuf_putc(&buf, 'z');
    markdown_core_strbuf_put(&buf, (const unsigned char *)"zz", 2);
    OK(runner, buf.oom && buf.size == 10 && buf.ptr[10] == 0, "a poisoned buffer takes no append, whatever its room");
    markdown_core_strbuf_free(&buf);
}

static void reference_definition_lifetime(test_batch_runner *runner) {
    const char source[] = "- a\n\n[ref]: /x\n\n#list#\n\n[ref]\n";
    bool retained_at_anchor = false;
    markdown_core_node *root = markdown_core_parse_document_with_setup(
        source, sizeof(source) - 1, observe_reference_definition_lifetime, &retained_at_anchor);
    OK(runner, root != NULL, "deferred reference definition parses");
    OK(runner, retained_at_anchor, "the finalized definition remains a sibling when anchor syntax is reached");
    if (root) {
        markdown_core_node *reference = child_at(root, 0) ? child_at(root, 1) : NULL;
        OK(runner, reference && reference->kind == MARKDOWN_CORE_NODE_REFERENCE,
           "the definition is a Reference where it was written");
        markdown_core_node *marker = child_at(root, 2);
        OK(runner,
           marker && marker->kind == MARKDOWN_CORE_NODE_PARAGRAPH && child_at(marker, 0) &&
               child_at(marker, 0)->kind == MARKDOWN_CORE_NODE_TEXT && literal_is(child_at(marker, 0), "#list#"),
           "the marker's paragraph follows the Reference");
        OK(runner,
           child_at(root, 3) && child_at(child_at(root, 3), 0) &&
               child_at(child_at(root, 3), 0)->kind == MARKDOWN_CORE_NODE_LINK && root->child_count == 4,
           "the definition resolves links without leaking a paragraph into the completed tree");
        markdown_core_node_free(root);
    }
}

static void block_identifier_ownership(test_batch_runner *runner) {
    static const markdown_core_open_ops seed_opens = {
        .open = seed_anchor,
    };
    static const markdown_core_element seed = {
        .name = "preexisting-anchor",
        .opens = &seed_opens,
    };
    const markdown_core_element *probes[] = {&seed};
    const char *sources[] = {"text #candidate#\n\n", "- text #candidate#\n\n"};
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); i++) {
        markdown_core_node *root = parse_with_probes(sources[i], strlen(sources[i]), probes, 1);
        markdown_core_node *owner = child_at(root, 0);
        if (owner->kind == MARKDOWN_CORE_NODE_LIST) {
            owner = child_at(owner, 0);
        }
        STR_EQ(runner, (const char *)owner->attributes.anchor.data, "existing", "an existing anchor is never replaced");
        markdown_core_node *paragraph = owner->kind == MARKDOWN_CORE_NODE_PARAGRAPH ? owner : child_at(owner, 0);
        LITERAL_EQ(runner, child_at(paragraph, 0), "text #candidate#",
                   "a refused attachment keeps the complete marker visible");
        INT_EQ(runner, count_anchors(root), 1, "a refused item anchor is not transferred to its paragraph");
        markdown_core_node_free(root);
    }
    const char *endings[] = {"\n", "\r\n", "\r"};
    for (size_t i = 0; i < sizeof(endings) / sizeof(*endings); i++) {
        for (int final_newline = 0; final_newline <= 1; final_newline++) {
            char source[80];
            snprintf(source, sizeof(source), "- text #item#%s%s#list#%s", endings[i], endings[i],
                     final_newline ? endings[i] : "");
            markdown_core_node *root = markdown_core_parse_document(source, strlen(source));
            markdown_core_node *list = child_at(root, 0);
            markdown_core_node *item = child_at(list, 0);
            INT_EQ(runner, END_LINE(list), 3, "detached line belongs to the list scope for every line ending");
            INT_EQ(runner, END_COLUMN(list), 6, "list scope includes the closing hash at EOF");
            INT_EQ(runner, END_COLUMN(child_at(child_at(item, 0), 0)), 6, "text scope ends before the separator");
            memset(source, 'x', strlen(source));
            STR_EQ(runner, (const char *)list->attributes.anchor.data, "list",
                   "detached EOF identifier owns its bytes");
            STR_EQ(runner, (const char *)item->attributes.anchor.data, "item", "item identifier owns its bytes");
            OK(runner, list->attributes.anchor.alloc && item->attributes.anchor.alloc, "anchors are owned chunks");
            markdown_core_node_free(root);
        }
    }
}

/* Exact slices may be read-only and need neither a NUL nor padding. Every
 * scanner exercises successful syntax and all truncated prefixes, with ASan
 * guarding an allocation that ends precisely at each prefix boundary. */
static void bounded_scanners(test_batch_runner *runner) {
    typedef bufsize_t (*scanner)(const unsigned char *, bufsize_t, bufsize_t);
    const struct {
        scanner scan;
        const char *text;
        int expected;
    } cases[] = {{scan_scheme, "https:", -1},
                 {scan_autolink_uri, "https://example.com>", -1},
                 {scan_autolink_email, "a@example.com>", -1},
                 {scan_html_tag, "x a='b'>", -1},
                 {scan_html_comment, "--hi-->", -1},
                 {scan_html_pi, "x?>", 1},
                 {scan_html_declaration, "DOCTYPE html>", 12},
                 {scan_html_cdata, "CDATA[x]]>", 7},
                 {scan_html_block_start, "<script>", 1},
                 {scan_html_block_start_7, "<x>\n", 7},
                 {scan_html_block_end_1, "body</script>", -1},
                 {scan_html_block_end_2, "x-->", -1},
                 {scan_html_block_end_3, "x?>", -1},
                 {scan_html_block_end_4, "x>", -1},
                 {scan_html_block_end_5, "x]]>", -1},
                 {scan_link_title, "\"title\"", -1},
                 {scan_atx_heading_start, "## ", -1},
                 {scan_setext_heading_line, "---\n", 2},
                 {scan_open_code_fence, "```lang\n", 3},
                 {scan_close_code_fence, "``` \n", 3},
                 {scan_entity, "&amp;", -1},
                 {scan_dangerous_url, "javascript:", -1},
                 {scan_footnote_definition, "[^note]: ", -1},
                 {scan_table_start, "| - | :--: |\n", -1},
                 {scan_table_cell, "x\\|y", -1},
                 {scan_table_cell_end, "| \t", -1},
                 {scan_table_row_end, " \r\n", -1},
                 {scan_table_horizontal, "+---+", '-'},
                 {scan_table_horizontal, "+===+", '='},
                 {scan_formula_dollar_inline_open, "$", -1},
                 {scan_formula_dollar_backtick_open, "$`", -1},
                 {scan_formula_dollar_display_open, "$$", -1},
                 {scan_formula_latex_backslash_inline_open, "\\\\(", -1},
                 {scan_formula_latex_backslash_display_open, "\\\\[", -1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        size_t length = strlen(cases[i].text);
        int expected = cases[i].expected < 0 ? (int)length : cases[i].expected;
        const unsigned char *literal = (const unsigned char *)cases[i].text;
        INT_EQ(runner, cases[i].scan(literal, (bufsize_t)length, 0), expected, "read-only scan: case=%zu", i);
        INT_EQ(runner, cases[i].scan(NULL, (bufsize_t)length, 0), 0, "absent source is not scanned: case=%zu", i);
        INT_EQ(runner, cases[i].scan(literal, -1, 0), 0, "negative length is not scanned: case=%zu", i);
        const bufsize_t invalid_offsets[] = {-1, (bufsize_t)length, (bufsize_t)length + 1, INT_MAX};
        for (size_t offset = 0; offset < sizeof(invalid_offsets) / sizeof(*invalid_offsets); offset++) {
            INT_EQ(runner, cases[i].scan(literal, (bufsize_t)length, invalid_offsets[offset]), 0,
                   "invalid or empty suffix is not scanned: case=%zu offset=%d", i, invalid_offsets[offset]);
        }
        for (size_t end = 0; end <= length; end++) {
            unsigned char *exact = malloc(end ? end : 1);
            memcpy(exact, cases[i].text, end);
            for (size_t start = 0; start < end; start++) {
                INT_EQ(runner, cases[i].scan(exact, (bufsize_t)end, (bufsize_t)start),
                       cases[i].scan(literal + start, (bufsize_t)(end - start), 0),
                       "bytes outside slice are invisible: case=%zu start=%zu end=%zu", i, start, end);
            }
            INT_EQ(runner, cases[i].scan(exact, (bufsize_t)end, (bufsize_t)end), 0,
                   "an empty slice needs no sentinel: case=%zu end=%zu", i, end);
            OK(runner, !memcmp(exact, cases[i].text, end), "scanning never changes borrowed bytes");
            free(exact);
        }
    }
    const unsigned char embedded[] = {'+', '-', '+', 0, '+', '-', '+'};
    INT_EQ(runner, scan_table_horizontal(embedded, (bufsize_t)sizeof(embedded), 0), 0,
           "embedded NUL cannot terminate a grid boundary");
}

/* Oracle-confirmed simple-body ownership is invariant under line ending,
 * EOF termination and block-looking inline cell content. */
/* A BUFFER WALK MUST TERMINATE, whatever the bytes are.
 *
 * Valid UTF-8 is the public precondition and what malformed input parses to is
 * not defined -- but the walk itself must still end and stay inside its own
 * buffer. `markdown_core_utf8proc_anchor` used to assert that precondition and
 * then add the decoder's width, which is NEGATIVE when no character starts
 * there; Release is the default build type, so the assertion was compiled out
 * and `# heading \xff\xfe tail` walked backwards below the literal forever.
 * Anchors are derived from every heading, so one stray byte in one heading hung
 * the parser. */
static void malformed_scalar_terminates(test_batch_runner *runner) {
    /* Each reaches a different decode-and-advance walk: the heading anchor,
     * the attribute value, and an ordinary text run. */
    static const char *const sources[] = {
        "# heading \xff\xfe tail\n\nbody\n",
        "# title {.cls\xff\xfe}\n\nbody\n",
        "para \xff\xfe more\n",
        "# \xe4\xb8\n",
        "# \x80\x80\x80\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); i++) {
        markdown_core_document *document = facade_parse((const uint8_t *)sources[i], strlen(sources[i]));
        OK(runner, document != NULL, "malformed scalar source %zu parses to a document", i);
        if (document) {
            OK(runner, markdown_core_document_root(document) != NULL, "malformed scalar source %zu has a root", i);
            markdown_core_document_free(document);
        }
    }
}

/* THE DEFINITION GATE READS A PREFIX, NEVER A LINE. The transaction that
 * decides a definition term reads the next line stripped of its container
 * prefix and asks whether it begins with a marker; the raw-source key that
 * spares the transaction asks the same question of the same byte, past the
 * quote markers and whitespace a prefix can be made of. So prose that merely
 * contains ": " opens no transaction at all, and the key's cost is the
 * container depth whatever the prose length: on lines of 4,096 bytes with a
 * ": " in the middle of each, the lookahead visits no line and the gate's
 * work is a constant per line. The gate must still admit what the grammar
 * accepts: a marker after the blank the grammar allows, and one behind a
 * quote marker. */
static void definition_gate_reads_the_prefix_not_the_line(test_batch_runner *runner) {
    static const size_t lines = 64;
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    for (size_t i = 0; i < lines; i++) {
        for (size_t j = 0; j < 2048; j++) {
            markdown_core_strbuf_putc(&source, 'a' + (char)(j % 26));
        }
        markdown_core_strbuf_puts(&source, ": ");
        for (size_t j = 0; j < 2048; j++) {
            markdown_core_strbuf_putc(&source, 'a' + (char)(j % 26));
        }
        markdown_core_strbuf_puts(&source, "\n\n");
    }
    inline_work work = {0};
    markdown_core_node *root =
        markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
    OK(runner, root != NULL, "prose with a colon-space in every line parses");
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_DEFINITION_LIST), 0, "and holds no definition list");
    INT_EQ(runner, work.lookahead, 0, "the definition gate opened no lookahead transaction for it");
    OK(runner, work.definition_lists <= 2 * lines,
       "and the gate's own work is a constant per paragraph-opening line, not the line: %zu for %zu lines",
       work.definition_lists, lines);
    markdown_core_node_free(root);
    markdown_core_strbuf_free(&source);

    /* Every container on the child spine, alone and nested (a footnote's
     * body is owned off it and strips indentation like a list item), so the
     * prefix bytes the key walks are checked against each continuation
     * grammar rather than against the one the key was written for. */
    static const char *const admitted[] = {"Term\n: body\n",
                                           "Term\n\n: body\n",
                                           "> Term\n> : body\n",
                                           "> Term\n>\n> : body\n",
                                           "Term\n~ body\n",
                                           "Term\n:\n",
                                           "- Term\n  : body\n",
                                           "1. Term\n   : body\n",
                                           "* Term\n\n  : body\n",
                                           "- > Term\n  > : body\n",
                                           "> - Term\n>   : body\n",
                                           "> > Term\n> > : body\n",
                                           "::: note\nTerm\n: body\n:::\n"};
    for (size_t i = 0; i < sizeof(admitted) / sizeof(*admitted); i++) {
        root = markdown_core_parse_document(admitted[i], strlen(admitted[i]));
        OK(runner, root != NULL, "definition shape %zu parses", i);
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_DEFINITION_LIST), 1,
               "the key admits the definition the grammar accepts: shape %zu", i);
        markdown_core_node_free(root);
    }
}

typedef struct {
    size_t declared, missing, extra;
    bool indentation;
} container_prefix_sweep;

/* The table is projected when the instance's dialect is sealed, so it is read
 * from inside the instance, as it is disposed. */
static void record_container_prefix(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    container_prefix_sweep *sweep = parser->context;
    const markdown_core_dialect *dialect = parser->dialect;
    bool expected[256] = {false};
    if (!sweep) {
        return;
    }
    expected[' '] = expected['\t'] = true;
    for (size_t i = 0; i < dialect->element_count; i++) {
        const char *bytes = dialect->instances[i].lines->prefix_bytes;
        if (!bytes) {
            continue;
        }
        sweep->declared++;
        for (const unsigned char *c = (const unsigned char *)bytes; *c; c++) {
            expected[*c] = true;
            sweep->missing += !dialect->container_prefix[*c];
        }
    }
    sweep->indentation = dialect->container_prefix[' '] && dialect->container_prefix['\t'];
    for (int byte = 0; byte < 256; byte++) {
        sweep->extra += dialect->container_prefix[byte] && !expected[byte];
    }
}
static const markdown_core_element CONTAINER_PREFIX_RECORDER = {.name = "container-prefix-recorder",
                                                                .dispose_parser = record_container_prefix};
static bool sweep_container_prefix(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    return markdown_core_dialect_builder_attach(builder, &CONTAINER_PREFIX_RECORDER);
}

/* The bytes the definition key walks over are not the key's own list: they
 * are projected from what each container element declares its continuation
 * strips, plus indentation, so a container added with a new prefix byte is
 * declared beside its grammar and the key follows. The projection is checked
 * here; the declarations are checked above, by the grammar. */
static void container_prefix_is_projected_from_the_elements(test_batch_runner *runner) {
    container_prefix_sweep sweep = {0, 0, 0, false};
    static const char probe_source[] = "probe\n";
    markdown_core_node *probe_doc =
        markdown_core_parse_document_with_setup(probe_source, sizeof(probe_source) - 1, sweep_container_prefix, &sweep);
    OK(runner, sweep.declared >= 1, "at least one container declares a prefix byte: declared=%zu", sweep.declared);
    OK(runner, sweep.indentation, "indentation is a container prefix without any declaration");
    INT_EQ(runner, (int)sweep.missing, 0, "every declared prefix byte is in the projected table");
    INT_EQ(runner, (int)sweep.extra, 0, "and nothing else is");
    markdown_core_node_free(probe_doc);
}

/* A container may declare a prefix byte that is also the definition marker
 * byte. Raw source cannot tell the two apart, so the key must hand such a
 * line to the transaction rather than walk over the marker and refuse the
 * definition: a key that walked it would make a definition nested in that
 * container silently parse as prose. */
static const markdown_core_line_ops COLON_PREFIX_CONTAINER_LINES = {
    .prefix_bytes = ":",
};

static const markdown_core_element COLON_PREFIX_CONTAINER = {
    .name = "colon-prefix",
    .lines = &COLON_PREFIX_CONTAINER_LINES,
};
static const markdown_core_line_ops TILDE_PREFIX_CONTAINER_LINES = {
    .prefix_bytes = "~",
};

static const markdown_core_element TILDE_PREFIX_CONTAINER = {
    .name = "tilde-prefix",
    .lines = &TILDE_PREFIX_CONTAINER_LINES,
};
static bool attach_marker_prefix_containers(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    return markdown_core_dialect_builder_attach(builder, &COLON_PREFIX_CONTAINER) &&
           markdown_core_dialect_builder_attach(builder, &TILDE_PREFIX_CONTAINER) && attach_work_recorder(builder);
}
static void a_prefix_byte_that_is_a_marker_byte_hands_the_line_to_the_transaction(test_batch_runner *runner) {
    static const char *const sources[] = {"Term\n: body\n", "Term\n~ body\n", "> Term\n> : body\n"};
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); i++) {
        inline_work work = {0};
        markdown_core_node *root = markdown_core_parse_document_with_setup(sources[i], strlen(sources[i]),
                                                                           attach_marker_prefix_containers, &work);
        OK(runner, root != NULL, "shape %zu parses with marker bytes declared as prefix bytes", i);
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_DEFINITION_LIST), 1,
               "and the definition is still recognised: shape %zu", i);
        OK(runner, work.lookahead >= 1, "because the key handed the line to the transaction: shape %zu", i);
        markdown_core_node_free(root);
    }
}

/* EACH INSTANCE PARSES WITH ITS OWN SEALED DIALECT. A setup extends only
 * the dialect of the instance it configures, and that dialect is sealed
 * before the instance reads a byte: an instance begun from inside another,
 * with a different setup, asks only the elements its own setup registered,
 * and the outer instance's dialect is exactly what its setup sealed, before
 * and after. Nothing about a dialect is process state. */
typedef struct instance_marks {
    size_t marked_a, marked_b;
    size_t core, registered_before, registered_after;
    const markdown_core_element *last_before, *last_after;
    struct instance_marks *inner;
    bool inner_parsed;
} instance_marks;

static markdown_core_node *mark_percent_a(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                          markdown_core_node *parent, unsigned char character,
                                          markdown_core_inline_state *inline_state) {
    (void)self;
    (void)parent;
    (void)character;
    (void)inline_state;
    ((instance_marks *)parser->context)->marked_a++;
    return NULL;
}
static markdown_core_node *mark_percent_b(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                          markdown_core_node *parent, unsigned char character,
                                          markdown_core_inline_state *inline_state) {
    (void)self;
    (void)parent;
    (void)character;
    (void)inline_state;
    ((instance_marks *)parser->context)->marked_b++;
    return NULL;
}
static const markdown_core_inline_ops PERCENT_MARKER_A_INLINES = {
    .match = mark_percent_a,
    .terminates = "%",
    .dispatch = "%",
};

static const markdown_core_element PERCENT_MARKER_A = {
    .name = "percent-a-marker",
    .inlines = &PERCENT_MARKER_A_INLINES,
};
static const markdown_core_inline_ops PERCENT_MARKER_B_INLINES = {
    .match = mark_percent_b,
    .terminates = "%",
    .dispatch = "%",
};

static const markdown_core_element PERCENT_MARKER_B = {
    .name = "percent-b-marker",
    .inlines = &PERCENT_MARKER_B_INLINES,
};

static bool register_inner_instance(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    return markdown_core_dialect_builder_attach(builder, &PERCENT_MARKER_B);
}

static void begin_outer_instance(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    static const char inner_source[] = "inner % text\n";
    instance_marks *outer = parser->context;
    const markdown_core_dialect *dialect = parser->dialect;
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->init(self, parser);
    outer->registered_before = dialect->element_count;
    outer->last_before = dialect->instances[dialect->element_count - 1].element;
    markdown_core_node *inner = markdown_core_parse_document_with_setup(inner_source, sizeof(inner_source) - 1,
                                                                        register_inner_instance, outer->inner);
    outer->inner_parsed = inner != NULL;
    markdown_core_node_free(inner);
    outer->registered_after = parser->dialect->element_count;
    outer->last_after = parser->dialect->instances[parser->dialect->element_count - 1].element;
}

static bool register_outer_instance(markdown_core_dialect_builder *builder, void *context) {
    static document_owner document;
    markdown_core_dialect_builder_elements(builder, &((instance_marks *)context)->core);
    return attach_document_owner(builder, &document, "outer-instance", begin_outer_instance) &&
           markdown_core_dialect_builder_attach(builder, &PERCENT_MARKER_A);
}

static void each_instance_parses_with_its_own_dialect(test_batch_runner *runner) {
    static const char source[] = "outer % text\n";
    instance_marks inner = {0}, outer = {0}, none = {0};
    outer.inner = &inner;
    markdown_core_node *root =
        markdown_core_parse_document_with_setup(source, sizeof(source) - 1, register_outer_instance, &outer);
    OK(runner, root != NULL && outer.inner_parsed, "an instance with another setup parses inside this one");
    INT_EQ(runner, (int)outer.marked_a, 1, "the outer instance asks the element its setup registered");
    INT_EQ(runner, (int)outer.marked_b, 0, "and never the one the inner instance's setup registered");
    INT_EQ(runner, (int)inner.marked_b, 1, "the inner instance asks its own");
    INT_EQ(runner, (int)inner.marked_a, 0, "and never the outer one's");
    INT_EQ(runner, (int)outer.registered_before, (int)outer.core + 2,
           "the outer dialect is the core dialect and what its setup registered");
    INT_EQ(runner, (int)outer.registered_after, (int)outer.registered_before,
           "and the inner instance leaves it as it was sealed");
    OK(runner, outer.last_before == &PERCENT_MARKER_A && outer.last_after == &PERCENT_MARKER_A,
       "with the same element last, before and after");
    markdown_core_node_free(root);

    root = markdown_core_parse_document_with_setup(source, sizeof(source) - 1, NULL, &none);
    OK(runner, root != NULL && none.marked_a == 0 && none.marked_b == 0,
       "an instance without setup registers neither marker, whatever ran before it");
    markdown_core_node_free(root);
}

/* THE DIALECT IS BOUNDED WHERE THE PROJECTION NEEDS IT TO BE. The block-start
 * projection lists a family's owners by byte, so the attachment API refuses
 * the element that would make an owner index or a count not fit, leaving the
 * dialect as it was; without that bound a release build would wrap the byte
 * and dispatch to the wrong owner. */
static markdown_core_element DIALECT_FILLERS[MARKDOWN_CORE_ELEMENT_LIMIT + 1];
typedef struct {
    size_t attached, count, refused_again, refused_twice;
} dialect_fill;
static bool fill_the_dialect(markdown_core_dialect_builder *builder, void *context) {
    dialect_fill *fill = context;
    for (size_t i = 0; i < sizeof(DIALECT_FILLERS) / sizeof(*DIALECT_FILLERS); i++) {
        DIALECT_FILLERS[i].name = "dialect-filler";
    }
    /* A dialect holds an element once: the first filler again is refused. */
    fill->refused_twice = markdown_core_dialect_builder_attach(builder, &DIALECT_FILLERS[0]) &&
                          !markdown_core_dialect_builder_attach(builder, &DIALECT_FILLERS[0]);
    fill->attached = fill->refused_twice;
    while (fill->attached < sizeof(DIALECT_FILLERS) / sizeof(*DIALECT_FILLERS) &&
           markdown_core_dialect_builder_attach(builder, &DIALECT_FILLERS[fill->attached])) {
        fill->attached++;
    }
    markdown_core_dialect_builder_elements(builder, &fill->count);
    fill->refused_again = fill->attached < sizeof(DIALECT_FILLERS) / sizeof(*DIALECT_FILLERS) &&
                          !markdown_core_dialect_builder_attach(builder, &DIALECT_FILLERS[fill->attached]);
    return true;
}
static void the_dialect_refuses_the_element_the_projection_could_not_index(test_batch_runner *runner) {
    dialect_fill fill = {0, 0, 0, 0};
    static const char probe_source[] = "- item\n\nTerm\n: body\n";
    markdown_core_node *root =
        markdown_core_parse_document_with_setup(probe_source, sizeof(probe_source) - 1, fill_the_dialect, &fill);
    OK(runner, root != NULL, "the document parses with the dialect full");
    OK(runner, fill.attached >= 1, "attachment succeeded up to the bound: %zu attached", fill.attached);
    INT_EQ(runner, (int)fill.count, MARKDOWN_CORE_ELEMENT_LIMIT, "and stopped exactly at the bound");
    OK(runner, fill.refused_again, "every attachment past it is refused");
    OK(runner, fill.refused_twice, "an element already attached is refused");
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_LIST), 1, "the projection still dispatches to the list owner");
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_DEFINITION_LIST), 1, "and to the definition owner");
    markdown_core_node_free(root);
}

/* THE INLINE-HOOK PROJECTION'S WORK INVARIANT.
 *
 * Three inline-content hooks used to be found by scanning the whole element
 * dialect once per inline-content node. They are projected now, so the
 * dispatch examines the elements that DECLARED each hook and no others.
 *
 * Nothing about the output can tell those two apart: a dispatch that went back
 * to scanning every attached element would call the same hooks in the same
 * order and build the identical tree, so every golden, every spec example and
 * every byte-equivalence check would still pass. `inline_hook_work` counts the
 * elements the dispatch examined, which is the one place the difference is
 * visible.
 *
 * The adversarial shape is the one that defeats the scan: attach elements that
 * declare NO inline hook at all. Under the scan the work grows with the
 * dialect, so attaching 32 of them multiplies it; under the projection they
 * are not in any family's list and the count cannot move. The second assertion
 * is the load-bearing one; the first is there so the invariant cannot be
 * satisfied by a counter that never fires. */
static const markdown_core_element INLINE_HOOK_BYSTANDERS[32];

static bool attach_inline_hook_bystanders(markdown_core_dialect_builder *builder, void *context) {
    (void)context;
    for (size_t i = 0; i < 32; i++) {
        if (!markdown_core_dialect_builder_attach(builder, &INLINE_HOOK_BYSTANDERS[i])) {
            return false;
        }
    }
    return attach_work_recorder(builder);
}

static void inline_hook_projection_ignores_non_owners(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    for (size_t i = 0; i < 256; i++) {
        markdown_core_strbuf_puts(&source, "a paragraph with *emphasis* and a `code span` in it\n\n");
    }

    inline_work plain = {0};
    markdown_core_node *root =
        markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &plain);
    OK(runner, root != NULL, "the inline-hook work document parses");
    OK(runner, plain.inline_hooks > 0, "the inline-hook dispatch examines at least one owner, so the counter is live");
    markdown_core_node_free(root);

    inline_work crowded = {0};
    root = markdown_core_parse_document_with_setup((char *)source.ptr, source.size, attach_inline_hook_bystanders,
                                                   &crowded);
    OK(runner, root != NULL, "the same document parses with 32 hookless elements attached");
    INT_EQ(runner, (int)crowded.inline_hooks, (int)plain.inline_hooks,
           "elements that declare no inline hook add no work to the per-node dispatch");
    markdown_core_node_free(root);

    markdown_core_strbuf_free(&source);
}

/* Metadata, the block driver and lookahead borrow the same physical lines.
 * Bare CR, LF and CRLF all count as one ending; every raw byte contributes
 * exactly once even when frontmatter classified its line before the driver. */
static void properties_envelope_derives_each_line_once(test_batch_runner *runner) {
    static const char *const endings[] = {"\n", "\r", "\r\n"};
    for (size_t e = 0; e < sizeof(endings) / sizeof(*endings); e++) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        char line[64];
        markdown_core_strbuf_puts(&source, "---");
        markdown_core_strbuf_puts(&source, endings[e]);
        for (size_t i = 0; i < 512; i++) {
            snprintf(line, sizeof(line), "unknown-%zu: value %zu", i, i);
            markdown_core_strbuf_puts(&source, line);
            markdown_core_strbuf_puts(&source, endings[e]);
        }
        markdown_core_strbuf_puts(&source, "keywords:");
        markdown_core_strbuf_puts(&source, endings[e]);
        for (size_t i = 0; i < 256; i++) {
            snprintf(line, sizeof(line), "  - item %zu", i);
            markdown_core_strbuf_puts(&source, line);
            markdown_core_strbuf_puts(&source, endings[e]);
        }
        markdown_core_strbuf_puts(&source, "abstract: |");
        markdown_core_strbuf_puts(&source, endings[e]);
        for (size_t i = 0; i < 256; i++) {
            snprintf(line, sizeof(line), "  literal line %zu", i);
            markdown_core_strbuf_puts(&source, line);
            markdown_core_strbuf_puts(&source, endings[e]);
        }
        markdown_core_strbuf_puts(&source, "# a comment line the skip must step over");
        markdown_core_strbuf_puts(&source, endings[e]);
        markdown_core_strbuf_puts(&source, "title: \"Envelope\"");
        markdown_core_strbuf_puts(&source, endings[e]);
        markdown_core_strbuf_puts(&source, "---");
        markdown_core_strbuf_puts(&source, endings[e]);
        markdown_core_strbuf_puts(&source, "body");
        markdown_core_strbuf_puts(&source, endings[e]);

        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "the envelope document parses: ending=%zu", e);
        markdown_core_node *metadata = root ? root->as.document->metadata : NULL;
        OK(runner, metadata != NULL, "the envelope is recognized: ending=%zu", e);
        if (metadata) {
            markdown_core_metadata_scalar scalar;
            const markdown_core_metadata_value *record = facade_metadata(metadata, markdown_core_metadata_title);
            OK(runner,
               metadata_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_TEXT &&
                   scalar.value.string.length == 8 && !memcmp(scalar.value.string.data, "Envelope", 8),
               "the scalar member after the multi-line members decodes: ending=%zu", e);
            record = facade_metadata(metadata, markdown_core_metadata_keywords);
            OK(runner, record && record->kind == MARKDOWN_CORE_METADATA_LIST && record->as.list.count == 256,
               "the block sequence owns exactly its item lines: ending=%zu", e);
            record = facade_metadata(metadata, markdown_core_metadata_abstract);
            size_t breaks = 0;
            bool text = metadata_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_TEXT;
            for (size_t i = 0; text && i < scalar.value.string.length; i++) {
                breaks += scalar.value.string.data[i] == '\n';
            }
            /* "literal line N" for N in 0..255, each ending in one LF. */
            size_t expected = 10 * 14 + 90 * 15 + 156 * 16 + 256;
            OK(runner, text && breaks == 256 && scalar.value.string.length == expected,
               "the literal block owns exactly its lines with endings normalized: ending=%zu", e);
        }
        OK(runner, work.properties_lines > 0, "the fence search examines bytes, so the counter is live: ending=%zu", e);
        OK(runner, work.properties_lines <= (size_t)source.size,
           "the closing fence search examines each source byte at most once");
        INT_EQ(runner, work.physical_line_bytes, (size_t)source.size,
               "all consumers share one physical scan of the input");
        OK(runner, work.metadata_key_bytes > 0 && work.metadata_key_bytes <= (size_t)source.size,
           "boundary discovery and field decoding classify each member line once");
        markdown_core_node_free(root);
        markdown_core_strbuf_free(&source);
    }
}

/* Unknown plain keys and already accepted fields borrow their source spans.
 * Only the line index grows with rejected members; values allocate nothing. */
static void properties_rejected_members_borrow_source(test_batch_runner *runner) {
    size_t baseline_allocations = 0, baseline_decoded = 0;
    for (size_t members = 0; members <= 1024; members += 1024) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "---\ntitle: Known\n");
        for (size_t i = 0; i < members; i++) {
            markdown_core_strbuf_puts(&source, i % 2 ? "title: ignored\n" : "unsupported: ignored\n");
        }
        markdown_core_strbuf_puts(&source, "---\nbody\n");
        payload_probe_arm();
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "metadata with %zu rejected members parses", members);
        if (!members) {
            baseline_allocations = payload_allocations;
            baseline_decoded = work.metadata_value_bytes;
        } else {
            OK(runner, payload_allocations <= baseline_allocations + 16,
               "rejected members add logarithmic index growth, not per-key allocations: %zu versus %zu",
               payload_allocations, baseline_allocations);
            INT_EQ(runner, work.metadata_value_bytes, baseline_decoded,
                   "unknown and duplicate values are never decoded");
        }
        markdown_core_node_free(root);
        INT_EQ(runner, payload_live, 0, "the metadata transaction releases all allocations");
        payload_probe_disarm();
        markdown_core_strbuf_free(&source);
    }
}

static void source_line_geometry_is_shared(test_batch_runner *runner) {
    /* Physical geometry classifies bytes before any UTF-8 interpretation.
     * Every byte other than CR/LF is content; NUL requests normalization. */
    for (unsigned value = 0; value <= 255; value++) {
        for (size_t position = 0; position < 3 * sizeof(uint64_t); position++) {
            unsigned char bytes[3 * sizeof(uint64_t) + 1];
            memset(bytes, 'a', sizeof(bytes));
            bytes[position] = (unsigned char)value;
            bytes[sizeof(bytes) - 1] = '\n';
            /* The whole text: no commit follows. */
            markdown_core_stream whole = {.ends = true};
            markdown_core_parser reader = {.stream = &whole};
            reader.input.source = bytes;
            reader.input.size = sizeof(bytes);
            reader.input.first_line = 1;
            markdown_core_input_line *first = markdown_core_parser_source_line(&reader, 1);
            bool split = value == '\r' || value == '\n';
            OK(runner, first && first->start == 0 && first->end == (split ? position : sizeof(bytes) - 1),
               "only CR and LF end a physical line: byte %u at %zu", value, position);
            OK(runner, first && ((first->facts && reader.input.facts[first->facts - 1].nul_count == 1) == (value == 0)),
               "only NUL requests normalization: byte %u", value);
            markdown_core_input_line *second = markdown_core_parser_source_line(&reader, 2);
            bool remaining = split && !(value == '\r' && position == sizeof(bytes) - 2);
            OK(runner,
               remaining ? second && second->start == position + 1 && second->end == sizeof(bytes) - 1 : !second,
               "content after a word or tail boundary belongs to exactly one line: byte %u at %zu", value, position);
            INT_EQ(runner, reader.input_line_work, sizeof(bytes), "span search advances the frontier exactly once");
            markdown_core_free(reader.input.facts);
            markdown_core_array_free(&reader.input.lines.front);
        }
    }
    static const unsigned char source[] = "a\0b\r\nc\rd\nlast";
    markdown_core_stream whole = {.ends = true};
    markdown_core_parser parser = {.stream = &whole};
    parser.input.source = source;
    parser.input.size = sizeof(source) - 1;
    parser.input.first_line = 7;
    static const size_t starts[] = {0, 5, 7, 9}, ends[] = {3, 6, 8, 13}, next[] = {5, 7, 9, 13};
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 3; i >= 0; i--) {
            markdown_core_input_line *line = markdown_core_parser_source_line(&parser, i + 7);
            OK(runner,
               line && line->start == starts[i] && line->end == ends[i] &&
                   markdown_core_input_line_next(&parser, line) == next[i],
               "physical line geometry is stable across lookahead and replay: %d/%d", pass, i);
            OK(runner, line && (line->facts && parser.input.facts[line->facts - 1].nul_count != 0) == (i == 0),
               "NUL normalization is a line fact");
        }
        INT_EQ(runner, parser.input_line_work, sizeof(source) - 1, "replay never scans source geometry again");
    }
    OK(runner, !markdown_core_parser_source_line(&parser, 6) && !markdown_core_parser_source_line(&parser, 11),
       "the active input has an explicit line range");
    INT_EQ(runner, parser.input.fact_count, 1, "only the NUL-bearing line needs optional geometry state");
    markdown_core_line_facts *first = markdown_core_parser_get_line_facts(&parser, 7);
    OK(runner, first != NULL, "a grammar query attaches facts to its physical line");
    if (first) {
        first->run_end = 10;
    }
    markdown_core_line_facts *last = markdown_core_parser_get_line_facts(&parser, 10);
    OK(runner, last && !last->run_end && markdown_core_parser_get_line_facts(&parser, 7)->run_end == 10,
       "unrelated line queries preserve their own facts");
    INT_EQ(runner, parser.input.fact_count, 2, "only queried lines own speculative facts");
    INT_EQ(runner, parser.input_line_work, sizeof(source) - 1, "facts do not rediscover geometry");
    markdown_core_free(parser.input.facts);
    markdown_core_array_free(&parser.input.lines.front);
}

static void short_line_storage_is_bounded(test_batch_runner *runner) {
    INT_EQ(runner, sizeof(markdown_core_input_line), 16, "ordinary physical geometry occupies sixteen bytes");
    for (size_t count = 8; count <= 65537; count = count == 8 ? 1025 : count * 64 - 63) {
        for (int shape = 0; shape < 3; shape++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            for (size_t i = 0; i < count; i++) {
                if (shape) {
                    markdown_core_strbuf_putc(&source, shape == 1 ? 'x' : 0);
                }
                markdown_core_strbuf_putc(&source, '\n');
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial short lines parse");
            INT_EQ(runner, work.physical_lines, count, "one geometry record per physical line");
            INT_EQ(runner, work.physical_line_bytes, source.size, "the source frontier consumes each byte once");
            INT_EQ(runner, work.physical_facts, shape == 2 ? count : 0,
                   "every NUL-bearing line, and no ordinary short line, needs one fact record");
            INT_EQ(runner, work.normalized_lines, shape == 2 ? count : 0,
                   "each NUL-bearing line owns exactly one replacement view");
            OK(runner,
               shape == 2 ? work.physical_fact_capacity >= count && work.physical_fact_capacity < 2 * count
                          : work.physical_fact_capacity == 0,
               "optional facts obey their own doubling bound");
            OK(runner, work.physical_capacity >= count && work.physical_capacity < 2 * count,
               "retained geometry capacity is bounded by twice the number of lines");
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void growth_preserves_input_views(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    markdown_core_strbuf_puts(&source, "---\nname: title\nabstract: |\n");
    for (int i = 0; i < 257; i++) {
        markdown_core_strbuf_puts(&source, "  prose\n");
    }
    markdown_core_strbuf_puts(&source, "---\n\n");
    for (int i = 0; i < 65; i++) {
        markdown_core_strbuf_puts(&source, "a   b\n--- ---\nc   d\n--- ---\n\n");
    }
    properties_probe_arm();
    markdown_core_document *reference = facade_parse(source.ptr, source.size);
    uint8_t *expected = NULL, *actual = NULL;
    size_t expected_length = 0, actual_length = 0;
    OK(runner,
       reference &&
           (markdown_core_document_dump(reference, markdown_core_document_root(reference), (const uint8_t *)source.ptr,
                                        source.size, &expected, &expected_length) == MARKDOWN_CORE_OK),
       "reference metadata and tables have a complete canonical dump");
    properties_force_moves = 1;
    markdown_core_document *moved = facade_parse(source.ptr, source.size);
    OK(runner,
       moved && (markdown_core_document_dump(moved, markdown_core_document_root(moved), (const uint8_t *)source.ptr,
                                             source.size, &actual, &actual_length) == MARKDOWN_CORE_OK),
       "metadata and separator views survive every growth moving storage");
    OK(runner, expected && actual && expected_length == actual_length && !memcmp(expected, actual, expected_length),
       "moving shared workspaces preserves every value, child and source position");
    markdown_core_dump_free(actual);
    markdown_core_document_free(moved);
    markdown_core_dump_free(expected);
    markdown_core_document_free(reference);
    INT_EQ(runner, properties_live_bytes, 0, "moving storage retains no allocations after disposal");
    properties_force_moves = 0;
    properties_probe_disarm();
    markdown_core_strbuf_free(&source);
}

static void construction_checks_containment(test_batch_runner *runner) {
#if !defined(NDEBUG) && !defined(_WIN32)
    pid_t child = fork();
    if (child == 0) {
        (void)freopen("/dev/null", "w", stderr);
        markdown_core_node *parent = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
        markdown_core_node *block = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
        markdown_core_node_push_child(NULL, parent, block);
        _exit(0);
    }
    int status = 0;
    OK(runner, child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
       "an unproven built-in attachment triggers the Debug/ASan containment assertion");
#else
    (void)runner;
#endif
}

static markdown_core_parse_error observed_token_error;
static void observe_token_error(const markdown_core_element_instance *self, markdown_core_inline_state *state) {
    (void)self;
    if (state->error) {
        observed_token_error = state->error;
    }
}
static const markdown_core_inline_ops ERROR_OBSERVER_INLINES = {
    .dispose = observe_token_error,
};

static const markdown_core_element ERROR_OBSERVER = {
    .name = "error-observer",
    .inlines = &ERROR_OBSERVER_INLINES,
};
static bool configure_error_observer(markdown_core_dialect_builder *builder, void *context) {
    return configure_conversion_policy(builder, context) &&
           markdown_core_dialect_builder_attach(builder, &ERROR_OBSERVER);
}
/* The policy of the parse under test, from its setup context: the document
 * refuses every table. */
typedef struct {
    size_t rejections;
} lead_policy;
static lead_policy *lead_policy_current;

static int reject_table(const markdown_core_element *element, markdown_core_node *node, markdown_core_node_type kind) {
    (void)element;
    if (kind == MARKDOWN_CORE_NODE_TABLE) {
        lead_policy_current->rejections++;
        return 0;
    }
    return markdown_core_node_can_contain_builtin(node, kind);
}
static const markdown_core_contain_ops LEAD_POLICY_CONTAIN = {
    .allows = reject_table,
};

static const markdown_core_element LEAD_POLICY = {
    .name = "lead-policy",
    .contain = &LEAD_POLICY_CONTAIN,
};
static void begin_lead_policy(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    MARKDOWN_CORE_ELEMENT_DOCUMENT.document->init(self, parser);
    parser->root->element = &LEAD_POLICY;
    lead_policy_current = parser->context;
}
static bool configure_lead_policy(markdown_core_dialect_builder *builder, void *context) {
    static document_owner document;
    (void)context;
    return attach_document_owner(builder, &document, "lead-policy-document", begin_lead_policy);
}
static void rejected_token_allocation_failures(test_batch_runner *runner) {
    const char *source = "! `code`\n";
    /* A constructor can return its owned token after a literal/attribute
     * allocation failed. Its later policy refusal must preserve that cause. */
    for (size_t fail_at = 1;; fail_at++) {
        conversion_policy policy = {MARKDOWN_CORE_NODE_CODE, 0, 0};
        payload_probe_arm();
        payload_fail_at = fail_at;
        observed_token_error = MARKDOWN_CORE_PARSE_OK;
        markdown_core_node *root =
            markdown_core_parse_document_with_setup(source, strlen(source), configure_error_observer, &policy);
        if (root) {
            markdown_core_node_free(root);
        }
        bool refused = payload_allocations >= fail_at;
        if (refused && observed_token_error) {
            INT_EQ(runner, observed_token_error, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED,
                   "a later policy rejection preserves the constructor's allocation failure: %zu", fail_at);
        }
        INT_EQ(runner, payload_live, 0, "allocation refusal plus semantic refusal leaks nothing: %zu", fail_at);
        payload_probe_disarm();
        if (!refused) {
            break;
        }
    }
}

static void semantic_rejection_preserves_its_cause(test_batch_runner *runner) {
    conversion_policy policy = {MARKDOWN_CORE_NODE_CODE, 0, 0};
    const char *source = "! `code`\n";
    observed_token_error = MARKDOWN_CORE_PARSE_OK;
    markdown_core_node *root =
        markdown_core_parse_document_with_setup(source, strlen(source), configure_error_observer, &policy);
    OK(runner, !root, "a consumed rejected token fails the transaction");
    INT_EQ(runner, observed_token_error, MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED,
           "semantic rejection is distinct from allocation failure");
    if (root) {
        markdown_core_node_free(root);
    }
    markdown_core_parser parser = {0};
    markdown_core_parser_fail(&parser, MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED);
    markdown_core_parser_fail(&parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    INT_EQ(runner, parser.error, MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED, "cleanup cannot overwrite the first cause");

    lead_policy lead = {0};
    source = "lead\n| h |\n| - |\n";
    root = markdown_core_parse_document_with_setup(source, strlen(source), configure_lead_policy, &lead);
    OK(runner, root && lead.rejections, "refused table lead leaves the original parse available");
    if (!root) {
        return;
    }
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_TABLE), 0, "a refused split never changes the paragraph kind");
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_PARAGRAPH), 1,
           "a refused split preserves one complete paragraph");
    markdown_core_node_free(root);
}

/* One semantic decision authorizes one commit. A stateful predicate that
 * accepts only the first link must not be consulted again after allocation. */
static void parser_attachment_commits_one_decision(test_batch_runner *runner) {
    conversion_policy paragraph_policy = {MARKDOWN_CORE_NODE_PARAGRAPH, 0, 1};
    payload_probe_arm();
    markdown_core_node *paragraph_root =
        markdown_core_parse_document_with_setup("plain\n", 6, configure_conversion_policy, &paragraph_policy);
    OK(runner, paragraph_root && count_kind(paragraph_root, MARKDOWN_CORE_NODE_PARAGRAPH) == 1,
       "a paragraph commits its already selected parent");
    INT_EQ(runner, paragraph_policy.allowed, 0, "paragraph parent selection consumes one acceptance");
    INT_EQ(runner, paragraph_policy.rejections, 0, "paragraph construction never repeats parent selection");
    markdown_core_node_free(paragraph_root);
    INT_EQ(runner, payload_live, 0, "a selected paragraph releases all storage");
    markdown_core_parser rejected = {0};
    paragraph_root = markdown_core_node_new(MARKDOWN_CORE_NODE_DOCUMENT);
    paragraph_root->element = &CONVERSION_POLICY;
    conversion_current_policy = &paragraph_policy;
    rejected.root = paragraph_root;
    rejected.at.block_root = rejected.at.current = paragraph_root;
    size_t attempts = payload_allocations;
    OK(runner, !markdown_core_parser_add_child(&rejected, paragraph_root, MARKDOWN_CORE_NODE_PARAGRAPH, 1),
       "root refusal terminates parent selection without walking past the root");
    INT_EQ(runner, rejected.error, MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED,
           "a rejected block parent reports semantic refusal");
    INT_EQ(runner, payload_allocations, attempts, "refused parent selection allocates no child");
    markdown_core_node_free(paragraph_root);
    INT_EQ(runner, payload_live, 0, "refused parent selection releases all storage");
    payload_probe_disarm();

    const char *source = "! [first](u) [second](v)\n";
    conversion_policy policy = {MARKDOWN_CORE_NODE_LINK, 0, 1};
    payload_probe_arm();
    markdown_core_node *root =
        markdown_core_parse_document_with_setup(source, strlen(source), configure_conversion_policy, &policy);
    OK(runner, root != NULL, "a stateful containment policy leaves parsing transactional");
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_LINK), 1,
           "the first decision commits its link and the second leaves literal text");
    INT_EQ(runner, policy.rejections, 1, "commit never repeats the semantic predicate");
    markdown_core_node_free(root);
    INT_EQ(runner, payload_live, 0, "no detached replacement is lost on semantic rejection");
    policy = (conversion_policy){MARKDOWN_CORE_NODE_CODE, 0, 0};
    source = "! `code`\n";
    root = markdown_core_parse_document_with_setup(source, strlen(source), configure_conversion_policy, &policy);
    OK(runner, !root && policy.rejections, "a refused constructed token fails the transaction");
    if (root) {
        markdown_core_node_free(root);
    }
    INT_EQ(runner, payload_live, 0, "a refused token is released even after its constructor consumed input");
    payload_probe_disarm();
}

/* Invalid syntax must not consume a stateful parent's acceptance. Once a
 * formula is recognized, that one decision authorizes its attachment. */
static void formula_containment_follows_recognition(test_batch_runner *runner) {
    static const struct {
        const char *source, *prefix;
    } cases[] = {
        {"! $`$ $good$ $later$\n", "! $`$ "},
        {"! $`bad$ $good$ $later$\n", "! $`bad$ "},
        {"! $`good`$ $later$\n", "! "},
    };
    payload_probe_arm();
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        conversion_policy policy = {MARKDOWN_CORE_NODE_FORMULA, 0, 1};
        markdown_core_node *root = markdown_core_parse_document_with_setup(cases[i].source, strlen(cases[i].source),
                                                                           configure_conversion_policy, &policy);
        OK(runner, root != NULL, "formula policy preserves the parse transaction");
        INT_EQ(runner, policy.allowed, 0, "the first valid formula consumes the acceptance");
        INT_EQ(runner, policy.rejections, 1, "only the later valid formula is rejected");
        if (root) {
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_FORMULA), 1,
                   "invalid candidates never prevent the first valid formula from attaching");
            markdown_core_node *paragraph = child_at(root, 0);
            markdown_core_node *prefix = paragraph ? child_at(paragraph, 0) : NULL;
            markdown_core_node *formula = paragraph ? child_at(paragraph, 1) : NULL;
            markdown_core_node *suffix = paragraph ? child_at(paragraph, 2) : NULL;
            bool expected_shape = prefix && prefix->kind == MARKDOWN_CORE_NODE_TEXT && formula &&
                                  formula->kind == MARKDOWN_CORE_NODE_FORMULA && suffix &&
                                  suffix->kind == MARKDOWN_CORE_NODE_TEXT && paragraph->child_count == 3;
            OK(runner, expected_shape, "only the accepted formula replaces its source span");
            if (expected_shape) {
                LITERAL_EQ(runner, prefix, cases[i].prefix, "invalid formula syntax remains literal text");
                FORMULA_LITERAL_EQ(runner, formula, "good", "the first valid formula owns the recognized body");
                LITERAL_EQ(runner, suffix, " $later$", "the rejected formula retains its original text");
            }
            markdown_core_node_free(root);
        }
        INT_EQ(runner, payload_live, 0, "accepted and rejected formula candidates release all allocations");
    }
    payload_probe_disarm();
}

/* Shared boundaries constrain word bodies, while committed citation affixes
 * constrain every rule. Mix all three run residues and failed/valid word
 * candidates in one growing range; field-local pairs must survive reduction. */
static void delimiter_boundary_floors(test_batch_runner *runner) {
    static const char unit[] = "^a b^ ^a\\ b^ ~c d~ ~c\\ d~ *e f* **g h** ***i j*** "
                               "++k l++ ==m n== [*pre* @key *post*] [*pre @key post*] ";
    for (size_t count = 16; count <= 4096; count *= 16) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, unit);
        }
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "mixed boundary range parses");
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_SUPERSCRIPT), count,
               "only escaped spaces enter word bodies");
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_SUBSCRIPT), count,
               "word floors are shared across word rules");
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_EMPHASIS), 2 * count,
               "ordinary boundaries preserve emphasis in the enclosing range");
        size_t cites = 0;
        for (size_t node_index = 0; node_index < child_at(root, 0)->child_count; node_index++) {
            markdown_core_node *node = child_at(root, 0)->children[node_index];
            if (node->kind == MARKDOWN_CORE_NODE_CITE) {
                markdown_core_node *item = child_at(node, 0);
                size_t emphasis = count_kind(item->as.citation->prefix, MARKDOWN_CORE_NODE_EMPHASIS) +
                                  count_kind(item->as.citation->suffix, MARKDOWN_CORE_NODE_EMPHASIS);
                INT_EQ(runner, emphasis, cites % 2 ? 0 : 2, "pairs stay within each committed citation affix");
                cites++;
            }
        }
        INT_EQ(runner, cites, 2 * count, "all citation groups retain their fields");
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_STRONG), 2 * count,
               "every residue retains its independent pairing");
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_INSERTION), count,
               "ordinary spaces do not inhibit inline bodies");
        INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_MARK), count,
               "unrelated rules retain independent search floors");
        OK(runner, work.delimiters <= 16 * (size_t)source.size, "failed searches and scope reduction remain linear");
        markdown_core_node_free(root);
        markdown_core_strbuf_free(&source);
    }
}

/* EVERY NODE OF A TREE, counted by the test's own walk: the children
 * arrays, and each node's owned field roots, an inline note's included,
 * through the inline-subtree visitor. */
static size_t owned_node_census(markdown_core_node *root);

/* Opaque bodies are leaves, not three disposable Text nodes per formula.
 * Exercise every delimiter form, multiline placement, padding and escapes at
 * growing body sizes and repetition counts, without relying on timings. */
static void formula_leaf_construction_work(test_batch_runner *runner) {
    static const char *const markers[][2] = {
        {"$", "$"}, {"$`", "`$"}, {"$$", "$$"}, {"\\\\(", "\\\\)"}, {"\\\\[", "\\\\]"}};
    for (size_t form = 0; form < sizeof(markers) / sizeof(*markers); form++) {
        for (size_t count = 1; count <= 256; count *= 16) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
            markdown_core_strbuf_puts(&source, "probe ");
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, markers[form][0]);
                for (size_t j = 0; j <= count; j++) {
                    markdown_core_strbuf_puts(&source, "body");
                }
                markdown_core_strbuf_puts(&source, "\nnext");
                markdown_core_strbuf_puts(&source, markers[form][1]);
                markdown_core_strbuf_puts(&source, " end ");
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
            OK(runner, root != NULL, "opaque formula constructs at every size and delimiter form");
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_FORMULA), count, "every body is one opaque leaf");
            INT_EQ(runner, owned_node_census(root), 2 * count + 3,
                   "the tree is the document, its paragraph, the formula leaves and the text around them");
            OK(runner, work.opaque <= 4 * (size_t)source.size, "opaque search remains linear");
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* A formula code block's promotion trims its literal, a view of the block's
 * content, to the formula's: the content's bytes are neither copied nor
 * written. An allocation failure before the commit preserves the old block. */
static void formula_promotion_views_content(test_batch_runner *runner) {
    static const char written[] = " \t value \n ";
    for (size_t fail = 0; fail < 3; fail++) {
        payload_probe_arm();
        markdown_core_parser parser = {0};
        /* An instance with no record: the formula element keeps none. */
        const markdown_core_element_instance formula_instance = {.element = &MARKDOWN_CORE_ELEMENT_FORMULA};
        markdown_core_node *root = markdown_core_node_new(MARKDOWN_CORE_NODE_DOCUMENT);
        markdown_core_node *old = markdown_core_node_new(MARKDOWN_CORE_NODE_CODE_BLOCK);
        append_child(root, old);
        old->as.code->info = markdown_core_optional_chunk_present(markdown_core_chunk_literal("formula"));
        markdown_core_strbuf_puts(&old->content, written);
        old->as.code->literal = (markdown_core_chunk){old->content.ptr, old->content.size, 0};
        const unsigned char *bytes = old->content.ptr;
        payload_fail_at = fail ? payload_allocations + fail : 0;
        markdown_core_formula_promote_fence(&formula_instance, &parser, old, root);
        payload_fail_at = 0;
        if (parser.error) {
            INT_EQ(runner, parser.error, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED, "loss is reported");
            OK(runner, child_at(root, 0) == old && old->kind == MARKDOWN_CORE_NODE_CODE_BLOCK,
               "failed promotion preserves the old block");
            OK(runner, old->as.code->literal.data == bytes, "failed promotion preserves the old literal");
        } else {
            markdown_core_node *formula = child_at(root, 0);
            OK(runner, formula == old && formula->kind == MARKDOWN_CORE_NODE_FORMULA_BLOCK,
               "the code block is the formula block");
            FORMULA_LITERAL_EQ(runner, formula, "value", "promotion trims its literal");
            OK(runner, markdown_core_elements_formula_literal(formula).data == bytes + 3,
               "the formula's literal views the block's content");
        }
        OK(runner,
           old->content.ptr == bytes && old->content.size == (bufsize_t)strlen(written) &&
               memcmp(old->content.ptr, written, strlen(written)) == 0,
           "the content is neither moved nor written");
        markdown_core_node_free(root);
        INT_EQ(runner, payload_live, 0, "all storage is released exactly once");
        payload_probe_disarm();
    }
}

/* A formula fence lies on the lines a code fence would: inside a quote its
 * runs, like the code block's, leave out every line's `> ` prefix. */
static void formula_promotion_keeps_the_fence_runs(test_batch_runner *runner) {
    const char *sources[] = {"> ```formula\n> x\n> ```\n", "> ```haskell\n> x\n> ```\n"};
    const markdown_core_node_type kinds[] = {MARKDOWN_CORE_NODE_FORMULA_BLOCK, MARKDOWN_CORE_NODE_CODE_BLOCK};
    markdown_core_document *documents[2];
    const markdown_core_run *runs[2] = {NULL, NULL};
    size_t counts[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        documents[i] = facade_parse((const uint8_t *)sources[i], strlen(sources[i]));
        OK(runner, documents[i] != NULL, "fence %d in a quote parses", i);
        if (!documents[i]) {
            return;
        }
        markdown_core_node *block = first_of_kind(documents[i]->root, kinds[i]);
        OK(runner, block != NULL, "the quote holds the block of fence %d", i);
        if (block) {
            runs[i] = markdown_core_node_runs(block, &counts[i]);
        }
    }
    INT_EQ(runner, counts[0], 3, "the formula block has a run per line");
    OK(runner, counts[0] == counts[1] && runs[0] && runs[1] && !memcmp(runs[0], runs[1], counts[0] * sizeof *runs[0]),
       "the formula block's runs are the code block's");
    for (int i = 0; i < 2; i++) {
        markdown_core_document_free(documents[i]);
    }
}

/* The committed opener and a direct decode have the same recognition work.
 * A probe owns a separate lifetime and must free its memo on every outcome. */
static void directive_recognition_survives_construction(test_batch_runner *runner) {
    static const char *const attributes[] = {"{}", "{.a k=1 k=2}", "{key=\"a b\" flag .c}", "{x=a=b=c}"};
    for (size_t i = 0; i < sizeof(attributes) / sizeof(*attributes); i++) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&source, "::: ");
        markdown_core_strbuf_puts(&source, attributes[i]);
        markdown_core_strbuf_puts(&source, "\nbody\n:::\n");
        markdown_core_attribute_scratch scratch = {0};
        markdown_core_attribute_parser recognized = {.data = source.ptr, .length = source.size, .scratch = &scratch};
        markdown_core_attributes value = {0};
        bufsize_t end;
        OK(runner, markdown_core_attributes_end(&recognized, 4) != 0, "the envelope is recognized");
        OK(runner, markdown_core_attributes_parse(&recognized, 4, &value, &end), "the same memo decodes the value");
        inline_work work = {0};
        markdown_core_node *root =
            markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
        OK(runner, root != NULL, "the block owns the decoded attributes");
        INT_EQ(runner, work.attributes, recognized.work,
               "construction reuses recognition instead of scanning the attribute extent again");
        markdown_core_node_free(root);
        markdown_core_attributes_free(&value);
        markdown_core_attribute_parser_free(&recognized);
        markdown_core_attribute_scratch_free(&scratch);
        markdown_core_strbuf_free(&source);
    }
}

/* A document counts the columns of its scope queries in the unit it was
 * parsed in; positions stay UTF-8 bytes underneath, so both units name the
 * same nodes. */
static void scope_queries_count_in_the_document_unit(test_batch_runner *runner) {
    static const char source[] = "\xc3\xa9\xf0\x9f\x9a\x80 x\r\nb\n";
    const size_t length = sizeof(source) - 1;
    static const struct {
        markdown_core_text_unit unit;
        int32_t text_end, rocket, after;
    } units[] = {{MARKDOWN_CORE_TEXT_UNIT_UTF8, 8, 3, 7}, {MARKDOWN_CORE_TEXT_UNIT_UTF16, 5, 2, 4}};
    for (size_t u = 0; u < sizeof(units) / sizeof(*units); u++) {
        markdown_core_document *document = facade_parse_in((const uint8_t *)source, length, units[u].unit);
        OK(runner, document && markdown_core_document_unit(document) == units[u].unit, "unit=%zu parses", u);
        if (!document) {
            continue;
        }
        const markdown_core_node *paragraph = facade_child(markdown_core_document_root(document), 0);
        const markdown_core_node *text = facade_child(paragraph, 0);
        markdown_core_scope scope = document_scope_of(document, text, source);
        OK(runner,
           scope.start.line == 1 && scope.start.column == 1 && scope.end.line == 1 &&
               scope.end.column == units[u].text_end,
           "unit=%zu the first line's text ends at column %d: %d:%d..%d:%d", u, units[u].text_end, scope.start.line,
           scope.start.column, scope.end.line, scope.end.column);
        scope = document_scope_of(document, paragraph, source);
        OK(runner, scope.end.line == 2 && scope.end.column == 1, "unit=%zu a CR LF ends one line", u);
        OK(runner,
           facade_node_at(document, (markdown_core_position){1, units[u].rocket}, (const uint8_t *)source, length) ==
                   text &&
               facade_node_at(document, (markdown_core_position){1, units[u].rocket + 1}, (const uint8_t *)source,
                              length) == NULL &&
               facade_node_at(document, (markdown_core_position){1, units[u].after}, (const uint8_t *)source, length) ==
                   text,
           "unit=%zu a position names a scalar's first unit and no other", u);
        markdown_core_document_free(document);
    }
    static const char *const empty[] = {"", "\n"};
    for (size_t i = 0; i < sizeof(empty) / sizeof(*empty); i++) {
        markdown_core_document *document =
            facade_parse_in((const uint8_t *)empty[i], strlen(empty[i]), MARKDOWN_CORE_TEXT_UNIT_UTF16);
        markdown_core_scope scope = {{0, 0}, {0, 0}};
        if (document) {
            scope = document_scope_of(document, markdown_core_document_root(document), empty[i]);
        }
        OK(runner,
           document && scope.start.line == 1 && scope.start.column == 1 && scope.end.line == 1 && scope.end.column == 0,
           "an empty document %zu is 1:1..1:0: %d:%d..%d:%d", i, scope.start.line, scope.start.column, scope.end.line,
           scope.end.column);
        markdown_core_document_free(document);
    }
}

/* A dump with every ` scope=L:C..L:C` removed, in place; its new length. */
static size_t without_scopes(uint8_t *dump, size_t length) {
    size_t kept = 0;
    static const char mark[] = " scope=";
    for (size_t i = 0; dump && i < length;) {
        if (i + sizeof(mark) - 1 <= length && !memcmp(dump + i, mark, sizeof(mark) - 1)) {
            i += sizeof(mark) - 1;
            while (i < length && dump[i] != ' ' && dump[i] != '\n') {
                i++;
            }
            continue;
        }
        dump[kept++] = dump[i++];
    }
    return kept;
}

/* Whether the node holding each NUL byte of `source` is Text whose literal
 * holds the replacement scalar. */
static bool nul_bytes_are_replacement_text(const markdown_core_document *document, const markdown_core_strbuf *source) {
    markdown_core_position position = {1, 1};
    for (size_t i = 0; i < (size_t)source->size; i++) {
        uint8_t byte = source->ptr[i];
        if (!byte) {
            const markdown_core_node *node = facade_node_at(document, position, source->ptr, (size_t)source->size);
            bool replaced = false;
            if (!node || node->kind != MARKDOWN_CORE_NODE_TEXT) {
                return false;
            }
            markdown_core_string literal = facade_literal(node);
            for (size_t at = 0; at + 3 <= literal.length && !replaced; at++) {
                replaced = !memcmp(literal.data + at, "\xef\xbf\xbd", 3);
            }
            if (!replaced) {
                return false;
            }
        }
        if (byte == '\n' || (byte == '\r' && !(i + 1 < (size_t)source->size && source->ptr[i + 1] == '\n'))) {
            position.line++;
            position.column = 1;
        } else {
            position.column++;
        }
    }
    return true;
}

/* NUL is one scalar represented as U+FFFD before grammar sees a line. Driver,
 * lookahead, inline cells and queued block cells must see identical bytes, so
 * a NUL and its replacement produce the same values; positions stay source
 * bytes, so the one NUL byte is where the replacement's text is. Views remain
 * stable while another line's view is allocated and while the line index
 * grows. */
static void lookahead_and_driver_share_normalized_lines(test_batch_runner *runner) {
    payload_probe_arm();
    static const char *const forms[] = {
        "| @ | h |\n| - | - |\n| a | @ |\n",
        "h   j\n--- ---\n@   w\n--- ---\n",
        "-------\nh   j\n--- ---\n@   w\n\nx   @\n-------\n",
        "+---+---+\n| @ | b |\n+---+---+\n| c | @ |\n+---+---+\n",
        /* Minimized mapping assertion: expansion used to happen only after
         * these lookahead bytes had already acquired cell source runs. */
        (" -  -  -  -  -\n Stage      Description    @                          Ir/B\n"
         "          repeated to the target size.\n\n-----------------------------------------\n"),
    };
    static const char *const endings[] = {"\n", "\r", "\r\n"};
    for (size_t f = 0; f < sizeof(forms) / sizeof(*forms); f++) {
        for (size_t e = 0; e < sizeof(endings) / sizeof(*endings); e++) {
            for (int quoted = 0; quoted < 2; quoted++) {
                markdown_core_strbuf input = MARKDOWN_CORE_BUF_INIT(), control = MARKDOWN_CORE_BUF_INIT();
                if (quoted) {
                    markdown_core_strbuf_puts(&input, "> ");
                    markdown_core_strbuf_puts(&control, "> ");
                }
                for (const char *at = forms[f]; *at; at++) {
                    if (*at == '@') {
                        markdown_core_strbuf_putc(&input, 0);
                        markdown_core_strbuf_puts(&control, "\xef\xbf\xbd");
                    } else if (*at == '\n') {
                        markdown_core_strbuf_puts(&input, endings[e]);
                        markdown_core_strbuf_puts(&control, endings[e]);
                        if (quoted && at[1]) {
                            markdown_core_strbuf_puts(&input, "> ");
                            markdown_core_strbuf_puts(&control, "> ");
                        }
                    } else {
                        markdown_core_strbuf_putc(&input, *at);
                        markdown_core_strbuf_putc(&control, *at);
                    }
                }
                uint8_t *dumps[2] = {NULL, NULL};
                size_t lengths[2] = {0, 0};
                markdown_core_strbuf *sources[] = {&input, &control};
                for (size_t i = 0; i < 2; i++) {
                    markdown_core_document *document = facade_parse(sources[i]->ptr, sources[i]->size);
                    OK(runner, document != NULL, "normalized table parses: %zu/%zu/%d/%zu", f, e, quoted, i);
                    if (document) {
                        OK(runner,
                           (markdown_core_document_dump(document, markdown_core_document_root(document),
                                                        (const uint8_t *)sources[i]->ptr, sources[i]->size, &dumps[i],
                                                        &lengths[i]) == MARKDOWN_CORE_OK),
                           "normalized table has a complete canonical dump");
                        if (i == 0) {
                            OK(runner, nul_bytes_are_replacement_text(document, &input),
                               "each NUL byte is the source of its replacement's text: %zu/%zu/%d", f, e, quoted);
                        }
                        markdown_core_document_free(document);
                    }
                }
                lengths[0] = without_scopes(dumps[0], lengths[0]);
                lengths[1] = without_scopes(dumps[1], lengths[1]);
                OK(runner, dumps[0] && dumps[1] && lengths[0] == lengths[1] && !memcmp(dumps[0], dumps[1], lengths[0]),
                   "NUL and its normalized scalar produce identical table values: %zu/%zu/%d", f, e, quoted);
                markdown_core_dump_free(dumps[0]);
                markdown_core_dump_free(dumps[1]);
                markdown_core_strbuf_free(&input);
                markdown_core_strbuf_free(&control);
            }
        }
    }
    INT_EQ(runner, payload_live, 0, "all immutable line views end with their input transaction");
    payload_probe_disarm();
}

/* Definition list is the other element whose opening grammar spans two lines:
 * a TERM is arbitrary prose, so nothing about the term's own line can rule the
 * grammar out, and the opener used to open a full lookahead transaction on
 * every line just to read the next one and find out.
 *
 * With both two-line gates in place, a document that holds neither a table
 * separator nor a definition marker must pull NO lookahead line at all.
 * `block_lookahead_work` counts lines pulled, so zero is the whole claim.
 *
 * The gate reads raw source, and a bare CR ends a line here. Getting the line
 * end wrong does not LOSE a definition list -- the gate only over-admits --
 * it silently runs the search to end of input, so one marker anywhere makes
 * every line look possible again. The CR case below is what makes that a
 * failure rather than a corpus blind spot. */
static void definition_open_gate_admits_only_possible_terms(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    for (size_t i = 0; i < 256; i++) {
        markdown_core_strbuf_puts(&source, "an ordinary prose line with no marker on it\n\n");
    }
    inline_work work = {0};
    markdown_core_node *root =
        markdown_core_parse_document_with_setup((char *)source.ptr, source.size, measure_inline_work, &work);
    OK(runner, root != NULL, "marker-free prose document parses");
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_DEFINITION_LIST), 0, "and produces no definition list");
    INT_EQ(runner, work.lookahead, 0, "neither two-line opener pulls a line where its grammar cannot open");
    markdown_core_node_free(root);
    markdown_core_strbuf_free(&source);

    /* CR-only prose with one marker at the very end. Reading the line with
     * `memchr` for '\n' makes every line find that marker. */
    markdown_core_strbuf cr = MARKDOWN_CORE_BUF_INIT();
    for (size_t i = 0; i < 256; i++) {
        markdown_core_strbuf_puts(&cr, "an ordinary prose line with no marker on it\r\r");
    }
    markdown_core_strbuf_puts(&cr, "term\r: body\r");
    inline_work cr_work = {0};
    markdown_core_node *cr_root =
        markdown_core_parse_document_with_setup((char *)cr.ptr, cr.size, measure_inline_work, &cr_work);
    OK(runner, cr_root != NULL, "CR-only prose document parses");
    INT_EQ(runner, count_kind(cr_root, MARKDOWN_CORE_NODE_DEFINITION_LIST), 1,
           "the CR-terminated definition list at the end is still found");
    OK(runner, cr_work.lookahead <= 8, "a CR line ends the line the gate reads: %zu pulls", cr_work.lookahead);
    markdown_core_node_free(cr_root);
    markdown_core_strbuf_free(&cr);

    /* A marker one line down, and one after a blank, are what the gate must
     * not miss: the transaction skips at most one blank line. */
    static const char *const shapes[] = {"term\n: body\n", "term\n\n: body\n"};
    for (size_t i = 0; i < sizeof(shapes) / sizeof(*shapes); i++) {
        markdown_core_node *built = markdown_core_parse_document_with_setup(shapes[i], strlen(shapes[i]), NULL, NULL);
        OK(runner, built != NULL, "definition shape %zu parses", i);
        INT_EQ(runner, count_kind(built, MARKDOWN_CORE_NODE_DEFINITION_LIST), 1,
               "the gate admits a definition list with %zu blank lines before its marker", i);
        markdown_core_node_free(built);
    }
}

/* The mailto links of a root and of every owned field root under it: the
 * public iterator walks children only, and a definition term, a callout
 * title, a citation affix or a table caption is a root of its own. */
typedef struct {
    size_t mailto, merged, after_link;
} mailto_census;

static void census_mailto_links(markdown_core_node *root, mailto_census *census);

static int census_owned_root(markdown_core_node **slot, void *context) {
    if (slot && *slot) {
        census_mailto_links(*slot, context);
    }
    return 1;
}

static void census_mailto_links(markdown_core_node *root, mailto_census *census) {
    tree_iter *iter = tree_iter_new(root);
    markdown_core_event_type event;
    bool seen_link = false;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        markdown_core_visit_inline_subtrees(node, census_owned_root, census);
        if (node->kind != MARKDOWN_CORE_NODE_LINK || !node->as.link->resource) {
            continue;
        }
        const char *url = (const char *)node->as.link->resource->url.data;
        if (strncmp(url, "mailto:", 7) != 0) {
            seen_link = true;
            continue;
        }
        census->mailto++;
        census->merged += strcmp(url, "mailto:x.y@z.com") == 0;
        census->after_link += seen_link && strcmp(url, "mailto:q@r.st") == 0;
    }
    tree_iter_free(iter);
}

static markdown_core_node *first_of_kind(markdown_core_node *root, markdown_core_node_type kind) {
    tree_iter *iter = tree_iter_new(root);
    markdown_core_node *found = NULL;
    markdown_core_event_type event;
    while (!found && (event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event == MARKDOWN_CORE_EVENT_ENTER && node->kind == kind) {
            found = node;
        }
    }
    tree_iter_free(iter);
    return found;
}

/* The byte a scope coordinate names in `source`: the column-th byte of the
 * line-th line, both counted from 1. -1 when no byte is there. */
static int source_byte_at(const char *source, int line, int column) {
    const char *text = source;
    for (int l = 1; l < line; l++) {
        text = strchr(text, '\n');
        if (!text) {
            return -1;
        }
        text++;
    }
    size_t length = strcspn(text, "\n");
    return column >= 1 && (size_t)column <= length ? (unsigned char)text[column - 1] : -1;
}

static bool scope_within(const markdown_core_node *inner, const markdown_core_node *outer) {
    return (START_LINE(inner) > START_LINE(outer) ||
            (START_LINE(inner) == START_LINE(outer) && START_COLUMN(inner) >= START_COLUMN(outer))) &&
           (END_LINE(inner) < END_LINE(outer) ||
            (END_LINE(inner) == END_LINE(outer) && END_COLUMN(inner) <= END_COLUMN(outer)));
}

/* Every node under `label` lies between its brackets: from the byte after the
 * `[` to the byte before the `]`. */
static bool label_holds_its_content(markdown_core_node *label) {
    tree_iter *iter = tree_iter_new(label);
    markdown_core_event_type event;
    bool held = true;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event != MARKDOWN_CORE_EVENT_ENTER || node == label) {
            continue;
        }
        held &= (START_LINE(node) > START_LINE(label) ||
                 (START_LINE(node) == START_LINE(label) && START_COLUMN(node) > START_COLUMN(label))) &&
                (END_LINE(node) < END_LINE(label) ||
                 (END_LINE(node) == END_LINE(label) && END_COLUMN(node) < END_COLUMN(label)));
    }
    tree_iter_free(iter);
    return held;
}

/* A DIRECTIVE'S SCOPES ARE THE EDITOR POSITIONS OF ITS OWN BYTES, wherever it
 * sits. A label starts on its `[` and ends on its `]`, its content starts on
 * the byte after the `[`, and everything in it lies between the brackets. An
 * inline directive starts on its `:` and ends on its last byte: the `}` of
 * its attributes, or else its label's `]`. The parser reads a table cell's
 * content as an input of its own, and a byte's index in that input is not its
 * column in the editor: a tab there is as many spaces as it reaches, and at a
 * cell's edge the byte after a directive is the cell's line ending, placed at
 * the end of the row. So every endpoint is the position of the byte it names,
 * never a column counted from another. Labels nest, so each label's own
 * directives are checked too. */
static void directive_scopes_are_editor_positions(test_batch_runner *runner) {
    static const char *const sources[] = {
        /* Block directives after indentation and container prefixes. */
        ":::note[a]\n:::\n",
        ":::note[]\n:::\n",
        "   ::note[a]\n",
        ">\t::note[ab]\n",
        "> :::note[label]{k=v}\n> :::\n",
        "1. ::x[*e*]\n",
        "  1. >  ::n[]\n",
        "> > :::n[\xE4\xB8\xAD]\n",
        /* Inline directives, with a label over a line ending, and in a pipe
         * cell. */
        "text :n[a] more\n",
        "> - x :n[lbl]\n",
        "> x :m[a\n> b]{.c}\n",
        "| :m[ab] | y |\n| - | - |\n| a | b |\n",
        /* A tab inside a label is one source byte. */
        "> ::n[a\t*b*]\n",
        "| x :m[a\tb] | y |\n| - | - |\n| a | b |\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const char *source = sources[i];
        const int shown = (int)strcspn(source, "\n");
        markdown_core_node *doc = parse(source);
        markdown_core_node *roots[16] = {doc};
        size_t root_count = 1, directives = 0;
        while (root_count) {
            tree_iter *iter = tree_iter_new(roots[--root_count]);
            markdown_core_event_type event;
            while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
                markdown_core_node *node = tree_iter_get_node(iter);
                if (event != MARKDOWN_CORE_EVENT_ENTER ||
                    (node->kind != MARKDOWN_CORE_NODE_DIRECTIVE && node->kind != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)) {
                    continue;
                }
                directives++;
                markdown_core_node *parent = tree_iter_parent(iter);
                OK(runner, !parent || scope_within(node, parent), "a directive in %.*s lies in its parent", shown,
                   source);
                markdown_core_node *label = markdown_core_directive_label(node);
                if (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE) {
                    /* Attributes follow the label's `]` directly, or stand
                     * alone; the directive ends on the last of these. */
                    bool attributed = !label || source_byte_at(source, END_LINE(label), END_COLUMN(label) + 1) == '{';
                    int last = source_byte_at(source, END_LINE(node), END_COLUMN(node));
                    OK(runner, source_byte_at(source, START_LINE(node), START_COLUMN(node)) == ':',
                       "an inline directive in %.*s starts on its colon", shown, source);
                    OK(runner,
                       attributed ? last == '}'
                                  : END_LINE(node) == END_LINE(label) && END_COLUMN(node) == END_COLUMN(label),
                       "an inline directive in %.*s ends on its last byte", shown, source);
                }
                if (!label) {
                    continue;
                }
                markdown_core_node *content = child_at(label, 0);
                OK(runner,
                   source_byte_at(source, START_LINE(label), START_COLUMN(label)) == '[' &&
                       source_byte_at(source, END_LINE(label), END_COLUMN(label)) == ']',
                   "a label in %.*s starts on its [ and ends on its ]", shown, source);
                OK(runner, scope_within(label, node), "a label in %.*s lies in its directive", shown, source);
                OK(runner,
                   !content ||
                       (START_LINE(content) == START_LINE(label) && START_COLUMN(content) == START_COLUMN(label) + 1),
                   "a label's content in %.*s starts on the byte after its [", shown, source);
                OK(runner, label_holds_its_content(label), "a label's content in %.*s lies between its brackets", shown,
                   source);
                if (root_count < sizeof(roots) / sizeof(roots[0])) {
                    roots[root_count++] = label;
                }
            }
            tree_iter_free(iter);
        }
        OK(runner, directives > 0, "%.*s holds a directive", shown, source);
        markdown_core_node_free(doc);
    }
}

/* A PARAGRAPH STARTS WHERE ITS TEXT DOES. Its first line's indentation is
 * not content, whether the line opened the paragraph with every container's
 * prefix or lazily without them: the paragraph, and the text it starts with,
 * start on the line's first non-space byte, and the literal starts with that
 * byte. A lazy line continues an open paragraph, except after a callout's
 * marker line, whose text is the title: there it opens the body's first
 * paragraph. The sources give lazy lines to both, under quotes, callouts,
 * list items and a grid cell, after spaces and a tab. */
static void paragraphs_start_on_their_first_byte(test_batch_runner *runner) {
    static const char *const sources[] = {
        "> [!note]\n   lazy\n",
        "> [!note] T\n  lazy\nmore\n",
        "- > [!note]\n \tlazy\n",
        "> > [!tip]\n  lazy\n",
        "- > [!note] T\n     lazy\n",
        "1. > [!note]\n   lazy\n",
        "+-------------+\n| > [!note]   |\n|   lazy      |\n+-------------+\n",
        "> quote\n   lazy\n",
        "> [!note]\n>    body\n",
        "   plain\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const char *source = sources[i];
        const int shown = (int)strcspn(source, "\n");
        markdown_core_node *doc = parse(source);
        tree_iter *iter = tree_iter_new(doc);
        markdown_core_event_type event;
        size_t paragraphs = 0;
        while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            markdown_core_node *node = tree_iter_get_node(iter);
            if (event != MARKDOWN_CORE_EVENT_ENTER || node->kind != MARKDOWN_CORE_NODE_PARAGRAPH) {
                continue;
            }
            paragraphs++;
            const int first = source_byte_at(source, START_LINE(node), START_COLUMN(node));
            markdown_core_node *text = child_at(node, 0);
            markdown_core_string literal;
            OK(runner, first > 0 && first != ' ' && first != '\t', "a paragraph in %.*s starts on a non-space byte",
               shown, source);
            OK(runner,
               text && text->kind == MARKDOWN_CORE_NODE_TEXT && START_LINE(text) == START_LINE(node) &&
                   START_COLUMN(text) == START_COLUMN(node) && literal_of(text, &literal) && literal.length &&
                   literal.data[0] == first,
               "a paragraph in %.*s starts with its text", shown, source);
        }
        tree_iter_free(iter);
        OK(runner, paragraphs > 0, "%.*s holds a paragraph", shown, source);
        markdown_core_node_free(doc);
    }
}

/* The canonical dump of `source`, with every column on `line` other than the
 * sentinel moved left by `shift`. NULL when the parse or the dump fails. */
static char *dump_with_line_shifted(const char *source, int line, int shift) {
    markdown_core_document *document = facade_parse((const uint8_t *)source, strlen(source));
    uint8_t *dump = NULL;
    size_t length = 0;
    if (!document ||
        markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)source,
                                    strlen(source), &dump, &length) != MARKDOWN_CORE_OK) {
        markdown_core_document_free(document);
        return NULL;
    }
    markdown_core_document_free(document);
    markdown_core_strbuf out = MARKDOWN_CORE_BUF_INIT();
    for (const char *at = (const char *)dump; *at;) {
        const char *scope = strstr(at, "scope=");
        if (!scope) {
            markdown_core_strbuf_puts(&out, at);
            break;
        }
        markdown_core_strbuf_put(&out, (const unsigned char *)at, (bufsize_t)(scope - at + 6));
        at = scope + 6;
        for (int end = 0; end < 2; end++) {
            char *next;
            long l = strtol(at, &next, 10), c = strtol(next + 1, &next, 10);
            char place[48];
            snprintf(place, sizeof(place), "%s%ld:%ld", end ? ".." : "", l, l == line && c ? c - shift : c);
            markdown_core_strbuf_puts(&out, place);
            at = next + (end ? 0 : 2);
        }
    }
    markdown_core_dump_free(dump);
    return (char *)markdown_core_strbuf_detach(&out);
}

/* A LAZY LINE'S TEXT PARSES AS IT WOULD WITH THE QUOTE'S PREFIX. A lazy line
 * is text:
 * its indentation is not content, as it is not on a line that continues a
 * paragraph with every prefix. After a callout's marker line, which is its
 * title, the line opens the body's first paragraph -- with the dialect's one
 * text-block opener, which is what opens it when the prefix is there. After a
 * quote whose paragraph so far is a reference definition, it continues that
 * paragraph and becomes its first text once the definition is taken. Either
 * way, text reads the same as it does with the prefix, in node kinds,
 * literals, anchors, destinations and every other field: a definition
 * defines, a standalone formula is a formula block, a pipe row can head a
 * table, and a setext underline makes a heading. Only the line's own columns
 * differ, by the prefix's two bytes. A line that opens a block with the
 * prefix, such as indented code after a marker line, is no text there, and
 * `lazy_after_marker_as_after_paragraph` says when it is lazy without. */
static void lazy_lines_parse_as_prefixed(test_batch_runner *runner) {
    static const char *const heads[] = {"> [!note]\n", "> [a]: /x\n"};
    static const char *const indents[] = {"", " ", "  ", "   "};
    static const char *const bodies[] = {
        "lazy\n",           "*em* x\n", "`c` x\n",          "[l](/u) x\n",   "<b>x</b> y\n",
        "[x]: /u\n\n[x]\n", "$$x$$\n",  "| a |\n> |---|\n", "lazy\n> ===\n", "lazy\n> more\n",
    };
    for (size_t h = 0; h < sizeof(heads) / sizeof(heads[0]); h++) {
        for (size_t i = 0; i < sizeof(indents) / sizeof(indents[0]); i++) {
            for (size_t b = 0; b < sizeof(bodies) / sizeof(bodies[0]); b++) {
                char lazy[64], prefixed[64];
                snprintf(lazy, sizeof(lazy), "%s%s%s", heads[h], indents[i], bodies[b]);
                snprintf(prefixed, sizeof(prefixed), "%s> %s%s", heads[h], indents[i], bodies[b]);
                char *lazy_dump = dump_with_line_shifted(lazy, 2, 0);
                char *prefixed_dump = dump_with_line_shifted(prefixed, 2, 2);
                OK(runner, lazy_dump && prefixed_dump && !strcmp(lazy_dump, prefixed_dump),
                   "a lazy line after %.*s and %zu spaces parses as with the prefix: %.*s",
                   (int)strcspn(heads[h], "\n"), heads[h], strlen(indents[i]), (int)strcspn(bodies[b], "\n"),
                   bodies[b]);
                markdown_core_free(lazy_dump);
                markdown_core_free(prefixed_dump);
            }
        }
    }
}

/* The innermost callout of `doc`: the last one a walk enters. */
static markdown_core_node *innermost_callout(markdown_core_node *doc) {
    markdown_core_node *callout = NULL;
    tree_iter *iter = tree_iter_new(doc);
    markdown_core_event_type event;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event == MARKDOWN_CORE_EVENT_ENTER && node->kind == MARKDOWN_CORE_NODE_CALLOUT) {
            callout = node;
        }
    }
    tree_iter_free(iter);
    return callout;
}

/* Every node after `doc`'s first child, as kind, scope and literal. */
static char *describe_after_first(markdown_core_node *doc) {
    markdown_core_strbuf out = MARKDOWN_CORE_BUF_INIT();
    for (size_t at = 1; at < doc->child_count; at++) {
        markdown_core_node *top = doc->children[at];
        tree_iter *iter = tree_iter_new(top);
        markdown_core_event_type event;
        while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            markdown_core_node *node = tree_iter_get_node(iter);
            if (event != MARKDOWN_CORE_EVENT_ENTER) {
                continue;
            }
            char place[96];
            snprintf(place, sizeof(place), "%d %d:%d..%d:%d ", node->kind, START_LINE(node), START_COLUMN(node),
                     END_LINE(node), END_COLUMN(node));
            markdown_core_strbuf_puts(&out, place);
            put_literal(&out, node);
            markdown_core_strbuf_putc(&out, '\n');
        }
        tree_iter_free(iter);
    }
    return (char *)markdown_core_strbuf_detach(&out);
}

/* A CALLOUT'S MARKER LINE TAKES A LAZY LINE AS A PARAGRAPH DOES. A line that
 * misses the quote's prefix is lazy when it opens no block, and the line
 * after a callout's marker line is lazy exactly when it is after a
 * paragraph's line. The starts that refuse a lazy line -- indented code, an
 * HTML block of the seventh kind, a dash-led table, a paragraph hook such as
 * a definition list's term -- refuse it there too, and every other start that
 * opens after a paragraph's line opens there. So the callout keeps the line
 * exactly when a quote whose paragraph the line would continue keeps it, and
 * whatever follows is the same blocks at the same places, at any depth. */
static void lazy_after_marker_as_after_paragraph(test_batch_runner *runner) {
    static const char *const heads[][2] = {
        {"> [!note]\n", "> p\n"},
        {"> > [!note] T\n", "> > p\n"},
        {"- > [!note]\n", "- > p\n"},
    };
    static const char *const bodies[] = {
        "term\n\n: def\n",
        "term\n: def\n",
        ": def\n",
        "    code\n",
        "\tcode\n",
        "<div2 a=\"b\">\n",
        "<div>\n",
        "- item\n",
        "*\n",
        "2. x\n",
        "1. x\n",
        "(@) ex\n",
        "# h\n",
        "---\n",
        "```\nx\n```\n",
        "| a |\n| - |\n",
        "----  ----\na     b\n----  ----\n",
        "a  b\n-  -\n",
        "+---+\n| a |\n+---+\n",
        "Table: cap\n\n| a |\n| - |\n",
        "text\n",
        "[x]: /u\n",
        "$$\nx\n$$\n",
        ":::note\n:::\n",
        "[^n]: note\n",
        "%%\nc\n%%\n",
    };
    size_t kept = 0, left = 0;
    for (size_t h = 0; h < sizeof(heads) / sizeof(heads[0]); h++) {
        for (size_t b = 0; b < sizeof(bodies) / sizeof(bodies[0]); b++) {
            char after_marker[96], after_paragraph[96];
            snprintf(after_marker, sizeof(after_marker), "%s%s", heads[h][0], bodies[b]);
            snprintf(after_paragraph, sizeof(after_paragraph), "%s%s", heads[h][1], bodies[b]);
            markdown_core_node *marked = parse(after_marker), *prose = parse(after_paragraph);
            markdown_core_node *callout = marked ? innermost_callout(marked) : NULL,
                               *quote = prose ? innermost_callout(prose) : NULL;
            char *marked_rest = marked ? describe_after_first(marked) : NULL,
                 *prose_rest = prose ? describe_after_first(prose) : NULL;
            const bool marker_keeps = callout && END_LINE(callout) >= 2,
                       paragraph_keeps = quote && END_LINE(quote) >= 2;
            OK(runner,
               callout && quote && marker_keeps == paragraph_keeps && marked_rest && prose_rest &&
                   !strcmp(marked_rest, prose_rest),
               "after %.*s, %.*s parses as it does after a paragraph", (int)strcspn(heads[h][0], "\n"), heads[h][0],
               (int)strcspn(bodies[b], "\n"), bodies[b]);
            kept += marker_keeps;
            left += !marker_keeps;
            markdown_core_free(marked_rest);
            markdown_core_free(prose_rest);
            markdown_core_node_free(marked);
            markdown_core_node_free(prose);
        }
    }
    OK(runner, kept > 0 && left > 0, "some lines are lazy and some open blocks: %zu and %zu", kept, left);
}

/* `base` with `first` before its first line, `rest` before each later
 * nonblank line and `blank` on each blank one. */
static void prefix_lines(markdown_core_strbuf *out, const char *base, const char *first, const char *rest,
                         const char *blank) {
    for (const char *line = base; *line;) {
        size_t length = strcspn(line, "\n");
        markdown_core_strbuf_puts(out, length ? (line == base ? first : rest) : blank);
        markdown_core_strbuf_put(out, (const unsigned char *)line, (bufsize_t)length);
        markdown_core_strbuf_putc(out, '\n');
        line += length + (line[length] == '\n');
    }
}

/* One scope column of `moved`, where `base` has `column` and every line of
 * the table gained `shift` leading bytes. Column 0 is the line-ending
 * sentinel and names no byte, so it does not move. */
static int shifted_column(int column, int shift) { return column ? column + shift : 0; }

static bool same_columns(const markdown_core_table *a, const markdown_core_table *b) {
    if (a->column_count != b->column_count || a->head_count != b->head_count) {
        return false;
    }
    for (size_t i = 0; i < a->column_count; i++) {
        if (a->columns[i].flow != b->columns[i].flow) {
            return false;
        }
    }
    return true;
}

/* Walk two table trees in lockstep: the same kinds, text and columns, and
 * every scope moved by exactly `shift` bytes. Returns
 * the number of nodes that differ. */
static size_t shifted_table_differences(markdown_core_node *base, markdown_core_node *moved, int shift) {
    tree_iter *a = tree_iter_new(base), *b = tree_iter_new(moved);
    markdown_core_event_type event;
    size_t differences = 0;
    while ((event = tree_iter_next(a)) != MARKDOWN_CORE_EVENT_DONE) {
        if (tree_iter_next(b) != event) {
            differences++;
            break;
        }
        markdown_core_node *x = tree_iter_get_node(a), *y = tree_iter_get_node(b);
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        markdown_core_string lx, ly;
        bool hx = literal_of(x, &lx), hy = literal_of(y, &ly);
        bool same = x->kind == y->kind && START_LINE(x) == START_LINE(y) && END_LINE(x) == END_LINE(y) &&
                    START_COLUMN(y) == shifted_column(START_COLUMN(x), shift) &&
                    END_COLUMN(y) == shifted_column(END_COLUMN(x), shift) && hx == hy && (!hx || strings_equal(lx, ly));
        if (same && x->kind == MARKDOWN_CORE_NODE_TABLE) {
            same = same_columns(x->opaque, y->opaque);
        }
        differences += !same;
    }
    differences += tree_iter_next(b) != MARKDOWN_CORE_EVENT_DONE;
    tree_iter_free(a);
    tree_iter_free(b);
    return differences;
}

static bool table_nodes_nest(markdown_core_node *table) {
    tree_iter *iter = tree_iter_new(table);
    markdown_core_event_type event;
    bool nested = true;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        markdown_core_node *parent = tree_iter_parent(iter);
        if (event == MARKDOWN_CORE_EVENT_ENTER && parent) {
            nested &= scope_within(node, parent);
        }
    }
    tree_iter_free(iter);
    return nested;
}

/* A TABLE READS THE SAME WHEREVER ITS INDENTATION CAME FROM, BUT FOR WHERE ITS
 * TABS STOP. Up to three columns of block indentation never change a block's
 * meaning, and a container's prefix is not part of the block it holds. So
 * under every prefix below -- spaces, a list item, a quote, a quote whose tab
 * the prefix consumes in part -- each table must parse to the tree it has
 * unprefixed: the same column flows, cells and text, and
 * every scope in it moved by exactly the prefix's bytes, inside its container.
 * Before the margin model, alignment probed the first column at the
 * container's content, so indentation right- or center-aligned it; the table
 * and its rows began at the container's content, in the indentation or on a
 * tab the prefix shared; and tab stops counted from the container, not the
 * line. A table with a tab in its geometry keeps its tree only under a prefix
 * four columns wide, because a tab reaches the physical line's next stop. Each
 * table's first line sits at its margin: a prefix indents every line, and a
 * first line pushed to four columns is indented code, not a table. */
static void table_geometry_ignores_indentation(test_batch_runner *runner) {
    static const struct {
        const char *source;
        bool tab;
    } tables[] = {
        {"| a | b |\n| :- | -: |\n| c |\n", false},
        {"a | b\n--|--\nc | d\n", false},
        {"| a | :--: |\n| - | - |\n| *c* | `d` |\n\n", false},
        {"| a\t| b |\n| - | - |\n| c\t| d |\n", true},
    };
    static const struct {
        const char *first, *rest, *blank;
        int columns;
    } prefixes[] = {
        {" ", " ", "", 1},        {"  ", "  ", "", 2},       {"   ", "   ", "", 3},
        {"* ", "  ", "", 2},      {"1. ", "   ", "", 3},     {"> ", "> ", ">", 2},
        {">\t", ">\t", ">", 4},   {" >\t", " >\t", " >", 4}, {"   > ", "   > ", "   >", 5},
        {"*   ", "    ", "", 4},  {"> * ", ">   ", ">", 4},  {">  ", ">  ", ">", 3},
        {">\t ", ">\t ", ">", 5}, {"1.  ", "    ", "", 4},
    };
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        const char *source = tables[t].source;
        const int shown = (int)strcspn(source, "\n");
        markdown_core_node *base_doc = parse(source);
        markdown_core_node *base = first_of_kind(base_doc, MARKDOWN_CORE_NODE_TABLE);
        OK(runner, base != NULL, "%.*s is a table", shown, source);
        for (size_t p = 0; base && p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
            if (tables[t].tab && prefixes[p].columns % 4) {
                continue;
            }
            markdown_core_strbuf moved_source = MARKDOWN_CORE_BUF_INIT();
            prefix_lines(&moved_source, source, prefixes[p].first, prefixes[p].rest, prefixes[p].blank);
            markdown_core_node *moved_doc = parse((const char *)moved_source.ptr);
            markdown_core_node *moved = first_of_kind(moved_doc, MARKDOWN_CORE_NODE_TABLE);
            OK(runner, moved && !shifted_table_differences(base, moved, (int)strlen(prefixes[p].first)),
               "%.*s under %s reads as it does unprefixed, every scope moved by the prefix", shown, source,
               prefixes[p].first);
            OK(runner, moved && table_nodes_nest(moved) && scope_within(moved, parent_in(moved_doc, moved)),
               "%.*s under %s nests in its container", shown, source, prefixes[p].first);
            markdown_core_node_free(moved_doc);
            markdown_core_strbuf_free(&moved_source);
        }
        markdown_core_node_free(base_doc);
    }
}

/* The row of `doc` that starts on `line`, as its cells and every node in
 * them: kind, literal, and scope, with lines counted from the row's. */
static char *describe_row_on(markdown_core_node *doc, int line) {
    markdown_core_node *row = NULL;
    tree_iter *iter = tree_iter_new(doc);
    markdown_core_event_type event;
    while (!row && (event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event == MARKDOWN_CORE_EVENT_ENTER && node->kind == MARKDOWN_CORE_NODE_TABLE_ROW &&
            START_LINE(node) == line) {
            row = node;
        }
    }
    tree_iter_free(iter);
    if (!row) {
        return NULL;
    }
    markdown_core_strbuf out = MARKDOWN_CORE_BUF_INIT();
    iter = tree_iter_new(row);
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = tree_iter_get_node(iter);
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        char place[96];
        snprintf(place, sizeof(place), "%d %d:%d..%d:%d ", node->kind, START_LINE(node) - line, START_COLUMN(node),
                 END_LINE(node) - line, END_COLUMN(node));
        markdown_core_strbuf_puts(&out, place);
        put_literal(&out, node);
        markdown_core_strbuf_putc(&out, '\n');
    }
    tree_iter_free(iter);
    return (char *)markdown_core_strbuf_detach(&out);
}

/* A PIPE ROW IS ONE ROW WHEREVER IT IS FOUND. Pipes delimit its cells, and
 * each cell holds its bytes: trimmed of the whitespace around them, its
 * escaped pipes contracted, a tab kept a tab. So the row's cells, and every
 * node in them, are the same whether it heads a table after a paragraph,
 * heads one after a blank line, or is a body row -- in their literals
 * and in their scopes -- under any container prefix. */
static void pipe_rows_are_one_row(test_batch_runner *runner) {
    static const char *const rows[] = {
        "| `a\tb` | c\\|d |",         "|\ta\t|\tb |", "| a | |", "a\t| b", "| *x*\t**y** | `\\|` |",
        "| \xc3\xa9\t\xc3\xbc | x |",
    };
    static const char *const prefixes[][2] = {{"", ""}, {"  ", "  "}, {"> ", "> "}, {"- ", "  "}};
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        const char *first = prefixes[p][0], *rest = prefixes[p][1];
        for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
            char headed[160], parted[160], body[160];
            snprintf(headed, sizeof(headed), "%s%s\n%s| - | - |\n", first, rows[r], rest);
            snprintf(parted, sizeof(parted), "%sprose\n%s\n%s%s\n%s| - | - |\n", first, rest, rest, rows[r], rest);
            snprintf(body, sizeof(body), "%s| x | y |\n%s| - | - |\n%s%s\n", first, rest, rest, rows[r]);
            markdown_core_node *headed_doc = parse(headed), *parted_doc = parse(parted), *body_doc = parse(body);
            char *after_paragraph = describe_row_on(headed_doc, 1), *after_blank = describe_row_on(parted_doc, 3),
                 *in_body = describe_row_on(body_doc, 3);
            OK(runner,
               after_paragraph && after_blank && in_body && !strcmp(after_blank, after_paragraph) &&
                   !strcmp(in_body, after_paragraph),
               "%s%s is one row as a header, after a blank line and in the body", first, rows[r]);
            markdown_core_free(after_paragraph);
            markdown_core_free(after_blank);
            markdown_core_free(in_body);
            markdown_core_node_free(headed_doc);
            markdown_core_node_free(parted_doc);
            markdown_core_node_free(body_doc);
        }
    }
}

static int census_owned_nodes(markdown_core_node **slot, void *context) {
    if (slot && *slot) {
        *(size_t *)context += owned_node_census(*slot);
    }
    return 1;
}

static size_t owned_node_census(markdown_core_node *root) {
    tree_iter *iter = tree_iter_new(root);
    markdown_core_event_type event;
    size_t count = 0;
    while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (event == MARKDOWN_CORE_EVENT_ENTER) {
            markdown_core_node *node = tree_iter_get_node(iter);
            count++;
            markdown_core_visit_inline_subtrees(node, census_owned_nodes, &count);
        }
    }
    tree_iter_free(iter);
    return count;
}

/* An escaped space a Text beside it absorbs is completed (into NBSP) before
 * consolidation copies it, once, at its own word depth. */
static void absorbed_text_completes_once(test_batch_runner *runner) {
    const char *source = "^a\\ b\\ c^ d\\*e\n";
    markdown_core_node *root = markdown_core_parse_document(source, strlen(source));
    OK(runner, root != NULL, "normal and absorbed text completion share a successful parse");
    if (root) {
        size_t uncompleted = 0, spaces = 0;
        tree_iter *iter = tree_iter_new(root);
        markdown_core_event_type event;
        while ((event = tree_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            const markdown_core_node *node = tree_iter_get_node(iter);
            if (event != MARKDOWN_CORE_EVENT_ENTER || node->kind != MARKDOWN_CORE_NODE_TEXT) {
                continue;
            }
            uncompleted += (node->flags & MARKDOWN_CORE_NODE__ESCAPED_SPACE) != 0;
            const markdown_core_chunk *text = node->as.literal;
            for (bufsize_t i = 0; i + 1 < text->len; i++) {
                spaces += text->data[i] == 0xc2 && text->data[i + 1] == 0xa0;
            }
        }
        tree_iter_free(iter);
        INT_EQ(runner, uncompleted, 0, "no Text leaves completion with an escaped space pending");
        INT_EQ(runner, spaces, 2, "both absorbed escaped spaces complete exactly once at their word depth");
        markdown_core_node *script = child_at(child_at(root, 0), 0);
        INT_EQ(runner, script->kind, MARKDOWN_CORE_NODE_SUPERSCRIPT, "the word owner is preserved");
        LITERAL_EQ(runner, child_at(script, 0),
                   "a\xc2\xa0"
                   "b\xc2\xa0"
                   "c",
                   "consolidation copies already completed operands");
    }
    markdown_core_node_free(root);
}

/* EVERY FINISH HOOK ACTS ON EVERY ROOT. Completion makes each settled
 * block's inline content and runs the finish steps over it, on the block's
 * own content and on every field root it owns, so the document is chosen so
 * that every step actually acts, and on nested owned roots: emails inside a
 * definition term, a callout title, a citation affix, a table caption, a
 * footnote body and an inline footnote; escapes that leave adjacent Text
 * nodes for consolidation to merge before autolink sees them (`x\.y@z.com`
 * must come out as ONE address); a standalone `$$` block and a `formula`
 * fence, which completion promotes; a Link inside a field root
 * followed by an address outside it, which a per-walk rather than per-root
 * "inside a link" fact would silently swallow; and an escaped space inside a
 * superscript, which completion turns into NBSP and which must therefore be
 * completed before the Text beside it absorbs it. */
static void completion_acts_on_every_root(test_batch_runner *runner) {
    static const char source[] = "Term a\\.b@c.io $x$\n"
                                 ": body with x\\.y@z.com and $$y$$\n"
                                 "\n"
                                 "$$\n"
                                 "block\n"
                                 "$$\n"
                                 "\n"
                                 "```formula\n"
                                 "E=mc^2\n"
                                 "```\n"
                                 "\n"
                                 "> [!note] [in a link](u) title q@r.st\n"
                                 "> body s@t.uv\n"
                                 "\n"
                                 "See [pre a@b.co @doe99 post c@d.ef] and a note[^n] here^[inline u@v.wx] ^a\\ b^.\n"
                                 "\n"
                                 "[^n]: body t@u.vw\n"
                                 "\n"
                                 "| a |\n"
                                 "| - |\n"
                                 "| b |\n"
                                 "\n"
                                 "Table: cap e@f.gh\n";
    markdown_core_node *root = markdown_core_parse_document(source, sizeof(source) - 1);
    OK(runner, root != NULL, "the document that feeds every finish hook parses");
    if (!root) {
        return;
    }
    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_FORMULA_BLOCK), 2,
           "the formula block and the fence completion promoted");
    /* Every address became a link -- seven of them, four on field roots the
     * public iterator does not descend into -- including the one
     * consolidation had to assemble first and the one after a Link. */
    mailto_census census = {0};
    census_mailto_links(root, &census);
    INT_EQ(runner, (int)census.mailto, 9, "autolink's step linked every address on every root");
    /* The escaped space is its own Text until consolidation; completion made
     * it NBSP before the Text before it absorbed it, so the superscript holds
     * ONE Text reading `a b` with the NBSP between. */
    markdown_core_node *superscript = first_of_kind(root, MARKDOWN_CORE_NODE_SUPERSCRIPT);
    OK(runner,
       superscript && child_at(superscript, 0) && !child_at(superscript, 1) &&
           child_at(superscript, 0)->kind == MARKDOWN_CORE_NODE_TEXT &&
           child_at(superscript, 0)->as.literal->len == 4 &&
           memcmp(child_at(superscript, 0)->as.literal->data,
                  "a\xC2\xA0"
                  "b",
                  4) == 0,
       "an absorbed escaped space was completed to NBSP before it was merged");
    INT_EQ(runner, (int)census.merged, 1, "the escape-split address was consolidated before autolink scanned it");
    INT_EQ(runner, (int)census.after_link, 1,
           "the address after a Link on a field root is linked: the state is per root");
    markdown_core_node_free(root);
}

/* The shape that defeats a per-hook walk: depth and width vary independently
 * -- `deep_inline_construction`'s generator, Span or Embedded nesting around
 * a run of addresses that autolink's step links -- and every address is
 * linked whatever they are. */
static void completion_links_at_any_depth(test_batch_runner *runner) {
    const size_t sizes[] = {1, 64, 1024, 8192};
    for (size_t shape = 0; shape < 2; shape++) {
        for (size_t d = 0; d < 4; d++) {
            for (size_t w = 0; w < 4; w += 3) {
                markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
                for (size_t i = 0; i < sizes[d]; i++) {
                    markdown_core_strbuf_puts(&source, shape ? "[" : "![");
                }
                for (size_t i = 0; i < sizes[w]; i++) {
                    markdown_core_strbuf_puts(&source, "a@b.co ");
                }
                for (size_t i = 0; i < sizes[d]; i++) {
                    markdown_core_strbuf_puts(&source, shape ? "]{}" : "](u)");
                }
                markdown_core_strbuf_putc(&source, '\n');
                markdown_core_node *root = markdown_core_parse_document((const char *)source.ptr, source.size);
                OK(runner, root != NULL, "shape %zu depth %zu width %zu parses", shape, sizes[d], sizes[w]);
                if (root) {
                    INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_LINK), sizes[w],
                           "autolink's step linked every address at depth %zu", sizes[d]);
                    markdown_core_node_free(root);
                }
                markdown_core_strbuf_free(&source);
            }
        }
    }
}

/* A descriptor whose finish step could never keep its contract is refused at
 * attachment, with the dialect untouched, so the parse that tried to attach
 * it reports the refusal rather than running a step that is asked where it
 * can find nothing. */
static markdown_core_finish_result no_op_finish_step(const markdown_core_element_instance *self,
                                                     markdown_core_parser *parser, markdown_core_node *node,
                                                     markdown_core_node *parent, markdown_core_event_type event,
                                                     void **state) {
    (void)self;
    (void)parser;
    (void)node;
    (void)event;
    (void)parent;
    (void)state;
    return MARKDOWN_CORE_FINISH_CONTINUE;
}

typedef struct {
    size_t before, after;
    int attached;
} attach_probe;

static const markdown_core_node_type STEP_AT_TEXT[] = {MARKDOWN_CORE_NODE_TEXT, MARKDOWN_CORE_NODE_NONE};

static bool attach_block_scope_step(markdown_core_dialect_builder *builder, void *context) {
    static const markdown_core_finish_ops block_scope_finish = {
        .step = no_op_finish_step,
        .scopes = STEP_AT_PARAGRAPH,
    };
    static const markdown_core_element block_scope = {
        .name = "step-scoped-at-paragraph",
        .finish = &block_scope_finish,
    };
    attach_probe *probe = context;
    markdown_core_dialect_builder_elements(builder, &probe->before);
    probe->attached = markdown_core_dialect_builder_attach(builder, &block_scope);
    markdown_core_dialect_builder_elements(builder, &probe->after);
    return probe->attached != 0;
}

static bool attach_overlapping_step(markdown_core_dialect_builder *builder, void *context) {
    static const markdown_core_finish_ops overlapping_finish = {
        .step = no_op_finish_step,
        .exits = STEP_AT_TEXT,
        .scopes = STEP_AT_TEXT,
    };
    static const markdown_core_element overlapping = {
        .name = "step-exit-and-scope",
        .finish = &overlapping_finish,
    };
    attach_probe *probe = context;
    markdown_core_dialect_builder_elements(builder, &probe->before);
    probe->attached = markdown_core_dialect_builder_attach(builder, &overlapping);
    markdown_core_dialect_builder_elements(builder, &probe->after);
    return probe->attached != 0;
}

static bool attach_kindless_step(markdown_core_dialect_builder *builder, void *context) {
    static const markdown_core_finish_ops kindless_finish = {
        .step = no_op_finish_step,
    };
    static const markdown_core_element kindless = {
        .name = "step-at-no-kind",
        .finish = &kindless_finish,
    };
    attach_probe *probe = context;
    markdown_core_dialect_builder_elements(builder, &probe->before);
    probe->attached = markdown_core_dialect_builder_attach(builder, &kindless);
    markdown_core_dialect_builder_elements(builder, &probe->after);
    return probe->attached != 0;
}

/* An ordinal past MARKDOWN_CORE_NODE_KIND_COUNT: the shape of an extension
 * kind the dispatch table cannot index. */
static const markdown_core_node_type STEP_AT_OUT_OF_TABLE_KIND[] = {
    (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | (MARKDOWN_CORE_NODE_KIND_COUNT + 8)),
    MARKDOWN_CORE_NODE_NONE};

static bool attach_out_of_table_step(markdown_core_dialect_builder *builder, void *context) {
    static const markdown_core_finish_ops outside_finish = {
        .step = no_op_finish_step,
        .exits = STEP_AT_OUT_OF_TABLE_KIND,
    };
    static const markdown_core_element outside = {
        .name = "step-outside-the-table",
        .finish = &outside_finish,
    };
    attach_probe *probe = context;
    markdown_core_dialect_builder_elements(builder, &probe->before);
    probe->attached = markdown_core_dialect_builder_attach(builder, &outside);
    markdown_core_dialect_builder_elements(builder, &probe->after);
    return probe->attached != 0;
}

static void begin_nothing(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    (void)parser;
}

/* An element that declares the document lifecycle is asked for all of it: the
 * engine calls every hook on the owner without asking. */
static bool attach_partial_document(markdown_core_dialect_builder *builder, void *context) {
    static const markdown_core_document_ops lifecycle = {.init = begin_nothing};
    static const markdown_core_element partial = {.name = "partial-document", .document = &lifecycle};
    attach_probe *probe = context;
    markdown_core_dialect_builder_elements(builder, &probe->before);
    probe->attached = markdown_core_dialect_builder_attach(builder, &partial);
    markdown_core_dialect_builder_elements(builder, &probe->after);
    return probe->attached != 0;
}

static void a_finish_step_is_asked_where_it_can_act(test_batch_runner *runner) {
    static const char source[] = "prose\n";
    attach_probe probe = {0, 0, -1};
    markdown_core_node *doc =
        markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_block_scope_step, &probe);
    INT_EQ(runner, probe.attached, 0,
           "a step tracking the extent of a block kind is refused: the blocks below a block complete before it, "
           "so its ENTER and EXIT bracket none of their events");
    INT_EQ(runner, (int)probe.after, (int)probe.before, "and the dialect is left as it was");
    OK(runner, doc == NULL, "so the parse whose setup tried to attach it reports the refusal");
    if (doc) {
        markdown_core_node_free(doc);
    }
    probe = (attach_probe){0, 0, -1};
    doc = markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_overlapping_step, &probe);
    INT_EQ(runner, probe.attached, 0, "a step declaring one kind as both an exit kind and a scope kind is refused");
    INT_EQ(runner, (int)probe.after, (int)probe.before, "with the dialect left as it was");
    OK(runner, doc == NULL, "and the parse reports that refusal too");
    if (doc) {
        markdown_core_node_free(doc);
    }
    probe = (attach_probe){0, 0, -1};
    doc = markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_kindless_step, &probe);
    INT_EQ(runner, probe.attached, 0, "a step asked at no kind, which would never be called, is refused");
    INT_EQ(runner, (int)probe.after, (int)probe.before, "with the dialect left as it was");
    OK(runner, doc == NULL, "and the parse reports the refusal");
    if (doc) {
        markdown_core_node_free(doc);
    }
    probe = (attach_probe){0, 0, -1};
    doc = markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_out_of_table_step, &probe);
    INT_EQ(runner, probe.attached, 0,
           "a step at a kind the dispatch table cannot index is refused rather than asked at every such node");
    INT_EQ(runner, (int)probe.after, (int)probe.before, "with the dialect left as it was");
    OK(runner, doc == NULL, "and the parse reports the refusal");
    if (doc) {
        markdown_core_node_free(doc);
    }
    probe = (attach_probe){0, 0, -1};
    doc = markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_partial_document, &probe);
    INT_EQ(runner, probe.attached, 0,
           "an element declaring only part of the document lifecycle is refused before it could own it");
    INT_EQ(runner, (int)probe.after, (int)probe.before, "with the dialect left as it was");
    OK(runner, doc == NULL, "and the parse reports the refusal");
    if (doc) {
        markdown_core_node_free(doc);
    }
}

static markdown_core_node *match_nothing(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_node *parent, unsigned char character,
                                         markdown_core_inline_state *inline_state) {
    (void)self;
    (void)parser;
    (void)parent;
    (void)character;
    (void)inline_state;
    return NULL;
}

typedef struct {
    const markdown_core_element *element;
    attach_probe probe;
} transparent_probe;

static bool attach_transparent(markdown_core_dialect_builder *builder, void *context) {
    transparent_probe *transparent = context;
    markdown_core_dialect_builder_elements(builder, &transparent->probe.before);
    transparent->probe.attached = markdown_core_dialect_builder_attach(builder, transparent->element);
    markdown_core_dialect_builder_elements(builder, &transparent->probe.after);
    return transparent->probe.attached != 0;
}

/* A FLANKING-TRANSPARENT BYTE IS ASCII. Flanking looks through these bytes as
 * whole characters, and its walk back stops at one that a continuation byte
 * follows, which only holds for an ASCII byte. Each element here inlines, so
 * its bytes would reach the flanking table if it attached; the last ASCII byte
 * attaches and every byte from 0x80 up, continuation bytes included, is
 * refused. */
static void a_flanking_transparent_byte_is_ascii(test_batch_runner *runner) {
    static const char source[] = "a ~~b~~ c\n";
    static const markdown_core_inline_ops inlines[] = {
        {.match = match_nothing, .transparent = "\x7F"},
        {.match = match_nothing, .transparent = "\x80"},
        {.match = match_nothing, .transparent = "\xBF"},
        {.match = match_nothing, .transparent = "~\xFF"},
    };
    static const markdown_core_element elements[] = {
        {.name = "transparent-7f", .inlines = &inlines[0]},
        {.name = "transparent-80", .inlines = &inlines[1]},
        {.name = "transparent-bf", .inlines = &inlines[2]},
        {.name = "transparent-ff", .inlines = &inlines[3]},
    };
    for (size_t i = 0; i < sizeof(elements) / sizeof(elements[0]); i++) {
        const bool ascii = i == 0;
        transparent_probe transparent = {&elements[i], {0, 0, -1}};
        markdown_core_node *doc =
            markdown_core_parse_document_with_setup(source, sizeof(source) - 1, attach_transparent, &transparent);
        INT_EQ(runner, transparent.probe.attached, ascii, "%s is %s", elements[i].name, ascii ? "attached" : "refused");
        INT_EQ(runner, (int)transparent.probe.after, (int)transparent.probe.before + ascii, "%s leaves the dialect %s",
               elements[i].name, ascii ? "one element longer" : "as it was");
        OK(runner, (doc != NULL) == ascii, "%s %s", elements[i].name,
           ascii ? "parses" : "makes the parse report the refusal");
        if (doc) {
            markdown_core_node_free(doc);
        }
    }
}

/* SESSIONS: a text past the capacity is ALLOCATION_FAILED before it is
 * read, an append takes its bytes as they are, and an append that leaves a
 * node's value alone leaves its id. */
static void session_appends(test_batch_runner *runner) {
    static const char source[] = "one\n\ntwo \xf0\xa0\x80\x80\n";
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    OK(runner,
       markdown_core_session_new(NULL, MARKDOWN_CORE_SOURCE_CAPACITY + 1, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
           MARKDOWN_CORE_ALLOCATION_FAILED,
       "a source past the capacity is refused before it is read");
    OK(runner,
       markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF16,
                                 &session) == MARKDOWN_CORE_OK,
       "a session opens");
    if (!session) {
        return;
    }
    document = markdown_core_session_document(session);
    const uint64_t first = markdown_core_node_id(facade_child(markdown_core_document_root(document), 0));
    OK(runner,
       markdown_core_session_append(session, NULL, MARKDOWN_CORE_SOURCE_CAPACITY, &document) ==
           MARKDOWN_CORE_ALLOCATION_FAILED,
       "a text past the capacity is refused before it is read");
    OK(runner,
       markdown_core_session_append(session, (const uint8_t *)"s", 1, &document) == MARKDOWN_CORE_OK &&
           markdown_core_node_id(facade_child(markdown_core_document_root(document), 0)) == first,
       "an append to the second paragraph keeps the first's id");
    OK(runner,
       markdown_core_session_append(session, (const uint8_t *)"\xe4\xb8", 2, &document) == MARKDOWN_CORE_OK &&
           markdown_core_session_text_size(session) == sizeof(source) + 2,
       "an append takes its bytes as they are");
    uint8_t text[64];
    markdown_core_session_text(session, text);
    OK(runner, memcmp(text, "one\n\ntwo \xf0\xa0\x80\x80\ns\xe4\xb8", sizeof(source) + 2) == 0, "the text");
    markdown_core_string lent = markdown_core_session_source(session);
    OK(runner, lent.length == sizeof(source) + 2 && memcmp(lent.data, text, lent.length) == 0,
       "the lent text is the text");
    markdown_core_string line = {0};
    OK(runner,
       markdown_core_session_lines(session) == 0 &&
           markdown_core_session_line(session, 1, &line) == MARKDOWN_CORE_SESSION_STREAMING,
       "a streaming session lends no line");
    const markdown_core_document *final = NULL;
    OK(runner,
       markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK && final &&
           final == markdown_core_session_document(session),
       "flush answers the session's final document");
    OK(runner,
       markdown_core_session_lines(session) == 4 && markdown_core_session_line(session, 3, &line) == MARKDOWN_CORE_OK &&
           line.length == 8 && memcmp(line.data, "two \xf0\xa0\x80\x80", 8) == 0 &&
           line.data == markdown_core_session_source(session).data + 5 &&
           markdown_core_session_line(session, 2, &line) == MARKDOWN_CORE_OK && line.length == 0 &&
           markdown_core_session_line(session, 4, &line) == MARKDOWN_CORE_OK && line.length == 3,
       "a flushed session lends each line without its ending");
    OK(runner,
       markdown_core_session_line(session, 0, &line) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
           markdown_core_session_line(session, 5, &line) == MARKDOWN_CORE_OUT_OF_BOUNDS,
       "a line the text does not have is OUT_OF_BOUNDS");
    document = NULL;
    OK(runner,
       markdown_core_session_append(session, (const uint8_t *)"x", 1, &document) == MARKDOWN_CORE_SESSION_FLUSHED &&
           !document && markdown_core_session_text_size(session) == sizeof(source) + 2,
       "an append after flush is SESSION_FLUSHED and takes nothing");
    OK(runner, markdown_core_session_flush(session, &document) == MARKDOWN_CORE_SESSION_FLUSHED && !document,
       "a second flush is SESSION_FLUSHED");
    OK(runner, facade_child(markdown_core_document_root(markdown_core_session_document(session)), 0) != NULL,
       "the final document stays readable");
    markdown_core_session_close(session);

    /* Every line ending ends a line, and one at the text's end leaves an
     * empty line after it. */
    static const char endings[] = "a\r\nb\rc\n";
    session = NULL;
    OK(runner,
       markdown_core_session_new((const uint8_t *)endings, sizeof(endings) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                 &session) == MARKDOWN_CORE_OK &&
           markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK,
       "a session of three line endings flushes");
    if (session) {
        markdown_core_string a = {0}, b = {0}, c = {0}, last = {0};
        OK(runner,
           markdown_core_session_lines(session) == 4 &&
               markdown_core_session_line(session, 1, &a) == MARKDOWN_CORE_OK &&
               markdown_core_session_line(session, 2, &b) == MARKDOWN_CORE_OK &&
               markdown_core_session_line(session, 3, &c) == MARKDOWN_CORE_OK &&
               markdown_core_session_line(session, 4, &last) == MARKDOWN_CORE_OK && a.length == 1 && a.data[0] == 'a' &&
               b.length == 1 && b.data[0] == 'b' && c.length == 1 && c.data[0] == 'c' && last.length == 0,
           "CRLF, CR and LF each end a line");
        markdown_core_session_close(session);
    }

    /* A snapshot's lookup sees the definitions written before its root, and
     * the final document's every one (plan 5.5, 5.10). */
    static const char later[] = "[x]\n\n[x]: /u\n";
    session = NULL;
    OK(runner,
       markdown_core_session_new((const uint8_t *)later, sizeof(later) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
           MARKDOWN_CORE_OK,
       "a session opens on a use before its definition");
    if (!session) {
        return;
    }
    const markdown_core_node *use =
        facade_child(facade_child(markdown_core_document_root(markdown_core_session_document(session)), 0), 0);
    OK(runner, use && markdown_core_node_get_kind(use) == MARKDOWN_CORE_KIND_TEXT,
       "a snapshot leaves a use before its definition as text");
    final = NULL;
    OK(runner, markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK, "the session flushes");
    use = final ? facade_child(facade_child(markdown_core_document_root(final), 0), 0) : NULL;
    OK(runner, use && markdown_core_node_get_kind(use) == MARKDOWN_CORE_KIND_LINK,
       "the final document resolves a use before its definition");
    markdown_core_session_close(session);
}

/* Whether the session's document dumps as a fresh parse of its text does. */
static bool session_as_parsed(markdown_core_session *session) {
    const size_t size = markdown_core_session_text_size(session);
    uint8_t *text = malloc(size + 1);
    if (!text) {
        return false;
    }
    markdown_core_session_text(session, text);
    markdown_core_document *fresh = facade_parse(text, size);
    const markdown_core_document *document = markdown_core_session_document(session);
    uint8_t *left = NULL, *right = NULL;
    size_t left_length = 0, right_length = 0;
    const bool same = fresh &&
                      markdown_core_document_dump(document, markdown_core_document_root(document), text, size, &left,
                                                  &left_length) == MARKDOWN_CORE_OK &&
                      markdown_core_document_dump(fresh, markdown_core_document_root(fresh), text, size, &right,
                                                  &right_length) == MARKDOWN_CORE_OK &&
                      left_length == right_length && memcmp(left, right, left_length) == 0;
    markdown_core_dump_free(left);
    markdown_core_dump_free(right);
    if (fresh) {
        markdown_core_document_free(fresh);
    }
    free(text);
    return same;
}

/* What a session holds stays as large as what its text needs: an edit lets
 * go what the reading it replaces held, so a thousand edits that type into
 * the blank line of a nested list and take the byte away again hold no more
 * than the first hundred do (plan 12.4). Each edit reads the nested items
 * again and maps their lines anew; the maps of the reading before go. */
static void session_edits_storage(test_batch_runner *runner) {
    static const char source[] = "  one\n\n  - two> \n    -# three four\n\n  ```";
    properties_probe_arm();
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool edited = markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                            &session) == MARKDOWN_CORE_OK &&
                  markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    size_t early = 0;
    const markdown_core_position at = {5, 1}, past = {5, 2};
    for (int edit = 0; edited && edit < 1000; edit++) {
        edited =
            edit % 2
                ? markdown_core_session_edit(session, at, past, (const uint8_t *)"", 0, &document) == MARKDOWN_CORE_OK
                : markdown_core_session_edit(session, at, at, (const uint8_t *)"x", 1, &document) == MARKDOWN_CORE_OK;
        if (edit == 99) {
            early = properties_live_bytes;
        }
    }
    const size_t late = properties_live_bytes;
    OK(runner, edited && session_as_parsed(session), "a thousand edits read the list as a parse does");
    OK(runner, late <= early + early / 8, "the session holds %zu bytes after a thousand edits, %zu after a hundred",
       late, early);
    markdown_core_session_close(session);
    properties_probe_disarm();
    OK(runner, properties_live_bytes == 0, "closing the session releases all it held");
}

/* An edit that renames a definition and cannot have the storage it needs
 * leaves no use of the old name as the link it no longer is: the next edit
 * reads as a parse does, or fails with the session (E-D2). Each allocation
 * of the edit is refused in turn, until the edit needs no more. */
static void session_edits_refresh_storage(test_batch_runner *runner) {
    static const char source[] = "[r]: /url\n\nmid\n\n[r]\n\nplain\n\nend\n";
    const markdown_core_position name = {1, 2}, after = {1, 3}, end = {9, 1}, past = {9, 4};
    bool held = true, swept = false;
    for (size_t fail = 1; held && !swept && fail < 4096; fail++) {
        markdown_core_session *session = NULL;
        const markdown_core_document *document = NULL;
        if (markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                      &session) != MARKDOWN_CORE_OK ||
            markdown_core_session_flush(session, &document) != MARKDOWN_CORE_OK) {
            held = false;
            markdown_core_session_close(session);
            break;
        }
        payload_probe_arm();
        payload_fail_at = fail;
        const markdown_core_status renamed =
            markdown_core_session_edit(session, name, after, (const uint8_t *)"s", 1, &document);
        swept = payload_allocations < fail;
        payload_fail_at = 0;
        const markdown_core_status next =
            markdown_core_session_edit(session, end, past, (const uint8_t *)"done", 4, &document);
        held = (renamed == MARKDOWN_CORE_OK || renamed == MARKDOWN_CORE_ALLOCATION_FAILED) &&
               (next == MARKDOWN_CORE_OK ? session_as_parsed(session) : next == MARKDOWN_CORE_ALLOCATION_FAILED);
        markdown_core_session_close(session);
        payload_probe_disarm();
    }
    OK(runner, held && swept, "no edit after a rename that runs out of storage publishes a stale use");
}

/* An edit reads again only the kept blocks that looked up a label whose
 * definedness it flipped (THE USES): renaming one of two definitions of a
 * label rereads no use of it, a new definition rereads no block that only
 * names its label in code, and deleting a label's only definition turns
 * its uses back into text. */
static void session_edits_uses(test_batch_runner *runner) {
    typedef struct {
        const char *name, *source;
        markdown_core_position from, to;
        const char *text;
        /* The offset no reading of the edit starts before, or 0. */
        size_t after;
    } uses_edit;
    static const char twice[] = "[r]: /a\n\nuse [r]\n\nmid\n\n`[z]` in code\n\n[r]: /b\n\nend [r]\n";
    static const char once[] = "[r]: /a\n\nuse [r]\n\nmid\n\nend [r]\n";
    const uses_edit edits[] = {
        {"rename a duplicate", twice, {9, 2}, {9, 3}, "q", 18},
        {"define a label only code names", twice, {12, 1}, {12, 1}, "\n[z]: /x\n", 38},
        {"delete the only definition", once, {1, 1}, {3, 1}, "", 0},
    };
    for (size_t e = 0; e < sizeof(edits) / sizeof(edits[0]); e++) {
        const uses_edit *edit = &edits[e];
        markdown_core_session *session = NULL;
        const markdown_core_document *document = NULL;
        bool edited = markdown_core_session_new((const uint8_t *)edit->source, strlen(edit->source),
                                                MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) == MARKDOWN_CORE_OK &&
                      markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK &&
                      markdown_core_session_edit(session, edit->from, edit->to, (const uint8_t *)edit->text,
                                                 strlen(edit->text), &document) == MARKDOWN_CORE_OK;
        size_t from = 0, to = 0;
        if (edited) {
            markdown_core_session_reread(session, &from, &to);
        }
        OK(runner, edited && session_as_parsed(session), "%s reads as a parse does", edit->name);
        OK(runner, edited && from >= edit->after, "%s reads again from byte %zu, not before %zu", edit->name, from,
           edit->after);
        markdown_core_session_close(session);
    }
}

/* A block identifier after a nested list names the list from where the
 * list starts, past its line's indentation: no edit of the identifier's
 * line reads from the list's first item, which would keep the name the
 * list had, and no locate there does either (session.c, session_rest_in).
 * A locate at any place leaves the session as it was: every edit after it
 * reads as a parse does. */
static void session_edits_nested_identifier(test_batch_runner *runner) {
    static const char source[] = "- outer\n  - inner\n\n  #inner#\n\n  rest\n";
    static const char *const typed[] = {"", "x"};
    for (size_t t = 0; t < sizeof(typed) / sizeof(typed[0]); t++) {
        markdown_core_session *session = NULL;
        const markdown_core_document *document = NULL;
        const markdown_core_position at = {4, 10};
        bool edited = markdown_core_session_new((const uint8_t *)source, strlen(source), MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                                &session) == MARKDOWN_CORE_OK &&
                      markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK &&
                      markdown_core_session_locate(session, at) == MARKDOWN_CORE_OK &&
                      markdown_core_session_edit(session, at, at, (const uint8_t *)typed[t], strlen(typed[t]),
                                                 &document) == MARKDOWN_CORE_OK;
        OK(runner, edited && session_as_parsed(session), "typing \"%s\" after the identifier reads as a parse does",
           typed[t]);
        markdown_core_session_close(session);
    }
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool same = markdown_core_session_new((const uint8_t *)source, strlen(source), MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                          &session) == MARKDOWN_CORE_OK &&
                markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    for (uint32_t line = 1; same && line <= 6; line++) {
        for (uint32_t column = 1; same && column <= 10; column++) {
            const markdown_core_position at = {line, column};
            const markdown_core_status located = markdown_core_session_locate(session, at);
            if (located == MARKDOWN_CORE_OUT_OF_BOUNDS) {
                continue;
            }
            same = located == MARKDOWN_CORE_OK &&
                   markdown_core_session_edit(session, at, at, (const uint8_t *)"", 0, &document) == MARKDOWN_CORE_OK &&
                   session_as_parsed(session);
        }
    }
    OK(runner, same, "an edit after a locate anywhere reads as a parse does");
    markdown_core_session_close(session);
}

/* Whether the session's context at its cursor is the document query's at
 * `at` of a fresh parse of its text. */
static bool session_as_queried(markdown_core_session *session, markdown_core_position at) {
    const size_t size = markdown_core_session_text_size(session);
    uint8_t *text = malloc(size + 1);
    if (!text) {
        return false;
    }
    markdown_core_session_text(session, text);
    markdown_core_document *fresh = facade_parse(text, size);
    markdown_core_context *queried = NULL;
    const markdown_core_context *located = markdown_core_session_context(session);
    bool same = fresh && markdown_core_document_context(fresh, text, size, at, &queried) == MARKDOWN_CORE_OK &&
                markdown_core_context_block_count(queried) == markdown_core_context_block_count(located) &&
                markdown_core_context_inline_count(queried) == markdown_core_context_inline_count(located) &&
                markdown_core_context_slot_count(queried) == markdown_core_context_slot_count(located);
    for (size_t i = 0; same && i < markdown_core_context_block_count(queried); i++) {
        markdown_core_opener left, right;
        same = markdown_core_context_block_at(queried, i, &left) == MARKDOWN_CORE_OK &&
               markdown_core_context_block_at(located, i, &right) == MARKDOWN_CORE_OK && left.kind == right.kind &&
               !memcmp(&left.scope, &right.scope, sizeof(left.scope));
    }
    for (size_t i = 0; same && i < markdown_core_context_inline_count(queried); i++) {
        markdown_core_opener left, right;
        same = markdown_core_context_inline_at(queried, i, &left) == MARKDOWN_CORE_OK &&
               markdown_core_context_inline_at(located, i, &right) == MARKDOWN_CORE_OK && left.kind == right.kind &&
               !memcmp(&left.scope, &right.scope, sizeof(left.scope));
    }
    for (size_t i = 0; same && i < markdown_core_context_slot_count(queried); i++) {
        markdown_core_slot left, right;
        same = markdown_core_context_slot_at(queried, i, &left) == MARKDOWN_CORE_OK &&
               markdown_core_context_slot_at(located, i, &right) == MARKDOWN_CORE_OK && left.kind == right.kind &&
               left.owner == right.owner && !memcmp(&left.before, &right.before, sizeof(left.before)) &&
               !memcmp(&left.after, &right.after, sizeof(left.after)) && left.closed == right.closed;
    }
    markdown_core_context_free(queried);
    if (fresh) {
        markdown_core_document_free(fresh);
    }
    free(text);
    return same;
}

/* A table's head row and its content rows are two relations of its
 * children, the content going on from where the head ends: a locate in a
 * content row reads a window of the table's rows that starts past the
 * head, which measures its first row from where the window says it starts
 * (ast.c, markdown_core_walk_next). At every place of the text, the
 * session's context is the document query's, the code span a row leaves
 * open among it. */
static void session_locates_table_rows(test_batch_runner *runner) {
    static const char source[] = "e||\n-|-\n/\n`\nle\"\n| [r] | x |\n\n[r]: /u\n";
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool same = markdown_core_session_new((const uint8_t *)source, strlen(source), MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                          &session) == MARKDOWN_CORE_OK &&
                markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    markdown_core_position at = {1, 1};
    for (const char *c = source; same && *c; c++) {
        const markdown_core_status located = markdown_core_session_locate(session, at);
        same = located == MARKDOWN_CORE_OK && session_as_queried(session, at);
        if (*c == '\n') {
            at = (markdown_core_position){at.line + 1, 1};
        } else {
            at.column++;
        }
    }
    OK(runner, same, "a locate in a table's content rows is the document query, at %d:%d", at.line, at.column);
    markdown_core_session_close(session);
}

/* An opener the text leaves open notes no reach (blocks.c, AN OPENER READ
 * TO THE END), and the text before a cursor may end in a stop line of it
 * that the whole line is not: `%%` and a tab before `x`, or `---` before
 * `-`. At every place of each text, the session's context is the document
 * query's. */
static void session_locates_unclosed_openers(test_batch_runner *runner) {
    static const char *const sources[] = {
        "%%\n\na\n\nb\n\nc\n\n%%\tx\n",
        "---\n\na\n\nb\n\nc\n\n----\n",
        "lead\n\n%%\n\na\n\n- b\n\n  %% x\n\n> %%\n",
    };
    bool same = true;
    markdown_core_position at = {1, 1};
    size_t which = 0;
    for (size_t i = 0; same && i < sizeof(sources) / sizeof(sources[0]); i++) {
        const char *source = sources[which = i];
        markdown_core_session *session = NULL;
        const markdown_core_document *document = NULL;
        same = markdown_core_session_new((const uint8_t *)source, strlen(source), MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                         &session) == MARKDOWN_CORE_OK &&
               markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
        at = (markdown_core_position){1, 1};
        for (const char *c = source; same && *c; c++) {
            const markdown_core_status located = markdown_core_session_locate(session, at);
            same = located == MARKDOWN_CORE_OK && session_as_queried(session, at);
            if (*c == '\n') {
                at = (markdown_core_position){at.line + 1, 1};
            } else {
                at.column++;
            }
        }
        markdown_core_session_close(session);
    }
    OK(runner, same, "a locate after an unclosed opener is the document query, text %zu at %d:%d", which, at.line,
       at.column);
}

/* A long list, and a long one nested in one of its items, find the rest
 * point of a line by the widths of their items and fold the items kept
 * from the marks of the chunks before (session.c, THE NESTS): a locate at
 * lines across the chunks' bounds is the document query's, and edits that
 * make the list tight, loose and tight again, add items and take them
 * away, each after locates that mark chunks past it, read as a parse
 * does. */
static void session_edits_wide_list(test_batch_runner *runner) {
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    for (int item = 0; item < 150; item++) {
        char line[96];
        snprintf(line, sizeof(line), item == 70 ? "- item %03d [r]\n" : "- item %03d with *em* and [r\n", item);
        markdown_core_strbuf_puts(&text, line);
        if (item == 120) {
            markdown_core_strbuf_puts(&text, "\n");
        }
        if (item == 40) {
            for (int inner = 0; inner < 140; inner++) {
                snprintf(line, sizeof(line), "  - inner %03d `code\n", inner);
                markdown_core_strbuf_puts(&text, line);
            }
        }
    }
    markdown_core_strbuf_puts(&text, "\n[r]: /u\n");
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool same =
        !text.oom &&
        markdown_core_session_new(text.ptr, text.size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) == MARKDOWN_CORE_OK &&
        markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    static const uint32_t lines[] = {2, 40, 42, 64, 65, 66, 105, 106, 107, 129, 130, 131, 170, 182, 200, 211, 250, 288};
    static const uint32_t columns[] = {3, 9, 20, 26};
    for (size_t l = 0; same && l < sizeof(lines) / sizeof(lines[0]); l++) {
        for (size_t c = 0; same && c < sizeof(columns) / sizeof(columns[0]); c++) {
            const markdown_core_position at = {lines[l], columns[c]};
            const markdown_core_status located = markdown_core_session_locate(session, at);
            same = located == MARKDOWN_CORE_OUT_OF_BOUNDS ||
                   (located == MARKDOWN_CORE_OK && session_as_queried(session, at));
        }
    }
    OK(runner, same, "a locate in a long list is the document query");
    typedef struct {
        markdown_core_position from, to;
        const char *text;
    } list_edit;
    static const list_edit edits[] = {
        {{262, 1}, {263, 1}, ""},  {{130, 1}, {130, 1}, "\n"},
        {{200, 5}, {200, 5}, "x"}, {{131, 1}, {132, 1}, ""},
        {{66, 3}, {66, 3}, "y"},   {{66, 1}, {66, 1}, "  - more\n"},
        {{3, 1}, {3, 1}, "\n"},    {{4, 1}, {5, 1}, ""},
        {{107, 1}, {109, 1}, ""},  {{250, 1}, {250, 1}, "- one\n- two\n"},
    };
    for (size_t e = 0; same && e < sizeof(edits) / sizeof(edits[0]); e++) {
        const list_edit *edit = &edits[e];
        for (size_t l = 0; same && l < sizeof(lines) / sizeof(lines[0]); l += 3) {
            const markdown_core_position at = {lines[l], 9};
            const markdown_core_status located = markdown_core_session_locate(session, at);
            same = located == MARKDOWN_CORE_OUT_OF_BOUNDS ||
                   (located == MARKDOWN_CORE_OK && session_as_queried(session, at));
        }
        same = same && markdown_core_session_locate(session, edit->from) == MARKDOWN_CORE_OK &&
               markdown_core_session_edit(session, edit->from, edit->to, (const uint8_t *)edit->text,
                                          strlen(edit->text), &document) == MARKDOWN_CORE_OK &&
               session_as_parsed(session);
        OK(runner, same, "edit %zu of a long list reads as a parse does", e);
    }
    markdown_core_session_close(session);
    markdown_core_strbuf_free(&text);
}

/* The allocator sweep of 12.7: an edit refused at each of its allocations
 * in turn either leaves the session as it was or fails it, and the session
 * then frees everything it holds. The edits type into a paragraph and a
 * nested item, open a fence, rename a definition, retitle a heading that
 * a reference names, write into a table and take most of a long text away,
 * which compacts it. After each refused edit, another edit either reads as
 * a parse of its text does or fails too. */
static void session_edits_allocations(test_batch_runner *runner) {
    typedef struct {
        const char *name, *source;
        markdown_core_position from, to;
        const char *text;
    } sweep_edit;
    markdown_core_strbuf long_text = MARKDOWN_CORE_BUF_INIT();
    for (int item = 0; item < 120; item++) {
        char line[64];
        snprintf(line, sizeof(line), "- item %03d\n\n  more [r]\n\n", item);
        markdown_core_strbuf_puts(&long_text, line);
    }
    markdown_core_strbuf_puts(&long_text, "[r]: /u\n");
    const sweep_edit edits[] = {
        {"paragraph", "one\n\ntwo\n\nthree\n", {3, 2}, {3, 2}, "x"},
        {"nested item", "- a\n  - b\n\n  c\n- d\n", {2, 6}, {2, 6}, "x"},
        {"fence", "one\n\ntwo\n\nthree\n", {1, 1}, {1, 1}, "```\n"},
        {"definition", "see [r]\n\n[r]: /u\n\nmid\n\n[r] again\n", {3, 2}, {3, 3}, "s"},
        {"heading", "see [Title]\n\n# Title\n\nend [Title]\n", {3, 8}, {3, 8}, "s"},
        {"table", "| a |\n|---|\n| b [r] |\n\n[r]: /u\n", {3, 3}, {3, 3}, "c"},
        {"compaction", (const char *)long_text.ptr, {3, 1}, {481, 1}, ""},
    };
    const markdown_core_position again = {1, 1};
    for (size_t e = 0; e < sizeof(edits) / sizeof(edits[0]); e++) {
        const sweep_edit *edit = &edits[e];
        bool held = !long_text.oom, swept = false;
        size_t fail = 1;
        for (; held && !swept && fail < 100000; fail++) {
            payload_probe_arm();
            markdown_core_session *session = NULL;
            const markdown_core_document *document = NULL;
            if (markdown_core_session_new((const uint8_t *)edit->source, strlen(edit->source),
                                          MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) != MARKDOWN_CORE_OK ||
                markdown_core_session_flush(session, &document) != MARKDOWN_CORE_OK) {
                held = false;
                markdown_core_session_close(session);
                payload_probe_disarm();
                break;
            }
            payload_fail_at = payload_allocations + fail;
            const size_t before = payload_allocations;
            const markdown_core_status status = markdown_core_session_edit(
                session, edit->from, edit->to, (const uint8_t *)edit->text, strlen(edit->text), &document);
            swept = payload_allocations - before < fail;
            payload_fail_at = 0;
            const markdown_core_status next =
                markdown_core_session_edit(session, again, again, (const uint8_t *)"z", 1, &document);
            held = (status == MARKDOWN_CORE_OK || status == MARKDOWN_CORE_ALLOCATION_FAILED) &&
                   (next == MARKDOWN_CORE_OK ? session_as_parsed(session) : next == MARKDOWN_CORE_ALLOCATION_FAILED);
            markdown_core_session_close(session);
            held = held && payload_live == 0;
            payload_probe_disarm();
        }
        OK(runner, held && swept,
           "the %s edit refused at each of its %zu allocations keeps the session or fails it, and frees all",
           edit->name, fail - 1);
    }
    markdown_core_strbuf_free(&long_text);
}

/* A use whose label holds a NUL, which the block replaces before the
 * inline parse looks the label up, is read again when an edit renames the
 * definition it names (E-D2). */
static void session_edits_replaced_label(test_batch_runner *runner) {
    static const char source[] = "[a\0b]: /url\n\nmid\n\n[a\0b]\n\nend\n";
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    const markdown_core_position name = {1, 2}, after = {1, 3};
    const bool edited =
        markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                  &session) == MARKDOWN_CORE_OK &&
        markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK &&
        markdown_core_session_edit(session, name, after, (const uint8_t *)"c", 1, &document) == MARKDOWN_CORE_OK;
    OK(runner, edited && session_as_parsed(session), "renaming a label that holds a NUL reads its use again");
    markdown_core_session_close(session);
}

/* An edit that leaps into a paragraph under a definition and parses its
 * inline content whole lists the inline notes of the paragraph's old tail
 * again: the old ones go, to the paragraph's end (blocks.c, THE LEAP). */
static void session_edits_nested_notes(test_batch_runner *runner) {
    static const char source[] = "a\n~\n  $m$ |a|\n  word word\n[^n]\nq -\n  [^[n]\nafter\n";
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    const markdown_core_position line = {5, 1}, next = {6, 1};
    const bool edited =
        markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                  &session) == MARKDOWN_CORE_OK &&
        markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK &&
        markdown_core_session_edit(session, line, next, (const uint8_t *)"", 0, &document) == MARKDOWN_CORE_OK;
    OK(runner, edited && session_as_parsed(session),
       "taking a line out of a definition's paragraph reads its inline note as a parse does");
    markdown_core_session_close(session);
}

/* A heading whose text holds more before its last line than a label may
 * hold parses from its seams as a paragraph does, and names itself by the
 * measure they keep of its anchor's base (heading.c, A LONG NAME): an edit
 * in one of two such headings that project to one base reads as a parse
 * does, and each shows its anchor whole, the second the first's with -1. */
static void session_edits_long_headings(test_batch_runner *runner) {
    char line[16];
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    for (int copy = 0; copy < 2; copy++) {
        for (int i = 0; i < 200; i++) {
            snprintf(line, sizeof(line), "word %d\n", i);
            markdown_core_strbuf_puts(&text, line);
        }
        markdown_core_strbuf_puts(&text, "===\n\n");
    }
    markdown_core_strbuf_puts(&text, "end\n");
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool edited = markdown_core_session_new(text.ptr, (size_t)text.size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                      MARKDOWN_CORE_OK &&
                  markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    for (int edit = 0; edited && edit < 6; edit++) {
        const markdown_core_position at = {(uint32_t)(100 + edit / 2), 2}, past = {(uint32_t)(100 + edit / 2), 3};
        edited = (edit % 2 ? markdown_core_session_edit(session, at, past, (const uint8_t *)"", 0, &document)
                           : markdown_core_session_edit(session, at, at, (const uint8_t *)"x", 1, &document)) ==
                     MARKDOWN_CORE_OK &&
                 session_as_parsed(session);
    }
    OK(runner, edited, "edits in a long heading read as a parse does");
    const markdown_core_node *root = edited ? markdown_core_document_root(document) : NULL;
    const markdown_core_optional_string first =
        root ? anchor_of(facade_child(root, 0)) : (markdown_core_optional_string){0};
    const markdown_core_optional_string second =
        root ? anchor_of(facade_child(root, 1)) : (markdown_core_optional_string){0};
    OK(runner,
       first.has_value && second.has_value && first.value.length > 1000 &&
           second.value.length == first.value.length + 2 && memcmp(first.value.data, "word-0-word-1-", 14) == 0 &&
           memcmp(first.value.data, second.value.data, first.value.length) == 0 &&
           memcmp(second.value.data + first.value.length, "-1", 2) == 0,
       "two long headings of one base show it, the second with -1");
    markdown_core_session_close(session);
    markdown_core_strbuf_free(&text);
}

/* An authored anchor of a long name reserves it as a short one does: a
 * heading whose computed base it is takes the next suffix, before the
 * authored one and after it, and an edit that writes the authored anchor or
 * lets it go reads as a parse does. */
/* A long computed anchor is made at its first read, which reports a
 * refused allocation rather than showing no anchor, and reads again after. */
static void heading_long_anchor_refused(test_batch_runner *runner) {
    char text[1104] = "# ";
    memset(text + 2, 'a', 1100);
    text[1102] = '\n';
    markdown_core_document *document = NULL;
    const bool ok = markdown_core_document_parse((const uint8_t *)text, 1103, &document) == MARKDOWN_CORE_OK;
    const markdown_core_node *heading = ok ? facade_child(markdown_core_document_root(document), 0) : NULL;
    markdown_core_optional_string anchor = {0};
    markdown_core_status refused = MARKDOWN_CORE_OK;
    if (heading) {
        payload_probe_arm();
        payload_fail_at = payload_allocations + 1;
        refused = markdown_core_node_anchor(heading, &anchor);
        payload_fail_at = 0;
        payload_probe_disarm();
    }
    INT_EQ(runner, refused, MARKDOWN_CORE_ALLOCATION_FAILED, "a refused first read of a long anchor reports it");
    OK(runner,
       heading && markdown_core_node_anchor(heading, &anchor) == MARKDOWN_CORE_OK && anchor.has_value &&
           anchor.value.length == 1100,
       "the long anchor reads once storage is had");
    markdown_core_document_free(document);
}

static void heading_long_authored_anchor(test_batch_runner *runner) {
    char stem[1101];
    memset(stem, 'a', 1100);
    stem[1100] = '\0';
    char text[4096];
    const int size = snprintf(text, sizeof(text), "# %s\n\n# x {#%s}\n\n# %s\n", stem, stem, stem);
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool ok = markdown_core_session_new((const uint8_t *)text, (size_t)size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                  MARKDOWN_CORE_OK &&
              markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    const markdown_core_node *root = ok ? markdown_core_document_root(document) : NULL;
    bool shown = root != NULL;
    static const char *const ends[] = {"-1", "", "-2"};
    for (int i = 0; shown && i < 3; i++) {
        const markdown_core_optional_string anchor = anchor_of(facade_child(root, i));
        const size_t end = strlen(ends[i]);
        shown = anchor.has_value && anchor.value.length == 1100 + end && memcmp(anchor.value.data, stem, 1100) == 0 &&
                memcmp(anchor.value.data + 1100, ends[i], end) == 0;
    }
    OK(runner, shown, "a long authored anchor reserves its name before and after it");
    /* Line 3 is the authored heading: its anchor goes, then comes back. */
    const markdown_core_position at = {3, 5}, past = {3, (uint32_t)(5 + 3 + 1100)};
    ok = ok && markdown_core_session_edit(session, at, past, (const uint8_t *)"", 0, &document) == MARKDOWN_CORE_OK &&
         session_as_parsed(session);
    const int written = snprintf(text, sizeof(text), "{#%s}", stem);
    ok = ok &&
         markdown_core_session_edit(session, at, at, (const uint8_t *)text, (size_t)written, &document) ==
             MARKDOWN_CORE_OK &&
         session_as_parsed(session);
    OK(runner, ok, "edits that write a long authored anchor or let it go read as a parse does");
    markdown_core_session_close(session);
}

/* What a session holds after an edit lets most of its text go: the edit
 * reads all of what is left into storage of its own, which holds no more
 * than twice what a session that reads that text afresh holds, and the
 * blocks before the edit keep their ids (plan 12.4, E-D4). */
static void session_edits_compaction(test_batch_runner *runner) {
    markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT();
    for (int item = 0; item < 400; item++) {
        char line[96];
        snprintf(line, sizeof(line), "- item %03d\n  more\n\n  - inner\n    - deep\n\n  ```\n  code\n  ```\n\n", item);
        markdown_core_strbuf_puts(&source, line);
    }
    properties_probe_arm();
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool edited = markdown_core_session_new(source.ptr, (size_t)source.size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                      MARKDOWN_CORE_OK &&
                  markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    const uint64_t first = edited ? markdown_core_node_id(facade_child(markdown_core_document_root(document), 0)) : 0;
    /* Lines 1 to 400 are the first forty items, and the edit takes the
     * rest away. */
    const markdown_core_position from = {401, 1}, to = {4001, 1};
    edited =
        edited && markdown_core_session_edit(session, from, to, (const uint8_t *)"", 0, &document) == MARKDOWN_CORE_OK;
    const size_t held = properties_live_bytes;
    const size_t size = edited ? markdown_core_session_text_size(session) : 0;
    uint8_t *const text = malloc(size + 1);
    size_t fresh = 0;
    if (edited && text) {
        markdown_core_session_text(session, text);
        markdown_core_session *again = NULL;
        const markdown_core_document *read = NULL;
        if (markdown_core_session_new(text, size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &again) == MARKDOWN_CORE_OK &&
            markdown_core_session_flush(again, &read) == MARKDOWN_CORE_OK) {
            fresh = properties_live_bytes - held;
        }
        markdown_core_session_close(again);
    }
    OK(runner, edited && session_as_parsed(session),
       "the edit that takes most of the list away reads it as a parse does");
    OK(runner, edited && markdown_core_node_id(facade_child(markdown_core_document_root(document), 0)) == first,
       "the list keeps its id");
    OK(runner, fresh && held <= 2 * fresh, "the session holds %zu bytes after the edit, a fresh one %zu", held, fresh);
    markdown_core_session_close(session);
    properties_probe_disarm();
    OK(runner, properties_live_bytes == 0, "closing the sessions releases all they held");
    free(text);
    markdown_core_strbuf_free(&source);
}

/* An edit after the flush: refused while the session streams and at a cursor
 * that stands nowhere, and otherwise the parse of the new text, keeping the
 * ids of what it did not change (plan 12.3). */
/* THE NAMES AN EDIT MOVES (heading.c): an edit names again only the groups
 * of the stems it touched, and the anchors every heading then shows are a
 * fresh parse's. The text holds bases that are each other's parent
 * (`section`, `section-1`, `section-1-1`), a run of one base (`notes`),
 * explicit anchors that take a heading's name, and bases no other heading
 * bears on (`part-2`, `part-3`). Each edit types a byte, a line ending, a
 * whole heading or an anchor at every place, and takes it away again. */
static void session_edits_name_headings(test_batch_runner *runner) {
    static const char text[] = "# Section\n\n# Section 1\n\n# Notes\n\nx\n\n# Notes\n\n# Section\n\n"
                               "# Other {#notes-1}\n\n# Section 1 1\n\n# Part 2\n\n# Notes\n\n# Part 3\n\n"
                               "# Notes 1\n\n# Section {#section-2}\n";
    static const char *const typed[] = {"z", "\n", "1", " 1", "# Notes\n\n", "# Section 1\n\n", " {#notes}", "-"};
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    bool same = markdown_core_session_new((const uint8_t *)text, sizeof(text) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                          &session) == MARKDOWN_CORE_OK &&
                markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK && session_as_parsed(session);
    size_t lines = 1;
    for (size_t i = 0; i + 1 < sizeof(text); i++) {
        lines += text[i] == '\n';
    }
    for (size_t t = 0; same && t < sizeof(typed) / sizeof(*typed); t++) {
        const size_t size = strlen(typed[t]);
        for (int line = 1; same && line <= (int)lines; line++) {
            for (int column = 1; same; column++) {
                const markdown_core_position at = {line, column};
                const markdown_core_status status =
                    markdown_core_session_edit(session, at, at, (const uint8_t *)typed[t], size, &document);
                if (status == MARKDOWN_CORE_OUT_OF_BOUNDS) {
                    break;
                }
                same = status == MARKDOWN_CORE_OK && session_as_parsed(session);
                /* Where the typed text ends: its own lines move the end. */
                markdown_core_position end = at;
                for (size_t i = 0; i < size; i++) {
                    if (typed[t][i] == '\n') {
                        end.line++;
                        end.column = 1;
                    } else {
                        end.column++;
                    }
                }
                same = same && markdown_core_session_edit(session, at, end, NULL, 0, &document) == MARKDOWN_CORE_OK &&
                       session_as_parsed(session);
            }
        }
    }
    OK(runner, same, "the headings an edit names again show the anchors a fresh parse gives them");
    markdown_core_session_close(session);
    /* Edits that take many lines at once: each run of whole lines of each
     * text goes, or gives way to a heading, in a session of its own. The
     * second text holds two headings of one base that one edit takes (a
     * stem that the edit empties once, however many of its headings go). */
    static const char *const texts[] = {text, "# A\n\n# A\n\n# A 1\n\n# A 1\n\n# A 2\n"};
    static const char *const put[] = {"", "# A\n\n", "# Notes\n\n", "# Section 1\n\n"};
    bool kept = true;
    for (size_t x = 0; kept && x < sizeof(texts) / sizeof(*texts); x++) {
        const size_t length = strlen(texts[x]);
        int count = 1;
        for (size_t i = 0; i + 1 < length; i++) {
            count += texts[x][i] == '\n';
        }
        for (int first = 1; kept && first <= count; first++) {
            for (int last = first + 1; kept && last <= count + 1; last++) {
                for (size_t p = 0; kept && p < sizeof(put) / sizeof(*put); p++) {
                    markdown_core_session *own = NULL;
                    const markdown_core_document *read = NULL;
                    const markdown_core_position from = {first, 1}, to = {last, 1};
                    kept = markdown_core_session_new((const uint8_t *)texts[x], length, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                                     &own) == MARKDOWN_CORE_OK &&
                           markdown_core_session_flush(own, &read) == MARKDOWN_CORE_OK;
                    const markdown_core_status status =
                        kept ? markdown_core_session_edit(own, from, to, (const uint8_t *)put[p], strlen(put[p]), &read)
                             : MARKDOWN_CORE_OK;
                    kept = kept && (status == MARKDOWN_CORE_OUT_OF_BOUNDS ||
                                    (status == MARKDOWN_CORE_OK && session_as_parsed(own)));
                    markdown_core_session_close(own);
                }
            }
        }
    }
    OK(runner, kept, "the headings an edit of many lines names again show a fresh parse's anchors");
    /* A base whose number is past any size: no earlier heading holds its
     * name, as no suffix wraps to the first heading's base. */
    static const char wide[] = "# a\n\n# a-18446744073709551616\n\nend\n";
    markdown_core_session *own = NULL;
    const markdown_core_document *read = NULL;
    const markdown_core_position from = {3, 3}, to = {3, 4};
    OK(runner,
       markdown_core_session_new((const uint8_t *)wide, sizeof(wide) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8, &own) ==
               MARKDOWN_CORE_OK &&
           markdown_core_session_flush(own, &read) == MARKDOWN_CORE_OK &&
           markdown_core_session_edit(own, from, to, (const uint8_t *)"a", 1, &read) == MARKDOWN_CORE_OK &&
           session_as_parsed(own),
       "a heading whose base ends in a number past any size keeps a fresh parse's anchor");
    markdown_core_session_close(own);
}

/* Every block of literal lines reads an edit of a later line of its own
 * from that line (blocks.c, THE LEAP): a code fence; a `$$`, `\\[` or `%%`
 * block, whose opener's line gives its content its first line as a fence's
 * does, and the `%%` block's opener, which read ahead for its closer,
 * decided only that it found one (element.h, the edit's `shuts`); and a
 * block a block of literal lines became as it closed, an HTML comment
 * block or a formula fence, as the block it read as (THE FORMER KIND). It
 * is the node it was, and the reading takes the old text past it back. An
 * edit of its last line reads it as a fresh parse does, and one that leaves
 * it no longer of its kind makes it a node of its own. */
static void session_edits_literal_blocks(test_batch_runner *runner) {
    static const struct {
        const char *open, *close;
        markdown_core_node_kind kind;
    } blocks[] = {
        {"```\n", "```\n", MARKDOWN_CORE_KIND_CODE_BLOCK},
        {"$$\n", "$$\n", MARKDOWN_CORE_KIND_FORMULA_BLOCK},
        {"\\\\[  \n", "\\\\]\n", MARKDOWN_CORE_KIND_FORMULA_BLOCK},
        {"%%\n", "%%\n", MARKDOWN_CORE_KIND_COMMENT},
        {"<!--\n", "-->\n", MARKDOWN_CORE_KIND_COMMENT},
        {"```formula\n", "```\n", MARKDOWN_CORE_KIND_FORMULA_BLOCK},
    };
    for (size_t b = 0; b < sizeof(blocks) / sizeof(*blocks); b++) {
        markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
        markdown_core_strbuf_puts(&text, "before\n\n");
        markdown_core_strbuf_puts(&text, blocks[b].open);
        for (int i = 0; i < 64; i++) {
            markdown_core_strbuf_puts(&text, "x_line\n");
        }
        markdown_core_strbuf_puts(&text, blocks[b].close);
        markdown_core_strbuf_puts(&text, "\nafter\n");
        markdown_core_session *session = NULL;
        const markdown_core_document *document;
        OK(runner,
           markdown_core_session_new(text.ptr, (size_t)text.size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                   MARKDOWN_CORE_OK &&
               markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK,
           "a session flushes the block");
        markdown_core_strbuf_free(&text);
        if (!session) {
            continue;
        }
        const markdown_core_node *block = facade_child(markdown_core_document_root(document), 1);
        const uint64_t id = markdown_core_node_id(block);
        OK(runner, markdown_core_node_get_kind(block) == blocks[b].kind, "the block became its own kind");
        bool kept = true, local = true;
        for (int k = 0; k < 8; k++) {
            const markdown_core_position at = {4 + 30 + k % 3, 2};
            size_t from = 0, to = 0;
            kept =
                kept &&
                markdown_core_session_edit(session, at, at, (const uint8_t *)"y", 1, &document) == MARKDOWN_CORE_OK &&
                session_as_parsed(session);
            block = facade_child(markdown_core_document_root(document), 1);
            kept = kept && markdown_core_node_get_kind(block) == blocks[b].kind && markdown_core_node_id(block) == id;
            markdown_core_session_reread(session, &from, &to);
            local = local && to - from < 64;
        }
        OK(runner, kept, "an edit of a later line keeps the block, the node it was");
        OK(runner, local, "and reads the text again only around the line");
        /* Its last line no longer closes it: it reads to the text's end
         * as the block of literal lines, or the fence it was. */
        const markdown_core_position last = {3 + 64 + 1, 1}, cut = {3 + 64 + 1, 3};
        OK(runner,
           markdown_core_session_edit(session, last, cut, NULL, 0, &document) == MARKDOWN_CORE_OK &&
               session_as_parsed(session),
           "an edit of its last line reads it as a fresh parse does");
        block = facade_child(markdown_core_document_root(document), 1);
        OK(runner,
           markdown_core_node_get_kind(block) != blocks[b].kind ? markdown_core_node_id(block) != id
                                                                : markdown_core_node_id(block) == id,
           "a block no longer of the kind it became is a node of its own");
        markdown_core_session_close(session);
    }
}

static void session_edits(test_batch_runner *runner) {
    static const char source[] = "one\n\ntw\xf0\xa0\x80\x80o\n";
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    OK(runner,
       markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF16,
                                 &session) == MARKDOWN_CORE_OK,
       "a session opens");
    if (!session) {
        return;
    }
    const markdown_core_position at = {3, 6};
    OK(runner,
       markdown_core_session_edit(session, at, at, (const uint8_t *)"s", 1, &document) ==
               MARKDOWN_CORE_SESSION_STREAMING &&
           !document,
       "an edit before the flush is SESSION_STREAMING");
    OK(runner, markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK, "the session flushes");
    const markdown_core_node *root = markdown_core_document_root(document);
    const uint64_t first = markdown_core_node_id(facade_child(root, 0));
    const uint64_t second = markdown_core_node_id(facade_child(root, 1));
    const markdown_core_position nowhere[][2] = {
        {{0, 1}, {0, 1}}, {{1, 5}, {1, 5}}, {{3, 4}, {3, 4}}, {{5, 1}, {5, 1}}, {{3, 2}, {3, 1}}};
    bool refused = true;
    for (size_t i = 0; i < sizeof(nowhere) / sizeof(nowhere[0]); i++) {
        document = NULL;
        refused = refused &&
                  markdown_core_session_edit(session, nowhere[i][0], nowhere[i][1], (const uint8_t *)"x", 1,
                                             &document) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
                  !document;
    }
    OK(runner, refused && markdown_core_session_text_size(session) == sizeof(source) - 1,
       "an edit at a cursor that stands nowhere, or reversed, is OUT_OF_BOUNDS and changes nothing");
    OK(runner,
       markdown_core_session_edit(session, at, at, (const uint8_t *)"s", 1, &document) == MARKDOWN_CORE_OK &&
           document == markdown_core_session_document(session),
       "an edit after the surrogate pair answers the session's document");
    uint8_t text[64];
    markdown_core_session_text(session, text);
    OK(runner,
       markdown_core_session_text_size(session) == sizeof(source) &&
           memcmp(text, "one\n\ntw\xf0\xa0\x80\x80os\n", sizeof(source)) == 0,
       "the edit lands in the text");
    root = markdown_core_document_root(document);
    OK(runner,
       markdown_core_node_id(facade_child(root, 0)) == first && markdown_core_node_id(facade_child(root, 1)) == second,
       "both paragraphs keep their ids");
    size_t from = 0, to = 0;
    markdown_core_session_reread(session, &from, &to);
    OK(runner, from <= 5 && to == sizeof(source), "the edit read the text again from the paragraph before it");
    const markdown_core_position start = {1, 1}, end = {3, 1};
    OK(runner,
       markdown_core_session_edit(session, start, end, (const uint8_t *)"# ", 2, &document) == MARKDOWN_CORE_OK &&
           markdown_core_node_get_kind(facade_child(markdown_core_document_root(document), 0)) ==
               MARKDOWN_CORE_KIND_HEADING &&
           !facade_child(markdown_core_document_root(document), 1),
       "an edit across lines reads the new text");
    markdown_core_session_close(session);

    /* An edit reads the new text up to the first old child after it that
     * reads as it did, and takes that child and those after it back as
     * they were (plan 12.3, step 4). */
    static const char middle[] = "one\n\ntwo\n\nthree\n";
    session = NULL;
    OK(runner,
       markdown_core_session_new((const uint8_t *)middle, sizeof(middle) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
               MARKDOWN_CORE_OK &&
           markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK,
       "a session flushes three paragraphs");
    if (!session) {
        return;
    }
    const uint64_t third = markdown_core_node_id(facade_child(markdown_core_document_root(document), 2));
    const markdown_core_position typed = {3, 2}, past = {3, 3};
    OK(runner,
       markdown_core_session_edit(session, typed, typed, (const uint8_t *)"x", 1, &document) == MARKDOWN_CORE_OK &&
           session_as_parsed(session) &&
           markdown_core_node_id(facade_child(markdown_core_document_root(document), 2)) == third,
       "an edit in the middle paragraph takes the last one back");
    markdown_core_session_reread(session, &from, &to);
    OK(runner, from == 0 && to == 11, "it reads the text again only up to the last paragraph");
    OK(runner,
       markdown_core_session_edit(session, typed, past, (const uint8_t *)"", 0, &document) == MARKDOWN_CORE_OK &&
           session_as_parsed(session),
       "an edit that takes the typed byte away takes the last paragraph back again");
    markdown_core_session_reread(session, &from, &to);
    OK(runner, from == 0 && to == 10, "it reads the text again only up to where the last paragraph starts");
    const markdown_core_position later = {5, 3};
    OK(runner,
       markdown_core_session_edit(session, later, later, (const uint8_t *)"\n\n# t", 5, &document) ==
               MARKDOWN_CORE_OK &&
           session_as_parsed(session),
       "an edit in a paragraph taken back reads it as any other");
    markdown_core_session_close(session);

    /* What the children taken back hold stands: their headings, numbered
     * after the new ones, their definitions and the links to them. */
    static const char held[] = "# a\n\nb\n\n# a\n\n[x]: /u\n\n[x] [^n]\n\n[^n]: n\n";
    session = NULL;
    OK(runner,
       markdown_core_session_new((const uint8_t *)held, sizeof(held) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
               MARKDOWN_CORE_OK &&
           markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK,
       "a session flushes headings, definitions and links");
    if (!session) {
        return;
    }
    const markdown_core_position into_b = {3, 2};
    OK(runner,
       markdown_core_session_edit(session, into_b, into_b, (const uint8_t *)"\n\n# a", 5, &document) ==
               MARKDOWN_CORE_OK &&
           session_as_parsed(session),
       "a heading the edit adds names those taken back after it");
    markdown_core_session_reread(session, &from, &to);
    OK(runner, to < markdown_core_session_text_size(session), "that edit took the children after it back");
    const markdown_core_position link = {11, 2};
    OK(runner,
       markdown_core_session_edit(session, link, link, (const uint8_t *)"y", 1, &document) == MARKDOWN_CORE_OK &&
           session_as_parsed(session),
       "an edit among the children taken back reads them as any other");
    const markdown_core_position head = {1, 3};
    OK(runner,
       markdown_core_session_edit(session, head, head, (const uint8_t *)"\n\n[x]: /v", 9, &document) ==
               MARKDOWN_CORE_OK &&
           session_as_parsed(session),
       "an edit that declares a label anew reads the whole text again");
    markdown_core_session_close(session);

    /* A block whose definitions were cut from its front starts past them,
     * and an edit reads it again from where they began. */
    static const char cut[] = "[x]: /a\n    a\na | b\n[x]: /a\n---\n";
    session = NULL;
    OK(runner,
       markdown_core_session_new((const uint8_t *)cut, sizeof(cut) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
               MARKDOWN_CORE_OK &&
           markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK,
       "a session flushes a heading whose definitions were cut");
    if (!session) {
        return;
    }
    const markdown_core_position last = {6, 1};
    OK(runner,
       markdown_core_session_edit(session, last, last, (const uint8_t *)"z", 1, &document) == MARKDOWN_CORE_OK &&
           session_as_parsed(session),
       "an edit after the heading reads it again from its definitions");
    markdown_core_session_close(session);

    /* An edit refused at any allocation leaves nothing behind once the
     * session closes, the blocks the edit took from the tree included. */
    static const char kept[] = "# a\n\nb\n\nc\n";
    bool clean = true, done = false;
    for (size_t fail = 1; fail < 400 && clean && !done; fail++) {
        payload_probe_arm();
        session = NULL;
        if (markdown_core_session_new((const uint8_t *)kept, sizeof(kept) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                      &session) != MARKDOWN_CORE_OK ||
            markdown_core_session_flush(session, &document) != MARKDOWN_CORE_OK) {
            clean = false;
        } else {
            const markdown_core_position into = {5, 1};
            payload_fail_at = payload_allocations + fail;
            done = markdown_core_session_edit(session, into, into, (const uint8_t *)"abc\n", 4, &document) ==
                   MARKDOWN_CORE_OK;
            payload_fail_at = 0;
        }
        markdown_core_session_close(session);
        payload_probe_disarm();
        clean = clean && payload_live == 0;
    }
    OK(runner, clean && done, "an edit refused at any allocation leaks nothing");

    /* An edit refused at any allocation leaves the session as a fresh
     * parse of its text has it, or refusing every edit after it, a label it
     * declares for the children before it included. */
    static const char labeled[] = "[x]\n\na\n\nb\n\nc\n";
    bool sound = true;
    done = false;
    for (size_t fail = 1; fail < 1000 && sound && !done; fail++) {
        payload_probe_arm();
        session = NULL;
        const markdown_core_position into = {8, 1};
        if (markdown_core_session_new((const uint8_t *)labeled, sizeof(labeled) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                      &session) != MARKDOWN_CORE_OK ||
            markdown_core_session_flush(session, &document) != MARKDOWN_CORE_OK) {
            sound = false;
        } else {
            payload_fail_at = payload_allocations + fail;
            done = markdown_core_session_edit(session, into, into, (const uint8_t *)"\n[x]: /a\n", 9, &document) ==
                   MARKDOWN_CORE_OK;
            payload_fail_at = 0;
            const markdown_core_status again = markdown_core_session_edit(session, into, into, NULL, 0, &document);
            sound = again == MARKDOWN_CORE_ALLOCATION_FAILED ? !done
                                                             : again == MARKDOWN_CORE_OK && session_as_parsed(session);
        }
        markdown_core_session_close(session);
        payload_probe_disarm();
        sound = sound && payload_live == 0;
    }
    OK(runner, sound && done, "an edit refused at any allocation leaves the session sound");

    /* The same of an edit that takes children back, with what they hold:
     * a heading, a definition and a link to it. */
    static const char spliced[] = "a\n\nb\n\n# h\n\n[x]: /u\n\n[x]\n";
    sound = true;
    done = false;
    for (size_t fail = 1; fail < 1000 && sound && !done; fail++) {
        payload_probe_arm();
        session = NULL;
        const markdown_core_position into = {3, 1};
        if (markdown_core_session_new((const uint8_t *)spliced, sizeof(spliced) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                      &session) != MARKDOWN_CORE_OK ||
            markdown_core_session_flush(session, &document) != MARKDOWN_CORE_OK) {
            sound = false;
        } else {
            payload_fail_at = payload_allocations + fail;
            done =
                markdown_core_session_edit(session, into, into, (const uint8_t *)"z", 1, &document) == MARKDOWN_CORE_OK;
            payload_fail_at = 0;
            from = to = 0;
            markdown_core_session_reread(session, &from, &to);
            sound = !done || to < markdown_core_session_text_size(session);
            const markdown_core_position back = {9, 2};
            const markdown_core_status again =
                markdown_core_session_edit(session, back, back, (const uint8_t *)"y", 1, &document);
            sound = sound && (again == MARKDOWN_CORE_ALLOCATION_FAILED
                                  ? !done
                                  : again == MARKDOWN_CORE_OK && session_as_parsed(session));
        }
        markdown_core_session_close(session);
        payload_probe_disarm();
        sound = sound && payload_live == 0;
    }
    OK(runner, sound && done, "an edit that takes children back, refused at any allocation, leaves the session sound");

    /* After an edit that took children back, an edit anywhere reads as a
     * fresh parse does: what it took back holds its lines, its reach, its
     * declarations and its headings in the new text's terms. */
    static const char twice[] = "# a\n\nb\n\n# a\n\n> q\n\n[x]: /u\n\n[x] [^n]\n\n[^n]: n\n";
    session = NULL;
    bool fresh = markdown_core_session_new((const uint8_t *)twice, sizeof(twice) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                           &session) == MARKDOWN_CORE_OK &&
                 markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
    const markdown_core_position seed = {3, 2};
    fresh = fresh &&
            markdown_core_session_edit(session, seed, seed, (const uint8_t *)"z", 1, &document) == MARKDOWN_CORE_OK;
    size_t lines = 0;
    for (size_t i = 0; i < sizeof(twice); i++) {
        lines += twice[i] == '\n';
    }
    for (int line = 1; fresh && line <= (int)lines; line++) {
        for (int column = 1; fresh; column++) {
            const markdown_core_position at = {line, column};
            const markdown_core_status status =
                markdown_core_session_edit(session, at, at, (const uint8_t *)"\n", 1, &document);
            if (status == MARKDOWN_CORE_OUT_OF_BOUNDS) {
                break;
            }
            const markdown_core_position next = {line + 1, 1};
            fresh = status == MARKDOWN_CORE_OK && session_as_parsed(session) &&
                    markdown_core_session_edit(session, at, next, NULL, 0, &document) == MARKDOWN_CORE_OK &&
                    session_as_parsed(session);
        }
    }
    OK(runner, fresh, "an edit anywhere after one that took children back reads as a fresh parse");
    markdown_core_session_close(session);
}

/* The anchor `node` has, compared with `expected`. */
static bool anchor_is(const markdown_core_node *node, const char *expected) {
    markdown_core_optional_string anchor = node ? anchor_of(node) : (markdown_core_optional_string){0};
    return anchor.has_value && anchor.value.length == strlen(expected) &&
           memcmp(anchor.value.data, expected, anchor.value.length) == 0;
}

/* The first heading under the document's first block, or inside its first
 * item. */
static const markdown_core_node *first_heading(const markdown_core_document *document) {
    const markdown_core_node *heading = facade_child(facade_child(markdown_core_document_root(document), 0), 0);
    if (heading && markdown_core_node_get_kind(heading) == MARKDOWN_CORE_KIND_LIST_ITEM) {
        heading = facade_child(heading, 0);
    }
    return heading;
}

/* A snapshot's headings take their anchors as a fresh parse's do: every
 * explicit anchor of the text is reserved first, a later one included, and
 * one of a block the text ends in too (docs/specs/dialect/anchors.md). */
static void session_snapshot_anchors(test_batch_runner *runner) {
    static const struct {
        const char *source;
        const char *message;
    } cases[] = {
        {":::note{#x}\n# x\n:::\n", "a heading inside a container avoids the container's anchor"},
        {"- # x\n\n#x#\n", "a heading avoids an explicit anchor written after it"},
        {"- # x\n\n:::note{#x}\nbody\n", "a heading avoids the anchor of a container the text ends in"},
        {"- # x\n\n- a #x#\n", "a heading avoids the anchor an open item takes from its paragraph"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        markdown_core_session *session = NULL;
        if (markdown_core_session_new((const uint8_t *)cases[i].source, strlen(cases[i].source),
                                      MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) != MARKDOWN_CORE_OK) {
            OK(runner, 0, "a session opens on %s", cases[i].source);
            continue;
        }
        OK(runner, anchor_is(first_heading(markdown_core_session_document(session)), "x-1"), "snapshot: %s",
           cases[i].message);
        const markdown_core_document *final = NULL;
        OK(runner,
           markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK && anchor_is(first_heading(final), "x-1"),
           "final: %s", cases[i].message);
        markdown_core_session_close(session);
    }
}

/* The canonical dump of `document` over `source`, or NULL. */
static uint8_t *dump_of(const markdown_core_document *document, const char *source, size_t length,
                        size_t *dump_length) {
    uint8_t *dump = NULL;
    if (!document ||
        markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)source, length,
                                    &dump, dump_length) != MARKDOWN_CORE_OK) {
        return NULL;
    }
    return dump;
}

/* Whether a session opened with the first `cut` bytes of `source` and fed
 * the rest in chunks of `chunk` bytes, or line by line when it is 0,
 * flushes to the dump of a fresh parse of it. */
static bool splits_as_a_parse(const char *source, size_t cut, size_t chunk) {
    const size_t length = strlen(source);
    size_t expected_length = 0, actual_length = 0;
    markdown_core_document *parsed = NULL;
    uint8_t *expected = markdown_core_document_parse((const uint8_t *)source, length, &parsed) == MARKDOWN_CORE_OK
                            ? dump_of(parsed, source, length, &expected_length)
                            : NULL;
    markdown_core_session *session = NULL;
    bool appended = markdown_core_session_new((const uint8_t *)source, cut, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                    MARKDOWN_CORE_OK;
    for (size_t at = cut; appended && at < length;) {
        size_t step = chunk;
        if (!step) {
            const char *newline = memchr(source + at, '\n', length - at);
            step = newline ? (size_t)(newline - (source + at)) + 1 : length - at;
        }
        step = step < length - at ? step : length - at;
        const markdown_core_document *document = NULL;
        appended =
            markdown_core_session_append(session, (const uint8_t *)source + at, step, &document) == MARKDOWN_CORE_OK;
        at += step;
    }
    const markdown_core_document *final = NULL;
    uint8_t *actual = appended && markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK
                          ? dump_of(final, source, length, &actual_length)
                          : NULL;
    const bool same =
        expected && actual && expected_length == actual_length && memcmp(expected, actual, expected_length) == 0;
    markdown_core_dump_free(actual);
    markdown_core_session_close(session);
    markdown_core_dump_free(expected);
    markdown_core_document_free(parsed);
    return same;
}

/* The same of a session opened empty. */
static bool flushes_as_a_parse(const char *source, size_t chunk) { return splits_as_a_parse(source, 0, chunk); }

/* A STREAMED SESSION FINALIZES TO WHAT A FRESH PARSE OF ITS TEXT IS (plan
 * 5.4, 5.10). The text arrives byte by byte, three bytes at a time and line
 * by line. A block that completes at a commit sees only the definitions
 * written before it, so each use below is defined after it, past enough
 * paragraphs that the use's block completes at a commit before the
 * definition arrives; flush looks the use up again. The uses sit in each
 * holder of inline content: a paragraph, a heading and its trailing
 * attributes, a callout title, a definition term, a table caption, a pipe
 * cell and a grid cell, a list item inside a quote, and a heading inside a
 * container. Some uses stay undefined, and a heading whose anchor depends on
 * whether its use resolves comes before a heading of the resolved text. A
 * grid table followed by a paragraph of several lines appends to the
 * paragraph's content after the table's cells. */
static void session_flush_matches_a_parse(test_batch_runner *runner) {
#define FILLER "\n\nfiller one\n\nfiller two\n\nfiller three\n\n"
    static const char *const sources[] = {
        "See [x], [^n] and @s." FILLER "[x]: /u\n\n[^n]: note\n\n(@s) sample\n",
        "# Head [x]\n\ntext" FILLER "[x]: /u\n",
        "# Title [x][]{#a}\n\n# a" FILLER "[x]: /u\n",
        "# Head [foo][x]" FILLER "# Head foo\n\n[x]: /u\n",
        "# Head @s here" FILLER "(@s) sample\n",
        "> [!NOTE] Title [x]\n> body" FILLER "[x]: /u\n",
        "Term [x]\n: def" FILLER "[x]: /u\n",
        "Table: Caption [x]\n\n| a | b |\n|---|---|\n| c | d |" FILLER "[x]: /u\n",
        "| a | b |\n|---|---|\n| [x] | y |" FILLER "[x]: /u\n",
        "+---+---+\n| a | b |\n+===+===+\n| [x] | y |\n+---+---+\n\npara line one\nline two\nline three" FILLER
        "[x]: /u\n",
        "> - item [x] and [y]" FILLER "[x]: /u\n",
        ":::note{#d}\n# x [x]\n:::" FILLER "# x\n\n[x]: /u\n",
        "See [^n] here." FILLER "[^n]: note\n",
        "See [^n]{#c} and [x][y]{#a}." FILLER "# a\n\n# c\n\n[^n]: note\n\n[y]: /u\n",
        "- a\n  - # A [x]\n  - > b [y]\n    > - # A [x]\n- c" FILLER ":n{#a}\n\n# A\n\n[x]: /u\n\n[y]: /v\n",
        /* A `$` pair whose body a code span begins makes no formula, but
         * takes the `*` between away: no settle point lies inside it. */
        "a\n$`\n`*$*",
        "a\n$`b\n`*b$*\n",
        /* A quote whose closer the text has not reached yet leaves the
         * attribute block open past the brace it closes at for now. */
        "a\n:d{b='\n}c'x",
        /* A topic's name reads the character after it whole: a four-byte
         * lead the text cuts short may yet be part of the name. */
        "%%%%\ns\n~\x9b~#\xc8\xdd\xf2"
        "3\xc2\xe3",
        /* A `$` the emphasis around it takes away still made each `$` after
         * it a closer: no settle point lies past it. */
        "a\n*$`m\n`*$x$",
    };
#undef FILLER
    static const size_t chunks[] = {1, 3, 0};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            OK(runner, flushes_as_a_parse(sources[i], chunks[c]), "a session streamed in %s flushes as a parse: %s",
               chunks[c] == 1   ? "bytes"
               : chunks[c] == 3 ? "three bytes"
                                : "lines",
               sources[i]);
        }
    }
}

/* A SNAPSHOT WALKS THROUGH A DEFINITION'S BODY, which is no node of the
 * tree but a relation of its definition: the commits change the body, as a
 * list closes inside it and the text goes on below the list, or a block
 * open inside it, a quote or a list. The session opens with every prefix of
 * the text and takes the rest in chunks of one, two, three and five bytes. */
static void session_walks_through_definition_bodies(test_batch_runner *runner) {
    static const char *const sources[] = {
        "m\n:\n  -\n\n  d|\n",
        "Term\n:\n  - item\n\n  more text|\n",
        "m\n: > a\n  > b\n  > c\n\n  > d\n  > e\n",
        "m\n: - a\n\n  - b\n\n    c\n    d\n",
    };
    static const size_t chunks[] = {1, 2, 3, 5};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const size_t length = strlen(sources[i]);
        bool same = true;
        size_t cut = 0, c = 0;
        for (cut = 0; same && cut < length; cut++) {
            for (c = 0; same && c < sizeof(chunks) / sizeof(chunks[0]); c++) {
                same = splits_as_a_parse(sources[i], cut, chunks[c]);
            }
        }
        OK(runner, same, "a session split through a definition's body flushes as a parse, at byte %zu: %s",
           same ? length : cut - 1, sources[i]);
    }
}

/* EVERY SNAPSHOT NAMES ITS HEADINGS AS A PARSE OF THE TEXT SO FAR DOES
 * (step 7, 4.5): the text arrives byte by byte, and after each append the
 * snapshot's dump is a fresh parse's of the same text. An explicit anchor
 * written after headings that took its name displaces the first of them,
 * which displaces the next along its own sequence: while the anchor is in a
 * snapshot's tail, a committed heading shows another name, and the commit
 * that reaches the anchor gives it that name for good. An anchor in an
 * unclosed code span is gone once the span closes, and the heading shows its
 * own name again. The headings sit at the top, in a footnote, as a setext
 * heading the last paragraph becomes, and with authored anchors. No text
 * here looks a definition up, so a snapshot's lookups change nothing. */
static void session_snapshot_names(test_batch_runner *runner) {
    static const char *const sources[] = {
        "# A\n\n# A\n\n# A\n\ntext :n{#a} more\n\n# A\n",
        "# A\n\none\n\ntwo\n\npara `x :n{#a}\nline` after\n\n# A\n\nthree\n\n`:n{#a-1}` and :n{#a-2}\n",
        "# A\n\n# A-1\n\n# A\n\n:n{#a}\n\n# A-1\n",
        "[^n]: # A\n\n    # A\n\n# A\n\n:n{#a}\n",
        "# A\n\n# A\n\n# A\n\n# A\n\n# A\n\n# A\n\n# A\n\n:n{#a-2}\n\n# A\n\n:n{#a}\n",
        "# A\n\n# B {#a}\n\n# A\n\nA\n=\n\nA {#a-1}\n-\n",
        "- # A\n\n  # A\n\n> :n{#a-1}\n> # A\n",
        "- a\n  - # A\n    text\n  - > # A\n    > - # A\n- # A\n\npara\n\n:n{#a}\n\n:n{#a-2}\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const char *source = sources[i];
        const size_t length = strlen(source);
        markdown_core_session *session = NULL;
        bool same = markdown_core_session_new((const uint8_t *)source, 0, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                    MARKDOWN_CORE_OK;
        size_t at = 0;
        while (same && at < length) {
            const markdown_core_document *document = NULL;
            same =
                markdown_core_session_append(session, (const uint8_t *)source + at, 1, &document) == MARKDOWN_CORE_OK;
            at++;
            markdown_core_document *parsed = NULL;
            size_t expected_length = 0, actual_length = 0;
            uint8_t *expected = markdown_core_document_parse((const uint8_t *)source, at, &parsed) == MARKDOWN_CORE_OK
                                    ? dump_of(parsed, source, at, &expected_length)
                                    : NULL;
            uint8_t *actual = same ? dump_of(document, source, at, &actual_length) : NULL;
            same = expected && actual && expected_length == actual_length &&
                   memcmp(expected, actual, expected_length) == 0;
            markdown_core_dump_free(expected);
            markdown_core_dump_free(actual);
            markdown_core_document_free(parsed);
        }
        const markdown_core_document *final = NULL;
        OK(runner, same && markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK,
           "every snapshot names its headings as a parse of its text, up to byte %zu of %zu: %s", at, length, source);
        markdown_core_session_close(session);
    }
}

/* WHERE A NODE IS in the source, which names a node of one document in
 * another parsed from the same source even where their trees differ: its
 * kind, and the start of its first run and the end of its last; the kind
 * alone, zero, for NULL. */
typedef struct {
    int kind;
    markdown_core_position start;
    markdown_core_position end;
} node_where;

static bool node_where_of(const markdown_core_document *document, const markdown_core_node *node, const char *source,
                          size_t length, node_where *where) {
    memset(where, 0, sizeof(*where));
    if (!node) {
        return true;
    }
    markdown_core_scope *scopes = NULL;
    size_t count = 0;
    if (markdown_core_document_scope(document, node, (const uint8_t *)source, length, &scopes, &count) !=
            MARKDOWN_CORE_OK ||
        !count) {
        markdown_core_scopes_free(scopes);
        return false;
    }
    where->kind = (int)markdown_core_node_get_kind(node) + 1;
    where->start = scopes[0].start;
    where->end = scopes[count - 1].end;
    markdown_core_scopes_free(scopes);
    return true;
}

/* The label of a definition of table `table` (0 Footnotes, 1 Specimens, 2
 * References), and whether it has one. */
static bool definition_label_of(int table, const markdown_core_node *node, markdown_core_string *label) {
    markdown_core_optional_string optional = {0};
    markdown_core_optional_i64 start;
    if (table == 2) {
        optional.has_value = markdown_core_reference_label(node, &optional.value) == MARKDOWN_CORE_OK;
    } else if (table == 1) {
        markdown_core_specimen_properties(node, &optional, &start);
    } else {
        markdown_core_footnote_label(node, &optional);
    }
    *label = optional.value;
    return optional.has_value;
}

static size_t definition_count(int table, const markdown_core_document *document) {
    return table == 0   ? markdown_core_document_footnote_count(document)
           : table == 1 ? markdown_core_document_specimen_count(document)
                        : markdown_core_document_reference_count(document);
}

static const markdown_core_node *definition_entry(int table, const markdown_core_document *document, size_t index) {
    const markdown_core_node *node = NULL;
    const markdown_core_status status = table == 0   ? markdown_core_document_footnote_at(document, index, &node)
                                        : table == 1 ? markdown_core_document_specimen_at(document, index, &node)
                                                     : markdown_core_document_reference_at(document, index, &node);
    return status == MARKDOWN_CORE_OK ? node : NULL;
}

static const markdown_core_node *definition_lookup(int table, const markdown_core_document *document,
                                                   markdown_core_string label) {
    return table == 0   ? markdown_core_document_footnote_for(document, label)
           : table == 1 ? markdown_core_document_specimen_for(document, label)
                        : markdown_core_document_reference_for(document, label);
}

/* Whether two documents parsed from `length` bytes of `source` have tables
 * that agree, entry for entry, by where their nodes are: every list, the
 * lookup of every label a list holds and of a label none does, and the
 * reference label table. */
static bool same_node(const markdown_core_document *actual, const markdown_core_node *mine,
                      const markdown_core_document *expected, const markdown_core_node *theirs, const char *source,
                      size_t length) {
    node_where a, b;
    return node_where_of(actual, mine, source, length, &a) && node_where_of(expected, theirs, source, length, &b) &&
           a.kind == b.kind && a.start.line == b.start.line && a.start.column == b.start.column &&
           a.end.line == b.end.line && a.end.column == b.end.column;
}

static bool tables_agree(const markdown_core_document *actual, const markdown_core_document *expected,
                         const char *source, size_t length) {
    bool same = true;
#define SAME_NODE(a, b) same_node(actual, (a), expected, (b), source, length)
    for (int table = 0; same && table < 3; table++) {
        const size_t count = definition_count(table, expected);
        same = definition_count(table, actual) == count;
        for (size_t i = 0; same && i < count; i++) {
            const markdown_core_node *entry = definition_entry(table, expected, i);
            markdown_core_string label;
            same = entry && SAME_NODE(definition_entry(table, actual, i), entry) &&
                   (!definition_label_of(table, entry, &label) ||
                    SAME_NODE(definition_lookup(table, actual, label), definition_lookup(table, expected, label)));
        }
        const markdown_core_string absent = {(const uint8_t *)"absent label", 12};
        same = same && !definition_lookup(table, actual, absent) && !definition_lookup(table, expected, absent);
    }
    const size_t labels = markdown_core_document_reference_label_count(expected);
    same = same && markdown_core_document_reference_label_count(actual) == labels;
    for (size_t i = 0; same && i < labels; i++) {
        markdown_core_string label, expected_label;
        const markdown_core_node *target = NULL, *expected_target = NULL;
        same = markdown_core_document_reference_label_at(actual, i, &label, &target) == MARKDOWN_CORE_OK &&
               markdown_core_document_reference_label_at(expected, i, &expected_label, &expected_target) ==
                   MARKDOWN_CORE_OK &&
               label.length == expected_label.length && !memcmp(label.data, expected_label.data, label.length) &&
               SAME_NODE(target, expected_target) &&
               SAME_NODE(markdown_core_document_reference_for(actual, label), expected_target);
    }
#undef SAME_NODE
    return same;
}

/* EVERY SNAPSHOT'S TABLES ARE A PARSE'S OF THE TEXT SO FAR (step 7, 4.5):
 * the text arrives byte by byte, and after each append the snapshot's
 * definition tables -- the lists, the lookups by label and the reference
 * labels -- name the nodes a fresh parse of the same text names, by where
 * they are in the text, since the snapshot's inline roots resolve only
 * definitions before them and its tree differs from the parse's. The committed definitions sit in the committed tables
 * and the tail's are the snapshot's own: footnotes nested in an open
 * footnote, inline notes, labels written twice, a Reference and Headings
 * declaring one label in either order, the tail's labels between committed
 * ones and over a committed Heading's, specimens, and a heading in a
 * footnote that a later anchor renames, which moves the footnote and the
 * heading to objects of their own while their entries stay committed. */
static void session_snapshot_tables(test_batch_runner *runner) {
    static const char *const sources[] = {
        "[^a]: one\n\n    [^b]: inner\n\n    more\n\n[^a]: again\n\ntext [^a] ^[inline] [^b]\n\n[^c]: last\n",
        "# Foo\n\n[foo]: /u\n\n# Foo\n\n[Foo] and [bar]\n\n[bar]: /b\n\n# Bar\n\n[foo]: /again\n",
        "[^n]: # A\n\n    # A\n\n# A\n\n[A] and [a-1]\n\n:n{#a}\n\n[x]: /x\n",
        "(5@etude) Body.\n(@) Anonymous.\n\n(@etude) Again.\n\n@etude and (@next) Next.\n",
        "- [r]: /r\n- # R\n\n> [^q]: quote\n>\n> # Q\n\n[R] [Q] [^q]\n",
        "[a]: /a\n\n# D\n\n[c]: /c\n\n# G\n\n[b]: /b\n[d]: /d\n[f]: /f\n\n[B] [D] [G]\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const char *source = sources[i];
        const size_t length = strlen(source);
        markdown_core_session *session = NULL;
        bool same = markdown_core_session_new((const uint8_t *)source, 0, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                    MARKDOWN_CORE_OK;
        size_t at = 0;
        while (same && at < length) {
            const markdown_core_document *document = NULL;
            same =
                markdown_core_session_append(session, (const uint8_t *)source + at, 1, &document) == MARKDOWN_CORE_OK;
            at++;
            markdown_core_document *parsed = NULL;
            same = same && markdown_core_document_parse((const uint8_t *)source, at, &parsed) == MARKDOWN_CORE_OK &&
                   tables_agree(document, parsed, source, at);
            markdown_core_document_free(parsed);
        }
        const markdown_core_document *final = NULL;
        markdown_core_document *parsed = NULL;
        same = same && markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK &&
               markdown_core_document_parse((const uint8_t *)source, length, &parsed) == MARKDOWN_CORE_OK &&
               tables_agree(final, parsed, source, length);
        OK(runner, same, "every snapshot's tables are a parse's of its text, up to byte %zu of %zu: %s", at, length,
           source);
        markdown_core_document_free(parsed);
        markdown_core_session_close(session);
    }
}

/* The lists of a document in canonical walk order, into `lists`, which
 * holds `capacity`; their count, or SIZE_MAX when the walk could not
 * allocate. */
static size_t document_lists(const markdown_core_document *document, const markdown_core_node **lists,
                             size_t capacity) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    size_t count = 0;
    markdown_core_walk_begin(&walk, markdown_core_document_root(document));
    while (markdown_core_walk_next(&walk, &item)) {
        if (item.node && markdown_core_node_get_kind(item.node) == MARKDOWN_CORE_KIND_LIST && count < capacity) {
            lists[count++] = item.node;
        }
    }
    const bool failed = walk.failed;
    markdown_core_walk_end(&walk);
    return failed ? SIZE_MAX : count;
}

static bool list_tight(const markdown_core_node *list) {
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start;
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    bool tight = false;
    markdown_core_node_list_properties(list, &flavor, &start, &variant, &delimiter, &tight);
    return tight;
}

/* EVERY SNAPSHOT LAYS ITS LISTS OUT AS A PARSE OF ITS TEXT (step 7, plan
 * 5.6): the text arrives byte by byte, and after each append every list of
 * the snapshot is where a fresh parse of the same text has one, and is
 * tight exactly when that one is. Each item and each child an item holds
 * folds into its list as it settles, and a snapshot closes copies of the
 * open list and item whose folds hold the committed children: a blank line
 * between items or between an item's children, a last item or child ending
 * with one, a nested list ending with one, a reference definition the
 * blank-line facts read through, a blank line inside a fence, an empty item
 * and lists that a later line ends. */
static void session_snapshot_lists(test_batch_runner *runner) {
    static const char *const sources[] = {
        "- a\n- b\n- c\n\nafter\n",    "- a\n\n- b\n- c\n",          "- a\n\n  b\n- c\n\n  d\n",
        "- a\n  - b\n\n- c\n",         "- a\n  - b\n\n  c\n",        "- a\n\n  [r]: /r\n- b\n\n- [s]: /s\n\n  e\n",
        "- a\n\n  [r]: /r\n\n[r]\n",   "- ```\n  x\n\n  ```\n- b\n", "-\n- b\n\n-\n\n  c\n",
        "1. a\n2. b\n\n\n3. c\n- d\n",
    };
    enum { LISTS = 16 };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const char *source = sources[i];
        const size_t length = strlen(source);
        markdown_core_session *session = NULL;
        bool same = markdown_core_session_new((const uint8_t *)source, 0, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                    MARKDOWN_CORE_OK;
        size_t at = 0;
        while (same && at <= length) {
            const markdown_core_document *document = NULL;
            if (at < length) {
                same = markdown_core_session_append(session, (const uint8_t *)source + at, 1, &document) ==
                       MARKDOWN_CORE_OK;
            } else {
                same = markdown_core_session_flush(session, &document) == MARKDOWN_CORE_OK;
            }
            const size_t parsed_length = at < length ? at + 1 : length;
            markdown_core_document *parsed = NULL;
            same = same &&
                   markdown_core_document_parse((const uint8_t *)source, parsed_length, &parsed) == MARKDOWN_CORE_OK;
            const markdown_core_node *mine[LISTS], *theirs[LISTS];
            const size_t count = same ? document_lists(parsed, theirs, LISTS) : 0;
            same = same && count < LISTS && document_lists(document, mine, LISTS) == count;
            for (size_t k = 0; same && k < count; k++) {
                same = same_node(document, mine[k], parsed, theirs[k], source, parsed_length) &&
                       list_tight(mine[k]) == list_tight(theirs[k]);
            }
            markdown_core_document_free(parsed);
            at++;
        }
        OK(runner, same, "every snapshot lays its lists out as a parse of its text, up to byte %zu of %zu: %s", at,
           length, source);
        markdown_core_session_close(session);
    }
}

/* Whether `document` dumps as a fresh parse of the first `length` bytes of
 * `source` does. */
static bool dumps_as_a_parse(const markdown_core_document *document, const char *source, size_t length) {
    markdown_core_document *parsed = NULL;
    size_t expected_length = 0, actual_length = 0;
    uint8_t *expected = markdown_core_document_parse((const uint8_t *)source, length, &parsed) == MARKDOWN_CORE_OK
                            ? dump_of(parsed, source, length, &expected_length)
                            : NULL;
    uint8_t *actual = dump_of(document, source, length, &actual_length);
    const bool same =
        expected && actual && expected_length == actual_length && memcmp(expected, actual, expected_length) == 0;
    markdown_core_dump_free(expected);
    markdown_core_dump_free(actual);
    markdown_core_document_free(parsed);
    return same;
}

/* Whether `source`, appended in pieces that end at each of the `count`
 * offsets `ends`, in order, gives after each append a snapshot whose dump is
 * a fresh parse's of the same text, and a flushed document whose dump is a
 * parse's of all of it; `at` is how many bytes were appended when it
 * stopped. */
static bool pieces_dump_as_parses(const char *source, const size_t *ends, size_t count, size_t *at) {
    markdown_core_session *session = NULL;
    bool same = markdown_core_session_new((const uint8_t *)source, 0, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) ==
                MARKDOWN_CORE_OK;
    *at = 0;
    for (size_t i = 0; same && i < count; i++) {
        const markdown_core_document *document = NULL;
        same = markdown_core_session_append(session, (const uint8_t *)source + *at, ends[i] - *at, &document) ==
                   MARKDOWN_CORE_OK &&
               dumps_as_a_parse(document, source, ends[i]);
        *at = ends[i];
    }
    const markdown_core_document *final = NULL;
    same = same && markdown_core_session_flush(session, &final) == MARKDOWN_CORE_OK &&
           dumps_as_a_parse(final, source, *at);
    markdown_core_session_close(session);
    return same;
}

/* The same of `source` appended byte by byte. */
static bool snapshots_dump_as_parses(const char *source, size_t *at) {
    const size_t length = strlen(source);
    size_t *ends = malloc((length ? length : 1) * sizeof(*ends));
    for (size_t i = 0; ends && i < length; i++) {
        ends[i] = i + 1;
    }
    const bool same = ends && pieces_dump_as_parses(source, ends, length, at);
    free(ends);
    return same;
}

/* WHAT A BLOCK'S OPENING LINE GIVES IT, IN EVERY SNAPSHOT (step 7, THE
 * COPIES): the text arrives byte by byte, and after each append the
 * snapshot's dump is a fresh parse's of the same text. The blocks own what
 * their opening lines give them -- the attributes and name of a directive,
 * the info string and attributes of a fence, a callout's variant, a
 * footnote's label, a table's columns, a task's marker -- which the copies a
 * snapshot reads view, while the block is open, while it is the pending
 * block a blank line or an identifier still writes, and after a commit
 * closes it, settles it or makes a formula of it. No text here looks a
 * definition up, so a snapshot's lookups change nothing. */
static void session_snapshot_openers(test_batch_runner *runner) {
    static const char *const sources[] = {
        ":::note{#d .c key=v}\nbody one\n\nbody two\n:::\n\nafter\n",
        ":::note[Label *x*]{#d}\nbody\n:::\n\n\n\nafter\n",
        "```js {#c .x key=v}\ncode one\ncode two\n```\n\nafter\n",
        "```formula {#f}\nx^2\n```\n\nafter\n",
        "> [!NOTE] Title *x*\n> body\n\nafter\n",
        "- > [!TIP] Tip title\n  > body\n- next\n",
        "[^n]: note one\n\n    note two\n\nafter\n",
        "| a | b |\n|---|---|\n| c | d |\n| e | f |\n\n#t#\n\nafter\n",
        "- [ ] task one #one#\n- [x] task two\n\n  more\n\nafter\n",
        "> ```js {#c}\n> code\n> ```\n>\n> after\n\n:::note{#d}\n> q\n:::\n",
        "(@s) sample one\n\n(@t) sample two\n",
        "Term\n: def one\n: def two\n\nafter\n",
        "<div>\nhtml\n</div>\n\n~~~ {.x}\ncode\n~~~\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        size_t at = 0;
        OK(runner, snapshots_dump_as_parses(sources[i], &at),
           "every snapshot holds what its blocks' opening lines give them as a parse of its text, up to byte %zu "
           "of %zu: %s",
           at, strlen(sources[i]), sources[i]);
    }
}

/* A SETTLED LOOKUP IS ASKED AGAIN AT THE FINALIZE (step 8, THE SETTLE
 * POINT): the text arrives byte by byte and flushes to a fresh parse's
 * dump. The paragraph settles a reference or note that finds nothing, and
 * the definition that ends the text, which only the flush completes,
 * defines it. */
static void session_settled_lookups_at_flush(test_batch_runner *runner) {
    static const char *const sources[] = {
        "[x]\na\nb\n\n[x]: /u",
        "a [x] b\nc\nd\ne\n\n[x]: /u \"t\"",
        "[x]\na\nb\n\n[x]:\n/u",
        "[^n] a\nb\nc\n\n[^n]: note",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        OK(runner, flushes_as_a_parse(sources[i], 1),
           "a settled lookup is asked again at the flush, as a parse of the text: %s", sources[i]);
    }
}

/* WHAT A CANDIDATE DECIDES, IN EVERY SNAPSHOT (step 7, THE CANDIDATES): the
 * text arrives byte by byte, and after each append the snapshot's dump is a
 * fresh parse's of the same text. A construct whose call reads to the end
 * of the text so far -- a grid, a multiline table with or without a header,
 * a headerless simple table, a `%%` comment, a block identifier line, a
 * definition's term and its body's blank line, the Properties envelope the
 * text opens with, a caption before or after a table -- is read on as the
 * commit reads its lines, and decided at the line that ends it, inside
 * containers too; one the text leaves open stays as its lines read. */
static void session_snapshot_candidates(test_batch_runner *runner) {
    static const char *const sources[] = {
        "%%\nhidden *x*\n\n- not a list\n%%\n\nafter\n",
        "> %%\n> a\n> %%\n> b\n\n%%\nopen\n",
        "- %%\n  x\n- y\n  %%\n",
        "%%\n%%x\n%\n %%  \nafter\n",
        "+---+---+\n| a | b |\n+---+---+\n| c | d |\n+---+---+\n\nafter\n",
        "+---+\n| a |\n|   |\n\npara\n",
        "-------------------\nHead A    Head B\n--------  --------\nrow a1    row b1\n\nrow a2    row b2\n"
        "-------------------\n\nafter\n",
        "----------- -------\n   First    row\n\n  Second    row\n-------------------\n\nafter\n",
        "------ ------\nx      y\n------ ------\nafter\n",
        "> -------------------\n> Head A    Head B\n> --------  --------\n> row a1    row b1\n>\n"
        "> row a2    row b2\n> -------------------\n>\n> after\n",
        "- a\n\n#id#\n  x\n",
        "- a\n\n#id#\n\nafter\n",
        "> [!NOTE]\n> x\n\n#id#\n x\n",
        "| a |\n|---|\n\n#id#\n\n#no#\n",
        "> - a\n>\n> #id#\n>  x\n",
        "Term\n: def\n\n  more\n\nafter\n",
        "Term\n\n: def\n\n\n  code?\nTwo\n:x\n",
        "> Term\n> : def\n>\n>   more\n> after\n",
        "Term\n: Caption\n\n| a |\n|---|\n| b |\n",
        "---\ntitle: a\n---\nbody\n",
        "---\ntitle: a\n\n- item\n> quote\n",
        "---\na: b\n----\n--\n -- -\n---\nafter\n",
        "---\n---\n",
        "---\r\na: b\r\n---\r\nx\r\n",
        "--- \na: b\n---\n",
        "x\n---\na: b\n---\n",
        "\n---\na: b\n---\n",
        "---\n%%\n---\n%%\nafter\n",
        "---\na: b\n---",
        "| a |\n|---|\n\nTable: ab\n\nafter\n",
        "| a |\n|---|\n| c |\n\nTable: ab\nmore\n\nx\n",
        "| a |\n|---|\n\nTable: ab\n| b |\n|---|\n\nTable: c\n",
        "Table: lead\n\n+---+\n| a |\n+---+\n\nafter\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        size_t at = 0;
        OK(runner, snapshots_dump_as_parses(sources[i], &at),
           "every snapshot decides a construct read on as a parse of its text, up to byte %zu of %zu: %s", at,
           strlen(sources[i]), sources[i]);
    }
}

/* A HELD LINE'S STEP READS TO THE END OF THE SCALAR IT TAKES (blocks.c, THE
 * LINE A SESSION HOLDS): the text arrives byte by byte, and after each append
 * the snapshot's dump is a fresh parse's of the same text. A column-counted
 * advance over a container's prefix -- a quote's `>` and the space after it,
 * a list item's, a footnote's and a specimen's indentation -- takes the
 * continuation bytes after the last byte it takes as that byte's scalar, so
 * its step on a line the text has not finished is decided once a byte that is
 * not one follows them. The bytes are malformed UTF-8, which the engine reads
 * as it reads any text. */
static void session_held_scalars(test_batch_runner *runner) {
    static const char *const sources[] = {
        ">\r> \x80\n",
        ">\n>\x80\x80"
        "b\n",
        "> a\n> \x80"
        "b\n",
        "> x\n>\x80\x80 b\n",
        "- a\n  \x80\x80"
        "b\n",
        "[^a]: x\n    \x80\x80"
        "b\n",
        "(@a) x\n    \x80\x80"
        "b\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        size_t at = 0;
        OK(runner, snapshots_dump_as_parses(sources[i], &at),
           "every snapshot reads a held prefix to its scalar's end as a parse of its text, up to byte %zu of %zu: %s",
           at, strlen(sources[i]), sources[i]);
    }
}

/* A CANDIDATE'S DECISION LEAVES THE LINES IT READS AS THE READING PLACED
 * THEM (blocks.c, THE CANDIDATES): a table's call on a quote's lazy line reads
 * the next line on the root's path, where the reading matched that line in
 * the quote, and the quote's paragraph, which spans it, takes its places from
 * where the reading found the line's own bytes. The text arrives byte by
 * byte, in the two pieces the lazy line ends, and whole, and each snapshot
 * and the flushed document dump as a parse of its text does. */
static void session_decisions_keep_lines(test_batch_runner *runner) {
    static const char *const sources[] = {">_\n:\n>a\n", ">_\r:\r>a"};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const size_t length = strlen(sources[i]);
        size_t at = 0;
        OK(runner, snapshots_dump_as_parses(sources[i], &at),
           "a decision leaves the lines it reads to the reading, byte by byte, up to byte %zu of %zu: %s", at, length,
           sources[i]);
        const size_t halves[] = {4, length};
        OK(runner, pieces_dump_as_parses(sources[i], halves, 2, &at),
           "a decision leaves the lines it reads to the reading, in two pieces, up to byte %zu of %zu: %s", at, length,
           sources[i]);
        const size_t whole[] = {length};
        OK(runner, pieces_dump_as_parses(sources[i], whole, 1, &at),
           "a decision leaves the lines it reads to the reading, whole: %s", sources[i]);
    }
}

/* WHAT AN ACCEPT TAKES AWAY (blocks.c, THE CANDIDATES) goes, and the next
 * publish matches what continues it against what the last publish recorded
 * of it as it placed it (ast.c, THE RECORDS), whatever the commit wrote in
 * it since: a simple table read after a multiline table's opening line, which
 * a publish places open, and which the commit then closes with a row no
 * publish placed, before the closing line accepts the multiline table; and
 * the same inside a quote. Each text arrives in two appends, split where the
 * first publish places the simple table, and each document dumps as a parse
 * of its text does. */
static void session_accept_after_commit_writes(test_batch_runner *runner) {
    static const struct {
        const char *source;
        size_t split;
    } cases[] = {
        {"-------\nh   j\n--- ---\na   b\nc   d\n\ne   f\n-------\n", 28},
        {"> -------\n> h   j\n> --- ---\n> a   b\n> c   d\n>\n> e   f\n> -------\n", 36},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const size_t ends[] = {cases[i].split, strlen(cases[i].source)};
        size_t at = 0;
        OK(runner, pieces_dump_as_parses(cases[i].source, ends, 2, &at),
           "a multiline table accepted after the commit closed the simple table a publish placed dumps as a parse, "
           "up to byte %zu: %s",
           at, cases[i].source);
    }
}

/* Where the bytes a block owns of each part a view of it views are: its
 * attributes' anchor, classes and records, its record's string and its
 * payload's storage. */
typedef struct {
    const void *anchor, *classes, *records, *text, *payload;
} owned_parts;

static owned_parts parts_of(const markdown_core_node *node) {
    owned_parts parts = {node->attributes.anchor.data, node->attributes.classes, node->attributes.records, NULL, NULL};
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        parts.text = node->as.code->info.value.data;
        break;
    case MARKDOWN_CORE_NODE_CALLOUT:
        parts.text = node->as.callout->variant.value.data;
        break;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        parts.text = node->as.list->task_marker.value.data;
        break;
    case MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK:
        parts.payload = markdown_core_elements_get_directive_name(node);
        break;
    case MARKDOWN_CORE_NODE_TABLE:
        parts.payload = ((const markdown_core_table *)node->opaque)->columns;
        break;
    default:
        break;
    }
    return parts;
}

/* Whether each part `a` holds is at the bytes `b` holds it at when `viewed`,
 * and at bytes of its own otherwise. */
static bool parts_alike(const owned_parts *a, const owned_parts *b, bool viewed) {
    const void *const x[] = {a->anchor, a->classes, a->records, a->text, a->payload};
    const void *const y[] = {b->anchor, b->classes, b->records, b->text, b->payload};
    for (size_t i = 0; i < sizeof(x) / sizeof(x[0]); i++) {
        if (x[i] && (x[i] == y[i]) != viewed) {
            return false;
        }
    }
    return true;
}

static bool chunks_equal(markdown_core_chunk a, markdown_core_chunk b) {
    return a.len == b.len && (!a.len || memcmp(a.data, b.data, (size_t)a.len) == 0);
}

/* Whether `a` holds what `b` does in each part a view views. */
static bool parts_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_attributes *x = &a->attributes, *y = &b->attributes;
    bool same = a->kind == b->kind && chunks_equal(x->anchor, y->anchor) && x->class_count == y->class_count &&
                x->record_count == y->record_count;
    for (uint32_t i = 0; same && i < x->class_count; i++) {
        same = chunks_equal(x->classes[i], y->classes[i]);
    }
    for (uint32_t i = 0; same && i < x->record_count; i++) {
        same = chunks_equal(x->records[i].name, y->records[i].name) &&
               chunks_equal(x->records[i].value, y->records[i].value);
    }
    if (!same) {
        return false;
    }
    switch (a->kind) {
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return chunks_equal(a->as.code->info.value, b->as.code->info.value);
    case MARKDOWN_CORE_NODE_CALLOUT:
        return chunks_equal(a->as.callout->variant.value, b->as.callout->variant.value);
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return chunks_equal(a->as.list->task_marker.value, b->as.list->task_marker.value);
    case MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK:
        return strcmp(markdown_core_elements_get_directive_name(a), markdown_core_elements_get_directive_name(b)) == 0;
    case MARKDOWN_CORE_NODE_TABLE: {
        const markdown_core_table *t = a->opaque, *u = b->opaque;
        return t->column_count == u->column_count &&
               memcmp(t->columns, u->columns, t->column_count * sizeof(*t->columns)) == 0;
    }
    default:
        return true;
    }
}

/* `pattern` with each `%s` in it the field `field`, in a new string the
 * caller frees. */
static char *with_field(const char *pattern, const char *field) {
    const size_t field_length = strlen(field);
    size_t length = 0;
    for (const char *p = pattern; *p; p++) {
        length += p[0] == '%' && p[1] == 's' ? (p++, field_length) : 1;
    }
    char *made = malloc(length + 1), *at = made;
    for (const char *p = pattern; made && *p; p++) {
        if (p[0] == '%' && p[1] == 's') {
            memcpy(at, field, field_length);
            at += field_length;
            p++;
        } else {
            *at++ = *p;
        }
    }
    if (made) {
        *at = '\0';
    }
    return made;
}

/* THE ROOTS A FINALIZE COMPLETES ANEW LIE AT EVERY DEPTH (blocks.c, A
 * LOOKUP THAT FOUND NOTHING), as do the headings a late anchor renames: in
 * three nested directives, each published open and then closed. The last
 * root of each directive completes as its closing line closes it, after the
 * roots that completed while it was open; a directive's label looks the late
 * definition up beside the roots inside it. Each text arrives byte by byte,
 * three bytes at a time and line by line, and its flush dumps as a parse
 * of the same text does. The headings look no definition up, so every
 * snapshot of their text dumps as a parse of the text so far does. */
static void session_roots_at_every_depth(test_batch_runner *runner) {
#define FILLER "\n\nfiller one\n\nfiller two\n\nfiller three\n\n"
    static const char *const sources[] = {
        ":::note\n\nouter [x]\n\n:::note\n\nmiddle [x]\n\n:::note\n\ninner one [x]\n\ninner two [x]\n:::\n\n"
        "middle again [x]\n:::\n\nouter again [x]\n:::" FILLER "[x]: /u\n",
        ":::note[Label [x]]\n\n:::note[Inner [x]]\n\nbody [x]\n\nlast [x]\n:::\n\nafter [x]\n:::" FILLER "[x]: /u\n",
    };
#undef FILLER
    static const char headings[] =
        ":::note\n\n# A\n\n:::note\n\n# A\n\n:::note\n\n# A\n\n# A\n:::\n\n# A\n:::\n\n# A\n:::\n\ntext :n{#a} more\n";
    static const size_t chunks[] = {1, 3, 0};
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        const char *const unit = chunks[c] == 1 ? "bytes" : chunks[c] == 3 ? "three bytes" : "lines";
        for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
            OK(runner, flushes_as_a_parse(sources[i], chunks[c]), "a session streamed in %s flushes as a parse: %s",
               unit, sources[i]);
        }
        OK(runner, flushes_as_a_parse(headings, chunks[c]), "a session streamed in %s flushes as a parse: %s", unit,
           headings);
    }
    size_t at = 0;
    const bool dumped = snapshots_dump_as_parses(headings, &at);
    OK(runner, dumped, "a session streamed byte by byte dumps as a parse, up to byte %zu: %s", at, headings);
}

/* A FIELD AN OPENING LINE WRITES COMPLETES ONCE ITS LINE IS READ (step 7,
 * plan 5.3): a callout's title, a definition's term, a block directive's
 * label and a table's caption, before or after its rows, each some
 * hundreds of bytes of the inline content a completion makes -- emphasis,
 * code, a reference, an inline note holding a reference, and an anchor. While its
 * block stays open over many appends, every snapshot holds it as a fresh
 * parse of the text so far does when each reference is defined before it.
 * When each is defined after it, the flush looks the field's references
 * up again and gives what a parse of the whole text gives: with the block
 * closed and published before the definition arrives, and with the block
 * still open when the text ends, the definition inside it -- in a quote, a
 * definition's body, an unclosed directive and a list item. */
static void session_opener_fields(test_batch_runner *runner) {
    char field[1024] = "";
    for (size_t length = 0; length + 40 < 300;) {
        length += (size_t)snprintf(field + length, sizeof(field) - length, "word *em* [x] ^[note [x]] `code` ");
    }
    strcat(field, ":n{#a} end");
    static const char *const defined_before[] = {
        "[x]: /u\n\n> [!NOTE] %s\n> body one\n>\n> body two\n>\n> body three\n\nafter\n",
        "[x]: /u\n\n%s\n: def one\n\n  def two\n\n  def three\n\nafter\n",
        "[x]: /u\n\n:::note[%s]\nbody one\n\nbody two\n\nbody three\n:::\n\nafter\n",
        "[x]: /u\n\nTable: %s\n\n| a | b |\n|---|---|\n| c | d |\n| e | f |\n| g | h |\n\nafter\n",
        "[x]: /u\n\n| a | b |\n|---|---|\n| c | d |\n| e | f |\n\nTable: %s\n\nafter\n",
    };
    for (size_t i = 0; i < sizeof(defined_before) / sizeof(defined_before[0]); i++) {
        char *source = with_field(defined_before[i], field);
        size_t at = 0;
        OK(runner, source && snapshots_dump_as_parses(source, &at),
           "every snapshot holds a long field its opening line wrote as a parse of its text, up to byte %zu: %s", at,
           defined_before[i]);
        free(source);
    }
    static const char *const defined_after[] = {
        "> [!NOTE] %s\n> body\n\nfiller one\n\nfiller two\n\n# a\n\n[x]: /u\n",
        "%s\n: def\n\nfiller one\n\nfiller two\n\n# a\n\n[x]: /u\n",
        ":::note[%s]\nbody\n:::\n\nfiller one\n\nfiller two\n\n# a\n\n[x]: /u\n",
        "Table: %s\n\n| a | b |\n|---|---|\n| c | d |\n\nfiller one\n\nfiller two\n\n# a\n\n[x]: /u\n",
        "| a | b |\n|---|---|\n| c | d |\n\nTable: %s\n\nfiller one\n\nfiller two\n\n# a\n\n[x]: /u\n",
        "> [!NOTE] %s\n> body\n>\n> filler\n>\n> # a\n>\n> [x]: /u\n",
        "%s\n: def\n\n  filler\n\n  # a\n\n  [x]: /u\n",
        ":::note[%s]\nbody\n\nfiller\n\n# a\n\n[x]: /u\n",
        "- > [!TIP] %s\n  > body\n  >\n  > # a\n  >\n  > [x]: /u\n",
    };
    static const size_t chunks[] = {1, 7, 0};
    for (size_t i = 0; i < sizeof(defined_after) / sizeof(defined_after[0]); i++) {
        char *source = with_field(defined_after[i], field);
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            OK(runner, source && flushes_as_a_parse(source, chunks[c]),
               "a session streamed in %s flushes a long field defined after it as a parse: %s",
               chunks[c] == 1   ? "bytes"
               : chunks[c] == 7 ? "seven bytes"
                                : "lines",
               defined_after[i]);
        }
        free(source);
    }
}

/* A VIEW VIEWS WHAT ITS BLOCK OWNS (markdown_core_node_view; blocks.c, THE
 * LENDING): it holds the block's bytes of each part, and the block's
 * children array, content buffer and field nodes, and letting it go and
 * releasing it leaves the block whole. */
static void node_view_views(test_batch_runner *runner) {
    static const char source[] = "```js {#c .x key=v}\ncode\n```\n\n"
                                 ":::note[label]{#d .y k=w}\nbody\n:::\n\n"
                                 "> [!NOTE] Title\n> callout\n\n"
                                 "- [x] done\n\n"
                                 "| a | b |\n|:-|-:|\n| 1 | 2 |\n";
    enum { BLOCKS = 5 };
    markdown_core_document *viewed = NULL, *kept = NULL;
    if (markdown_core_document_parse((const uint8_t *)source, sizeof(source) - 1, &viewed) != MARKDOWN_CORE_OK ||
        markdown_core_document_parse((const uint8_t *)source, sizeof(source) - 1, &kept) != MARKDOWN_CORE_OK) {
        OK(runner, false, "the viewed blocks parse");
        markdown_core_document_free(viewed);
        return;
    }
    const markdown_core_node *blocks[BLOCKS], *alike[BLOCKS];
    for (size_t i = 0; i < BLOCKS; i++) {
        blocks[i] = child_at(markdown_core_document_root(viewed), i);
        alike[i] = child_at(markdown_core_document_root(kept), i);
        if (blocks[i]->kind == MARKDOWN_CORE_NODE_LIST) {
            blocks[i] = child_at(blocks[i], 0);
            alike[i] = child_at(alike[i], 0);
        }
    }
    markdown_core_node_pool pool = {0};
    for (size_t i = 0; i < BLOCKS; i++) {
        const owned_parts original = parts_of(blocks[i]);
        markdown_core_node *view = markdown_core_node_view(&pool, blocks[i]);
        const owned_parts parts = view ? parts_of(view) : (owned_parts){0};
        OK(runner,
           view && parts_alike(&original, &parts, true) && parts_equal(view, alike[i]) &&
               view->children == blocks[i]->children && view->child_count == blocks[i]->child_count &&
               view->content.ptr == blocks[i]->content.ptr,
           "a view of block %zu holds its block's bytes of each part and its storage", i);
        if (view) {
            markdown_core_node_view_let_go(&pool, view, blocks[i]);
            markdown_core_node_pool_release(&pool, view);
        }
        OK(runner, parts_equal(blocks[i], alike[i]) && blocks[i]->child_count == alike[i]->child_count,
           "letting a view of block %zu go leaves its block whole", i);
    }
    markdown_core_node_pool_dispose(&pool);
    markdown_core_document_free(viewed);
    markdown_core_document_free(kept);
}

/* The allocator-seam sweep (plan 8, gates 4.7): an append and a flush
 * fail at every allocation, report ALLOCATION_FAILED, and the session frees
 * everything it holds. */
static void session_allocation_failures(test_batch_runner *runner) {
    static const char source[] = "# Head\n\nSome *text* [a][r].\n\n- item\n- item\n\n[r]: /url\n";
    payload_probe_arm();
    size_t needed = 0;
    int failed = 1;
    for (size_t fail = 0; fail == 0 || fail <= needed; fail++) {
        markdown_core_session *session = NULL;
        const markdown_core_document *document = NULL;
        if (markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                      &session) != MARKDOWN_CORE_OK) {
            failed = 0;
            break;
        }
        size_t before = payload_allocations;
        payload_fail_at = fail ? before + fail : 0;
        markdown_core_status status =
            markdown_core_session_append(session, (const uint8_t *)"more *text*\n", 12, &document);
        payload_fail_at = 0;
        if (!fail) {
            needed = payload_allocations - before;
            failed &= status == MARKDOWN_CORE_OK;
        } else {
            failed &= status == MARKDOWN_CORE_ALLOCATION_FAILED;
        }
        markdown_core_session_close(session);
        failed &= payload_live == 0;
    }
    OK(runner, needed > 0 && failed, "an append fails at each of its %zu allocations and leaks nothing", needed);
    needed = 0;
    failed = 1;
    for (size_t fail = 0; fail == 0 || fail <= needed; fail++) {
        markdown_core_session *session = NULL;
        const markdown_core_document *document = NULL;
        if (markdown_core_session_new((const uint8_t *)source, sizeof(source) - 1, MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                      &session) != MARKDOWN_CORE_OK) {
            failed = 0;
            break;
        }
        size_t before = payload_allocations;
        payload_fail_at = fail ? before + fail : 0;
        markdown_core_status status = markdown_core_session_flush(session, &document);
        payload_fail_at = 0;
        if (!fail) {
            needed = payload_allocations - before;
            failed &= status == MARKDOWN_CORE_OK;
        } else {
            failed &= status == MARKDOWN_CORE_ALLOCATION_FAILED;
        }
        markdown_core_session_close(session);
        failed &= payload_live == 0;
    }
    OK(runner, needed > 0 && failed, "a flush fails at each of its %zu allocations and leaks nothing", needed);
    payload_probe_disarm();
}

int main(void) {
    int retval;
    test_batch_runner *runner = test_batch_runner_new();

    universal_values(runner);
    absorbed_text_completes_once(runner);
    strbuf_growth_preserves_termination(runner);
    node_reuse_initializes_the_active_record(runner);
    block_content_storage_follows_writes(runner);
    node_slots_come_from_slabs_and_go_back_to_the_pool(runner);
    resource_slots_come_from_slabs_and_outlive_the_pool(runner);
    heading_reference_resolves_to_the_heading(runner);
    class_runs_split_on_ascii_white_space(runner);
    unicode_classes_are_unicode_17(runner);
    utf8_decode_reads_every_scalar(runner);
    utf8_decode_is_total(runner);
    unicode_classes_are_total(runner);
    anchor_images_only_scalars(runner);
    whitespace_is_space_tab_and_line_ending(runner);
    roster_records_are_carved_from_its_blocks(runner);
    key_tree_matches_a_sorted_model(runner);
    roster_kinds_and_starts(runner);
    roster_rollback_matches_a_model(runner);
    properties_values(runner);
    properties_source_boundaries(runner);
    properties_nul(runner);
    properties_member_work(runner);
    peers_resolve_at_seal(runner);
    properties_text_memory(runner);
    block_identifier_linear_work(runner);
    callout_linear_work(runner);
    image_dimension_linear_work(runner);
    block_identifier_ownership(runner);
    reference_definition_lifetime(runner);
    reference_label_normal_form(runner);
    image_cursor_stops_at_buffer_limit(runner);
    attribute_linear_work(runner);
    attribute_recognition_is_memoised(runner);
    open_members_are_memoised(runner);
    attribute_unclosed_quote_is_unquoted(runner);
    attribute_reads_refuse_every_allocation(runner);
    attribute_values_are_one_allocation(runner);
    source_entries_order_by_the_key_bytes_that_differ(runner);
    delimiter_entries_are_pooled_across_inline_containers(runner);
    releasing_an_empty_attribute_value_makes_no_allocator_call(runner);
    inline_placement_probes_the_map_a_bounded_number_of_times(runner);
    consolidation_keeps_a_view_when_its_operands_are_views(runner);
    placement_from_the_cursor_agrees_with_the_span(runner);
    an_undeclared_first_byte_reaches_no_gated_scanner(runner);
    the_list_scanner_reads_a_numeral_by_its_first_bytes_class(runner);
    attribute_attachment_linear_work(runner);
    heading_completion_invariants(runner);
    heading_roster_invariants(runner);
    reference_occurrences_hold_the_label_alone(runner);
    heading_label_length_boundary(runner);
    autolink_domain_linear_work(runner);
    deep_inline_construction(runner);
    ordered_numeral_ceiling(runner);
    citation_sparse_brace_storage(runner);
    definition_list_linear_work(runner);
    definition_gate_reads_the_prefix_not_the_line(runner);
    container_prefix_is_projected_from_the_elements(runner);
    a_prefix_byte_that_is_a_marker_byte_hands_the_line_to_the_transaction(runner);
    the_dialect_refuses_the_element_the_projection_could_not_index(runner);
    each_instance_parses_with_its_own_dialect(runner);
    citation_linear_work(runner);
    cross_link_linear_work(runner);
    inline_footnote_linear_work(runner);
    footnote_registration(runner);
    footnote_removal(runner);
    speculative_probe_allocations(runner);
    literal_text_allocations(runner);
    core_delimiter_stack_eligibility(runner);
    mark_linear_work(runner);
    insertion_linear_work(runner);
    span_and_script_linear_work(runner);
    comment_inline_linear_work(runner);
    comment_block_linear_work(runner);
    line_facts_survive_moved_text(runner);
    percent_comment_nodes(runner);
    cross_link_fields(runner);
    node_payload_lifecycle(runner);
    kind_conversion_reuses_cell_storage(runner);
    element_owned_field_lifecycle(runner);
    kind_conversion_containment(runner);
    node_type_values(runner);
    constructor(runner);
    accessors(runner);
    formula_element_accessors(runner);
    directive_element_accessors(runner);
    directive_scopes_are_editor_positions(runner);
    paragraphs_start_on_their_first_byte(runner);
    lazy_lines_parse_as_prefixed(runner);
    lazy_after_marker_as_after_paragraph(runner);
    table_geometry_ignores_indentation(runner);
    pipe_rows_are_one_row(runner);
    create_tree(runner);
    attachment_containment(runner);
    hierarchy(runner);
    parser(runner);
    utf8(runner);
    line_endings(runner);
    numeric_entities(runner);
    test_cplusplus(runner);
    comment_nodes(runner);
    test_crlf_line_ending(runner);
    test_pathological_regressions(runner);
    element_decline_yields_turn(runner);
    source_pos(runner);
    source_pos_inlines(runner);
    ref_source_pos(runner);
    link_resource_lifecycle(runner);
    reference_attribute_lifecycle(runner);
    block_cursor_coordinates(runner);
    task_marker_tab_structure(runner);
    task_marker_ownership(runner);
    specimen_values(runner);
    definition_blank_continuation(runner);
    set_kind_keeps_element_data_beside_the_arm(runner);
    citation_and_footnote_values(runner);
    autolink_source_pos(runner);
    table_source_map_growth(runner);
    table_values(runner);
    bounded_scanners(runner);
    malformed_scalar_terminates(runner);
    inline_hook_projection_ignores_non_owners(runner);
    properties_envelope_derives_each_line_once(runner);
    properties_rejected_members_borrow_source(runner);
    source_line_geometry_is_shared(runner);
    short_line_storage_is_bounded(runner);
    growth_preserves_input_views(runner);
    construction_checks_containment(runner);
    semantic_rejection_preserves_its_cause(runner);
    rejected_token_allocation_failures(runner);
    parser_attachment_commits_one_decision(runner);
    formula_containment_follows_recognition(runner);
    delimiter_boundary_floors(runner);
    formula_leaf_construction_work(runner);
    formula_promotion_views_content(runner);
    formula_promotion_keeps_the_fence_runs(runner);
    directive_recognition_survives_construction(runner);
    lookahead_and_driver_share_normalized_lines(runner);
    definition_open_gate_admits_only_possible_terms(runner);
    completion_acts_on_every_root(runner);
    completion_links_at_any_depth(runner);
    a_finish_step_is_asked_where_it_can_act(runner);
    a_flanking_transparent_byte_is_ascii(runner);
    standalone_formula_as_owned_root(runner);
    strbuf_overflow(runner);
    strbuf_failure_is_a_transaction(runner);
    lazy_literal_failure_writes_nothing(runner);
    void_source_continues(runner);
    stray_delimiter(runner);
    finish_step_skips_absent_kinds(runner);
    finish_step_gate_opens_when_its_kind_is_made(runner);
    finish_kind_sets_are_well_formed(runner);
    block_gate_admits_every_opener(runner);
    inline_dispatch_ownership(runner);
    inline_dispatch_orders_every_precedence(runner);
    scope_queries_count_in_the_document_unit(runner);
    session_appends(runner);
    session_edits(runner);
    session_edits_literal_blocks(runner);
    session_edits_name_headings(runner);
    session_edits_storage(runner);
    session_edits_nested_notes(runner);
    session_edits_long_headings(runner);
    heading_long_authored_anchor(runner);
    heading_long_anchor_refused(runner);
    session_edits_refresh_storage(runner);
    session_edits_uses(runner);
    session_edits_nested_identifier(runner);
    session_locates_table_rows(runner);
    session_locates_unclosed_openers(runner);
    session_edits_wide_list(runner);
    session_edits_replaced_label(runner);
    session_edits_allocations(runner);
    session_edits_compaction(runner);
    session_snapshot_anchors(runner);
    session_snapshot_names(runner);
    session_snapshot_tables(runner);
    session_snapshot_lists(runner);
    session_snapshot_openers(runner);
    session_settled_lookups_at_flush(runner);
    session_snapshot_candidates(runner);
    session_held_scalars(runner);
    session_decisions_keep_lines(runner);
    session_accept_after_commit_writes(runner);
    session_opener_fields(runner);
    node_view_views(runner);
    session_flush_matches_a_parse(runner);
    session_walks_through_definition_bodies(runner);
    session_roots_at_every_depth(runner);
    session_allocation_failures(runner);

    test_print_summary(runner);
    retval = test_ok(runner) ? 0 : 1;
    free(runner);

    return retval;
}
