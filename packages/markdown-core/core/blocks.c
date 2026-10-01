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
#include "references.h"
#include "utf8.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"
#include "iterator.h"

#define TAB_STOP 4

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

#define peek_at(i, n) (i)->data[n]

/* A block's last line was blank: the line machine's own record, or, once the
 * finish stage has asked whether the block ends with a blank line, that
 * answer. */
bool markdown_core_block_last_line_blank(const markdown_core_node *node) {
    unsigned bit = node->flags & MARKDOWN_CORE_NODE__LAST_LINE_CHECKED ? MARKDOWN_CORE_NODE__ENDS_BLANK
                                                                       : MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    return (node->flags & bit) != 0;
}

static bool S_last_line_checked(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_CHECKED) != 0;
}

markdown_core_node_type markdown_core_block_type(const markdown_core_node *node) {
    return (markdown_core_node_type)node->kind;
}

static void S_set_ends_blank(markdown_core_node *node, bool blank) {
    if (blank) {
        node->flags |= MARKDOWN_CORE_NODE__ENDS_BLANK;
    } else {
        node->flags &= ~MARKDOWN_CORE_NODE__ENDS_BLANK;
    }
}

/* Every line writes the flag on the blocks it reaches, and seldom changes
 * it: only a change is a write (markdown_core_parser_set_flags). */
static inline void S_set_last_line_blank(markdown_core_parser *parser, markdown_core_node *node,
                                         bool markdown_core_block_is_blank) {
    if (((node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0) != markdown_core_block_is_blank) {
        markdown_core_parser_set_flags(parser, node, (uint16_t)(node->flags ^ MARKDOWN_CORE_NODE__LAST_LINE_BLANK));
    }
}

static void S_set_last_line_checked(markdown_core_node *node) { node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_CHECKED; }

static void S_input_open(markdown_core_parser *parser, const unsigned char *bytes, size_t length);
static bool S_input_open_text(markdown_core_parser *parser, const markdown_core_text_tree *text, size_t start);
static bool S_restart(markdown_core_parser *parser);
static void S_log_free(markdown_core_ledger_log *log);
static void S_parse_source(markdown_core_parser *parser);
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_process_line(markdown_core_parser *parser,
                                                                           const unsigned char *buffer,
                                                                           bufsize_t bytes);
static bool S_last_child_is_open(markdown_core_node *container);
static markdown_core_node *S_finish_parse(markdown_core_parser *parser);
static inline bool S_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
static inline int S_append_input_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t line_start, int column, bufsize_t length, bufsize_t offset);

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

/* The storage the active input owns: its normalized lines and the window's
 * buffer. */
static void S_input_release(markdown_core_parser *parser) {
    while (parser->normalized_lines) {
        markdown_core_normalized_line *line = parser->normalized_lines;
        parser->normalized_lines = line->next;
        markdown_core_free(line);
    }
    markdown_core_free(parser->input_buffer);
    parser->input_buffer = NULL;
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
    markdown_core_source_order_dispose(&parser->source_order);
    markdown_core_free(parser->walk_stack);
    markdown_core_free(parser->block_inputs);
    S_input_release(parser);
    markdown_core_free(parser->input_lines);
    markdown_core_free(parser->input_facts);
    /* What the parse took and did not commit goes, and a parse that failed
     * leaves the session no checkpoints: its next edit re-reads the text. */
    markdown_core_checkpoints *store = parser->revision ? parser->revision->checkpoints : NULL;
    if (store) {
        for (markdown_core_checkpoint *taken = parser->taken, *next; taken; taken = next) {
            next = markdown_core_checkpoint_of(taken->link.up);
            markdown_core_checkpoint_free(store, taken);
        }
        markdown_core_frame_drop(store, parser->spine);
        for (markdown_core_entry *entry = parser->entered, *next; entry; entry = next) {
            next = markdown_core_entry_of(entry->link.up);
            markdown_core_entry_drop(store, entry);
        }
        if (parser->error) {
            markdown_core_checkpoints_clear(store);
        }
    }
    markdown_core_free(parser->line_frames);
    S_log_free(&parser->line_records);
    S_log_free(&parser->undo_records);
    markdown_core_free(parser->cuts);
    markdown_core_free(parser->shifts);
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

    /* The block-start lookahead's chain and resume cache are parser state of
     * the same kind: indexed by open containers and source lines, owned by no
     * node, and dead with the parse. */
    markdown_core_free(parser->lookahead_chain);
    markdown_core_free(parser->lookahead_chain_flags);
    parser->lookahead_chain = NULL;
    parser->lookahead_chain_flags = NULL;
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

/* Begins a transaction that continues `revision` and borrows its pool. */
static void S_parse_begin(markdown_core_parser *parser, markdown_core_revision *revision) {
    parser->revision = revision;
    parser->pool = revision->pool;
    /* The parse owns the old tree: it holds it apart until it reopens it. */
    parser->old_root = revision->previous;
    parser->rejoin_from = SIZE_MAX;
    parser->next_restart = SIZE_MAX;
    markdown_core_strbuf_init(&parser->curline, 256);
    markdown_core_strbuf_init(&parser->lookahead_last_line, 0);
    /* The line index is a parse-owned workspace, like curline. Establish its
     * initial capacity before any input; inputs reset length, never
     * ownership. */
    parser->input_lines = markdown_core_reserve(NULL, &parser->input_line_capacity, 1, sizeof(*parser->input_lines));

    /* A parse keeps a ledger for its session's checkpoints, when it has a
     * store; a global pass reads the whole finished tree, so a dialect that
     * declares one restarts at the document's start and keeps none. */
    parser->line_state = revision->checkpoints && !parser->dialect->passes_declared ? MARKDOWN_CORE_LINE_PLAIN
                                                                                    : MARKDOWN_CORE_LINE_UNKEPT;

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. Only a
     * complete transaction begins the document lifecycle -- its owner is
     * whichever element the dialect resolved it to, and it never sees a
     * failed transaction. Disposal does not depend on it having begun: the
     * lifecycle's own allocations can fail halfway, so its release already
     * takes whatever state it finds. */
    if (!parser->input_lines || parser->curline.oom || parser->lookahead_last_line.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    if (revision->previous) {
        if (!S_restart(parser)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
    } else {
        parser->root = parser->block_root = parser->current = make_document(parser);
        parser->applied = revision->edit_count;
    }
    if (!parser->root || parser->root->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    } else {
        const markdown_core_element_instance *document_structure = parser->dialect->document_structure;
        document_structure->element->init_document(document_structure, parser);
    }
}

/* Ends the transaction: its parse state is released, and the parser and the
 * records are as the instance was made, for the next. */
static void S_parse_end(markdown_core_parser *parser) {
    markdown_core_instance *instance = (markdown_core_instance *)parser;
    S_parser_dispose(parser);
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->lookahead_last_line);
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

/* "This block ends on the line being processed", lifted out of `markdown_core_block_finalize` so
 * that the element close path can say the same thing. The three kinds that
 * take it there — the document, a closed fenced code block, a setext heading —
 * are the ones whose last line IS the line in hand; every other block ended on
 * the line before. An element container closing on its own fence is a fourth,
 * and `markdown_core_block_finalize` cannot know that from the type alone. */
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
static bool S_kind_contains_inlines(const markdown_core_kind_record *kind, markdown_core_node *node) {
    return (kind->flags & MARKDOWN_CORE_KIND_INLINES) ||
           ((kind->flags & MARKDOWN_CORE_KIND_INLINES_ASK) &&
            kind->structure->element->contains_inlines_func(kind->structure->element, node));
}

// Returns true if the line at `offset` has only spaces and tabs, else false.
bool markdown_core_block_is_blank(markdown_core_strbuf *s, bufsize_t offset) {
    return markdown_core_is_blank_to_line_end(s->ptr, offset, s->size);
}

/* Whether the line may be LAZY: it did not match the prefix of the current
 * block, `matched` being the deepest block whose prefix it did match, and the
 * current block takes such a line as text (`accepts_lazy`). A paragraph takes
 * one; so does a callout on the line after its marker line, whose body the
 * line then starts. The line is lazy if it opens no block, and the starts
 * that refuse a lazy line (`block_start_context`) are no block start on it. */
static bool S_may_be_lazy(markdown_core_parser *parser, const markdown_core_node *matched) {
    if (parser->current == matched) {
        return false;
    }
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, parser->current);
    return structure && structure->element->accepts_lazy &&
           structure->element->accepts_lazy(structure, parser, parser->current);
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

static void S_record_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t column,
                                  bufsize_t length) {
    S_append_input_marks(parser, node, parser->line_number, parser->line_start, column, length, node->content.size);
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
    if (!parser->input_facts[geometry->facts - 1].nul_count) {
        return NULL;
    }
    bufsize_t length;
    if (!S_input_line_content(parser, geometry, &length)) {
        return NULL;
    }
    return parser->input_facts[geometry->facts - 1].normalized;
}

/* Only a document line with facts can carry NUL. */
static inline const markdown_core_normalized_line *S_nul_view(markdown_core_parser *parser,
                                                              markdown_core_input_line *geometry) {
    assert(parser->block_root == parser->root);
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
    if (parser->block_root == parser->root) {
        const markdown_core_normalized_line *view = S_nul_view(parser, geometry);
        bool nul;
        return view ? S_denormalized_offset(view, geometry->start, column - 1, &nul)
                    : (bufsize_t)geometry->start + column - 1;
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_place(parser, &parser->block_root->content_map,
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
static bufsize_t S_mapped_line_end(markdown_core_parser *parser, const markdown_core_input_line *line) {
    const markdown_core_content_map *map = &parser->block_root->content_map;
    /* A cell's content is in the window whole, from offset 0. */
    const unsigned char *content = markdown_core_parser_input_at(parser, 0);
    bufsize_t at = (bufsize_t)line->start;
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
    if (parser->block_root == parser->root) {
        if (!column) {
            return (bufsize_t)geometry->start;
        }
        return markdown_core_parser_mapped_source_offset(parser, line, column) + 1;
    }
    if (!column) {
        return S_mapped_line_end(parser, geometry);
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_end_place(parser, &parser->block_root->content_map,
                                                (bufsize_t)geometry->start + column - 1, &ignored, &source)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

/* Where byte `at` of the active input was written in the source. */
static inline bufsize_t S_input_source(markdown_core_parser *parser, bufsize_t at) {
    if (parser->block_root == parser->root) {
        return at;
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_place(parser, &parser->block_root->content_map, at, &ignored, &source)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

static inline bufsize_t S_line_offset(markdown_core_parser *parser, int line) {
    return S_input_source(parser, (bufsize_t)markdown_core_parser_visited_line(parser, line)->start);
}

static inline bool S_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line) {
    if (line < parser->input_first_line || line > parser->line_number) {
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
    if (!parser->input_mapped) {
        return S_append_copied_mark(parser, node, line, line_start + column - 1, offset);
    }
    if (parser->block_root == parser->root) {
        return S_append_nul_line_marks(parser, node, line, line_start, column, length, offset);
    }
    return markdown_core_parser_append_content_marks(parser, &parser->block_root->content_map, &node->content_map,
                                                     line_start + column - 1, length, offset);
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

bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_node *owner) {
    if (!owner->content.size) {
        return true;
    }
    assert(owner->content_map.count && owner->parent);
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
    parser->block_inputs[parser->block_input_count++] = owner;
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

/* Drop `dropped` bytes off the FRONT of `node`'s content, leaving `remaining`
 * bytes, and keep the map describing what is left. The marks stay where they are in the vector: the
 * run's head moves past the slices that went away, and the slice the cut
 * landed inside keeps its line with its column advanced to the cut. */
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
                                              bufsize_t remaining) {
    if (node->content_map.count <= 0 || dropped <= 0) {
        return;
    }
    if (remaining <= 0) {
        node->content_map.count = 0;
        node->content_map.offset = 0;
        return;
    }
    markdown_core_parser_adopt_content_marks(parser, &node->content_map, &node->content_map, dropped, remaining);
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
bool markdown_core_block_ends_with_blank_line(const markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_node *last = node;
    while (!S_last_line_checked(last) &&
           (markdown_core_parser_kind(parser, last)->flags & MARKDOWN_CORE_KIND_BLANK_PROPAGATES) && last->last_child) {
        last = last->last_child;
    }
    bool blank = markdown_core_block_last_line_blank(last);
    /* Cache the answer as well as the fact that it was checked. Both list
     * finalization and detached identifiers ask this of finalized blocks. */
    for (;;) {
        S_set_ends_blank(last, blank);
        S_set_last_line_checked(last);
        if (last == node) {
            return blank;
        }
        last = last->parent;
    }
}

/* A container's children folded by its element: the sum of every summary
 * but the last child's, and the last child's. */
static void S_fold_children(markdown_core_parser *parser, markdown_core_node *node, uint32_t *sum, uint32_t *last) {
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
    uint32_t (*const fold_child)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *,
                                 markdown_core_node *, bool) = structure->element->fold_child;
    uint32_t total = 0, end = 0;
    for (markdown_core_node *child = node->first_child; child; child = child->next) {
        if (child->next) {
            total += fold_child(structure, parser, node, child, false);
        } else {
            end = fold_child(structure, parser, node, child, true);
        }
    }
    *sum = total;
    *last = end;
}

void markdown_core_parser_fold_totals(markdown_core_parser *parser, markdown_core_node *node, uint32_t *sum,
                                      uint32_t *last) {
    if (node->entry) {
        *sum = node->entry->sum;
        *last = node->entry->last;
        return;
    }
    S_fold_children(parser, node, sum, last);
}

void markdown_core_parser_fold(markdown_core_parser *parser, markdown_core_node *node) {
    uint32_t sum, last;
    S_fold_children(parser, node, &sum, &last);
    if (node->entry) {
        node->entry->sum = sum;
        node->entry->last = last;
    }
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
    if (structure->element->fold_apply) {
        structure->element->fold_apply(structure, parser, node, sum, last);
    }
}

markdown_core_node *markdown_core_block_finalize(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_node *parent;

    parent = b->parent;
    assert(b->flags & MARKDOWN_CORE_NODE__OPEN); // shouldn't call markdown_core_block_finalize on closed blocks
    b->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    /* A leaf a restart reopened closes as the old parse closed it. */
    if (b == parser->settled) {
        parser->settled = NULL;
        return parent;
    }

    if (parser->curline.size == 0) {
        // end of input - line number has not been incremented
        b->where.place.end = (uint32_t)parser->last_line_end;
    } else if (markdown_core_block_type(b) == MARKDOWN_CORE_NODE_DOCUMENT ||
               /* D35: a block finalized on the line it OPENED did not end on
                * the previous one. `line_number - 1` below assumes the block
                * was closed by a later line, which is true of every block that
                * needs a following line to end it -- and false of an HTML block
                * of type 2 to 5, whose terminator can be on its own first line.
                * Measured: `<!-- c -->` alone on line 3 gave
                * `HTMLBlock scope=3:1..2:0` for a literal whose last byte is at
                * 3:10, and the last line's end there was the end of the BLANK
                * line before it. Four of the eleven observed negative rows
                * were this. */
               S_starts_on_line(parser, b, parser->line_number) ||
               /* M0: the same block closed by its end condition on a LATER
                * line. The terminator line is the block's last line and is
                * the line being processed, so the block ends here too:
                * `<!--\nmulti\n-->` gave `HTMLBlock scope=1:1..2:5`, one line
                * short of the `-->` its literal holds, and `<pre>\nx\n</pre>`
                * the same shape. Only the last two ways a block can close --
                * the input ending and a container closing under it -- still
                * end it on the line before. */
               (b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0) {
        markdown_core_block_set_end_to_current_line(parser, b);
    } else {
        b->where.place.end = (uint32_t)parser->last_line_end;
    }

    /* The element's one chance to read its own block as a finished thing.
     * Placed after the scope is settled and before the switch, because what a
     * close hook has to say is about the whole block. */

    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, b);
    if (structure && structure->element->finalize_block) {
        structure->element->finalize_block(structure, parser, b);
    }

    return parent;
}

/* Finalize to the container that will own the next block-level construct,
 * including a detached identifier, which contributes no child node. */
/* A source-owning block candidate commits only after the prior open path has
 * closed at this line's matched boundary. This is also the ordinary text path's
 * transition; caption attachment therefore cannot strand an open preceding table. */
void markdown_core_parser_finalize_unmatched_blocks(markdown_core_parser *parser) {
    while (parser->current != parser->matched_container && !parser->error) {
        parser->current = markdown_core_block_finalize(parser, parser->current);
        assert(parser->current);
    }
}

markdown_core_node *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type) {
    assert(parent);

    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!markdown_core_node_can_contain_type(parent, block_type)) {
        if (!parent->parent) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED);
            return NULL;
        }
        parent = markdown_core_block_finalize(parser, parent);
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
    markdown_core_node *child =
        make_block(parser, block_type, markdown_core_parser_source_offset(parser, parser->line_number, start_column));
    if (!child || child->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        if (child) {
            markdown_core_parser_release_node(parser, child);
        }
        /* The loop above may have finalized blocks; keep the parser anchored
         * at a still-open ancestor so the finish path stays consistent. */
        parser->current = parent;
        return NULL;
    }
    if (parser->line_state == MARKDOWN_CORE_LINE_PLAIN) {
        markdown_core_parser_line_opened(parser);
    }
    /* block_parent_for already established containment. Commit that decision
     * without re-entering a possibly stateful containment predicate. */
    markdown_core_node_attach_validated(parent, child, NULL);
    return child;
}

/* THE INLINE PARSER'S OWN FIELD PARSE. A token that owns fields -- a
 * citation's affixes, a directive's label -- has them parsed by the parse
 * that made it, before it scans on (complete_inline_token in inlines.c), so
 * that the token can be closed on what the fields turned out to hold. That
 * parse walks the field's small tree with an iterator of its own; the
 * document's content tree is not parsed this way but at each container's
 * ENTER of the one finish walk (walk_owned_trees), which is why the walk does
 * not parse below a node it reached inside an inline tree. Inline parsing
 * completes fields at their owning token; the structural walk here therefore
 * skips the emitted inline tree. */
static bool process_inline_tree(markdown_core_parser *parser, markdown_core_node *root, markdown_core_map *refmap) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;
    bool whitespace = false;

    if (!iter) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }

    while (!parser->error && (ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, cur);
            if (S_kind_contains_inlines(kind, cur)) {
                if (!(kind->flags & MARKDOWN_CORE_KIND_DEFERRED)) {
                    whitespace |= markdown_core_parse_inlines(parser, cur, refmap);
                }
                markdown_core_iter_reset(iter, cur, MARKDOWN_CORE_EVENT_EXIT);
            }
            whitespace |= markdown_core_parse_inline_subtrees(parser, cur, refmap);
        }
    }

    markdown_core_iter_free(iter);
    return whitespace;
}

/* Core and element fields participate in the same parser phases. The
 * private title root stays owned here from source capture through cleanup. */

typedef struct {
    markdown_core_parser *parser;
    markdown_core_map *refmap;
    bool whitespace;
} inline_parse_context;

static int parse_inline_field(markdown_core_node **root_slot, void *context) {
    inline_parse_context *fields = context;
    if (root_slot && *root_slot && !fields->parser->error) {
        fields->whitespace |= process_inline_tree(fields->parser, *root_slot, fields->refmap);
    }
    return !fields->parser->error;
}

bool markdown_core_parse_inline_subtrees(markdown_core_parser *parser, markdown_core_node *node,
                                         markdown_core_map *refmap) {
    inline_parse_context context = {parser, refmap, false};
    if (!markdown_core_visit_inline_subtrees(node, parse_inline_field, &context)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return context.whitespace;
}

typedef int (*tree_phase_func)(markdown_core_parser *parser, markdown_core_node *root, void *context);

/* ONE FRAME PER ROOT THE WALK IS INSIDE. The iterator is the frame's own, not
 * an allocation: the walk steps it in place (iterator.h).
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
 * parsed, and the walk is where the old inline traversal skipped to the
 * container's EXIT. */
typedef struct {
    markdown_core_node *root;
    /* The root when it is one of the walk's own roots, which finish steps
     * see as such and `finish` completes; NULL for a block of a tree the
     * walk finishes in part (a restart's new blocks). */
    markdown_core_node *top;
    markdown_core_iter iter;
    markdown_core_node *parsed;
    int script_depth;
    bool parses, started;
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

static int push_owned_root(markdown_core_node *root, bool rooted, owned_tree_walk *walk) {
    if (!root || walk->parser->error) {
        return !walk->parser->error;
    }
    if (walk->count == walk->capacity) {
        size_t capacity = walk->capacity ? 2 * walk->capacity : 8;
        if (capacity > SIZE_MAX / sizeof(*walk->frames) ||
            (walk->slots && capacity > SIZE_MAX / sizeof(*walk->states) / walk->slots)) {
            markdown_core_parser_fail(walk->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return 0;
        }
        void *frames = markdown_core_parser_walk_stack(walk->parser, capacity, sizeof(*walk->frames));
        if (!frames) {
            markdown_core_parser_fail(walk->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return 0;
        }
        walk->frames = frames;
        if (walk->slots) {
            void *states = markdown_core_realloc(walk->states, capacity * walk->slots * sizeof(*walk->states));
            if (!states) {
                markdown_core_parser_fail(walk->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return 0;
            }
            walk->states = states;
        }
        walk->capacity = capacity;
    }
    if (walk->slots) {
        memset(walk->states + walk->count * walk->slots, 0, walk->slots * sizeof(*walk->states));
    }
    /* Word depth counts the inline word bodies around a node, and a block
     * begins content of its own: a root that is a block, such as an inline
     * note's Footnote, starts outside every word body its owner is in. */
    bool block = ((unsigned)root->kind & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK;
    walk->frames[walk->count++] = (owned_tree_frame){.root = root,
                                                     .top = rooted ? root : NULL,
                                                     .parsed = NULL,
                                                     .script_depth = block ? 0 : walk->script_depth,
                                                     .parses = walk->parses,
                                                     .started = false};
    return 1;
}

/* INLINE COMPLETION IS THE FINISH WALK'S ENTER. A node is completed once, the
 * first time the finish stage reaches it: the element's `complete_inline`
 * (text.c turns an escaped space into NBSP inside a word) and the document's
 * `observe_inline` (a node's explicit anchor is reserved before the headings
 * are given theirs). Both read the node and its ancestors' word depth and
 * nothing else, so the ENTER of the one walk is where they belong. The walk
 * reads the two hooks through its per-kind record and the document's hook
 * it holds in a register; this form is for the sibling consolidation absorbs
 * at a Text's EXIT, whose ENTER is stepped over, so consolidation completes
 * it first (iterator.h). */
static inline void complete_inline_from_plan(markdown_core_parser *parser, markdown_core_node *node,
                                             const markdown_core_kind_record *plan, int script_depth,
                                             const markdown_core_element_instance *document,
                                             void (*observe)(const markdown_core_element_instance *,
                                                             markdown_core_parser *, markdown_core_node *)) {
    if (plan->complete) {
        plan->complete(plan->structure, parser, node, script_depth);
    }
    if (observe) {
        observe(document, parser, node);
    }
}

static void complete_consolidated_text(markdown_core_parser *parser, markdown_core_node *node, int script_depth) {
    const markdown_core_element_instance *document = parser->dialect->document_structure;
    assert(node->kind == MARKDOWN_CORE_NODE_TEXT);
    complete_inline_from_plan(parser, node, &parser->dialect->kinds[MARKDOWN_CORE_FINISH_TEXT_INDEX], script_depth,
                              document, document->element->observe_inline);
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
                                                    int is_root, void **states) {
    markdown_core_finish_result result = MARKDOWN_CORE_FINISH_CONTINUE;
    for (; entry->instance; entry++) {
        if (!markdown_core_finish_step_admitted(entry, parser)) {
            continue;
        }
        result =
            entry->instance->element->finish_step(entry->instance, parser, node, event, is_root, &states[entry->slot]);
        if (result != MARKDOWN_CORE_FINISH_CONTINUE) {
            /* A node is consumed only at its EXIT: at ENTER the lookahead
             * names its first child, and a step that freed it here would
             * have broken the LOCAL contract the API header states. And the
             * node it consumed owned no field roots, which the walk pushed at
             * its ENTER and may have recorded for the passes: the contract
             * again, and the in-tree steps consume Text, Paragraph and
             * CodeBlock, none of which owns one. */
            assert(result == MARKDOWN_CORE_FINISH_FAILED || event == MARKDOWN_CORE_EVENT_EXIT);
            break;
        }
    }
    return result;
}

/* THE FINISH STAGE'S TRAVERSAL COUNT (parser.h): the walk's own events are
 * kept in three registers and added to the parser's totals at each root's
 * completion -- before that root's global passes, which read the totals so
 * far -- and when the walk ends, so that counting an event costs an increment
 * and not a read-modify-write of the parser. The events consolidation takes
 * over the siblings it absorbs are counted by that step itself, on the parser
 * (iterator.c, S_count_step): the step is shared with the public entry point,
 * which has no walk and no registers, and both paths add to the same totals. */
typedef struct {
    size_t events, entered, roots;
} finish_count;

static void flush_finish_count(markdown_core_parser *parser, finish_count *count) {
    parser->finish_walk_events += count->events;
    parser->finish_nodes_entered += count->entered;
    parser->finish_walk_roots += count->roots;
    *count = (finish_count){0, 0, 0};
}

/* The owned-subtree visitor reports SLOTS, because destruction has to clear
 * them. A walk only reads one: no phase may substitute a field's root. */
static int push_owned_tree(markdown_core_node **slot, void *context) {
    return push_owned_root(slot ? *slot : NULL, true, context);
}

/* THE ROOTS THE FINISH WALK COMPLETED, in completion order, kept only while a
 * global pass is declared. A pass runs after the document's finalization, on
 * the finalized roots, and a root the walk found is not found again by
 * walking: the field roots and the document root are recorded here as they
 * complete. A step never frees a node that owns field roots (the LOCAL contract),
 * so every recorded root is live when the passes reach it. */
typedef struct {
    markdown_core_node **roots;
    size_t count, capacity;
} finish_roots;

static int record_finish_root(finish_roots *record, markdown_core_node *root) {
    if (record->count == record->capacity) {
        size_t capacity = record->capacity ? record->capacity * 2 : 8;
        markdown_core_node **roots;
        if (capacity > SIZE_MAX / sizeof(*roots)) {
            return 0;
        }
        roots = markdown_core_realloc(record->roots, capacity * sizeof(*roots));
        if (!roots) {
            return 0;
        }
        record->roots = roots;
        record->capacity = capacity;
    }
    record->roots[record->count++] = root;
    return 1;
}

/* THE FINISH WALK: one traversal of `root` and of every field root found
 * under it, on one explicit continuation stack -- a field root stays the node
 * its owner put there, no phase may substitute one, so a frame carries the
 * root itself and never the slot holding it, and depth never uses C frames.
 *
 * Every event of every node is delivered here and answered from the walk's
 * own registers and the parser's per-kind record: at a node's ENTER the walk
 * parses the node's inline content when it is the node's to parse (`parses`,
 * see owned_tree_frame; the content of a kind the record marks PARSES, not
 * DEFERRED, that the structure says holds inlines), runs the steps projected
 * for the ENTER, completes the node (the element's `complete_inline` the
 * record carries, the document's `observe_inline`) and pushes the node's
 * field roots; at a Text's EXIT it consolidates the run
 * when there is one to merge or a Text with no bytes to drop, and then runs
 * the steps projected for the EXIT, in descriptor order, each behind its
 * gate. `finish` runs on each root once its iteration completes, and the
 * roots it completes are recorded for the passes when `record` is given.
 * Parsing at ENTER gives the walk the container's children as the next
 * events: the lookahead is re-established over them, which re-delivers
 * nothing and is not counted. */
static int walk_owned_trees(markdown_core_parser *parser, markdown_core_node *const *roots, size_t count_roots,
                            bool rooted, tree_phase_func finish, void *context, finish_roots *record) {
    owned_tree_walk walk = {
        .parser = parser, .slots = parser->dialect->finish_step_slots, .script_depth = 0, .parses = true};
    finish_count count = {0, 0, 0};
    const markdown_core_element_instance *const document = parser->dialect->document_structure;
    void (*const observe)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *) =
        document->element->observe_inline;
    const markdown_core_kind_record *const kinds = parser->dialect->kinds;
    const markdown_core_finish_step_entry *const *const dispatch = parser->dialect->finish_dispatch;

    /* The first root on top. */
    for (size_t i = count_roots; i--;) {
        push_owned_root(roots[i], rooted, &walk);
    }
    while (walk.count && !parser->error) {
        owned_tree_frame *frame = &walk.frames[walk.count - 1];
        /* `frame` is the top of the stack, so its state words are the last row. */
        void **states = walk.states + (walk.count - 1) * walk.slots;
        markdown_core_iter *iter = &frame->iter;
        if (!frame->started) {
            markdown_core_iter_init(iter, frame->root);
            frame->started = true;
        }
        for (;;) {
            markdown_core_event_type event = markdown_core_iter_step(iter);
            markdown_core_finish_result result;
            count.events++;
            if (event == MARKDOWN_CORE_EVENT_DONE) {
                /* A block the walk finishes as part of a tree may have been
                 * consumed at its EXIT. */
                markdown_core_node *completed = frame->top;
                walk.count--;
                count.roots++;
                flush_finish_count(parser, &count);
                if (completed && finish && !finish(parser, completed, context)) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                }
                if (completed && record && !record_finish_root(record, completed)) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                }
                break;
            }
            markdown_core_node *node = iter->cur.node;
            size_t index = markdown_core_finish_kind_index((markdown_core_node_type)node->kind);
            if (node->element && node->element->delimiter.body == DELIMITER_WORD_BODY) {
                frame->script_depth += event == MARKDOWN_CORE_EVENT_ENTER ? 1 : -1;
            }
            if (event == MARKDOWN_CORE_EVENT_EXIT) {
                if (node == frame->parsed) {
                    frame->parsed = NULL;
                }
                /* Consolidation goes first at a Text's EXIT and takes the
                 * walk's own iterator over the siblings it absorbs, so by the
                 * time an element step sees this event the Text holds its
                 * whole run and the absorbed nodes are gone -- they were never
                 * delivered to anything else, which is what makes freeing
                 * them safe. */
                result = MARKDOWN_CORE_FINISH_CONTINUE;
                if (index == MARKDOWN_CORE_FINISH_TEXT_INDEX && markdown_core_text_needs_consolidation(node)) {
                    result = markdown_core_consolidate_text_step(parser, iter, node, complete_consolidated_text,
                                                                 frame->script_depth);
                }
                if (result == MARKDOWN_CORE_FINISH_CONTINUE && dispatch[2 * index + 1]) {
                    result = run_finish_steps(parser, dispatch[2 * index + 1], node, event, node == frame->top, states);
                }
                if (result == MARKDOWN_CORE_FINISH_FAILED) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
                continue;
            }
            /* A node is consumed only at its EXIT, so every ENTER reaches here. */
            count.entered++;
            const markdown_core_kind_record *facts = &kinds[index];
            unsigned flags = facts->flags;
            bool reach = frame->parses && !frame->parsed;
            if (reach && S_kind_contains_inlines(facts, node)) {
                if (!(flags & MARKDOWN_CORE_KIND_DEFERRED)) {
                    /* What the parse made less what it discarded on the way
                     * -- a bracket's opener text, a token that failed -- is
                     * what it handed the walk (parser.h). */
                    size_t made = parser->nodes_created, discarded = parser->nodes_freed;
                    markdown_core_parse_inlines(parser, node, parser->refmap);
                    parser->finish_nodes_parsed += (parser->nodes_created - made) - (parser->nodes_freed - discarded);
                    if (parser->error) {
                        break;
                    }
                    markdown_core_iter_reset(iter, node, MARKDOWN_CORE_EVENT_ENTER);
                }
                frame->parsed = node;
            }
            if (dispatch[2 * index]) {
                result = run_finish_steps(parser, dispatch[2 * index], node, event, node == frame->top, states);
                if (result == MARKDOWN_CORE_FINISH_FAILED) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
            }
            complete_inline_from_plan(parser, node, facts, frame->script_depth, document, observe);
            /* The field roots, of a kind that owns them through its record
             * or of an element that owns them through its hook: the answer is
             * no for almost every node, and it is one flag and one load. */
            if (!(flags & MARKDOWN_CORE_KIND_FIELDS) && !node->element) {
                continue;
            }
            walk.script_depth = frame->script_depth;
            walk.parses = reach;
            size_t first = walk.count;
            if (!markdown_core_visit_inline_subtrees(node, push_owned_tree, &walk)) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                break;
            }
            if (walk.count == first) {
                continue;
            }
            /* The visitor reports fields in source order; a stack consumes their
             * reversed registration order. No field is visited or scanned twice.
             * The frames carry no state yet -- a pushed root's words are zero
             * until its walk starts -- so swapping frames leaves nothing behind.
             * The push may have moved the frames, so the top is taken afresh:
             * it is the first field root, walked before this node's children. */
            for (size_t left = first, right = walk.count; left < right && left < --right; left++) {
                owned_tree_frame swap = walk.frames[left];
                walk.frames[left] = walk.frames[right];
                walk.frames[right] = swap;
            }
            break;
        }
    }
    flush_finish_count(parser, &count);
    markdown_core_free(walk.states);
    return !parser->error;
}

static markdown_core_node *finalize_document(markdown_core_parser *parser) {
    while (parser->current != parser->root) {
        parser->current = markdown_core_block_finalize(parser, parser->current);
    }

    markdown_core_block_finalize(parser, parser->root);

    return parser->root;
}

static void S_parse_block_inputs(markdown_core_parser *parser) {
    /* The ledger, and a rejoin, is of lines of the document's own input. */
    parser->line_state = MARKDOWN_CORE_LINE_UNKEPT;
    parser->rejoin_from = SIZE_MAX;
    while (parser->block_input_cursor < parser->block_input_count && !parser->error) {
        markdown_core_node *owner = parser->block_inputs[parser->block_input_cursor++];
        parser->block_root = owner;
        parser->current = owner;
        parser->input_line_count = 0;
        parser->input_fact_count = 0;
        parser->input_first_line = parser->line_marks[owner->content_map.first].line;
        parser->line_number = parser->input_first_line - 1;
        parser->last_line_end = parser->line_marks[owner->content_map.first].source;
        owner->flags |= MARKDOWN_CORE_NODE__OPEN;
        S_input_open(parser, owner->content.ptr, (size_t)owner->content.size);
        S_parse_source(parser);
        while (parser->current != owner && !parser->error) {
            parser->current = markdown_core_block_finalize(parser, parser->current);
        }
        owner->flags &= ~MARKDOWN_CORE_NODE__OPEN;
        markdown_core_strbuf_clear(&owner->content);
        owner->content_map.count = 0;
        owner->content_map.offset = 0;
    }
    parser->block_root = parser->root;
    parser->current = parser->root;
}

markdown_core_node *markdown_core_parser_parse(markdown_core_parser *parser, const char *source, size_t length,
                                               markdown_core_revision *revision) {
    S_parse_begin(parser, revision);
    if (!parser->error) {
        if (source) {
            S_input_open(parser, (const unsigned char *)source, length);
        }
        if (source || S_input_open_text(parser, revision->text, parser->restart)) {
            S_parse_source(parser);
        }
    }
    markdown_core_node *document = S_finish_parse(parser);
    revision->applied = parser->applied;
    S_parse_end(parser);
    return document;
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
    const unsigned char *raw = markdown_core_parser_input_at(parser, line->start);
    for (size_t at = 0; at < (size_t)(line->end - line->start); at++) {
        unsigned char byte = raw[at];
        if (byte) {
            *out++ = byte;
        } else {
            view->nuls[view->nul_count++] = line->start + (uint32_t)at;
            *out++ = 0xef;
            *out++ = 0xbf;
            *out++ = 0xbd;
        }
    }
    *out++ = '\n';
    *out = 0;
    view->next = parser->normalized_lines;
    parser->normalized_lines = view;
    facts->normalized = view;
    return view->bytes;
}

static inline const unsigned char *S_input_line_content(markdown_core_parser *parser, markdown_core_input_line *line,
                                                        bufsize_t *length) {
    size_t size = line->end - line->start;
    markdown_core_line_facts *facts = line->facts ? &parser->input_facts[line->facts - 1] : NULL;
    uint32_t nul_count = facts ? facts->nul_count : 0;
    /* Raw geometry is bounded when its input is installed. Only expansion
     * needs an additional bound at this allocation boundary. */
    *length = (bufsize_t)size;
    if (!nul_count) {
        return markdown_core_parser_input_at(parser, line->start);
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

/* THE WINDOW REACHES THE NEXT PIECE of a session's text (parser.h, the
 * input's bytes): its bytes are copied in after the ones before. Kept out of
 * line: the scanner reaches it once per piece, not per line. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_input_fill(markdown_core_parser *parser) {
    const uint8_t *bytes;
    size_t size;
    bool more = markdown_core_text_cursor_next(&parser->input_cursor, &bytes, &size);
    assert(more);
    (void)more;
    memcpy(parser->input_buffer + parser->input_filled, bytes, size);
    parser->input_filled += size;
}

/* The sole physical-line scanner for root and mapped inputs. Grammar facts
 * live beside their line, so changing inputs drops them together. Inlining
 * is explicit: both GCC and Clang may otherwise outline this per-line step. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_input_line *S_extend_source_lines(markdown_core_parser *parser, size_t index) {
    assert(parser->input_length <= MARKDOWN_CORE_SOURCE_CAPACITY);
    while (index >= parser->input_line_count && parser->input_scanned < parser->input_length) {
        if (parser->input_line_count == parser->input_line_capacity) {
            void *lines = markdown_core_reserve(parser->input_lines, &parser->input_line_capacity,
                                                parser->input_line_count + 1, sizeof(*parser->input_lines));
            if (!lines) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return NULL;
            }
            parser->input_lines = lines;
        }
        markdown_core_input_line entry = {.start = (uint32_t)parser->input_scanned};
        uint32_t nul_count = 0;
        const unsigned char *window = parser->input_window;
        const unsigned char *cursor = window + entry.start;
        const unsigned char *end = window + parser->input_filled;
        /* Scan spans ending at a line boundary or a normalization boundary.
         * The NUL count changes only at the latter, not on every source byte. */
        for (;;) {
            cursor = S_source_span_end(cursor, end);
            if (cursor == end) {
                if (parser->input_filled == parser->input_length) {
                    break;
                }
                S_input_fill(parser);
                end = window + parser->input_filled;
                continue;
            }
            if (*cursor) {
                break;
            }
            nul_count++;
            cursor++;
        }
        entry.end = (uint32_t)(cursor - window);
        if (cursor < end && *cursor == '\r') {
            cursor++;
            /* A CR that ends a piece may have its LF in the next. */
            if (cursor == end && parser->input_filled < parser->input_length) {
                S_input_fill(parser);
                end = window + parser->input_filled;
            }
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
            parser->input_mapped = true;
        }
        parser->input_scanned = (size_t)(cursor - window);
        parser->input_line_work += parser->input_scanned - entry.start;
        parser->input_lines[parser->input_line_count++] = entry;
    }
    if (index < parser->input_line_count) {
        return &parser->input_lines[index];
    }
    /* A reader that asks past the last line reads the end of the input. */
    parser->input_ended = true;
    return NULL;
}

/* External speculative readers share the driver's scanner. The driver calls
 * the static inline body directly, including when it advances the frontier. */
markdown_core_input_line *markdown_core_parser_extend_source_lines(markdown_core_parser *parser, size_t index) {
    return S_extend_source_lines(parser, index);
}

markdown_core_line_facts *markdown_core_parser_extend_line_facts(markdown_core_parser *parser,
                                                                 markdown_core_input_line *line) {
    void *facts = markdown_core_reserve(parser->input_facts, &parser->input_fact_capacity, parser->input_fact_count + 1,
                                        sizeof(*parser->input_facts));
    if (!facts) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    parser->input_facts = facts;
    markdown_core_line_facts *entry = &parser->input_facts[parser->input_fact_count++];
    *entry = (markdown_core_line_facts){0};
    line->facts = (uint32_t)parser->input_fact_count;
    return entry;
}

/* THE ACTIVE INPUT. Every input starts a new index whose first line is the
 * line after the parser's current one; its geometry is in the document's
 * offsets for the document, and in the content's for a cell. */
static void S_input_reset(markdown_core_parser *parser) {
    S_input_release(parser);
    parser->input_line_count = 0;
    parser->input_fact_count = 0;
    parser->input_ended = false;
    parser->input_mapped = parser->block_root != parser->root;
    parser->input_first_line = parser->line_number + 1;
    parser->lookahead_last_line_ready = false;
}

/* An input whose bytes are all in hand: the document a fresh parse reads,
 * or a cell's content. */
static void S_input_open(markdown_core_parser *parser, const unsigned char *bytes, size_t length) {
    assert(length <= MARKDOWN_CORE_SOURCE_CAPACITY);
    S_input_reset(parser);
    parser->input_length = length;
    parser->input_scanned = 0;
    parser->input_window = bytes;
    parser->input_filled = length;
}

/* A session's text, read from byte `from` on, which begins a line: the
 * window has room for the whole text and holds the piece `from` is in from
 * there. False when it could not allocate. */
static bool S_input_open_text(markdown_core_parser *parser, const markdown_core_text_tree *text, size_t from) {
    S_input_reset(parser);
    parser->input_length = markdown_core_text_tree_size(text);
    assert(parser->input_length <= MARKDOWN_CORE_SOURCE_CAPACITY);
    parser->input_scanned = from;
    parser->input_filled = from;
    parser->input_buffer = markdown_core_realloc(NULL, parser->input_length + 1);
    if (!parser->input_buffer) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->input_window = parser->input_buffer;
    const uint8_t *bytes;
    size_t size, start;
    if (markdown_core_text_cursor_seek(&parser->input_cursor, text, from, &bytes, &size, &start)) {
        size -= from - start;
        memcpy(parser->input_buffer + from, bytes + (from - start), size);
        parser->input_filled += size;
    }
    return true;
}

/* The entry that names `node`, made for this parse's checkpoints when it
 * has none. The parse holds each entry it makes until it commits. */
static markdown_core_entry *S_entry_of(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->entry) {
        return node->entry;
    }
    markdown_core_entry *entry = markdown_core_entry_new(parser->revision->checkpoints, node);
    if (!entry) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    entry->link.own = node->where.place.start;
    entry->link.up = parser->entered ? &parser->entered->link : NULL;
    entry->holds = 1;
    parser->entered = entry;
    parser->entered_count++;
    return entry;
}

/* The state `node`'s element carries from line to line (E3), or 0 for an
 * element that carries none; and the same state put back on `node`. */
static uint64_t S_carry_save(const markdown_core_parser *parser, const markdown_core_node *node) {
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
    return structure->element->carry_save ? structure->element->carry_save(structure, node) : 0;
}
static void S_carry_restore(const markdown_core_parser *parser, markdown_core_node *node, uint64_t carry) {
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
    if (structure->element->carry_restore) {
        structure->element->carry_restore(structure, node, carry);
    }
}

/* Whether a later line may write `node`, a closed block (E2): some element
 * says it may. */
static bool S_writable(const markdown_core_parser *parser, const markdown_core_node *node) {
    const markdown_core_dialect *dialect = parser->dialect;
    for (size_t i = 0; i < dialect->closed_writer_count; i++) {
        const markdown_core_element_instance *instance = dialect->closed_writers[i];
        if (instance->element->writes_below(instance, parser, node)) {
            return true;
        }
    }
    return false;
}

/* THE OPEN SPINE AT A LINE START where a rejoin may be (parser.h): every
 * open block of a kind that reopens, from the document down, in
 * `line_frames` with its flags and carried state; and the block below it --
 * the deepest open block when its kind does not reopen, the open leaf, or
 * else the last child of the innermost container -- in `line_point`. False
 * when the line start can be no checkpoint: an open block of a kind that
 * does not reopen holds an open child, the open leaf is in the document
 * itself, which always asks it whether it continues, or the block below is
 * closed and a later line may write it. */
static bool S_read_spine(markdown_core_parser *parser) {
    markdown_core_node *current = parser->current;
    if (current->parent == parser->root &&
        !(markdown_core_parser_kind(parser, current)->flags & MARKDOWN_CORE_KIND_REOPENS)) {
        return false;
    }
    markdown_core_node *node = parser->root, *leaf = NULL;
    size_t depth = 0;
    while (S_last_child_is_open(node)) {
        markdown_core_node *child = node->last_child;
        const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, child);
        if (!(kind->flags & MARKDOWN_CORE_KIND_REOPENS)) {
            if (S_last_child_is_open(child)) {
                return false;
            }
            leaf = child;
            break;
        }
        if (depth == parser->line_frame_capacity) {
            void *grown = markdown_core_reserve(parser->line_frames, &parser->line_frame_capacity, depth + 1,
                                                sizeof(*parser->line_frames));
            if (!grown) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return false;
            }
            parser->line_frames = grown;
        }
        parser->line_frames[depth++] =
            (markdown_core_line_frame){child, (uint16_t)(child->flags & MARKDOWN_CORE_CARRIED_BITS), 0};
        node = child;
    }
    markdown_core_node *below = leaf ? leaf : node->last_child;
    if (!leaf && below && S_writable(parser, below)) {
        return false;
    }
    for (size_t at = 0; at < depth; at++) {
        parser->line_frames[at].carry = S_carry_save(parser, parser->line_frames[at].node);
    }
    uint8_t marks = below ? MARKDOWN_CORE_CHECKPOINT_BELOW : 0;
    if (leaf) {
        marks |= MARKDOWN_CORE_CHECKPOINT_LEAF;
    }
    parser->line_frame_count = depth;
    parser->line_point.below = below;
    parser->line_point.below_bits = below ? (uint16_t)(below->flags & MARKDOWN_CORE_CARRIED_BITS) : 0;
    parser->line_point.marks = marks;
    return true;
}

/* THE LEDGER (parser.h). The first block a line of the document's input
 * opens, or the first whose kind it changes, fixes what was below the spine
 * at the line start: nothing has opened or changed kind in the line yet, so
 * the line start's open block still has the kind and the children it had. */
void markdown_core_parser_line_opened(markdown_core_parser *parser) {
    markdown_core_node *current = parser->line_current;
    parser->line_state = MARKDOWN_CORE_LINE_OPENED;
    if (markdown_core_parser_kind(parser, current)->flags & MARKDOWN_CORE_KIND_REOPENS || current == parser->root) {
        parser->line_inner = current;
        parser->line_below = current->last_child;
        parser->line_below_marks = current->last_child ? MARKDOWN_CORE_CHECKPOINT_BELOW : 0;
    } else {
        parser->line_inner = current->parent;
        parser->line_below = current;
        parser->line_below_marks = MARKDOWN_CORE_CHECKPOINT_BELOW | MARKDOWN_CORE_CHECKPOINT_LEAF;
    }
}

/* The run a ledger log (parser.h) appends to once its last is full: the
 * one after it, kept from before, or a new one with room for twice as many
 * records. NULL when it could not allocate. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) markdown_core_ledger_run *S_log_next_run(markdown_core_parser *parser,
                                                                                    markdown_core_ledger_log *log,
                                                                                    size_t size) {
    markdown_core_ledger_run *run = log->last, *next = run ? run->after : log->first;
    if (!next) {
        size_t capacity = run ? 2 * run->capacity : 64;
        next = markdown_core_realloc(NULL, sizeof(*next) + capacity * size);
        if (!next) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return NULL;
        }
        *next = (markdown_core_ledger_run){.before = run, .capacity = capacity};
        if (run) {
            run->after = next;
        } else {
            log->first = next;
        }
    }
    next->count = 0;
    log->last = next;
    return next;
}

/* A record appended to a ledger log; NULL when it could not allocate. */
static inline void *S_log_append(markdown_core_parser *parser, markdown_core_ledger_log *log, size_t size) {
    markdown_core_ledger_run *run = log->last;
    if ((!run || run->count == run->capacity) && !(run = S_log_next_run(parser, log, size))) {
        return NULL;
    }
    log->count++;
    return (unsigned char *)(void *)run->storage + run->count++ * size;
}

/* Empties a log, which keeps its runs. */
static void S_log_empty(markdown_core_ledger_log *log) {
    log->last = NULL;
    log->count = 0;
}

static void S_log_free(markdown_core_ledger_log *log) {
    while (log->first) {
        markdown_core_ledger_run *run = log->first;
        log->first = run->after;
        markdown_core_free(run);
    }
    S_log_empty(log);
}

/* A line that opened a block goes into the ledger when it ended in a state
 * a restart reopens: it wrote no closed block, and an open leaf below the
 * spine was closed by the line without being asked whether it continues, in
 * a container. That every container of the spine reopens, and that no later
 * line writes a closed block below it, the replay finds. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_keep_line(markdown_core_parser *parser) {
    size_t line = (size_t)parser->line_start;
    markdown_core_node *below = parser->line_below;
    bool leaf = (parser->line_below_marks & MARKDOWN_CORE_CHECKPOINT_LEAF) != 0;
    if (parser->written_line == line + 1 ||
        (leaf && ((below->flags & MARKDOWN_CORE_NODE__OPEN) || parser->line_reached == below ||
                  parser->line_inner == parser->root))) {
        return;
    }
    markdown_core_line_record *record = S_log_append(parser, &parser->line_records, sizeof(*record));
    if (!record) {
        return;
    }
    *record = (markdown_core_line_record){
        .inner = parser->line_inner,
        .below = below,
        .line = (uint32_t)line,
        .frontier = (uint32_t)(parser->line_frontier - line),
        .after = (uint32_t)(parser->input_scanned + parser->input_ended - line),
        .mark = (uint32_t)parser->line_mark,
        .marks = parser->line_below_marks,
        .written = below && (below->flags & MARKDOWN_CORE_NODE__WRITTEN),
    };
}

/* The root is no frame: a restart reopens it with the flags it ends in. */
void markdown_core_parser_keep_flags(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_undo_record *record =
        node != parser->root ? S_log_append(parser, &parser->undo_records, sizeof(*record)) : NULL;
    if (record) {
        *record = (markdown_core_undo_record){.node = node, .bits = node->flags, .flags = true};
    }
}

void markdown_core_parser_keep_carry(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_undo_record *record = S_log_append(parser, &parser->undo_records, sizeof(*record));
    if (record) {
        *record = (markdown_core_undo_record){.node = node, .carry = S_carry_save(parser, node)};
    }
}

/* Room for `depth` frames in `line_frames`. */
static bool S_reserve_line_frames(markdown_core_parser *parser, size_t depth) {
    if (depth <= parser->line_frame_capacity) {
        return true;
    }
    void *grown =
        markdown_core_reserve(parser->line_frames, &parser->line_frame_capacity, depth, sizeof(*parser->line_frames));
    if (!grown) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->line_frames = grown;
    return true;
}

/* Whether a line of the ledger makes a checkpoint, read from the state the
 * parse is in: every container of its spine reopens, and the block below,
 * unless the line settled it, was written when the line ended, or is not
 * written later and no later line may write it. The line's spine is then in
 * `line_frames`, from the document down, and every block it names has an
 * entry. The replay names the lines first to last, so the entries are made
 * in source order: a block a line names first is open at its line start or
 * below its spine, so it starts after every block an earlier line names,
 * and a container before what it holds. */
static bool S_name_line(markdown_core_parser *parser, markdown_core_line_record *record) {
    size_t depth = 0;
    for (const markdown_core_node *node = record->inner; node != parser->root; node = node->parent) {
        if (!(markdown_core_parser_kind(parser, node)->flags & MARKDOWN_CORE_KIND_REOPENS)) {
            return false;
        }
        depth++;
    }
    markdown_core_node *below = record->below;
    if (below && !(record->marks & MARKDOWN_CORE_CHECKPOINT_LEAF) && !record->written &&
        (below->flags & MARKDOWN_CORE_NODE__WRITTEN || S_writable(parser, below))) {
        return false;
    }
    if (!S_reserve_line_frames(parser, depth)) {
        return false;
    }
    record->depth = (uint32_t)depth;
    size_t level = depth;
    for (markdown_core_node *node = record->inner; node != parser->root; node = node->parent) {
        parser->line_frames[--level].node = node;
    }
    for (; level < depth; level++) {
        if (!S_entry_of(parser, parser->line_frames[level].node)) {
            return false;
        }
    }
    return !below || S_entry_of(parser, below);
}

/* An entry's carried state as the replay has it: the state its block is in
 * until the replay undoes a change. Only a container that reopens carries a
 * word. */
static inline void S_replay_state(const markdown_core_parser *parser, markdown_core_entry *entry, uint32_t epoch) {
    if (entry->epoch != epoch) {
        const markdown_core_node *node = entry->node;
        entry->epoch = epoch;
        entry->bits = (uint16_t)(node->flags & MARKDOWN_CORE_CARRIED_BITS);
        entry->carry = markdown_core_parser_kind(parser, node)->flags & MARKDOWN_CORE_KIND_REOPENS
                           ? S_carry_save(parser, node)
                           : 0;
    }
}

/* The checkpoint of a named line of the ledger, made from the replayed
 * state of its spine and the block below. Its frames are those of the
 * checkpoint made before it as far as the spines agree from the document
 * down. */
static markdown_core_checkpoint *S_replay_line(markdown_core_parser *parser, const markdown_core_line_record *record,
                                               uint32_t epoch) {
    markdown_core_checkpoints *store = parser->revision->checkpoints;
    size_t depth = record->depth, level = depth;
    for (markdown_core_node *node = record->inner; node != parser->root; node = node->parent) {
        markdown_core_entry *entry = node->entry;
        S_replay_state(parser, entry, epoch);
        parser->line_frames[--level] = (markdown_core_line_frame){node, entry->bits, entry->carry};
    }
    markdown_core_entry *below = record->below ? record->below->entry : NULL;
    if (below) {
        S_replay_state(parser, below, epoch);
    }
    /* The levels both spines share from the document down, and the frame
     * the new ones hang from: the other spine is walked from its innermost
     * frame out, and the outermost level that differs bounds the share. */
    size_t keep = depth < parser->spine_depth ? depth : parser->spine_depth;
    markdown_core_frame *parent = NULL;
    level = parser->spine_depth;
    for (markdown_core_frame *frame = parser->spine; frame; frame = frame->parent) {
        level--;
        if (level < keep) {
            const markdown_core_line_frame *line = &parser->line_frames[level];
            if (frame->entry->node != line->node || frame->bits != line->bits || frame->carry != line->carry) {
                keep = level;
                parent = frame->parent;
            } else if (level + 1 == keep) {
                parent = frame;
            }
        }
    }
    markdown_core_frame *inner = parent;
    for (size_t at = keep; at < depth; at++) {
        const markdown_core_line_frame *line = &parser->line_frames[at];
        markdown_core_frame *frame = markdown_core_frame_new(store, inner, line->node->entry, line->bits, line->carry);
        if (!frame) {
            /* The frames made so far go with the one that holds them. */
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            if (inner != parent) {
                markdown_core_frame_hold(inner);
                markdown_core_frame_drop(store, inner);
            }
            return NULL;
        }
        inner = frame;
    }
    if (inner != parser->spine) {
        markdown_core_frame_hold(inner);
        markdown_core_frame_drop(store, parser->spine);
        parser->spine = inner;
        parser->spine_depth = depth;
    }
    markdown_core_checkpoint *checkpoint = markdown_core_checkpoint_new(store);
    if (!checkpoint) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    checkpoint->frame = inner;
    markdown_core_frame_hold(inner);
    checkpoint->below = below;
    markdown_core_entry_hold(below);
    checkpoint->below_bits = below ? below->bits : 0;
    checkpoint->marks = record->marks;
    checkpoint->frontier = record->frontier;
    checkpoint->after = record->after;
    checkpoint->link.own = record->line;
    return checkpoint;
}

/* THE REPLAY of the ledger's lines kept since the last one. The lines are
 * named first to last; then, from the last back, the changes made since a
 * line started are undone before its checkpoint is made. The checkpoints
 * join the parse's, newest first. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_replay(markdown_core_parser *parser) {
    markdown_core_checkpoints *store = parser->revision->checkpoints;
    markdown_core_ledger_run *last = parser->line_records.last;
    for (markdown_core_ledger_run *run = parser->line_records.first; last && !parser->error; run = run->after) {
        markdown_core_line_record *records = (markdown_core_line_record *)(void *)run->storage;
        for (size_t at = 0; at < run->count && !parser->error; at++) {
            if (!S_name_line(parser, &records[at])) {
                records[at].inner = NULL;
            }
        }
        if (run == last) {
            break;
        }
    }
    uint32_t epoch = ++store->epoch;
    markdown_core_checkpoint *newest = NULL, *oldest = NULL;
    /* The undo records still to undo end at `undo` in `changes`, the run
     * they are in, and number `undone` before it. */
    markdown_core_ledger_run *changes = parser->undo_records.last;
    size_t undo = changes ? changes->count : 0, undone = parser->undo_records.count - undo;
    for (markdown_core_ledger_run *run = last; run && !parser->error; run = run->before) {
        const markdown_core_line_record *records = (const markdown_core_line_record *)(const void *)run->storage;
        for (size_t at = run->count; at-- > 0 && !parser->error;) {
            const markdown_core_line_record *record = &records[at];
            while (undone + undo > record->mark) {
                if (!undo) {
                    changes = changes->before;
                    undo = changes->count;
                    undone -= undo;
                    continue;
                }
                const markdown_core_undo_record *change =
                    &((const markdown_core_undo_record *)(const void *)changes->storage)[--undo];
                markdown_core_entry *entry = change->node->entry;
                if (!entry) {
                    continue;
                }
                S_replay_state(parser, entry, epoch);
                if (change->flags) {
                    entry->bits = (uint16_t)(change->bits & MARKDOWN_CORE_CARRIED_BITS);
                } else {
                    entry->carry = change->carry;
                }
            }
            markdown_core_checkpoint *checkpoint = record->inner ? S_replay_line(parser, record, epoch) : NULL;
            if (checkpoint) {
                if (oldest) {
                    oldest->link.up = &checkpoint->link;
                } else {
                    newest = checkpoint;
                }
                oldest = checkpoint;
                parser->taken_count++;
            }
        }
    }
    if (oldest) {
        oldest->link.up = parser->taken ? &parser->taken->link : NULL;
        parser->taken = newest;
    }
    S_log_empty(&parser->line_records);
    S_log_empty(&parser->undo_records);
}

void markdown_core_parser_touch(markdown_core_parser *parser, const markdown_core_node *node) {
    size_t start = node->where.place.start, end = node->where.place.end;
    if (node->flags & MARKDOWN_CORE_NODE__OPEN && (size_t)parser->line_end > end) {
        end = (size_t)parser->line_end;
    }
    if (!parser->touched_end) {
        parser->touched_start = start;
        parser->touched_end = end;
        return;
    }
    parser->touched_start = start < parser->touched_start ? start : parser->touched_start;
    parser->touched_end = end > parser->touched_end ? end : parser->touched_end;
}

void markdown_core_parser_write_closed(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_parser_touch(parser, node);
    markdown_core_parser_set_flags(parser, node, (uint16_t)(node->flags | MARKDOWN_CORE_NODE__WRITTEN));
    if ((size_t)parser->line_end > parser->touched_end) {
        parser->touched_end = (size_t)parser->line_end;
    }
    parser->written_line = (size_t)parser->line_start + 1;
}

/* THE RESTART (docs/plans/2026-09-29-incremental-parsing.md, 5.3). A parse
 * that continues a session's document reads the new text from a line start
 * where the old parse took a checkpoint before the first edit, with the
 * spine that checkpoint names reopened, and rejoins the old parse at the
 * first later line start past the edits where the old parse took a
 * checkpoint in the state the new one is in: from there the old parse's
 * future is the new parse's, so its blocks are kept. The parse covers the
 * edits up to the rejoin, and the next parse continues from there with the
 * rest. A parse that finds no checkpoint to restart at reads from the
 * document's start with a new root, and may still rejoin.
 *
 * Offsets in the old text are those of the session's checkpoints, and of
 * the old tree's extents; the edits the parse covers turn them into offsets
 * of the text it reads. */

static inline int64_t S_edit_change(const markdown_core_byte_edit *edit) {
    return (int64_t)edit->size - (int64_t)(edit->end - edit->start);
}

/* An edit's start in the old text. */
static inline size_t S_edit_start(const markdown_core_parser *parser, size_t index) {
    return (size_t)((int64_t)parser->revision->edits[index].start + parser->revision->edit_offset);
}

/* The start of the line an edit at `at` in the new text damages first: the
 * line that holds the byte before it when that byte is a CR, which an LF at
 * `at` would join. */
static size_t S_damaged_line(const markdown_core_text_tree *text, size_t at) {
    if (at && markdown_core_text_tree_byte(text, at - 1) == '\r') {
        at--;
    }
    return markdown_core_text_tree_line_start(text, at);
}

/* Whether every block a checkpoint names is still in the tree. */
static bool S_checkpoint_alive(const markdown_core_checkpoint *checkpoint) {
    for (const markdown_core_frame *frame = checkpoint->frame; frame; frame = frame->parent) {
        if (!frame->entry->node) {
            return false;
        }
    }
    return !checkpoint->below || checkpoint->below->node;
}

/* THE CHECKPOINT A RESTART TAKES for damage from the old line start
 * `damage`: the latest one before which nothing the old parse decided read
 * the damage -- the reads made before its line, and the ones its line made
 * when it settled a leaf -- whose blocks are all in the tree, and which is
 * not past the touched span's start when `touched` (checkpoints.h). NULL
 * when there is none. */
static markdown_core_checkpoint *S_restart_point(const markdown_core_checkpoints *store, size_t damage, bool touched) {
    markdown_core_summed_node *link = markdown_core_summed_last_through(&store->lines, damage);
    size_t line = link ? markdown_core_summed_before(link) + link->own : 0;
    while (link) {
        markdown_core_checkpoint *checkpoint = markdown_core_checkpoint_of(link);
        if (line + checkpoint->frontier <= damage &&
            (!(checkpoint->marks & MARKDOWN_CORE_CHECKPOINT_LEAF) || line + checkpoint->after <= damage) &&
            (!touched || line <= store->touched_start) && S_checkpoint_alive(checkpoint)) {
            return checkpoint;
        }
        line -= link->own;
        link = markdown_core_summed_previous(link);
    }
    return NULL;
}

/* The old line start where the restart for the next edit the parse does not
 * cover yet would be, or SIZE_MAX when every edit is covered. */
static void S_plan_next_restart(markdown_core_parser *parser) {
    markdown_core_revision *revision = parser->revision;
    parser->next_restart = SIZE_MAX;
    if (parser->applied == revision->edit_count) {
        return;
    }
    int64_t at = (int64_t)S_edit_start(parser, parser->applied) + parser->shift;
    int64_t damage = (int64_t)S_damaged_line(revision->text, (size_t)at) - parser->shift;
    markdown_core_checkpoint *checkpoint =
        S_restart_point(revision->checkpoints, damage < 0 ? 0 : (size_t)damage, false);
    parser->next_restart = checkpoint ? markdown_core_checkpoint_line(checkpoint) : 0;
}

/* The parse covers the next edit: a rejoin must come after the line its new
 * bytes end on. `shifts` keeps the length change before each covered
 * edit. */
static bool S_cover_edit(markdown_core_parser *parser) {
    size_t index = parser->applied;
    void *grown = markdown_core_reserve(parser->shifts, &parser->shift_capacity, index + 2, sizeof(*parser->shifts));
    if (!grown) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->shifts = grown;
    const markdown_core_byte_edit *edit = &parser->revision->edits[index];
    parser->rejoin_from = (size_t)((int64_t)S_edit_start(parser, index) + parser->shift) + edit->size;
    parser->shift += S_edit_change(edit);
    parser->shifts[index + 1] = parser->shift;
    parser->applied = index + 1;
    S_plan_next_restart(parser);
    return true;
}

bool markdown_core_parser_image(const markdown_core_parser *parser, size_t start, size_t end, size_t *image) {
    const markdown_core_byte_edit *edits = parser->revision->edits;
    int64_t offset = parser->revision->edit_offset;
    size_t lo = 0, hi = parser->applied, x = start;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((size_t)((int64_t)edits[mid].end + offset) <= x) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (; lo < parser->applied && (size_t)((int64_t)edits[lo].start + offset) <= x; lo++) {
        size_t replaced = (size_t)((int64_t)edits[lo].end + offset);
        x = replaced > x ? replaced : x;
    }
    if (x >= end) {
        return false;
    }
    *image = (size_t)((int64_t)x + parser->shifts[lo]);
    return true;
}

static markdown_core_cut *S_cuts_reserve(markdown_core_parser *parser, size_t count) {
    void *grown = markdown_core_reserve(parser->cuts, &parser->cut_capacity, count, sizeof(*parser->cuts));
    if (!grown) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    parser->cuts = grown;
    return parser->cuts;
}

/* The summary of `child` as a child of `container` (E4), or 0 for a
 * container whose element folds nothing. */
static uint32_t S_fold_child(markdown_core_parser *parser, markdown_core_node *container, markdown_core_node *child,
                             bool last) {
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, container);
    return structure->element->fold_child ? structure->element->fold_child(structure, parser, container, child, last)
                                          : 0;
}

bool markdown_core_parser_relation_ends(const markdown_core_parser *parser, const markdown_core_node *container,
                                        const markdown_core_node *child) {
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, container);
    return structure->element->relation_ends && structure->element->relation_ends(structure, container, child);
}

/* REOPENING THE SPINE a checkpoint names, at its line start `line`. Each
 * container of the spine, the root first, is a cut: what it held after the
 * child the spine went through waits in the cut's holder, and the
 * container is open again with the flags and carried state its frame
 * kept; its old flags, carried state, end and extent, its fold and the old
 * summary of that child are kept for the rejoin and the refold, read before
 * anything changes. The block below
 * the spine keeps its value; a leaf the checkpoint's line settled is open
 * again, and that line closes it as it is. */
static void S_reopen(markdown_core_parser *parser, markdown_core_checkpoint *checkpoint, size_t line) {
    const markdown_core_text_tree *text = parser->revision->text;
    size_t depth = 0;
    for (markdown_core_frame *frame = checkpoint->frame; frame; frame = frame->parent) {
        depth++;
    }
    void *grown = markdown_core_reserve(parser->line_frames, &parser->line_frame_capacity, depth + 1,
                                        sizeof(*parser->line_frames));
    if (!grown || !S_cuts_reserve(parser, depth + 1)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    parser->line_frames = grown;
    markdown_core_line_frame *frames = parser->line_frames;
    markdown_core_cut *cuts = parser->cuts;
    frames[0] = (markdown_core_line_frame){parser->revision->previous, 0, 0};
    size_t level = depth;
    for (markdown_core_frame *frame = checkpoint->frame; frame; frame = frame->parent, level--) {
        frames[level] = (markdown_core_line_frame){frame->entry->node, frame->bits, frame->carry};
    }
    markdown_core_node *below = checkpoint->below ? checkpoint->below->node : NULL;
    for (size_t d = 0; d <= depth; d++) {
        markdown_core_node *node = frames[d].node, *chain = d < depth ? frames[d + 1].node : below;
        size_t start = d ? markdown_core_entry_start(node->entry) : 0;
        markdown_core_cut *cut = &cuts[d];
        *cut = (markdown_core_cut){.node = node,
                                   .chain = chain,
                                   .extent = node->where.extent,
                                   .flags = node->flags,
                                   .carry = d ? S_carry_save(parser, node) : 0};
        cut->end = (uint32_t)(start + node->where.extent.span);
        cut->base = cut->anchor = (uint32_t)start;
        if (chain) {
            cut->chain_extent = chain->where.extent;
            if (!markdown_core_parser_relation_ends(parser, node, chain)) {
                cut->anchor = (uint32_t)(markdown_core_entry_start(chain->entry) + chain->where.extent.span);
            }
            cut->chain_fold = S_fold_child(parser, node, chain, false);
        }
        if (node->entry) {
            cut->sum = node->entry->sum;
            cut->last = node->entry->last;
        }
    }
    parser->cut_count = parser->cut_reopened = depth + 1;
    for (size_t d = 0; d <= depth; d++) {
        markdown_core_cut *cut = &cuts[d];
        markdown_core_node *node = cut->node, *chain = cut->chain;
        markdown_core_node *held = chain ? chain->next : node->first_child;
        if (held) {
            cut->holder.first_child = held;
            cut->holder.last_child = node->last_child;
            held->prev = NULL;
        }
        if (chain) {
            chain->next = NULL;
        } else {
            node->first_child = NULL;
        }
        node->last_child = chain;
        size_t start = d ? markdown_core_entry_start(node->entry) : 0;
        node->where.place = (markdown_core_place){(uint32_t)start, (uint32_t)start};
        node->flags = d ? (uint16_t)(frames[d].bits | MARKDOWN_CORE_NODE__OPEN)
                        : (uint16_t)((node->flags & MARKDOWN_CORE_CARRIED_BITS) | MARKDOWN_CORE_NODE__OPEN);
        if (d) {
            S_carry_restore(parser, node, frames[d].carry);
        }
    }
    if (below) {
        size_t start = markdown_core_entry_start(checkpoint->below);
        below->where.place = (markdown_core_place){(uint32_t)start, (uint32_t)(start + below->where.extent.span)};
        if (checkpoint->marks & MARKDOWN_CORE_CHECKPOINT_LEAF) {
            below->flags |= MARKDOWN_CORE_NODE__OPEN;
            parser->settled = below;
        }
    }
    parser->root = parser->block_root = frames[0].node;
    parser->old_root = NULL;
    parser->current = parser->settled ? below : frames[depth].node;
    parser->spine = checkpoint->frame;
    markdown_core_frame_hold(parser->spine);
    parser->spine_depth = depth;
    /* The line before ends where its terminator begins; the first line has
     * none before it. */
    size_t end = line ? line - 1 : 0;
    if (end && markdown_core_text_tree_byte(text, end) == '\n' && markdown_core_text_tree_byte(text, end - 1) == '\r') {
        end--;
    }
    parser->last_line_end = (bufsize_t)end;
}

/* THE RESTART of a parse that continues `previous`: where it reads from,
 * the spine it reopens there or the new root it makes, and the first rejoin
 * it may take. False when it could not allocate. */

static bool S_restart(markdown_core_parser *parser) {
    markdown_core_revision *revision = parser->revision;
    markdown_core_checkpoints *store = revision->checkpoints;
    if (!S_cuts_reserve(parser, 1)) {
        return false;
    }
    void *grown = markdown_core_reserve(parser->shifts, &parser->shift_capacity, 1, sizeof(*parser->shifts));
    if (!grown) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->shifts = grown;
    parser->shifts[0] = 0;
    markdown_core_checkpoint *checkpoint = NULL;
    if (revision->edit_count) {
        size_t damage = S_damaged_line(revision->text, S_edit_start(parser, 0));
        checkpoint = S_restart_point(store, damage, store->touched_end != 0);
        if (!S_cover_edit(parser)) {
            return false;
        }
    } else {
        checkpoint = S_restart_point(store, markdown_core_text_tree_size(revision->text), store->touched_end != 0);
        parser->rejoin_from = 0;
    }
    parser->rejoin_old = store->touched_end;
    parser->restarted = checkpoint;
    if (checkpoint) {
        parser->restart = markdown_core_checkpoint_line(checkpoint);
        markdown_core_summed_node *next = markdown_core_summed_next(&checkpoint->link);
        parser->rejoin_next = next ? markdown_core_checkpoint_of(next) : NULL;
        parser->rejoin_next_line = next ? parser->restart + next->own : 0;
        S_reopen(parser, checkpoint, parser->restart);
        return !parser->error;
    }
    markdown_core_summed_node *first = markdown_core_summed_first(&store->lines);
    parser->rejoin_next = first ? markdown_core_checkpoint_of(first) : NULL;
    parser->rejoin_next_line = first ? first->own : 0;
    parser->old_root = revision->previous;
    markdown_core_node *document = make_document(parser);
    parser->root = parser->block_root = parser->current = document;
    return document != NULL;
}

/* Hands every reference this parse's checkpoints make to `from`, an entry
 * of a node the rejoin retires, to `to`, the entry of the old node that
 * continues it, which `node` now is. */
static void S_redirect_entry(markdown_core_parser *parser, markdown_core_entry *from, markdown_core_entry *to,
                             markdown_core_node *node) {
    markdown_core_checkpoints *store = parser->revision->checkpoints;
    if (from) {
        for (markdown_core_checkpoint *taken = parser->taken; taken;
             taken = markdown_core_checkpoint_of(taken->link.up)) {
            for (markdown_core_frame *frame = taken->frame; frame; frame = frame->parent) {
                if (frame->entry == from) {
                    frame->entry = to;
                    markdown_core_entry_hold(to);
                    markdown_core_entry_drop(store, from);
                }
            }
            if (taken->below == from) {
                taken->below = to;
                markdown_core_entry_hold(to);
                markdown_core_entry_drop(store, from);
            }
        }
        for (markdown_core_frame *frame = parser->spine; frame; frame = frame->parent) {
            if (frame->entry == from) {
                frame->entry = to;
                markdown_core_entry_hold(to);
                markdown_core_entry_drop(store, from);
            }
        }
        from->node->entry = NULL;
        from->node = NULL;
    }
    if (to->node && to->node != node) {
        to->node->entry = NULL;
    }
    to->node = node;
    node->entry = to;
}

/* The old summaries, as children but the last, of the children of `cut`'s
 * container the parse replaced: `chain` and its old children through
 * `through` (none when NULL), all but the container's old last child. */
static void S_cut_drops(markdown_core_parser *parser, markdown_core_cut *cut, markdown_core_node *through) {
    markdown_core_node *node = cut->node;
    markdown_core_node *old_last = cut->holder.last_child ? cut->holder.last_child : cut->chain;
    if (!markdown_core_parser_structure(parser, node)->element->fold_child) {
        return;
    }
    uint32_t dropped = cut->chain && cut->chain != old_last ? cut->chain_fold : 0;
    for (markdown_core_node *child = through ? cut->holder.first_child : NULL; child; child = child->next) {
        if (child != old_last) {
            dropped += S_fold_child(parser, node, child, false);
        }
        if (child == through) {
            break;
        }
    }
    cut->dropped = dropped;
}

/* Where an old node starts in the old text, read from its entry. */
static inline size_t S_old_start(const markdown_core_node *node) { return markdown_core_entry_start(node->entry); }

/* THE STAND-IN CHECK (5.9): the new node `node`, the last child of the
 * container at its level, continues the old node `old` only when matching
 * would pair them: among the old children of `container` from `first`
 * (whose lead is measured from `anchor`; a relation's first from `base`,
 * the container's old start), `old`'s anchor has an image in the new text at
 * or after `node`'s start, and no old sibling of its kind before it in its
 * relation does. */
static bool S_continues(const markdown_core_parser *parser, const markdown_core_node *node,
                        const markdown_core_node *old, const markdown_core_node *container,
                        const markdown_core_node *first, size_t anchor, size_t base) {
    size_t from = node->where.place.start, image;
    bool taken = false;
    for (const markdown_core_node *sibling = first; sibling; sibling = sibling->next) {
        size_t start = (size_t)((int64_t)anchor + sibling->where.extent.lead);
        size_t end = start + sibling->where.extent.span;
        bool mapped = markdown_core_parser_image(parser, start, end, &image);
        if (sibling == old) {
            return !taken && mapped && image >= from;
        }
        taken = taken || (sibling->kind == old->kind && mapped && image >= from);
        anchor = end;
        if (markdown_core_parser_relation_ends(parser, container, sibling)) {
            anchor = base;
            taken = false;
        }
    }
    return false;
}

/* THE REJOIN (5.3): at the line start `line` of the new text, where the old
 * parse took `checkpoint` and the new spine (`line_frames`, read by the
 * caller) has its depth, kinds, flags and carried state. Levels 0 to `same`
 * hold the same containers on both sides; each deeper old container takes
 * the new value, fields and children of the container that continues it and
 * stands in for it (a stand-in cut), and the new node takes the old ones,
 * its children being the old children read again. Every container of the
 * spine then has the flags, carried state and end the old parse gave it,
 * and takes back the old children after the rejoin. False when the new parse does not continue the old one here. */
static bool S_rejoin(markdown_core_parser *parser, markdown_core_checkpoint *checkpoint, size_t line) {
    markdown_core_revision *revision = parser->revision;
    size_t depth = parser->line_frame_count;
    markdown_core_node *below = parser->line_point.below,
                       *old_below = checkpoint->below ? checkpoint->below->node : NULL;
    if (parser->line_point.marks != checkpoint->marks || (checkpoint->below && !old_below)) {
        return false;
    }
    if (below && (below->kind != old_below->kind || parser->line_point.below_bits != checkpoint->below_bits ||
                  (!(checkpoint->marks & MARKDOWN_CORE_CHECKPOINT_LEAF) && S_writable(parser, below)) ||
                  (below != old_below && below->entry && below->entry->placed))) {
        return false;
    }
    /* The old spine, by depth, beside the new one. */
    markdown_core_node **olds = markdown_core_alloc(depth + 1, 2 * sizeof(*olds));
    if (!olds) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    markdown_core_node **news = olds + depth + 1;
    size_t level = depth;
    markdown_core_frame *frame = checkpoint->frame;
    for (; frame && level; frame = frame->parent, level--) {
        const markdown_core_line_frame *now = &parser->line_frames[level - 1];
        markdown_core_node *old = frame->entry->node;
        if (!old || old->kind != now->node->kind || frame->bits != now->bits || frame->carry != now->carry) {
            break;
        }
        olds[level] = old;
        news[level] = now->node;
    }
    bool rejoins = !frame && !level;
    olds[0] = revision->previous;
    news[0] = parser->root;
    /* The deepest level whose container is the same on both sides, plus
     * one: 0 when even the root is new. */
    size_t same = 0;
    while (rejoins && same <= depth && olds[same] == news[same]) {
        same++;
    }
    for (size_t d = same; rejoins && d <= depth; d++) {
        markdown_core_node *old = olds[d], *node = news[d];
        rejoins = (!d || S_old_start(old) >= parser->restart) && (!node->entry || !node->entry->placed) &&
                  !markdown_core_attributes_owns(&node->attributes);
        if (rejoins && d) {
            const markdown_core_node *container = olds[d - 1], *first = container->first_child;
            size_t base = d > 1 ? S_old_start(container) : 0, anchor = base;
            if (d == same) {
                const markdown_core_cut *owner = &parser->cuts[d - 1];
                first = owner->holder.first_child;
                anchor = owner->anchor;
            }
            rejoins = S_continues(parser, node, old, container, first, anchor, base);
        }
    }
    if (!rejoins || !S_cuts_reserve(parser, parser->cut_count + depth + 1 - same)) {
        markdown_core_free(olds);
        return false;
    }
    /* The new parse's line `line` closes a leaf below the spine as the old
     * one's did. */
    if (below && (checkpoint->marks & MARKDOWN_CORE_CHECKPOINT_LEAF)) {
        parser->current = markdown_core_block_finalize(parser, below);
    }
    int64_t shift = parser->shift;
    markdown_core_cut *cuts = parser->cuts;
    /* What each level keeps from the old parse, read while the old tree is
     * whole: the old children after the old spine's child at that level
     * (`chained`), where they start, and the container's old last child. */
    markdown_core_node **chained = markdown_core_alloc(depth + 1, 3 * sizeof(*chained));
    if (!chained) {
        markdown_core_free(olds);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    markdown_core_node **suffixes = chained + depth + 1, **lasts = suffixes + depth + 1;
    uint32_t *starts = markdown_core_alloc(depth + 1, sizeof(*starts));
    if (!starts) {
        markdown_core_free(chained);
        markdown_core_free(olds);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    for (size_t d = 0; d <= depth; d++) {
        markdown_core_node *child = d < depth ? olds[d + 1] : old_below, *suffix;
        size_t base = d ? S_old_start(olds[d]) : 0, end = base;
        if (d < same) {
            markdown_core_cut *cut = &cuts[d];
            lasts[d] = cut->holder.last_child;
            if (!child || child == cut->chain) {
                suffix = cut->holder.first_child;
                end = cut->anchor;
                child = NULL;
            } else {
                suffix = child->next;
            }
            S_cut_drops(parser, cut, child);
        } else {
            lasts[d] = olds[d]->last_child;
            suffix = child ? child->next : olds[d]->first_child;
        }
        if (child && !markdown_core_parser_relation_ends(parser, olds[d], child)) {
            end = S_old_start(child) + child->where.extent.span;
        }
        chained[d] = child;
        suffixes[d] = suffix;
        starts[d] = suffix ? (uint32_t)((int64_t)end + suffix->where.extent.lead + shift) : 0;
    }
    /* The suffixes leave the old relations. */
    for (size_t d = 0; d <= depth; d++) {
        if (!suffixes[d]) {
            continue;
        }
        if (d < same && !chained[d]) {
            cuts[d].holder.first_child = cuts[d].holder.last_child = NULL;
        } else if (d < same) {
            chained[d]->next = NULL;
            cuts[d].holder.last_child = chained[d];
        } else if (chained[d]) {
            chained[d]->next = NULL;
            olds[d]->last_child = chained[d];
        } else {
            olds[d]->first_child = olds[d]->last_child = NULL;
        }
        suffixes[d]->prev = NULL;
    }
    /* The stand-ins, outermost first. */
    for (size_t d = same; d <= depth; d++) {
        markdown_core_node *old = olds[d], *node = news[d];
        markdown_core_cut *cut = &cuts[parser->cut_count++];
        size_t start = d ? S_old_start(old) : 0;
        *cut = (markdown_core_cut){.node = old,
                                   .old = node,
                                   .standin = true,
                                   .suffix = suffixes[d],
                                   .suffix_start = starts[d],
                                   .zone_end = d < depth ? olds[d + 1] : suffixes[d],
                                   .old_end = d < depth ? news[d + 1] : NULL,
                                   .extent = old->where.extent,
                                   .flags = old->flags,
                                   .end = (uint32_t)(start + old->where.extent.span),
                                   .base = (uint32_t)start,
                                   .anchor = (uint32_t)start};
        if (markdown_core_parser_structure(parser, old)->element->fold_child) {
            if (old->entry) {
                cut->sum = old->entry->sum;
                cut->last = old->entry->last;
            }
            for (markdown_core_node *child = old->first_child; child; child = child->next) {
                if (child != lasts[d]) {
                    cut->dropped += S_fold_child(parser, old, child, false);
                }
            }
        }
    }
    for (size_t d = same; d <= depth; d++) {
        markdown_core_node *old = olds[d], *node = news[d];
        markdown_core_cut *cut = &cuts[parser->cut_count - (depth + 1 - d)];
        markdown_core_node_swap_values(node, old);
        markdown_core_node_swap_fields(node, old);
        markdown_core_node *first = node->first_child, *last = node->last_child;
        node->first_child = old->first_child;
        node->last_child = old->last_child;
        old->first_child = first;
        old->last_child = last;
        for (markdown_core_node *child = old->first_child; child; child = child->next) {
            child->parent = old;
        }
        for (markdown_core_node *child = node->first_child; child; child = child->next) {
            child->parent = node;
        }
        if (d) {
            /* `node` is the last child of the container that now holds the
             * new children at the level above, and `old` the last of the old
             * relation there. */
            markdown_core_node *owner = d == same ? cuts[d - 1].node : olds[d - 1];
            markdown_core_node *held = d == same ? &cuts[d - 1].holder : news[d - 1];
            markdown_core_node *before = node->prev, *old_before = old->prev;
            if (before) {
                before->next = old;
            } else {
                owner->first_child = old;
            }
            owner->last_child = old;
            old->prev = before;
            old->parent = owner;
            if (old_before) {
                old_before->next = node;
            } else {
                held->first_child = node;
            }
            held->last_child = node;
            node->prev = old_before;
            node->parent = d == same ? cuts[d - 1].node : news[d - 1];
        } else {
            parser->root = parser->block_root = old;
            parser->old_root = node;
        }
        node->id = old->id;
        if (old->entry) {
            S_redirect_entry(parser, node->entry, old->entry, old);
        }
        old->flags = (uint16_t)(cut->flags & MARKDOWN_CORE_CARRIED_BITS);
        if (d) {
            S_carry_restore(parser, old, S_carry_save(parser, node));
        }
        old->where.place.end = (uint32_t)((int64_t)cut->end + shift);
    }
    /* The reopened containers still open, as the old parse closed them. */
    for (size_t d = 0; d < same; d++) {
        markdown_core_cut *cut = &cuts[d];
        markdown_core_node *node = cut->node;
        cut->suffix = suffixes[d];
        cut->suffix_start = starts[d];
        cut->zone_end = d + 1 == same && same <= depth ? olds[same] : suffixes[d];
        cut->old_end = d + 1 == same && same <= depth ? news[same] : NULL;
        node->flags = (uint16_t)(cut->flags & MARKDOWN_CORE_CARRIED_BITS);
        if (d) {
            S_carry_restore(parser, node, cut->carry);
        }
        node->where.place.end = (uint32_t)((int64_t)cut->end + shift);
    }
    /* Every container of the spine takes back its old children after the
     * rejoin, after the new ones. */
    for (size_t d = 0; d <= depth; d++) {
        markdown_core_node *owner = d < same ? cuts[d].node : olds[d];
        if (suffixes[d]) {
            if (owner->last_child) {
                owner->last_child->next = suffixes[d];
            } else {
                owner->first_child = suffixes[d];
            }
            suffixes[d]->prev = owner->last_child;
            owner->last_child = lasts[d];
        }
    }
    /* The block below: the new one takes the old one's entry, and the blank
     * lines after it that the old parse marked on it. */
    if (below && below != old_below) {
        below->flags = (uint16_t)((below->flags & ~MARKDOWN_CORE_NODE__LAST_LINE_BLANK) |
                                  (old_below->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK));
        S_redirect_entry(parser, below->entry, checkpoint->below, below);
    }
    /* Reopened containers the parse closed keep none of their old children. */
    for (size_t d = same; d < parser->cut_reopened; d++) {
        S_cut_drops(parser, &cuts[d], cuts[d].holder.last_child);
    }
    parser->rejoin = checkpoint;
    (void)line;
    markdown_core_free(starts);
    markdown_core_free(chained);
    markdown_core_free(olds);
    return true;
}

/* A line start the driver is about to read: whether the parse rejoins the
 * old one there. The edits not yet covered whose restart would come before
 * it are covered first. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool S_try_rejoin(markdown_core_parser *parser, size_t index) {
    size_t line = parser->input_scanned;
    if (index != parser->input_line_count || line >= parser->input_length) {
        return false;
    }
    size_t old_line = (size_t)((int64_t)line - parser->shift);
    while (parser->next_restart <= old_line) {
        if (!S_cover_edit(parser)) {
            return false;
        }
        if ((size_t)parser->last_line_end < parser->rejoin_from) {
            return false;
        }
        old_line = (size_t)((int64_t)line - parser->shift);
    }
    if (old_line < parser->rejoin_old) {
        return false;
    }
    while (parser->rejoin_next && parser->rejoin_next_line < old_line) {
        markdown_core_summed_node *next = markdown_core_summed_next(&parser->rejoin_next->link);
        parser->rejoin_next = next ? markdown_core_checkpoint_of(next) : NULL;
        parser->rejoin_next_line += next ? next->own : 0;
    }
    markdown_core_checkpoint *checkpoint = parser->rejoin_next;
    if (!checkpoint || parser->rejoin_next_line != old_line || checkpoint->frontier) {
        return false;
    }
    /* The rejoin moves this parse's blocks into the old ones' places, so the
     * ledger's checkpoints are made first; the replay reads its spines into
     * `line_frames`, which then take this line start's. */
    if (parser->line_state != MARKDOWN_CORE_LINE_UNKEPT) {
        S_replay(parser);
    }
    if (parser->error || !S_read_spine(parser)) {
        return false;
    }
    size_t depth = 0;
    for (markdown_core_frame *frame = checkpoint->frame; frame; frame = frame->parent) {
        depth++;
    }
    if (depth != parser->line_frame_count) {
        return false;
    }
    return S_rejoin(parser, checkpoint, line);
}

/* THE DRIVER: the active input's lines, from the line after the current one,
 * each handed to the block parser, and the document's prefix first when the
 * input is the document read from its first byte. */
static void S_parse_source(markdown_core_parser *parser) {
    bool at_root = parser->block_root == parser->root;
    if (at_root && parser->input_scanned == 0) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->read_document_prefix(document, parser);
    }
    while (!parser->error) {
        size_t index = (size_t)(parser->line_number + 1 - parser->input_first_line);
        if ((size_t)parser->last_line_end >= parser->rejoin_from && S_try_rejoin(parser, index)) {
            parser->rejoin_from = SIZE_MAX;
            break;
        }
        if (parser->line_state != MARKDOWN_CORE_LINE_UNKEPT) {
            parser->line_state = MARKDOWN_CORE_LINE_PLAIN;
            parser->line_frontier = parser->input_scanned + parser->input_ended;
            parser->line_current = parser->current;
            parser->line_mark = parser->undo_records.count;
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
        parser->line_start = (bufsize_t)found->start;
        /* A document line's scan recorded where its content ends; a cell's
         * line ends where the cell's map places its last byte. */
        if (at_root) {
            parser->line_end = (bufsize_t)found->end;
        } else {
            parser->line_end = markdown_core_parser_mapped_source_end(parser, parser->line_number + 1, content_length);
        }
        S_process_line(parser, content, content_length);
        if (parser->line_state == MARKDOWN_CORE_LINE_OPENED) {
            S_keep_line(parser);
        }
        if (parser->claimed_line) {
            assert(parser->claimed_line >= parser->line_number);
            parser->line_number = parser->claimed_line;
            parser->line_start = (bufsize_t)markdown_core_parser_visited_line(parser, parser->line_number)->start;
            parser->last_line_end = parser->claimed_last_end;
            parser->claimed_line = 0;
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

static bool S_last_child_is_open(markdown_core_node *container) {
    return container->last_child && (container->last_child->flags & MARKDOWN_CORE_NODE__OPEN);
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
     * on the line before, and the container ends here.
     *
     * `parser->current` is the deepest open block and `container` is on the
     * path from the root to it, so walking up through `markdown_core_block_finalize` reaches it.
     * Definition-only paragraphs stay attached until block parsing completes;
     * no block can disappear while it is still on the open spine. */
    *should_continue = false;
    while (parser->current != container) {
        parser->current = markdown_core_block_finalize(parser, parser->current);
        assert(parser->current != NULL);
    }
    /* A block survives its own finalization and can still be positioned. */
    assert(!(kind->flags & MARKDOWN_CORE_KIND_IS_PARAGRAPH));
    markdown_core_parser_set_flags(parser, container,
                                   (uint16_t)(container->flags | MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION));
    parser->current = markdown_core_block_finalize(parser, container);
    markdown_core_block_set_end_to_current_line(parser, container);
    return false;
}

static markdown_core_node *check_open_blocks(markdown_core_parser *parser, markdown_core_chunk *input,
                                             bool *all_matched) {
    bool should_continue = true;
    *all_matched = false;
    markdown_core_node *container = parser->block_root;
    markdown_core_node *closing = NULL;

    while (S_last_child_is_open(container)) {
        container = container->last_child;
        parser->line_reached = container;

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
                parser->line_reached = parser->current;
                if (S_kind_accepts_lines(markdown_core_parser_kind(parser, parser->current), parser->current)) {
                    markdown_core_block_add_line(parser->current, input, parser);
                }
                return NULL;
            }
        }

        /* Whatever this container's prefix consumed is that container's
         * MARKER: `> ` belongs to the block quote, the item's indent to the
         * list item. One claim per container, walking down the spine. */
    }

    *all_matched = true;

done:
    if (closing) {
        while (parser->current != closing) {
            parser->current = markdown_core_block_finalize(parser, parser->current);
        }
        parser->current = markdown_core_block_finalize(parser, closing);
        markdown_core_block_set_end_to_current_line(parser, closing);
        return NULL;
    }
    /* A container whose prefix consumed bytes and then declined still read
     * them; they are its marker up to the point it gave up. */
    if (!*all_matched) {
        container = container->parent; // back up to last matching node
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
    markdown_core_node **chain;
    markdown_core_node_internal_flags *flags;

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
    flags = markdown_core_realloc(parser->lookahead_chain_flags, (size_t)capacity * sizeof(*flags));
    if (!flags) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->lookahead_chain_flags = flags;
    parser->lookahead_chain_alloc = capacity;
    return true;
}

static void S_lookahead_close_run(markdown_core_block_lookahead *lookahead, int line) {
    if (lookahead->run_start) {
        markdown_core_line_facts *entry = markdown_core_parser_get_line_facts(lookahead->parser, lookahead->run_start);
        if (entry) {
            entry->run_end = line;
        }
        lookahead->run_start = 0;
    }
}

/* Whether the containers chain[from .. depth) accept every blank line. */
static bool S_lookahead_extras_accept_blank(const markdown_core_parser *parser, int from, int depth) {
    int i;
    for (i = from; i < depth; i++) {
        if (!(markdown_core_parser_kind(parser, parser->lookahead_chain[i])->flags & MARKDOWN_CORE_KIND_BLANK_RUNS)) {
            return false;
        }
    }
    return true;
}

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, markdown_core_node *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead) {
    markdown_core_node *parent = parent_container;
    markdown_core_node *node;
    int depth = 0;
    int i;

    memset(lookahead, 0, sizeof(*lookahead));
    /* The block joins the nearest open container that can hold it, which is
     * where `markdown_core_parser_add_child` backs up to when it is opened. */
    while (parent->parent && !markdown_core_node_can_contain_type(parent, child)) {
        parent = parent->parent;
    }
    for (node = parent; node; node = node == parser->block_root ? NULL : node->parent) {
        depth++;
    }
    if (!S_lookahead_reserve_chain(parser, depth)) {
        return false;
    }
    i = depth;
    for (node = parent; node; node = node == parser->block_root ? NULL : node->parent) {
        i--;
        parser->lookahead_chain[i] = node;
        parser->lookahead_chain_flags[i] = node->flags;
    }

    lookahead->parser = parser;
    lookahead->parent = parent;
    lookahead->depth = depth;
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
    while (!parser->error) {
        size_t next;
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
            break;
        }
        bufsize_t content_length;
        const unsigned char *content = S_input_line_content(parser, geometry, &content_length);
        if (!content) {
            return 0;
        }
        next = markdown_core_input_line_next(parser, geometry);
        parser->block_lookahead_work++;
        if (geometry->facts && parser->input_facts[geometry->facts - 1].nul_count) {
            input.data = (unsigned char *)content;
            input.len = content_length + 1;
        } else if (next == parser->input_length) {
            /* The input's last line, normalized once: the matchers read a line
             * through its terminator, and the source may not end in one. */
            if (!parser->lookahead_last_line_ready) {
                markdown_core_strbuf_set(&parser->lookahead_last_line, content, content_length);
                markdown_core_strbuf_putc(&parser->lookahead_last_line, '\n');
                if (parser->lookahead_last_line.oom) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    return 0;
                }
                parser->lookahead_last_line_ready = true;
            }
            input.data = parser->lookahead_last_line.ptr;
            input.len = parser->lookahead_last_line.size;
        } else {
            input.data = (unsigned char *)markdown_core_parser_input_at(parser, geometry->start);
            input.len = (bufsize_t)(next - geometry->start);
        }
        input.alloc = 0;
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
            parser->lookahead_chain[entry->depth] == entry->container) {
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
            markdown_core_node *container = parser->lookahead_chain[i];
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
            S_lookahead_close_run(lookahead, this_line);
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
                lookahead->line = entry->run_end;
            }
            continue;
        }
        S_lookahead_close_run(lookahead, this_line);
        *line = input;
        *first_nonspace = parser->first_nonspace;
        *indent = parser->indent;
        return 1;
    }
    S_lookahead_close_run(lookahead, lookahead->line);
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
        markdown_core_node *node = parser->lookahead_chain[i];
        const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
        unsigned mask = structure ? structure->element->speculative_flags : 0;
        node->flags =
            (markdown_core_node_internal_flags)((node->flags & ~mask) | (parser->lookahead_chain_flags[i] & mask));
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
                    if (parser->claimed_line) {
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
        if (S_kind_accepts_lines(kind, *container) || (kind->flags & MARKDOWN_CORE_KIND_PROSE)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        maybe_lazy = false;
    }
}

/* The line's one text step, part of the line's processing. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void add_text_to_container(
    markdown_core_parser *parser, markdown_core_node *container, markdown_core_node *last_matched_container,
    markdown_core_chunk *input) {
    markdown_core_node *tmp;
    // what remains at parser->offset is a text line.  add the text to the
    // appropriate container.

    markdown_core_block_find_first_nonspace(parser, input);

    /* A blank line ends the container's last child blank, a closed one the
     * finish stage may already have asked included: its answer says so too. */
    if (parser->blank && container->last_child) {
        S_set_last_line_blank(parser, container->last_child, true);
        if (S_last_line_checked(container->last_child)) {
            S_set_ends_blank(container->last_child, true);
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

    S_set_last_line_blank(parser, container, last_line_blank);

    tmp = container;
    while (tmp != parser->block_root && tmp->parent) {
        S_set_last_line_blank(parser, tmp->parent, false);
        tmp = tmp->parent;
    }

    // A line that may be lazy, opened no block and is not blank is a lazy
    // line: the current block takes it.
    if (container == last_matched_container && !parser->blank && S_may_be_lazy(parser, last_matched_container)) {
        const markdown_core_element_instance *current = markdown_core_parser_structure(parser, parser->current);
        markdown_core_node *lazy = current->element->open_lazy(current, parser, parser->current, input);
        if (!lazy) {
            return;
        }
        parser->current = lazy;
        /* A lazy line is text, so its indentation is not content, as it is not
         * on a line that continues a paragraph with every prefix. */
        markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
        markdown_core_block_add_line(parser->current, input, parser);
    } else { // not a lazy continuation
        // Finalize any blocks that were not matched and set cur to container:
        markdown_core_parser_finalize_unmatched_blocks(parser);

        if (accepts_lines) {
            markdown_core_block_add_line(container, input, parser);
            if (structure->element->ends_block && structure->element->ends_block(structure, parser, container, input)) {
                markdown_core_parser_set_flags(
                    parser, container, (uint16_t)(container->flags | MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION));
                container = markdown_core_block_finalize(parser, container);
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

        parser->current = container;
    }
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
/* A line of the active input, processed where the driver reads it. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_process_line(markdown_core_parser *parser,
                                                                           const unsigned char *buffer,
                                                                           bufsize_t bytes) {
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
    parser->matched_container = last_matched_container;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->error) {
        goto finished;
    }

    if (parser->claimed_line) {
        parser->current = container->flags & MARKDOWN_CORE_NODE__OPEN ? container : container->parent;
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

/* THE FINISH STAGE'S ONE WALK. The content tree holds every definition
 * where it was written -- a block definition as a child, an inline note as
 * its Citation's field -- so the one walk reaches them all. Every root is
 * walked once, with inline parsing and completion at each ENTER, the finish steps
 * at the events they declared, and `phase` run on each root as its walk
 * completes -- the structural check. The global passes come later, after
 * finalization, on the roots `record` collects (finish_roots). A root is
 * rewritten in place throughout; none of them is ever substituted. */
static int S_apply_tree_phase(markdown_core_parser *parser, markdown_core_node *root, tree_phase_func phase,
                              void *context, finish_roots *record) {
    if (!root || parser->error) {
        return !parser->error;
    }
    assert(root == parser->root && root->kind == MARKDOWN_CORE_NODE_DOCUMENT);
    parser->nodes_created_before_finish = parser->nodes_created;
    parser->nodes_freed_before_finish = parser->nodes_freed;
    return walk_owned_trees(parser, &root, 1, true, phase, context, record);
}

static int S_check_root(markdown_core_parser *parser, markdown_core_node *root, void *context);

typedef struct {
    markdown_core_node **roots;
    size_t count, capacity;
    bool failed;
} zone_roots;

static int S_add_root(markdown_core_node **slot, void *context) {
    zone_roots *roots = context;
    if (!slot || !*slot) {
        return 1;
    }
    void *grown = markdown_core_reserve(roots->roots, &roots->capacity, roots->count + 1, sizeof(*roots->roots));
    if (!grown) {
        roots->failed = true;
        return 0;
    }
    roots->roots = grown;
    roots->roots[roots->count++] = *slot;
    return 1;
}

/* A field root the parse made: it has no id yet. */
static int S_add_new_root(markdown_core_node **slot, void *context) {
    return !slot || !*slot || (*slot)->id || S_add_root(slot, context);
}

/* THE FINISH WALK OF A RESTART: the blocks the parse read, in source order,
 * each walked as part of the tree that holds it -- the new children of the
 * reopened containers, the deepest first, each followed by the fields the
 * parse wrote into it, and then of each stand-in, the outermost first, after
 * its fields. The containers themselves were finished by the old parse, or
 * take a new value whose fields are its only part to finish (the rejoin
 * admits no stand-in with attributes); their folds are redone from their
 * cuts (S_refold). */
static bool S_finish_zone(markdown_core_parser *parser) {
    zone_roots roots = {NULL, 0, 0, false};
    zone_roots fields = {NULL, 0, 0, false};
    bool ok = true;
    for (size_t i = parser->cut_reopened; ok && i--;) {
        const markdown_core_cut *cut = &parser->cuts[i];
        fields.count = roots.count = 0;
        for (markdown_core_node *child = markdown_core_cut_first(cut); child != cut->zone_end; child = child->next) {
            S_add_root(&child, &roots);
        }
        ok = !roots.failed && walk_owned_trees(parser, roots.roots, roots.count, false, NULL, NULL, NULL) &&
             markdown_core_visit_inline_subtrees(cut->node, S_add_new_root, &fields) && !fields.failed &&
             walk_owned_trees(parser, fields.roots, fields.count, true, S_check_root, NULL, NULL);
    }
    for (size_t i = parser->cut_reopened; ok && i < parser->cut_count; i++) {
        const markdown_core_cut *cut = &parser->cuts[i];
        fields.count = roots.count = 0;
        ok = markdown_core_visit_inline_subtrees(cut->node, S_add_root, &fields) && !fields.failed &&
             walk_owned_trees(parser, fields.roots, fields.count, true, S_check_root, NULL, NULL);
        for (markdown_core_node *child = cut->node->first_child; ok && child != cut->zone_end; child = child->next) {
            S_add_root(&child, &roots);
        }
        ok = ok && !roots.failed && walk_owned_trees(parser, roots.roots, roots.count, false, NULL, NULL, NULL);
    }
    markdown_core_free(roots.roots);
    markdown_core_free(fields.roots);
    return ok;
}

/* THE REFOLD (E4) of a cut's container, from the fold the old parse left:
 * the summaries of the children the parse replaced leave the sum and those
 * of the children it read join it; the last child's is the old one when the
 * suffix holds it. */
static void S_refold_cut(markdown_core_parser *parser, const markdown_core_cut *cut) {
    markdown_core_node *node = cut->node;
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
    if (!structure->element->fold_child) {
        return;
    }
    markdown_core_node *last = cut->suffix ? NULL : node->last_child;
    uint32_t sum = cut->sum - cut->dropped;
    for (markdown_core_node *child = cut->chain ? cut->chain : node->first_child; child && child != cut->suffix;
         child = child->next) {
        if (child != last) {
            sum += structure->element->fold_child(structure, parser, node, child, false);
        }
    }
    uint32_t end = cut->suffix ? cut->last
                   : last      ? structure->element->fold_child(structure, parser, node, last, true)
                               : 0;
    if (node->entry) {
        node->entry->sum = sum;
        node->entry->last = end;
    }
    if (structure->element->fold_apply) {
        structure->element->fold_apply(structure, parser, node, sum, end);
    }
}

/* The cuts' containers, each after every cut inside it: the stand-ins from
 * the innermost, then the reopened containers from the innermost. */
static void S_refold(markdown_core_parser *parser) {
    for (size_t i = parser->cut_count; i-- > parser->cut_reopened;) {
        S_refold_cut(parser, &parser->cuts[i]);
    }
    for (size_t i = parser->cut_reopened; i--;) {
        S_refold_cut(parser, &parser->cuts[i]);
    }
}

bool markdown_core_parser_register_definition(markdown_core_parser *parser,
                                              markdown_core_definition_collection *collection,
                                              markdown_core_node *definition) {
    assert(definition && collection && definition->parent);
    markdown_core_parser_touch(parser, definition);
    if (collection->count == collection->capacity) {
        size_t capacity = collection->capacity ? collection->capacity * 2 : 8;
        markdown_core_node **values;
        if (capacity > SIZE_MAX / sizeof(*values)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        values = markdown_core_realloc(collection->values, capacity * sizeof(*values));
        if (!values) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        collection->values = values;
        collection->capacity = capacity;
    }
    collection->values[collection->count++] = definition;
    parser->definition_registration_work++;
    return true;
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

int markdown_core_block_order_definitions(markdown_core_parser *parser,
                                          markdown_core_definition_collection *collection) {
    return markdown_core_order_source_entries(&parser->source_order, collection->values, collection->count,
                                              sizeof(*collection->values), markdown_core_source_key);
}

/* THE FINISH STAGE IS ONE WALK, NOT ONE PER PHASE.
 *
 * Every owned root of the document is traversed exactly once here, and
 * everything the finish stage does to a node happens from inside that one
 * traversal: inline completion at a node's ENTER, text consolidation at a
 * Text's EXIT, and every finish step at the events of the kinds it declared.
 * It used to be four traversals -- one to find the roots, and then one each
 * for consolidation, autolink and formula, every one of them a private
 * iterator over the whole root -- and before that one more per pass just to
 * enumerate the roots again; and the inline stage walked every root once
 * more before any of them, to complete the nodes.
 *
 * What makes fusing them safe is the order the walk fixes. A Text's EXIT
 * comes after every Text sibling it will absorb has been produced and before
 * any step reads it, so a step at that EXIT sees the merged run, exactly as
 * the separate autolink walk saw it after the separate consolidation walk. A
 * Paragraph's EXIT comes after every child's EXIT, so formula's test of the
 * children sees what consolidation left. And an owned root nested in a node
 * -- a definition term, a citation's prefix -- is finished, popped and gone
 * from the stack before that node's own children are visited, so nothing
 * still on the stack lives inside a tree a step or pass is rewriting: a pass
 * may free an owned root, a Cite owns its citations' prefix and suffix, and
 * there is no later replay to hand that freed root to.
 *
 * The global passes -- external elements that need a whole finished root --
 * still run at the point where each root's walk completes, in descriptor
 * order, and that list is chosen before the walk starts, which it can be only
 * because the record it reads is written where kinds are produced rather than
 * gathered by a walk. */
typedef struct {
    const markdown_core_element_instance **passes;
    size_t pass_count;
} finish_phases;

/* THE GATE: a hook that declares the kinds it acts on runs only in a parse
 * that produced one of them. The declared kinds are read here rather than
 * stored as a bit set on the descriptor, so each keeps the namespace that
 * tells a block from an inline; the list is a handful of entries per element,
 * read once per parse. */
static bool S_finish_hook_selected(const markdown_core_parser *parser, const markdown_core_element *element) {
    markdown_core_node_kind_set declared = {0, 0};
    if (!element->finish_acts_on_kinds) {
        return true;
    }
    for (const markdown_core_node_type *kind = element->finish_acts_on_kinds; *kind; kind++) {
        markdown_core_node_kind_set_add(&declared, *kind);
    }
    return markdown_core_node_kind_set_intersects(&declared, &parser->kinds_created);
}

/* Every writer of this tree is checked.
 *
 * `markdown_core_node_check` is the one structural self-check this tree has.
 * The finish walk rewrites the tree from inside -- consolidation and every
 * step unlink, relink and free nodes -- so it is checked once per root when
 * the walk completes, and then again after each global pass, which rewrites
 * the whole root it is handed. A break introduced by a step is attributed to
 * the root's walk rather than to the step: the steps are interleaved, and a
 * per-step check would cost a traversal per step, which is the shape this
 * stage exists not to have. This is compiled in only when
 * `MARKDOWN_CORE_DEBUG_NODES` is defined, which no shipping configuration
 * defines, so the per-root cost is not a release cost. */
#if MARKDOWN_CORE_DEBUG_NODES
#define MARKDOWN_CORE_CHECK_TREE(root)                                                                                 \
    do {                                                                                                               \
        if (markdown_core_node_check((root), stderr) != 0) {                                                           \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)
#else
#define MARKDOWN_CORE_CHECK_TREE(root) ((void)0)
#endif

static int S_check_root(markdown_core_parser *parser, markdown_core_node *root, void *context) {
    (void)parser;
    (void)context;
    MARKDOWN_CORE_CHECK_TREE(root);
    return 1;
}

/* THE GLOBAL PASSES, on one root, after the document's finalization: each
 * pass receives the finalized root and walks it itself. */
static int S_run_passes(markdown_core_parser *parser, markdown_core_node *root, const finish_phases *phases) {
    for (size_t i = 0; i < phases->pass_count; i++) {
        const markdown_core_element_instance *pass = phases->passes[i];
        if (!pass->element->postprocess_func(pass, parser, root) || parser->error) {
            return 0;
        }
        MARKDOWN_CORE_CHECK_TREE(root);
    }
    return 1;
}

/* THE CHECKPOINTS A PARSE TOOK BECOME THE SESSION'S, once its tree is
 * finished and before it is published, while every block still holds its
 * place. An entry is placed where its block starts, or where the block
 * started when the entry was made if the block is gone; the replay makes
 * the entries in source order, and a stable sort keeps blocks that start
 * together in the order they opened in. */
/* How deep an entry's block is in its tree, or 0 when it is gone. */
static size_t S_entry_depth(const markdown_core_summed_node *link) {
    size_t depth = 0;
    for (const markdown_core_node *node = markdown_core_entry_of((markdown_core_summed_node *)link)->node;
         node && node->parent; node = node->parent) {
        depth++;
    }
    return depth;
}

/* Whether entry `a` goes before entry `b`: by where its block starts, and,
 * for blocks that start together, a container before what it holds. */
static bool S_entry_before(const markdown_core_summed_node *a, const markdown_core_summed_node *b) {
    return a->own != b->own ? a->own < b->own : S_entry_depth(a) < S_entry_depth(b);
}

/* The end of the run of entries in source order (S_entry_before) that
 * starts at `from`. */
static size_t S_entry_run(markdown_core_summed_node *const *links, size_t from, size_t count) {
    size_t end = from + 1;
    while (end < count && !S_entry_before(links[end], links[end - 1])) {
        end++;
    }
    return end;
}

/* Sorts `count` entries' links into source order (S_entry_before), stably:
 * each pass merges pairs of adjacent runs through `scratch`, which has room
 * for as many, until one run holds them all. */
static void S_sort_entries(markdown_core_summed_node **links, markdown_core_summed_node **scratch, size_t count) {
    markdown_core_summed_node **from = links, **to = scratch;
    while (count && S_entry_run(from, 0, count) < count) {
        for (size_t left = 0; left < count;) {
            size_t middle = S_entry_run(from, left, count);
            size_t right = middle < count ? S_entry_run(from, middle, count) : count;
            size_t i = left, j = middle, k = left;
            while (i < middle && j < right) {
                to[k++] = S_entry_before(from[j], from[i]) ? from[j++] : from[i++];
            }
            while (i < middle) {
                to[k++] = from[i++];
            }
            while (j < right) {
                to[k++] = from[j++];
            }
            left = right;
        }
        markdown_core_summed_node **swap = from;
        from = to;
        to = swap;
    }
    if (from != links) {
        memcpy(links, from, count * sizeof(*links));
    }
}

/* THE CHECKPOINTS OF A RESTART become the session's in place of the old
 * parse's from the restart to the rejoin (or to the end): those after the
 * rejoin move by the edits' length change, and the entries of the blocks
 * open across the rejoin move to where their new values start. An entry
 * only the parse holds is no checkpoint's, and goes. */
static void S_splice_checkpoints(markdown_core_parser *parser) {
    markdown_core_checkpoints *store = parser->revision->checkpoints;
    markdown_core_checkpoint *rejoin = parser->rejoin;
    size_t restart = parser->restart;
    int64_t shift = parser->shift;
    markdown_core_summed_node *link =
        parser->restarted ? &parser->restarted->link : markdown_core_summed_first(&store->lines);
    while (link && (!rejoin || link != &rejoin->link)) {
        markdown_core_summed_node *next = markdown_core_summed_next(link);
        markdown_core_summed_take(&store->lines, link);
        markdown_core_checkpoint_free(store, markdown_core_checkpoint_of(link));
        link = next;
    }
    if (rejoin) {
        rejoin->link.own = (size_t)((int64_t)rejoin->link.own + shift);
        markdown_core_summed_refresh(&rejoin->link);
    }
    /* The parse's checkpoints, and its entries, which are chained newest
     * first. */
    size_t made = parser->entered_count, count = parser->taken_count + 3 * made + 2 * (parser->cut_count + 1);
    markdown_core_summed_node **links = markdown_core_realloc(NULL, count * sizeof(*links));
    if (!links) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    size_t at = parser->taken_count;
    for (markdown_core_checkpoint *taken = parser->taken; taken; taken = markdown_core_checkpoint_of(taken->link.up)) {
        links[--at] = &taken->link;
    }
    for (size_t i = 0; i < parser->taken_count; i++) {
        markdown_core_summed_put(&store->lines, links[i], links[i]->own);
    }
    parser->taken = NULL;
    parser->taken_count = 0;
    markdown_core_entry **entered = (markdown_core_entry **)(void *)links;
    at = made;
    for (markdown_core_entry *entry = parser->entered; entry; entry = markdown_core_entry_of(entry->link.up)) {
        entered[--at] = entry;
    }
    parser->entered = NULL;
    parser->entered_count = 0;
    markdown_core_summed_node **placing = links + made;
    /* The entries that move: the stand-ins' and the block below's. */
    size_t placed = 0;
    for (size_t i = parser->cut_reopened; rejoin && i <= parser->cut_count; i++) {
        markdown_core_entry *entry = i < parser->cut_count ? parser->cuts[i].node->entry : rejoin->below;
        if (entry && entry->placed && entry->node) {
            markdown_core_summed_take(&store->entries, &entry->link);
            entry->placed = false;
            entry->link.own = entry->node->where.place.start;
            placing[placed++] = &entry->link;
        }
    }
    if (rejoin) {
        markdown_core_summed_node *before =
            restart ? markdown_core_summed_last_through(&store->entries, restart - 1) : NULL;
        markdown_core_summed_node *after =
            before ? markdown_core_summed_next(before) : markdown_core_summed_first(&store->entries);
        if (after) {
            after->own = (size_t)((int64_t)after->own + shift);
            markdown_core_summed_refresh(after);
        }
    }
    for (size_t i = 0; i < made; i++) {
        markdown_core_entry *entry = entered[i];
        if (entry->holds > 1) {
            entry->link.own = entry->node ? entry->node->where.place.start : entry->link.own;
            placing[placed++] = &entry->link;
        }
    }
    S_sort_entries(placing, placing + placed, placed);
    for (size_t i = 0; i < placed; i++) {
        markdown_core_summed_put(&store->entries, placing[i], placing[i]->own);
        markdown_core_entry_of(placing[i])->placed = true;
    }
    /* The checkpoints hold the entries now. */
    for (size_t i = 0; i < made; i++) {
        markdown_core_entry_drop(store, entered[i]);
    }
    store->touched_start = parser->touched_start;
    store->touched_end = parser->touched_end;
    markdown_core_free(links);
}

static void S_commit_checkpoints(markdown_core_parser *parser) {
    markdown_core_checkpoints *store = parser->revision->checkpoints;
    if (!store) {
        return;
    }
    if (parser->restarted || parser->rejoin) {
        S_splice_checkpoints(parser);
        return;
    }
    markdown_core_checkpoints_clear(store);
    store->touched_start = parser->touched_start;
    store->touched_end = parser->touched_end;
    size_t count = parser->entered_count > parser->taken_count ? parser->entered_count : parser->taken_count;
    if (!count) {
        return;
    }
    markdown_core_summed_node **links = markdown_core_alloc(count, 2 * sizeof(*links));
    if (!links) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    /* The chains run newest first, so each fills its array from the end. */
    size_t at = parser->taken_count;
    for (markdown_core_checkpoint *taken = parser->taken; taken; taken = markdown_core_checkpoint_of(taken->link.up)) {
        links[--at] = &taken->link;
    }
    size_t line = 0;
    for (size_t i = 0; i < parser->taken_count; i++) {
        size_t start = links[i]->own;
        links[i]->own = start - line;
        line = start;
    }
    markdown_core_summed_build(&store->lines, links, parser->taken_count);
    parser->taken = NULL;
    parser->taken_count = 0;
    at = parser->entered_count;
    for (markdown_core_entry *entry = parser->entered; entry; entry = markdown_core_entry_of(entry->link.up)) {
        if (entry->node) {
            entry->link.own = entry->node->where.place.start;
        }
        links[--at] = &entry->link;
    }
    S_sort_entries(links, links + count, parser->entered_count);
    size_t start = 0;
    for (size_t i = 0; i < parser->entered_count; i++) {
        size_t own = links[i]->own;
        links[i]->own = own - start;
        start = own;
        markdown_core_entry_of(links[i])->placed = true;
    }
    markdown_core_summed_build(&store->entries, links, parser->entered_count);
    /* The checkpoints hold the entries now. */
    for (size_t i = 0; i < parser->entered_count; i++) {
        markdown_core_entry_drop(store, markdown_core_entry_of(links[i]));
    }
    parser->entered = NULL;
    parser->entered_count = 0;
    markdown_core_free(links);
}

static markdown_core_node *S_finish_parse(markdown_core_parser *parser) {
    markdown_core_node *res;

    if (parser->root == NULL || parser->error) {
        goto failed;
    }
    /* The ledger's checkpoints are made from the state the parse ends in. */
    if (parser->line_state != MARKDOWN_CORE_LINE_UNKEPT) {
        S_replay(parser);
        parser->line_state = MARKDOWN_CORE_LINE_UNKEPT;
    }

    /* A parse that rejoined the old one has the spine as the old parse
     * closed it; one that read to the end closes it, and covers every edit,
     * and its reopened containers keep none of their old children. */
    if (!parser->rejoin) {
        finalize_document(parser);
        while (parser->applied < parser->revision->edit_count && S_cover_edit(parser)) {
        }
        for (size_t i = 0; i < parser->cut_reopened; i++) {
            S_cut_drops(parser, &parser->cuts[i], parser->cuts[i].holder.last_child);
        }
    }
    S_parse_block_inputs(parser);
    if (!parser->error) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->prepare_document(document, parser);
    }
    if (parser->error) {
        goto failed;
    }

    /* THE ONE WALK, then the passes it may have earned.
     *
     * The walk parses each container's inline content at that container's
     * ENTER, completes every node, consolidates every Text run and runs the
     * element steps at the events they declared (S_apply_tree_phase). A
     * step's gate is read at each event it is asked at, since a kind the
     * step acts on may first be made by the walk itself; a pass's gate is
     * read once the walk is done, when the record is complete.
     *
     * ONE GATE FOR BOTH SHAPES. An empty declaration means the hook always
     * runs, so an element that says nothing keeps the behaviour it had. One
     * that declares the kinds it acts on is skipped for a parse that has
     * produced none of them: a pass is left out of the list below, and
     * skipping saves the whole pass, its walk over every root; a step is
     * skipped at the event, which for formula is every paragraph's EXIT of a
     * document with no formula in it.
     *
     * `kinds_created` OVER-APPROXIMATES: a node the parse creates and then
     * discards -- the text a formula consumes -- leaves its bit set although
     * the finished tree holds no such node. It can only make the gate skip
     * FEWER hooks, never miss one, because a kind in the finished tree was
     * necessarily created; and a hook that runs over a tree holding none of
     * its declared kinds finds nothing to do. Exactness is not available here:
     * it would need the parse to observe removal too, and `node_free` takes a
     * node and no parser precisely because a node outlives the parse.
     *
     * The roots the walk completes are recorded while any pass is declared
     * at all: which of them the gate selects is known only afterwards, and
     * the record is a pointer per root. */
    bool passes_declared = parser->dialect->passes_declared;
    finish_roots record = {NULL, 0, 0};
    if (parser->cut_count) {
        parser->nodes_created_before_finish = parser->nodes_created;
        parser->nodes_freed_before_finish = parser->nodes_freed;
        if (!S_finish_zone(parser)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        } else {
            S_refold(parser);
        }
    } else if (!S_apply_tree_phase(parser, parser->root, S_check_root, NULL, passes_declared ? &record : NULL)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }

    finish_phases phases = {NULL, 0};
    if (!parser->error && parser->dialect->element_count) {
        phases.passes = markdown_core_alloc(parser->dialect->element_count, sizeof(*phases.passes));
        if (!phases.passes) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    for (size_t i = 0; !parser->error && i < parser->dialect->element_count; i++) {
        const markdown_core_element_instance *instance = &parser->dialect->instances[i];
        if (instance->element->postprocess_func && S_finish_hook_selected(parser, instance->element)) {
            phases.passes[phases.pass_count++] = instance;
        }
    }

    /* The document's finalization reads the finished tree: the headings take
     * their anchors once every explicit anchor has been reserved, which the
     * walk did at each node's ENTER. */
    if (!parser->error) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->finish_document(document, parser);
    }

    /* Then the global passes, each on every finalized root: the field roots
     * and the document in the order the walk completed them. */
    if (!parser->error && phases.pass_count) {
        int ok = 1;
        for (size_t i = 0; ok && i < record.count; i++) {
            ok = S_run_passes(parser, record.roots[i], &phases);
        }
        if (!ok) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    markdown_core_free(record.roots);
    markdown_core_free((void *)phases.passes);

    /* Last, the checkpoints become the session's and the finished tree is
     * published: nothing changes it after this. */
    if (!parser->error) {
        S_commit_checkpoints(parser);
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
    /* The trees go: the one the parse built, which holds the reopened old
     * nodes, and the old nodes it held apart. */
    parser->dialect->document_structure->element->dispose_document(parser->dialect->document_structure, parser);
    if (parser->revision->checkpoints) {
        markdown_core_checkpoints_clear(parser->revision->checkpoints);
    }
    if (parser->root) {
        markdown_core_parser_release_node(parser, parser->root);
    }
    if (parser->old_root) {
        markdown_core_node_pool_release(parser->pool, parser->old_root);
    }
    for (size_t i = 0; i < parser->cut_reopened; i++) {
        if (parser->cuts[i].holder.first_child) {
            markdown_core_node_pool_release_chain(parser->pool, parser->cuts[i].holder.first_child);
        }
    }
    parser->root = parser->old_root = NULL;
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
