#include "alloc.h"
#include "block_internal.h"
/**
 * Block parsing implementation.
 *
 * For a high-level overview of the block parsing process,
 * see http://spec.commonmark.org/0.24/#phase-1-block-structure
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "markdown_core_ctype.h"
#include "element.h"
#include "config.h"
#include "parser.h"
#include "node_type.h"
#include "node.h"
#include "registry.h"
#include "utf8.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"

#define TAB_STOP 4

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

#define peek_at(i, n) (i)->data[n]

bool markdown_core_block_last_line_blank(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0;
}

markdown_core_node_type markdown_core_block_type(const markdown_core_node *node) {
    return (markdown_core_node_type)node->kind;
}

static void S_set_last_line_blank(markdown_core_node *node, bool markdown_core_block_is_blank) {
    if (markdown_core_block_is_blank) {
        node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    } else {
        node->flags &= ~MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    }
}

/* The parse's two stages, the work of its commit and of its finalize, which
 * the benchmark measures at these two calls (scripts/benchmark/run.mjs): out
 * of line by declaration, so a stage that shrinks never folds into its
 * caller. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_parse_source(markdown_core_parser *parser,
                                                               const unsigned char *source, size_t length,
                                                               size_t limit);
static MARKDOWN_CORE_ATTRIBUTE((noinline)) markdown_core_node *S_finish_parse(markdown_core_parser *parser);
static inline bool S_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
static inline int S_append_input_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t line_start, int column, bufsize_t length, bufsize_t offset);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);
static inline const unsigned char *S_input_line_content(markdown_core_parser *parser, markdown_core_input_line *line,
                                                        bufsize_t *length);

static markdown_core_node *make_block(markdown_core_parser *parser, markdown_core_node_type tag, bufsize_t start) {
    markdown_core_node *e;

    e = markdown_core_parser_make_node(parser, tag);
    if (!e) {
        return NULL;
    }
    /* Empty content borrows the strbuf sentinel. Only writing content takes
     * storage; containers and empty leaves need no separate allocation. */
    e->flags = MARKDOWN_CORE_NODE__OPEN;
    e->where.place = (markdown_core_place){(uint32_t)start, (uint32_t)start};

    return e;
}

// Create a root document node.
static markdown_core_node *make_document(markdown_core_parser *parser) {
    markdown_core_node *e = make_block(parser, MARKDOWN_CORE_NODE_DOCUMENT, 0);
    return e;
}

/* A physical line containing NUL owns one immutable normalized view. Both
 * the driver and speculative readers borrow it until the active input ends;
 * queued mapped inputs therefore receive the same bytes the driver would.
 * Only transformed lines allocate, and the ownership chain releases only
 * those allocations rather than visiting every untransformed input line. */
typedef struct markdown_core_normalized_line {
    struct markdown_core_normalized_line *next;
    /* The source offset of each NUL of the line, in order, so a normalized
     * byte finds its source byte by a search rather than a scan. */
    uint32_t *nuls;
    uint32_t nul_count;
    unsigned char bytes[];
} markdown_core_normalized_line;

static void S_clear_normalized_lines(markdown_core_parser_input *input) {
    while (input->normalized) {
        markdown_core_normalized_line *line = input->normalized;
        input->normalized = line->next;
        markdown_core_free(line);
    }
}

static void S_release_input(markdown_core_parser_input *input) {
    S_clear_normalized_lines(input);
    markdown_core_free(input->lines);
    markdown_core_free(input->facts);
    markdown_core_strbuf_free(&input->last_line);
}

/* A parse that ends before the end of its input leaves builders no block
 * holds yet: every open block above the input's root, each with the children
 * that settled into it, and the pending block. They go with the parse. */
static void S_release_builders(markdown_core_parser *parser) {
    if (parser->pending) {
        markdown_core_parser_release_node(parser, parser->pending);
        parser->pending = NULL;
    }
    while (parser->current != parser->block_root) {
        markdown_core_node *below = markdown_core_scaffold_parent(parser->current);
        markdown_core_parser_release_node(parser, parser->current);
        parser->current = below;
    }
}

static void S_parser_dispose(markdown_core_parser *parser) {
    const markdown_core_dialect *dialect = parser->dialect;
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element_instance *instance = &dialect->instances[i];
        if (instance->element->dispose_parser) {
            instance->element->dispose_parser(instance, parser);
        }
    }
    dialect->document_structure->element->dispose_document(dialect->document_structure, parser);
    markdown_core_registry_free(&parser->registry);
    markdown_core_source_order_dispose(&parser->source_order);
    markdown_core_free(parser->walk_stack);
    markdown_core_free(parser->block_inputs);
    S_release_input(&parser->input);
    S_release_input(&parser->idle_input);
    S_release_builders(parser);
    if (parser->root) {
        markdown_core_node_free(parser->root);
    }

    /* The content-to-source map outlives every block that indexes it and
     * nothing else does, so it is released here rather than with the node. */
    markdown_core_free(parser->line_marks);
    parser->line_marks = NULL;
    parser->line_marks_size = 0;
    parser->line_marks_alloc = 0;

    while (parser->free_delimiters) {
        delimiter *entry = parser->free_delimiters;
        parser->free_delimiters = entry->next;
        markdown_core_free(entry);
    }
    markdown_core_inline_release_records(parser);
    markdown_core_attribute_scratch_free(&parser->attribute_scratch);

    /* The block-start lookahead's flags and resume cache are parser state of
     * the same kind: indexed by open containers and source lines, owned by no
     * node, and dead with the parse. */
    markdown_core_free(parser->lookahead_chain);
    parser->lookahead_chain = NULL;
    parser->lookahead_chain_alloc = 0;
}

/* ONE INSTANCE'S FIXED STATE: the parser and the dialect sealed for it,
 * followed by the dialect's tables and then the elements' parse records
 * (markdown-core-element-api.h, "AN ELEMENT AS ONE PARSE HOLDS IT"). They
 * are one allocation because they
 * begin and end together -- the dialect is sealed as the instance is made,
 * nothing else holds it, and it is released with the instance. Two blocks
 * would be two lifetimes to keep in step, and two places among the parse's
 * own buffers: split in two, the parser is small enough to fill a hole those
 * buffers would have grown into. The instance runs one transaction at a
 * time, and each begins with the parser and the records as the instance was
 * made: zeroed, holding the dialect and the setup's context. */
typedef struct markdown_core_instance {
    markdown_core_parser parser;
    /* The elements' parse records, which follow the dialect's tables. */
    unsigned char *records;
    size_t record_bytes;
    /* Last: sealing lays the dialect's tables out right after it. */
    markdown_core_dialect dialect;
} markdown_core_instance;

markdown_core_parser *markdown_core_parser_create(const markdown_core_element *const *elements, size_t count,
                                                  markdown_core_parser_setup_func setup, void *context) {
    markdown_core_dialect_builder builder;
    markdown_core_dialect_sizes sizes;

    /* The instance's dialect: the given elements, whatever its setup
     * registers after them, and then nothing more. The builder is gone before
     * the instance parses, so no code a parse runs can hold anything that
     * registers. */
    markdown_core_dialect_builder_init(&builder, elements, count);
    if (setup && !setup(&builder, context)) {
        markdown_core_dialect_builder_dispose(&builder);
        return NULL;
    }
    /* The records follow the dialect's tables at the records' alignment.
     * Measuring counts every element's aligned record size, which is what
     * sealing lays out. */
    size_t records =
        markdown_core_state_align(sizeof(markdown_core_instance) + markdown_core_dialect_measure(&builder, &sizes));
    markdown_core_instance *instance = markdown_core_alloc(1, records + sizes.state_bytes);
    if (instance) {
        instance->records = (unsigned char *)instance + records;
        instance->record_bytes = sizes.state_bytes;
        markdown_core_dialect_seal(&builder, &sizes, &instance->dialect, instance->records);
        instance->parser.dialect = &instance->dialect;
        instance->parser.context = context;
    }
    markdown_core_dialect_builder_dispose(&builder);
    return instance ? &instance->parser : NULL;
}

void markdown_core_parser_destroy(markdown_core_parser *parser) {
    markdown_core_free((markdown_core_instance *)parser);
}

static bool S_reserve_content_marks(markdown_core_parser *parser, bufsize_t count);

/* Starts a transaction that continues `revision` and borrows its pool. */
void markdown_core_parser_start(markdown_core_parser *parser, markdown_core_revision *revision) {
    markdown_core_node *document;

    parser->revision = revision;
    parser->pool = revision->pool;
    markdown_core_strbuf_init(&parser->curline, 256);
    markdown_core_strbuf_init(&parser->input.last_line, 0);
    markdown_core_strbuf_init(&parser->idle_input.last_line, 0);
    markdown_core_registry_init(&parser->registry);
    /* The line index is a parse-owned workspace, like curline. Establish its
     * initial capacity before any input; inputs reset length, never
     * ownership. */
    parser->input.lines = markdown_core_reserve(NULL, &parser->input.line_capacity, 1, sizeof(*parser->input.lines));
    /* The first run of the content map is the identity an inline root's
     * parse places on (parser.h, MARKDOWN_CORE_IDENTITY_MARK): first, so that
     * no map appended after it ends with it. */
    if (S_reserve_content_marks(parser, 1)) {
        parser->line_marks[parser->line_marks_size++] = (markdown_core_line_mark){0, 0, 0, 1, 1, 0};
    }

    document = make_document(parser);
    parser->root = document;
    parser->block_root = parser->current = document;

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. Only a
     * complete transaction begins the document lifecycle -- its owner is
     * whichever element the dialect resolved it to, and it never sees a
     * failed transaction. Disposal does not depend on it having begun: the
     * lifecycle's own allocations can fail halfway, so its release already
     * takes whatever state it finds. */
    if (!parser->root || !parser->input.lines || parser->curline.oom || parser->input.last_line.oom ||
        parser->root->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    } else {
        const markdown_core_element_instance *document_structure = parser->dialect->document_structure;
        document_structure->element->init_document(document_structure, parser);
    }
}

/* Releases the transaction's parse state: the parser and the records are as
 * the instance was made, for the next. */
static void S_release_parse(markdown_core_parser *parser) {
    markdown_core_instance *instance = (markdown_core_instance *)parser;
    S_parser_dispose(parser);
    markdown_core_strbuf_free(&parser->curline);
    void *context = parser->context;
    memset(parser, 0, sizeof(*parser));
    parser->dialect = &instance->dialect;
    parser->context = context;
    memset(instance->records, 0, instance->record_bytes);
}

const markdown_core_element_instance *markdown_core_parser_instance(const markdown_core_parser *parser,
                                                                    const markdown_core_element *element) {
    return markdown_core_dialect_instance(parser->dialect, element);
}

/* "This block ends on the line being processed", lifted out of `markdown_core_block_close` so
 * that the element close path can say the same thing. The three kinds that
 * take it there — the document, a closed fenced code block, a setext heading —
 * are the ones whose last line IS the line in hand; every other block ended on
 * the line before. An element container closing on its own fence is a fourth,
 * and `markdown_core_block_close` cannot know that from the type alone. */
void markdown_core_block_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b) {
    b->where.place.end = (uint32_t)parser->line_end;
}

/* Whether a node of `kind` takes lines as its content, and whether it may
 * hold inline content: the two facts a hook answers per node. */
static bool S_kind_accepts_lines(const markdown_core_kind_record *kind, markdown_core_node *node) {
    return (kind->flags & MARKDOWN_CORE_KIND_LINES) ||
           ((kind->flags & MARKDOWN_CORE_KIND_LINES_ASK) &&
            kind->structure->element->accepts_lines_func(kind->structure->element, node));
}
/* Whether a node of `kind` takes a text line: as its content, or as prose. */
static bool S_kind_takes_text(const markdown_core_kind_record *kind, markdown_core_node *node) {
    return S_kind_accepts_lines(kind, node) || (kind->flags & MARKDOWN_CORE_KIND_PROSE);
}
static bool S_kind_contains_inlines(const markdown_core_kind_record *kind, markdown_core_node *node) {
    return (kind->flags & MARKDOWN_CORE_KIND_INLINES) ||
           ((kind->flags & MARKDOWN_CORE_KIND_INLINES_ASK) &&
            kind->structure->element->contains_inlines_func(kind->structure->element, node));
}

/* Whether the line may be LAZY: it did not match the prefix of the current
 * block, `matched` being the deepest block whose prefix it did match, and the
 * current block takes such a line as text (`accepts_lazy`). A paragraph takes
 * one; so does a callout on the line after its marker line, whose body the
 * line then starts. The line is lazy if it opens no block, and the starts
 * that refuse a lazy line (`block_start_context`) are no block start on it. */
static inline bool S_may_be_lazy(markdown_core_parser *parser, const markdown_core_node *matched) {
    markdown_core_node *current = markdown_core_parser_current(parser);
    if (current == matched) {
        return false;
    }
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, current);
    return structure && structure->element->accepts_lazy &&
           structure->element->accepts_lazy(structure, parser, current);
}

/* Record where the bytes about to be appended to `node`'s content came from.
 *
 * `column` is a BYTE column counted from 1, which is what every position in
 * the tree is counted in; `parser->column` is not one, because it counts a tab
 * as the several columns it expands to. */
/* One vector owns all content/source runs. A run can start on the same
 * source line as its predecessor when a transformation removes bytes. */
static bool S_reserve_content_marks(markdown_core_parser *parser, bufsize_t count) {
    if (count > INT32_MAX - parser->line_marks_size) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    bufsize_t needed = parser->line_marks_size + count;
    if (needed <= parser->line_marks_alloc) {
        return true;
    }
    bufsize_t capacity = parser->line_marks_alloc ? parser->line_marks_alloc : 64;
    while (capacity < needed) {
        capacity = capacity > INT32_MAX / 2 ? INT32_MAX : capacity * 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(markdown_core_line_mark)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    markdown_core_line_mark *grown = markdown_core_realloc(parser->line_marks, (size_t)capacity * sizeof(*grown));
    if (!grown) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->line_marks = grown;
    parser->line_marks_alloc = capacity;
    return true;
}

static int S_append_content_mark(markdown_core_parser *parser, markdown_core_content_map *map,
                                 markdown_core_line_mark mark) {
    if (!parser || !map) {
        return 0;
    }
    mark.content_offset += map->offset;
    if (map->count && map->first + map->count == parser->line_marks_size &&
        parser->line_marks[parser->line_marks_size - 1].content_offset == mark.content_offset) {
        parser->line_marks[parser->line_marks_size - 1] = mark;
        return 1;
    }
    if (!S_reserve_content_marks(parser, 1)) {
        return 0;
    }
    if (map->count == 0) {
        map->first = parser->line_marks_size;
    } else {
        assert(map->first + map->count == parser->line_marks_size);
        assert(parser->line_marks[parser->line_marks_size - 1].content_offset < mark.content_offset);
    }
    assert(mark.source_width >= 1);
    parser->line_marks[parser->line_marks_size++] = mark;
    map->count++;
    return 1;
}

int markdown_core_parser_append_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                             int line, bufsize_t source, int source_width, int source_step) {
    return S_append_content_mark(parser, &node->content_map,
                                 (markdown_core_line_mark){offset, line, source, source_width, source_step, 0});
}

/* The bytes land at the end of the open block's text, which its map is of. */
static void S_record_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t column,
                                  bufsize_t length) {
    S_append_input_marks(parser, node, parser->line_number, parser->line_start, column, length,
                         markdown_core_node_text(node).len);
}

void markdown_core_block_add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
    /* Block content accumulates physical lines. Keep its existing initial
     * minimum reservation, but acquire enough for the complete first write
     * rather than allocating a small buffer and immediately growing it.
     * Empty blocks, including containers, never acquire this storage.
     * Producers of already delimited values use ordinary strbuf writes. */
    if (!markdown_core_strbuf_owns(&node->content) && (parser->partially_consumed_tab || ch->len > parser->offset)) {
        size_t initial = (size_t)(ch->len - parser->offset);
        if (parser->partially_consumed_tab) {
            initial += TAB_STOP - (parser->column % TAB_STOP) - 1;
        }
        /* Saturate only the conversion; strbuf owns the capacity limit and
         * failure contract, including a tab expansion beyond that limit. */
        markdown_core_strbuf_grow(&node->content, initial > INT32_MAX ? INT32_MAX
                                                  : initial > 32      ? (bufsize_t)initial
                                                                      : 32);
        if (node->content.oom) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
    }
    /* Indentation stripped ahead of the content belongs to the CONTAINER that
     * stripped it, not to the block being written into -- the same rule the
     * block openers follow, and for the same reason: a block begins at its own
     * first non-space byte, so a region of its own that started earlier would
     * start before its own scope. The bytes that ARE copied are its content,
     * and the tab below is one of them, because its expansion is what lands in
     * the buffer. */
    if (parser->partially_consumed_tab) {
        /* The spaces below stand for the tail of the tab at parser->offset and
         * have no source bytes of their own, so they are marked against the
         * tab itself and the copied bytes get a mark of their own. */
        S_record_content_mark(parser, node, parser->offset + 1, 1);
        parser->offset += 1; // skip over tab
        // add space characters:
        chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
        for (i = 0; i < chars_to_tab; i++) {
            markdown_core_strbuf_putc(&node->content, ' ');
        }
    }
    S_record_content_mark(parser, node, parser->offset + 1, ch->len - parser->offset);
    markdown_core_strbuf_put(&node->content, ch->data + parser->offset, ch->len - parser->offset);
    if (node->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

/* Seed a map for one unchanged source line assembled by a producer rather
 * than fed through add_line. Transformed content appends its own source runs;
 * slices share their owner's runs through adopt_content_marks. A failed map
 * allocation loses the parse transaction, just like a failed content buffer. */
int markdown_core_parser_mark_content(markdown_core_parser *parser, markdown_core_node *node, int line,
                                      bufsize_t source) {
    if (!parser || !node) {
        return 0;
    }
    node->content_map.count = 0;
    node->content_map.offset = 0;
    return markdown_core_parser_append_content_mark(parser, node, 0, line, source, 1, 1);
}

/* Find the immutable run containing an offset, shared by slice and lookup. */
/* A map slice is a view into parser-owned immutable runs. Neither the source
 * nor the slice owns the vector; both end with the parse transaction. */
int markdown_core_parser_adopt_content_marks(markdown_core_parser *parser, const markdown_core_content_map *owner,
                                             markdown_core_content_map *map, bufsize_t from, bufsize_t length) {
    if (!parser || !owner || !map || !owner->count || length <= 0) {
        return 0;
    }
    from += owner->offset;
    int first = markdown_core_block_content_mark_at(parser, owner, from);
    int last = markdown_core_block_content_mark_at(parser, owner, from + length - 1);
    map->first = first;
    map->count = last - first + 1;
    map->offset = from;
    return 1;
}

int markdown_core_parser_append_content_marks(markdown_core_parser *parser, const markdown_core_content_map *owner,
                                              markdown_core_content_map *map, bufsize_t from, bufsize_t length,
                                              bufsize_t offset) {
    if (length <= 0) {
        return 1;
    }
    if (!owner->count) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return 0;
    }
    from += owner->offset;
    int first = markdown_core_block_content_mark_at(parser, owner, from);
    int last = markdown_core_block_content_mark_at(parser, owner, from + length - 1);
    for (int i = first; i <= last; i++) {
        markdown_core_line_mark mark = parser->line_marks[i];
        bufsize_t start = mark.content_offset < from ? from : mark.content_offset;
        mark.source += (start - mark.content_offset) * mark.source_step;
        mark.content_offset = offset + start - from;
        if (!S_append_content_mark(parser, map, mark)) {
            return 0;
        }
    }
    return 1;
}

/* The normalized view of a document line that carries NUL, or NULL: the
 * block parser reads each NUL as the three bytes of U+FFFD, so a column past
 * one is two bytes past its source byte. Only the document's own lines carry
 * NUL; a cell's content was copied from a normalized line. */
static const markdown_core_normalized_line *S_nul_view_read(markdown_core_parser *parser,
                                                            markdown_core_input_line *geometry) {
    if (!parser->input.facts[geometry->facts - 1].nul_count) {
        return NULL;
    }
    bufsize_t length;
    if (!S_input_line_content(parser, geometry, &length)) {
        return NULL;
    }
    return parser->input.facts[geometry->facts - 1].normalized;
}

/* Only a document line with facts can carry NUL. */
static inline const markdown_core_normalized_line *S_nul_view(markdown_core_parser *parser,
                                                              markdown_core_input_line *geometry) {
    assert(markdown_core_parser_block_root(parser) == parser->root);
    return geometry->facts ? S_nul_view_read(parser, geometry) : NULL;
}

/* The source byte of normalized byte `index` of a NUL-bearing line, and
 * whether it is a byte of a replacement character, which stands for its NUL.
 * The NUL numbered k is at normalized byte `nuls[k] - start + 2k`. */
static bufsize_t S_denormalized_offset(const markdown_core_normalized_line *view, uint32_t start, bufsize_t index,
                                       bool *nul) {
    uint32_t lo = 0, hi = view->nul_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((bufsize_t)(view->nuls[mid] - start + 2 * mid) <= index) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    /* `lo` NULs are at or before the byte; the last of them may hold it. */
    if (lo && index < (bufsize_t)(view->nuls[lo - 1] - start + 2 * (lo - 1) + 3)) {
        *nul = true;
        return (bufsize_t)view->nuls[lo - 1];
    }
    *nul = false;
    return (bufsize_t)start + index - 2 * (bufsize_t)lo;
}

bufsize_t markdown_core_parser_mapped_source_offset(markdown_core_parser *parser, int line, int column) {
    assert(column > 0);
    markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    if (markdown_core_parser_block_root(parser) == parser->root) {
        const markdown_core_normalized_line *view = S_nul_view(parser, geometry);
        bool nul;
        return view ? S_denormalized_offset(view, geometry->start, column - 1, &nul)
                    : (bufsize_t)geometry->start + column - 1;
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_place(parser, &markdown_core_parser_block_root(parser)->content_map,
                                            (bufsize_t)geometry->start + column - 1, &ignored, &source)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

/* A cell's content is its parts joined by line endings, one part per source
 * line, and a part is empty when the cell is blank on that line. The end of
 * an empty cell line is where the cell's content before it ends: the last
 * byte the cell holds before the line, or the cell's first byte when it holds
 * none. */
static bufsize_t S_mapped_line_end(markdown_core_parser *parser, bufsize_t input_offset) {
    const markdown_core_content_map *map = &markdown_core_parser_block_root(parser)->content_map;
    const unsigned char *content = parser->input.source;
    bufsize_t at = input_offset;
    int ignored;
    bufsize_t source = 0;
    while (at > 0 && (content[at - 1] == '\n' || content[at - 1] == '\r')) {
        at--;
    }
    bool placed = at > 0 ? markdown_core_parser_content_end_place(parser, map, at - 1, &ignored, &source)
                         : markdown_core_parser_content_place(parser, map, 0, &ignored, &source);
    if (!placed) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

bufsize_t markdown_core_parser_mapped_source_end(markdown_core_parser *parser, int line, int column) {
    markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    if (markdown_core_parser_block_root(parser) == parser->root) {
        if (!column) {
            return (bufsize_t)geometry->start;
        }
        return markdown_core_parser_mapped_source_offset(parser, line, column) + 1;
    }
    if (!column) {
        return S_mapped_line_end(parser, (bufsize_t)geometry->start);
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_end_place(parser, &markdown_core_parser_block_root(parser)->content_map,
                                                (bufsize_t)geometry->start + column - 1, &ignored, &source)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

/* Where byte `at` of the active input was written in the source. */
static inline bufsize_t S_input_source(markdown_core_parser *parser, bufsize_t at) {
    if (markdown_core_parser_block_root(parser) == parser->root) {
        return at;
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_place(parser, &markdown_core_parser_block_root(parser)->content_map, at, &ignored,
                                            &source)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

static inline bufsize_t S_line_offset(markdown_core_parser *parser, int line) {
    return S_input_source(parser, (bufsize_t)markdown_core_parser_visited_line(parser, line)->start);
}

static inline bool S_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line) {
    if (line < parser->input.first_line || line > parser->line_number) {
        return false;
    }
    uint32_t start = node->where.place.start;
    if (line == parser->line_number) {
        return start >= (uint32_t)S_input_source(parser, parser->line_start);
    }
    return start >= (uint32_t)S_line_offset(parser, line) && start < (uint32_t)S_line_offset(parser, line + 1);
}

bufsize_t markdown_core_parser_line_offset(markdown_core_parser *parser, int line) {
    return S_line_offset(parser, line);
}

bool markdown_core_parser_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line) {
    return S_starts_on_line(parser, node, line);
}

/* The source of input line `line` from where its own bytes begin to where it
 * ends, through its terminator on a line of the document itself. False when
 * the line's content could not be read. */
static bool S_line_own(markdown_core_parser *parser, int line, markdown_core_place *own_place) {
    bufsize_t length;
    if (!S_input_line_content(parser, markdown_core_parser_visited_line(parser, line), &length)) {
        return false;
    }
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    const bufsize_t own = (bufsize_t)geometry->own;
    own_place->start = (uint32_t)(own < length ? markdown_core_parser_source_offset(parser, line, own + 1)
                                               : markdown_core_parser_source_end(parser, line, own));
    own_place->end = markdown_core_parser_block_root(parser) == parser->root
                         ? (uint32_t)markdown_core_input_line_next(parser, geometry)
                         : (uint32_t)markdown_core_parser_source_end(parser, line, length);
    return true;
}

/* The places of `node`, which spans input lines `first` to `last`, more
 * than one: the own source of each line, a place of length 0, those that
 * touch joined. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_line_places(markdown_core_parser *parser, markdown_core_node *node,
                                                              int first, int last) {
    markdown_core_places *places = markdown_core_node_pool_bytes(
        parser->pool, sizeof(*places) + (size_t)(last - first + 1) * sizeof(markdown_core_source_place));
    if (!places) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    places->count = places->content = 0;
    markdown_core_place own;
    for (int line = first; line <= last; line++) {
        if (!S_line_own(parser, line, &own)) {
            markdown_core_node_pool_bytes_free(parser->pool, places);
            return;
        }
        if (places->count && places->items[places->count - 1].end == own.start) {
            places->items[places->count - 1].end = own.end;
        } else {
            places->items[places->count++] = (markdown_core_source_place){own.start, own.end, 0};
        }
    }
    if (places->count < 2) {
        markdown_core_node_pool_bytes_free(parser->pool, places);
        return;
    }
    if (node->source.places) {
        markdown_core_node_pool_bytes_free(parser->pool, node->source.places);
    }
    node->source.places = places;
}

void markdown_core_parser_line_places(markdown_core_parser *parser, markdown_core_node *node,
                                      const markdown_core_node *container, int *line) {
    /* A block of the document itself lies on whole lines of the source: its
     * own source is its range. */
    if (container == parser->root) {
        return;
    }
    const int visited = parser->input.first_line + (int)parser->input.line_count - 1;
    const uint32_t start = node->where.place.start, end = node->where.place.end;
    int first = *line;
    while (first > parser->input.first_line && (uint32_t)S_line_offset(parser, first) > start) {
        first--;
    }
    /* Down the lines from there: one that begins at or before the start is
     * the first so far, and the node ends on the last that begins before
     * its end. */
    int last = first;
    while (last < visited) {
        const uint32_t next = (uint32_t)S_line_offset(parser, last + 1);
        if (next > start && next >= end) {
            break;
        }
        last++;
        if (next <= start) {
            first = last;
        }
    }
    if (first < last) {
        S_line_places(parser, node, first, last);
    }
    *line = last;
}

/* One copied run of a document line. */
static inline int S_append_copied_mark(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t source, bufsize_t offset) {
    return S_append_content_mark(parser, &node->content_map,
                                 (markdown_core_line_mark){offset, line, source, 1, 1, parser->indent});
}

/* The runs of a document line of an input that holds NUL. A line without
 * NUL is one copied run. On a NUL-bearing line, each replacement character is
 * a run whose three content bytes all stand for its one NUL, and the bytes
 * between them are copied runs. NUL k is at normalized byte
 * `nuls[k] - start + 2k`; the walk starts at the first NUL whose replacement
 * ends after the slice's first byte and meets each later one once. It stays
 * out of line, apart from the path of an input without NUL. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) int S_append_nul_line_marks(markdown_core_parser *parser,
                                                                       markdown_core_node *node, int line,
                                                                       bufsize_t line_start, int column,
                                                                       bufsize_t length, bufsize_t offset) {
    const markdown_core_normalized_line *view = S_nul_view(parser, markdown_core_parser_visited_line(parser, line));
    if (!view) {
        return S_append_copied_mark(parser, node, line, line_start + column - 1, offset);
    }
    uint32_t start = (uint32_t)line_start;
    bufsize_t first = column - 1, last = first + length, index = first;
    uint32_t k = 0, hi = view->nul_count;
    while (k < hi) {
        uint32_t mid = k + (hi - k) / 2;
        if ((bufsize_t)(view->nuls[mid] - start + 2 * mid) + 3 <= index) {
            k = mid + 1;
        } else {
            hi = mid;
        }
    }
    while (index < last) {
        bool inside = false;
        bufsize_t at = last, source;
        if (k < view->nul_count) {
            at = (bufsize_t)(view->nuls[k] - start + 2 * k);
            inside = at <= index;
        }
        source = inside ? (bufsize_t)view->nuls[k] : (bufsize_t)start + index - 2 * (bufsize_t)k;
        if (!S_append_content_mark(
                parser, &node->content_map,
                (markdown_core_line_mark){offset + index - first, line, source, 1, inside ? 0 : 1, parser->indent})) {
            return 0;
        }
        bufsize_t next = inside ? at + 3 : at;
        k += inside;
        index = next < last ? next : last;
    }
    return 1;
}

/* The runs of `length` bytes from `column` of input line `line`, which
 * starts at `line_start` in the active input. */
static inline int S_append_input_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t line_start, int column, bufsize_t length, bufsize_t offset) {
    if (!parser->input.mapped) {
        return S_append_copied_mark(parser, node, line, line_start + column - 1, offset);
    }
    if (markdown_core_parser_block_root(parser) == parser->root) {
        return S_append_nul_line_marks(parser, node, line, line_start, column, length, offset);
    }
    return markdown_core_parser_append_content_marks(parser, &markdown_core_parser_block_root(parser)->content_map,
                                                     &node->content_map, line_start + column - 1, length, offset);
}

int markdown_core_parser_append_line_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                           bufsize_t line_start, int column, bufsize_t length, bufsize_t offset) {
    return S_append_input_marks(parser, node, line, line_start, column, length, offset);
}

int markdown_core_parser_append_source_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                             int column, bufsize_t length, bufsize_t offset) {
    return S_append_input_marks(parser, node, line, (bufsize_t)markdown_core_parser_visited_line(parser, line)->start,
                                column, length, offset);
}

bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_node *owner,
                                            uint32_t enclosing) {
    if (!owner->content.size) {
        return true;
    }
    assert(owner->content_map.count);
    if (parser->block_input_count == parser->block_input_capacity) {
        size_t capacity = parser->block_input_capacity ? 2 * parser->block_input_capacity : 16;
        if (capacity > SIZE_MAX / sizeof(*parser->block_inputs)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        void *inputs = markdown_core_realloc(parser->block_inputs, capacity * sizeof(*parser->block_inputs));
        if (!inputs) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        parser->block_inputs = inputs;
        parser->block_input_capacity = capacity;
    }
    parser->block_inputs[parser->block_input_count++] = (markdown_core_block_input){owner, enclosing};
    return true;
}

/* Requirement 10: for any block with a content buffer and any byte offset
 * within it, name the source line and column of that byte.
 *
 * The answer is a projection of the block's mark run, not a counter anyone
 * maintains: find the slice the offset falls in and add the distance from its
 * start. The slice is found from the node's first run here; a caller with a
 * position of its own to start from asks through `content_span`. */
static int S_content_place(markdown_core_parser *parser, const markdown_core_content_map *map, bufsize_t content_offset,
                           bool end, int *line, bufsize_t *source) {
    const markdown_core_line_mark *mark;

    if (!parser || !map || map->count <= 0 || content_offset < 0) {
        return 0;
    }

    content_offset += map->offset;
    mark = &parser->line_marks[markdown_core_block_content_mark_at(parser, map, content_offset)];
    *line = mark->line;
    *source =
        mark->source + (content_offset - mark->content_offset) * mark->source_step + (end ? mark->source_width : 0);
    return 1;
}

int markdown_core_parser_content_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                       bufsize_t offset, int *line, bufsize_t *source) {
    return S_content_place(parser, map, offset, false, line, source);
}

int markdown_core_parser_content_end_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                           bufsize_t offset, int *line, bufsize_t *end) {
    return S_content_place(parser, map, offset, true, line, end);
}

/* Appends to `places` a place of length 0 over `start` to `end`, joined to a
 * place of length 0 it touches. */
static inline void S_places_add_own(markdown_core_places *places, uint32_t start, uint32_t end) {
    if (start >= end) {
        return;
    }
    markdown_core_source_place *previous = places->count ? &places->items[places->count - 1] : NULL;
    if (previous && !previous->length && previous->end == start) {
        previous->end = end;
    } else {
        places->items[places->count++] = (markdown_core_source_place){start, end, 0};
    }
}

/* AN INLINE ROOT'S CONTENT BECOMES THE INPUT ITS NODES ARE PLACED IN
 * (markdown_core_inline_start_inlines). The `length` bytes its parse reads
 * keep their map to the source in the root's places: one place per run of
 * the map, the bytes it reads from where it reads them, copied places that
 * touch in the source joined into one, and a place that reads source the
 * place before read joined to it, so the places' source ranges are in order
 * and apart or touching. A copied place reads each content byte from one
 * source byte; any other reads all of its content from all of its source.
 * The root's own source that gives no content lies in places of length 0
 * between them: the rest of each line its places of length 0 cover
 * (markdown_core_parser_line_places), or, when it has none, all of the
 * source between its content places. The root's map becomes the identity,
 * so the parse places its nodes at offsets of the content. */
void markdown_core_parser_read_content(markdown_core_parser *parser, markdown_core_node *node, bufsize_t length) {
    const markdown_core_content_map map = node->content_map;
    node->content_map = (markdown_core_content_map){MARKDOWN_CORE_IDENTITY_MARK, 1, 0};
    if (length <= 0 || map.count <= 0) {
        return;
    }
    const bufsize_t end = map.offset + length;
    const markdown_core_line_mark *mark =
        &parser->line_marks[markdown_core_block_content_mark_at(parser, &map, map.offset)];
    const markdown_core_line_mark *const past = &parser->line_marks[map.first + map.count];
    /* The node's own lines, when it has places of them; one line without. */
    markdown_core_places *const lines = node->source.places;
    const uint32_t line_count = lines ? lines->count : 0;
    /* At most one content place per mark from the first to the end of the
     * map, one place of length 0 before each and one after each line. */
    markdown_core_places *places =
        markdown_core_node_pool_bytes(parser->pool, sizeof(*places) + ((size_t)(past - mark) * 2 + line_count + 1) *
                                                                          sizeof(markdown_core_source_place));
    if (!places) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    places->count = 0;
    places->content = (uint32_t)length;
    uint32_t line = 0, at = 0;
    for (bufsize_t from = map.offset; from < end; mark++) {
        const bufsize_t to = mark + 1 < past && mark[1].content_offset < end ? mark[1].content_offset : end;
        const uint32_t start = (uint32_t)(mark->source + (from - mark->content_offset) * mark->source_step);
        const uint32_t stop = start + (uint32_t)((to - from - 1) * mark->source_step + mark->source_width);
        const uint32_t size = (uint32_t)(to - from);
        from = to;
        markdown_core_source_place *previous =
            places->count && places->items[places->count - 1].length ? &places->items[places->count - 1] : NULL;
        /* A copied slice that continues a copied place extends it, and one
         * that reads source the place already read, as an expanded tab's
         * columns read one tab, joins it: the place reads all of its content
         * from all of its source. */
        const bool continues = previous && previous->end == start && stop - start == size &&
                               previous->end - previous->start == previous->length;
        if (continues || (previous && start < previous->end)) {
            previous->end = stop > previous->end ? stop : previous->end;
            previous->length += size;
            at = previous->end;
            continue;
        }
        /* The own source before the slice: the rest of each line it passes,
         * and its own line up to it; with no lines, all of the source since
         * the place before. */
        for (; line < line_count && lines->items[line].end <= start; line++) {
            const markdown_core_source_place own = lines->items[line];
            S_places_add_own(places, at > own.start ? at : own.start, own.end);
        }
        if (line < line_count && lines->items[line].start <= start) {
            const uint32_t first = lines->items[line].start;
            S_places_add_own(places, at > first ? at : first, start);
        } else if (!lines && places->count) {
            S_places_add_own(places, at, start);
        }
        places->items[places->count++] = (markdown_core_source_place){start, stop, size};
        at = stop;
    }
    for (; line < line_count; line++) {
        const markdown_core_source_place own = lines->items[line];
        S_places_add_own(places, at > own.start ? at : own.start, own.end);
    }
    if (lines) {
        markdown_core_node_pool_bytes_free(parser->pool, lines);
    }
    node->source.places = places;
}

/* `cut` bytes are cut off the FRONT of `node`'s text, leaving `remaining`
 * bytes: the map comes to describe what is left. The marks stay where they
 * are in the vector: the run's head moves past the slices that went away,
 * and the slice the cut landed inside keeps its line with its column
 * advanced to the cut. */
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t cut,
                                              bufsize_t remaining) {
    if (node->content_map.count <= 0 || cut <= 0) {
        return;
    }
    if (remaining <= 0) {
        node->content_map.count = 0;
        node->content_map.offset = 0;
        return;
    }
    markdown_core_parser_adopt_content_marks(parser, &node->content_map, &node->content_map, cut, remaining);
}

/* The last child of `node`, a block whose children have settled, the
 * blank-line facts see, or NULL. */
static markdown_core_node *S_last_seen_child(const markdown_core_node *node) {
    for (uint32_t i = node->child_count; i > 0; i--) {
        if (!(node->children[i - 1]->flags & MARKDOWN_CORE_NODE__BLANK_TRANSPARENT)) {
            return node->children[i - 1];
        }
    }
    return NULL;
}

/* Whether `node`, a closed block, ends with a blank line: its own last line
 * was blank, or, for a list and its items, the last child they see ends with
 * one, which is folded into the block as it closes (S_fold_ends_blank). */
static inline bool S_ends_with_blank_line(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__BLANK_FOLDED) ? (node->flags & MARKDOWN_CORE_NODE__ENDS_BLANK) != 0
                                                            : (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0;
}

bool markdown_core_block_ends_with_blank_line(const markdown_core_node *node) { return S_ends_with_blank_line(node); }

/* A block of a kind whose blank-line answer is its children's, closing: its
 * children have settled, and the last one the facts see has its own answer,
 * which is the block's. A block with no such child answers with its own last
 * line, which a blank line can still write until it settles. */
static void S_fold_ends_blank(markdown_core_node *node) {
    const markdown_core_node *last = S_last_seen_child(node);
    if (last) {
        node->flags |=
            MARKDOWN_CORE_NODE__BLANK_FOLDED | (S_ends_with_blank_line(last) ? MARKDOWN_CORE_NODE__ENDS_BLANK : 0);
    }
}

/* A block whose block structure is final joins the completion queue, at
 * its end, as it settles into `holder` (blocks.c, A BLOCK SETTLES): a block
 * the block parser closes as its parent takes its next child or closes, a
 * block made whole -- a Reference, the rows and cells a table makes -- as it
 * is made. The queue's order is the order blocks settle in, and a block joins
 * once, so its own scaffold is the queue's storage (node.h, THE SCAFFOLD). */
static inline void S_complete_later(markdown_core_parser *parser, markdown_core_node *node,
                                    markdown_core_node *holder) {
    markdown_core_scaffold_of(node)->parent = holder;
    if (parser->completion_last) {
        markdown_core_scaffold_of(parser->completion_last)->next = node;
    } else {
        parser->completion_first = node;
    }
    parser->completion_last = node;
}

/* A block the parent's children had no room for: the parse is lost, and the
 * block, which nothing holds, goes with it. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_settle_failed(markdown_core_parser *parser,
                                                                markdown_core_node *node) {
    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    markdown_core_parser_release_node(parser, node);
}

/* A BLOCK SETTLES (docs/plans/2026-09-29-incremental-parsing.md, 5.2) into
 * `parent`, the block that holds it, once its own children have: it joins the
 * end of the parent's children, and the completion queue with its parent.
 * Nothing writes it after this but completion, which makes its inline content
 * and asks the finish steps of its kind (markdown-core-element-api.h, THE
 * FINISH STEP). The document settles into nothing as the input ends: it joins
 * the queue last, and nothing holds it. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_settle(markdown_core_parser *parser,
                                                                     markdown_core_node *node,
                                                                     markdown_core_node *parent) {
    if (!markdown_core_node_push_child(parser->pool, parent, node)) {
        S_settle_failed(parser, node);
        return;
    }
    S_complete_later(parser, node, parent);
}

void markdown_core_block_settle(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node *parent) {
    assert(parent != markdown_core_parser_current(parser) || !parser->pending);
    S_settle(parser, node, parent);
}

/* The pending block settles into `holder`, the current block, as it takes
 * its next child or closes. */
static inline void S_settle_pending(markdown_core_parser *parser, markdown_core_node *holder) {
    markdown_core_node *pending = parser->pending;
    if (pending) {
        parser->pending = NULL;
        S_settle(parser, pending, holder);
    }
}

/* A BLOCK CLOSES, its range settled, inside `parent`: a list and its items
 * fold whether they end with a blank line from their children, its element's
 * close hook makes the node it settles as, and a block that takes text lines
 * in a container takes the runs of its own lines. False when the block
 * yields no node of its own: a paragraph that held only reference
 * definitions yielded them as References settled into `parent` before it. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_close(markdown_core_parser *parser, markdown_core_node *b,
                                                                    markdown_core_node *parent) {
    /* A block that takes text lines in a container has a run of its own
     * source on each of its lines, whatever its close hook makes of it; one
     * of the document itself lies on whole lines, its own source its range. */
    const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, b);
    const bool lines = parent && parent != parser->root && S_kind_takes_text(kind, b);
    const markdown_core_element_instance *structure = kind->structure;
    if (kind->flags & MARKDOWN_CORE_KIND_BLANK_PROPAGATES) {
        S_fold_ends_blank(b);
    }
    if (structure && structure->element->close_block &&
        !structure->element->close_block(structure, parser, b, parent)) {
        markdown_core_parser_release_node(parser, b);
        return false;
    }
    if (lines) {
        int line = parser->line_number;
        markdown_core_parser_line_places(parser, b, parent, &line);
    }
    return true;
}

void markdown_core_block_close_into(markdown_core_parser *parser, markdown_core_node *b, markdown_core_node *parent) {
    if (S_close(parser, b, parent)) {
        markdown_core_block_settle(parser, b, parent);
    }
}

/* The current block, its range settled, closes: its pending child settles
 * into it, it closes, and it leaves the stack to be the pending child of the
 * block below. */
static void S_close_current(markdown_core_parser *parser) {
    markdown_core_node *closing = parser->current;
    markdown_core_node *parent = markdown_core_scaffold_parent(closing);
    assert(closing->flags & MARKDOWN_CORE_NODE__OPEN);
    S_settle_pending(parser, closing);
    closing->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    bool kept = S_close(parser, closing, parent);
    markdown_core_scaffold_of(parent)->last = NULL;
    parser->current = parent;
    parser->pending = kept ? closing : NULL;
}

void markdown_core_block_close_current(markdown_core_parser *parser) {
    assert(parser->current != parser->block_root);
    S_close_current(parser);
}

/* THE BUILDER STACK CLOSES FROM THE TOP. A block closes after everything
 * open inside it, and a block the line machine adds to a container first
 * closes what is open inside the container, so a block closes after every
 * one of its children has: what its element's close hook reads is a
 * finished subtree. `b` is open above the active input's root, which only
 * the input's end closes. */
markdown_core_node *markdown_core_block_close(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_node *closing;
    assert(b != markdown_core_parser_block_root(parser));
    /* The stack closes from its top down to `b`. */
    do {
        closing = markdown_core_parser_current(parser);

        if (parser->curline.size == 0) {
            // end of input - line number has not been incremented
            closing->where.place.end = (uint32_t)parser->last_line_end;
        } else if (/* D35: a block closed on the line it OPENED did not end on
                    * the previous one. `line_number - 1` below assumes the block
                    * was closed by a later line, which is true of every block that
                    * needs a following line to end it -- and false of an HTML block
                    * of type 2 to 5, whose terminator can be on its own first line.
                    * Measured: `<!-- c -->` alone on line 3 gave
                    * `HTMLBlock scope=3:1..2:0` for a literal whose last byte is at
                    * 3:10, and the last line's end there was the end of the BLANK
                    * line before it. Four of the eleven observed negative rows
                    * were this. */
                   S_starts_on_line(parser, closing, parser->line_number) ||
                   /* M0: the same block closed by its end condition on a LATER
                    * line. The terminator line is the block's last line and is
                    * the line being processed, so the block ends here too:
                    * `<!--\nmulti\n-->` gave `HTMLBlock scope=1:1..2:5`, one line
                    * short of the `-->` its literal holds, and `<pre>\nx\n</pre>`
                    * the same shape. Only the last two ways a block can close --
                    * the input ending and a container closing under it -- still
                    * end it on the line before. */
                   (closing->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0) {
            markdown_core_block_set_end_to_current_line(parser, closing);
        } else {
            closing->where.place.end = (uint32_t)parser->last_line_end;
        }

        S_close_current(parser);
    } while (closing != b);
    return markdown_core_parser_current(parser);
}

void markdown_core_parser_close_below(markdown_core_parser *parser, markdown_core_node *container) {
    /* The block open inside the container is the one above it. */
    if (parser->current != container) {
        markdown_core_block_close(parser, markdown_core_scaffold_last(container));
    }
}

markdown_core_node *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type) {
    assert(parent);

    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!markdown_core_node_can_contain_type(parent, block_type)) {
        if (parent == markdown_core_parser_block_root(parser)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED);
            return NULL;
        }
        parent = markdown_core_block_close(parser, parent);
    }
    return parent;
}

// Add a node as child of another.  Return pointer to child.
markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column) {
    parent = markdown_core_block_parent_for(parser, parent, block_type);
    return parent ? markdown_core_parser_add_child_validated(parser, parent, block_type, start_column) : NULL;
}

/* A selected parent is a semantic decision, not a hint to repeat the search. */
markdown_core_node *markdown_core_parser_add_child_validated(markdown_core_parser *parser, markdown_core_node *parent,
                                                             markdown_core_node_type block_type, int start_column) {
    assert(parent);
    /* The container takes its next child: what is open inside it closes
     * first, and its pending child settles (THE BUILDERS, parser.h). */
    markdown_core_parser_close_below(parser, parent);
    S_settle_pending(parser, parent);
    markdown_core_node *child =
        make_block(parser, block_type, markdown_core_parser_source_offset(parser, parser->line_number, start_column));
    if (!child || child->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        if (child) {
            markdown_core_parser_release_node(parser, child);
        }
        return NULL;
    }
    /* block_parent_for already established containment. The child is the
     * parent's open child, the builder above it, until it settles. */
    markdown_core_scaffold_of(child)->parent = parent;
    markdown_core_scaffold_of(parent)->last = child;
    parser->current = child;
    return child;
}

/* A WALK OF ONE INLINE TREE, over the scaffold it is built with (node.h):
 * every node yields one ENTER and one EXIT, with its children's events
 * between them, and DONE follows the root's EXIT. The next event is fixed as
 * the current one is delivered, so at a node's EXIT the walk already names
 * what follows the node, and the node may be freed. */
typedef struct {
    markdown_core_node *root;
    markdown_core_node *node;
    markdown_core_node *next;
    markdown_core_event_type next_event;
} inline_walk;

static inline void S_walk_init(inline_walk *walk, markdown_core_node *root) {
    *walk = (inline_walk){root, NULL, root, MARKDOWN_CORE_EVENT_ENTER};
}

/* The event after `node`'s EXIT: its next sibling's ENTER, its parent's
 * EXIT, or DONE after the root's. */
static inline void S_walk_after(inline_walk *walk, markdown_core_node *node) {
    markdown_core_node *next = node == walk->root ? NULL : markdown_core_scaffold_next(node);
    if (node == walk->root) {
        walk->next_event = MARKDOWN_CORE_EVENT_DONE;
        walk->next = NULL;
    } else if (next) {
        walk->next_event = MARKDOWN_CORE_EVENT_ENTER;
        walk->next = next;
    } else {
        walk->next_event = MARKDOWN_CORE_EVENT_EXIT;
        walk->next = markdown_core_scaffold_parent(node);
    }
}

static inline markdown_core_event_type S_walk_step(inline_walk *walk) {
    markdown_core_event_type event = walk->next_event;
    markdown_core_node *node = walk->next;
    walk->node = node;
    if (event == MARKDOWN_CORE_EVENT_ENTER) {
        markdown_core_node *first = markdown_core_scaffold_first(node);
        walk->next_event = first ? MARKDOWN_CORE_EVENT_ENTER : MARKDOWN_CORE_EVENT_EXIT;
        walk->next = first ? first : node;
    } else if (event == MARKDOWN_CORE_EVENT_EXIT) {
        S_walk_after(walk, node);
    }
    return event;
}

/* THE INLINE PARSER'S OWN FIELD PARSE. A token that owns fields -- a
 * citation's affixes, a directive's label -- has them parsed by the parse
 * that made it, before it scans on (complete_inline_token in inlines.c), so
 * that the token can be closed on what the fields turned out to hold. That
 * parse walks the field's small tree; the document's content tree is not
 * parsed this way but at each container's ENTER of completion's walk
 * (walk_owned_trees), which is why the walk does not parse below a node it
 * reached inside an inline tree. Inline parsing completes fields at their
 * owning token; the structural walk here therefore skips the emitted inline
 * tree. */
static bool process_inline_tree(markdown_core_parser *parser, markdown_core_node *root) {
    inline_walk walk;
    markdown_core_event_type event;
    bool whitespace = false;

    S_walk_init(&walk, root);
    while (!parser->error && (event = S_walk_step(&walk)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *cur = walk.node;
        if (event == MARKDOWN_CORE_EVENT_ENTER) {
            const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, cur);
            if (S_kind_contains_inlines(kind, cur)) {
                if (!(kind->flags & MARKDOWN_CORE_KIND_DEFERRED)) {
                    whitespace |= markdown_core_parse_inlines(parser, cur, false);
                }
                walk.next_event = MARKDOWN_CORE_EVENT_EXIT;
                walk.next = cur;
            }
            whitespace |= markdown_core_parse_inline_subtrees(parser, cur);
        }
    }
    return whitespace;
}

/* Core and element fields participate in the same parser phases. The
 * private title root stays owned here from source capture through cleanup. */

typedef struct {
    markdown_core_parser *parser;
    bool whitespace;
} inline_parse_context;

static int parse_inline_field(markdown_core_node **root_slot, void *context) {
    inline_parse_context *fields = context;
    if (root_slot && *root_slot && !fields->parser->error) {
        fields->whitespace |= process_inline_tree(fields->parser, *root_slot);
    }
    return !fields->parser->error;
}

bool markdown_core_parse_inline_subtrees(markdown_core_parser *parser, markdown_core_node *node) {
    inline_parse_context context = {parser, false};
    if (!markdown_core_visit_inline_subtrees(node, parse_inline_field, &context)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return context.whitespace;
}

/* ONE FRAME PER ROOT THE WALK IS INSIDE: the walk of the root's inline tree,
 * held in the frame, rooted at a field root, or, at the bottom, at the
 * queued block the document's walk is at.
 *
 * `parses` and `parsed` are the inline parser's reach in this frame. The
 * content tree's nodes are the walk's to parse, and so are the fields of its
 * block nodes (a definition's term, a callout's title, a table's caption, a
 * block directive's label); a field of an inline node is not -- the inline
 * parser completed it when it made the token (process_inline_tree above) --
 * and nor is a definition the document owns as a root of its own, whose
 * content was made as inline content. `parsed` is the container whose inline
 * tree the walk is inside, or NULL: nothing below a parsed container is
 * parsed again, since an inline node's content is its container's, already
 * parsed. */
typedef struct {
    inline_walk walk;
    markdown_core_node *parsed;
    int script_depth;
    bool parses;
} owned_tree_frame;

typedef struct {
    markdown_core_parser *parser;
    owned_tree_frame *frames;
    /* One word per projected step per frame, zero when a root's walk starts:
     * frame i's words are `states + i * slots`. Sized with the frames. */
    void **states;
    size_t slots;
    size_t count, capacity;
    /* What a root pushed now starts with: the pushing node's word depth and
     * whether its fields are the walk's to parse. */
    int script_depth;
    bool parses;
} owned_tree_walk;

/* Room for one frame more than the walk holds. */
static bool S_reserve_frame(owned_tree_walk *walk) {
    if (walk->count < walk->capacity) {
        return true;
    }
    size_t capacity = walk->capacity ? 2 * walk->capacity : 8;
    if (capacity > SIZE_MAX / sizeof(*walk->frames) ||
        (walk->slots && capacity > SIZE_MAX / sizeof(*walk->states) / walk->slots)) {
        markdown_core_parser_fail(walk->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    void *frames = markdown_core_parser_walk_stack(walk->parser, capacity, sizeof(*walk->frames));
    if (!frames) {
        markdown_core_parser_fail(walk->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    walk->frames = frames;
    if (walk->slots) {
        void *states = markdown_core_realloc(walk->states, capacity * walk->slots * sizeof(*walk->states));
        if (!states) {
            markdown_core_parser_fail(walk->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        walk->states = states;
    }
    walk->capacity = capacity;
    return true;
}

/* A root's state words start zero. */
static inline void S_clear_states(owned_tree_walk *walk, size_t frame) {
    void **states = walk->states + frame * walk->slots;
    for (size_t i = 0; i < walk->slots; i++) {
        states[i] = NULL;
    }
}

/* A field root's frame, its walk to begin at the root's ENTER. */
static int push_owned_root(markdown_core_node *root, owned_tree_walk *walk) {
    if (!root || walk->parser->error) {
        return !walk->parser->error;
    }
    if (!S_reserve_frame(walk)) {
        return 0;
    }
    S_clear_states(walk, walk->count);
    /* Word depth counts the inline word bodies around a node, and a block
     * begins content of its own: a root that is a block, such as an inline
     * note's Footnote, starts outside every word body its owner is in. */
    bool block = ((unsigned)root->kind & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK;
    owned_tree_frame *frame = &walk->frames[walk->count++];
    S_walk_init(&frame->walk, root);
    frame->parsed = NULL;
    frame->script_depth = block ? 0 : walk->script_depth;
    frame->parses = walk->parses;
    return 1;
}

/* A NODE'S COMPLETION IS THE COMPLETION WALK'S ENTER. A node is completed
 * once, the first time completion reaches it: the element's `complete_node`
 * (text.c turns an escaped space into NBSP inside a word; code_block.c makes
 * a fence that names a formula a FormulaBlock) reads the node, its links and
 * its ancestors' word depth, so the ENTER of the walk is where it belongs.
 * The walk reads the hook through its per-kind record; this form is for the
 * sibling consolidation absorbs at a Text's EXIT, whose ENTER is stepped
 * over, so consolidation completes it first. */
static void complete_consolidated_text(markdown_core_parser *parser, markdown_core_node *node, int script_depth) {
    const markdown_core_kind_record *plan = &parser->dialect->kinds[MARKDOWN_CORE_FINISH_TEXT_INDEX];
    assert(node->kind == MARKDOWN_CORE_NODE_TEXT);
    if (plan->complete) {
        plan->complete(plan->structure, parser, node, script_depth);
    }
}

/* Whether consolidation has anything to do at `text`'s EXIT: a Text sibling
 * to absorb, or no bytes of its own to keep. */
static inline bool S_text_needs_consolidation(const markdown_core_node *text) {
    const markdown_core_node *next = markdown_core_scaffold_next(text);
    return (next && next->kind == MARKDOWN_CORE_NODE_TEXT) || text->as.literal->len == 0;
}

/* TEXT CONSOLIDATION IS ONE STEP OF THE WALK, not a walk of its own.
 *
 * At `cur`'s EXIT, `cur` a Text: absorb every Text sibling that follows `cur`
 * into it (literal, source runs, end position), completing each first, as
 * its ENTER, which the walk steps over, would have; the walk's next event is
 * then the one after `cur` again, so no step is ever handed an absorbed
 * node. Then drop `cur` itself if it owns no bytes. The merged literal is
 * allocated once, at the run's summed length; the survivor takes the runs
 * the nodes it absorbed owned (requirement 11b).
 *
 * CONSUMED when `cur` was freed, FAILED on allocation failure (the tree is
 * consistent: every absorbed operand was unlinked before it was freed), and
 * CONTINUE otherwise. */
static markdown_core_finish_result S_consolidate_text(markdown_core_parser *parser, inline_walk *walk,
                                                      markdown_core_node *cur, int depth) {
    markdown_core_node *tmp, *next;

    assert(walk->node == cur && cur->kind == MARKDOWN_CORE_NODE_TEXT);
    tmp = markdown_core_scaffold_next(cur);
    if (tmp && tmp->kind == MARKDOWN_CORE_NODE_TEXT) {
        /* THE MERGED TEXT'S MAP IS A VIEW WHEN ITS OPERANDS ARE. A Text that
         * is a verbatim copy of its source holds a slice of its container's
         * runs (markdown_core_inline_map_text), and the siblings absorbed
         * here were placed left to right in that container, so while each
         * operand's bytes begin where the previous one's ended and its first
         * run is the previous one's last or the one after, the union is the
         * slice from the first operand's first run to the last operand's
         * last, at the first operand's offset -- the same runs the copy below
         * would append, answering every position the same way, and nothing
         * is appended. A decoded operand (its own run, at offset zero, a
         * literal shorter than its scope) or an operand with no map breaks
         * the chain, and the union is materialized run by run. */
        markdown_core_content_map combined_map = {0};
        bool view = cur->content_map.count > 0;
        int view_end = cur->content_map.first + cur->content_map.count;
        bufsize_t view_offset = cur->content_map.offset + cur->as.literal->len;
        /* THE MERGED LITERAL IS ALLOCATED ONCE, at its length. Every operand
         * is known before the first is absorbed, so the run's length is a sum
         * taken here, and each operand is copied to its place: no buffer
         * grows, and none is handed over and grown again for the next run. */
        size_t length = (size_t)cur->as.literal->len;
        for (; tmp && tmp->kind == MARKDOWN_CORE_NODE_TEXT; tmp = markdown_core_scaffold_next(tmp)) {
            length += (size_t)tmp->as.literal->len;
            if (!view) {
                continue;
            }
            view = tmp->content_map.count > 0 && tmp->content_map.offset == view_offset &&
                   tmp->content_map.first >= view_end - 1 && tmp->content_map.first <= view_end;
            view_end = tmp->content_map.first + tmp->content_map.count;
            view_offset += tmp->as.literal->len;
        }
        /* The bound every literal buffer shares. */
        if (length > (size_t)MARKDOWN_CORE_STRBUF_LIMIT) {
            return MARKDOWN_CORE_FINISH_FAILED;
        }
        if (view) {
            combined_map.first = cur->content_map.first;
            combined_map.count = view_end - cur->content_map.first;
            combined_map.offset = cur->content_map.offset;
        } else if (!markdown_core_parser_append_content_marks(parser, &cur->content_map, &combined_map, 0,
                                                              cur->as.literal->len, 0)) {
            return MARKDOWN_CORE_FINISH_FAILED;
        }
        unsigned char *merged = markdown_core_realloc(NULL, length + 1);
        if (!merged) {
            return MARKDOWN_CORE_FINISH_FAILED;
        }
        bufsize_t at = cur->as.literal->len;
        if (at) {
            memcpy(merged, cur->as.literal->data, (size_t)at);
        }
        tmp = markdown_core_scaffold_next(cur);
        while (tmp && tmp->kind == MARKDOWN_CORE_NODE_TEXT) {
            complete_consolidated_text(parser, tmp, depth);
            if (!view && !markdown_core_parser_append_content_marks(parser, &tmp->content_map, &combined_map, 0,
                                                                    tmp->as.literal->len, at)) {
                markdown_core_free(merged);
                return MARKDOWN_CORE_FINISH_FAILED;
            }
            if (tmp->as.literal->len) {
                memcpy(merged + at, tmp->as.literal->data, (size_t)tmp->as.literal->len);
                at += tmp->as.literal->len;
            }
            // ONLY AN OPERAND THAT OWNS BYTES CAN SAY WHERE THE RUN ENDS.
            // An empty one has no last byte to end at, and the empties in
            // this tree carry a zeroed position rather than an honest one,
            // so taking their end put `1:1..1:0` on a run of four real
            // characters.
            if (tmp->as.literal->len > 0) {
                cur->where.place.end = tmp->where.place.end;
            }
            next = markdown_core_scaffold_next(tmp);
            markdown_core_parser_release_node(parser, tmp);
            tmp = next;
        }
        /* Every node the loop freed followed `cur` and is unlinked, so the
         * event after `cur`'s EXIT is taken again from the siblings that
         * survived. */
        cur->content_map = combined_map;
        S_walk_after(walk, cur);
        markdown_core_chunk_free(cur->as.literal);
        merged[at] = '\0';
        *cur->as.literal = (markdown_core_chunk){merged, at, 1};
    }

    // A `TEXT` NODE THAT OWNS NO BYTES IS NOT A NODE. It has no literal to
    // render and no source to point at, so the only position it can carry
    // is borrowed or zeroed -- and a consumer that walks children sees a
    // child that is not there. Dropping it here also makes the third
    // producer unreachable by construction: a run of empties can no longer
    // merge into an empty, because the operands are gone before the merge.
    //
    // Freeing here is legal because `cur`'s EXIT is current: the walk's next
    // event already names a node outside this one's subtree. The caller
    // learns that `cur` is gone and hands this event to nothing else.
    if (cur->as.literal->len == 0) {
        markdown_core_chunk_free(cur->as.literal);
        markdown_core_parser_release_node(parser, cur);
        return MARKDOWN_CORE_FINISH_CONSUMED;
    }
    return MARKDOWN_CORE_FINISH_CONTINUE;
}

/* A CONTAINER'S CHILDREN SETTLE AT ITS EXIT: its scaffold children, every one
 * of them complete, become its children array, in order, in storage of the
 * pool's whose capacity their count determines (node.h), and their scaffold
 * links are cleared, so a node outside an inline tree under construction has
 * none. */
static bool S_settle_scaffold(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_scaffold *holder = markdown_core_scaffold_of(node);
    uint32_t count = 0;
    for (markdown_core_node *child = holder->first; child; child = markdown_core_scaffold_next(child)) {
        count++;
    }
    assert(!node->child_count);
    markdown_core_node **items =
        markdown_core_node_pool_bytes(parser->pool, (size_t)markdown_core_node_array_capacity(count) * sizeof(*items));
    if (!items) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    uint32_t at = 0;
    for (markdown_core_node *child = holder->first, *next; child; child = next) {
        markdown_core_scaffold *link = markdown_core_scaffold_of(child);
        next = link->next;
        link->parent = link->prev = link->next = NULL;
        items[at++] = child;
    }
    holder->first = holder->last = NULL;
    node->children = items;
    node->child_count = count;
    return true;
}

/* The steps projected for one event, in descriptor order, each behind its
 * gate, until one of them consumes the node. A step that consumes the node
 * ends the event: the node it named is gone and there is nothing left to
 * hand on.
 *
 * Returns CONSUMED when the node is gone, and FAILED with parser->error set. */
static markdown_core_finish_result run_finish_steps(markdown_core_parser *parser,
                                                    const markdown_core_finish_step_entry *entry,
                                                    markdown_core_node *node, markdown_core_event_type event,
                                                    void **states) {
    markdown_core_finish_result result = MARKDOWN_CORE_FINISH_CONTINUE;
    markdown_core_node *parent = markdown_core_scaffold_parent(node);
    for (; entry->instance; entry++) {
        if (!markdown_core_finish_step_admitted(entry, parser)) {
            continue;
        }
        result =
            entry->instance->element->finish_step(entry->instance, parser, node, parent, event, &states[entry->slot]);
        if (result != MARKDOWN_CORE_FINISH_CONTINUE) {
            /* A node is consumed only at its EXIT: at ENTER the lookahead
             * names its first child, and a step that freed it here would
             * have broken the contract the API header states. And the node
             * it consumed owned no field roots, which the walk pushed at its
             * ENTER: the contract again, and the in-tree step consumes Text,
             * which owns none. */
            assert(result == MARKDOWN_CORE_FINISH_FAILED || event == MARKDOWN_CORE_EVENT_EXIT);
            break;
        }
    }
    return result;
}

/* The owned-subtree visitor reports SLOTS, because destruction has to clear
 * them. A walk only reads one: no phase may substitute a field's root. */
static int push_owned_tree(markdown_core_node **slot, void *context) {
    return push_owned_root(slot ? *slot : NULL, context);
}

/* What completion does at a node's ENTER, in the frame at `index`: it parses
 * the node's inline content when the content is the frame's to parse
 * (`parses`, see owned_tree_frame; the content of a kind the record marks
 * PARSES, not DEFERRED, that the structure says holds inlines) and points the
 * frame's walk into that content; it runs the steps projected for the
 * ENTER, completes the node (the element's `complete_node` the record
 * carries), reserves its explicit anchor and pushes the node's field roots,
 * which are walked before anything below the node. Returns 1 when it
 * pushed a field root, 0 when it pushed none, and -1 with parser->error set. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) int S_enter_node(owned_tree_walk *walk, size_t index,
                                                                        markdown_core_node *node, size_t kind) {
    markdown_core_parser *const parser = walk->parser;
    owned_tree_frame *frame = &walk->frames[index];
    const markdown_core_kind_record *facts = &parser->dialect->kinds[kind];
    const markdown_core_finish_step_entry *steps = parser->dialect->finish_dispatch[2 * kind];
    unsigned flags = facts->flags;
    bool reach = frame->parses && !frame->parsed;
    if (reach && S_kind_contains_inlines(facts, node)) {
        if (!(flags & MARKDOWN_CORE_KIND_DEFERRED)) {
            markdown_core_parse_inlines(parser, node, true);
            if (parser->error) {
                return -1;
            }
        }
        /* The walk goes into the content next: its first node's ENTER, or
         * the node's EXIT when it holds none. */
        markdown_core_node *first = markdown_core_scaffold_first(node);
        frame->walk.next_event = first ? MARKDOWN_CORE_EVENT_ENTER : MARKDOWN_CORE_EVENT_EXIT;
        frame->walk.next = first ? first : node;
        frame->parsed = node;
    }
    if (steps && run_finish_steps(parser, steps, node, MARKDOWN_CORE_EVENT_ENTER, walk->states + index * walk->slots) ==
                     MARKDOWN_CORE_FINISH_FAILED) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return -1;
    }
    if (facts->complete) {
        facts->complete(facts->structure, parser, node, frame->script_depth);
    }
    /* An explicit anchor is reserved once the node is complete, at the
     * place it is written. A FormulaBlock a fence became holds no
     * attributes; a Text holds none either, so the siblings consolidation
     * absorbs reserve none. */
    const markdown_core_chunk *anchor = &node->attributes.anchor;
    if (anchor->len && !markdown_core_registry_declare(&parser->registry, MARKDOWN_CORE_REGISTRY_ANCHOR, anchor->data,
                                                       anchor->len, node->anchor_start)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return -1;
    }
    /* The field roots, of a kind that owns them through its record or of an
     * element that owns them through its hook: the answer is no for almost
     * every node, and it is one flag and one load. */
    if (!(flags & MARKDOWN_CORE_KIND_FIELDS) && !node->element) {
        return 0;
    }
    walk->script_depth = frame->script_depth;
    walk->parses = reach;
    size_t first = walk->count;
    if (((flags & MARKDOWN_CORE_KIND_FIELDS) && !markdown_core_visit_kind_fields(node, push_owned_tree, walk)) ||
        !markdown_core_visit_element_subtrees(node, push_owned_tree, walk)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return -1;
    }
    /* The visitor reports fields in source order; a stack consumes their
     * reversed registration order. No field is visited or scanned twice. The
     * frames carry no state yet -- a pushed root's words are zero until its
     * walk starts -- so swapping frames leaves nothing behind. */
    for (size_t left = first, right = walk->count; left < right && left < --right; left++) {
        owned_tree_frame swap = walk->frames[left];
        walk->frames[left] = walk->frames[right];
        walk->frames[right] = swap;
    }
    return walk->count != first;
}

/* THE COMPLETION WALK, on one explicit continuation stack. The bottom frame
 * is the walk of the document's queue, and it takes the queue a block at a
 * time: every block of the tree joined the queue as it settled
 * (S_complete_later), so nothing below a block that is a
 * block is that block's to walk. Each time the bottom walk is done, it starts
 * over at the next queued block: the block's ENTER is the queue's to deliver,
 * from word depth zero, since a block begins content of its own, and the
 * walk's next event is the block's EXIT unless the ENTER points it into the
 * block's inline content (S_enter_node). Above the bottom frame are the
 * fields the nodes own, roots of their own walked as they are found -- a
 * field root stays the node its owner put there, no step may substitute one,
 * so a frame carries the root itself and never the slot holding it, and depth
 * never uses C frames.
 *
 * Every event of every node is answered from the walk's own registers and the
 * parser's per-kind record: at a node's ENTER, S_enter_node; at a node's EXIT
 * its scaffold children settle into its children array (S_settle_scaffold),
 * at a Text's EXIT consolidation of the run when there is one to merge or a
 * Text with no bytes to drop, and then the steps projected for the EXIT of
 * its kind, in descriptor order, each behind its gate, asked with the node's
 * parent: its scaffold parent inside an inline tree, and for a queued block
 * the block it settled into, which the queue links it to. A field root's walk
 * ends when it is done, the document's when the queue is too. */
static int walk_owned_trees(markdown_core_parser *parser) {
    owned_tree_walk walk = {
        .parser = parser, .slots = parser->dialect->finish_step_slots, .script_depth = 0, .parses = true};
    const markdown_core_finish_step_entry *const *const dispatch = parser->dialect->finish_dispatch;
    markdown_core_node *queued = parser->completion_first;

    if (S_reserve_frame(&walk)) {
        /* The document's walk, done before its first block. */
        S_clear_states(&walk, 0);
        *walk.frames = (owned_tree_frame){.walk = {.next_event = MARKDOWN_CORE_EVENT_DONE}, .parses = true};
        walk.count = 1;
    }
    while (walk.count && !parser->error) {
        const size_t top = walk.count - 1;
        owned_tree_frame *frame = &walk.frames[top];
        /* `frame` is the top of the stack, so its state words are the last row. */
        void **states = walk.states + top * walk.slots;
        inline_walk *tree = &frame->walk;
        for (;;) {
            markdown_core_event_type event = S_walk_step(tree);
            markdown_core_node *node = tree->node;
            if (event == MARKDOWN_CORE_EVENT_DONE) {
                if (top || !queued) {
                    /* The end of a root's walk: a field root's, a tree of its
                     * own, or the document's. */
                    walk.count--;
                    break;
                }
                /* The next queued block leaves the queue. */
                node = queued;
                queued = markdown_core_scaffold_next(node);
                markdown_core_scaffold_of(node)->next = NULL;
                tree->root = node;
                tree->node = node;
                tree->next_event = MARKDOWN_CORE_EVENT_EXIT;
                tree->next = node;
                frame->script_depth = 0;
            } else {
                if (node->element && node->element->delimiter.body == DELIMITER_WORD_BODY) {
                    frame->script_depth += event == MARKDOWN_CORE_EVENT_ENTER ? 1 : -1;
                }
                if (event == MARKDOWN_CORE_EVENT_EXIT) {
                    if (node == frame->parsed) {
                        frame->parsed = NULL;
                    }
                    if (markdown_core_scaffold_first(node) && !S_settle_scaffold(parser, node)) {
                        break;
                    }
                    const size_t index = markdown_core_finish_kind_index((markdown_core_node_type)node->kind);
                    /* Consolidation goes first at a Text's EXIT and absorbs the
                     * siblings that follow it, so by the time an element step
                     * sees this event the Text holds its whole run and the
                     * absorbed nodes are gone -- they were never delivered to
                     * anything else, which is what makes freeing them safe. */
                    markdown_core_finish_result result = MARKDOWN_CORE_FINISH_CONTINUE;
                    if (index == MARKDOWN_CORE_FINISH_TEXT_INDEX && S_text_needs_consolidation(node)) {
                        result = S_consolidate_text(parser, tree, node, frame->script_depth);
                    }
                    if (result == MARKDOWN_CORE_FINISH_CONTINUE && dispatch[2 * index + 1]) {
                        result = run_finish_steps(parser, dispatch[2 * index + 1], node, event, states);
                    }
                    /* A root's walk is done with the link to the block that
                     * holds it: the queue's, for a queued block. */
                    if (node == tree->root) {
                        markdown_core_scaffold_of(node)->parent = NULL;
                    }
                    if (result == MARKDOWN_CORE_FINISH_FAILED) {
                        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                        break;
                    }
                    continue;
                }
            }
            /* A node is consumed only at its EXIT, so every ENTER reaches here. */
            if (S_enter_node(&walk, top, node, markdown_core_finish_kind_index((markdown_core_node_type)node->kind))) {
                /* A failure, or field roots pushed: the push may have moved
                 * the frames, so the top is taken afresh. */
                break;
            }
        }
    }
    /* What is left of the queue is the blocks the walk did not reach. */
    parser->completion_first = queued;
    if (!queued) {
        parser->completion_last = NULL;
    }
    markdown_core_free(walk.states);
    return !parser->error;
}

/* THE END OF THE BLOCK PHASE: every block still open closes at the end of
 * the input, the blocks of the table cells queued for it are read, and the
 * document closes last, so it is the last block the queue holds, where its
 * input ended. */
static void S_parse_block_inputs(markdown_core_parser *parser);
static void S_close_document(markdown_core_parser *parser) {
    markdown_core_node *root = parser->root;
    markdown_core_parser_close_below(parser, root);
    S_settle_pending(parser, root);
    S_parse_block_inputs(parser);
    if (parser->error) {
        return;
    }
    root->where.place.end = (uint32_t)parser->last_line_end;
    root->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    if (S_close(parser, root, NULL)) {
        S_complete_later(parser, root, NULL);
    }
}

/* Each queued cell is the root of its own input: the stack holds only the
 * cell while its content is read, and its blocks settle into it as the input
 * ends. The cells are read on the idle input's storage, and the document's
 * input and line state wait unchanged until they are read. */
static void S_parse_block_inputs(markdown_core_parser *parser) {
    if (parser->block_input_cursor == parser->block_input_count) {
        return;
    }
    const markdown_core_parser_input document = parser->input;
    const int line_number = parser->line_number;
    const bufsize_t line_start = parser->line_start, line_end = parser->line_end;
    const bufsize_t last_line_end = parser->last_line_end;
    const unsigned char *lookahead_cursor = parser->lookahead_cursor;
    parser->input = parser->idle_input;
    while (parser->block_input_cursor < parser->block_input_count && !parser->error) {
        const markdown_core_block_input input = parser->block_inputs[parser->block_input_cursor++];
        markdown_core_node *owner = input.owner;
        parser->block_root = parser->current = owner;
        parser->input_enclosing = input.enclosing;
        parser->line_number = parser->line_marks[owner->content_map.first].line - 1;
        parser->last_line_end = parser->line_marks[owner->content_map.first].source;
        owner->flags |= MARKDOWN_CORE_NODE__OPEN;
        S_parse_source(parser, owner->content.ptr, (size_t)owner->content.size, (size_t)owner->content.size);
        /* A failed read ends the parse where it failed, as a failed commit
         * does: nothing open closes, and the builders open in the cell are
         * released with the parse. */
        if (parser->error) {
            break;
        }
        markdown_core_parser_close_below(parser, owner);
        S_settle_pending(parser, owner);
        owner->flags &= ~MARKDOWN_CORE_NODE__OPEN;
        /* Its content was read as its blocks, so none of it is its text. */
        owner->cut_front = owner->content.size;
        owner->content_map.count = 0;
        owner->content_map.offset = 0;
    }
    parser->idle_input = parser->input;
    parser->input = document;
    parser->line_number = line_number;
    parser->line_start = line_start;
    parser->line_end = line_end;
    parser->last_line_end = last_line_end;
    parser->lookahead_cursor = lookahead_cursor;
    if (!parser->error) {
        parser->block_root = parser->current = parser->root;
        parser->input_enclosing = 0;
    }
}

bool markdown_core_parser_commit(markdown_core_parser *parser, const char *source, size_t length, size_t limit) {
    if (!parser->error) {
        S_parse_source(parser, (const unsigned char *)source, length, limit);
    }
    return !parser->error;
}

size_t markdown_core_parser_commit_point(const markdown_core_parser *parser) {
    return parser->end_seen ? (size_t)parser->end_seen_at : parser->input.complete;
}

markdown_core_node *markdown_core_parser_finalize(markdown_core_parser *parser) {
    markdown_core_node *document = S_finish_parse(parser);
    S_release_parse(parser);
    return document;
}

void markdown_core_parser_discard(markdown_core_parser *parser) { S_release_parse(parser); }

markdown_core_node *markdown_core_parser_snapshot(markdown_core_parser *parser) {
    parser->snapshot = true;
    return markdown_core_parser_finalize(parser);
}

/* Materializing a normalized view is an allocation boundary, separate from
 * the ordinary borrowed view resolved inline by both consumers. */
static const unsigned char *S_normalize_input_line(markdown_core_parser *parser, const markdown_core_input_line *line,
                                                   markdown_core_line_facts *facts, bufsize_t length) {
    size_t bytes = ((size_t)length + 2 + sizeof(uint32_t) - 1) / sizeof(uint32_t) * sizeof(uint32_t);
    markdown_core_normalized_line *view =
        markdown_core_alloc(1, sizeof(*view) + bytes + (size_t)facts->nul_count * sizeof(uint32_t));
    if (!view) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    view->nuls = (uint32_t *)(void *)(view->bytes + bytes);
    unsigned char *out = view->bytes;
    for (size_t at = line->start; at < line->end; at++) {
        unsigned char byte = parser->input.source[at];
        if (byte) {
            *out++ = byte;
        } else {
            view->nuls[view->nul_count++] = (uint32_t)at;
            *out++ = 0xef;
            *out++ = 0xbf;
            *out++ = 0xbd;
        }
    }
    *out++ = '\n';
    *out = 0;
    view->next = parser->input.normalized;
    parser->input.normalized = view;
    facts->normalized = view;
    return view->bytes;
}

static inline const unsigned char *S_input_line_content(markdown_core_parser *parser, markdown_core_input_line *line,
                                                        bufsize_t *length) {
    size_t size = line->end - line->start;
    markdown_core_line_facts *facts = line->facts ? &parser->input.facts[line->facts - 1] : NULL;
    uint32_t nul_count = facts ? facts->nul_count : 0;
    /* Raw geometry is bounded when its input is installed. Only expansion
     * needs an additional bound at this allocation boundary. */
    *length = (bufsize_t)size;
    if (!nul_count) {
        return parser->input.source + line->start;
    }
    if (nul_count > ((size_t)(INT32_MAX / 2) - size) / 2) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    *length += (bufsize_t)(2 * nul_count);
    return facts->normalized ? facts->normalized->bytes : S_normalize_input_line(parser, line, facts, *length);
}

/* THE END OF A PHYSICAL SPAN: the first NUL, CR or LF at or after `cursor`,
 * or `end`. This alphabet belongs to the input contract (markdown_core.h), not
 * to a grammar: no dialect changes it, and this is the one scan that reads
 * every byte of every input. So it does not go through a class table
 * (markdown_core_scan_to_class, one load per byte), but compares a machine
 * word at a time against its three bytes and steps bytewise only through the
 * word that holds the boundary. (x - ones) & ~x & highs flags a word that
 * holds a zero byte; a word holds CR or LF when the word XOR that byte
 * repeated holds a zero. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) const
    unsigned char *S_source_span_end(const unsigned char *cursor, const unsigned char *end) {
    const uint64_t ones = UINT64_C(0x0101010101010101);
    const uint64_t highs = UINT64_C(0x8080808080808080);
    while ((size_t)(end - cursor) >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, cursor, sizeof(word));
        const uint64_t cr = word ^ (ones * '\r');
        const uint64_t lf = word ^ (ones * '\n');
        if ((((word - ones) & ~word) | ((cr - ones) & ~cr) | ((lf - ones) & ~lf)) & highs) {
            break;
        }
        cursor += sizeof(word);
    }
    for (; cursor < end; cursor++) {
        const unsigned char byte = *cursor;
        if (byte == '\0' || byte == '\r' || byte == '\n') {
            break;
        }
    }
    return cursor;
}

/* The sole physical-line scanner for root and mapped inputs. Grammar facts
 * live beside their line, so changing inputs drops them together. Inlining
 * is explicit: both GCC and Clang may otherwise outline this per-line step. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_input_line *S_extend_source_lines(markdown_core_parser *parser, size_t index) {
    assert(parser->input.size <= MARKDOWN_CORE_SOURCE_CAPACITY);
    while (index >= parser->input.line_count && parser->input.scanned < parser->input.size) {
        if (parser->input.line_count == parser->input.line_capacity) {
            void *lines = markdown_core_reserve(parser->input.lines, &parser->input.line_capacity,
                                                parser->input.line_count + 1, sizeof(*parser->input.lines));
            if (!lines) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return NULL;
            }
            parser->input.lines = lines;
        }
        markdown_core_input_line entry = {.start = (uint32_t)parser->input.scanned};
        uint32_t nul_count = 0;
        const unsigned char *source = parser->input.source;
        const unsigned char *cursor = source + entry.start;
        const unsigned char *end = source + parser->input.size;
        /* Scan spans ending at a line boundary or a normalization boundary.
         * The NUL count changes only at the latter, not on every source byte. */
        for (;;) {
            cursor = S_source_span_end(cursor, end);
            if (cursor == end || *cursor) {
                break;
            }
            nul_count++;
            cursor++;
        }
        entry.end = (uint32_t)(cursor - source);
        if (cursor < end && *cursor == '\r') {
            cursor++;
        }
        if (cursor < end && *cursor == '\n') {
            cursor++;
        }
        if (nul_count) {
            markdown_core_line_facts *facts = markdown_core_parser_extend_line_facts(parser, &entry);
            if (!facts) {
                return NULL;
            }
            facts->nul_count = nul_count;
            parser->input.mapped = true;
        }
        parser->input.scanned = (size_t)(cursor - source);
        parser->input_line_work += parser->input.scanned - entry.start;
        parser->input.lines[parser->input.line_count++] = entry;
    }
    return index < parser->input.line_count ? &parser->input.lines[index] : NULL;
}

/* External speculative readers share the driver's scanner. The driver calls
 * the static inline body directly, including when it advances the frontier. */
markdown_core_input_line *markdown_core_parser_extend_source_lines(markdown_core_parser *parser, size_t index) {
    markdown_core_input_line *found = S_extend_source_lines(parser, index);
    /* A line asked past the last one, or the last one before it is complete,
     * is a reading of the input's end. */
    if (found ? found->start >= parser->input.complete : !parser->error) {
        markdown_core_parser_see_end(parser);
    }
    return found;
}

markdown_core_line_facts *markdown_core_parser_extend_line_facts(markdown_core_parser *parser,
                                                                 markdown_core_input_line *line) {
    void *facts = markdown_core_reserve(parser->input.facts, &parser->input.fact_capacity, parser->input.fact_count + 1,
                                        sizeof(*parser->input.facts));
    if (!facts) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    parser->input.facts = facts;
    markdown_core_line_facts *entry = &parser->input.facts[parser->input.fact_count++];
    *entry = (markdown_core_line_facts){0};
    line->facts = (uint32_t)parser->input.fact_count;
    return entry;
}

/* Where the last complete line of `source` ends (parser.h, input_complete). */
static size_t S_complete_end(const unsigned char *source, size_t length) {
    size_t end = length;
    if (end && source[end - 1] == '\r') {
        end--;
    }
    while (end && source[end - 1] != '\n' && source[end - 1] != '\r') {
        end--;
    }
    return end;
}

/* THE DRIVER: reads the lines of `source` that start before `limit` as the
 * active input. A cell's content is an input of its own, installed and read
 * whole. The document's text is installed at its first commit and grows at
 * its end from one commit to the next, so its lines keep their geometry and
 * facts and the driver continues from the line it stopped before; the
 * envelope at its start is read with its first line. */
static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length, size_t limit) {
    assert(length <= MARKDOWN_CORE_SOURCE_CAPACITY);
    bool at_root = markdown_core_parser_block_root(parser) == parser->root;
    if (!at_root || !parser->input.source) {
        S_clear_normalized_lines(&parser->input);
        parser->input.scanned = 0;
        parser->input.line_count = 0;
        parser->input.fact_count = 0;
        parser->input.mapped = !at_root;
        parser->input.first_line = parser->line_number + 1;
        if (at_root) {
            parser->end_seen = false;
            parser->line_start = 0;
        }
    }
    parser->input.source = source;
    parser->input.size = length;
    parser->input.complete = at_root ? S_complete_end(source, length) : length;
    parser->input.last_line_ready = false;
    if (at_root && !parser->line_number && limit) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->read_document_prefix(document, parser, source, length);
    }
    while (!parser->error) {
        size_t index = (size_t)(parser->line_number + 1 - parser->input.first_line);
        size_t start = index < parser->input.line_count ? parser->input.lines[index].start : parser->input.scanned;
        if (start >= limit) {
            break;
        }
        markdown_core_input_line *found = S_extend_source_lines(parser, index);
        if (!found) {
            break;
        }
        bufsize_t content_length;
        const unsigned char *content = S_input_line_content(parser, found, &content_length);
        if (!content) {
            return;
        }
        /* Callbacks may grow the line index; keep only stable bytes/offsets. */
        size_t next = markdown_core_input_line_next(parser, found);
        parser->lookahead_cursor = source + next;
        parser->line_start = (bufsize_t)found->start;
        /* A document line's scan recorded where its content ends; a cell's
         * line ends where the cell's map places its last byte. */
        if (at_root) {
            parser->line_end = (bufsize_t)found->end;
        } else {
            parser->line_end = markdown_core_parser_mapped_source_end(parser, parser->line_number + 1, content_length);
        }
        S_process_line(parser, content, content_length);
        if (parser->claimed_cursor) {
            assert(parser->claimed_cursor >= source + next && parser->claimed_cursor <= source + length);
            parser->line_number = parser->claimed_line;
            parser->line_start = (bufsize_t)markdown_core_parser_visited_line(parser, parser->line_number)->start;
            parser->last_line_end = parser->claimed_last_end;
            parser->claimed_cursor = NULL;
        }
    }
}

// Check for thematic break.  On failure, return 0 and update
// thematic_break_kill_pos with the index at which the
// parse fails.  On success, return length of match.
// "...three or more hyphens, asterisks,
// or underscores on a line by themselves. If you wish, you may use
// spaces between the hyphens or asterisks."
// Find first nonspace character from current offset, setting
// parser->first_nonspace, parser->first_nonspace_column,
// parser->indent, and parser->blank. Does not advance parser->offset.
/* THE DISTANCE TO THE NEXT TAB STOP IS DERIVED, NOT CARRIED.
 *
 * This used to open with `chars_to_tab = TAB_STOP - (parser->column %
 * TAB_STOP)` and then maintain that counter across every space: decrement,
 * test for zero, reset. Two things were wrong with that. The division ran on
 * EVERY call, including the majority that consume no whitespace at all and
 * the ones that take the `first_nonspace <= offset` early-out; and `column`
 * is a SIGNED int32_t (`bufsize_t`), so `% TAB_STOP` cannot be compiled to an
 * AND -- the compiler must emit the sign-correcting sequence, about ten
 * instructions, measured at 10.6 Ir per call.
 *
 * The counter was never independent state: `first_nonspace_column` starts at
 * `column` and rises by one per space, so `chars_to_tab` equalled
 * `TAB_STOP - (first_nonspace_column % TAB_STOP)` at every point of the walk
 * -- including the reset, which is the case where that expression yields
 * TAB_STOP. Deriving it at the one place it is read leaves the space branch
 * with nothing to maintain, and pays for the division only on a line that
 * actually contains a tab. */
void markdown_core_block_find_first_nonspace(markdown_core_parser *parser, markdown_core_chunk *input) {
    char c;

    if (parser->first_nonspace <= parser->offset) {
        parser->first_nonspace = parser->offset;
        parser->first_nonspace_column = parser->column;
        while ((c = peek_at(input, parser->first_nonspace))) {
            if (c == ' ') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += 1;
            } else if (c == '\t') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += TAB_STOP - (parser->first_nonspace_column % TAB_STOP);
            } else {
                break;
            }
        }
    }

    parser->indent = parser->first_nonspace_column - parser->column;
    parser->blank = markdown_core_is_line_end(peek_at(input, parser->first_nonspace));
}

// Advance parser->offset and parser->column.  parser->offset is the
// byte position in input; parser->column is a virtual column number
// that counts each Unicode scalar once and expands tabs to tab stops.
// Source positions remain byte-based. The count parameter indicates
// how far to advance the offset.  If columns is true, then count
// indicates a number of columns; otherwise, a number of bytes.
// If advancing a certain number of columns partially consumes
// a tab character, parser->partially_consumed_tab is set to true.
void markdown_core_block_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count,
                                        bool columns) {
    char c;
    int chars_to_tab;
    int chars_to_advance;
    while (count > 0 && parser->offset < input->len && (c = peek_at(input, parser->offset))) {
        if (c == '\t') {
            chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
            if (columns) {
                parser->partially_consumed_tab = chars_to_tab > count;
                chars_to_advance = MIN(count, chars_to_tab);
                parser->column += chars_to_advance;
                parser->offset += (parser->partially_consumed_tab ? 0 : 1);
                count -= chars_to_advance;
            } else {
                parser->partially_consumed_tab = false;
                parser->column += chars_to_tab;
                parser->offset += 1;
                count -= 1;
            }
        } else {
            parser->partially_consumed_tab = false;
            parser->offset += 1;
            /* Valid UTF-8 is a caller precondition. Complete a virtual column
             * only at the scalar's end. Byte-counted advances may split a
             * scalar; column-counted advances always consume it completely. */
            bool scalar_end = parser->offset == input->len || (peek_at(input, parser->offset) & 0xC0) != 0x80;
            parser->column += scalar_end;
            count -= columns ? scalar_end : 1;
        }
    }
}

bool markdown_core_block_continue_indented(markdown_core_parser *parser, markdown_core_chunk *input, int continuation,
                                           bool accepts_blank) {
    bool res = false;

    if (parser->indent >= continuation) {
        markdown_core_block_advance_offset(parser, input, continuation, true);
        res = true;
    } else if (parser->blank && accepts_blank) {
        // Blankness is relative to the cursor after ancestor prefixes. Lists
        // require a first block; definition-list bodies first check whether
        // the blank run leads to a carried line. Footnotes and specimens
        // accept blank continuation directly, including during lookahead.
        markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
        res = true;
    }
    return res;
}

/* ONE CONTAINER'S CLAIM ON THE LINE at the parser's cursor, for the kinds whose
 * prefix the core knows: the block quote's `>`, the list item's indentation,
 * a footnote or specimen's four columns, and the list's rule for consecutive
 * blank lines. `check_open_blocks` asks it walking down the open spine, and
 * the block-start lookahead asks the same question of the same containers on
 * later lines, so a candidate's decision and the parse that follows it cannot
 * disagree about which lines a container owns.
 *
 * Returns false when the line does not carry the container's prefix. `taken`
 * is set when a list took the whole line: a second consecutive blank line at
 * indentation zero cannot open a block, every closable descendant was closed
 * by the first, and only a raw-line leaf still wants it. `joining` is the
 * block a lookahead is about to add, or NULL in the real pass. */
/* `structure` is the container's structure instance, which its caller
 * resolved once for the visit. */
static bool S_container_prefix_matches(markdown_core_parser *parser, const markdown_core_element_instance *structure,
                                       markdown_core_node *container, markdown_core_chunk *input,
                                       const markdown_core_node *joining, bool *taken) {
    return !structure || !structure->element->continue_container ||
           structure->element->continue_container(structure, parser, container, input, joining, taken);
}

static bool parse_element_block(markdown_core_parser *parser, const markdown_core_kind_record *kind,
                                markdown_core_node *container, markdown_core_chunk *input, bool *should_continue,
                                markdown_core_node **closing) {
    const markdown_core_element_instance *structure = kind->structure;
    int matched;

    if (!structure->element->last_block_matches) {
        return false;
    }

    matched = structure->element->last_block_matches(structure, parser, input->data, input->len, container);
    if (matched && structure->element->pending_close) {
        *closing = matched == MARKDOWN_CORE_BLOCK_PENDING_CLOSE ? container : NULL;
    } else if (matched && S_kind_accepts_lines(kind, container)) {
        *closing = NULL;
    }
    if (matched != MARKDOWN_CORE_BLOCK_CLOSED) {
        return matched != 0;
    }

    /* The container's own closing line. Everything still open inside it ended
     * on the line before, and the container ends here. */
    *should_continue = false;
    /* A block survives its own close and can still be positioned. */
    assert(!(kind->flags & MARKDOWN_CORE_KIND_IS_PARAGRAPH));
    container->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
    markdown_core_block_close(parser, container);
    return false;
}

static markdown_core_node *check_open_blocks(markdown_core_parser *parser, markdown_core_chunk *input,
                                             bool *all_matched) {
    bool should_continue = true;
    *all_matched = false;
    markdown_core_node *container = markdown_core_parser_block_root(parser);
    markdown_core_node *closing = NULL;

    while (container != parser->current) {
        /* Whatever a container's prefix consumed is that container's
         * MARKER: `> ` belongs to the block quote, the item's indent to the
         * list item. So the bytes of the block below begin where the
         * prefixes above it end: one claim per block, walking down the
         * spine, each naming the line by number, as matching a container can
         * read lines ahead, which can move the line table. */
        markdown_core_parser_visited_line(parser, parser->line_number)->own = (uint32_t)parser->offset;
        container = markdown_core_scaffold_last(container);

        markdown_core_block_find_first_nonspace(parser, input);

        const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, container);
        const markdown_core_element_instance *structure = kind->structure;
        if (structure && structure->element->last_block_matches) {
            if (!parse_element_block(parser, kind, container, input, &should_continue, &closing)) {
                goto done;
            }
        } else {
            bool taken = false;
            if (structure && structure->element->accepts_blank && parser->blank &&
                !structure->element->accepts_blank(structure, parser, container)) {
                goto done;
            }
            if (!S_container_prefix_matches(parser, structure, container, input, NULL, &taken)) {
                goto done;
            }
            if (taken) {
                markdown_core_node *current = markdown_core_parser_current(parser);
                if (S_kind_accepts_lines(markdown_core_parser_kind(parser, current), current)) {
                    markdown_core_block_add_line(current, input, parser);
                }
                return NULL;
            }
        }
    }

    *all_matched = true;

done:
    if (closing) {
        /* The container ends on its own closing line, which it knows as it
         * closes; what is open inside it ended on the line before. */
        closing->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
        markdown_core_block_close(parser, closing);
        return NULL;
    }
    /* A container whose prefix consumed bytes and then declined still read
     * them; they are its marker up to the point it gave up. */
    if (!*all_matched) {
        container = markdown_core_scaffold_parent(container); // back up to last matching node
    }

    if (!should_continue) {
        container = NULL;
    }

    return container;
}

/* --- Block-start lookahead ---------------------------------------------------
 *
 * The block parser reads one line at a time and never rewinds. A block start
 * whose grammar depends on a later line -- the `%%` block comment is a
 * paragraph line unless a closer line follows under the same container
 * prefixes -- therefore looks ahead here BEFORE it opens anything, so a
 * candidate that fails consumes nothing, and one that succeeds is a block the
 * parser then reads line by line exactly as the lookahead saw it.
 *
 * Exactness is the whole contract. Every later line is offered as the block
 * parser will see it: the containers between the root and the block's parent
 * are matched by `S_container_prefix_matches`, the operation `check_open_blocks`
 * runs, through the same cursor fields, with the same list flags, which are
 * saved and restored around the lookahead. A container an element owns is
 * asked through its `continues_block` hook, the side-effect-free form of its
 * matcher. So a line the lookahead counts as carrying the prefixes is one
 * the block parser will hand to the new block, and the first line it rejects
 * is where the block parser will close the block's container.
 *
 * Linearity is the other contract, and it is why the cache exists. Two failed
 * candidates whose scans reach the same line have nested chains: the later
 * candidate's line lay inside the earlier one's scan, so every container open
 * at the earlier candidate was still open, and the later one's chain extends
 * it. The earlier scan therefore leaves, on every line it visited, the state
 * after the deepest container it matched, and a later scan resumes from
 * there: each (container, line) prefix is matched once per parse, and a line
 * visited by a deeper scan costs the bytes of the containers it adds. Blank
 * lines, which cost no bytes, are recorded as runs, and a scan whose extra
 * containers are lists and list items -- the containers that accept every
 * blank line -- steps over a recorded run at once. Footnote definitions
 * accept a blank line by its shape, so a scan visits their blank lines one by
 * one; their nesting is bounded by MAX_FOOTNOTE_DEPTH, so that visit count is
 * a constant factor. A block quote rejects a blank line, so a scan below one
 * ends at the run's first line. Deliberately nested failing candidates inside
 * directive containers cannot be built: a `%%` line in an inner container is
 * the outer candidate's closer, because the container adds no prefix. */

static bool S_lookahead_reserve_chain(markdown_core_parser *parser, int depth) {
    int capacity;
    markdown_core_lookahead_container *chain;

    if (depth <= parser->lookahead_chain_alloc) {
        return true;
    }
    capacity = parser->lookahead_chain_alloc ? parser->lookahead_chain_alloc : 16;
    while (capacity < depth) {
        capacity = capacity > INT_MAX / 2 ? INT_MAX : capacity * 2;
    }
    chain = markdown_core_realloc(parser->lookahead_chain, (size_t)capacity * sizeof(*chain));
    if (!chain) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->lookahead_chain = chain;
    parser->lookahead_chain_alloc = capacity;
    return true;
}

static void S_lookahead_close_run(markdown_core_block_lookahead *lookahead, int line, const unsigned char *cursor) {
    if (lookahead->run_start) {
        markdown_core_line_facts *entry = markdown_core_parser_get_line_facts(lookahead->parser, lookahead->run_start);
        if (entry) {
            entry->run_end = line;
            entry->run_end_cursor = cursor;
        }
        lookahead->run_start = 0;
    }
}

/* Whether the containers chain[from .. depth) accept every blank line. */
static bool S_lookahead_extras_accept_blank(const markdown_core_parser *parser, int from, int depth) {
    int i;
    for (i = from; i < depth; i++) {
        if (!(markdown_core_parser_kind(parser, parser->lookahead_chain[i].node)->flags &
              MARKDOWN_CORE_KIND_BLANK_RUNS)) {
            return false;
        }
    }
    return true;
}

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, markdown_core_node *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead) {
    memset(lookahead, 0, sizeof(*lookahead));
    /* The block joins the nearest open container that can hold it, which is
     * where `markdown_core_parser_add_child` backs up to when it is opened:
     * the containers the lookahead matches are the stack up to it. */
    markdown_core_node *parent = parent_container;
    while (parent != parser->block_root && !markdown_core_node_can_contain_type(parent, child)) {
        parent = markdown_core_scaffold_parent(parent);
    }
    int depth = 0;
    for (markdown_core_node *node = parent; node; node = markdown_core_parser_builder_below(parser, node)) {
        depth++;
    }
    if (!S_lookahead_reserve_chain(parser, depth)) {
        return false;
    }
    int at = depth;
    for (markdown_core_node *node = parent; node; node = markdown_core_parser_builder_below(parser, node)) {
        parser->lookahead_chain[--at] = (markdown_core_lookahead_container){node, node->flags};
    }

    lookahead->parser = parser;
    lookahead->parent = parent;
    lookahead->depth = depth;
    lookahead->cursor = parser->lookahead_cursor;
    lookahead->line = parser->line_number + 1;
    lookahead->saved_offset = parser->offset;
    lookahead->saved_column = parser->column;
    lookahead->saved_first_nonspace = parser->first_nonspace;
    lookahead->saved_first_nonspace_column = parser->first_nonspace_column;
    lookahead->saved_indent = parser->indent;
    lookahead->saved_blank = parser->blank;
    lookahead->saved_partially_consumed_tab = parser->partially_consumed_tab;
    lookahead->active = true;
    return true;
}

int markdown_core_parser_lookahead_next(markdown_core_block_lookahead *lookahead, markdown_core_chunk *line,
                                        int *first_nonspace, int *indent, int *blank_lines) {
    markdown_core_parser *parser = lookahead->parser;

    *blank_lines = 0;
    if (!lookahead->active) {
        return 0;
    }
    while (lookahead->cursor && !markdown_core_parser_input_ends(parser, lookahead->cursor) && !parser->error) {
        const unsigned char *start = lookahead->cursor;
        const unsigned char *next;
        markdown_core_chunk input;
        markdown_core_line_facts *entry;
        int this_line = lookahead->line;
        int from = 1;
        bufsize_t resumed_offset;
        bool carried = true;
        bool closing = false;
        bool taken = false;
        bool resumed = false;
        bool blank;
        int i;

        markdown_core_input_line *geometry = markdown_core_parser_source_line(parser, this_line);
        if (!geometry) {
            return 0;
        }
        bufsize_t content_length;
        const unsigned char *content = S_input_line_content(parser, geometry, &content_length);
        if (!content) {
            return 0;
        }
        next = parser->input.source + markdown_core_input_line_next(parser, geometry);
        parser->block_lookahead_work++;
        if (geometry->facts && parser->input.facts[geometry->facts - 1].nul_count) {
            input.data = (unsigned char *)content;
            input.len = content_length + 1;
        } else if (next == parser->input.source + geometry->end) {
            /* A line without a terminator, the input's last, normalized once:
             * the matchers read a line through its terminator. */
            if (!parser->input.last_line_ready) {
                markdown_core_strbuf_set(&parser->input.last_line, content, content_length);
                markdown_core_strbuf_putc(&parser->input.last_line, '\n');
                if (parser->input.last_line.oom) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    return 0;
                }
                parser->input.last_line_ready = true;
            }
            input.data = parser->input.last_line.ptr;
            input.len = parser->input.last_line.size;
        } else {
            input.data = (unsigned char *)start;
            input.len = (bufsize_t)(next - start);
        }
        input.alloc = 0;
        lookahead->cursor = next;
        lookahead->line = this_line + 1;

        parser->offset = 0;
        parser->column = 0;
        parser->first_nonspace = 0;
        parser->first_nonspace_column = 0;
        parser->indent = 0;
        parser->blank = false;
        parser->partially_consumed_tab = false;

        entry = markdown_core_parser_get_line_facts(parser, this_line);
        if (!entry && parser->error) {
            return 0;
        }
        if (entry && entry->container && entry->depth < lookahead->depth &&
            parser->lookahead_chain[entry->depth].node == entry->container) {
            resumed = true;
            from = entry->depth + 1;
            if (entry->taken) {
                taken = true;
            } else {
                parser->offset = entry->offset;
                parser->column = entry->column;
                parser->partially_consumed_tab = entry->partially_consumed_tab;
                parser->first_nonspace = parser->offset;
                parser->first_nonspace_column = parser->column;
            }
        }
        resumed_offset = parser->offset;
        for (i = from; i < lookahead->depth && carried && !taken; i++) {
            markdown_core_node *container = parser->lookahead_chain[i].node;
            markdown_core_block_find_first_nonspace(parser, &input);
            const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, container);
            if (structure && structure->element->last_block_matches) {
                int match =
                    structure->element->continues_block
                        ? structure->element->continues_block(structure, parser, input.data, (int)input.len, container)
                        : 0;
                carried = match != 0;
                if (carried && structure->element->pending_close) {
                    closing = match == MARKDOWN_CORE_BLOCK_PENDING_CLOSE;
                }
            } else {
                carried = S_container_prefix_matches(parser, structure, container, &input, lookahead->parent, &taken);
            }
        }
        /* The work of this visit is the prefix bytes it had to match itself:
         * what the resumed state did not already cover. */
        parser->block_lookahead_work += (size_t)(parser->offset - resumed_offset);
        if (!carried || closing) {
            S_lookahead_close_run(lookahead, this_line, start);
            lookahead->active = false;
            return 0;
        }
        if (!taken) {
            markdown_core_block_find_first_nonspace(parser, &input);
        }
        blank = taken || parser->blank;

        /* Element callbacks may append optional facts and move their vector.
         * A line number, not the earlier borrow, survives that boundary. */
        entry = markdown_core_parser_get_line_facts(parser, this_line);
        if (!entry) {
            return 0;
        }
        /* Record the deepest result for the scans that come after this one.
         * A run an earlier scan recorded from this line stays until this scan
         * closes its own, which ends where that one did. */
        if (entry) {
            markdown_core_parser_visited_line(parser, this_line)->own = (uint32_t)parser->offset;
            entry->container = lookahead->parent;
            entry->depth = lookahead->depth - 1;
            entry->offset = parser->offset;
            entry->column = parser->column;
            entry->partially_consumed_tab = parser->partially_consumed_tab;
            entry->taken = taken;
            entry->blank = blank;
        }

        if (blank) {
            *blank_lines += 1;
            if (!lookahead->run_start) {
                lookahead->run_start = this_line;
            }
            if (resumed && entry->run_end > this_line + 1 &&
                S_lookahead_extras_accept_blank(parser, from, lookahead->depth)) {
                *blank_lines += entry->run_end - this_line - 1;
                lookahead->cursor = entry->run_end_cursor;
                lookahead->line = entry->run_end;
            }
            continue;
        }
        S_lookahead_close_run(lookahead, this_line, start);
        *line = input;
        *first_nonspace = parser->first_nonspace;
        *indent = parser->indent;
        return 1;
    }
    S_lookahead_close_run(lookahead, lookahead->line, lookahead->cursor);
    lookahead->active = false;
    return 0;
}

void markdown_core_parser_lookahead_end(markdown_core_block_lookahead *lookahead) {
    markdown_core_parser *parser = lookahead->parser;
    int i;

    if (!parser) {
        return;
    }
    for (i = 0; i < lookahead->depth; i++) {
        markdown_core_node *node = parser->lookahead_chain[i].node;
        const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
        unsigned mask = structure ? structure->element->speculative_flags : 0;
        node->flags =
            (markdown_core_node_internal_flags)((node->flags & ~mask) | (parser->lookahead_chain[i].flags & mask));
    }
    parser->offset = lookahead->saved_offset;
    parser->column = lookahead->saved_column;
    parser->first_nonspace = lookahead->saved_first_nonspace;
    parser->first_nonspace_column = lookahead->saved_first_nonspace_column;
    parser->indent = lookahead->saved_indent;
    parser->blank = lookahead->saved_blank;
    parser->partially_consumed_tab = lookahead->saved_partially_consumed_tab;
    lookahead->active = false;
    lookahead->parser = NULL;
}

/* THE LINE NAMES ITS CANDIDATES; THE DISPATCHER DOES NOT ASK EVERY OWNER.
 *
 * A family's gates are projected into one list of owners per KEY, in the
 * family's own order: a key for each first non-space byte, one for a line
 * with no such byte, and one for an indented line. Reading the list for the
 * line's key is the whole admission test, so a line meets only the owners
 * whose grammar can start on it -- cmark's one if-chain keyed on the byte,
 * as a table built once from what the elements declared. An ungated owner is
 * on every list; a family in which no owner declares a gate has no table and
 * every owner is asked, which is the behaviour a gate replaces.
 *
 * The lists are projected when the dialect is sealed (dialect.c); each is
 * a count followed by owner indices. */

/* The owners of `hook`'s family a line with key `key` is asked, and how many;
 * NULL with `count` = the whole family when the family has no table. */
static const uint8_t *S_gate_candidates(const markdown_core_dialect *dialect, markdown_core_block_hook hook, int key,
                                        size_t *count) {
    const uint8_t *table = dialect->block_gate_lists[hook];
    if (!table) {
        *count = dialect->block_hook_counts[hook];
        return NULL;
    }
    const uint8_t *list = table + (size_t)key * (dialect->block_hook_counts[hook] + 1);
    *count = *list;
    return list + 1;
}

/* The key of the line at `first` for a family that asks an indented line by
 * indent bound rather than by byte (scan and interrupt). */
static int S_gate_key(const markdown_core_chunk *input, int first, int indent) {
    if (indent >= CODE_INDENT) {
        return MARKDOWN_CORE_BLOCK_GATE_KEY_INDENTED;
    }
    return first < input->len ? (int)(unsigned char)input->data[first] : MARKDOWN_CORE_BLOCK_GATE_KEY_NONE;
}

/* Ask the scan family about the line, in family order, and leave what the
 * claiming owner wrote in `start`; `start->open` is NULL when none claimed.
 * The payload fields are the claiming owner's to write before its `open` reads
 * them, so nothing is cleared per line beyond the three the dispatcher reads. */
static bool scan_element_start(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    const markdown_core_dialect *const dialect = parser->dialect;
    const markdown_core_element_instance *const *owners = dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_SCAN];
    size_t count;
    const uint8_t *candidates = S_gate_candidates(dialect, MARKDOWN_CORE_BLOCK_HOOK_SCAN,
                                                  S_gate_key(context->input, context->first, context->indent), &count);

    start->open = NULL;
    start->kind = MARKDOWN_CORE_NODE_NONE;
    start->matched = 0;
    for (size_t i = 0; i < count; i++) {
        const markdown_core_element_instance *owner = owners[candidates ? candidates[i] : i];
        /* The indent bound stays a runtime test rather than part of the
         * projection: `context->indent` is a property of the line, not of the
         * element set, so it cannot be folded into a table built once. */
        if (context->indent <= owner->element->maximum_block_indent &&
            owner->element->scan_block_start(owner, parser, context, start)) {
            start->owner = owner;
            return true;
        }
        if (parser->error) {
            return false;
        }
    }
    return false;
}

bool markdown_core_parser_has_block_start(markdown_core_parser *parser, markdown_core_node *parent,
                                          markdown_core_chunk *input, int first, int column, int indent, bool paragraph,
                                          markdown_core_block_reader *reader) {
    block_start_context context = {.container = parent,
                                   .input = input,
                                   .first = first,
                                   .column = column,
                                   .indent = indent,
                                   .paragraph = paragraph,
                                   .lazy = false,
                                   .all_matched = true,
                                   .depth = 1};
    block_start start;
    if (scan_element_start(parser, &context, &start)) {
        return true;
    }
    if (parser->error) {
        return false;
    }
    const markdown_core_dialect *const dialect = parser->dialect;
    const markdown_core_element_instance *const *probes = dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_PROBE];
    size_t probe_count = dialect->block_hook_counts[MARKDOWN_CORE_BLOCK_HOOK_PROBE];
    for (size_t element_index = 0; element_index < probe_count; element_index++) {
        const markdown_core_element_instance *probe = probes[element_index];
        if (probe->element->probe_block(probe, parser, input, first, indent, reader)) {
            return true;
        }
        if (parser->error) {
            break;
        }
    }
    return false;
}

static void open_new_blocks(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                            bool all_matched) {
    /* Two facts keep a line from interrupting text: `paragraph`, the matched
     * container is a paragraph the line would continue, and `maybe_lazy`, the
     * current block would take the line lazily. Only the first turn has
     * either: a block opened here holds the rest of the line. */
    bool maybe_lazy = S_may_be_lazy(parser, *container);
    size_t depth = 0;
    const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, *container);
    /* The instance's dialect is sealed: read its families through one local. */
    const markdown_core_dialect *const dialect = parser->dialect;

    while (!S_kind_accepts_lines(kind, *container)) {
        bool paragraph = kind->flags & MARKDOWN_CORE_KIND_IS_PARAGRAPH;
        /* Not cleared: the dispatcher writes the three fields it reads, and
         * the payload is the claiming owner's, written before its `open`
         * reads it (scan_element_start). Clearing the 120 bytes here was
         * paid by every line, the ones a code block or a properties block
         * takes without a round of this loop included. */
        block_start start;
        depth++;
        markdown_core_block_find_first_nonspace(parser, input);
        /* Indentation ahead of whatever opens here is the CONTAINER's, not the
         * new block's: a block begins at its own first non-space byte, so
         * giving the spaces to the block being opened would make its first
         * region start before its own scope. Measured before it was fixed --
         * 52 rows of an indented code block's four spaces alone. */
        block_start_context context = {.container = *container,
                                       .input = input,
                                       .first = parser->first_nonspace,
                                       .column = parser->first_nonspace_column,
                                       .indent = parser->indent,
                                       .paragraph = paragraph,
                                       .lazy = maybe_lazy,
                                       .all_matched = all_matched,
                                       .depth = depth,
                                       .thematic_kill = parser->thematic_break_kill_pos};
        scan_element_start(parser, &context, &start);
        if (parser->error) {
            return;
        }
        parser->thematic_break_kill_pos = context.thematic_kill;

        /* Dash-led tables precede thematic breaks and lists. An opener may
         * close the old path before an allocation fails; OOM is terminal,
         * never a grammar miss that can try another owner on that path. */
        const markdown_core_element_instance *const *interrupters =
            dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT];
        size_t interrupter_count;
        const uint8_t *interrupter_candidates =
            S_gate_candidates(dialect, MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT,
                              S_gate_key(input, parser->first_nonspace, parser->indent), &interrupter_count);
        for (size_t element_index = 0; element_index < interrupter_count; element_index++) {
            const markdown_core_element_instance *owner =
                interrupters[interrupter_candidates ? interrupter_candidates[element_index] : element_index];
            markdown_core_node *opened =
                owner->element->try_interrupting_block(owner, parser, *container, input, maybe_lazy);
            if (parser->error) {
                return;
            }
            if (opened) {
                *container = opened;
                return;
            }
        }

        if (start.open) {
            if (!start.open(start.owner, parser, container, input, &start)) {
                return;
            }
        } else {
            markdown_core_node *new_container = NULL;
            const markdown_core_element_instance *const *openers = dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_OPEN];
            size_t opener_count;
            /* An opener is asked about an indented line too, and told so: its
             * key is the byte alone. */
            const uint8_t *opener_candidates = S_gate_candidates(
                dialect, MARKDOWN_CORE_BLOCK_HOOK_OPEN, S_gate_key(input, parser->first_nonspace, 0), &opener_count);

            for (size_t element_index = 0; element_index < opener_count; element_index++) {
                const markdown_core_element_instance *opener =
                    openers[opener_candidates ? opener_candidates[element_index] : element_index];

                new_container =
                    opener->element->try_opening_block(opener, parser->indent > opener->element->maximum_block_indent,
                                                       parser, *container, input->data, input->len);
                if (parser->error) {
                    return;
                }

                if (new_container) {
                    *container = new_container;
                    if (parser->claimed_cursor) {
                        return;
                    }
                    break;
                }
            }

            if (!new_container) {
                if (!maybe_lazy && !paragraph) {
                    const markdown_core_element_instance *const *last_chance =
                        dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH];
                    size_t last_chance_count = dialect->block_hook_counts[MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH];
                    for (size_t element_index = 0; element_index < last_chance_count; element_index++) {
                        const markdown_core_element_instance *opener = last_chance[element_index];
                        new_container = opener->element->try_opening_paragraph(
                            opener, parser->indent > opener->element->maximum_block_indent, parser, *container,
                            input->data, input->len);
                        if (parser->error) {
                            return;
                        }
                        if (new_container) {
                            *container = new_container;
                            return;
                        }
                    }
                }
                break;
            }
        }

        /* What this opener consumed made the block it just opened, so the
         * block owns it: `> `, `- `, the `#`s of a heading, the opening fence,
         * `[^label]:`. Claimed once per turn of the loop -- once per block
         * opened -- and before a block that takes the text line breaks out. */

        kind = markdown_core_parser_kind(parser, *container);
        if (S_kind_takes_text(kind, *container)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        maybe_lazy = false;
    }
}

static void add_text_to_container(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_node *last_matched_container, markdown_core_chunk *input) {
    // what remains at parser->offset is a text line.  add the text to the
    // appropriate container.

    markdown_core_block_find_first_nonspace(parser, input);

    /* A blank line is the last line of the block before it that has not
     * settled: the one open inside the container, or the container's pending
     * child. */
    if (parser->blank) {
        markdown_core_node *before =
            container != parser->current ? markdown_core_scaffold_last(container) : parser->pending;
        if (before) {
            S_set_last_line_blank(before, true);
        }
    }

    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, container);
    const markdown_core_element_instance *structure = kind->structure;
    const bool accepts_lines = S_kind_accepts_lines(kind, container);
    const bool last_line_blank =
        parser->blank && !(kind->flags & MARKDOWN_CORE_KIND_BLANK_OPAQUE) &&
        (!(kind->flags & MARKDOWN_CORE_KIND_BLANK_ASK) ? !accepts_lines
                                                       : structure->element->blank_line(structure, parser, container));

    S_set_last_line_blank(container, last_line_blank);

    for (markdown_core_node *below = markdown_core_parser_builder_below(parser, container); below;
         below = markdown_core_parser_builder_below(parser, below)) {
        S_set_last_line_blank(below, false);
    }

    // A line that may be lazy, opened no block and is not blank is a lazy
    // line: the current block takes it.
    if (container == last_matched_container && !parser->blank && S_may_be_lazy(parser, last_matched_container)) {
        markdown_core_node *current = markdown_core_parser_current(parser);
        const markdown_core_element_instance *taker = markdown_core_parser_structure(parser, current);
        markdown_core_node *lazy = taker->element->open_lazy(taker, parser, current, input);
        if (!lazy) {
            return;
        }
        assert(lazy == markdown_core_parser_current(parser));
        /* A lazy line is text, so its indentation is not content, as it is not
         * on a line that continues a paragraph with every prefix. */
        markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
        markdown_core_block_add_line(lazy, input, parser);
    } else { // not a lazy continuation
        // Close the blocks the line did not match: those open below the
        // container that takes it.
        markdown_core_parser_close_below(parser, container);

        if (accepts_lines) {
            markdown_core_block_add_line(container, input, parser);
            if (structure->element->ends_block && structure->element->ends_block(structure, parser, container, input)) {
                container->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
                container = markdown_core_block_close(parser, container);
            }
        } else if (parser->blank) {
            // ??? do nothing
        } else if (kind->flags & MARKDOWN_CORE_KIND_PROSE) {
            markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            markdown_core_block_add_line(container, input, parser);
        } else {
            const markdown_core_element_instance *text_block = parser->dialect->text_block_structure;
            container = text_block->element->open_text_block(text_block, parser, container, input);
            if (!container) {
                return;
            }
            markdown_core_block_add_line(container, input, parser);
        }
        assert(container == markdown_core_parser_current(parser));
    }
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes) {
    markdown_core_node *last_matched_container;
    bool all_matched = true;
    markdown_core_node *container;
    markdown_core_chunk input;

    if (parser->error || parser->root == NULL) {
        return;
    }

    assert(parser->curline.size == 0);
    assert(bytes >= 0);
    /* The shared input view excludes its physical terminator. Construct the
     * mutable grammar line (content + LF + NUL) with one reservation, rather
     * than appending content and then rediscovering/adding its terminator. */
    if (bytes >= INT32_MAX / 2) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    if (bytes + 1 >= parser->curline.asize) {
        markdown_core_strbuf_grow(&parser->curline, bytes + 1);
    }

    if (parser->curline.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    memcpy(parser->curline.ptr, buffer, (size_t)bytes);
    parser->curline.ptr[bytes] = '\n';
    parser->curline.ptr[bytes + 1] = 0;
    parser->curline.size = bytes + 1;

    parser->offset = 0;
    parser->column = 0;
    parser->first_nonspace = 0;
    parser->first_nonspace_column = 0;
    parser->thematic_break_kill_pos = 0;
    parser->indent = 0;
    parser->blank = false;
    parser->partially_consumed_tab = false;

    input.data = parser->curline.ptr;
    input.len = parser->curline.size;
    input.alloc = 0;

    parser->line_number++;

    last_matched_container = check_open_blocks(parser, &input, &all_matched);

    if (!last_matched_container) {
        goto finished;
    }

    container = last_matched_container;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->error) {
        goto finished;
    }

    if (parser->claimed_cursor) {
        goto finished;
    }

    add_text_to_container(parser, container, last_matched_container, &input);

finished:
    /* Block scopes cover the complete physical line, including closing
     * delimiters and attribute containers. Inline content trimming never
     * changes this source boundary. */
    parser->last_line_end = parser->line_end;

    markdown_core_strbuf_clear(&parser->curline);
}

/* COMPLETION: every queued block, in the order it joined, as one walk over
 * the queue. A block is completed with its inline content and its field
 * roots, and rewritten in place; none is ever substituted. */
static void S_complete_queue(markdown_core_parser *parser) {
    if (!walk_owned_trees(parser)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

uint64_t markdown_core_source_key(const void *entry) {
    markdown_core_node *node;
    memcpy(&node, entry, sizeof(node));
    return node->where.place.start;
}

/* A STABLE LINEAR ORDERING BY SOURCE COORDINATE, keyed once.
 *
 * The key is a node's first source byte, and a counting pass per key byte
 * orders any set of them in linear work with no comparison-sort worst case.
 * What is not constant is WHICH bytes carry information: the entries a grid
 * table closes, or the headings of one document, differ in their low offset
 * bytes and agree on every other. A pass over a byte on which every
 * key agrees moves every entry to where it already is, so the passes run are
 * exactly those over bytes on which some key differs -- found by one pass
 * that also computes the keys, once, so that they travel with their entries
 * instead of being recomputed twice per entry per pass. The same pass sees
 * whether the entries are already in order, and a stable sort of an ordered
 * input is the identity, so no entry scratch is needed for it. Every input
 * shape is still bounded by eight passes; none pays for a pass that cannot
 * change it. */
void markdown_core_source_order_dispose(markdown_core_source_order *workspace) {
    markdown_core_free(workspace->keys);
    markdown_core_free(workspace->entries);
    *workspace = (markdown_core_source_order){0};
}

void *markdown_core_parser_walk_stack(markdown_core_parser *parser, size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
    size_t needed = count * size;
    if (needed > parser->walk_stack_size) {
        size_t grown = parser->walk_stack_size > SIZE_MAX / 2 ? SIZE_MAX : 2 * parser->walk_stack_size;
        size_t bytes = grown > needed ? grown : needed;
        void *stack = markdown_core_realloc(parser->walk_stack, bytes);
        if (!stack) {
            return NULL;
        }
        parser->walk_stack = stack;
        parser->walk_stack_size = bytes;
    }
    return parser->walk_stack;
}

int markdown_core_order_source_entries(markdown_core_source_order *workspace, void *entries, size_t count,
                                       size_t stride, uint64_t (*key)(const void *)) {
    if (count < 2) {
        return 1;
    }
    if (!stride || count > SIZE_MAX / stride || count > SIZE_MAX / (2 * sizeof(uint64_t))) {
        return 0;
    }
    unsigned char *source = entries;
    uint64_t *keys = markdown_core_reserve(workspace->keys, &workspace->key_capacity, 2 * count, sizeof(uint64_t));
    if (!keys) {
        return 0;
    }
    workspace->keys = keys;
    workspace->work += count;
    uint64_t *key_source = keys;
    uint64_t *key_target = keys + count;
    uint64_t differing = 0;
    bool ordered = true;
    key_source[0] = key(source);
    for (size_t i = 1; i < count; i++) {
        key_source[i] = key(source + i * stride);
        differing |= key_source[i] ^ key_source[0];
        ordered = ordered && key_source[i - 1] <= key_source[i];
    }
    if (ordered) {
        return 1;
    }
    unsigned char *target = markdown_core_reserve(workspace->entries, &workspace->entry_capacity, count * stride, 1);
    if (!target) {
        return 0;
    }
    workspace->entries = target;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        if (!((differing >> shift) & 255)) {
            continue;
        }
        size_t offsets[256] = {0};
        size_t offset = 0;
        workspace->work += 2 * count;
        for (size_t i = 0; i < count; i++) {
            offsets[(key_source[i] >> shift) & 255]++;
        }
        for (size_t byte = 0; byte < 256; byte++) {
            size_t length = offsets[byte];
            offsets[byte] = offset;
            offset += length;
        }
        for (size_t i = 0; i < count; i++) {
            size_t destination = offsets[(key_source[i] >> shift) & 255]++;
            key_target[destination] = key_source[i];
            memcpy(target + destination * stride, source + i * stride, stride);
        }
        unsigned char *swap = source;
        source = target;
        target = swap;
        uint64_t *key_swap = key_source;
        key_source = key_target;
        key_target = key_swap;
    }
    if (source != entries) {
        memcpy(entries, source, count * stride);
    }
    return 1;
}

/* THE FINISH STAGE. Every block joined the queue as it settled; completion
 * walks each queued block once, and everything else the stage does to a node
 * happens from inside that one walk: inline parsing and completion at a
 * node's ENTER, text consolidation at a Text's EXIT, and every step at the
 * events of the kinds it declared.
 *
 * What makes fusing them safe is the order the walk fixes. A Text's EXIT
 * comes after every Text sibling it will absorb has been produced and before
 * any step reads it, so a step at that EXIT sees the merged run. And an
 * owned root nested in a node -- a definition term, a citation's prefix --
 * is finished, popped and gone from the stack before that node's own
 * children are visited, so nothing still on the stack lives inside a tree a
 * step is rewriting.
 *
 * A step's gate is read at each event it is asked at, since a kind the step
 * acts on may first be made after the parse began. An empty declaration
 * means the step is always asked; one that declares the kinds it acts on is
 * skipped at the event in a parse that has produced none of them so far.
 * `kinds_created` OVER-APPROXIMATES: a node the parse creates and then
 * discards -- the text a formula consumes -- leaves its bit set although the
 * finished tree holds no such node. It can only make the gate skip FEWER
 * steps, never miss one, because a kind in the finished tree was necessarily
 * created; and a step asked where none of its declared kinds is finds
 * nothing to do. */
static markdown_core_node *S_finish_parse(markdown_core_parser *parser) {
    markdown_core_node *res;

    if (parser->root == NULL || parser->error) {
        return NULL;
    }

    S_close_document(parser);
    if (!parser->error) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->prepare_document(document, parser);
    }
    if (!parser->error) {
        S_complete_queue(parser);
    }

    /* The document's finalization reads the finished tree: the headings take
     * their anchors once every explicit anchor has been reserved, which
     * completion did at each node's ENTER. */
    if (!parser->error) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->finish_document(document, parser);
    }

    /* Last, the finished tree is published: nothing changes it after this. */
    if (!parser->error) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->publish_document(document, parser);
    }
    markdown_core_free(parser->walk_stack);
    parser->walk_stack = NULL;
    parser->walk_stack_size = 0;
    if (parser->error) {
        goto failed;
    }

    res = parser->root;
    parser->root = NULL;
    return res;

failed:
    parser->dialect->document_structure->element->dispose_document(parser->dialect->document_structure, parser);
    markdown_core_parser_release_node(parser, parser->root);
    parser->root = NULL;
    return NULL;
}

int markdown_core_parser_get_line_number(markdown_core_parser *parser) { return parser->line_number; }

bufsize_t markdown_core_parser_get_offset(markdown_core_parser *parser) { return parser->offset; }

bufsize_t markdown_core_parser_get_column(markdown_core_parser *parser) { return parser->column; }

int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser) { return parser->first_nonspace; }

int markdown_core_parser_get_first_nonspace_column(markdown_core_parser *parser) {
    return parser->first_nonspace_column;
}

int markdown_core_parser_get_indent(markdown_core_parser *parser) { return parser->indent; }

int markdown_core_parser_is_blank(markdown_core_parser *parser) { return parser->blank; }

int markdown_core_parser_has_partially_consumed_tab(markdown_core_parser *parser) {
    return parser->partially_consumed_tab;
}

bufsize_t markdown_core_parser_get_last_line_end(markdown_core_parser *parser) { return parser->last_line_end; }

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    markdown_core_block_advance_offset(parser, &input_chunk, count, columns != 0);
}
