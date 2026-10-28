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
#include "sink.h"
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
                                                               const unsigned char *source, size_t length);
static MARKDOWN_CORE_ATTRIBUTE((noinline)) markdown_core_node *S_finish_parse(markdown_core_parser *parser);
static inline bool S_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
static inline int S_append_input_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t line_start, int column, bufsize_t length, bufsize_t offset);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_process_held_line(markdown_core_parser *parser,
                                                                    const unsigned char *buffer, bufsize_t bytes,
                                                                    const markdown_core_line_stop *from, bool hold);
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
    e->place = (markdown_core_place){(uint32_t)start, (uint32_t)start};

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
    /* The line's facts that name it, counted from 1 (parser.h). */
    uint32_t fact;
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
    markdown_core_array_free(&input->lines);
    markdown_core_free(input->facts);
    markdown_core_strbuf_free(&input->last_line);
}

static void S_release_stream(markdown_core_parser *parser);
static void S_take_back(markdown_core_parser *parser, bool release);
static void S_candidate_make(markdown_core_parser *parser);
static void S_candidate_undo(markdown_core_parser *parser, size_t m, size_t index);
static void S_candidate_see(markdown_core_parser *parser, size_t m, size_t index);
static void S_candidates_release(markdown_core_parser *parser);
static bool S_feed(markdown_core_parser *parser, int through, const unsigned char *limit);
static bool S_candidates_end(markdown_core_parser *parser);
static bool S_candidates_see_end(markdown_core_parser *parser, size_t *m, size_t *index);
static markdown_core_node *const *S_candidate_path(const markdown_core_parser *parser, const markdown_core_node *parent,
                                                   int *depth);
static void S_meet_from_stop(markdown_core_parser *parser);
static void S_complete_settled(markdown_core_parser *parser);

/* The stack the stage's walks borrow, and what the walks keep for every
 * walk of the parse, let go. */
static void S_release_walk(markdown_core_parser *parser) {
    markdown_core_free(parser->walk_stack);
    parser->walk_stack = NULL;
    parser->walk_stack_size = 0;
    for (size_t table = 0; table < MARKDOWN_CORE_DEFINITION_TABLES; table++) {
        markdown_core_free(parser->definition_values[table]);
        parser->definition_values[table] = NULL;
        parser->definition_capacity[table] = 0;
    }
    markdown_core_free(parser->content_places);
    parser->content_places = NULL;
    parser->content_place_capacity = 0;
}

static void S_parser_dispose(markdown_core_parser *parser) {
    if (parser->stream) {
        S_take_back(parser, false);
        S_candidates_release(parser);
    }
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
    S_release_walk(parser);
    markdown_core_free(parser->block_inputs);
    S_release_input(&parser->input);
    if (parser->cell_input) {
        S_release_input(parser->cell_input);
        markdown_core_free(parser->cell_input);
    }
    if (parser->root) {
        markdown_core_node_free(parser->root);
    }
    if (parser->stream) {
        S_release_stream(parser);
    }

    /* The content-to-source map outlives every block that indexes it and
     * nothing else does, so it is released here rather than with the node. */
    markdown_core_array_free(&parser->line_marks);

    while (parser->free_delimiters) {
        delimiter *entry = parser->free_delimiters;
        parser->free_delimiters = entry->next;
        markdown_core_free(entry);
    }
    markdown_core_inline_release_records(parser);
    markdown_core_attribute_scratch_free(&parser->attribute_scratch);

    /* The block-start lookahead's path, flags and resume cache are parser
     * state of the same kind: indexed by open containers and source lines,
     * owned by no node, and dead with the parse. */
    markdown_core_free(parser->lookahead_path);
    parser->lookahead_path = NULL;
    parser->lookahead_containers = NULL;
    parser->lookahead_capacity = 0;
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

static inline bool S_reserve_content_marks(markdown_core_parser *parser, bufsize_t count);

/* Starts a transaction that continues `revision` and borrows its pool. */
void markdown_core_parser_start(markdown_core_parser *parser, markdown_core_revision *revision) {
    markdown_core_node *document;

    parser->revision = revision;
    parser->pool = revision->pool;
    parser->settle_flags = MARKDOWN_CORE_NODE__COMMITTED;
    markdown_core_strbuf_init(&parser->curline, 256);
    markdown_core_strbuf_init(&parser->input.last_line, 0);
    markdown_core_registry_init(&parser->registry);
    /* The line index is a parse-owned workspace, like curline. Establish its
     * initial capacity before any input; inputs reset length, never
     * ownership. */
    (void)markdown_core_array_reserve(&parser->input.lines, 1);
    /* The first run of the content map is the identity an inline root's
     * parse places on (parser.h, MARKDOWN_CORE_IDENTITY_MARK): first, so that
     * no map appended after it ends with it. */
    if (!markdown_core_array_reserve(&parser->line_marks, 64) ||
        !markdown_core_array_push(&parser->line_marks, ((markdown_core_line_mark){0, 0, 0, 1, 1, 0}))) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }

    document = make_document(parser);
    parser->root = document;
    parser->at.block_root = parser->at.current = document;
    /* WHAT A STREAMING PARSE KEEPS (parser.h, markdown_core_stream), released
     * with the parse's state: the lending's storage, the records and the
     * nodes they own, the committed definitions and the records of the
     * lookups that found nothing. Every parse streams: a one-shot parse is
     * a stream whose first commit is its whole text, and its finalize. */
    parser->stream = markdown_core_alloc(1, sizeof(*parser->stream) + parser->dialect->candidate_state_size +
                                                parser->dialect->kept_size);
    if (parser->stream) {
        markdown_core_strbuf_init(&parser->stream->miss_labels, 0);
        markdown_core_strbuf_init(&parser->stream->settled.labels, 0);
    }

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. Only a
     * complete transaction begins the document lifecycle -- its owner is
     * whichever element the dialect resolved it to, and it never sees a
     * failed transaction. Disposal does not depend on it having begun: the
     * lifecycle's own allocations can fail halfway, so its release already
     * takes whatever state it finds. */
    if (!parser->root || !parser->stream || !parser->input.lines.values || parser->curline.oom ||
        parser->input.last_line.oom || parser->root->content.oom) {
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
    b->place.end = (uint32_t)parser->at.line_end;
}

/* Whether a node of `kind` takes lines as its content, and whether it may
 * hold inline content: the two facts a hook answers per node. */
static bool S_kind_accepts_lines(const markdown_core_kind_record *kind, markdown_core_node *node) {
    return (kind->flags & MARKDOWN_CORE_KIND_LINES) ||
           ((kind->flags & MARKDOWN_CORE_KIND_LINES_ASK) &&
            kind->structure->element->accepts_lines_func(kind->structure->element, node));
}
/* Whether a node of `kind` takes a text line: as its content, or as prose. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_kind_takes_text(const markdown_core_kind_record *kind,
                                                                              markdown_core_node *node) {
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
    const markdown_core_element_instance *structure = markdown_core_parser_block_kind(parser, current)->structure;
    return structure && structure->element->accepts_lazy &&
           structure->element->accepts_lazy(structure, parser, current);
}

/* Record where the bytes about to be appended to `node`'s content came from.
 *
 * `column` is a BYTE column counted from 1, which is what every position in
 * the tree is counted in; `parser->at.column` is not one, because it counts a tab
 * as the several columns it expands to. */
/* One array owns all content/source runs (array.h), indexed by int32. A
 * run can start on the same source line as its predecessor when a
 * transformation removes bytes. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool S_extend_content_marks(markdown_core_parser *parser, bufsize_t count) {
    if ((size_t)count > INT32_MAX - parser->line_marks.count ||
        !markdown_core_array_reserve(&parser->line_marks, parser->line_marks.count + (size_t)count)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    return true;
}

static inline bool S_reserve_content_marks(markdown_core_parser *parser, bufsize_t count) {
    return parser->line_marks.count + (size_t)count <= parser->line_marks.room || S_extend_content_marks(parser, count);
}

/* A map's marks are contiguous and a map grows at the end of the vector. An
 * open block's map can find the end taken: the parse completes the blocks
 * that settled after each commit (plan 5.4), and their inline content takes
 * marks of its own. The map then moves its marks to the end first; the marks
 * it leaves behind stay as they are, where a map that shares them reads
 * them. Completion after a commit completes the blocks that settled in it,
 * and a block settles as the block after it opens or as a block closes, so
 * an open block whose map moves opened in that commit, and no mark moves
 * twice. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool S_move_content_marks(markdown_core_parser *parser,
                                                                     markdown_core_content_map *map) {
    if (!S_reserve_content_marks(parser, map->count)) {
        return false;
    }
    const bufsize_t first = (bufsize_t)parser->line_marks.count;
    markdown_core_array_append(&parser->line_marks, parser->line_marks.values + map->first, (size_t)map->count);
    map->first = first;
    return true;
}

static int S_append_content_mark(markdown_core_parser *parser, markdown_core_content_map *map,
                                 markdown_core_line_mark mark) {
    if (!parser || !map) {
        return 0;
    }
    mark.content_offset += map->offset;
    if (map->count) {
        if ((size_t)(map->first + map->count) != parser->line_marks.count && !S_move_content_marks(parser, map)) {
            return 0;
        }
        const size_t last = parser->line_marks.count - 1;
        if (parser->line_marks.values[last].content_offset == mark.content_offset) {
            *markdown_core_array_write(&parser->line_marks, last) = mark;
            return 1;
        }
    }
    if (!S_reserve_content_marks(parser, 1)) {
        return 0;
    }
    if (map->count == 0) {
        map->first = (bufsize_t)parser->line_marks.count;
    } else {
        assert((size_t)(map->first + map->count) == parser->line_marks.count);
        assert(parser->line_marks.values[parser->line_marks.count - 1].content_offset < mark.content_offset);
    }
    assert(mark.source_width >= 1);
    parser->line_marks.values[parser->line_marks.count++] = mark;
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
    S_append_input_marks(parser, node, parser->at.line_number, parser->at.line_start, column, length,
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
    if (!markdown_core_strbuf_owns(&node->content) &&
        (parser->at.partially_consumed_tab || ch->len > parser->at.offset)) {
        size_t initial = (size_t)(ch->len - parser->at.offset);
        if (parser->at.partially_consumed_tab) {
            initial += TAB_STOP - (parser->at.column % TAB_STOP) - 1;
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
    if (parser->at.partially_consumed_tab) {
        /* The spaces below stand for the tail of the tab at parser->at.offset and
         * have no source bytes of their own, so they are marked against the
         * tab itself and the copied bytes get a mark of their own. */
        S_record_content_mark(parser, node, parser->at.offset + 1, 1);
        parser->at.offset += 1; // skip over tab
        // add space characters:
        chars_to_tab = TAB_STOP - (parser->at.column % TAB_STOP);
        for (i = 0; i < chars_to_tab; i++) {
            markdown_core_strbuf_putc(&node->content, ' ');
        }
    }
    S_record_content_mark(parser, node, parser->at.offset + 1, ch->len - parser->at.offset);
    markdown_core_strbuf_put_ahead(&node->content, ch->data + parser->at.offset, ch->len - parser->at.offset);
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
        markdown_core_line_mark mark = parser->line_marks.values[i];
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
    if (line < parser->input.first_line || line > parser->at.line_number) {
        return false;
    }
    uint32_t start = node->place.start;
    if (line == parser->at.line_number) {
        return start >= (uint32_t)S_input_source(parser, parser->at.line_start);
    }
    return start >= (uint32_t)S_line_offset(parser, line) && start < (uint32_t)S_line_offset(parser, line + 1);
}

bufsize_t markdown_core_parser_line_offset(markdown_core_parser *parser, int line) {
    return S_line_offset(parser, line);
}

void markdown_core_parser_open(markdown_core_parser *parser, markdown_core_node_kind opens, bufsize_t from,
                               bufsize_t to, const markdown_core_field *fields, size_t count) {
    markdown_core_context_sink *sink = parser->sink;
    if (!sink || markdown_core_parser_block_root(parser) != parser->root ||
        (size_t)parser->at.line_end < sink->length) {
        return;
    }
    const bufsize_t line = parser->at.line_start;
    const int32_t opener = markdown_core_context_opener_add(
        sink, opens, (markdown_core_place){(uint32_t)(line + from), (uint32_t)(line + to)}, true);
    for (size_t i = 0; i < count && opener >= 0; i++) {
        const markdown_core_field *field = &fields[i];
        markdown_core_context_field open = {.kind = field->slot,
                                            .owner = field->owner,
                                            .opener = opener,
                                            .start = (uint32_t)(line + field->start),
                                            .end = field->end,
                                            .declares = field->declares,
                                            .lead = (uint32_t)field->lead};
        for (size_t j = 0; j < field->prefix_count && j < 2; j++) {
            open.prefix[j] =
                (markdown_core_place){(uint32_t)(line + field->prefix[j][0]), (uint32_t)(line + field->prefix[j][1])};
            open.prefix_count++;
        }
        markdown_core_context_field_add(sink, &open);
    }
}

const unsigned char *markdown_core_parser_line_content(markdown_core_parser *parser, int line, bufsize_t *length) {
    return S_input_line_content(parser, markdown_core_parser_visited_line(parser, line), length);
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
    const int visited = parser->input.first_line + (int)parser->input.lines.count - 1;
    const uint32_t start = node->place.start, end = node->place.end;
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

void markdown_core_parser_claim(markdown_core_parser *parser, int line, const unsigned char *cursor, bufsize_t end) {
    /* The lookahead's last scan of each line recorded where it matched the
     * line's containers to in the line's facts. */
    for (int claimed = parser->at.line_number + 1; claimed <= line; claimed++) {
        markdown_core_input_line *visited = markdown_core_parser_rewritten_line(parser, claimed);
        assert(visited->facts);
        visited->own = (uint32_t)parser->input.facts[visited->facts - 1].offset;
    }
    parser->claimed_cursor = cursor;
    parser->claimed_line = line;
    parser->claimed_last_end = end;
}

/* One copied run of a document line. */
static inline int S_append_copied_mark(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t source, bufsize_t offset) {
    return S_append_content_mark(parser, &node->content_map,
                                 (markdown_core_line_mark){offset, line, source, 1, 1, parser->at.indent});
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
        if (!S_append_content_mark(parser, &node->content_map,
                                   (markdown_core_line_mark){offset + index - first, line, source, 1, inside ? 0 : 1,
                                                             parser->at.indent})) {
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
    mark = &parser->line_marks.values[markdown_core_block_content_mark_at(parser, map, content_offset)];
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

/* Where a read of the settled paragraph's content stands before its last
 * mark, `mark` of its map, at content offset `from`, with `places` read
 * before it, ending at `at`: where the next read goes on (THE SETTLE
 * POINT). False, with the parse failed, when storage could not be had. */
static bool S_keep_read(markdown_core_parser *parser, markdown_core_settled *settled,
                        const markdown_core_places *places, uint32_t mark, bufsize_t from, uint32_t at) {
    const size_t bytes = sizeof(*places) + places->count * sizeof(*places->items);
    markdown_core_places *kept = markdown_core_realloc(settled->places, bytes);
    if (!kept) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    memcpy(kept, places, bytes);
    settled->places = kept;
    settled->mark = mark;
    settled->from = from;
    settled->at = at;
    return true;
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
 * so the parse places its nodes at offsets of the content; a root is read
 * once, and a root parsed again (A LOOKUP THAT FOUND NOTHING) is placed in
 * the content it was read as. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_read_content(markdown_core_parser *parser,
                                                                           markdown_core_node *node, bufsize_t length,
                                                                           markdown_core_settled *settled) {
    const markdown_core_content_map map = node->content_map;
    if (map.first == MARKDOWN_CORE_IDENTITY_MARK) {
        return;
    }
    node->content_map = (markdown_core_content_map){MARKDOWN_CORE_IDENTITY_MARK, 1, 0};
    if (length <= 0 || map.count <= 0) {
        return;
    }
    const bufsize_t end = map.offset + length;
    const markdown_core_line_mark *mark =
        &parser->line_marks.values[markdown_core_block_content_mark_at(parser, &map, map.offset)];
    const markdown_core_line_mark *const past = &parser->line_marks.values[map.first + map.count];
    /* The node's own lines, when it has places of them; one line without. */
    markdown_core_places *const lines = node->source.places;
    const uint32_t line_count = lines ? lines->count : 0;
    /* A read the settled paragraph kept goes on where it stopped: at the
     * mark the last read ended in, which the content's text keeps as it
     * grows (THE SETTLE POINT). */
    const markdown_core_places *kept = settled && !lines ? settled->places : NULL;
    if (kept && (settled->mark >= (uint32_t)map.count ||
                 parser->line_marks.values[map.first + settled->mark].content_offset != settled->from)) {
        kept = NULL;
    }
    const markdown_core_line_mark *const resume = kept ? &parser->line_marks.values[map.first + settled->mark] : mark;
    /* At most one content place per mark from the first to the end of the
     * map, one place of length 0 before each and one after each line. */
    markdown_core_places *places = markdown_core_node_pool_bytes(
        parser->pool, sizeof(*places) + ((size_t)(past - resume) * 2 + line_count + 1 + (kept ? kept->count : 0)) *
                                            sizeof(markdown_core_source_place));
    if (!places) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    places->count = 0;
    places->content = (uint32_t)length;
    uint32_t line = 0, at = 0;
    bufsize_t from = map.offset;
    if (kept) {
        memcpy(places->items, kept->items, kept->count * sizeof(*places->items));
        places->count = kept->count;
        mark = resume;
        from = settled->from;
        at = settled->at;
    }
    for (; from < end; mark++) {
        const bufsize_t to = mark + 1 < past && mark[1].content_offset < end ? mark[1].content_offset : end;
        if (settled && !lines && to == end &&
            !S_keep_read(parser, settled, places, (uint32_t)(mark - parser->line_marks.values - map.first), from, at)) {
            markdown_core_node_pool_bytes_free(parser->pool, places);
            return;
        }
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

void markdown_core_parser_read_content(markdown_core_parser *parser, markdown_core_node *node, bufsize_t length) {
    S_read_content(parser, node, length, NULL);
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

/* ENDS_BLANK when a closed block whose flags are `flags` ends with a blank
 * line, and none otherwise: its own last line was blank, or, for a block of
 * a kind whose answer is its children's, the last child the facts see that
 * settled into it ends with one (S_fold). */
static inline markdown_core_node_internal_flags S_closed_ends_blank(markdown_core_node_internal_flags flags) {
    return (flags & MARKDOWN_CORE_NODE__BLANK_FOLDED)      ? flags & MARKDOWN_CORE_NODE__ENDS_BLANK
           : (flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) ? MARKDOWN_CORE_NODE__ENDS_BLANK
                                                           : 0;
}

/* The same of any block: an open block answers with its own last line. */
static inline bool S_ends_with_blank_line(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__OPEN) ? (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0
                                                    : S_closed_ends_blank(node->flags) != 0;
}

bool markdown_core_block_ends_with_blank_line(const markdown_core_node *node) { return S_ends_with_blank_line(node); }

/* The flags `flags` of `parent`, a block of kind `kind` that folds its
 * children, with what they take from a child whose flags are `child` as it
 * settles (A BLOCK SETTLES): a child the blank-line facts see gives a block
 * whose answer is its children's that answer, and spaces it when the child
 * before it ended with a blank line; then the kind's fold hook takes it. It
 * writes nothing of the parent but what the hook publishes. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) markdown_core_node_internal_flags
    S_fold(const markdown_core_kind_record *kind, markdown_core_node *parent, markdown_core_node_internal_flags flags,
           markdown_core_node_internal_flags child) {
    if ((kind->flags & MARKDOWN_CORE_KIND_BLANK_PROPAGATES) && !(child & MARKDOWN_CORE_NODE__BLANK_TRANSPARENT)) {
        flags = (markdown_core_node_internal_flags)((flags & ~MARKDOWN_CORE_NODE__ENDS_BLANK) |
                                                    MARKDOWN_CORE_NODE__BLANK_FOLDED |
                                                    ((flags & MARKDOWN_CORE_NODE__ENDS_BLANK)
                                                         ? MARKDOWN_CORE_NODE__BLANK_SPACED
                                                         : 0) |
                                                    S_closed_ends_blank(child));
    }
    if (kind->flags & MARKDOWN_CORE_KIND_FOLDS) {
        flags = kind->structure->element->fold_child(kind->structure, parent, flags, child);
    }
    return flags;
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
 * `parent`, the block that holds it, once its own children have: it is one
 * of the parent's children already, and joins the completion queue with its
 * parent. It takes the parser's `settle_flags` -- COMMITTED in a commit,
 * which a snapshot reads and never writes. A parent of a kind that folds its
 * children takes what it reads of the child at once (S_fold).
 * Nothing writes it after this but completion, which makes its inline content
 * and asks the finish steps of its kind (markdown-core-element-api.h, THE
 * FINISH STEP). The document settles into nothing as the input ends: it joins
 * the queue last, and nothing holds it. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_settle(markdown_core_parser *parser,
                                                                     markdown_core_node *node,
                                                                     markdown_core_node *parent,
                                                                     const markdown_core_kind_record *kind) {
    node->flags |= parser->settle_flags;
    if (kind->flags & (MARKDOWN_CORE_KIND_BLANK_PROPAGATES | MARKDOWN_CORE_KIND_FOLDS)) {
        parent->flags = S_fold(kind, parent, parent->flags, node->flags);
    }
    S_complete_later(parser, node, parent);
}

/* A block made whole joins the end of its parent's children as it settles:
 * the block's own children settled as it was made, and no block of the
 * parent is open or pending. */
void markdown_core_block_settle(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node *parent) {
    assert(parent != markdown_core_parser_current(parser) || !parser->at.pending);
    if (!markdown_core_node_push_child(parser->pool, parent, node)) {
        S_settle_failed(parser, node);
        return;
    }
    S_settle(parser, node, parent, markdown_core_parser_block_kind(parser, parent));
}

/* The same before the last of its parent's children, the block it was read
 * from as that closes: a reference definition a paragraph's close reads. */
void markdown_core_block_settle_before(markdown_core_parser *parser, markdown_core_node *node,
                                       markdown_core_node *parent) {
    assert(parent->child_count);
    if (!markdown_core_node_array_insert_before_last(parser->pool, &parent->children, &parent->child_count, node)) {
        S_settle_failed(parser, node);
        return;
    }
    S_settle(parser, node, parent, markdown_core_parser_block_kind(parser, parent));
}

/* A field root settles as its block installs it: the line that wrote it is
 * read, so nothing writes it after this but its completion, which joins the
 * queue on its own, and a snapshot's view of the block holds it as it is
 * (markdown_core_node_view). */
void markdown_core_block_settle_field(markdown_core_parser *parser, markdown_core_node *field,
                                      markdown_core_node *holder) {
    field->flags |= parser->settle_flags;
    S_complete_later(parser, field, holder);
}

/* The pending block settles into `holder`, the current block of kind
 * `kind`, as it takes its next child or closes. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_settle_pending_as(markdown_core_parser *parser,
                                                                                markdown_core_node *holder,
                                                                                const markdown_core_kind_record *kind) {
    markdown_core_node *pending = parser->at.pending;
    if (pending) {
        parser->at.pending = NULL;
        S_settle(parser, pending, holder, kind);
    }
}

/* The same, the holder's kind read only when a block is pending. */
static inline void S_settle_pending(markdown_core_parser *parser, markdown_core_node *holder) {
    if (parser->at.pending) {
        S_settle_pending_as(parser, holder, markdown_core_parser_block_kind(parser, holder));
    }
}

/* `block` closes: it is open no longer, and the runs a publish gave it while
 * it was open go (node.h, PUBLISHED). */
static inline void S_shut(markdown_core_parser *parser, markdown_core_node *block) {
    if (block->flags & MARKDOWN_CORE_NODE__PUBLISHED) {
        markdown_core_node_pool_bytes_free(parser->pool, block->source.runs);
        block->source.runs = NULL;
    }
    block->flags &= (markdown_core_node_internal_flags) ~(MARKDOWN_CORE_NODE__OPEN | MARKDOWN_CORE_NODE__PUBLISHED);
}

/* A BLOCK CLOSES, its range settled, inside `parent`: its element's close
 * hook makes the node it settles as, and a block that takes text lines
 * in a container takes the runs of its own lines. False when the block
 * yields no node of its own: a paragraph that held only reference
 * definitions yielded them as References settled into `parent` before it,
 * and the caller lets it go. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_close(markdown_core_parser *parser, markdown_core_node *b,
                                                                    const markdown_core_kind_record *kind,
                                                                    markdown_core_node *parent) {
    /* A block that takes text lines in a container has a run of its own
     * source on each of its lines, whatever its close hook makes of it; one
     * of the document itself lies on whole lines, its own source its range. */
    const bool lines = parent && parent != parser->root && S_kind_takes_text(kind, b);
    const markdown_core_element_instance *structure = kind->structure;
    if (structure && structure->element->close_block &&
        !structure->element->close_block(structure, parser, b, parent)) {
        return false;
    }
    if (lines) {
        int line = parser->at.line_number;
        markdown_core_parser_line_places(parser, b, parent, &line);
    }
    return true;
}

static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_lend_below(markdown_core_parser *parser, markdown_core_node *block);
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_candidate_drops(markdown_core_parser *parser,
                                                                  const markdown_core_node *block);

/* The current block, its range settled, closes: its pending child settles
 * into it, it closes, and it leaves the stack to be the pending child of the
 * block below, or, when it yields no node of its own, leaves that block's
 * children. A block a snapshot lent waits for the take-back (THE LENDING),
 * one a candidate lent for its decision (THE CANDIDATES), and the block the
 * lending reaches next is lent as it closes. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_close_current(markdown_core_parser *parser) {
    markdown_core_node *closing = parser->at.current;
    markdown_core_node *parent = markdown_core_scaffold_parent(closing);
    assert(closing->flags & MARKDOWN_CORE_NODE__OPEN);
    if (closing == parser->stream->boundary) {
        S_lend_below(parser, closing);
    }
    const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, closing);
    S_settle_pending_as(parser, closing, kind);
    S_shut(parser, closing);
    const bool kept = S_close(parser, closing, kind, parent);
    parser->at.current = parent;
    parser->at.pending = kept ? closing : NULL;
    if (!kept) {
        assert(parent->children[parent->child_count - 1] == closing);
        markdown_core_node_array_pop(parent->children, &parent->child_count);
        if (!(closing->flags & MARKDOWN_CORE_NODE__LENT)) {
            markdown_core_parser_release_node(parser, closing);
        } else {
            S_candidate_drops(parser, closing);
        }
    }
}

void markdown_core_block_close_current(markdown_core_parser *parser) {
    assert(parser->at.current != parser->at.block_root);
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

        if (!parser->reading) {
            // end of input - line number has not been incremented
            closing->place.end = (uint32_t)parser->at.last_line_end;
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
                   S_starts_on_line(parser, closing, parser->at.line_number) ||
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
            closing->place.end = (uint32_t)parser->at.last_line_end;
        }

        S_close_current(parser);
    } while (closing != b);
    return markdown_core_parser_current(parser);
}

void markdown_core_parser_close_below(markdown_core_parser *parser, markdown_core_node *container) {
    /* The block open inside the container is the one above it, its last
     * child. */
    if (parser->at.current != container) {
        markdown_core_block_close(parser, container->children[container->child_count - 1]);
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

/* `child`, open, as the last child of `parent`, whose open children have
 * closed and whose pending child has settled: the builder above it, as
 * block_parent_for established that the parent holds it. NULL, with the
 * parse failed and the child let go, when the children could not grow. */
static inline markdown_core_node *S_open_made(markdown_core_parser *parser, markdown_core_node *parent,
                                              markdown_core_node *child) {
    markdown_core_scaffold_of(child)->parent = parent;
    if (!markdown_core_node_push_child(parser->pool, parent, child)) {
        S_settle_failed(parser, child);
        return NULL;
    }
    parser->at.current = child;
    return child;
}

/* A selected parent is a semantic decision, not a hint to repeat the search. */
markdown_core_node *markdown_core_parser_add_child_validated(markdown_core_parser *parser, markdown_core_node *parent,
                                                             markdown_core_node_type block_type, int start_column) {
    assert(parent);
    /* The container takes its next child: what is open inside it closes
     * first, and its pending child settles (THE BUILDERS, parser.h). */
    markdown_core_parser_close_below(parser, parent);
    S_settle_pending(parser, parent);
    markdown_core_node *child = make_block(
        parser, block_type, markdown_core_parser_source_offset(parser, parser->at.line_number, start_column));
    if (!child || child->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        if (child) {
            markdown_core_parser_release_node(parser, child);
        }
        return NULL;
    }
    return S_open_made(parser, parent, child);
}

markdown_core_node *markdown_core_parser_open_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                    markdown_core_node *child) {
    markdown_core_parser_close_below(parser, parent);
    S_settle_pending(parser, parent);
    child->flags |= MARKDOWN_CORE_NODE__OPEN;
    return S_open_made(parser, parent, child);
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
 * held in the frame, rooted at the field root of an inline node, or, at the
 * bottom, at the queued root the document's walk is at.
 *
 * The bottom frame's root is the walk's to parse: a block's inline content,
 * or a field root's (a definition's term, a callout's title, a table's
 * caption, a block directive's label), each a queued root of its own. A
 * field of an inline node is not -- the inline parser completed it when it
 * made the token (process_inline_tree above) -- and nor is a definition the
 * document owns as a root of its own, whose content was made as inline
 * content. `parsed` is the container whose inline tree the walk is inside,
 * or NULL: nothing below a parsed container is parsed again, since an inline
 * node's content is its container's, already parsed. */
typedef struct {
    inline_walk walk;
    markdown_core_node *parsed;
    int script_depth;
    /* One word per projected step, zero when the root's walk starts. */
    void *states[];
} owned_tree_frame;

/* The frames sit in the parser's walk stack, `stride` bytes apart: a frame
 * and its `slots` state words. */
typedef struct {
    markdown_core_parser *parser;
    unsigned char *frames;
    size_t slots, stride;
    size_t count, capacity;
    /* What a root pushed now starts with: the pushing node's word depth. */
    int script_depth;
} owned_tree_walk;

static inline owned_tree_frame *S_frame(const owned_tree_walk *walk, size_t index) {
    return (owned_tree_frame *)(walk->frames + index * walk->stride);
}

/* The walk starts with the room the parser keeps for walks: as many frames
 * as it holds. */
static void S_frames_kept(owned_tree_walk *walk) {
    walk->frames = walk->parser->walk_stack;
    walk->capacity = walk->parser->walk_stack_size / walk->stride;
}

/* Room for one frame more than the walk holds. */
static bool S_reserve_frame(owned_tree_walk *walk) {
    if (walk->count < walk->capacity) {
        return true;
    }
    void *frames = markdown_core_parser_walk_stack(walk->parser, walk->capacity ? 2 * walk->capacity : 8, walk->stride);
    if (!frames) {
        markdown_core_parser_fail(walk->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    S_frames_kept(walk);
    return true;
}

/* A root's state words start zero. */
static inline void S_clear_states(owned_tree_walk *walk, size_t frame) {
    void **states = S_frame(walk, frame)->states;
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
    owned_tree_frame *frame = S_frame(walk, walk->count++);
    S_walk_init(&frame->walk, root);
    frame->parsed = NULL;
    frame->script_depth = block ? 0 : walk->script_depth;
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
                cur->place.end = tmp->place.end;
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

static MARKDOWN_CORE_ATTRIBUTE((noinline)) markdown_core_node **S_settle_after(markdown_core_parser *parser,
                                                                               markdown_core_node *node,
                                                                               uint32_t count);

/* A CONTAINER'S CHILDREN SETTLE AT ITS EXIT: its scaffold children, every one
 * of them complete, become its children array, in order, in storage of the
 * pool's whose capacity their count determines (node.h), and their scaffold
 * links are cleared, so a node outside an inline tree under construction has
 * none. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_settle_scaffold_after(markdown_core_parser *parser,
                                                                                    markdown_core_node *node,
                                                                                    uint32_t held) {
    markdown_core_scaffold *holder = markdown_core_scaffold_of(node);
    uint32_t count = 0;
    for (markdown_core_node *child = holder->first; child; child = markdown_core_scaffold_next(child)) {
        count++;
    }
    markdown_core_node **items =
        held ? S_settle_after(parser, node, count)
             : markdown_core_node_pool_bytes(parser->pool,
                                             (size_t)markdown_core_node_array_capacity(count) * sizeof(*items));
    if (!items) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    uint32_t at = held;
    count += held;
    for (markdown_core_node *child = holder->first, *next; child; child = next) {
        markdown_core_scaffold *link = markdown_core_scaffold_of(child);
        next = link->next;
        link->parent = link->next = NULL;
        link->prev = NULL;
        items[at++] = child;
    }
    holder->first = holder->last = NULL;
    node->children = items;
    node->child_count = count;
    return true;
}

static bool S_settle_scaffold(markdown_core_parser *parser, markdown_core_node *node) {
    assert(!node->child_count);
    return S_settle_scaffold_after(parser, node, 0);
}

/* The same of a root that holds its settled nodes, which takes its scaffold
 * after them (THE SETTLE POINT). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool S_settle_scaffold_held(markdown_core_parser *parser,
                                                                       markdown_core_node *node) {
    return S_settle_scaffold_after(parser, node, node->child_count);
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

/* The explicit anchor `node` holds reserved as starting at `start`: its own,
 * not one the headings computed for it. A node reserves it once it is
 * complete, and a block the snapshot ends without closing (S_close_tail) as
 * it ends. False when storage could not be had. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_reserve_anchor(markdown_core_parser *parser,
                                                                             const markdown_core_node *node,
                                                                             uint32_t start) {
    const markdown_core_chunk *anchor = &node->attributes.anchor;
    return !anchor->len || (node->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED) ||
           markdown_core_registry_declare(&parser->registry, MARKDOWN_CORE_REGISTRY_ANCHOR, anchor->data, anchor->len,
                                          start);
}

/* THE SETTLE POINT (docs/plans/2026-09-29-incremental-parsing.md, 5.8). A
 * snapshot reads the open paragraph's content again on every append, and
 * what it makes of the content before the paragraph's settle point is what
 * it made before: the top-level nodes there were made by tokens no reading
 * of which reached the end of the content, and nothing read before the
 * point holds anything open past it (parser.h, markdown_core_inline_settle).
 * The paragraph's content grows only at its end while it stays the same
 * paragraph -- the same block with the same id, its text read from the same
 * offset, and no shorter than the read before, which covers every byte a
 * token before the point read -- so the stream keeps those nodes (parser.h,
 * markdown_core_settled): the paragraph's next completion in a snapshot
 * takes them as its first children and parses from the point, and so does
 * the commit's that completes it, after which the tree owns them. A node
 * that settles is COMMITTED, so the publish makes it FINAL (ast.c, THE
 * FINAL PREFIX), never walks it again and leaves it its id (plan 5.9).
 *
 * A read's point is the latest start it listed that is no later than its
 * earliest undecided start and at which a top-level node starts whose node
 * before it is no Text: a node starting there began with a token, and
 * neither consolidation nor a scanner that takes back the bytes of the Text
 * before it (an address's scheme) reaches across a node that is no Text.
 *
 * A node that settles owns the bytes its record views: the content buffer
 * it was read from moves as the paragraph grows. What a completion does
 * besides making the nodes is the stream's to do again for those it keeps:
 * the explicit anchors below them are reserved at every completion, as a
 * snapshot's registry goes with the snapshot, and the lookups they made
 * that found nothing are the paragraph's when the commit completes it (A
 * LOOKUP THAT FOUND NOTHING). The finalize's completion, which sees every
 * definition, asks them again, and reads the content whole when one finds a
 * definition. */

static bool S_record_miss(markdown_core_parser *parser, markdown_core_node *root, markdown_core_registry_kind kind,
                          const unsigned char *label, bufsize_t size);

/* Whether `node`'s completion settles its content: a snapshot's, of a
 * paragraph it lent, read on its own lines, while no candidate waits. */
static inline bool S_settles(const markdown_core_parser *parser, const markdown_core_node *node) {
    return parser->stream->snapshot_mark &&
           (markdown_core_parser_kind(parser, node)->flags & MARKDOWN_CORE_KIND_IS_PARAGRAPH) &&
           (node->flags & MARKDOWN_CORE_NODE__LENT) && !node->source.places && !markdown_core_parser_undecided(parser);
}

/* The settled record emptied, the nodes it held its owner's. */
static void S_settled_clear(markdown_core_settled *settled) {
    settled->address = settled->root = NULL;
    settled->taken = false;
    markdown_core_array_truncate(&settled->nodes, 0);
    settled->point = 0;
    settled->miss_count = settled->miss_total = 0;
    markdown_core_strbuf_truncate(&settled->labels, 0);
    settled->anchored_count = 0;
    markdown_core_free(settled->places);
    settled->places = NULL;
}

/* The settled nodes let go, unless the tree holds them, and the record
 * emptied. */
static void S_settled_release(markdown_core_parser *parser, markdown_core_settled *settled) {
    for (size_t i = 0; !settled->taken && i < settled->nodes.count; i++) {
        markdown_core_parser_release_node(parser, settled->nodes.values[i]);
    }
    S_settled_clear(settled);
}

/* The settled nodes of the paragraph `node` leave it, and the record is
 * emptied: the paragraph goes on, but its content no longer goes on from
 * them, so what it holds now is read whole, and continues them as the last
 * publish placed them (markdown_core_publish_keep_left). */
static void S_settled_leave(markdown_core_parser *parser, markdown_core_settled *settled,
                            const markdown_core_node *node) {
    if (!markdown_core_publish_keep_left(parser, node, settled->nodes.values, (uint32_t)settled->nodes.count)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    S_settled_clear(settled);
}

/* Whether the settled nodes are of `node`, whose content is `size` bytes. */
static inline bool S_settled_holds(const markdown_core_settled *settled, const markdown_core_node *node,
                                   bufsize_t size) {
    return settled->address == node && !settled->taken && settled->id == node->id && settled->kind == node->kind &&
           settled->offset == node->content_map.offset && size >= settled->size;
}

/* The explicit anchors below the settled nodes reserved as starting at
 * `start`. False when storage could not be had. */
static bool S_settled_anchors(markdown_core_parser *parser, const markdown_core_settled *settled, uint32_t start) {
    for (size_t i = 0; i < settled->anchored_count; i++) {
        if (!S_reserve_anchor(parser, settled->anchored[i], start)) {
            return false;
        }
    }
    return true;
}

/* The node in `*slot` pushed on the stack `context` holds, linked through
 * the nodes' scaffolds, which a complete node no longer uses. */
static int S_settled_push(markdown_core_node **slot, void *context) {
    markdown_core_node **stack = context;
    if (*slot) {
        markdown_core_scaffold_of(*slot)->next = *stack;
        *stack = *slot;
    }
    return 1;
}

/* The node `node` settles, and with it every node below it, the field roots
 * of its inline nodes included: each owns the bytes its record views, and
 * one that holds an explicit anchor is listed with the settled ones, after
 * the first `*anchored`. False when storage could not be had. */
static bool S_settled_own(markdown_core_settled *settled, markdown_core_node *node, size_t *anchored) {
    markdown_core_node *stack = node;
    bool owned = true;
    while (stack) {
        markdown_core_node *below = stack;
        stack = markdown_core_scaffold_of(below)->next;
        markdown_core_scaffold_of(below)->next = NULL;
        if (!owned) {
            continue;
        }
        owned = markdown_core_node_own_viewed(below);
        if (owned && below->attributes.anchor.len && !(below->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED)) {
            markdown_core_node **nodes =
                markdown_core_reserve(settled->anchored, &settled->anchored_capacity, *anchored + 1, sizeof(*nodes));
            owned = nodes != NULL;
            if (owned) {
                settled->anchored = nodes;
                nodes[(*anchored)++] = below;
            }
        }
        if (owned) {
            markdown_core_visit_inline_subtrees(below, S_settled_push, &stack);
            for (uint32_t i = 0; i < below->child_count; i++) {
                S_settled_push(&below->children[i], &stack);
            }
        }
    }
    return owned;
}

/* The end of the labels of the settled lookups that found nothing. */
static inline size_t S_settled_labels(const markdown_core_settled *settled) {
    return settled->miss_count ? (size_t)(settled->misses[settled->miss_count - 1].label_start +
                                          settled->misses[settled->miss_count - 1].label_size)
                               : 0;
}

/* A lookup the snapshot's reading of the settling paragraph found nothing
 * for, listed after the settled ones. False when storage could not be
 * had. */
static bool S_settled_miss(markdown_core_settled *settled, markdown_core_registry_kind kind,
                           const markdown_core_chunk *label) {
    markdown_core_settled_miss *misses =
        markdown_core_reserve(settled->misses, &settled->miss_capacity, settled->miss_total + 1, sizeof(*misses));
    if (!misses) {
        return false;
    }
    settled->misses = misses;
    misses[settled->miss_total++] = (markdown_core_settled_miss){kind, settled->labels.size, label->len};
    markdown_core_strbuf_put(&settled->labels, label->data, label->len);
    settled->settle.misses++;
    return !settled->labels.oom;
}

/* The bytes of `node`'s content its inline parse reads. */
static inline bufsize_t S_content_bytes(const markdown_core_node *node) {
    markdown_core_chunk content = markdown_core_node_text(node);
    markdown_core_chunk_rtrim(&content);
    return content.len;
}

/* Whether a lookup the settled nodes made that found nothing finds a
 * definition at the finalize's completion, which sees every definition: the
 * content then no longer goes on from them. A commit's completion keeps the
 * lookups for the finalize instead (S_settled_take). */
static bool S_settled_found(markdown_core_parser *parser, const markdown_core_settled *settled) {
    for (size_t i = 0; !parser->snapshot && i < settled->miss_count; i++) {
        const markdown_core_settled_miss *miss = &settled->misses[i];
        const markdown_core_chunk label = {(unsigned char *)settled->labels.ptr + miss->label_start, miss->label_size,
                                           0};
        bool failed = false;
        const bool found = markdown_core_parser_find_definition(parser, miss->kind, &label, &failed);
        if (failed) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        if (found || failed) {
            return true;
        }
    }
    return false;
}

/* The commit completes the settled paragraph `node`, whose content is
 * `size` bytes: the settled nodes are its first children, the tree's from
 * now on, the lookups they made that found nothing its own, their anchors
 * reserved, and its content parsed from the point. While a candidate waits,
 * an accept may put the paragraph back as the candidate's mark found it,
 * open, and the nodes come back to the stream (S_candidate_take_back): the
 * stream keeps them as taken until no candidate waits. */
static void S_settled_take(markdown_core_parser *parser, markdown_core_node *node, bufsize_t size) {
    markdown_core_settled *settled = &parser->stream->settled;
    markdown_core_node **items = markdown_core_node_pool_bytes(
        parser->pool, (size_t)markdown_core_node_array_capacity((uint32_t)settled->nodes.count) * sizeof(*items));
    if (!items) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    memcpy(items, settled->nodes.values, settled->nodes.count * sizeof(*items));
    node->children = items;
    node->child_count = (uint32_t)settled->nodes.count;
    const bool undecided = markdown_core_parser_undecided(parser);
    /* A lookup's record keeps where the root lies once its content is read.
     * Only a snapshot's completion records one (markdown_core_parser_find_definition). */
    markdown_core_parser_read_content(parser, node, size);
    for (size_t i = 0; parser->snapshot && !parser->error && i < settled->miss_count; i++) {
        const markdown_core_settled_miss *miss = &settled->misses[i];
        if (!S_record_miss(parser, node, miss->kind, (const unsigned char *)settled->labels.ptr + miss->label_start,
                           miss->label_size)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    if (!parser->error && !S_settled_anchors(parser, settled, node->place.start)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (!parser->error) {
        settled->settle.from = settled->point;
        markdown_core_parse_inlines_settle(parser, node, &settled->settle);
    }
    if (undecided && !parser->error) {
        settled->taken = true;
    } else {
        S_settled_clear(settled);
    }
}

/* A snapshot completes the paragraph `node`, whose content is `size` bytes:
 * it takes the settled nodes as its first children when they are its, and
 * parses its content from the point, listing where the next point may be
 * when the read `settles`. A read that does not settle leaves the held nodes
 * as they are: they are the published document's, which made them FINAL. */
static void S_settled_read(markdown_core_parser *parser, markdown_core_node *node, bufsize_t size, bool settles) {
    markdown_core_settled *settled = &parser->stream->settled;
    if (!settled->address) {
        settled->address = node;
        settled->id = node->id;
        settled->kind = node->kind;
    }
    settled->root = node;
    settled->offset = node->content_map.offset;
    if (settles) {
        settled->size = size;
    }
    settled->miss_total = settled->miss_count;
    markdown_core_strbuf_truncate(&settled->labels, (bufsize_t)S_settled_labels(settled));
    if (settled->nodes.count) {
        node->children = settled->nodes.values;
        node->child_count = (uint32_t)settled->nodes.count;
    }
    if (!S_settled_anchors(parser, settled, node->place.start)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    S_read_content(parser, node, size, settled);
    if (!parser->error) {
        settled->settle.from = settled->point;
        markdown_core_parse_inlines_settle(parser, node, &settled->settle);
        if (!settles) {
            settled->settle.count = 0;
        }
    }
}

/* The inline content of the paragraph `node` read with the settled nodes,
 * or read to settle (THE SETTLE POINT). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_parse_settled(markdown_core_parser *parser, markdown_core_node *node,
                                                                bool settles) {
    markdown_core_settled *settled = &parser->stream->settled;
    const bufsize_t size = S_content_bytes(node);
    if (settled->address == node && !settled->taken && settled->id == node->id && settled->kind == node->kind &&
        !S_settled_holds(settled, node, size)) {
        S_settled_leave(parser, settled, node);
    } else if (settled->address && !S_settled_holds(settled, node, size)) {
        S_settled_release(parser, settled);
    }
    if (parser->error) {
        return;
    }
    /* The commit's completion takes the nodes; a snapshot's reads with them,
     * and settles more of the content only when it may (S_settles). */
    const bool commits = !parser->stream->snapshot_mark;
    if (settled->nodes.count && commits && (node->flags & MARKDOWN_CORE_NODE__COMMITTED)) {
        if (!S_settled_found(parser, settled)) {
            S_settled_take(parser, node, size);
        } else if (!parser->error) {
            S_settled_leave(parser, settled, node);
            markdown_core_parse_inlines(parser, node, true);
        }
    } else if (settles || (settled->nodes.count && !commits)) {
        S_settled_read(parser, node, size, settles);
    } else {
        S_settled_release(parser, settled);
        markdown_core_parse_inlines(parser, node, true);
    }
}

/* The inline content of the queued root `node` parsed. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_parse_root(markdown_core_parser *parser,
                                                                         markdown_core_node *node) {
    const bool settles = S_settles(parser, node);
    if (settles || parser->stream->settled.address == node) {
        S_parse_settled(parser, node, settles);
    } else {
        markdown_core_parse_inlines(parser, node, true);
    }
}

/* The index of the first child of `node` from `from` on that starts at
 * content offset `start`, or the child count when none does. */
static uint32_t S_child_at(const markdown_core_node *node, uint32_t from, bufsize_t start) {
    uint32_t low = from, top = node->child_count;
    while (low < top) {
        const uint32_t middle = low + (top - low) / 2;
        if ((bufsize_t)node->children[middle]->place.start < start) {
            low = middle + 1;
        } else {
            top = middle;
        }
    }
    return low < node->child_count && (bufsize_t)node->children[low]->place.start == start ? low : node->child_count;
}

/* The settling paragraph `node` is complete: its nodes before the point its
 * read leaves settle. False when storage could not be had. */
static bool S_settle_end(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_settled *settled = &parser->stream->settled;
    const markdown_core_inline_settle *settle = &settled->settle;
    const uint32_t held = (uint32_t)settled->nodes.count;
    for (size_t k = settle->count; k-- > 0;) {
        const markdown_core_settle_start *start = &settle->starts[k];
        if (start->start > settle->undecided) {
            continue;
        }
        const uint32_t at = S_child_at(node, held, start->start);
        if (at == node->child_count || (at && node->children[at - 1]->kind == MARKDOWN_CORE_NODE_TEXT)) {
            continue;
        }
        /* The point the read began at: nothing more settles. */
        if (at == held) {
            return true;
        }
        if (node->children != settled->nodes.values) {
            if (!markdown_core_array_append(&settled->nodes, node->children + held, (size_t)(at - held))) {
                return false;
            }
        } else {
            markdown_core_array_take(&settled->nodes, at);
        }
        size_t anchored = settled->anchored_count;
        for (uint32_t i = held; i < at; i++) {
            node->children[i]->flags |= MARKDOWN_CORE_NODE__COMMITTED;
            if (!S_settled_own(settled, node->children[i], &anchored)) {
                return false;
            }
        }
        settled->anchored_count = anchored;
        /* The notes the settled nodes hold commit with them. */
        for (size_t n = 0; n < settle->note_count; n++) {
            if ((bufsize_t)settle->notes[n]->place.start < start->start) {
                settle->notes[n]->flags |= MARKDOWN_CORE_NODE__COMMITTED;
            }
        }
        settled->point = start->start;
        settled->miss_count += start->misses;
        return true;
    }
    return true;
}

/* The scaffold of the root `node`, `count` nodes, after the nodes it holds
 * already: the settled ones a snapshot's read keeps in the stream's storage,
 * which grows to hold them all, or those a commit's took, in storage of the
 * pool's (THE SETTLE POINT). NULL when storage could not be had. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) markdown_core_node **S_settle_after(markdown_core_parser *parser,
                                                                               markdown_core_node *node,
                                                                               uint32_t count) {
    markdown_core_settled *settled = &parser->stream->settled;
    const uint32_t total = node->child_count + count;
    if (node->children == settled->nodes.values) {
        return markdown_core_array_reserve_past(&settled->nodes, node->child_count, total) ? settled->nodes.values
                                                                                           : NULL;
    }
    markdown_core_node **items =
        markdown_core_node_pool_bytes(parser->pool, (size_t)markdown_core_node_array_capacity(total) * sizeof(*items));
    if (items) {
        memcpy(items, node->children, node->child_count * sizeof(*items));
        markdown_core_node_pool_bytes_free(parser->pool, node->children);
    }
    return items;
}

/* What completion does at a node's ENTER, in the frame at `index`: it parses
 * the node's inline content when the node is the queued root (see
 * owned_tree_frame; the content of a kind the record marks PARSES, not
 * DEFERRED, that the structure says holds inlines) and points the frame's
 * walk into that content; it runs the steps projected for the ENTER,
 * completes the node (the element's `complete_node` the record carries),
 * reserves its explicit anchor and pushes the field roots of an inline node,
 * which are walked before anything below the node. Returns 1 when it pushed
 * a field root, 0 when it pushed none, and -1 with parser->error set. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) int S_enter_node(owned_tree_walk *walk, size_t index,
                                                                        markdown_core_node *node, size_t kind) {
    markdown_core_parser *const parser = walk->parser;
    owned_tree_frame *frame = S_frame(walk, index);
    const markdown_core_kind_record *facts = &parser->dialect->kinds[kind];
    const markdown_core_finish_step_entry *steps = parser->dialect->finish_dispatch[2 * kind];
    unsigned flags = facts->flags;
    /* The queued root is the only node the bottom frame enters outside the
     * content it parsed; every other node any frame enters is an inline
     * node. */
    const bool queued = !index && !frame->parsed;
    if (queued && S_kind_contains_inlines(facts, node)) {
        if (!(flags & MARKDOWN_CORE_KIND_DEFERRED)) {
            S_parse_root(parser, node);
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
    if (steps && run_finish_steps(parser, steps, node, MARKDOWN_CORE_EVENT_ENTER, frame->states) ==
                     MARKDOWN_CORE_FINISH_FAILED) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return -1;
    }
    if (facts->complete) {
        facts->complete(facts->structure, parser, node, frame->script_depth);
    }
    /* An explicit anchor is reserved once the node is complete, as starting
     * where the queued root it is in starts. A FormulaBlock a fence became
     * holds no attributes; a Text holds none either, so the siblings
     * consolidation absorbs reserve none. */
    if (!S_reserve_anchor(parser, node, parser->completing->place.start)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return -1;
    }
    /* The field roots of an inline node, which the inline parser completed
     * with it, of a kind that owns them through its record or of an element
     * that owns them through its hook: the answer is no for almost every
     * node, and it is one flag and one load. A block's field roots are
     * queued roots of their own (A BLOCK SETTLES). */
    if (queued || (!(flags & MARKDOWN_CORE_KIND_FIELDS) && !node->element)) {
        return 0;
    }
    walk->script_depth = frame->script_depth;
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
        owned_tree_frame *a = S_frame(walk, left), *b = S_frame(walk, right);
        const owned_tree_frame swap = *a;
        *a = *b;
        *b = swap;
    }
    return walk->count != first;
}

/* THE COMPLETION WALK, on one explicit continuation stack. The bottom frame
 * is the walk of the document's queue, and it takes the queue a root at a
 * time: every block of the tree, and every field root a block holds, joined
 * the queue as it settled (S_complete_later), so nothing below a block that
 * is a block, and no field root of a block, is that block's to walk. Each
 * time the bottom walk is done, it starts over at the next queued root: the
 * root's ENTER is the queue's to deliver, from word depth zero, since a block
 * begins content of its own, and the walk's next event is the root's EXIT
 * unless the ENTER points it into the root's inline content (S_enter_node).
 * Above the bottom frame are the fields the inline nodes own, roots of their
 * own walked as they are found -- a field root stays the node its owner put
 * there, no step may substitute one, so a frame carries the root itself and
 * never the slot holding it, and depth never uses C frames.
 *
 * Every event of every node is answered from the walk's own registers and the
 * parser's per-kind record: at a node's ENTER, S_enter_node; at a node's EXIT
 * its scaffold children settle into its children array (S_settle_scaffold),
 * at a Text's EXIT consolidation of the run when there is one to merge or a
 * Text with no bytes to drop, and then the steps projected for the EXIT of
 * its kind, in descriptor order, each behind its gate, asked with the node's
 * parent: its scaffold parent inside an inline tree, and for a queued root
 * the block it settled into or the block that holds it, which the queue
 * links it to. A field root's walk ends when it is done, the document's when
 * the queue is too. */
static int walk_owned_trees(markdown_core_parser *parser) {
    const size_t slots = parser->dialect->finish_step_slots;
    owned_tree_walk walk = {.parser = parser,
                            .slots = slots,
                            .stride = sizeof(owned_tree_frame) + slots * sizeof(void *),
                            .script_depth = 0};
    const markdown_core_finish_step_entry *const *const dispatch = parser->dialect->finish_dispatch;
    markdown_core_node *queued = parser->completion_first;

    S_frames_kept(&walk);
    if (S_reserve_frame(&walk)) {
        /* The document's walk, done before its first block. */
        S_clear_states(&walk, 0);
        owned_tree_frame *bottom = S_frame(&walk, 0);
        bottom->walk = (inline_walk){.next_event = MARKDOWN_CORE_EVENT_DONE};
        bottom->parsed = NULL;
        bottom->script_depth = 0;
        walk.count = 1;
    }
    while (walk.count && !parser->error) {
        const size_t top = walk.count - 1;
        owned_tree_frame *frame = S_frame(&walk, top);
        void **states = frame->states;
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
                /* The next queued root leaves the queue. */
                node = queued;
                queued = markdown_core_scaffold_next(node);
                markdown_core_scaffold_of(node)->next = NULL;
                tree->root = node;
                tree->node = node;
                tree->next_event = MARKDOWN_CORE_EVENT_EXIT;
                tree->next = node;
                frame->script_depth = 0;
                parser->completing = node;
            } else {
                if (node->element && node->element->delimiter.body == DELIMITER_WORD_BODY) {
                    frame->script_depth += event == MARKDOWN_CORE_EVENT_ENTER ? 1 : -1;
                }
                if (event == MARKDOWN_CORE_EVENT_EXIT) {
                    const bool parsed = node == frame->parsed;
                    if (parsed) {
                        frame->parsed = NULL;
                    }
                    if (markdown_core_scaffold_first(node) &&
                        !(parsed && node->child_count ? S_settle_scaffold_held(parser, node)
                                                      : S_settle_scaffold(parser, node))) {
                        break;
                    }
                    if (parsed && node == parser->stream->settled.root && !S_settle_end(parser, node)) {
                        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
                    /* The walk of an inline node's field root is done with
                     * the link to the node that holds it. A queued root keeps
                     * the link to the block it settled into, or to the block
                     * that holds it. */
                    if (node == tree->root && top) {
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
    return !parser->error;
}

/* THE END OF THE BLOCK PHASE: every block still open closes at the end of
 * the input, the blocks of the table cells queued for it are read, and the
 * document closes last, so it is the last block the queue holds, where its
 * input ended. */
static void S_parse_block_inputs(markdown_core_parser *parser);
/* THE OPEN BLOCKS of the parse of the text before a cursor, as its text
 * ends: the builders from the document's last child to the current block,
 * outermost first, which the context sink lists (sink.h). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_context_blocks(markdown_core_parser *parser) {
    markdown_core_context_sink *sink = parser->sink;
    for (markdown_core_node *open = parser->root; open != parser->at.current && open->child_count;) {
        open = open->children[open->child_count - 1];
        const markdown_core_node **blocks =
            markdown_core_reserve(sink->blocks, &sink->block_capacity, sink->block_count + 1, sizeof(*blocks));
        if (!blocks) {
            sink->failed = true;
            return;
        }
        sink->blocks = blocks;
        blocks[sink->block_count++] = open;
    }
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_close_document(markdown_core_parser *parser) {
    markdown_core_node *root = parser->root;
    if (parser->sink) {
        S_context_blocks(parser);
    }
    markdown_core_parser_close_below(parser, root);
    const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, root);
    S_settle_pending_as(parser, root, kind);
    S_parse_block_inputs(parser);
    if (parser->error) {
        return;
    }
    root->place.end = (uint32_t)parser->at.last_line_end;
    S_shut(parser, root);
    if (S_close(parser, root, kind, NULL)) {
        S_complete_later(parser, root, NULL);
    }
}

/* Each queued cell is the root of its own input: the stack holds only the
 * cell while its content is read, and its blocks settle into it as the input
 * ends. The cells are read on the cell input's storage, and the document's
 * input, line state and builders wait unchanged until they are read, so the
 * cells can be read wherever the document's parse stands. A failed read
 * leaves the blocks open in the cell to go with the cell. */
static void S_parse_block_inputs(markdown_core_parser *parser) {
    if (parser->block_input_cursor == parser->block_input_count) {
        return;
    }
    if (!parser->cell_input) {
        parser->cell_input = markdown_core_alloc(1, sizeof(*parser->cell_input));
        if (!parser->cell_input) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        markdown_core_strbuf_init(&parser->cell_input->last_line, 0);
    }
    const markdown_core_parser_input document = parser->input;
    const markdown_core_parser_at at = parser->at;
    const uint32_t enclosing = parser->input_enclosing;
    parser->input = *parser->cell_input;
    parser->at.pending = NULL;
    while (parser->block_input_cursor < parser->block_input_count && !parser->error) {
        const markdown_core_block_input input = parser->block_inputs[parser->block_input_cursor++];
        markdown_core_node *owner = input.owner;
        parser->at.block_root = parser->at.current = owner;
        parser->input_enclosing = input.enclosing;
        parser->at.line_number = parser->line_marks.values[owner->content_map.first].line - 1;
        parser->at.last_line_end = parser->line_marks.values[owner->content_map.first].source;
        owner->flags |= MARKDOWN_CORE_NODE__OPEN;
        S_parse_source(parser, owner->content.ptr, (size_t)owner->content.size);
        if (parser->error) {
            break;
        }
        markdown_core_parser_close_below(parser, owner);
        S_settle_pending(parser, owner);
        S_shut(parser, owner);
        /* Its content was read as its blocks, so none of it is its text. */
        owner->cut_front = owner->content.size;
        owner->content_map.count = 0;
        owner->content_map.offset = 0;
    }
    *parser->cell_input = parser->input;
    parser->input = document;
    parser->at = at;
    parser->input_enclosing = enclosing;
}

/* A commit reads its lines (S_parse_source) until a request ends it at an
 * opener's call, where the candidate is made and the reading goes on, and
 * the last commit decides the candidates its lines leave (THE CANDIDATES):
 * an accept reads the commit's lines again from its candidate's call. */
bool markdown_core_parser_commit(markdown_core_parser *parser, const char *source, size_t length, bool ends) {
    markdown_core_stream *stream = parser->stream;
    if (!parser->error) {
        S_take_back(parser, false);
        stream->ends = ends;
        stream->fed = 0;
    }
    while (!parser->error) {
        S_parse_source(parser, (const unsigned char *)source, length);
        if (parser->error) {
            break;
        }
        if (stream->requested) {
            S_candidate_make(parser);
        } else if (!ends || !stream->mark_count || !S_candidates_end(parser)) {
            break;
        }
    }
    return !parser->error;
}

markdown_core_node *markdown_core_parser_flush(markdown_core_parser *parser) { return S_finish_parse(parser); }

markdown_core_node *markdown_core_parser_finalize(markdown_core_parser *parser) {
    markdown_core_node *document = S_finish_parse(parser);
    S_release_parse(parser);
    return document;
}

void markdown_core_parser_discard(markdown_core_parser *parser) { S_release_parse(parser); }

/* THE SNAPSHOT (docs/plans/2026-09-29-incremental-parsing.md, 5.3; step 7,
 * 4.4): the committed parse, read on to the end of the text and published in
 * place, then put back as its commit left it. Its work is the tail's -- the
 * rest of the line the commit holds, the lines after the call of a
 * candidate it accepts (THE CANDIDATES), the open blocks they reach and the
 * pending block -- and never the committed blocks', which it reads and does
 * not write. Everything it writes in the
 * parser it puts back (S_snapshot_mark); the blocks it writes it lends.
 *
 * THE LENDING. What the snapshot reads changes no open block below its STOP:
 * the block the held line stopped in, when that line is the last of the
 * text; the root, when a line follows the commit's; and the current block,
 * when no line is left. The snapshot LENDS the stop, every block open above
 * it and the pending block (S_lend): a lent block's state goes to a slot of
 * its own, and its address holds a VIEW of it (markdown_core_node_view),
 * which the snapshot reads, writes, closes and publishes in its place -- a
 * view holds its block's children array, content buffer and field nodes,
 * and adds to them only past what the block holds. The lending REACHES the
 * open block below the stop, into which the lent blocks settle, and keeps
 * what it held -- its child count, its last child, its flags, its end and
 * its anchor, which a paragraph closing as its first child can give it (its
 * ROOM, S_room) -- as the snapshot adds children past them or replaces the
 * last. A block the lending reaches that the snapshot closes after all, as
 * a block start none of the blocks above it can hold takes its place, is
 * lent as it closes, and the lending reaches the block below it
 * (S_lend_below). The open blocks below the one the lending reaches end
 * where the text ends without closing, and the snapshot writes only what
 * they publish (S_close_tail): their ends and what they take from their
 * folds, which it reads without writing their flags; the publish gives
 * them their extents and runs. The published document reads the views
 * until the next TAKE-BACK, at the next commit, at the finalize or at the
 * release (S_take_back): each lent block takes its own state back with the
 * storage its view grew, each room what it held, and the nodes the snapshot
 * made are the publish's records' from then on (ast.c, THE RECORDS).
 *
 * THE MARK: what a reading so far says, which a reading put back to the
 * mark finds as the mark found it -- where the reading stands, the input's
 * index, the content map's last mark, the registry, the completion queue,
 * the cell inputs, and what each element's parse record keeps (element.h,
 * `kept_size`), in the dialect's `kept_size` bytes at `kept`, which its
 * taker owns. */
typedef struct markdown_core_parser_mark {
    markdown_core_parser_at at;
    bool mapped;
    size_t line_count, fact_count, scanned;
    markdown_core_normalized_line *normalized;
    size_t line_mark_count;
    markdown_core_line_mark last_mark;
    markdown_core_registry_mark registry;
    markdown_core_node *completion_first, *completion_last;
    size_t block_input_count, block_input_cursor;
    unsigned char *kept;
} S_snapshot_mark;

/* A snapshot takes a mark and rolls back to it on every append, so the
 * mark, the restore and the rollback stay in their callers; a candidate's
 * making and undo share them. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_mark(const markdown_core_parser *parser,
                                                                   S_snapshot_mark *mark) {
    const markdown_core_dialect *dialect = parser->dialect;
    for (size_t i = 0; i < dialect->kept_count; i++) {
        const markdown_core_element_instance *instance = dialect->kept_instances[i];
        memcpy(mark->kept + instance->kept_offset, instance->state, instance->element->kept_size);
    }
    *mark = (S_snapshot_mark){
        .at = parser->at,
        .mapped = parser->input.mapped,
        .line_count = parser->input.lines.count,
        .fact_count = parser->input.fact_count,
        .scanned = parser->input.scanned,
        .normalized = parser->input.normalized,
        .line_mark_count = parser->line_marks.count,
        .registry = markdown_core_registry_mark_now(&parser->registry),
        .completion_first = parser->completion_first,
        .completion_last = parser->completion_last,
        .block_input_count = parser->block_input_count,
        .block_input_cursor = parser->block_input_cursor,
        .kept = mark->kept,
    };
    if (parser->line_marks.count) {
        mark->last_mark = parser->line_marks.values[parser->line_marks.count - 1];
    }
}

/* Puts back the parser's state as `mark` found it: where the reading stood,
 * the content map, the registry, the completion queue, the cell inputs and
 * what the elements' parse records keep. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_restore(markdown_core_parser *parser,
                                                                      const S_snapshot_mark *mark) {
    const markdown_core_dialect *dialect = parser->dialect;
    for (size_t i = 0; i < dialect->kept_count; i++) {
        const markdown_core_element_instance *instance = dialect->kept_instances[i];
        memcpy(instance->state, mark->kept + instance->kept_offset, instance->element->kept_size);
    }
    parser->at = mark->at;
    parser->input.last_line_ready = false;
    markdown_core_array_truncate(&parser->line_marks, mark->line_mark_count);
    if (mark->line_mark_count) {
        *markdown_core_array_write(&parser->line_marks, mark->line_mark_count - 1) = mark->last_mark;
    }
    markdown_core_registry_rollback(&parser->registry, mark->registry);
    parser->completion_first = mark->completion_first;
    parser->completion_last = mark->completion_last;
    if (mark->completion_last) {
        markdown_core_scaffold_of(mark->completion_last)->next = NULL;
    }
    parser->block_input_count = mark->block_input_count;
    parser->block_input_cursor = mark->block_input_cursor;
}

/* The container and table facts of input lines `first` up to `end`, which
 * name blocks a reading made, gone, and the facts past the first
 * `fact_count` with them. */
static void S_drop_facts(markdown_core_parser_input *input, size_t first, size_t end, size_t fact_count) {
    for (size_t i = first; i < end; i++) {
        if (input->lines.values[i].facts > fact_count) {
            markdown_core_array_write(&input->lines, i)->facts = 0;
        } else if (input->lines.values[i].facts) {
            markdown_core_line_facts *facts = &input->facts[input->lines.values[i].facts - 1];
            facts->container = NULL;
            facts->table_container = NULL;
        }
    }
}

/* Puts back what the snapshot wrote in the parser: its state (S_restore)
 * and the input's index -- the lines it scanned, whether they map their
 * source, the facts it gave lines the commit had scanned, the normalized
 * lines it made. The container and table
 * facts of those lines go too: they name blocks the snapshot made, which
 * the next take-back unhooks. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_rollback(markdown_core_parser *parser,
                                                                       const S_snapshot_mark *mark) {
    S_restore(parser, mark);
    markdown_core_parser_input *input = &parser->input;
    input->mapped = mark->mapped;
    while (input->normalized != mark->normalized) {
        markdown_core_normalized_line *line = input->normalized;
        input->normalized = line->next;
        input->facts[line->fact - 1].normalized = NULL;
        markdown_core_free(line);
    }
    /* The lines past the commit's line that the commit had scanned, and the
     * line it holds, which the snapshot read on. */
    const size_t first = (size_t)(mark->at.line_number + 1 - input->first_line) - (parser->stream->held ? 1 : 0);
    S_drop_facts(input, first, mark->line_count, mark->fact_count);
    markdown_core_array_truncate(&input->lines, mark->line_count);
    input->fact_count = mark->fact_count;
    input->scanned = mark->scanned;
}

void markdown_core_parser_release_records(markdown_core_parser *parser, markdown_core_tail_records *records) {
    for (size_t i = 0; i < records->count; i++) {
        if (records->items[i].node) {
            markdown_core_node_pool_release(parser->pool, records->items[i].node);
        }
    }
    records->count = 0;
    records->free = 0;
}

static void S_release_stream(markdown_core_parser *parser) {
    markdown_core_stream *stream = parser->stream;
    markdown_core_free(stream->lends);
    markdown_core_free(stream->rooms);
    markdown_core_parser_release_records(parser, &stream->records);
    markdown_core_free(stream->records.items);
    for (size_t table = 0; table < MARKDOWN_CORE_DEFINITION_TABLES; table++) {
        markdown_core_key_tree_free(&stream->committed.starts[table]);
    }
    for (size_t table = 0; table < MARKDOWN_CORE_LABEL_TABLES; table++) {
        markdown_core_key_tree_free(&stream->committed.labels[table]);
    }
    markdown_core_free(stream->kept);
    markdown_core_free(stream->own_tables);
    markdown_core_free(stream->defined);
    markdown_core_free(stream->misses);
    markdown_core_strbuf_free(&stream->miss_labels);
    for (size_t i = 0; i < stream->miss_root_count; i++) {
        if (stream->miss_roots[i].position.places) {
            markdown_core_node_pool_bytes_free(parser->pool, stream->miss_roots[i].position.places);
        }
    }
    markdown_core_free(stream->miss_roots);
    markdown_core_settled *settled = &stream->settled;
    S_settled_release(parser, settled);
    markdown_core_array_free(&settled->nodes);
    markdown_core_free(settled->misses);
    markdown_core_strbuf_free(&settled->labels);
    markdown_core_free(settled->anchored);
    markdown_core_free(settled->settle.starts);
    markdown_core_free(settled->settle.notes);
    markdown_core_free(stream);
    parser->stream = NULL;
}

/* Lends `block` (THE LENDING) into the lends `*lends` holds, `*count` of
 * them with room for `*capacity`: its state moves to a slot of its own, the
 * backup, and its address takes a view of it, linked as the block was.
 * False, with the parse failed, when storage could not be had. */
static bool S_lend_to(markdown_core_parser *parser, markdown_core_lend **lends, size_t *count, size_t *capacity,
                      markdown_core_node *block) {
    markdown_core_lend *grown = markdown_core_reserve(*lends, capacity, *count + 1, sizeof(*grown));
    if (grown) {
        *lends = grown;
    }
    markdown_core_node *backup = grown ? markdown_core_node_view(parser->pool, block) : NULL;
    if (!backup) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    grown[(*count)++] = (markdown_core_lend){block, backup, markdown_core_parser_last_child(block), false};
    markdown_core_node_swap(block, backup);
    *markdown_core_scaffold_of(block) = *markdown_core_scaffold_of(backup);
    block->flags |= MARKDOWN_CORE_NODE__LENT;
    return true;
}

/* The same for the snapshot. */
static bool S_lend(markdown_core_parser *parser, markdown_core_node *block) {
    markdown_core_stream *stream = parser->stream;
    return S_lend_to(parser, &stream->lends, &stream->lend_count, &stream->lend_capacity, block);
}

/* The room of `node`, the block the lending reaches (THE LENDING): what it
 * holds as the lending reaches it. False, with the parse failed, when
 * storage could not be had. */
static bool S_room(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_stream *stream = parser->stream;
    markdown_core_room *grown =
        markdown_core_reserve(stream->rooms, &stream->room_capacity, stream->room_count + 1, sizeof(*grown));
    if (!grown) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    stream->rooms = grown;
    grown[stream->room_count++] = (markdown_core_room){node,
                                                       markdown_core_parser_last_child(node),
                                                       node->child_count,
                                                       node->place.end,
                                                       node->flags,
                                                       node->attributes.anchor};
    return true;
}

/* The block the lending reaches, which the snapshot closes, lent: the
 * lending reaches the block below it next, or nothing past the root. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_lend_below(markdown_core_parser *parser, markdown_core_node *block) {
    markdown_core_stream *stream = parser->stream;
    stream->boundary = markdown_core_scaffold_parent(block);
    if (S_lend(parser, block) && stream->boundary) {
        S_room(parser, stream->boundary);
    }
}

/* The snapshot's stop (THE LENDING), reading `length` bytes of text. */
static markdown_core_node *S_stop(const markdown_core_parser *parser, size_t length) {
    const markdown_core_parser_input *input = &parser->input;
    if (parser->stream->held) {
        const markdown_core_input_line *line = &input->lines.values[parser->at.line_number - input->first_line];
        return markdown_core_input_line_next(parser, line) >= length ? parser->stream->stop.container : parser->root;
    }
    const size_t index = (size_t)(parser->at.line_number + 1 - input->first_line);
    const size_t start = index < input->lines.count ? input->lines.values[index].start : input->scanned;
    return start < length ? parser->root : parser->at.current;
}

/* The lending as the snapshot starts: the room of the block the lending
 * reaches, below the stop, and the stop, the blocks open above it and the
 * pending block lent. False, with the parse failed, when storage could not
 * be had. */
static bool S_lend_tail(markdown_core_parser *parser, size_t length) {
    markdown_core_stream *stream = parser->stream;
    markdown_core_node *stop = S_stop(parser, length);
    stream->boundary = stop == parser->root ? NULL : markdown_core_scaffold_parent(stop);
    if (stream->boundary && !S_room(parser, stream->boundary)) {
        return false;
    }
    if (parser->at.pending && !S_lend(parser, parser->at.pending)) {
        return false;
    }
    for (markdown_core_node *node = parser->at.current;; node = markdown_core_scaffold_parent(node)) {
        if (!S_lend(parser, node)) {
            return false;
        }
        if (node == stop) {
            return true;
        }
    }
}

/* The content buffer of a lent block, which its view holds and may have
 * grown, the block's own again, holding the block's bytes; one the view took
 * for a block that held none goes. The view holds none. */
static void S_take_back_content(markdown_core_node *view, markdown_core_node *block) {
    if (markdown_core_strbuf_owns(&block->content)) {
        markdown_core_strbuf_take_storage(&block->content, &view->content);
    } else if (markdown_core_strbuf_owns(&view->content)) {
        markdown_core_strbuf_free(&view->content);
    }
    markdown_core_strbuf_init_zeroed(&view->content);
}

/* The same of its children array, holding the block's children again, the
 * last of them `last`. */
static void S_take_back_children(markdown_core_parser *parser, markdown_core_node *view, markdown_core_node *block,
                                 markdown_core_node *last) {
    if (block->children) {
        block->children = view->children;
        if (block->child_count) {
            block->children[block->child_count - 1] = last;
        }
    } else if (view->children && view->children != parser->stream->settled.nodes.values) {
        markdown_core_node_pool_bytes_free(parser->pool, view->children);
    }
}

/* The children `node` holds that a reading added after it found the node
 * holding `count`, the last of them `last`, from the one at `from` on: every
 * one from the place of that last on but the last itself, which a block
 * that settled before it may have moved on (markdown_core_block_settle_before),
 * released. */
static void S_release_added(markdown_core_parser *parser, const markdown_core_node *node, uint32_t from,
                            const markdown_core_node *last) {
    for (uint32_t i = from; i < node->child_count; i++) {
        if (node->children[i] != last) {
            markdown_core_parser_release_node(parser, node->children[i]);
        }
    }
}

/* Where the children a reading added to a node that held `count` start,
 * the last of those it held included. */
static inline uint32_t S_added_from(uint32_t count) { return count ? count - 1 : 0; }

/* A node the snapshot made that the take-back unhooks: one its publish
 * placed is its record's (ast.c, THE RECORDS); one no publish places is a
 * group of its holder's relations -- a definition's body, a callout's title
 * -- and goes, what it groups being their records'. */
static void S_unhook(markdown_core_parser *parser, markdown_core_node *node) {
    if (!node->id) {
        node->child_count = 0;
        markdown_core_parser_release_node(parser, node);
    }
}

/* The same of the children a snapshot added, as S_release_added finds them. */
static void S_unhook_added(markdown_core_parser *parser, const markdown_core_node *node, uint32_t from,
                           const markdown_core_node *last) {
    for (uint32_t i = from; i < node->child_count; i++) {
        if (node->children[i] != last) {
            S_unhook(parser, node->children[i]);
        }
    }
}

static int S_unhook_field(markdown_core_node **slot, void *context) {
    S_unhook(context, *slot);
    *slot = NULL;
    return 1;
}

/* A lent block takes its state back (THE LENDING): its address holds it
 * again, with the id the publish gave its view, and the view goes. A view whose kind the snapshot changed is a node
 * the snapshot made: the block takes back its content, and the view is its
 * record's, or goes with a failed snapshot. With `release`, the nodes the
 * view holds that the block does not go first. */
static void S_take_back_lend(markdown_core_parser *parser, const markdown_core_lend *entry, bool release) {
    markdown_core_node *const address = entry->address, *const backup = entry->backup;
    if (address->kind != backup->kind) {
        assert(!backup->child_count);
        S_take_back_content(address, backup);
        markdown_core_node_swap(address, backup);
        markdown_core_tail_records *records = &parser->stream->records;
        for (size_t i = 0; !release && i < records->count; i++) {
            if (records->items[i].node == address) {
                records->items[i].node = backup;
                return;
            }
        }
        markdown_core_parser_release_node(parser, backup);
        return;
    }
    /* The settled nodes a lent paragraph holds first are the stream's (THE
     * SETTLE POINT). */
    const markdown_core_settled *settled = &parser->stream->settled;
    uint32_t from = S_added_from(backup->child_count);
    if (address == settled->address && settled->nodes.count > from) {
        from = (uint32_t)settled->nodes.count;
    }
    if (release) {
        S_release_added(parser, address, from, entry->last);
    } else {
        markdown_core_node_view_taken(address, backup, S_unhook_field, parser);
        S_unhook_added(parser, address, from, entry->last);
    }
    S_take_back_children(parser, address, backup, entry->last);
    S_take_back_content(address, backup);
    markdown_core_node_view_let_go(release ? parser->pool : NULL, address, backup);
    backup->id = address->id;
    markdown_core_node_swap(address, backup);
    markdown_core_parser_release_node(parser, backup);
}

/* An open block below the lent ones takes back what it held (THE LENDING),
 * and keeps the runs the publish gave it: the children a reading gave it
 * are its caller's to let go first. */
static void S_take_back_room(const markdown_core_room *room) {
    markdown_core_node *node = room->node;
    node->child_count = room->count;
    if (room->count) {
        node->children[room->count - 1] = room->last;
    }
    const markdown_core_node_internal_flags published = MARKDOWN_CORE_NODE__PUBLISHED | MARKDOWN_CORE_NODE__FINAL;
    node->flags = (markdown_core_node_internal_flags)((room->flags & ~published) | (node->flags & published));
    node->place.end = room->end;
    if (node->attributes.anchor.data != room->anchor.data) {
        markdown_core_chunk_free(&node->attributes.anchor);
        node->attributes.anchor = room->anchor;
    }
}

/* THE TAKE-BACK (THE LENDING): the lent blocks, the last lent first, and
 * then the rooms, the last reached first, each unhooking what the snapshot
 * gave it. With `release`, a snapshot that failed lets the nodes it made
 * go. */
static void S_take_back(markdown_core_parser *parser, bool release) {
    markdown_core_stream *stream = parser->stream;
    for (size_t i = stream->lend_count; i-- > 0;) {
        S_take_back_lend(parser, &stream->lends[i], release);
    }
    for (size_t i = stream->room_count; i-- > 0;) {
        const markdown_core_room *room = &stream->rooms[i];
        (release ? S_release_added : S_unhook_added)(parser, room->node, S_added_from(room->count), room->last);
        S_take_back_room(room);
    }
    stream->lend_count = stream->room_count = 0;
    stream->boundary = NULL;
}

/* The end of the text as the snapshot reads it: every block above the
 * block the lending reaches closes and settles into it, the blocks of the
 * cells queued for them are read, and the open blocks from there down to
 * the root end there without closing (THE LENDING) -- each takes its end
 * and what its close hook makes of its children, reserves its explicit
 * anchor as a complete block does, and folds into the block below as its
 * children fold, which gives the block below the flags it ends with and
 * what it publishes of them, and writes none of its flags -- and join no
 * queue. With nothing below the stop, the document closes. */
static void S_close_tail(markdown_core_parser *parser) {
    markdown_core_stream *stream = parser->stream;
    markdown_core_node *boundary = stream->boundary;
    if (!boundary) {
        S_close_document(parser);
        return;
    }
    markdown_core_parser_close_below(parser, boundary);
    S_settle_pending(parser, boundary);
    S_parse_block_inputs(parser);
    const uint32_t end = (uint32_t)parser->at.last_line_end;
    markdown_core_node_internal_flags flags = boundary->flags;
    const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, boundary);
    for (markdown_core_node *node = boundary;;) {
        markdown_core_node *const below = markdown_core_scaffold_parent(node);
        node->place.end = end;
        const markdown_core_element_instance *structure = kind->structure;
        if (structure && structure->element->close_block) {
            structure->element->close_block(structure, parser, node, below);
        }
        if (parser->error || !S_reserve_anchor(parser, node, node->place.start)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        if (!below) {
            return;
        }
        kind = markdown_core_parser_block_kind(parser, below);
        flags = kind->flags & (MARKDOWN_CORE_KIND_BLANK_PROPAGATES | MARKDOWN_CORE_KIND_FOLDS)
                    ? S_fold(kind, below, below->flags, flags)
                    : below->flags;
        node = below;
    }
}

markdown_core_node *markdown_core_parser_snapshot(markdown_core_parser *parser, const char *source, size_t length) {
    if (parser->error) {
        return NULL;
    }
    markdown_core_stream *const stream = parser->stream;
    S_snapshot_mark mark = {.kept = (unsigned char *)stream->candidate_room + parser->dialect->candidate_state_size};
    S_mark(parser, &mark);
    parser->settle_flags = 0;
    parser->snapshot = true;
    markdown_core_node *result = NULL;
    /* A SNAPSHOT SEES A CANDIDATE END (THE CANDIDATES): the commit puts
     * back what it read since the call and holds there, its calls waiting,
     * and the snapshot reads the lines anew, where the call accepted
     * claims. */
    size_t accepted_mark, accepted_call;
    if (stream->mark_count && S_candidates_see_end(parser, &accepted_mark, &accepted_call)) {
        S_rollback(parser, &mark);
        S_candidate_see(parser, accepted_mark, accepted_call);
        S_mark(parser, &mark);
    }
    stream->snapshot_mark = &mark;

    const bool lent = !parser->error && S_lend_tail(parser, length);
    if (lent) {
        S_parse_source(parser, (const unsigned char *)source, length);
        if (!parser->error) {
            S_close_tail(parser);
        }
        if (!parser->error) {
            S_complete_settled(parser);
        }
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        if (!parser->error) {
            document->element->finish_document(document, parser);
        }
        if (!parser->error) {
            document->element->publish_document(document, parser);
        }
        if (!parser->error) {
            result = parser->root;
            parser->stream->miss_published = parser->stream->miss_root_count;
            /* The settling paragraph keeps the id the publish gave it. */
            if (stream->settled.root) {
                stream->settled.id = stream->settled.root->id;
            }
        }
    }
    if (!result) {
        S_take_back(parser, true);
    }
    /* Settled nodes this snapshot's document does not hold are no
     * paragraph's to take: what it published continues them no more. Those
     * the commit took are the tree's once no candidate waits. */
    if (!stream->settled.root && stream->settled.address &&
        !(stream->settled.taken && markdown_core_parser_undecided(parser))) {
        S_settled_release(parser, &stream->settled);
    }
    stream->settled.root = NULL;
    S_rollback(parser, &mark);
    stream->claimant = NULL;
    stream->snapshot_mark = NULL;
    parser->settle_flags = MARKDOWN_CORE_NODE__COMMITTED;
    parser->snapshot = false;
    return result;
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
    view->fact = (uint32_t)(facts - parser->input.facts) + 1;
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

/* THE SOLE PHYSICAL-LINE SCANNER for root and mapped inputs: scans line
 * `entry` from `from` to its end, through its terminator, and counts the
 * NULs it meets in the line's facts, and the bytes past the frontier as the
 * input's line work. Grammar facts live beside their line,
 * so changing inputs drops them together. Inlining is explicit: both GCC and
 * Clang may otherwise outline this per-line step. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_scan_line(markdown_core_parser *parser,
                                                                        markdown_core_input_line *entry, size_t from) {
    uint32_t nul_count = 0;
    const size_t scanned = parser->input.scanned;
    const unsigned char *source = parser->input.source;
    const unsigned char *cursor = source + from;
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
    entry->end = (uint32_t)(cursor - source);
    if (cursor < end && *cursor == '\r') {
        cursor++;
    }
    if (cursor < end && *cursor == '\n') {
        cursor++;
    }
    if (nul_count) {
        markdown_core_line_facts *facts = entry->facts ? &parser->input.facts[entry->facts - 1]
                                                       : markdown_core_parser_extend_line_facts(parser, entry);
        if (!facts) {
            return false;
        }
        facts->nul_count += nul_count;
        parser->input.mapped = true;
    }
    parser->input.scanned = (size_t)(cursor - source);
    parser->input_line_work += parser->input.scanned - scanned;
    return true;
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_input_line *S_extend_source_lines(markdown_core_parser *parser, size_t index) {
    assert(parser->input.size <= MARKDOWN_CORE_SOURCE_CAPACITY);
    while (index >= parser->input.lines.count && parser->input.scanned < parser->input.size) {
        if (!markdown_core_array_reserve(&parser->input.lines, parser->input.lines.count + 1)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return NULL;
        }
        markdown_core_input_line entry = {.start = (uint32_t)parser->input.scanned};
        if (!S_scan_line(parser, &entry, entry.start)) {
            return NULL;
        }
        parser->input.lines.values[parser->input.lines.count++] = entry;
    }
    return index < parser->input.lines.count ? &parser->input.lines.values[index] : NULL;
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

/* Where the last complete line of `source` ends (parser.h, input_complete):
 * after the last CR or LF, leaving out a CR that ends the text. The text
 * grows at its end (THE DRIVER), and the call before read `read` bytes of it
 * and found its complete lines ending at `complete`: the search goes back no
 * further than the last byte that call read, as a CR there may end a line
 * now, and what it does not find ends where that call found. A text that
 * does not end with LF can end in a long line, so the search compares a
 * machine word at a time against the two bytes, as S_source_span_end does
 * forward, and steps bytewise only through the word that holds the
 * boundary. One of the driver's steps out of its line loop (THE DRIVER). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) size_t
    S_complete_end(const unsigned char *source, size_t length, size_t read, size_t complete) {
    assert(read <= length && complete <= read);
    const size_t floor = read ? read - 1 : 0;
    size_t end = length;
    if (end > floor && source[end - 1] == '\n') {
        return end;
    }
    if (end > floor && source[end - 1] == '\r') {
        end--;
    }
    const uint64_t ones = UINT64_C(0x0101010101010101);
    const uint64_t highs = UINT64_C(0x8080808080808080);
    while (end - floor >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, source + end - sizeof(word), sizeof(word));
        const uint64_t cr = word ^ (ones * '\r');
        const uint64_t lf = word ^ (ones * '\n');
        if ((((cr - ones) & ~cr) | ((lf - ones) & ~lf)) & highs) {
            break;
        }
        end -= sizeof(word);
    }
    while (end > floor && source[end - 1] != '\n' && source[end - 1] != '\r') {
        end--;
    }
    return end > floor ? end : complete;
}

/* THE LINE THE TEXT HAD NOT FINISHED, the last the line index scanned when
 * a session's text grows, scanned on to where the text now ends: the text
 * grew at its end, so the line keeps its bytes and only its end moves, and
 * what a reading learned of it as it was goes -- the view its NULs made of
 * it, which it has outgrown, and the facts of its prefixes, which a reading
 * of its end recorded. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool S_scan_last_line(markdown_core_parser *parser) {
    markdown_core_parser_input *input = &parser->input;
    markdown_core_input_line *line = markdown_core_array_write(&input->lines, input->lines.count - 1);
    const uint32_t end = line->end;
    if (!S_scan_line(parser, line, end)) {
        return false;
    }
    markdown_core_line_facts *facts = line->facts ? &input->facts[line->facts - 1] : NULL;
    if (line->end != end && facts) {
        facts->container = NULL;
        facts->table_container = NULL;
        if (facts->normalized) {
            markdown_core_normalized_line **link = &input->normalized;
            while (*link != facts->normalized) {
                link = &(*link)->next;
            }
            *link = facts->normalized->next;
            markdown_core_free(facts->normalized);
            facts->normalized = NULL;
        }
    }
    return true;
}

/* Reads input line `index`, from its start or from `from`, holding it with
 * `hold` (THE LINE A SESSION HOLDS). */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_read_line(markdown_core_parser *parser,
                                                                        const unsigned char *source, size_t length,
                                                                        markdown_core_input_line *found,
                                                                        const markdown_core_line_stop *from,
                                                                        const bool hold, const bool at_root) {
    bufsize_t content_length;
    const unsigned char *content = S_input_line_content(parser, found, &content_length);
    if (!content) {
        return;
    }
    /* Callbacks may grow the line index; keep only stable bytes/offsets. */
    size_t next = markdown_core_input_line_next(parser, found);
    parser->at.lookahead_cursor = source + next;
    parser->at.line_start = (bufsize_t)found->start;
    /* A document line's scan recorded where its content ends; a cell's
     * line ends where the cell's map places its last byte. */
    if (at_root) {
        parser->at.line_end = (bufsize_t)found->end;
    } else {
        parser->at.line_end =
            markdown_core_parser_mapped_source_end(parser, parser->at.line_number + 1, content_length);
    }
    if (from || hold) {
        S_process_held_line(parser, content, content_length, from, hold);
    } else {
        S_process_line(parser, content, content_length);
    }
    if (parser->claimed_cursor) {
        assert(parser->claimed_cursor >= source + next && parser->claimed_cursor <= source + length);
        parser->at.line_number = parser->claimed_line;
        parser->at.line_start = (bufsize_t)markdown_core_parser_visited_line(parser, parser->at.line_number)->start;
        parser->at.last_line_end = parser->claimed_last_end;
        parser->claimed_cursor = NULL;
    }
}

/* The commit stops in the root, before line 1: the reading of the envelope
 * requested it (THE CANDIDATES). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_stop_at_root(markdown_core_parser *parser) {
    parser->stream->stop = (markdown_core_line_stop){.container = parser->root};
}

/* How a step of the driver out of its line loop leaves the call: reading
 * lines on, reading again from the envelope, where a candidate's accept put
 * the commit back, or stopped. */
typedef enum { S_DRIVER_READS_ON, S_DRIVER_RESTARTS, S_DRIVER_STOPS } S_driver_step;

/* THE LINE A SESSION HOLDS, read on from where the last commit stopped in
 * it, and held again when the text still ends in it. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) S_driver_step
    S_read_held_line(markdown_core_parser *parser, const unsigned char *source, size_t length, size_t limit) {
    markdown_core_stream *const stream = parser->stream;
    const size_t index = (size_t)(parser->at.line_number - parser->input.first_line);
    markdown_core_input_line *found = &parser->input.lines.values[index];
    bool hold = false;
    if (!parser->snapshot) {
        if (stream->mark_count &&
            S_feed(parser, parser->at.line_number, source + markdown_core_input_line_next(parser, found))) {
            return S_DRIVER_RESTARTS;
        }
        found = &parser->input.lines.values[index];
        stream->held = false;
        hold = found->start >= limit;
    }
    const markdown_core_line_stop from = stream->stop;
    S_meet_from_stop(parser);
    S_read_line(parser, source, length, found, &from, hold, true);
    /* A line held again ends the commit, and so does a request. */
    return hold || stream->requested ? S_DRIVER_STOPS : S_DRIVER_READS_ON;
}

/* The line that starts where a commit more text may follow stops, input
 * line `index`, read and held: true when a candidate's accept put the commit
 * back instead. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool S_hold_next_line(markdown_core_parser *parser,
                                                                 const unsigned char *source, size_t length,
                                                                 size_t index) {
    markdown_core_input_line *found = S_extend_source_lines(parser, index);
    if (found && parser->stream->mark_count) {
        if (S_feed(parser, parser->at.line_number + 1, source + markdown_core_input_line_next(parser, found))) {
            return true;
        }
        found = &parser->input.lines.values[index];
    }
    if (found) {
        S_read_line(parser, source, length, found, NULL, true, true);
    }
    return false;
}

/* THE DRIVER: reads the lines of `source` as the active input. A cell's
 * content is an input of its own, installed and read whole. The document's
 * text is installed at its first commit and grows at its end from one commit
 * to the next, so its lines keep their geometry and facts and the driver
 * continues from the line it stopped before; the envelope at its start is
 * read with its first line, by the call before it, which stops in the root
 * at line 0 (THE CANDIDATES). A commit more text may follow reads the lines
 * the text has finished, to `limit`, and holds the line that starts there
 * once a line before it has been read with the envelope; the next reads on
 * in it first (THE LINE A SESSION HOLDS). What a call does once -- find
 * where the complete text ends, stop in the root, read on in the held line
 * and hold the next -- is out of line, so the loop that reads each line has
 * the driver's frame to itself. */
static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length) {
    assert(length <= MARKDOWN_CORE_SOURCE_CAPACITY);
    bool at_root = markdown_core_parser_block_root(parser) == parser->root;
    if (!at_root || !parser->input.source) {
        S_clear_normalized_lines(&parser->input);
        parser->input.scanned = 0;
        markdown_core_array_truncate(&parser->input.lines, 0);
        parser->input.fact_count = 0;
        parser->input.mapped = !at_root;
        parser->input.first_line = parser->at.line_number + 1;
        if (at_root) {
            parser->at.line_start = 0;
        }
    }
    if (!at_root || !parser->input.source || parser->input.size != length) {
        parser->input.epoch = ++parser->epochs;
    }
    const bool extends = at_root && parser->input.source;
    parser->input.complete =
        at_root ? S_complete_end(source, length, extends ? parser->input.size : 0, extends ? parser->input.complete : 0)
                : length;
    parser->input.source = source;
    parser->input.size = length;
    parser->input.last_line_ready = false;
    const size_t limit = markdown_core_parser_candidate_final(parser) ? length : parser->input.complete;
    markdown_core_stream *const stream = at_root ? parser->stream : NULL;
    if (stream && parser->input.lines.count && parser->input.scanned < length &&
        source[parser->input.scanned - 1] != '\n' && !S_scan_last_line(parser)) {
        return;
    }
read:
    if (at_root && !parser->at.line_number && limit) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        if (stream) {
            S_meet_from_stop(parser);
        }
        parser->at.asks = parser->at.step_asks = 0;
        document->element->read_document_prefix(document, parser);
        if (stream && stream->requested) {
            S_stop_at_root(parser);
            return;
        }
    }
    if (stream && stream->held) {
        const S_driver_step step = S_read_held_line(parser, source, length, limit);
        if (step == S_DRIVER_RESTARTS) {
            goto read;
        }
        if (step == S_DRIVER_STOPS) {
            return;
        }
    }
    /* THE FEED (THE CANDIDATES): the candidates see each line first. */
    while (!parser->error) {
        size_t index = (size_t)(parser->at.line_number + 1 - parser->input.first_line);
        size_t start =
            index < parser->input.lines.count ? parser->input.lines.values[index].start : parser->input.scanned;
        if (start >= limit) {
            if (stream && !parser->snapshot && start == limit && start < length && parser->at.line_number &&
                S_hold_next_line(parser, source, length, index)) {
                goto read;
            }
            break;
        }
        markdown_core_input_line *found = S_extend_source_lines(parser, index);
        if (!found) {
            break;
        }
        if (stream && stream->mark_count) {
            if (S_feed(parser, parser->at.line_number + 1, source + markdown_core_input_line_next(parser, found))) {
                goto read;
            }
            found = &parser->input.lines.values[index];
        }
        S_read_line(parser, source, length, found, NULL, false, at_root);
        if (stream && stream->requested) {
            break;
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
// parser->at.first_nonspace, parser->at.first_nonspace_column,
// parser->at.indent, and parser->at.blank. Does not advance parser->at.offset.
/* THE DISTANCE TO THE NEXT TAB STOP IS DERIVED, NOT CARRIED.
 *
 * This used to open with `chars_to_tab = TAB_STOP - (parser->at.column %
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

    if (parser->at.first_nonspace <= parser->at.offset) {
        parser->at.first_nonspace = parser->at.offset;
        parser->at.first_nonspace_column = parser->at.column;
        while ((c = peek_at(input, parser->at.first_nonspace))) {
            if (c == ' ') {
                parser->at.first_nonspace += 1;
                parser->at.first_nonspace_column += 1;
            } else if (c == '\t') {
                parser->at.first_nonspace += 1;
                parser->at.first_nonspace_column += TAB_STOP - (parser->at.first_nonspace_column % TAB_STOP);
            } else {
                break;
            }
        }
    }

    parser->at.indent = parser->at.first_nonspace_column - parser->at.column;
    parser->at.blank = markdown_core_is_line_end(peek_at(input, parser->at.first_nonspace));
}

// Advance parser->at.offset and parser->at.column.  parser->at.offset is the
// byte position in input; parser->at.column is a virtual column number
// that counts each Unicode scalar once and expands tabs to tab stops.
// Source positions remain byte-based. The count parameter indicates
// how far to advance the offset.  If columns is true, then count
// indicates a number of columns; otherwise, a number of bytes.
// If advancing a certain number of columns partially consumes
// a tab character, parser->at.partially_consumed_tab is set to true.
void markdown_core_block_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count,
                                        bool columns) {
    char c;
    int chars_to_tab;
    int chars_to_advance;
    while (count > 0 && parser->at.offset < input->len && (c = peek_at(input, parser->at.offset))) {
        if (c == '\t') {
            chars_to_tab = TAB_STOP - (parser->at.column % TAB_STOP);
            if (columns) {
                parser->at.partially_consumed_tab = chars_to_tab > count;
                chars_to_advance = MIN(count, chars_to_tab);
                parser->at.column += chars_to_advance;
                parser->at.offset += (parser->at.partially_consumed_tab ? 0 : 1);
                count -= chars_to_advance;
            } else {
                parser->at.partially_consumed_tab = false;
                parser->at.column += chars_to_tab;
                parser->at.offset += 1;
                count -= 1;
            }
        } else {
            parser->at.partially_consumed_tab = false;
            parser->at.offset += 1;
            /* Valid UTF-8 is a caller precondition. Complete a virtual column
             * only at the scalar's end. Byte-counted advances may split a
             * scalar; column-counted advances always consume it completely. */
            bool scalar_end = parser->at.offset == input->len || (peek_at(input, parser->at.offset) & 0xC0) != 0x80;
            parser->at.column += scalar_end;
            count -= columns ? scalar_end : 1;
        }
    }
}

bool markdown_core_block_continue_indented(markdown_core_parser *parser, markdown_core_chunk *input, int continuation,
                                           bool accepts_blank) {
    bool res = false;

    if (parser->at.indent >= continuation) {
        markdown_core_block_advance_offset(parser, input, continuation, true);
        res = true;
    } else if (parser->at.blank && accepts_blank) {
        // Blankness is relative to the cursor after ancestor prefixes. Lists
        // require a first block; definition-list bodies first check whether
        // the blank run leads to a carried line. Footnotes and specimens
        // accept blank continuation directly, including during lookahead.
        markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace - parser->at.offset, false);
        res = true;
    }
    return res;
}

bufsize_t markdown_core_block_indent_reach(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                           const markdown_core_node *node, const markdown_core_chunk *input) {
    (void)self;
    (void)node;
    return markdown_core_block_scalar_end(input, parser->at.first_nonspace);
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

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool parse_element_block(
    markdown_core_parser *parser, const markdown_core_kind_record *kind, markdown_core_node *container,
    markdown_core_chunk *input, bool *should_continue, markdown_core_node **closing, markdown_core_node **taker) {
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
    if (matched == MARKDOWN_CORE_BLOCK_TAKEN) {
        assert(structure->element->take_line);
        *taker = container;
        return true;
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

/* Whether the call just made asked to be a candidate (THE CANDIDATES). */
static inline bool S_requested(const markdown_core_parser *parser) { return parser->stream->requested; }

/* Whether the step that matches `container` on the line a session holds
 * reads only bytes the line already has (THE LINE A SESSION HOLDS): as far
 * as its element says, and the first non-space byte, whether the line is
 * blank, when the element matches without reading the line. An element
 * that reads the line and does not say how far is not asked. */
static bool S_continue_decided(markdown_core_parser *parser, const markdown_core_kind_record *kind,
                               const markdown_core_node *container, const markdown_core_chunk *input) {
    const markdown_core_element_instance *structure = kind->structure;
    bufsize_t reach = parser->at.first_nonspace;
    if (structure) {
        const markdown_core_element *element = structure->element;
        if (element->continue_reach) {
            reach = element->continue_reach(structure, parser, container, input);
        } else if (element->last_block_matches || element->continue_container) {
            return false;
        }
    }
    return reach < input->len - 1;
}

/* Matches the line against the open blocks below `from`, the block root on a
 * new line. On the line a session holds, `hold`, it stops before the
 * first step that would read past the line's bytes, sets `*held` and
 * returns the last block matched. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_node *check_open_blocks(markdown_core_parser *parser, markdown_core_chunk *input, bool *all_matched,
                                          markdown_core_node *from, const bool hold, bool *held) {
    bool should_continue = true;
    *all_matched = false;
    markdown_core_node *container = from;
    markdown_core_node *closing = NULL;
    markdown_core_node *taker = NULL;

    while (container != parser->at.current && !taker) {
        /* Whatever a container's prefix consumed is that container's
         * MARKER: `> ` belongs to the block quote, the item's indent to the
         * list item. So the bytes of the block below begin where the
         * prefixes above it end: one claim per block, walking down the
         * spine, each naming the line by number, as matching a container can
         * read lines ahead, which can move the line table. */
        markdown_core_parser_rewritten_line(parser, parser->at.line_number)->own = (uint32_t)parser->at.offset;
        parser->at.step_asks = parser->at.asks;
        markdown_core_node *const above = container;
        container = container->children[container->child_count - 1];

        markdown_core_block_find_first_nonspace(parser, input);

        const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, container);
        if (hold && !S_continue_decided(parser, kind, container, input)) {
            *held = true;
            return above;
        }
        const markdown_core_element_instance *structure = kind->structure;
        if (structure && structure->element->last_block_matches) {
            if (!parse_element_block(parser, kind, container, input, &should_continue, &closing, &taker)) {
                goto done;
            }
        } else {
            bool taken = false;
            if (structure && structure->element->accepts_blank && parser->at.blank &&
                !structure->element->accepts_blank(structure, parser, container)) {
                /* A blank line's acceptance asked to be a candidate (THE
                 * CANDIDATES): the line holds at the call. */
                if (S_requested(parser)) {
                    *held = true;
                    return above;
                }
                goto done;
            }
            if (!S_container_prefix_matches(parser, structure, container, input, NULL, &taken)) {
                goto done;
            }
            if (taken) {
                markdown_core_node *current = markdown_core_parser_current(parser);
                if (S_kind_accepts_lines(markdown_core_parser_block_kind(parser, current), current)) {
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
    if (taker) {
        /* The line is the taker's, whole: what is open inside it ended on the
         * line before. */
        markdown_core_parser_close_below(parser, taker);
        if (!parser->error) {
            const markdown_core_element_instance *structure = markdown_core_parser_block_kind(parser, taker)->structure;
            structure->element->take_line(structure, parser, taker, input);
        }
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

static void S_lookahead_close_run(markdown_core_block_lookahead *lookahead, int line, const unsigned char *cursor) {
    if (lookahead->run_start) {
        /* A run holds only lines the text has finished: the line it has not
         * may take bytes that are not blank. */
        const unsigned char *finished = markdown_core_parser_complete_end(lookahead->parser);
        if (cursor > finished) {
            line--;
            cursor = finished;
        }
        markdown_core_line_facts *entry = markdown_core_parser_get_line_facts(lookahead->parser, lookahead->run_start);
        if (entry) {
            entry->run_end = line;
            entry->run_end_at = (size_t)(cursor - lookahead->parser->input.source);
        }
        lookahead->run_start = 0;
    }
}

/* Whether the containers path[from .. depth) accept every blank line. */
static bool S_lookahead_extras_accept_blank(const markdown_core_block_lookahead *lookahead, int from) {
    for (int i = from; i < lookahead->depth; i++) {
        if (!(markdown_core_parser_block_kind(lookahead->parser, lookahead->path[i])->flags &
              MARKDOWN_CORE_KIND_BLANK_RUNS)) {
            return false;
        }
    }
    return true;
}

/* Line `line` of the active input for a candidate's reading, which reads no
 * end (markdown_core_block_lookahead). */
static inline markdown_core_input_line *S_source_line_unread(markdown_core_parser *parser, int line) {
    if (line < parser->input.first_line || parser->error) {
        return NULL;
    }
    const size_t index = (size_t)(line - parser->input.first_line);
    return index < parser->input.lines.count ? &parser->input.lines.values[index]
                                             : S_extend_source_lines(parser, index);
}

/* Room for a lookahead on a path of `depth` containers. A lookahead starts
 * in a room no other one reads, so the room grows anew. */
static bool S_lookahead_room_grow(markdown_core_parser *parser, int depth) {
    size_t capacity = parser->lookahead_capacity ? parser->lookahead_capacity : 8;
    while (capacity < (size_t)depth) {
        capacity *= 2;
    }
    const size_t place = sizeof(*parser->lookahead_path) + sizeof(*parser->lookahead_containers);
    markdown_core_node **room = markdown_core_realloc(parser->lookahead_path, capacity * place);
    if (!room) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->lookahead_path = room;
    parser->lookahead_containers = (markdown_core_lookahead_container *)(room + capacity);
    parser->lookahead_capacity = capacity;
    return true;
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_lookahead_room(markdown_core_parser *parser, int depth) {
    return (size_t)depth <= parser->lookahead_capacity || S_lookahead_room_grow(parser, depth);
}

/* A lookahead on `path`, the `depth` containers from the active input's root
 * to the one its block would join, with room for them (S_lookahead_room) and
 * where the reading stands kept for `end`, reading from `cursor`, line
 * `line`. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_lookahead_start(
    markdown_core_parser *parser, markdown_core_node *const *path, int depth, const unsigned char *cursor, int line,
    markdown_core_block_lookahead *lookahead) {
    *lookahead = (markdown_core_block_lookahead){
        .parser = parser,
        .path = path,
        .cursor = cursor,
        .depth = depth,
        .touched = depth,
        .line = line,
        .saved = parser->at,
        .active = true,
    };
}

/* The container a block of `child` opened in `container` joins: the nearest
 * open container that can hold it, which is where
 * `markdown_core_parser_add_child` backs up to when it is opened. */
static markdown_core_node *S_joined(const markdown_core_parser *parser, markdown_core_node *container,
                                    markdown_core_node_type child) {
    while (container != parser->at.block_root && !markdown_core_node_can_contain_type(container, child)) {
        container = markdown_core_scaffold_parent(container);
    }
    return container;
}

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, markdown_core_node *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead) {
    /* The containers the lookahead matches are the stack up to the one the
     * block joins: a candidate's decision reads on the path its line keeps
     * (THE CANDIDATES), and every other reading builds its path from it. */
    markdown_core_node *parent = S_joined(parser, parent_container, child);
    int depth = 0;
    markdown_core_node *const *path = S_candidate_path(parser, parent, &depth);
    if (!path) {
        for (markdown_core_node *node = parent; node; node = markdown_core_parser_builder_below(parser, node)) {
            depth++;
        }
    }
    if (!S_lookahead_room(parser, depth)) {
        return false;
    }
    if (!path) {
        markdown_core_node **built = parser->lookahead_path;
        int at = depth;
        for (markdown_core_node *node = parent; node; node = markdown_core_parser_builder_below(parser, node)) {
            built[--at] = node;
        }
        path = built;
    }
    S_lookahead_start(parser, path, depth, parser->at.lookahead_cursor, parser->at.line_number + 1, lookahead);
    return true;
}

/* A reading that waits stops before line `line`, at `start`, which the text
 * has not finished: the line is unread, and its chain's flags are as the
 * line found them. */
static void S_lookahead_wait(markdown_core_block_lookahead *lookahead, int line, const unsigned char *start) {
    markdown_core_parser *parser = lookahead->parser;
    for (int i = lookahead->from; i < lookahead->depth; i++) {
        markdown_core_node *node = lookahead->path[i];
        const markdown_core_element_instance *structure = markdown_core_parser_block_kind(parser, node)->structure;
        const unsigned mask = structure ? structure->element->speculative_flags : 0;
        node->flags = (markdown_core_node_internal_flags)((node->flags & ~mask) |
                                                          (parser->lookahead_containers[i].line_flags & mask));
    }
    S_lookahead_close_run(lookahead, line, start);
    lookahead->cursor = start;
    lookahead->line = line;
    lookahead->waited = true;
}

/* The next line of `lookahead`, which is a reading that can wait,
 * `can_wait`, when its `limit` is set (markdown_core_block_lookahead). */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) int S_lookahead_next(markdown_core_block_lookahead *lookahead,
                                                                            markdown_core_chunk *line,
                                                                            int *first_nonspace, int *indent,
                                                                            int *blank_lines, const bool can_wait) {
    markdown_core_parser *parser = lookahead->parser;

    *blank_lines = 0;
    if (!lookahead->active) {
        return 0;
    }
    while (lookahead->cursor && !parser->error) {
        if (!can_wait && markdown_core_parser_input_ends(parser, lookahead->cursor)) {
            break;
        }
        if (can_wait && lookahead->cursor >= lookahead->limit) {
            /* A reading that can wait waits here, as it is. */
            S_lookahead_close_run(lookahead, lookahead->line, lookahead->cursor);
            lookahead->exhausted = true;
            return 0;
        }
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

        markdown_core_input_line *geometry =
            can_wait ? S_source_line_unread(parser, this_line) : markdown_core_parser_source_line(parser, this_line);
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

        parser->at.offset = 0;
        parser->at.column = 0;
        parser->at.first_nonspace = 0;
        parser->at.first_nonspace_column = 0;
        parser->at.indent = 0;
        parser->at.blank = false;
        parser->at.partially_consumed_tab = false;

        /* A line the text has not finished, read by a reading that waits,
         * matches each prefix only as far as its bytes decide, and resumes
         * only past prefixes they decided. */
        const bool finished = start < markdown_core_parser_complete_end(parser);
        const bool unfinished = can_wait && lookahead->waits && !finished;
        entry = markdown_core_parser_get_line_facts(parser, this_line);
        if (!entry && parser->error) {
            return 0;
        }
        if (entry && entry->container && entry->depth < lookahead->depth &&
            lookahead->path[entry->depth] == entry->container && (!unfinished || entry->decided)) {
            resumed = true;
            from = entry->depth + 1;
            if (entry->taken) {
                taken = true;
            } else {
                parser->at.offset = entry->offset;
                parser->at.column = entry->column;
                parser->at.partially_consumed_tab = entry->partially_consumed_tab;
                parser->at.first_nonspace = parser->at.offset;
                parser->at.first_nonspace_column = parser->at.column;
            }
        }
        /* The containers the line matches keep their flags as a line first
         * matched them, for `end`, and as the line found them, for a line it
         * waits before. */
        lookahead->from = from;
        markdown_core_lookahead_container *const containers = parser->lookahead_containers;
        for (i = from; i < lookahead->touched; i++) {
            containers[i].flags = lookahead->path[i]->flags;
        }
        if (from < lookahead->touched) {
            lookahead->touched = from;
        }
        if (unfinished) {
            for (i = from; i < lookahead->depth; i++) {
                containers[i].line_flags = lookahead->path[i]->flags;
            }
        }
        resumed_offset = parser->at.offset;
        for (i = from; i < lookahead->depth && carried && !taken; i++) {
            markdown_core_node *container = lookahead->path[i];
            markdown_core_block_find_first_nonspace(parser, &input);
            const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, container);
            if (unfinished && !S_continue_decided(parser, kind, container, &input)) {
                S_lookahead_wait(lookahead, this_line, start);
                return 0;
            }
            const markdown_core_element_instance *structure = kind->structure;
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
                carried = S_container_prefix_matches(parser, structure, container, &input,
                                                     markdown_core_parser_lookahead_parent(lookahead), &taken);
            }
        }
        /* The work of this visit is the prefix bytes it had to match itself:
         * what the resumed state did not already cover. */
        parser->block_lookahead_work += (size_t)(parser->at.offset - resumed_offset);
        if (!carried || closing) {
            S_lookahead_close_run(lookahead, this_line, start);
            lookahead->active = false;
            return 0;
        }
        if (!taken) {
            markdown_core_block_find_first_nonspace(parser, &input);
        }
        blank = taken || parser->at.blank;
        if (unfinished && !taken && blank) {
            S_lookahead_wait(lookahead, this_line, start);
            return 0;
        }

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
            entry->container = markdown_core_parser_lookahead_parent(lookahead);
            entry->depth = lookahead->depth - 1;
            entry->offset = parser->at.offset;
            entry->column = parser->at.column;
            entry->partially_consumed_tab = parser->at.partially_consumed_tab;
            entry->decided = finished || unfinished;
            entry->taken = taken;
            entry->blank = blank;
        }

        if (blank) {
            *blank_lines += 1;
            if (!lookahead->run_start) {
                lookahead->run_start = this_line;
            }
            if (resumed && entry->run_end > this_line + 1 &&
                (!can_wait || parser->input.source + entry->run_end_at <= lookahead->limit) &&
                S_lookahead_extras_accept_blank(lookahead, from)) {
                *blank_lines += entry->run_end - this_line - 1;
                lookahead->cursor = parser->input.source + entry->run_end_at;
                lookahead->line = entry->run_end;
            }
            continue;
        }
        S_lookahead_close_run(lookahead, this_line, start);
        *line = input;
        *first_nonspace = parser->at.first_nonspace;
        *indent = parser->at.indent;
        lookahead->incomplete = start >= markdown_core_parser_complete_end(parser);
        return 1;
    }
    S_lookahead_close_run(lookahead, lookahead->line, lookahead->cursor);
    lookahead->active = false;
    return 0;
}

int markdown_core_parser_lookahead_next(markdown_core_block_lookahead *lookahead, markdown_core_chunk *line,
                                        int *first_nonspace, int *indent, int *blank_lines) {
    return lookahead->limit ? S_lookahead_next(lookahead, line, first_nonspace, indent, blank_lines, true)
                            : S_lookahead_next(lookahead, line, first_nonspace, indent, blank_lines, false);
}

void markdown_core_parser_lookahead_end(markdown_core_block_lookahead *lookahead) {
    markdown_core_parser *parser = lookahead->parser;
    int i;

    if (!parser) {
        return;
    }
    for (i = lookahead->touched; i < lookahead->depth; i++) {
        markdown_core_node *node = lookahead->path[i];
        const markdown_core_element_instance *structure = markdown_core_parser_block_kind(parser, node)->structure;
        unsigned mask = structure ? structure->element->speculative_flags : 0;
        node->flags =
            (markdown_core_node_internal_flags)((node->flags & ~mask) | (parser->lookahead_containers[i].flags & mask));
    }
    parser->at = lookahead->saved;
    lookahead->active = false;
    lookahead->parser = NULL;
}

/* THE LINE'S VISIT (parser.h): whether a reading of input line `line` goes
 * on in the visit the line is in hand for. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_line_in_hand(const markdown_core_parser *parser,
                                                                           int line) {
    return parser->visit_line == line && parser->visit_epoch == parser->input.epoch;
}

/* A visit of input line `line` begins, with its content, the `bytes` bytes at
 * `buffer`, taken in hand: false, with the parse failed, when storage could
 * not be had. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_line_take(markdown_core_parser *parser, int line,
                                                                        const unsigned char *buffer, bufsize_t bytes) {
    if (bytes >= INT32_MAX / 2) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    if (bytes + 1 >= parser->curline.asize) {
        markdown_core_strbuf_grow(&parser->curline, bytes + 1);
    }
    if (parser->curline.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    memcpy(parser->curline.ptr, buffer, (size_t)bytes);
    parser->curline.ptr[bytes] = '\n';
    parser->curline.ptr[bytes + 1] = 0;
    parser->curline.size = bytes + 1;
    parser->visit_line = line;
    parser->visit_epoch = parser->input.epoch;
    parser->line_visit++;
    return true;
}

/* THE CANDIDATES (parser.h, markdown_core_parser_request_candidate). A call
 * whose reading waited where the text read so far ends, in a commit more
 * text may follow, asks to be a CANDIDATE: the commit holds the line at the
 * call (THE LINE A SESSION HOLDS) -- the document's prefix, called before
 * line 1, stops in the root at line 0 and holds none -- and reads on from the
 * call, which declines: one parse of the text the commit has, which ends
 * before the construct does, reads it so. Every later line the commit reads
 * is the candidate's to see first: each one its holder's prefixes carry is
 * FED to its owner (element.h, `feed_candidate`), until one ends the
 * construct, and the owner DECIDES (`decide_candidate`). An ACCEPT puts the
 * commit back at the call, where the call reads the lines anew and claims
 * them. A REJECT keeps what the commit read. A decision that read where the
 * text ends WAITS for more text, which it reads once each commit.
 *
 * A LINE'S MARK. The first call of a line MARKS the commit's state where it
 * holds: what settled completes, and the mark keeps the parser's state
 * with what the elements' records keep (THE MARK), the misses, and every
 * open block, the root included, and the pending
 * block, LENT (THE LENDING) from the root up, as what the commit reads after
 * the call can close any of them; and the mark's PATH, the open blocks from
 * the root to the container the call was asked in. Each later call of the
 * line asked in that container or below it joins the mark, its path reaching
 * the container; a call the path does not reach takes a mark of its own. An
 * accept of any call of a mark puts the commit back at the mark -- its state
 * as marked, each lent block its own state again, and what the commit read
 * since released, the records the last publish kept naming what it placed of
 * it (ast.c, THE RECORDS) -- and holds the line at the mark's first call. The
 * commit reads the line anew from there, and the calls before the one
 * accepted MEET it again (markdown_core_parser_candidate_meets), in the order
 * they were made: each declines as it did, its container and its path the
 * ones read anew, and the call accepted then claims. A rejected call stays
 * in its mark, DECIDED, for a reading anew to meet; a mark whose every call is
 * decided goes, and each block it lent keeps what the commit read: its view
 * takes what it views (markdown_core_node_view_adopt). The feed and the
 * decisions read on the mark's path, so no call's reading walks the blocks
 * below its holder again.
 *
 * A mark taken after another lies inside the other's lines: marks are kept
 * outermost first, an accept puts back every mark taken after its own, and a
 * block two marks lent, at the same depth in both, holds the outer one's view
 * in the inner one's backup. The commit's reading of a candidate's lines is
 * the one a snapshot publishes, unless the snapshot sees the construct end:
 * then it puts the commit back at the call's mark as an accept does, every
 * call of the mark waiting as the commit fed it, as only a commit decides a
 * call, and reads the lines anew to its text's end, where the calls before
 * the one accepted meet it and that one, the CLAIMANT, claims. The commit
 * reads the lines anew from the mark, where every call of it meets the
 * reading, and feeds each call from the line it was fed through. */
struct markdown_core_candidate {
    const markdown_core_element_instance *owner;
    /* The block the opener's reading read below, `depth` in its mark's path,
     * the depth there of the container the call was asked in, and where the
     * commit stopped at the call. */
    markdown_core_node *holder;
    size_t depth, container_depth;
    markdown_core_line_stop stop;
    /* What a reading anew meets the call by, with its owner and its line:
     * its number among the line's asks; and the kind of the block it would
     * open. */
    size_t ask;
    markdown_core_node_type child;
    /* The call after it in its mark, past which the next that waits stands:
     * decided calls on the way are stepped over once (S_call_next). */
    size_t next;
    /* The size of the text a decision that waits read. */
    size_t size;
    /* The call's line, 0 for the document's prefix, and the last line
     * fed. */
    int line, fed;
    /* Whether the call could claim the lines fed were the text to end
     * after them, whether a line ended the construct, and whether its
     * decision rejected it. */
    bool ends, ended, decided;
    /* Where the reading stood as a decision began. */
    markdown_core_parser_at saved;
};

/* A LINE'S MARK: the state, the misses and the lent blocks, the root first,
 * the pending block last, so a block's lend stands at its depth; the path;
 * the calls, the first that waits and how many wait; and in `states`, bytes
 * the mark owns, what the elements' parse records keep (`state.kept`, the
 * dialect's `kept_size` bytes) and after it the record each call keeps
 * (element.h, `candidate_state_size`), the call's at its index in room for
 * the dialect's largest; `state_capacity` counts bytes. */
struct markdown_core_candidate_mark {
    S_snapshot_mark state;
    size_t miss_count, miss_label_size, miss_root_count;
    markdown_core_lend *lends;
    size_t lend_count, lend_capacity;
    markdown_core_node **path;
    size_t path_capacity;
    struct markdown_core_candidate *calls;
    size_t call_count, call_capacity, first, live;
    unsigned char *states;
    size_t state_capacity;
};

const void *markdown_core_parser_kept(const markdown_core_parser *parser, const markdown_core_element_instance *self,
                                      size_t *records) {
    const markdown_core_stream *stream = parser->stream;
    assert(parser->snapshot && stream->snapshot_mark);
    const S_snapshot_mark *mark = stream->mark_count ? &stream->marks[0].state : stream->snapshot_mark;
    *records = mark->registry.count;
    return mark->kept + self->kept_offset;
}

/* The record the call at `index` of `mark` keeps. */
static inline void *S_call_state(const markdown_core_parser *parser, const struct markdown_core_candidate_mark *mark,
                                 size_t index) {
    const size_t size = parser->dialect->candidate_state_size;
    return size ? mark->states + parser->dialect->kept_size + index * size : NULL;
}

void markdown_core_parser_request_candidate(markdown_core_parser *parser, const markdown_core_element_instance *owner,
                                            markdown_core_node *container, markdown_core_node_type child,
                                            const void *state, bool ends) {
    markdown_core_stream *stream = parser->stream;
    assert(stream && !markdown_core_parser_candidate_final(parser) && !stream->requested);
    stream->requested = true;
    stream->request_owner = owner;
    stream->request_container = container;
    stream->request_child = child;
    assert(parser->at.asks);
    stream->request_ask = parser->at.asks - 1;
    if (owner->element->candidate_state_size) {
        memcpy(stream->candidate_room, state, owner->element->candidate_state_size);
    }
    stream->request_ends = ends;
}

/* The call of `mark` after the one at `index` that waits, or the mark's call
 * count: the calls decided on the way are stepped over from then on. */
static size_t S_call_next(struct markdown_core_candidate_mark *mark, size_t index) {
    size_t next = mark->calls[index].next;
    while (next < mark->call_count && mark->calls[next].decided) {
        next = mark->calls[next].next;
    }
    mark->calls[index].next = next;
    return next;
}

/* The holder of a call asked in `path[at]` that would open a block of
 * `child`: the nearest container on the path that can hold it, which is
 * where `markdown_core_parser_add_child` backs up to. Its depth. */
static size_t S_holder_depth(const markdown_core_parser *parser, markdown_core_node *const *path, size_t at,
                             markdown_core_node_type child) {
    while (path[at] != parser->at.block_root && !markdown_core_node_can_contain_type(path[at], child)) {
        at--;
    }
    return at;
}

/* Whether `container` is `path[from]` of `mark` or lies below it, the path
 * then reaching it: its depth there is `*at`. */
static bool S_path_reach(markdown_core_parser *parser, struct markdown_core_candidate_mark *mark, size_t from,
                         markdown_core_node *container, size_t *at) {
    size_t steps = 0;
    const markdown_core_node *node = container;
    for (; node && node != mark->path[from]; node = markdown_core_parser_builder_below(parser, node)) {
        steps++;
    }
    if (!node) {
        return false;
    }
    markdown_core_node **path =
        markdown_core_reserve(mark->path, &mark->path_capacity, from + steps + 1, sizeof(*mark->path));
    if (!path) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    mark->path = path;
    markdown_core_node *step = container;
    for (size_t i = from + steps; i > from; i--) {
        path[i] = step;
        step = markdown_core_scaffold_parent(step);
    }
    *at = from + steps;
    return true;
}

/* Whether `call` was made at `stop` of line `line`: a reading that holds
 * there asks no later call of the line. */
static bool S_made_at(const struct markdown_core_candidate *call, const markdown_core_line_stop *stop, int line) {
    return call->line == line && call->ask >= stop->asks;
}

/* A reading that starts where the commit holds its line, or at the
 * document's prefix, meets the calls of the last mark made there or after
 * it on the line, in the order they were made: every call of a mark an
 * accept put back, and otherwise the last ones made, at the stop itself. */
static void S_meet_from_stop(markdown_core_parser *parser) {
    markdown_core_stream *stream = parser->stream;
    stream->meet_mark = stream->mark_count;
    if (!stream->mark_count) {
        return;
    }
    struct markdown_core_candidate_mark *mark = &stream->marks[stream->mark_count - 1];
    size_t first = mark->call_count;
    if (stream->anew) {
        first = 0;
        stream->anew = parser->snapshot;
    } else {
        while (first && S_made_at(&mark->calls[first - 1], &stream->stop, parser->at.line_number)) {
            first--;
        }
    }
    if (first < mark->call_count) {
        stream->meet_mark = stream->mark_count - 1;
        stream->meet_call = first;
        stream->meet_depth = mark->calls[first].container_depth;
    }
}

/* The next call a reading meets is the next one made on its line, from
 * where the reading started (S_meet_from_stop), up to a snapshot's
 * CLAIMANT, which claims. A commit's reading takes the call's container and
 * path anew; a snapshot's writes no call. */
bool markdown_core_parser_meets_call(markdown_core_parser *parser, const markdown_core_element_instance *owner,
                                     markdown_core_node *container, size_t ask) {
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate_mark *mark = &stream->marks[stream->meet_mark];
    if (stream->meet_call >= mark->call_count) {
        return false;
    }
    struct markdown_core_candidate *call = &mark->calls[stream->meet_call];
    if (call == stream->claimant || call->line != parser->at.line_number || call->ask != ask) {
        return false;
    }
    assert(call->owner == owner);
    (void)owner;
    stream->meet_call++;
    if (parser->snapshot) {
        return true;
    }
    size_t at;
    if (!S_path_reach(parser, mark, stream->meet_depth, container, &at)) {
        assert(parser->error);
        return true;
    }
    call->stop.container = container;
    call->container_depth = at;
    call->depth = S_holder_depth(parser, mark->path, at, call->child);
    call->holder = mark->path[call->depth];
    stream->meet_depth = at;
    /* A mark whose calls are all decided, kept for its line read anew, goes
     * once the reading meets its last call. */
    if (stream->meet_call == mark->call_count && !mark->live) {
        stream->mark_count--;
    }
    return true;
}

bool markdown_core_parser_candidate_start(markdown_core_parser *parser, markdown_core_chunk *line,
                                          markdown_core_node **container) {
    struct markdown_core_candidate *candidate = parser->stream->deciding;
    candidate->saved = parser->at;
    markdown_core_input_line *found = &parser->input.lines.values[candidate->line - parser->input.first_line];
    bufsize_t length;
    const unsigned char *content = S_input_line_content(parser, found, &length);
    if (!content) {
        return false;
    }
    if (!S_line_in_hand(parser, candidate->line) && !S_line_take(parser, candidate->line, content, length)) {
        return false;
    }
    parser->reading = true;
    parser->at.line_number = candidate->line;
    parser->at.offset = candidate->stop.offset;
    parser->at.column = candidate->stop.column;
    parser->at.partially_consumed_tab = candidate->stop.partially_consumed_tab;
    parser->at.first_nonspace = 0;
    parser->at.first_nonspace_column = 0;
    parser->at.thematic_break_kill_pos = 0;
    parser->at.lookahead_cursor = parser->input.source + markdown_core_input_line_next(parser, found);
    parser->at.line_start = (bufsize_t)found->start;
    parser->at.line_end = (bufsize_t)found->end;
    *line = (markdown_core_chunk){.data = parser->curline.ptr, .len = parser->curline.size, .alloc = 0};
    markdown_core_block_find_first_nonspace(parser, line);
    *container = candidate->stop.container;
    return true;
}

void markdown_core_parser_candidate_end(markdown_core_parser *parser) {
    parser->at = parser->stream->deciding->saved;
    parser->reading = false;
}

/* The path a candidate's decision reads on: its mark's, from the root to
 * `parent`, the holder of the call being decided, `*depth` blocks; NULL for
 * every other reading. */
static markdown_core_node *const *S_candidate_path(const markdown_core_parser *parser, const markdown_core_node *parent,
                                                   int *depth) {
    const markdown_core_stream *stream = parser->stream;
    if (!stream->deciding || parent != stream->deciding->holder) {
        return NULL;
    }
    *depth = (int)stream->deciding->depth + 1;
    return stream->decision_mark->path;
}

/* THE DECIDED CONTENT (parser.h): the first mark's holders, found by
 * depth, as it lent every open block from the root up and the pending block
 * last. */
bool markdown_core_parser_undecided(const markdown_core_parser *parser) {
    const markdown_core_stream *stream = parser->stream;
    return stream->mark_count && stream->marks[0].lend_count;
}

bool markdown_core_parser_holder(const markdown_core_parser *parser, size_t depth, const markdown_core_node *block,
                                 markdown_core_holder *entry) {
    const struct markdown_core_candidate_mark *first = &parser->stream->marks[0];
    if (depth >= first->lend_count || first->lends[depth].address != block) {
        return false;
    }
    const markdown_core_lend *lend = &first->lends[depth];
    *entry = (markdown_core_holder){block, lend->last, lend->backup, lend->backup->child_count, (uint32_t)depth};
    return true;
}

/* The open blocks lent to `mark` from the root up, the pending block last,
 * each a view no publish placed. The path holds them, the root first.
 * False, with the parse failed, when storage could not be had. */
static bool S_mark_lend(markdown_core_parser *parser, struct markdown_core_candidate_mark *mark) {
    size_t depth = 0;
    for (markdown_core_node *node = parser->at.current; node; node = markdown_core_scaffold_parent(node)) {
        depth++;
    }
    markdown_core_node **path = markdown_core_reserve(mark->path, &mark->path_capacity, depth, sizeof(*path));
    if (!path) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    mark->path = path;
    size_t at = depth;
    for (markdown_core_node *node = parser->at.current; node; node = markdown_core_scaffold_parent(node)) {
        path[--at] = node;
    }
    for (size_t i = 0; i < depth; i++) {
        if (!S_lend_to(parser, &mark->lends, &mark->lend_count, &mark->lend_capacity, path[i])) {
            return false;
        }
    }
    return !parser->at.pending ||
           S_lend_to(parser, &mark->lends, &mark->lend_count, &mark->lend_capacity, parser->at.pending);
}

/* The mark the first call of a line takes, the call asked in `container`,
 * whose depth in its path is `*at`; NULL, with the parse failed, when
 * storage could not be had. Each entry of the marks keeps its storage for
 * the next mark made there. */
static struct markdown_core_candidate_mark *S_mark_make(markdown_core_parser *parser, markdown_core_node *container,
                                                        size_t *at) {
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate_mark *marks =
        markdown_core_reserve(stream->marks, &stream->mark_capacity, stream->mark_count + 1, sizeof(*marks));
    if (!marks) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    stream->marks = marks;
    if (stream->mark_count == stream->mark_made) {
        memset(&marks[stream->mark_made++], 0, sizeof(*marks));
    }
    struct markdown_core_candidate_mark *mark = &marks[stream->mark_count];
    /* Room for what the records keep and the call the mark is made for. */
    const size_t room = parser->dialect->kept_size + parser->dialect->candidate_state_size;
    if (room) {
        unsigned char *states = markdown_core_reserve(mark->states, &mark->state_capacity, room, 1);
        if (!states) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return NULL;
        }
        mark->states = mark->state.kept = states;
    }
    stream->mark_count++;
    mark->miss_count = stream->miss_count;
    mark->miss_label_size = (size_t)stream->miss_labels.size;
    mark->miss_root_count = stream->miss_root_count;
    mark->lend_count = mark->call_count = mark->first = mark->live = 0;
    S_mark(parser, &mark->state);
    if (!S_mark_lend(parser, mark)) {
        return NULL;
    }
    *at = 0;
    for (const markdown_core_node *node = container; node != parser->root; node = markdown_core_scaffold_parent(node)) {
        (*at)++;
    }
    assert(mark->path[*at] == container);
    return mark;
}

/* The candidate the opener asked for, made at the line its commit holds:
 * the call joins its line's mark, or takes one. */
static void S_candidate_make(markdown_core_parser *parser) {
    markdown_core_stream *stream = parser->stream;
    stream->requested = false;
    if (!markdown_core_parser_complete(parser)) {
        return;
    }
    markdown_core_node *const container = stream->request_container;
    assert(stream->stop.container == container && stream->stop.asks <= stream->request_ask);
    struct markdown_core_candidate_mark *mark = stream->mark_count ? &stream->marks[stream->mark_count - 1] : NULL;
    assert(!mark || mark->live);
    size_t at = 0;
    if (!mark || mark->calls[mark->call_count - 1].line != parser->at.line_number ||
        !S_path_reach(parser, mark, mark->calls[mark->call_count - 1].container_depth, container, &at)) {
        mark = parser->error ? NULL : S_mark_make(parser, container, &at);
        if (!mark) {
            return;
        }
    }
    struct markdown_core_candidate *calls =
        markdown_core_reserve(mark->calls, &mark->call_capacity, mark->call_count + 1, sizeof(*calls));
    if (!calls) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    mark->calls = calls;
    const size_t size = parser->dialect->candidate_state_size;
    if (size) {
        unsigned char *states = markdown_core_reserve(mark->states, &mark->state_capacity,
                                                      parser->dialect->kept_size + (mark->call_count + 1) * size, 1);
        if (!states) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        mark->states = mark->state.kept = states;
        memcpy(S_call_state(parser, mark, mark->call_count), stream->candidate_room, size);
    }
    const size_t index = mark->call_count++;
    const size_t depth = S_holder_depth(parser, mark->path, at, stream->request_child);
    struct markdown_core_candidate *candidate = &calls[index];
    *candidate = (struct markdown_core_candidate){
        .owner = stream->request_owner,
        .holder = mark->path[depth],
        .depth = depth,
        .container_depth = at,
        .stop = stream->stop,
        .ask = stream->request_ask,
        .child = stream->request_child,
        .next = index + 1,
        .line = parser->at.line_number,
        .fed = parser->at.line_number,
        .ends = stream->request_ends,
    };
    mark->live++;
}

/* Where the line after line `line` starts: the text's start after line 0,
 * where the document's prefix is read. */
static size_t S_line_next(const markdown_core_parser *parser, int line) {
    return line ? markdown_core_input_line_next(parser, &parser->input.lines.values[line - parser->input.first_line])
                : 0;
}

/* Feeds `candidate`, a call of `mark`, the lines its holder carries after the
 * last it was fed, up to `limit`, as more text may follow when `waits`:
 * CONTINUES once they are fed, ENDS at the first line that ends the
 * construct -- one the holder does not carry ends it -- and WAITS at a line
 * the text has not finished whose bytes do not tell. Blank lines before the
 * limit are fed as a line the text read so far does not have, and fed again
 * with the line after them. */
static markdown_core_candidate_step S_candidate_feed(markdown_core_parser *parser,
                                                     const struct markdown_core_candidate_mark *mark,
                                                     struct markdown_core_candidate *candidate, void *state,
                                                     const unsigned char *limit, bool waits) {
    markdown_core_block_lookahead lookahead;
    if (!S_lookahead_room(parser, (int)candidate->depth + 1)) {
        return MARKDOWN_CORE_CANDIDATE_ENDS;
    }
    S_lookahead_start(parser, mark->path, (int)candidate->depth + 1,
                      parser->input.source + S_line_next(parser, candidate->fed), candidate->fed + 1, &lookahead);
    lookahead.limit = limit;
    lookahead.waits = waits;
    const markdown_core_element_instance *owner = candidate->owner;
    markdown_core_candidate_step step = MARKDOWN_CORE_CANDIDATE_CONTINUES;
    for (;;) {
        markdown_core_chunk content;
        markdown_core_candidate_line line = {.content = &content};
        bool ends = candidate->ends;
        if (markdown_core_parser_lookahead_next(&lookahead, &content, &line.first, &line.indent, &line.blanks)) {
            line.unfinished = waits && lookahead.incomplete;
            step = owner->element->feed_candidate(owner, parser, state, &line, &ends);
            if (step != MARKDOWN_CORE_CANDIDATE_CONTINUES) {
                break;
            }
            candidate->fed = lookahead.line - 1;
            candidate->ends = ends;
            continue;
        }
        if (parser->error || (!lookahead.exhausted && !lookahead.waited)) {
            step = MARKDOWN_CORE_CANDIDATE_ENDS;
        } else if (line.blanks) {
            line = (markdown_core_candidate_line){.blanks = line.blanks};
            step = owner->element->feed_candidate(owner, parser, state, &line, &ends);
            if (step == MARKDOWN_CORE_CANDIDATE_CONTINUES) {
                candidate->ends = ends;
                if (lookahead.waited) {
                    step = MARKDOWN_CORE_CANDIDATE_WAITS;
                }
            }
        } else if (lookahead.waited) {
            step = MARKDOWN_CORE_CANDIDATE_WAITS;
        } else {
            candidate->fed = lookahead.line - 1;
        }
        break;
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return step;
}

/* The decision of the call at `index` of the mark at `m`, which asks whether
 * it read the end: one that read where more text may follow waits. */
static markdown_core_candidate_decision S_candidate_decide(markdown_core_parser *parser, size_t m, size_t index) {
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate *candidate = &stream->marks[m].calls[index];
    const markdown_core_element_instance *owner = candidate->owner;
    markdown_core_parser_clear_end_read(parser);
    stream->deciding = candidate;
    stream->decision_mark = &stream->marks[m];
    markdown_core_candidate_decision decision =
        owner->element->decide_candidate(owner, parser, S_call_state(parser, &stream->marks[m], index));
    stream->deciding = NULL;
    stream->decision_mark = NULL;
    if (markdown_core_parser_end_read(parser)) {
        decision = MARKDOWN_CORE_CANDIDATE_WAIT;
    }
    return parser->error ? MARKDOWN_CORE_CANDIDATE_REJECT : decision;
}

/* A lent block takes its state as the mark found it back: its address holds
 * it again, with the scaffold it has now, and the view goes with what it
 * holds that the block does not -- the children it added since and the
 * fields it took. What the next publish matches of them the records the
 * last publish kept name (ast.c, THE RECORDS). A view whose kind the commit
 * changed is a node the commit made, and the block it was comes back
 * continuing nothing (ast.c, PUBLISHING IN PLACE). */
static void S_candidate_take_back(markdown_core_parser *parser, const markdown_core_lend *entry) {
    markdown_core_node *const address = entry->address, *const backup = entry->backup;
    const markdown_core_scaffold scaffold = *markdown_core_scaffold_of(address);
    if (address->kind != backup->kind) {
        assert(!backup->child_count);
        S_take_back_content(address, backup);
        markdown_core_node_swap(address, backup);
        *markdown_core_scaffold_of(address) = scaffold;
        address->id = 0;
        markdown_core_parser_release_node(parser, backup);
        return;
    }
    /* The settled nodes the commit took come back to the stream (THE
     * SETTLE POINT). */
    markdown_core_settled *settled = &parser->stream->settled;
    uint32_t from = S_added_from(backup->child_count);
    if (address == settled->address && settled->taken) {
        settled->taken = false;
        from = settled->nodes.count > from ? (uint32_t)settled->nodes.count : from;
    }
    S_release_added(parser, address, from, entry->last);
    S_take_back_children(parser, address, backup, entry->last);
    S_take_back_content(address, backup);
    markdown_core_node_view_let_go(parser->pool, address, backup);
    backup->id = address->id;
    markdown_core_node_swap(address, backup);
    *markdown_core_scaffold_of(address) = scaffold;
    markdown_core_parser_release_node(parser, backup);
}

/* The commit back at the mark at `m`, every mark taken after it put back
 * first, holding the line at the mark's first call to read it anew. */
static void S_candidate_put_back(markdown_core_parser *parser, size_t m) {
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate_mark *mark = &stream->marks[m];
    for (size_t i = mark->miss_root_count; i < stream->miss_root_count; i++) {
        if (stream->miss_roots[i].position.places) {
            markdown_core_node_pool_bytes_free(parser->pool, stream->miss_roots[i].position.places);
        }
    }
    stream->miss_count = mark->miss_count;
    markdown_core_strbuf_truncate(&stream->miss_labels, (bufsize_t)mark->miss_label_size);
    stream->miss_root_count = mark->miss_root_count;
    if (stream->miss_published > stream->miss_root_count) {
        stream->miss_published = stream->miss_root_count;
    }
    stream->miss_root = NULL;
    S_restore(parser, &mark->state);
    /* The lines read anew start at the mark's line; the prefix's, before
     * line 1, holds no line, and the driver reads it again first. */
    const struct markdown_core_candidate *first = &mark->calls[0];
    markdown_core_parser_input *input = &parser->input;
    const int line = first->line ? first->line : input->first_line;
    S_drop_facts(input, (size_t)(line - input->first_line), input->lines.count, input->fact_count);
    stream->stop = first->stop;
    stream->held = first->line > 0;
    stream->fed = first->line;
    for (size_t i = stream->mark_count; i-- > m;) {
        struct markdown_core_candidate_mark *undone = &stream->marks[i];
        for (size_t k = 0; k < undone->lend_count; k++) {
            S_candidate_take_back(parser, &undone->lends[k]);
        }
        undone->lend_count = 0;
    }
    stream->mark_count = m + 1;
}

/* The mark at `m` marked and lent again while one of its calls waits; with
 * none, it stays while it keeps a decided call for the reading anew to meet,
 * lending nothing. */
static void S_mark_anew(markdown_core_parser *parser, size_t m) {
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate_mark *mark = &stream->marks[m];
    if (mark->live) {
        S_mark_lend(parser, mark);
    } else if (!mark->call_count) {
        stream->mark_count = m;
    }
}

/* AN ACCEPT: the commit back at the mark at `m` (S_candidate_put_back),
 * where the calls before the one at `index` meet the reading anew, and the
 * call accepted claims: it and the calls after it go. */
static void S_candidate_undo(markdown_core_parser *parser, size_t m, size_t index) {
    S_candidate_put_back(parser, m);
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate_mark *mark = &stream->marks[m];
    for (size_t k = index; k < mark->call_count; k++) {
        mark->live -= !mark->calls[k].decided;
    }
    mark->call_count = index;
    if (mark->first > index) {
        mark->first = index;
    }
    stream->anew = index > 0;
    S_mark_anew(parser, m);
}

/* A SNAPSHOT'S ACCEPT: the commit back at the mark at `m`
 * (S_candidate_put_back), every call of it waiting as the commit fed it, as
 * only a commit decides a call; the snapshot's reading anew meets each call
 * before the one at `index`, the CLAIMANT, which claims. */
static void S_candidate_see(markdown_core_parser *parser, size_t m, size_t index) {
    S_candidate_put_back(parser, m);
    markdown_core_stream *stream = parser->stream;
    stream->claimant = &stream->marks[m].calls[index];
    stream->anew = true;
    S_mark_anew(parser, m);
}

/* The lend of the block a lend of the mark at `m` names, at the same depth,
 * by the mark at `other`, or NULL. */
static markdown_core_lend *S_mark_lend_of(markdown_core_stream *stream, size_t other, size_t k,
                                          const markdown_core_node *address) {
    if (other >= stream->mark_count) {
        return NULL;
    }
    struct markdown_core_candidate_mark *mark = &stream->marks[other];
    return k < mark->lend_count && mark->lends[k].address == address ? &mark->lends[k] : NULL;
}

/* What the mark at `m` lent stays as the commit read it: each view takes
 * what it views and its backup goes. The view the next mark kept of a block
 * in its own backup takes it, when there is one. A view no other mark lends
 * is lent no more, the one the next mark keeps too. A view that left its
 * holder's children as it closed goes with the last mark that lent it. */
static void S_mark_unlend(markdown_core_parser *parser, size_t m) {
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate_mark *mark = &stream->marks[m];
    for (size_t k = 0; k < mark->lend_count; k++) {
        markdown_core_lend *entry = &mark->lends[k];
        markdown_core_node *const address = entry->address;
        markdown_core_lend *inner = S_mark_lend_of(stream, m + 1, k, address);
        markdown_core_lend *outer = m ? S_mark_lend_of(stream, m - 1, k, address) : NULL;
        markdown_core_node *const view = inner ? inner->backup : address;
        markdown_core_node_view_adopt(parser->pool, view, entry->backup);
        markdown_core_parser_release_node(parser, entry->backup);
        if (!outer) {
            view->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__LENT;
        }
        if (inner) {
            continue;
        }
        if (entry->dropped) {
            if (outer) {
                outer->dropped = true;
            } else {
                markdown_core_parser_release_node(parser, address);
            }
        }
    }
    mark->lend_count = 0;
}

/* A REJECT: the call at `index` of the mark at `m` is decided, and what the
 * commit read stays. The mark goes with its last call that waits, its
 * storage kept at the end of the marks. */
static void S_candidate_drop(markdown_core_parser *parser, size_t m, size_t index) {
    markdown_core_stream *stream = parser->stream;
    struct markdown_core_candidate_mark *mark = &stream->marks[m];
    mark->calls[index].decided = true;
    mark->live--;
    if (index == mark->first) {
        mark->first = S_call_next(mark, index);
    }
    if (mark->live) {
        return;
    }
    S_mark_unlend(parser, m);
    const struct markdown_core_candidate_mark gone = *mark;
    memmove(mark, mark + 1, (stream->mark_count - m - 1) * sizeof(*mark));
    stream->marks[--stream->mark_count] = gone;
    /* The reading meets no call of a mark that went. */
    if (stream->meet_mark > m) {
        stream->meet_mark--;
    } else if (stream->meet_mark == m) {
        stream->meet_mark = stream->mark_count;
    }
}

/* Every mark let go as the parse is released, the last taken first, each
 * as the last one, as a reject of its last call lets it go, and the marks'
 * storage with them. */
static void S_candidates_release(markdown_core_parser *parser) {
    markdown_core_stream *stream = parser->stream;
    for (; stream->mark_count; stream->mark_count--) {
        S_mark_unlend(parser, stream->mark_count - 1);
    }
    for (size_t i = 0; i < stream->mark_made; i++) {
        markdown_core_free(stream->marks[i].lends);
        markdown_core_free(stream->marks[i].path);
        markdown_core_free(stream->marks[i].calls);
        markdown_core_free(stream->marks[i].states);
    }
    markdown_core_free(stream->marks);
    stream->marks = NULL;
    stream->mark_made = stream->mark_capacity = 0;
}

/* A lent block that left its holder's children as it closed: in a commit,
 * a mark lent it, and the last that lent anything did, at its depth, which
 * keeps it until its calls are decided. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_candidate_drops(markdown_core_parser *parser,
                                                                  const markdown_core_node *block) {
    if (!parser->snapshot) {
        markdown_core_stream *stream = parser->stream;
        size_t m = stream->mark_count;
        while (!stream->marks[--m].lend_count) {
        }
        struct markdown_core_candidate_mark *mark = &stream->marks[m];
        size_t k = mark->lend_count;
        while (mark->lends[--k].address != block) {
        }
        mark->lends[k].dropped = true;
    }
}

/* THE FEED: every call that waits, outermost first, fed the lines up to
 * `through`, the line the driver reads next, which ends where `limit` is,
 * once: a commit feeds through each line once, and after an accept from
 * the line it reads anew. True when an accept put the commit back at a
 * mark. */
static bool S_feed(markdown_core_parser *parser, int through, const unsigned char *limit) {
    markdown_core_stream *stream = parser->stream;
    if (parser->snapshot || through <= stream->fed) {
        return false;
    }
    for (size_t m = 0; m < stream->mark_count && !parser->error;) {
        struct markdown_core_candidate_mark *mark = &stream->marks[m];
        bool gone = false;
        for (size_t k = mark->first; k < mark->call_count && !parser->error && !gone;) {
            struct markdown_core_candidate *candidate = &mark->calls[k];
            if (candidate->ended ? candidate->size == parser->input.size
                                 : candidate->fed >= through ||
                                       S_candidate_feed(parser, mark, candidate, S_call_state(parser, mark, k), limit,
                                                        !stream->ends) != MARKDOWN_CORE_CANDIDATE_ENDS) {
                k = S_call_next(mark, k);
                continue;
            }
            candidate->ended = true;
            switch (S_candidate_decide(parser, m, k)) {
            case MARKDOWN_CORE_CANDIDATE_ACCEPT:
                S_candidate_undo(parser, m, k);
                return true;
            case MARKDOWN_CORE_CANDIDATE_REJECT:
                gone = mark->live == 1;
                S_candidate_drop(parser, m, k);
                if (!gone) {
                    k = S_call_next(mark, k);
                }
                break;
            case MARKDOWN_CORE_CANDIDATE_WAIT:
                candidate->size = parser->input.size;
                k = S_call_next(mark, k);
                break;
            }
        }
        m += !gone;
    }
    stream->fed = through;
    return false;
}

/* The candidates as the last commit's text ends, each decided, outermost
 * first. True when an accept put the commit back at a mark. */
static bool S_candidates_end(markdown_core_parser *parser) {
    markdown_core_stream *stream = parser->stream;
    while (stream->mark_count && !parser->error) {
        const size_t first = stream->marks[0].first;
        assert(first < stream->marks[0].call_count);
        if (S_candidate_decide(parser, 0, first) == MARKDOWN_CORE_CANDIDATE_ACCEPT) {
            S_candidate_undo(parser, 0, first);
            return true;
        }
        S_candidate_drop(parser, 0, first);
    }
    return false;
}

/* A SNAPSHOT SEES A CANDIDATE END: each call that waits, outermost first, is
 * fed the lines to the end of the text on a copy, and one whose construct
 * the opener could claim there is decided as the text ends. True, with the
 * mark and the call, for the first that accepts. */
static bool S_candidates_see_end(markdown_core_parser *parser, size_t *m, size_t *index) {
    markdown_core_stream *stream = parser->stream;
    for (size_t i = 0; i < stream->mark_count; i++) {
        struct markdown_core_candidate_mark *mark = &stream->marks[i];
        for (size_t k = mark->first; k < mark->call_count && !parser->error; k = S_call_next(mark, k)) {
            bool possible = mark->calls[k].ended;
            if (!possible) {
                struct markdown_core_candidate fed = mark->calls[k];
                void *state = S_call_state(parser, mark, k);
                if (state) {
                    state = memcpy(stream->candidate_room, state, parser->dialect->candidate_state_size);
                }
                const markdown_core_candidate_step step =
                    S_candidate_feed(parser, mark, &fed, state, parser->input.source + parser->input.size, false);
                possible =
                    step == MARKDOWN_CORE_CANDIDATE_ENDS || (step == MARKDOWN_CORE_CANDIDATE_CONTINUES && fed.ends);
            }
            if (possible && S_candidate_decide(parser, i, k) == MARKDOWN_CORE_CANDIDATE_ACCEPT) {
                *m = i;
                *index = k;
                return true;
            }
        }
    }
    return false;
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

/* Whether the block start at the line's first non-space byte, on the line a
 * session holds, reads only bytes the line already has (THE LINE A SESSION
 * HOLDS): every element the dispatcher asks there before one claims it
 * says how far it reads, the one that claims it opens a container, and no
 * interrupter is asked there. */
static bool S_start_decided(markdown_core_parser *parser, const block_start_context *context) {
    const markdown_core_dialect *const dialect = parser->dialect;
    const int key = S_gate_key(context->input, context->first, context->indent);
    size_t count;
    S_gate_candidates(dialect, MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT, key, &count);
    if (count) {
        return false;
    }
    const markdown_core_element_instance *const *owners = dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_SCAN];
    const uint8_t *candidates = S_gate_candidates(dialect, MARKDOWN_CORE_BLOCK_HOOK_SCAN, key, &count);
    bufsize_t reach = (bufsize_t)context->first;
    for (size_t i = 0; i < count; i++) {
        const markdown_core_element_instance *owner = owners[candidates ? candidates[i] : i];
        if (context->indent > owner->element->maximum_block_indent) {
            continue;
        }
        if (!owner->element->start_reach) {
            return false;
        }
        bool opens = false;
        const bufsize_t read = owner->element->start_reach(owner, parser, context, &opens);
        reach = read > reach ? read : reach;
        if (opens) {
            return reach < context->input->len - 1;
        }
    }
    return false;
}

/* Where the line machine stands as it opens blocks: whether the line may
 * still be lazy, and how many blocks this line has asked to open. */
typedef struct {
    bool maybe_lazy;
    size_t depth;
} S_opens;

/* Opens the blocks the line starts in `*container`. On the line a session
 * holds, `hold`, it stops before the first start that would read past the
 * line's bytes and sets `*held`, as it does at an opener's call that asked
 * to be a candidate; `*opens` says where it stands. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void open_new_blocks(markdown_core_parser *parser,
                                                                            markdown_core_node **container,
                                                                            markdown_core_chunk *input,
                                                                            bool all_matched, S_opens *opens,
                                                                            const bool hold, bool *held) {
    /* Two facts keep a line from interrupting text: `paragraph`, the matched
     * container is a paragraph the line would continue, and `maybe_lazy`, the
     * current block would take the line lazily. Only the first turn has
     * either: a block opened here holds the rest of the line. */
    bool maybe_lazy = opens->maybe_lazy;
    size_t depth = opens->depth;
    const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, *container);
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
        markdown_core_block_find_first_nonspace(parser, input);
        /* Indentation ahead of whatever opens here is the CONTAINER's, not the
         * new block's: a block begins at its own first non-space byte, so
         * giving the spaces to the block being opened would make its first
         * region start before its own scope. Measured before it was fixed --
         * 52 rows of an indented code block's four spaces alone. */
        block_start_context context = {.container = *container,
                                       .input = input,
                                       .first = parser->at.first_nonspace,
                                       .column = parser->at.first_nonspace_column,
                                       .indent = parser->at.indent,
                                       .paragraph = paragraph,
                                       .lazy = maybe_lazy,
                                       .all_matched = all_matched,
                                       .depth = depth + 1,
                                       .thematic_kill = parser->at.thematic_break_kill_pos};
        parser->at.step_asks = parser->at.asks;
        if (hold && !S_start_decided(parser, &context)) {
            *opens = (S_opens){maybe_lazy, depth};
            *held = true;
            return;
        }
        depth++;
        scan_element_start(parser, &context, &start);
        if (parser->error) {
            return;
        }
        parser->at.thematic_break_kill_pos = context.thematic_kill;

        /* Dash-led tables precede thematic breaks and lists. An opener may
         * close the old path before an allocation fails; OOM is terminal,
         * never a grammar miss that can try another owner on that path. */
        const markdown_core_element_instance *const *interrupters =
            dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT];
        size_t interrupter_count;
        const uint8_t *interrupter_candidates =
            S_gate_candidates(dialect, MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT,
                              S_gate_key(input, parser->at.first_nonspace, parser->at.indent), &interrupter_count);
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
            if (S_requested(parser)) {
                goto requested;
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
                dialect, MARKDOWN_CORE_BLOCK_HOOK_OPEN, S_gate_key(input, parser->at.first_nonspace, 0), &opener_count);

            for (size_t element_index = 0; element_index < opener_count; element_index++) {
                const markdown_core_element_instance *opener =
                    openers[opener_candidates ? opener_candidates[element_index] : element_index];

                new_container = opener->element->try_opening_block(
                    opener, parser->at.indent > opener->element->maximum_block_indent, parser, *container, input->data,
                    input->len);
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
                if (S_requested(parser)) {
                    goto requested;
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
                            opener, parser->at.indent > opener->element->maximum_block_indent, parser, *container,
                            input->data, input->len);
                        if (parser->error) {
                            return;
                        }
                        if (new_container) {
                            *container = new_container;
                            return;
                        }
                        if (S_requested(parser)) {
                            goto requested;
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

        kind = markdown_core_parser_block_kind(parser, *container);
        if (S_kind_takes_text(kind, *container)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        maybe_lazy = false;
    }
    *opens = (S_opens){maybe_lazy, depth};
    return;
requested:
    /* An opener asked for its call to be a candidate (THE CANDIDATES): the
     * line holds at the call. */
    *opens = (S_opens){maybe_lazy, depth - 1};
    *held = true;
}

/* What remains at parser->at.offset is a text line: adds the text to the
 * appropriate container. False when the block that would take it did not
 * open: the parse failed, or the text opener's call asked to be a
 * candidate (THE CANDIDATES). */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool add_text_to_container(
    markdown_core_parser *parser, markdown_core_node *container, markdown_core_node *last_matched_container,
    markdown_core_chunk *input) {

    markdown_core_block_find_first_nonspace(parser, input);

    /* A blank line is the last line of the block before it that has not
     * settled: the one open inside the container, its last child, or the
     * container's pending child. */
    if (parser->at.blank) {
        markdown_core_node *before =
            container != parser->at.current ? container->children[container->child_count - 1] : parser->at.pending;
        if (before) {
            S_set_last_line_blank(before, true);
        }
    }

    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, container);
    const markdown_core_element_instance *structure = kind->structure;
    const bool accepts_lines = S_kind_accepts_lines(kind, container);
    const bool last_line_blank =
        parser->at.blank && !(kind->flags & MARKDOWN_CORE_KIND_BLANK_OPAQUE) &&
        (!(kind->flags & MARKDOWN_CORE_KIND_BLANK_ASK) ? !accepts_lines
                                                       : structure->element->blank_line(structure, parser, container));

    S_set_last_line_blank(container, last_line_blank);

    for (markdown_core_node *below = markdown_core_parser_builder_below(parser, container); below;
         below = markdown_core_parser_builder_below(parser, below)) {
        S_set_last_line_blank(below, false);
    }

    // A line that may be lazy, opened no block and is not blank is a lazy
    // line: the current block takes it.
    if (container == last_matched_container && !parser->at.blank && S_may_be_lazy(parser, last_matched_container)) {
        markdown_core_node *current = markdown_core_parser_current(parser);
        const markdown_core_element_instance *taker = markdown_core_parser_block_kind(parser, current)->structure;
        markdown_core_node *lazy = taker->element->open_lazy(taker, parser, current, input);
        if (!lazy) {
            return false;
        }
        assert(lazy == markdown_core_parser_current(parser));
        /* A lazy line is text, so its indentation is not content, as it is not
         * on a line that continues a paragraph with every prefix. */
        markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace - parser->at.offset, false);
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
        } else if (parser->at.blank) {
            // ??? do nothing
        } else if (kind->flags & MARKDOWN_CORE_KIND_PROSE) {
            markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace - parser->at.offset, false);
            markdown_core_block_add_line(container, input, parser);
        } else {
            const markdown_core_element_instance *text_block = parser->dialect->text_block_structure;
            container = text_block->element->open_text_block(text_block, parser, container, input);
            if (!container) {
                return false;
            }
            markdown_core_block_add_line(container, input, parser);
        }
        assert(container == markdown_core_parser_current(parser));
    }
    return true;
}

/* THE LINE A SESSION HOLDS. A commit more text may follow reads the lines
 * the text has finished and then, of the line it has not, each step whose
 * decision reads only bytes the line has
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.1): matching an open block, and opening a container, as far as their
 * elements say they read (element.h, HOW FAR A STEP READS). The line's last
 * byte is the end of the text or the last before its terminator, and a step
 * that reads it reads what more text can change, so the commit stops before
 * it, and before the text the line leaves to its leaf, and keeps where it
 * stopped (parser.h, markdown_core_line_stop). The next reading of the line
 * starts there: a snapshot's, which reads the rest of it as the line the
 * text ends with, and the next commit's, which reads on as far as the
 * line's bytes decide, or to the line's end once the text has finished
 * it. Every step of the line is taken once, in the commit that
 * decides it, as a parse of any longer text takes it. */

/* Keeps where the line machine stopped in the line a session holds: once
 * a commit, out of line as the driver's steps out of its line loop are
 * (THE DRIVER). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_hold(markdown_core_parser *parser, markdown_core_line_step step,
                                                       markdown_core_node *container, bool all_matched, bool opened,
                                                       S_opens opens) {
    parser->stream->stop = (markdown_core_line_stop){
        .container = container,
        .depth = opens.depth,
        .asks = parser->at.step_asks,
        .offset = parser->at.offset,
        .column = parser->at.column,
        .own = markdown_core_parser_visited_line(parser, parser->at.line_number)->own,
        .step = step,
        .all_matched = all_matched,
        .opened = opened,
        .maybe_lazy = opens.maybe_lazy,
        .partially_consumed_tab = parser->at.partially_consumed_tab,
    };
    parser->stream->held = true;
}

/* THE LINE MACHINE (http://spec.commonmark.org/0.24/#phase-1-block-structure)
 * reads the line in `buffer` from its start, or from `from`, where a commit
 * stopped in it; with `hold`, it stops before the first step that reads past
 * the line's bytes (THE LINE A SESSION HOLDS). */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_line_machine(markdown_core_parser *parser,
                                                                           const unsigned char *buffer, bufsize_t bytes,
                                                                           const markdown_core_line_stop *from,
                                                                           const bool hold) {
    markdown_core_node *last_matched_container;
    bool all_matched = true;
    markdown_core_node *container;
    markdown_core_chunk input;
    S_opens opens;
    bool held = false;

    if (parser->error || parser->root == NULL) {
        return;
    }

    assert(!parser->reading);
    assert(bytes >= 0);
    /* The shared input view excludes its physical terminator: the line is in
     * hand with a line feed and NUL after its content. */
    const int line = from ? parser->at.line_number : parser->at.line_number + 1;
    const bool resumed = S_line_in_hand(parser, line);
    if (!resumed && !S_line_take(parser, line, buffer, bytes)) {
        return;
    }
    parser->reading = true;

    /* What the first non-space byte tells is found again from the cursor: on
     * a line that grew, the end it found may have moved. */
    parser->at.offset = 0;
    parser->at.column = 0;
    parser->at.first_nonspace = 0;
    parser->at.first_nonspace_column = 0;
    parser->at.indent = 0;
    parser->at.blank = false;
    parser->at.partially_consumed_tab = false;

    input.data = parser->curline.ptr;
    input.len = parser->curline.size;
    input.alloc = 0;

    parser->at.asks = parser->at.step_asks = from ? from->asks : 0;
    if (from) {
        parser->at.offset = from->offset;
        parser->at.column = from->column;
        parser->at.partially_consumed_tab = from->partially_consumed_tab;
        markdown_core_parser_rewritten_line(parser, parser->at.line_number)->own = from->own;
    } else {
        parser->at.line_number = line;
    }
    /* Where a thematic break failed holds while the reading goes on: a
     * resumed visit reads on from where the last reading stopped. */
    if (!resumed || !from) {
        parser->at.thematic_break_kill_pos = 0;
    }

    if (!from || from->step == MARKDOWN_CORE_LINE_AT_MATCH) {
        last_matched_container =
            check_open_blocks(parser, &input, &all_matched,
                              from ? from->container : markdown_core_parser_block_root(parser), hold, &held);
        if (held) {
            S_hold(parser, MARKDOWN_CORE_LINE_AT_MATCH, last_matched_container, false, false, (S_opens){false, 0});
            goto finished;
        }
        if (!last_matched_container) {
            goto finished;
        }
        container = last_matched_container;
        opens = (S_opens){S_may_be_lazy(parser, container), 0};
    } else {
        /* A line that has opened a block is no lazy line: the block it
         * reaches is not the last it matched. */
        container = from->container;
        last_matched_container = from->opened ? NULL : container;
        all_matched = from->all_matched;
        opens = (S_opens){from->maybe_lazy, from->depth};
    }

    if (!from || from->step != MARKDOWN_CORE_LINE_AT_TEXT) {
        open_new_blocks(parser, &container, &input, all_matched, &opens, hold, &held);
        if (held) {
            S_hold(parser, MARKDOWN_CORE_LINE_AT_OPEN, container, all_matched, container != last_matched_container,
                   opens);
            goto finished;
        }
    }

    if (container == NULL || parser->error) {
        goto finished;
    }

    if (parser->claimed_cursor) {
        goto finished;
    }

    parser->at.step_asks = parser->at.asks;
    /* The text a held line leaves to its leaf is read to the line's end. */
    if (hold) {
        held = true;
        S_hold(parser, MARKDOWN_CORE_LINE_AT_TEXT, container, all_matched, container != last_matched_container, opens);
        goto finished;
    }

    /* The text opener asked for its call to be a candidate (THE
     * CANDIDATES): the line holds at the call, in the block it was asked
     * in, where the blocks the line did not match have closed. */
    if (!add_text_to_container(parser, container, last_matched_container, &input) && S_requested(parser)) {
        held = true;
        container = markdown_core_parser_current(parser);
        S_hold(parser, MARKDOWN_CORE_LINE_AT_TEXT, container, all_matched, container != last_matched_container, opens);
    }

finished:
    /* Block scopes cover the complete physical line, including closing
     * delimiters and attribute containers. Inline content trimming never
     * changes this source boundary. A held line has not ended. */
    if (!held) {
        parser->at.last_line_end = parser->at.line_end;
    }
    parser->reading = false;
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_process_line(markdown_core_parser *parser,
                                                                           const unsigned char *buffer,
                                                                           bufsize_t bytes) {
    S_line_machine(parser, buffer, bytes, NULL, false);
}

static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_process_held_line(markdown_core_parser *parser,
                                                                    const unsigned char *buffer, bufsize_t bytes,
                                                                    const markdown_core_line_stop *from, bool hold) {
    S_line_machine(parser, buffer, bytes, from, hold);
}

/* COMPLETION: every queued block, in the order it joined, as one walk over
 * the queue. A block is completed with its inline content and its field
 * roots, and rewritten in place; none is ever substituted. */
static void S_complete_queue(markdown_core_parser *parser) {
    if (!walk_owned_trees(parser)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

/* The blocks that have settled are completed, once the cells of the tables
 * that closed are read as blocks of their own (S_parse_block_inputs): the
 * document prepares the headings that settled (heading.c), and the queue is
 * completed. */
static void S_complete_settled(markdown_core_parser *parser) {
    const markdown_core_element_instance *document = parser->dialect->document_structure;
    document->element->prepare_document(document, parser);
    if (!parser->error) {
        S_complete_queue(parser);
    }
}

bool markdown_core_parser_complete(markdown_core_parser *parser) {
    if (!parser->error) {
        parser->snapshot = true;
        parser->stream->miss_root = NULL;
        S_parse_block_inputs(parser);
        if (!parser->error) {
            S_complete_settled(parser);
        }
        parser->snapshot = false;
    }
    return !parser->error;
}

/* Where `root` lies as the block phase left it, kept in `position`: its
 * place and a copy of its places, in storage of the pool's. */
static bool S_miss_position(markdown_core_parser *parser, const markdown_core_node *root,
                            markdown_core_miss_position *position) {
    markdown_core_places *places = NULL;
    if (root->source.places) {
        const size_t bytes =
            sizeof(markdown_core_places) + root->source.places->count * sizeof(markdown_core_source_place);
        places = markdown_core_node_pool_bytes(parser->pool, bytes);
        if (!places) {
            return false;
        }
        memcpy(places, root->source.places, bytes);
    }
    *position = (markdown_core_miss_position){root->place, places};
    return true;
}

static int S_take_field(markdown_core_node **slot, void *context) {
    markdown_core_node ***taken = context;
    assert(!*taken);
    *taken = slot;
    return 1;
}

/* The slot of the field root `block` holds, or NULL when it holds none: a
 * block holds one at most, its opener's or its caption
 * (markdown_core_block_settle_field). */
static markdown_core_node **S_field_slot(markdown_core_node *block) {
    markdown_core_node **slot = NULL;
    markdown_core_visit_inline_subtrees(block, S_take_field, &slot);
    return slot;
}

/* THE RECORD OF AN INLINE ROOT A LOOKUP FOUND NOTHING FOR (parser.h,
 * markdown_core_miss_root), made at its first such lookup: the block that is
 * the root, or that holds it, the block the queue links it to, and where the
 * root lies. Its completion has not changed where it lies, and nothing does
 * before the publish measures it; a block keeps its address for life. False
 * when it could not be made. */
static bool S_miss_root(markdown_core_parser *parser, markdown_core_node *root) {
    assert(S_kind_contains_inlines(markdown_core_parser_kind(parser, root), root));
    markdown_core_node *holder = markdown_core_scaffold_parent(root);
    markdown_core_node **slot = holder ? S_field_slot(holder) : NULL;
    const bool field = slot && *slot == root;
    markdown_core_miss_root *roots =
        markdown_core_reserve(parser->stream->miss_roots, &parser->stream->miss_root_capacity,
                              parser->stream->miss_root_count + 1, sizeof(*roots));
    if (!roots) {
        return false;
    }
    parser->stream->miss_roots = roots;
    markdown_core_miss_root *record = &roots[parser->stream->miss_root_count];
    *record = (markdown_core_miss_root){.node = field ? holder : root, .holder = holder, .field = field};
    if (!S_miss_position(parser, root, &record->position)) {
        return false;
    }
    parser->stream->miss_root_count++;
    parser->stream->miss_root = root;
    return true;
}

const markdown_core_registry_record *markdown_core_parser_find_at_snapshot(markdown_core_parser *parser,
                                                                           markdown_core_registry_kind kind,
                                                                           const markdown_core_chunk *label,
                                                                           const markdown_core_registry_record *record,
                                                                           bool *failed) {
    markdown_core_node *root = parser->completing;
    if (record && markdown_core_registry_starts_before(record, root->place.start)) {
        return record;
    }
    /* Only a committed root is asked again: a snapshot's own are gone with
     * it, but for the lookups of the open paragraph it settles, whose
     * settled nodes stay (THE SETTLE POINT). */
    if (!(root->flags & MARKDOWN_CORE_NODE__COMMITTED)) {
        if (root == parser->stream->settled.root && !S_settled_miss(&parser->stream->settled, kind, label)) {
            *failed = true;
        }
        return NULL;
    }
    *failed = !S_record_miss(parser, root, kind, label->data, label->len);
    return NULL;
}

/* A lookup of the committed root `root` that found nothing for `label`, of
 * `size` bytes, recorded for the finalize. False when storage could not be
 * had. */
static bool S_record_miss(markdown_core_parser *parser, markdown_core_node *root, markdown_core_registry_kind kind,
                          const unsigned char *label, bufsize_t size) {
    markdown_core_definition_miss *misses = markdown_core_reserve(
        parser->stream->misses, &parser->stream->miss_capacity, parser->stream->miss_count + 1, sizeof(*misses));
    if (misses) {
        parser->stream->misses = misses;
    }
    if (!misses || (parser->stream->miss_root != root && !S_miss_root(parser, root))) {
        return false;
    }
    misses[parser->stream->miss_count++] = (markdown_core_definition_miss){parser->stream->miss_root_count - 1, kind,
                                                                           parser->stream->miss_labels.size, size};
    markdown_core_strbuf_put(&parser->stream->miss_labels, label, size);
    return !parser->stream->miss_labels.oom;
}

/* The nodes of what completion made of a root, gathered as their anchors
 * are withdrawn. */
typedef struct {
    markdown_core_parser *parser;
    size_t count;
} completion_collect;

static int S_collect_completed(markdown_core_node **slot, void *context) {
    completion_collect *collect = context;
    if (!*slot) {
        return 1;
    }
    markdown_core_node **stack =
        markdown_core_parser_walk_stack(collect->parser, collect->count + 1, sizeof(markdown_core_node *));
    if (!stack) {
        return 0;
    }
    stack[collect->count++] = *slot;
    return 1;
}

/* The explicit anchor `node` reserved, withdrawn as starting at `start`. */
static bool S_withdraw_anchor(markdown_core_parser *parser, const markdown_core_node *node, uint32_t start) {
    const markdown_core_chunk *anchor = &node->attributes.anchor;
    return !anchor->len || (node->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED) ||
           markdown_core_registry_withdraw(&parser->registry, MARKDOWN_CORE_REGISTRY_ANCHOR, anchor->data, anchor->len,
                                           start);
}

/* THE ANCHORS COMPLETING A ROOT RESERVED, withdrawn as starting at `start`,
 * where the root starts: the root's own and those of every node of its
 * inline content, the field roots of its inline nodes included. */
static void S_withdraw_completion(markdown_core_parser *parser, markdown_core_node *root, uint32_t start) {
    completion_collect collect = {.parser = parser};
    bool withdrawn = S_withdraw_anchor(parser, root, start);
    for (uint32_t i = 0; withdrawn && i < root->child_count; i++) {
        withdrawn = S_collect_completed(&root->children[i], &collect);
    }
    while (withdrawn && collect.count) {
        markdown_core_node *node = ((markdown_core_node **)parser->walk_stack)[--collect.count];
        withdrawn = S_withdraw_anchor(parser, node, start) &&
                    markdown_core_visit_inline_subtrees(node, S_collect_completed, &collect);
        for (uint32_t i = 0; withdrawn && i < node->child_count; i++) {
            withdrawn = S_collect_completed(&node->children[i], &collect);
        }
    }
    if (!withdrawn) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

/* A root completed anew: its candidate, and the record of the root. */
typedef struct {
    markdown_core_node *candidate;
    const markdown_core_miss_root *record;
} S_completion_anew;

/* The root `record` names. */
static markdown_core_node *S_record_root(const markdown_core_miss_root *record) {
    return record->field ? *S_field_slot(record->node) : record->node;
}

/* The candidate of the root `record` names (markdown_core_node_candidate),
 * at the position the record kept, joins the completion queue with the block
 * the root settled into or the block that holds it, once the anchors the
 * root's completion reserved are withdrawn; the element of a kind whose
 * inline content is parsed before the walk parses it first. NULL, with the
 * parse failed, when no candidate could be made. */
static markdown_core_node *S_complete_anew(markdown_core_parser *parser, markdown_core_miss_root *record) {
    markdown_core_node *root = S_record_root(record);
    S_withdraw_completion(parser, root, record->position.place.start);
    const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, root);
    markdown_core_node *candidate = parser->error ? NULL : markdown_core_node_candidate(parser->pool, root);
    if (!candidate) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    candidate->place = record->position.place;
    candidate->source.places = record->position.places;
    record->position.places = NULL;
    if (kind->flags & MARKDOWN_CORE_KIND_DEFERRED) {
        parser->completing = candidate;
        kind->structure->element->reparse_inlines(kind->structure, parser, candidate);
    }
    S_complete_later(parser, candidate, record->holder);
    return candidate;
}

/* THE DEFINITIONS A ROOT COMPLETED AGAIN MAKES AGAIN. The candidate makes
 * the root's inline content anew. The definitions the published document
 * holds as COMMITTED in that content (parser.h, `committed`) lie in its
 * source, at the place the root's record kept, and go as the candidate is
 * published, which lists the ones it makes there; the root's own entry
 * stays, naming the object that holds its state. A table finds the ones a
 * place holds by where they start, and each one going is emptied; what the
 * candidates list joins the tables once every candidate is placed
 * (S_keep_defined), so no candidate empties another's. */
static void S_withdraw_defined(markdown_core_parser *parser, const markdown_core_miss_root *record,
                               const markdown_core_node *root) {
    const markdown_core_place place = record->position.place;
    const markdown_core_key_entry from = markdown_core_key_number(place.start);
    for (size_t table = 0; table < MARKDOWN_CORE_DEFINITION_TABLES; table++) {
        markdown_core_key_tree_cursor cursor;
        markdown_core_key_tree_seek(&parser->stream->committed.starts[table], &from, &cursor);
        for (markdown_core_key_entry *entry;
             (entry = markdown_core_key_tree_next(&cursor)) && entry->extra < place.end;) {
            if (entry->value && entry->value != root) {
                entry->value = NULL;
            }
        }
    }
}

/* The definitions the candidates listed, in the committed tables: each at
 * the entry of where it starts, which one emptied may already hold. */
static void S_keep_defined(markdown_core_parser *parser) {
    for (size_t i = 0; i < parser->stream->defined_count; i++) {
        const markdown_core_defined *defined = &parser->stream->defined[i];
        const markdown_core_key_entry key = markdown_core_key_number(defined->start);
        bool made;
        markdown_core_key_entry *entry =
            markdown_core_key_tree_entry(&parser->stream->committed.starts[defined->table], &key, &made);
        if (!entry) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        entry->extra = defined->start;
        entry->value = defined->node;
    }
    parser->stream->defined_count = 0;
}

/* A candidate completed takes its root's place. A root a snapshot
 * published complete is in the published document as a publish left it,
 * which no later publish walks again (ast.c, THE FINAL PREFIX): the
 * candidate is published against it (the document's publish_candidate),
 * its nodes matched to the root's. Any other root the finalize's publish
 * walks, the candidate with it. A block root takes the candidate's state in
 * its own slot, with its id, and the candidate's slot its old state; a field
 * root's block holds the candidate in the root's field. The storage the
 * candidate borrowed from the root's is the candidate's
 * (markdown_core_node_take_borrowed), and the root's old state goes. */
static void S_complete_place(markdown_core_parser *parser, const S_completion_anew *anew, bool published) {
    markdown_core_node *candidate = anew->candidate;
    const markdown_core_miss_root *record = anew->record;
    markdown_core_node *root = S_record_root(record);
    S_withdraw_defined(parser, record, root);
    if (published) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->publish_candidate(document, parser, candidate, root, record->node->place.start);
    }
    if (parser->error) {
        markdown_core_node_pool_release(parser->pool, candidate);
        return;
    }
    if (record->field) {
        *S_field_slot(record->node) = candidate;
        markdown_core_node_take_borrowed(candidate, root);
        markdown_core_node_pool_release(parser->pool, root);
        return;
    }
    const uint64_t id = root->id;
    markdown_core_node_swap(root, candidate);
    root->id = id;
    markdown_core_node_take_borrowed(root, candidate);
    markdown_core_node_pool_release(parser->pool, candidate);
}

/* A LOOKUP THAT FOUND NOTHING at a commit's completion is asked anew as
 * the finalize ends the completion, with every definition visible (plan
 * 5.10): a root one of whose lookups finds a definition now is completed
 * anew as a candidate, what the block phase made of it, at the position its
 * record kept, with every block the finalize completes. The candidate then
 * takes the root's place (S_complete_place). */
static void S_complete_misses(markdown_core_parser *parser) {
    S_completion_anew *anew = markdown_core_alloc(parser->stream->miss_root_count, sizeof(*anew));
    if (!anew) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    size_t count = 0;
    for (size_t miss = 0, record = 0; record < parser->stream->miss_root_count && !parser->error; record++) {
        bool found = false;
        for (; miss < parser->stream->miss_count && parser->stream->misses[miss].root == record; miss++) {
            const markdown_core_definition_miss *at = &parser->stream->misses[miss];
            const markdown_core_chunk label = {parser->stream->miss_labels.ptr + at->label_start, at->label_length, 0};
            bool failed = false;
            found = found || markdown_core_parser_find_definition(parser, at->kind, &label, &failed);
            if (failed) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                break;
            }
        }
        markdown_core_node *candidate =
            found && !parser->error ? S_complete_anew(parser, &parser->stream->miss_roots[record]) : NULL;
        if (candidate) {
            anew[count++] = (S_completion_anew){candidate, &parser->stream->miss_roots[record]};
        }
    }
    if (!parser->error && parser->completion_first) {
        S_complete_queue(parser);
    }
    size_t placed = 0;
    for (; placed < count && !parser->error; placed++) {
        S_complete_place(parser, &anew[placed],
                         (size_t)(anew[placed].record - parser->stream->miss_roots) < parser->stream->miss_published);
    }
    /* A candidate a failure left unplaced is no root's. */
    for (; placed < count; placed++) {
        markdown_core_node_pool_release(parser->pool, anew[placed].candidate);
    }
    markdown_core_free(anew);
    if (!parser->error && count) {
        S_keep_defined(parser);
    }
}

uint64_t markdown_core_source_key(const void *entry) {
    markdown_core_node *node;
    memcpy(&node, entry, sizeof(node));
    return node->place.start;
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
 * A session's commits completed the blocks that settled before them
 * (markdown_core_parser_complete). The stage completes the rest, and a
 * finalize then asks again each lookup that found nothing at a commit (A
 * LOOKUP THAT FOUND NOTHING) before the document's finalization.
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
    S_take_back(parser, false);

    S_close_document(parser);
    if (!parser->error) {
        S_complete_settled(parser);
    }
    if (!parser->error && !parser->snapshot && parser->stream->miss_count) {
        S_complete_misses(parser);
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
    S_release_walk(parser);
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

int markdown_core_parser_get_line_number(markdown_core_parser *parser) { return parser->at.line_number; }

bufsize_t markdown_core_parser_get_offset(markdown_core_parser *parser) { return parser->at.offset; }

bufsize_t markdown_core_parser_get_column(markdown_core_parser *parser) { return parser->at.column; }

int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser) { return parser->at.first_nonspace; }

int markdown_core_parser_get_first_nonspace_column(markdown_core_parser *parser) {
    return parser->at.first_nonspace_column;
}

int markdown_core_parser_get_indent(markdown_core_parser *parser) { return parser->at.indent; }

int markdown_core_parser_is_blank(markdown_core_parser *parser) { return parser->at.blank; }

int markdown_core_parser_has_partially_consumed_tab(markdown_core_parser *parser) {
    return parser->at.partially_consumed_tab;
}

bufsize_t markdown_core_parser_get_last_line_end(markdown_core_parser *parser) { return parser->at.last_line_end; }

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    markdown_core_block_advance_offset(parser, &input_chunk, count, columns != 0);
}
