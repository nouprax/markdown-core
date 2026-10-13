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
#include "iterator.h"

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

/* The two parse stages stay out of line: the benchmark measures each one as
 * the cost of its call (scripts/benchmark/run.mjs). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_parse_source(markdown_core_parser *parser,
                                                               const markdown_core_input *input);
static MARKDOWN_CORE_ATTRIBUTE((noinline)) markdown_core_node *S_finish_parse(markdown_core_parser *parser);
static void S_complete_node(markdown_core_parser *parser, markdown_core_frame *frame, uint32_t start);
static void S_complete_inline_root(markdown_core_parser *parser, markdown_core_inline_root *root);
static void S_complete_closed(markdown_core_parser *parser, markdown_core_frame *frame);
static void S_complete_trailed(markdown_core_parser *parser, markdown_core_frame *owner);
static void S_take_another(markdown_core_parser *parser, markdown_core_frame *owner);
static inline bool S_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
static inline int S_append_input_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t line_start, int column, bufsize_t length, bufsize_t offset);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);
static inline bool S_input_seek(markdown_core_parser *parser, size_t offset);
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
 * the driver and speculative readers borrow it until its input ends; the
 * content of a cell read as blocks therefore holds the bytes the driver read.
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

static void S_clear_normalized_lines(markdown_core_parser *parser) {
    while (parser->input.normalized_lines) {
        markdown_core_normalized_line *line = parser->input.normalized_lines;
        parser->input.normalized_lines = line->next;
        markdown_core_free(line);
    }
}

/* AN INPUT'S STATE: the line machine's workspace, owned by the input being
 * read. The document is one input, and a cell read as blocks is another,
 * read while the document's is put aside (markdown_core_parser_read_blocks).
 * False, with the parse failed, when it could not be made. */
static bool S_input_begin(markdown_core_parser *parser) {
    markdown_core_strbuf_init(&parser->input.curline, 256);
    markdown_core_strbuf_init(&parser->input.lookahead_last_line, 0);
    /* The line index is the input's workspace, like curline: its first
     * capacity is made before any line, and lines reset its length, never
     * its ownership. */
    parser->input.lines = markdown_core_reserve(NULL, &parser->input.line_capacity, 1, sizeof(*parser->input.lines));
    if (!parser->input.lines || parser->input.curline.oom || parser->input.lookahead_last_line.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    return true;
}

static void S_input_dispose(markdown_core_parser *parser) {
    S_clear_normalized_lines(parser);
    markdown_core_free(parser->input.lines);
    markdown_core_free(parser->input.facts);
    markdown_core_free(parser->input.chunks);
    markdown_core_strbuf_free(&parser->input.curline);
    markdown_core_strbuf_free(&parser->input.lookahead_last_line);
    parser->input = (markdown_core_parser_input){0};
}

/* The instances release their parse state in the reverse of the order they
 * were attached, so an element still reads the state of the elements it was
 * attached after, its peers among them, as it releases its own. */
static void S_parser_dispose(markdown_core_parser *parser) {
    const markdown_core_dialect *dialect = parser->dialect;
    for (size_t i = dialect->element_count; i-- > 0;) {
        const markdown_core_element_instance *instance = &dialect->instances[i];
        if (instance->element->dispose_parser) {
            instance->element->dispose_parser(instance, parser);
        }
    }
    dialect->document_structure->element->dispose_document(dialect->document_structure, parser);
    markdown_core_source_order_dispose(&parser->source_order);
    markdown_core_free(parser->walk_stack);
    markdown_core_free(parser->complete_states);
    markdown_core_drafts_dispose(&parser->drafts);
    S_input_dispose(parser);
    markdown_core_free(parser->edit_shift);
    for (size_t i = 0; i < parser->replacement_count; i++) {
        markdown_core_node_pool_release(parser->pool, parser->replacements[i].node);
    }
    markdown_core_free(parser->replacements);
    markdown_core_free(parser->listed);
    markdown_core_free(parser->old_content.stems);
    if (parser->root) {
        markdown_core_frame_release(parser->pool, parser->root);
        parser->root = NULL;
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
    markdown_core_free(parser->stays);
    parser->stays = NULL;
    parser->stay_count = parser->stay_capacity = 0;
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

static bool S_reserve_content_marks(markdown_core_parser *parser, bufsize_t count);

/* Begins a transaction that continues `revision` and borrows its pool. */
static void S_parse_begin(markdown_core_parser *parser, markdown_core_revision *revision) {
    markdown_core_node *document;

    parser->revision = revision;
    parser->pool = revision->pool;
    parser->registry = &revision->pool->registry;
    parser->last_id = revision->last_id;
    const bool input_failed = !S_input_begin(parser);
    /* The first run of the content map is the identity an inline root's
     * parse places on (parser.h, MARKDOWN_CORE_IDENTITY_MARK): first, so that
     * no map appended after it ends with it. */
    if (S_reserve_content_marks(parser, 1)) {
        parser->line_marks[parser->line_marks_size++] = (markdown_core_line_mark){0, 0, 0, 1, 1, 0};
    }

    /* The length change before each edit (5.2), read by every image. */
    parser->edit_shift = markdown_core_alloc(revision->edit_count + 1, sizeof(*parser->edit_shift));
    const bool shift_failed = !parser->edit_shift;
    for (size_t i = 0; parser->edit_shift && i < revision->edit_count; i++) {
        const markdown_core_byte_edit *edit = &revision->edits[i];
        parser->edit_shift[i + 1] = parser->edit_shift[i] + (int64_t)edit->size - (int64_t)(edit->end - edit->start);
    }

    document = make_document(parser);
    if (document) {
        parser->root = markdown_core_parser_frame(parser, document, true);
        if (!parser->root) {
            markdown_core_parser_release_node(parser, document);
        } else {
            /* The document continues the previous one (5.9). */
            markdown_core_node *previous = revision->previous;
            parser->root->decided = true;
            parser->root->old = previous;
            parser->root->old_start = previous ? (uint32_t)previous->where.extent.lead : 0;
            /* The document reads its old node again from its start (5.3). */
            if (previous) {
                parser->root->scan = previous;
                parser->root->scan_start = parser->root->scan_at = parser->root->old_start;
                parser->root->scan_equal = true;
            }
        }
    }
    parser->input.block_root = parser->root;
    parser->input.current = parser->root;

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. Only a
     * complete transaction begins the document lifecycle -- its owner is
     * whichever element the dialect resolved it to, and it never sees a
     * failed transaction. Disposal does not depend on it having begun: the
     * lifecycle's own allocations can fail halfway, so its release already
     * takes whatever state it finds. */
    if (!parser->root || shift_failed || input_failed || parser->root->node->content.oom) {
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

/* THE EDIT MAPPING (docs/plans/2026-09-29-incremental-parsing.md, 5.2). */

size_t markdown_core_parser_edit_after(const markdown_core_parser *parser, uint32_t x) {
    const markdown_core_revision *revision = parser->revision;
    const markdown_core_byte_edit *edits = revision->edits;
    size_t lo = 0, hi = revision->edit_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (edits[mid].end <= x) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

uint32_t markdown_core_parser_image(const markdown_core_parser *parser, uint32_t x) {
    const markdown_core_revision *revision = parser->revision;
    const size_t at = markdown_core_parser_edit_after(parser, x);
    if (at < revision->edit_count && revision->edits[at].start <= x) {
        return (uint32_t)((int64_t)revision->edits[at].end + parser->edit_shift[at + 1]);
    }
    return (uint32_t)((int64_t)x + parser->edit_shift[at]);
}

uint32_t markdown_core_parser_origin(const markdown_core_parser *parser, uint32_t y) {
    const markdown_core_revision *revision = parser->revision;
    const markdown_core_byte_edit *edits = revision->edits;
    /* The edits whose replacements end at `y` or before it. */
    size_t lo = 0, hi = revision->edit_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((int64_t)edits[mid].start + parser->edit_shift[mid] + edits[mid].size <= y) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (uint32_t)((int64_t)y - parser->edit_shift[lo]);
}

bool markdown_core_parser_touched(const markdown_core_parser *parser, uint32_t from, uint32_t to) {
    return markdown_core_parser_edge(parser, from) <= to;
}

int64_t markdown_core_parser_edge(const markdown_core_parser *parser, uint32_t from) {
    const markdown_core_revision *revision = parser->revision;
    const size_t at = markdown_core_parser_edit_after(parser, from);
    return at < revision->edit_count ? revision->edits[at].start : INT64_MAX;
}

bool markdown_core_parser_source_anchor(const markdown_core_parser *parser, uint32_t start, uint32_t end,
                                        uint32_t *image) {
    const markdown_core_revision *revision = parser->revision;
    const markdown_core_byte_edit *edits = revision->edits;
    size_t lo = markdown_core_parser_edit_after(parser, start), x = start;
    while (lo < revision->edit_count && edits[lo].start <= x) {
        if (x < edits[lo].end) {
            x = edits[lo].end;
        }
        lo++;
    }
    if (x >= end) {
        return false;
    }
    *image = (uint32_t)((int64_t)x + parser->edit_shift[lo]);
    return true;
}

/* "This block ends on the line being processed". The three kinds that take
 * it in `markdown_core_block_finalize` — the document, a closed fenced code
 * block, a setext heading — are the ones whose last line IS the line in hand;
 * every other block ended on the line before. An element container closing on
 * its own fence is a fourth, and `markdown_core_block_finalize` cannot know
 * that from the type alone. */
static void S_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b) {
    b->where.place.end = (uint32_t)parser->input.line_end;
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
static bool S_may_be_lazy(markdown_core_parser *parser, const markdown_core_frame *matched) {
    if (parser->input.current == matched) {
        return false;
    }
    const markdown_core_element_instance *structure =
        markdown_core_parser_structure(parser, parser->input.current->node);
    return structure && structure->element->accepts_lazy &&
           structure->element->accepts_lazy(structure, parser, parser->input.current);
}

/* Record where the bytes about to be appended to `node`'s content came from.
 *
 * `column` is a BYTE column counted from 1, which is what every position in
 * the tree is counted in; `parser->input.column` is not one, because it counts a tab
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
    assert(mark.source_step == 1 ? mark.source_width == 1 : mark.source_step == 0 && mark.source_width >= 0);
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
    S_append_input_marks(parser, node, parser->input.line_number, parser->input.line_start, column, length,
                         node->content.size);
}

void markdown_core_block_add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
    /* A block that takes its lines as a literal keeps no map of its content:
     * no parse places nodes in it. */
    const bool mapped = !S_kind_accepts_lines(markdown_core_parser_kind(parser, node), node);
    /* Block content accumulates physical lines. Keep its existing initial
     * minimum reservation, but acquire enough for the complete first write
     * rather than allocating a small buffer and immediately growing it.
     * Empty blocks, including containers, never acquire this storage.
     * Producers of already delimited values use ordinary strbuf writes. */
    if (!markdown_core_strbuf_owns(&node->content) &&
        (parser->input.partially_consumed_tab || ch->len > parser->input.offset)) {
        size_t initial = (size_t)(ch->len - parser->input.offset);
        if (parser->input.partially_consumed_tab) {
            initial += TAB_STOP - (parser->input.column % TAB_STOP) - 1;
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
    if (parser->input.partially_consumed_tab) {
        /* The spaces below are the rest of the tab at parser->input.offset: a
         * decoded run of their own, which reads the tab, and the copied
         * bytes after it get a run of their own. */
        const int line = parser->input.line_number, column = parser->input.offset + 1;
        if (mapped) {
            const bufsize_t tab = markdown_core_parser_source_offset(parser, line, column);
            S_append_content_mark(
                parser, &node->content_map,
                (markdown_core_line_mark){node->content.size, line, tab,
                                          (int)(markdown_core_parser_source_end(parser, line, column) - tab), 0,
                                          parser->input.indent});
        }
        parser->input.offset += 1; // skip over tab
        // add space characters:
        chars_to_tab = TAB_STOP - (parser->input.column % TAB_STOP);
        for (i = 0; i < chars_to_tab; i++) {
            markdown_core_strbuf_putc(&node->content, ' ');
        }
    }
    if (mapped) {
        S_record_content_mark(parser, node, parser->input.offset + 1, ch->len - parser->input.offset);
    }
    markdown_core_strbuf_put(&node->content, ch->data + parser->input.offset, ch->len - parser->input.offset);
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
    assert(parser->input.block_root == parser->root);
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
    if (parser->input.block_root == parser->root) {
        const markdown_core_normalized_line *view = S_nul_view(parser, geometry);
        bool nul;
        return view ? S_denormalized_offset(view, geometry->start, column - 1, &nul)
                    : (bufsize_t)geometry->start + column - 1;
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_place(parser, &parser->input.block_root->node->content_map,
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
static bufsize_t S_mapped_line_end(markdown_core_parser *parser, int line) {
    const markdown_core_content_map *map = &parser->input.block_root->node->content_map;
    bufsize_t at = 0;
    int ignored;
    bufsize_t source = 0;
    for (int before = line - 1; before >= parser->input.first_line; before--) {
        const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, before);
        if (geometry->end > geometry->start) {
            at = (bufsize_t)geometry->end;
            break;
        }
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
    if (parser->input.block_root == parser->root) {
        if (!column) {
            return (bufsize_t)geometry->start;
        }
        return markdown_core_parser_mapped_source_offset(parser, line, column) + 1;
    }
    if (!column) {
        return S_mapped_line_end(parser, line);
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_end_place(parser, &parser->input.block_root->node->content_map,
                                                (bufsize_t)geometry->start + column - 1, &ignored, &source)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

/* Where byte `at` of the active input was written in the source. */
static inline bufsize_t S_input_source(markdown_core_parser *parser, bufsize_t at) {
    if (parser->input.block_root == parser->root) {
        return at;
    }
    int ignored;
    bufsize_t source = 0;
    if (!markdown_core_parser_content_place(parser, &parser->input.block_root->node->content_map, at, &ignored,
                                            &source)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return source;
}

static inline bufsize_t S_line_offset(markdown_core_parser *parser, int line) {
    return S_input_source(parser, (bufsize_t)markdown_core_parser_visited_line(parser, line)->start);
}

static inline bool S_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line) {
    if (line < parser->input.first_line || line > parser->input.line_number) {
        return false;
    }
    uint32_t start = node->where.place.start;
    if (line == parser->input.line_number) {
        return start >= (uint32_t)S_input_source(parser, parser->input.line_start);
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
    own_place->end = parser->input.block_root == parser->root
                         ? (uint32_t)markdown_core_input_line_next(parser, geometry)
                         : (uint32_t)markdown_core_parser_source_end(parser, line, length);
    return true;
}

/* The runs of `node`, which spans input lines `first` to `last`, more than
 * one: the own source of each line, a run of length 0, those that touch
 * joined. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_line_runs(markdown_core_parser *parser, markdown_core_node *node,
                                                            int first, int last) {
    markdown_core_runs *runs = markdown_core_runs_new(parser->pool, (uint32_t)(last - first + 1), false);
    if (!runs) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    markdown_core_place own;
    for (int line = first; line <= last; line++) {
        if (!S_line_own(parser, line, &own)) {
            markdown_core_node_pool_bytes_free(parser->pool, runs);
            return;
        }
        if (runs->count && runs->items[runs->count - 1].place.end == own.start) {
            runs->items[runs->count - 1].place.end = own.end;
        } else {
            runs->items[runs->count++].place = own;
        }
    }
    if (runs->count < 2) {
        markdown_core_node_pool_bytes_free(parser->pool, runs);
        return;
    }
    if (node->runs) {
        markdown_core_node_pool_bytes_free(parser->pool, node->runs);
    }
    node->runs = runs;
}

void markdown_core_parser_place_runs(markdown_core_parser *parser, markdown_core_node *node,
                                     const markdown_core_frame *container, int *line) {
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
        S_line_runs(parser, node, first, last);
    }
    *line = last;
}

/* One copied run of a document line. */
static inline int S_append_copied_mark(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t source, bufsize_t offset) {
    return S_append_content_mark(parser, &node->content_map,
                                 (markdown_core_line_mark){offset, line, source, 1, 1, parser->input.indent});
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
                                                             parser->input.indent})) {
            return 0;
        }
        bufsize_t next = inside ? at + 3 : at;
        k += inside;
        index = next < last ? next : last;
    }
    return 1;
}

/* Whether document line `geometry` ends in LF, the byte the block parser
 * reads its ending as. False, with the parse failed, when the input could
 * not be read. */
static inline bool S_ends_in_lf(markdown_core_parser *parser, const markdown_core_input_line *geometry) {
    return markdown_core_input_line_next(parser, geometry) - geometry->end == 1 &&
           S_input_seek(parser, geometry->end) &&
           parser->input.chunk[geometry->end - parser->input.chunk_start] == '\n';
}

/* The runs of `length` bytes from `column` of input line `line`, which
 * starts at `line_start` in the active input. The block parser reads a
 * document line's ending as LF: a copied byte when it is LF, and otherwise
 * a decoded run of its own over CR, CR LF, or nothing at the end of an input
 * without one. A line of a cell's content reads its owner's runs. */
static inline int S_append_input_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                       bufsize_t line_start, int column, bufsize_t length, bufsize_t offset) {
    if (parser->input.block_root != parser->root) {
        return markdown_core_parser_append_content_marks(parser, &parser->input.block_root->node->content_map,
                                                         &node->content_map, line_start + column - 1, length, offset);
    }
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    bufsize_t text = (bufsize_t)(geometry->end - geometry->start);
    if (geometry->facts) {
        text += 2 * (bufsize_t)parser->input.facts[geometry->facts - 1].nul_count;
    }
    const bool ending = column - 1 <= text && column - 1 + length > text;
    if (ending && !S_ends_in_lf(parser, geometry)) {
        if (parser->error) {
            return 0;
        }
        length--;
        if (length &&
            !(parser->input.mapped ? S_append_nul_line_marks(parser, node, line, line_start, column, length, offset)
                                   : S_append_copied_mark(parser, node, line, line_start + column - 1, offset))) {
            return 0;
        }
        const uint32_t next = (uint32_t)markdown_core_input_line_next(parser, geometry);
        return S_append_content_mark(parser, &node->content_map,
                                     (markdown_core_line_mark){offset + length, line, (bufsize_t)geometry->end,
                                                               (int)(next - geometry->end), 0, parser->input.indent});
    }
    return parser->input.mapped ? S_append_nul_line_marks(parser, node, line, line_start, column, length, offset)
                                : S_append_copied_mark(parser, node, line, line_start + column - 1, offset);
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

/* Appends to `runs` a run that decodes nothing over `start` to `end`, joined
 * to such a run it touches. */
static inline void S_runs_add_own(markdown_core_runs *runs, uint32_t start, uint32_t end) {
    if (start >= end) {
        return;
    }
    markdown_core_run_piece *const pieces = markdown_core_runs_pieces(runs);
    markdown_core_place *previous = runs->count ? &runs->items[runs->count - 1].place : NULL;
    if (previous && !pieces[runs->count - 1].decoded && previous->end == start) {
        previous->end = end;
    } else {
        pieces[runs->count].decoded = 0;
        runs->items[runs->count++].place = (markdown_core_place){start, end};
    }
}

/* AN INLINE ROOT'S CONTENT BECOMES THE INPUT ITS NODES ARE PLACED IN
 * (markdown_core_inline_read_root). The `length` bytes its parse reads
 * keep their map to the source in the root's runs: one run per run of the
 * map, the bytes it decodes from where it reads them, runs that read as many
 * bytes as they decode and touch in the source joined into one, so the runs'
 * source ranges are in order and apart or touching (markdown_core_run). The root's
 * own source that gives no content lies in runs that decode nothing between
 * them: the rest of each line its runs that decode nothing cover
 * (markdown_core_parser_place_runs), or, when it has none, all of the source
 * between its content runs. The root's map becomes the identity, so the
 * parse places its nodes at offsets of the content. A root whose map is the
 * identity holds its runs already: one a parse reads again (5.7). */
void markdown_core_parser_read_content(markdown_core_parser *parser, markdown_core_node *node, bufsize_t length) {
    const markdown_core_content_map map = node->content_map;
    node->content_map = (markdown_core_content_map){MARKDOWN_CORE_IDENTITY_MARK, 1, 0};
    if (length <= 0 || map.count <= 0 || map.first == MARKDOWN_CORE_IDENTITY_MARK) {
        return;
    }
    const bufsize_t end = map.offset + length;
    const markdown_core_line_mark *mark =
        &parser->line_marks[markdown_core_block_content_mark_at(parser, &map, map.offset)];
    const markdown_core_line_mark *const past = &parser->line_marks[map.first + map.count];
    /* The node's own lines, when it has runs of them; one line without. */
    markdown_core_runs *const lines = node->runs;
    const uint32_t line_count = lines ? lines->count : 0;
    /* At most one content run per mark from the first to the end of the map,
     * one run that decodes nothing before each and one after each line. */
    markdown_core_runs *runs =
        markdown_core_runs_new(parser->pool, (uint32_t)((past - mark) * 2 + line_count + 1), true);
    if (!runs) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    runs->decoded = (uint32_t)length;
    markdown_core_run_piece *const pieces = markdown_core_runs_pieces(runs);
    uint32_t line = 0, at = 0;
    for (bufsize_t from = map.offset; from < end; mark++) {
        const bufsize_t to = mark + 1 < past && mark[1].content_offset < end ? mark[1].content_offset : end;
        const uint32_t start = (uint32_t)(mark->source + (from - mark->content_offset) * mark->source_step);
        const uint32_t stop = start + (uint32_t)((to - from - 1) * mark->source_step + mark->source_width);
        const uint32_t size = (uint32_t)(to - from);
        from = to;
        const uint32_t last = runs->count - 1;
        markdown_core_place *previous = runs->count && pieces[last].decoded ? &runs->items[last].place : NULL;
        assert(!previous || start >= previous->end);
        /* A slice that reads as many bytes as it decodes continues such a run
         * that ends where it starts. */
        if (stop - start == size && previous && previous->end - previous->start == pieces[last].decoded &&
            previous->end == start) {
            previous->end = stop;
            pieces[last].decoded += size;
            at = stop;
            continue;
        }
        /* The own source before the slice: the rest of each line it passes,
         * and its own line up to it; with no lines, all of the source since
         * the run before. */
        for (; line < line_count && lines->items[line].place.end <= start; line++) {
            const markdown_core_place own = lines->items[line].place;
            S_runs_add_own(runs, at > own.start ? at : own.start, own.end);
        }
        if (line < line_count && lines->items[line].place.start <= start) {
            const uint32_t first = lines->items[line].place.start;
            S_runs_add_own(runs, at > first ? at : first, start);
        } else if (!lines && runs->count) {
            S_runs_add_own(runs, at, start);
        }
        pieces[runs->count].decoded = size;
        runs->items[runs->count++].place = (markdown_core_place){start, stop};
        at = stop;
    }
    for (; line < line_count; line++) {
        const markdown_core_place own = lines->items[line].place;
        S_runs_add_own(runs, at > own.start ? at : own.start, own.end);
    }
    if (lines) {
        markdown_core_node_pool_bytes_free(parser->pool, lines);
    }
    node->runs = runs;
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

static void S_read_again_where_it_begins(markdown_core_parser *parser, markdown_core_frame *frame);

/* Raises the reach of `node`, which is not numbered yet, to `reach`. */
static inline void S_raise_reach(markdown_core_node *node, uint32_t reach) {
    node->reach = node->reach < reach ? reach : node->reach;
}

uint64_t markdown_core_parser_carry(const markdown_core_parser *parser, const markdown_core_frame *parent,
                                    const markdown_core_node *previous) {
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, parent->node);
    const uint64_t word =
        structure && structure->element->carry_save ? structure->element->carry_save(structure, parent) : 0;
    const uint64_t before = previous ? previous->kind : 0;
    const uint64_t blank =
        parent->node->flags & (MARKDOWN_CORE_NODE__LAST_LINE_BLANK | MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK);
    return word | before << 32 | blank << 48;
}

markdown_core_frame *markdown_core_parser_attach_split(markdown_core_parser *parser, markdown_core_frame *whole,
                                                       markdown_core_node *piece) {
    markdown_core_frame *frame = markdown_core_parser_attach(parser, whole->owner, piece, whole);
    if (!frame) {
        return NULL;
    }
    piece->entry = whole->node->entry;
    piece->reach = whole->node->reach;
    piece->flags |= MARKDOWN_CORE_NODE__HOLDS_NEXT;
    whole->node->entry = markdown_core_parser_carry(parser, whole->owner, piece);
    /* Its lines (E5) began where the piece begins now: it has none. */
    if (whole->node->lines) {
        markdown_core_node_pool_bytes_free(parser->pool, whole->node->lines);
        whole->node->lines = NULL;
    }
    S_read_again_where_it_begins(parser, whole);
    return frame;
}

/* THE PARSE RECORD OF `frame`, a block the line being processed closes or
 * writes into (5.1, 5.3, E2): its reach and its parent's cover what the line
 * has read, and it takes `hold`, MARKDOWN_CORE_NODE__HOLDS_NEXT when the line
 * was read in its context and MARKDOWN_CORE_NODE__TRAILED when the line lies
 * after its end and wrote into it. A taken block keeps the record the old
 * parse wrote. */
static void S_record(markdown_core_parser *parser, markdown_core_frame *frame, markdown_core_node_internal_flags hold) {
    markdown_core_node *node = frame->node;
    if (parser->input.block_root != parser->root || frame->numbered) {
        return;
    }
    S_raise_reach(node, parser->input.line_reach);
    node->flags |= hold;
    if (frame->owner) {
        S_raise_reach(frame->owner->node, node->reach);
    }
}

void markdown_core_parser_write_closed(markdown_core_parser *parser, markdown_core_frame *parent, bufsize_t end) {
    parent->last->node->where.place.end = (uint32_t)end;
    S_record(parser, parent->last, MARKDOWN_CORE_NODE__TRAILED);
}

/* THE BLANK-LINE SUMMARY (E4) of the children of a kind a child's blank
 * line propagates out of, a list and its items. A child the facts read
 * through is no child to them, the word 0. Any other child is SEEN, and says
 * whether it ENDS with a blank line, whether its own LAST line was blank,
 * and, through its children, whether a blank line lies BETWEEN two of them.
 * A run of children ends as its last seen child does; a blank line lies
 * between two of them when a seen child before another ends with one
 * (BETWEEN), or ends with one or had a blank last line (AFTER); and a child
 * whose children have a blank line between them makes the run INSIDE. */
enum {
    S_BLANK_SEEN = 1u << 0,
    S_BLANK_ENDS = 1u << 1,
    S_BLANK_LAST = 1u << 2,
    S_BLANK_BETWEEN = 1u << 3,
    S_BLANK_AFTER = 1u << 4,
    S_BLANK_INSIDE = 1u << 5
};

static uint64_t S_blank_summary(const markdown_core_node *node) { return node->children ? node->children->summary : 0; }

static bool S_ends_with_blank_line(const markdown_core_node *node, uint64_t children) {
    return children & S_BLANK_SEEN ? (children & S_BLANK_ENDS) != 0 : markdown_core_block_last_line_blank(node);
}

static uint64_t S_blank_of(const markdown_core_node *node);
static uint64_t S_blank_combine(uint64_t front, uint64_t back);

/* A block that has not completed holds its children in its frame: the
 * entries it numbered, then the nodes it holds and the candidates it took,
 * each a stem with the summary of its run. They fold as its stem will, for a
 * kind whose stem keeps the blank summary. */
bool markdown_core_block_ends_with_blank_line(markdown_core_parser *parser, const markdown_core_frame *frame) {
    const markdown_core_node *node = frame->node;
    if (frame->completed || markdown_core_parser_kind(parser, node)->summary != &MARKDOWN_CORE_BLANK_SUMMARY) {
        return S_ends_with_blank_line(node, S_blank_summary(node));
    }
    uint64_t children = 0;
    for (size_t i = 0; i < frame->built; i++) {
        const markdown_core_built *built = &frame->entries[i];
        children = S_blank_combine(children, built->candidate ? built->candidate->summary : S_blank_of(built->node));
    }
    for (const markdown_core_frame *child = frame->first; child; child = child->next) {
        if (child->candidate) {
            children = S_blank_combine(children, child->candidate->summary);
        } else if (child->held) {
            children = S_blank_combine(children, S_blank_of(child->node));
        }
    }
    return S_ends_with_blank_line(node, children);
}

bool markdown_core_block_loose(const markdown_core_node *node) {
    return (S_blank_summary(node) & (S_BLANK_AFTER | S_BLANK_INSIDE)) != 0;
}

static uint64_t S_blank_of(const markdown_core_node *node) {
    if (node->flags & MARKDOWN_CORE_NODE__BLANK_TRANSPARENT) {
        return 0;
    }
    return S_BLANK_SEEN | (S_ends_with_blank_line(node, S_blank_summary(node)) ? S_BLANK_ENDS : 0) |
           (markdown_core_block_last_line_blank(node) ? S_BLANK_LAST : 0) |
           (S_blank_summary(node) & S_BLANK_BETWEEN ? S_BLANK_INSIDE : 0);
}

static uint64_t S_blank_combine(uint64_t front, uint64_t back) {
    if (!(front & S_BLANK_SEEN) || !(back & S_BLANK_SEEN)) {
        return front | back;
    }
    const uint64_t between = front & S_BLANK_ENDS   ? S_BLANK_BETWEEN | S_BLANK_AFTER
                             : front & S_BLANK_LAST ? S_BLANK_AFTER
                                                    : 0;
    return S_BLANK_SEEN | (back & (S_BLANK_ENDS | S_BLANK_LAST)) |
           ((front | back) & (S_BLANK_BETWEEN | S_BLANK_AFTER | S_BLANK_INSIDE)) | between;
}

const markdown_core_stem_summary MARKDOWN_CORE_BLANK_SUMMARY = {S_blank_of, S_blank_combine};

static void S_settle(markdown_core_parser *parser, markdown_core_frame *frame);

/* Adds the `count` lines at `added` to the lines of `node`, a leaf (E5).
 * False, with the parse failed, when they could not grow. */
static bool S_lines_append(markdown_core_parser *parser, markdown_core_node *node, int32_t lead,
                           const markdown_core_line *added, uint32_t count) {
    markdown_core_lines *lines = node->lines;
    const uint32_t needed = (lines ? lines->count : 0) + count;
    if (!lines || needed > lines->capacity) {
        uint32_t capacity = lines ? lines->capacity * 2 : 4;
        while (capacity < needed) {
            capacity *= 2;
        }
        markdown_core_lines *grown = markdown_core_node_pool_bytes(
            parser->pool, offsetof(markdown_core_lines, items) + capacity * sizeof(markdown_core_line));
        if (!grown) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        *grown = (markdown_core_lines){.capacity = capacity, .lead = lead};
        if (lines) {
            grown->count = lines->count;
            grown->lead = lines->lead;
            grown->broken = lines->broken;
            memcpy(grown->items, lines->items, lines->count * sizeof(markdown_core_line));
            markdown_core_node_pool_bytes_free(parser->pool, lines);
        }
        node->lines = lines = grown;
    }
    memcpy(lines->items + lines->count, added, count * sizeof(markdown_core_line));
    lines->count = needed;
    return true;
}

/* THE LINE THE PARSE HAS JUST READ, AS A LINE OF THE LEAF IT LEFT OPEN AND
 * CURRENT (E5): `before` was the current block as the line began. A leaf
 * the line opened begins its lines with it; a leaf that was current before
 * adds it, as a plain line when it was one. A leaf that is current again
 * after a line it did not take cannot follow its lines, and is broken. */
static void S_record_line(markdown_core_parser *parser, markdown_core_frame *before) {
    markdown_core_frame *leaf = parser->input.current;
    if (parser->input.lines_taken || parser->input.taken || leaf == parser->root ||
        !(leaf->node->flags & MARKDOWN_CORE_NODE__OPEN) ||
        !S_kind_takes_text(markdown_core_parser_kind(parser, leaf->node), leaf->node)) {
        return;
    }
    const int last = parser->input.claimed ? parser->input.claimed_line : parser->input.line_number;
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, last);
    const uint32_t next = (uint32_t)markdown_core_input_line_next(parser, geometry);
    markdown_core_line line = leaf == before && !parser->input.claimed ? parser->input.plain : (markdown_core_line){0};
    line.span = next - (uint32_t)parser->input.line_start;
    line.text = geometry->end - (uint32_t)parser->input.line_start;
    line.reach = parser->input.line_reach > next ? parser->input.line_reach - next : 0;
    markdown_core_lines *lines = leaf->node->lines;
    if (leaf != before) {
        if (lines) {
            lines->broken = true;
            return;
        }
        S_lines_append(parser, leaf->node, (int32_t)((int64_t)parser->input.line_start - leaf->node->where.place.start),
                       &line, 1);
    } else if (lines) {
        S_lines_append(parser, leaf->node, lines->lead, &line, 1);
    }
}

static bool S_holds(const markdown_core_frame *frame, const markdown_core_frame *held);

markdown_core_frame *markdown_core_block_finalize(markdown_core_parser *parser, markdown_core_frame *frame) {
    markdown_core_frame *parent = frame->owner;
    markdown_core_node *b = frame->node;
    markdown_core_node_internal_flags held = 0;
    assert(b->flags & MARKDOWN_CORE_NODE__OPEN); // shouldn't call markdown_core_block_finalize on closed blocks
    /* A block closes after the blocks it holds: those still open on the
     * current path close first, the deepest first. */
    while (parser->input.current != frame && S_holds(frame, parser->input.current)) {
        parser->input.current = markdown_core_block_finalize(parser, parser->input.current);
    }
    S_complete_trailed(parser, frame);
    b->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    /* Whether the line that closes it matched it: its decisions could read
     * the block, open. */
    const bool matched = S_holds(frame, parser->input.matched_container);
    /* The current block and the block the line matched are open: closing
     * either makes its owner take its place, before the block settles and
     * may be released. */
    if (parser->input.current == frame) {
        parser->input.current = parent;
    }
    if (parser->input.matched_container == frame) {
        parser->input.matched_container = parent;
    }

    if (parser->input.curline.size == 0) {
        // end of input - line number has not been incremented
        b->where.place.end = (uint32_t)parser->input.last_line_end;
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
               S_starts_on_line(parser, b, parser->input.line_number) ||
               /* M0: the same block closed by its end condition on a LATER
                * line. The terminator line is the block's last line and is
                * the line being processed, so the block ends here too:
                * `<!--\nmulti\n-->` gave `HTMLBlock scope=1:1..2:5`, one line
                * short of the `-->` its literal holds, and `<pre>\nx\n</pre>`
                * the same shape. Only the last two ways a block can close --
                * the input ending and a container closing under it -- still
                * end it on the line before. */
               (b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0) {
        S_set_end_to_current_line(parser, b);
    } else {
        b->where.place.end = (uint32_t)parser->input.last_line_end;
        held = matched || parser->input.line_context ? MARKDOWN_CORE_NODE__HOLDS_NEXT
               : parser->input.previous_blank        ? MARKDOWN_CORE_NODE__TRAILED
                                                     : 0;
    }
    /* A block a later line closed holds the next one when that line read it:
     * matched it, or was read with an open paragraph as its context; it
     * trails when the blank lines before that line were read with it open. */
    S_record(parser, frame, held);

    /* Every block it holds has closed: it settles. */
    assert(!frame->last || !(frame->last->node->flags & MARKDOWN_CORE_NODE__OPEN) || parser->error);
    if (!parser->error) {
        S_settle(parser, frame);
    }
    return parent;
}

static void S_settle(markdown_core_parser *parser, markdown_core_frame *frame) {
    markdown_core_frame *parent = frame->owner;
    markdown_core_node *b = frame->node;
    /* A block that takes text lines in a container has a run of its own
     * source on each of its lines, whatever its close hook makes of it; one
     * of the document itself lies on whole lines, its own source its range. */
    const bool text = S_kind_takes_text(markdown_core_parser_kind(parser, b), b);
    const bool lines = parent != parser->root && text;
    /* The lines of a leaf (E5) are those of a block that takes text; one that
     * became another kind, a table, has none. */
    if (!text && b->lines) {
        markdown_core_node_pool_bytes_free(parser->pool, b->lines);
        b->lines = NULL;
    }
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, b);
    if (structure && structure->element->finalize_block) {
        structure->element->finalize_block(structure, parser, frame);
    }
    /* A paragraph that held only reference definitions left them References
     * before it and nothing of its own: it has no place in the tree. The
     * References are the definitions' siblings from here, in source order,
     * and the last of them ends where the paragraph ended: it holds the next
     * block as the paragraph did (5.3). */
    if (b->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY) {
        const markdown_core_node_internal_flags holds = MARKDOWN_CORE_NODE__HOLDS_NEXT | MARKDOWN_CORE_NODE__TRAILED;
        /* The last of them, unless the parse failed making them. */
        markdown_core_frame *last = frame->prev;
        assert(last || parser->error);
        if (last) {
            last->node->flags = (markdown_core_node_internal_flags)((last->node->flags & ~holds) | (b->flags & holds));
        }
        markdown_core_parser_release_frame(parser, frame);
        return;
    }
    if (lines) {
        int line = parser->input.line_number;
        markdown_core_parser_place_runs(parser, b, parent, &line);
    }
    /* The last block of an open owner that a later line may still add to
     * completes when that line comes, its owner takes another child or its
     * owner closes; its range is open until then (5.9). */
    if (frame->awaits_trailer && parent && parent->last == frame && (parent->node->flags & MARKDOWN_CORE_NODE__OPEN)) {
        return;
    }
    frame->awaits_trailer = false;
    S_complete_closed(parser, frame);
}

/* The block is complete (docs/plans/2026-09-29-incremental-parsing.md, 5.8):
 * it decides as it completes (5.9), its children become its stem, and it
 * numbers what it holds. */
static void S_complete_closed(markdown_core_parser *parser, markdown_core_frame *frame) {
    if (!parser->error) {
        S_complete_node(parser, frame, frame->node->where.place.start);
    }
}

/* `owner`'s last block, when it awaits a trailer, has none: it completes. */
static void S_complete_trailed(markdown_core_parser *parser, markdown_core_frame *owner) {
    markdown_core_frame *last = owner->last;
    if (last && last->awaits_trailer) {
        last->awaits_trailer = false;
        S_complete_closed(parser, last);
    }
}

/* `owner` takes another child after its last: that one has no trailer, and
 * the children before the new one are numbered as far as they completed. */
static void S_take_another(markdown_core_parser *parser, markdown_core_frame *owner) {
    S_complete_trailed(parser, owner);
    if (!parser->error) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->number_children(document, parser, owner);
    }
}

void markdown_core_block_close(markdown_core_parser *parser, markdown_core_frame *b, bool holds) {
    S_record(parser, b, holds ? MARKDOWN_CORE_NODE__HOLDS_NEXT : 0);
    S_settle(parser, b);
}

/* Finalize to the container that will own the next block-level construct,
 * including a detached identifier, which contributes no child node. */
/* A source-owning block candidate commits only after the prior open path has
 * closed at this line's matched boundary. This is also the ordinary text path's
 * transition; caption attachment therefore cannot strand an open preceding table. */
/* Whether `frame` is `held` or holds it. */
static bool S_holds(const markdown_core_frame *frame, const markdown_core_frame *held) {
    while (held && held != frame) {
        held = held->owner;
    }
    return held != NULL;
}

/* Closes the open blocks on the current path below `target`, which then
 * holds the current block. */
static void S_close_to(markdown_core_parser *parser, const markdown_core_frame *target) {
    while (!S_holds(parser->input.current, target) && !parser->error) {
        parser->input.current = markdown_core_block_finalize(parser, parser->input.current);
    }
}

void markdown_core_parser_finalize_unmatched_blocks(markdown_core_parser *parser) {
    S_close_to(parser, parser->input.matched_container);
}

markdown_core_frame *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_frame *parent,
                                                    markdown_core_node_type block_type) {
    assert(parent);

    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!markdown_core_node_can_contain_type(parent->node, block_type)) {
        if (!parent->owner) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED);
            return NULL;
        }
        parent = markdown_core_block_finalize(parser, parent);
    }
    return parent;
}

// Add a node as child of another.  Return pointer to child.
markdown_core_frame *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_frame *parent,
                                                    markdown_core_node_type block_type, int start_column) {
    parent = markdown_core_block_parent_for(parser, parent, block_type);
    return parent ? markdown_core_parser_add_child_validated(parser, parent, block_type, start_column) : NULL;
}

bool markdown_core_parser_replace(markdown_core_parser *parser, const markdown_core_node *old,
                                  markdown_core_node *node) {
    struct markdown_core_replacement *replacements = markdown_core_reserve(
        parser->replacements, &parser->replacement_capacity, parser->replacement_count + 1, sizeof(*replacements));
    if (!replacements) {
        markdown_core_parser_release_node(parser, node);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    parser->replacements = replacements;
    replacements[parser->replacement_count++] = (struct markdown_core_replacement){old, node};
    return true;
}

/* THE CURSOR (docs/plans/2026-09-29-incremental-parsing.md, 5.3). Where
 * the line machine is about to start a block at `start` under `parent`,
 * which reads its old node again, the cursor passes the old children whose
 * images end at `start` or before it and holds the next, which begins at
 * `*child_start` in its old coordinates and whose image begins at `start`,
 * after it, or before it when it spans `start`. NULL when none is left. A
 * group (MARKDOWN_CORE_NODE__GROUP), as a definition's body, has no extent:
 * its owner measures the nodes it holds along its relation, so the cursor
 * measures nothing from a group on and holds none. */
static const markdown_core_node *S_cursor_at(markdown_core_parser *parser, markdown_core_frame *parent, uint32_t start,
                                             uint32_t *child_start) {
    const markdown_core_stem *children = parent->scan->children;
    const size_t count = markdown_core_stem_count(children);
    while (parent->scan_next < count) {
        const markdown_core_node *child = markdown_core_stem_at(children, parent->scan_next);
        if (child->flags & MARKDOWN_CORE_NODE__GROUP) {
            return NULL;
        }
        const uint32_t at = (uint32_t)((int64_t)parent->scan_at + child->where.extent.lead);
        const uint32_t end = at + child->where.extent.span;
        if (markdown_core_parser_image(parser, end) > start || markdown_core_parser_image(parser, at) >= start) {
            *child_start = at;
            return child;
        }
        parent->scan_at = end;
        parent->scan_next++;
    }
    return NULL;
}

/* The old child the cursor offers where a block begins at `start`: the one
 * whose image begins there, or NULL. */
static const markdown_core_node *S_old_child(markdown_core_parser *parser, markdown_core_frame *parent, uint32_t start,
                                             uint32_t *child_start) {
    const markdown_core_node *child = S_cursor_at(parser, parent, start, child_start);
    return child && markdown_core_parser_image(parser, *child_start) == start ? child : NULL;
}

/* Whether the input holds nothing but blank lines from `from` to its end:
 * lines that write into the block before them and close nothing. */
static bool S_blank_to_end(markdown_core_parser *parser, size_t from) {
    for (size_t at = from; at < parser->input.length; at++) {
        if (!S_input_seek(parser, at)) {
            return false;
        }
        const unsigned char c = parser->input.chunk[at - parser->input.chunk_start];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}

/* The cursor takes `first`, the old child that begins where the line
 * machine is about to start a block, whole when no edit meets it from its
 * lead to its reach and its entry is `carry`, the state `parent` carries now,
 * with the candidate of unchanged siblings after it, up to the last one after
 * which the next line is read as the old parse read it; the line ends there,
 * and the parse goes on after the candidate. Equal state and equal bytes
 * start the block the old parse started, whatever kind its lines made of it:
 * a paragraph's a Reference, a table or a setext heading, an HTML block's a
 * comment. True when it took a candidate. */
static bool S_take(markdown_core_parser *parser, markdown_core_frame *parent, uint32_t start, uint64_t carry,
                   const markdown_core_node *first, uint32_t first_start) {
    const markdown_core_stem *children = parent->scan->children;
    if ((first->flags & MARKDOWN_CORE_NODE__GROUP) || !parent->scan_equal || first->entry != carry) {
        return false;
    }
    /* The old node before `first`, which the cursor passed, was read again.
     * When it holds the next, the line that closed it, on which `first`
     * begins, was read in its context, as an item marker is read against the
     * list it may join: that line is read again with it. */
    if (parent->scan_next > 0 &&
        (markdown_core_stem_at(children, parent->scan_next - 1)->flags & MARKDOWN_CORE_NODE__HOLDS_NEXT)) {
        return false;
    }
    /* The candidate stops at the first node an edit meets from where it is
     * measured to its reach, which is the first whose reach meets the first
     * edit that ends after the candidate begins, or at a group; it ends at the
     * last node before that which does not hold the next. A node that later
     * lines wrote into (MARKDOWN_CORE_NODE__TRAILED) ends it only before an
     * untouched node, or, among the document's children, at the end of the
     * input when nothing but blank lines runs to it: those lines lie before
     * that node, or the end, read as the old parse read them. */
    int64_t measured = parent->scan_at;
    const size_t stop = markdown_core_stem_meet(children, parent->scan_next, &measured,
                                                markdown_core_parser_edge(parser, parent->scan_at));
    const int64_t shift = (int64_t)start - first_start;
    size_t final = markdown_core_stem_last_free(children, parent->scan_next, stop);
    bool to_end = false;
    if (final != SIZE_MAX && final + 1 == stop &&
        (markdown_core_stem_at(children, final)->flags & MARKDOWN_CORE_NODE__TRAILED)) {
        const int64_t trailed_end =
            parent->scan_at + markdown_core_stem_length(children, parent->scan_next, stop - parent->scan_next);
        to_end = parent == parser->root && stop == markdown_core_stem_count(children) &&
                 trailed_end + shift + markdown_core_stem_at(children, final)->reach >= (int64_t)parser->input.length &&
                 S_blank_to_end(parser, (size_t)(trailed_end + shift));
        if (!to_end) {
            final = markdown_core_stem_last_free(children, parent->scan_next, final);
        }
    }
    if (final == SIZE_MAX) {
        return false;
    }
    const size_t end = final + 1;
    const uint32_t candidate_end =
        (uint32_t)(parent->scan_at + markdown_core_stem_length(children, parent->scan_next, end - parent->scan_next));
    /* No edit lies in the candidate, so it moved as a whole; the parse goes on at
     * the line after the one its last node ends on, or, before an untouched
     * node or the end of the input, at the line that node begins on, in the
     * state its entry records, or at the end, in the state the old parse
     * left: the lines between were read as the old parse read them. */
    const size_t last_end = (size_t)(candidate_end + shift);
    const markdown_core_node *next = end < stop ? markdown_core_stem_at(children, end) : NULL;
    const size_t next_start = next     ? last_end + (size_t)next->where.extent.lead
                              : to_end ? parser->input.length
                                       : last_end;
    size_t resume = last_end, resume_last_end = last_end;
    bool skipped = false;
    if (resume < parser->input.length) {
        if (!S_input_seek(parser, resume)) {
            return false;
        }
        unsigned char terminator = parser->input.chunk[resume - parser->input.chunk_start];
        if (terminator != '\r' && terminator != '\n') {
            return false;
        }
        for (size_t at = resume;;) {
            at++;
            if (terminator == '\r' && at < parser->input.length) {
                if (!S_input_seek(parser, at)) {
                    return false;
                }
                at += parser->input.chunk[at - parser->input.chunk_start] == '\n';
            }
            resume = at;
            /* The next terminator before the next node's start ends a line
             * between them. */
            for (; at < next_start; at++) {
                if (!S_input_seek(parser, at)) {
                    return false;
                }
                terminator = parser->input.chunk[at - parser->input.chunk_start];
                if (terminator == '\r' || terminator == '\n') {
                    break;
                }
            }
            if (at >= next_start) {
                break;
            }
            resume_last_end = at;
            skipped = true;
        }
    }
    /* The open blocks below `parent` are those this line closes: they close
     * before the candidate, which then lies after `parent`'s last child as it did
     * after the old one. */
    S_close_to(parser, parent);
    if (parser->error) {
        return false;
    }
    uint32_t anchor;
    if (!markdown_core_frame_children_end(parent, &anchor)) {
        anchor = parent->node->where.place.start;
    }
    if ((int64_t)start - anchor != first->where.extent.lead) {
        return false;
    }
    /* The candidate is one frame, holding it as a stem, which stands for
     * its last node. */
    bool failed;
    const size_t taken = end - parent->scan_next;
    markdown_core_stem *candidate =
        markdown_core_stem_slice(parser->pool, children, parent->scan_next, taken,
                                 markdown_core_parser_kind(parser, parent->node)->summary, &failed);
    markdown_core_node *final_node = markdown_core_stem_at(children, final);
    markdown_core_frame *frame = failed ? NULL : markdown_core_parser_frame(parser, final_node, false);
    if (!frame) {
        markdown_core_stem_release(parser->pool, candidate);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    frame->candidate = candidate;
    frame->past = end;
    S_take_another(parser, parent);
    markdown_core_frame_attach(parent, frame, NULL);
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, parent->node);
    if (structure && structure->element->fold_children) {
        structure->element->fold_children(structure, parser, parent, candidate->summary);
    }
    /* A taken node is complete: it continues itself, with its id. */
    const uint32_t at = candidate_end;
    frame->decided = frame->identified = frame->numbered = true;
    frame->old = final_node;
    frame->old_start = at - final_node->where.extent.span;
    frame->place = (markdown_core_place){(uint32_t)(first_start + shift), (uint32_t)(at + shift)};
    parent->scan_next = end;
    parent->scan_at = at;
    parser->input.taken = true;
    parser->input.resume = resume;
    parser->input.resume_last_end = (bufsize_t)resume_last_end;
    parser->input.resume_blank = skipped;
    parser->input.resume_flags =
        next     ? (markdown_core_node_internal_flags)(next->entry >> 48)
        : to_end ? (markdown_core_node_internal_flags)(parent->scan->flags & (MARKDOWN_CORE_NODE__LAST_LINE_BLANK |
                                                                              MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK))
                 : 0;
    parser->input.current = parent;
    return true;
}

/* A block whose old node of its kind begins where it does reads that node
 * again: a container its children, and a leaf its lines (E5). */
static void S_read_again(markdown_core_parser *parser, markdown_core_frame *frame, const markdown_core_node *old,
                         uint32_t old_start) {
    const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, frame->node);
    if (old->kind == frame->node->kind && (old->lines || (structure && structure->element->carry_save))) {
        frame->scan = old;
        frame->scan_start = frame->scan_at = old_start;
        /* What the block's own lines read is its kind and the state its
         * parent carries (E3); its siblings decided only where it begins,
         * which its opening line, read again, decides anew. */
        frame->scan_equal = frame->owner->scan_equal && (uint32_t)old->entry == (uint32_t)frame->node->entry;
    }
}

/* A block whose kind or start its lines changed reads again the old node of
 * its kind that begins where it begins now: the old node it read, of the
 * kind it was or from where it began, is another block's. */
static void S_read_again_where_it_begins(markdown_core_parser *parser, markdown_core_frame *frame) {
    const uint32_t start = frame->node->where.place.start;
    if (frame->scan &&
        (frame->scan->kind != frame->node->kind || markdown_core_parser_image(parser, frame->scan_start) != start)) {
        frame->scan = NULL;
        frame->scan_start = frame->scan_at = 0;
        frame->scan_next = 0;
        frame->scan_equal = false;
    }
    markdown_core_frame *owner = frame->owner;
    if (!frame->scan && owner && owner->scan && parser->input.block_root == parser->root) {
        uint32_t old_start;
        const markdown_core_node *old = S_old_child(parser, owner, start, &old_start);
        if (old) {
            S_read_again(parser, frame, old, old_start);
        }
    }
}

/* THE CURSOR DESCENDS (5.3). A container that read no old node where it
 * began, as one whose opening line an edit moved, reads again the old node
 * of its kind its parent's cursor holds across `start`, where it is about to
 * start a block, as tree-sitter breaks a changed node down to the nodes that
 * begin where the parse is. The parent's cursor passes the node it descends
 * into. */
static void S_descend(markdown_core_parser *parser, markdown_core_frame *frame, uint32_t start) {
    markdown_core_frame *owner = frame->owner;
    if (!owner || !owner->scan) {
        return;
    }
    uint32_t at;
    const markdown_core_node *child = S_cursor_at(parser, owner, start, &at);
    if (child && child->kind == frame->node->kind && markdown_core_parser_image(parser, at) <= start) {
        owner->scan_at = at + child->where.extent.span;
        owner->scan_next++;
        S_read_again(parser, frame, child, at);
    }
}

/* A selected parent is a semantic decision, not a hint to repeat the search. */
markdown_core_frame *markdown_core_parser_add_child_validated(markdown_core_parser *parser, markdown_core_frame *parent,
                                                              markdown_core_node_type block_type, int start_column) {
    assert(parent);
    const bufsize_t start = markdown_core_parser_source_offset(parser, parser->input.line_number, start_column);
    /* The block's entry, read where the line machine starts it (5.3). */
    const bool records = parser->input.block_root == parser->root;
    const uint64_t carry =
        records ? markdown_core_parser_carry(parser, parent, markdown_core_parser_last_child(parser, parent)) : 0;
    const markdown_core_node *old = NULL;
    uint32_t old_start = 0;
    if (records && !parent->scan) {
        S_descend(parser, parent, (uint32_t)start);
    }
    if (records && parent->scan) {
        old = S_old_child(parser, parent, (uint32_t)start, &old_start);
        /* A container's opening line is read whole (5.3). */
        if (old && (parent == parser->root || !S_starts_on_line(parser, parent->node, parser->input.line_number)) &&
            S_take(parser, parent, (uint32_t)start, carry, old, old_start)) {
            return NULL;
        }
    }
    if (parser->error) {
        return NULL;
    }
    markdown_core_node *child = make_block(parser, block_type, start);
    if (child && child->content.oom) {
        markdown_core_parser_release_node(parser, child);
        child = NULL;
    }
    /* block_parent_for already established containment. Commit that decision
     * without re-entering a possibly stateful containment predicate. */
    markdown_core_frame *frame = child ? markdown_core_parser_attach(parser, parent, child, NULL) : NULL;
    if (!frame) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        /* The loop above may have finalized blocks; keep the parser anchored
         * at a still-open ancestor so the finish path stays consistent. */
        parser->input.current = parent;
        return NULL;
    }
    child->entry = carry;
    if (old) {
        S_read_again(parser, frame, old, old_start);
    }
    return frame;
}

/* A block its lines or its content turned into `frame`'s kind reads the
 * old node of that kind which begins where it does. Its kind was not final,
 * so its choice was not (5.9): the choice it made as the kind it was goes,
 * and it chooses as the kind it is when it next needs to, its owner's
 * cursor standing where it did. */
void markdown_core_parser_kind_changed(markdown_core_parser *parser, markdown_core_frame *frame) {
    S_read_again_where_it_begins(parser, frame);
    assert(!frame->decided || frame->tentative);
    frame->decided = frame->tentative = false;
}

markdown_core_frame *markdown_core_parser_attach(markdown_core_parser *parser, markdown_core_frame *owner,
                                                 markdown_core_node *node, markdown_core_frame *before) {
    markdown_core_frame *frame = markdown_core_parser_frame(parser, node, true);
    if (!frame) {
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    if (!before) {
        S_take_another(parser, owner);
    }
    markdown_core_frame_attach(owner, frame, before);
    return frame;
}

markdown_core_frame *markdown_core_parser_attach_field(markdown_core_parser *parser, markdown_core_frame *owner,
                                                       markdown_core_node *node) {
    markdown_core_frame *frame = markdown_core_parser_frame(parser, node, false);
    if (!frame) {
        return NULL;
    }
    markdown_core_frame_attach_field(owner, frame);
    return frame;
}

void markdown_core_parser_release_frame(markdown_core_parser *parser, markdown_core_frame *frame) {
    if (frame->owner) {
        markdown_core_frame_unlink(frame);
    }
    markdown_core_frame_release(parser ? parser->pool : NULL, frame);
}

/* THE INLINE PARSER'S OWN FIELD PARSE. A token that owns fields -- a
 * citation's affixes, a directive's label -- has them parsed by the parse
 * that made it, before it scans on (markdown_core_inline_complete_token), so
 * that the token can be closed on what the fields turned out to hold. That
 * parse walks the drafts the field builds with an iterator of its own; an
 * inline root's content is not parsed this way but as the root's completion
 * begins (S_complete_inline_root), which parses nothing below it. Inline
 * parsing completes fields at their owning token; the structural walk here
 * therefore skips the emitted inline tree. */
static bool process_inline_tree(markdown_core_parser *parser, markdown_core_draft *root) {
    markdown_core_iter iter;
    markdown_core_event_type ev_type;
    bool whitespace = false;

    markdown_core_iter_init(&iter, root);
    while (!parser->error && (ev_type = markdown_core_iter_step(&iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_draft *cur = iter.cur.draft;
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, cur->node);
            if (S_kind_contains_inlines(kind, cur->node)) {
                whitespace |= markdown_core_parse_inlines(parser, cur);
                markdown_core_iter_reset(&iter, cur, MARKDOWN_CORE_EVENT_EXIT);
            }
            whitespace |= markdown_core_parse_inline_subtrees(parser, cur);
        }
    }
    return whitespace;
}

bool markdown_core_parse_inline_subtrees(markdown_core_parser *parser, markdown_core_draft *draft) {
    bool whitespace = false;
    for (markdown_core_draft *field = draft->fields; field && !parser->error; field = field->next) {
        whitespace |= process_inline_tree(parser, field);
    }
    return whitespace;
}

markdown_core_draft *markdown_core_inline_attach(markdown_core_parser *parser, markdown_core_draft *owner,
                                                 markdown_core_node *node, markdown_core_draft *before) {
    markdown_core_draft *draft = markdown_core_parser_draft(parser, node, true);
    if (!draft) {
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    markdown_core_draft_attach(owner, draft, before);
    return draft;
}

markdown_core_draft *markdown_core_inline_attach_field(markdown_core_parser *parser, markdown_core_draft *owner,
                                                       markdown_core_node *node) {
    markdown_core_draft *draft = markdown_core_parser_draft(parser, node, false);
    if (draft) {
        markdown_core_draft_attach_field(owner, draft);
    }
    return draft;
}

void markdown_core_inline_release(markdown_core_parser *parser, markdown_core_draft *draft) {
    if (draft->owner) {
        markdown_core_draft_unlink(draft);
    }
    markdown_core_draft_release(parser ? parser->pool : NULL, draft);
}

/* A FRAME PER ROOT THE COMPLETION PASS IS INSIDE: the inline root's holder,
 * and each field root of an inline node in its content -- a citation's note
 * and affixes, a directive's label -- which the inline parser filled as it
 * made the token. The iterator is the frame's own, not an allocation: the
 * pass steps it in place (iterator.h). */
typedef struct {
    markdown_core_draft *root;
    markdown_core_iter iter;
    int script_depth;
    bool started;
} complete_frame;

typedef struct {
    markdown_core_parser *parser;
    complete_frame *frames;
    /* One word per projected step per frame, zero when a root's pass starts:
     * frame i's words are `states + i * slots`. Sized with the frames. */
    void **states;
    size_t slots;
    size_t count, capacity;
    /* The word depth a root pushed now starts with. */
    int script_depth;
} complete_pass;

static int push_complete_root(markdown_core_draft *root, complete_pass *pass) {
    if (pass->count == pass->capacity) {
        size_t capacity = pass->capacity ? 2 * pass->capacity : 8;
        if (capacity > SIZE_MAX / sizeof(*pass->frames) ||
            (pass->slots && capacity > SIZE_MAX / sizeof(*pass->states) / pass->slots)) {
            markdown_core_parser_fail(pass->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return 0;
        }
        void *frames = markdown_core_parser_walk_stack(pass->parser, capacity, sizeof(*pass->frames));
        if (!frames) {
            markdown_core_parser_fail(pass->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return 0;
        }
        pass->frames = frames;
        if (pass->slots) {
            void *states = markdown_core_realloc(pass->states, capacity * pass->slots * sizeof(*pass->states));
            if (!states) {
                markdown_core_parser_fail(pass->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return 0;
            }
            pass->states = states;
        }
        pass->capacity = capacity;
    }
    if (pass->slots) {
        memset(pass->states + pass->count * pass->slots, 0, pass->slots * sizeof(*pass->states));
    }
    /* Word depth counts the inline word bodies around a node, and a block
     * begins content of its own: a root that is a block, such as an inline
     * note's Footnote, starts outside every word body its owner is in. */
    bool block = ((unsigned)root->node->kind & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK;
    pass->frames[pass->count++] =
        (complete_frame){.root = root, .script_depth = block ? 0 : pass->script_depth, .started = false};
    return 1;
}

/* A NODE'S OWN COMPLETION IS THE PASS'S ENTER: the element's
 * `complete_inline` (text.c turns an escaped space into NBSP inside a word)
 * reads the node and its ancestors' word depth and nothing else. This form is
 * for the sibling consolidation absorbs at a Text's EXIT, whose ENTER is
 * stepped over, so consolidation completes it first (iterator.h). */
static void complete_consolidated_text(markdown_core_parser *parser, markdown_core_node *node, int script_depth) {
    const markdown_core_kind_record *plan = &parser->dialect->kinds[MARKDOWN_CORE_KIND_TEXT_INDEX];
    assert(node->kind == MARKDOWN_CORE_NODE_TEXT);
    if (plan->complete) {
        plan->complete(plan->structure, parser, node, script_depth);
    }
}

/* The steps projected for one event, in descriptor order, each behind its
 * gate, until one of them consumes the node. A step that consumes the node
 * ends the event: the draft it named is gone and there is nothing left to
 * hand on.
 *
 * Returns CONSUMED when the node is gone, and FAILED with parser->error set. */
static markdown_core_complete_result run_complete_steps(markdown_core_parser *parser,
                                                        const markdown_core_complete_step_entry *entry,
                                                        markdown_core_draft *draft, markdown_core_event_type event,
                                                        int is_root, void **states) {
    markdown_core_complete_result result = MARKDOWN_CORE_COMPLETE_CONTINUE;
    for (; entry->instance; entry++) {
        if (!markdown_core_complete_step_admitted(entry, parser)) {
            continue;
        }
        result = entry->instance->element->complete_step(entry->instance, parser, draft, event, is_root,
                                                         &states[entry->slot]);
        if (result != MARKDOWN_CORE_COMPLETE_CONTINUE) {
            /* A node is consumed only at its EXIT: at ENTER the lookahead
             * names its first child, and a step that freed it here would
             * have broken the LOCAL contract the API header states. */
            assert(result == MARKDOWN_CORE_COMPLETE_FAILED || event == MARKDOWN_CORE_EVENT_EXIT);
            break;
        }
    }
    return result;
}

/* A DRAFT IS COMPLETE once the nodes it holds are (5.8): its children become
 * its stem, and the document decides what it continues and numbers what it
 * holds. */
static void S_complete_draft(markdown_core_parser *parser, markdown_core_draft *draft) {
    if (!markdown_core_draft_freeze(parser->pool, draft, markdown_core_parser_kind(parser, draft->node)->summary)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    const markdown_core_element_instance *document = parser->dialect->document_structure;
    document->element->complete_draft(document, parser, draft);
}

void markdown_core_inline_complete(markdown_core_parser *parser, markdown_core_draft *draft) {
    markdown_core_iter iter;
    markdown_core_iter_init(&iter, draft);
    while (!parser->error && markdown_core_iter_step(&iter) != MARKDOWN_CORE_EVENT_DONE) {
        if (iter.cur.ev_type == MARKDOWN_CORE_EVENT_EXIT) {
            S_complete_draft(parser, iter.cur.draft);
        }
    }
}

/* A BLOCK COMPLETES AS ITS BUILDER IS DONE (5.8, 5.11): its children, each
 * complete, become its stem, and the document numbers what it holds,
 * measured from `start`. One that holds an inline root keeps the old root's
 * content or leaves its content to the inline stage (5.7). */
static void S_complete_node(markdown_core_parser *parser, markdown_core_frame *frame, uint32_t start) {
    frame->completed = true;
    const markdown_core_element_instance *document = parser->dialect->document_structure;
    document->element->complete_node(document, parser, frame, start);
}

void markdown_core_parser_complete_node(markdown_core_parser *parser, markdown_core_frame *frame) {
    S_complete_node(parser, frame, frame->node->where.place.start);
}

bool markdown_core_parser_contains_inlines(markdown_core_parser *parser, markdown_core_node *node) {
    return S_kind_contains_inlines(markdown_core_parser_kind(parser, node), node);
}

bool markdown_core_parser_complete_root(markdown_core_parser *parser, markdown_core_frame *frame,
                                        markdown_core_node *holder, markdown_core_place place,
                                        const markdown_core_node *old, uint32_t anchor) {
    assert(!parser->completing);
    markdown_core_draft *builder = markdown_core_parser_draft(parser, holder, false);
    if (!builder) {
        return false;
    }
    markdown_core_inline_root root = {frame->node, holder, frame, builder, place, frame->field || holder != frame->node,
                                      old,         anchor};
    S_complete_inline_root(parser, &root);
    return !parser->error;
}

markdown_core_node *markdown_core_parser_owner(const markdown_core_parser *parser, const markdown_core_frame *frame) {
    (void)parser;
    return frame->owner ? frame->owner->node : NULL;
}

/* The furthest content offset at which a delimiter its token pushed, or one
 * it holds, left the stack. */
static int32_t S_reads_until(const markdown_core_parser *parser, const markdown_core_inline_reads *reads) {
    const int32_t stay = reads->stay ? parser->stays[reads->stay - 1] : -1;
    return stay > reads->until ? stay : reads->until;
}

/* `into` also read what `from` read: the decisions about a node read what
 * those about the nodes it holds or absorbs read. */
static void S_join_reads(const markdown_core_parser *parser, markdown_core_inline_reads *into,
                         const markdown_core_inline_reads *from) {
    const int32_t until = S_reads_until(parser, from);
    into->rules |= from->rules;
    into->low = from->low < into->low ? from->low : into->low;
    into->reach = from->reach > into->reach ? from->reach : into->reach;
    into->until = until > into->until ? until : into->until;
    into->flags = (into->flags & from->flags & (MARKDOWN_CORE_INLINE_RECORDED | MARKDOWN_CORE_INLINE_LOCAL)) |
                  ((into->flags | from->flags) & (MARKDOWN_CORE_INLINE_BOUNDARY | MARKDOWN_CORE_INLINE_CONTEXT));
}

/* A TEXT ABSORBS THE TEXTS AFTER IT (markdown_core_consolidate_text_step),
 * and what their decisions read with them. */
static void S_absorb_reads(const markdown_core_parser *parser, markdown_core_draft *text) {
    for (markdown_core_draft *next = text->next; next && next->node->kind == MARKDOWN_CORE_NODE_TEXT;
         next = next->next) {
        S_join_reads(parser, &text->reads, &next->reads);
        text->reads.end = next->reads.end;
    }
}

/* AN INLINE NODE'S ENTRY (docs/plans/2026-09-29-incremental-parsing.md, 5.6),
 * once it is complete: a later parse may take it whole where the stack holds
 * no entry of the rules its decisions counted or searched, when its
 * decisions read nothing but the root's content, none read the nodes around
 * it or a token held open where it begins, the rules they counted had no
 * entry there, every delimiter it holds left the stack by its end, and it
 * lies on the bytes it was read from: where the root's content is all the
 * old one's, and else when every decision about it said what it read. Its
 * reach is the content offset its decisions read up to. Its owner read what
 * it read. */
static void S_settle_reads(markdown_core_parser *parser, markdown_core_draft *draft) {
    markdown_core_inline_reads *reads = &draft->reads;
    markdown_core_node *node = draft->node;
    reads->until = S_reads_until(parser, reads);
    reads->stay = 0;
    if ((reads->flags & (MARKDOWN_CORE_INLINE_LOCAL | MARKDOWN_CORE_INLINE_CONTEXT)) == MARKDOWN_CORE_INLINE_LOCAL &&
        !(reads->state & MARKDOWN_CORE_INLINE_HELD) && !(reads->rules & reads->state) && reads->until <= reads->end &&
        node->where.place.start == (uint32_t)reads->start && node->where.place.end == (uint32_t)reads->end) {
        node->entry = markdown_core_inline_entry(reads->flags & MARKDOWN_CORE_INLINE_RECORDED, reads->rules,
                                                 reads->flags & MARKDOWN_CORE_INLINE_BOUNDARY,
                                                 (uint32_t)(reads->start - reads->low));
        node->reach = (uint32_t)reads->reach;
    }
    S_join_reads(parser, &draft->owner->reads, reads);
}

const markdown_core_node *markdown_core_parser_old_root(markdown_core_parser *parser, const markdown_core_frame *frame,
                                                        const markdown_core_node *holder, uint32_t *anchor) {
    const bool decided = frame->decided && frame->old;
    const markdown_core_node *old = decided ? frame->old : frame->scan;
    if (frame->field || holder != frame->node || !old || old->kind != frame->node->kind ||
        !markdown_core_parser_contains_inlines(parser, (markdown_core_node *)old)) {
        return NULL;
    }
    *anchor = (uint32_t)((int64_t)(decided ? frame->old_start : frame->scan_start) - old->where.extent.lead);
    return old;
}

/* A TAKEN NODE THE ROOT'S COMPLETION CHANGES IS READ AS A NEW ONE: a Text
 * that merges with a Text beside it or holds no bytes, or one that no longer
 * lies on the bytes it was taken at. It continues what its search finds, and
 * its entry and reach are its decisions'. */
static void S_settle_taken(markdown_core_draft *draft) {
    const markdown_core_node *node = draft->node;
    if (!draft->taken || (node->where.place.start == (uint32_t)draft->reads.start &&
                          node->where.place.end == (uint32_t)draft->reads.end &&
                          !(node->kind == MARKDOWN_CORE_NODE_TEXT && markdown_core_text_needs_consolidation(draft)))) {
        return;
    }
    draft->taken = draft->decided = false;
    draft->old = NULL;
    draft->old_start = 0;
    draft->node->entry = 0;
    draft->node->reach = 0;
}

/* AN INLINE ROOT COMPLETES ITS OWN TREE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.7, 5.8): in the inline stage, its content is parsed into
 * its holder's draft, and one pass over the holder's tree and every field
 * root in it completes each node, on one explicit continuation stack. The
 * root is numbered already, and its holder held, by its owner's stem or
 * field, and holds nothing yet.
 *
 * At a node's ENTER the pass runs the steps projected for the ENTER,
 * completes the node itself (the element's `complete_inline` the record
 * carries) and pushes the field roots its draft builds, which it passes
 * before the node's children. At a Text's EXIT it consolidates the run when
 * there is one to merge or a Text with no bytes to drop; at every EXIT it
 * runs the steps projected for the EXIT, in descriptor order, each behind its
 * gate, and then the node, its subtree complete, takes its stem, decides what
 * it continues and numbers what it holds. The holder's EXIT numbers the
 * content, and the root completes. The drafts go with the pass. */
static void S_complete_pass(markdown_core_parser *parser, markdown_core_inline_root *root, complete_pass *pass) {
    const markdown_core_kind_record *const kinds = parser->dialect->kinds;
    const markdown_core_complete_step_entry *const *const dispatch = parser->dialect->complete_dispatch;
    markdown_core_draft *holder = root->builder;
    /* Its range and its kind are final: it chooses what it continues, whose
     * content it reads again (5.6), before it reads its own (5.9). */
    const markdown_core_element_instance *document = parser->dialect->document_structure;
    document->element->decide_frame(document, parser, root->frame);
    parser->completing = root;
    parser->asker = root->node;
    if (!parser->error) {
        markdown_core_parse_root_inlines(parser, holder, root->old, root->anchor);
    }
    /* A read that waits completes nothing: the questions it asked go with
     * it, and the root is read again with the other roots. */
    if (parser->waits) {
        markdown_core_registry_unlink(root->node);
    }
    pass->script_depth = 0;
    if (!parser->error && !parser->waits) {
        push_complete_root(holder, pass);
    }
    while (pass->count && !parser->error) {
        complete_frame *frame = &pass->frames[pass->count - 1];
        /* `frame` is the top of the stack, so its state words are the last row. */
        void **states = pass->states + (pass->count - 1) * pass->slots;
        markdown_core_iter *iter = &frame->iter;
        if (!frame->started) {
            markdown_core_iter_init(iter, frame->root);
            frame->started = true;
        }
        for (;;) {
            markdown_core_event_type event = markdown_core_iter_step(iter);
            markdown_core_complete_result result;
            if (event == MARKDOWN_CORE_EVENT_DONE) {
                pass->count--;
                break;
            }
            markdown_core_draft *draft = iter->cur.draft;
            markdown_core_node *node = draft->node;
            size_t index = markdown_core_kind_index((markdown_core_node_type)node->kind);
            /* A field root belongs to its owner and is never replaced; so is
             * the holder of a field's content. */
            int is_root = draft == frame->root && (pass->count > 1 || root->field);
            if (node->element && node->element->delimiter.body == DELIMITER_WORD_BODY) {
                frame->script_depth += event == MARKDOWN_CORE_EVENT_ENTER ? 1 : -1;
            }
            if (event == MARKDOWN_CORE_EVENT_EXIT) {
                /* Consolidation goes first at a Text's EXIT and takes the
                 * pass's own iterator over the siblings it absorbs, so by the
                 * time an element step sees this event the Text holds its
                 * whole run and the absorbed drafts are gone -- they were
                 * never delivered to anything else, which is what makes
                 * releasing them safe. */
                result = MARKDOWN_CORE_COMPLETE_CONTINUE;
                if (index == MARKDOWN_CORE_KIND_TEXT_INDEX && !draft->taken &&
                    markdown_core_text_needs_consolidation(draft)) {
                    S_absorb_reads(parser, draft);
                    result = markdown_core_consolidate_text_step(parser, iter, draft, complete_consolidated_text,
                                                                 frame->script_depth);
                }
                if (result == MARKDOWN_CORE_COMPLETE_CONTINUE && !draft->taken && dispatch[2 * index + 1]) {
                    result = run_complete_steps(parser, dispatch[2 * index + 1], draft, event, is_root, states);
                }
                if (result == MARKDOWN_CORE_COMPLETE_FAILED) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
                if (result == MARKDOWN_CORE_COMPLETE_CONTINUE) {
                    /* The nodes of the root's content keep what their
                     * decisions read; a field's are read with their token. */
                    if (pass->count == 1 && draft != holder) {
                        S_settle_reads(parser, draft);
                    }
                    /* A taken node is complete: its owner numbers it. */
                    if (!draft->taken) {
                        S_complete_draft(parser, draft);
                    }
                    if (draft == holder && !parser->error) {
                        /* The root's content is numbered: the root
                         * completes, numbering all else it holds. */
                        document->element->complete_node(document, parser, root->frame, root->place.start);
                    }
                    if (parser->error) {
                        break;
                    }
                }
                continue;
            }
            S_settle_taken(draft);
            if (draft->taken) {
                continue;
            }
            const markdown_core_kind_record *facts = &kinds[index];
            if (dispatch[2 * index]) {
                result = run_complete_steps(parser, dispatch[2 * index], draft, event, is_root, states);
                if (result == MARKDOWN_CORE_COMPLETE_FAILED) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
            }
            if (facts->complete) {
                facts->complete(facts->structure, parser, node, frame->script_depth);
            }
            if (!draft->fields) {
                continue;
            }
            pass->script_depth = frame->script_depth;
            size_t first = pass->count;
            for (markdown_core_draft *field = draft->fields; field; field = field->next) {
                if (!push_complete_root(field, pass)) {
                    break;
                }
            }
            if (parser->error) {
                break;
            }
            /* The fields are in canonical order; a stack consumes them
             * reversed. The frames carry no state yet -- a pushed root's
             * words are zero until its pass starts -- so swapping frames
             * leaves nothing behind. The push may have moved the frames, so
             * the top is taken afresh: it is the first field root, passed
             * before this node's children. */
            for (size_t left = first, right = pass->count; left < right && left < --right; left++) {
                complete_frame swap = pass->frames[left];
                pass->frames[left] = pass->frames[right];
                pass->frames[right] = swap;
            }
            break;
        }
    }
    pass->count = 0;
    parser->completing = NULL;
    parser->asker = NULL;
    root->builder = NULL;
    markdown_core_drafts_clear(&parser->drafts, parser->pool);
}

/* The pass borrows the parser's walk stack for its frames and keeps its
 * state words for the next root. */
static void S_complete_inline_root(markdown_core_parser *parser, markdown_core_inline_root *root) {
    complete_pass pass = {.parser = parser,
                          .slots = parser->dialect->complete_step_slots,
                          .states = parser->complete_states,
                          .capacity = parser->complete_capacity};
    if (pass.capacity) {
        pass.frames = markdown_core_parser_walk_stack(parser, pass.capacity, sizeof(*pass.frames));
        if (!pass.frames) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            markdown_core_drafts_clear(&parser->drafts, parser->pool);
            return;
        }
    }
    S_complete_pass(parser, root, &pass);
    parser->complete_states = pass.states;
    parser->complete_capacity = pass.capacity;
}

static void finalize_document(markdown_core_parser *parser) {
    /* The blocks open at the end of the input read all of it. */
    parser->input.line_reach = (uint32_t)parser->input.length;
    while (parser->input.current != parser->root) {
        parser->input.current = markdown_core_block_finalize(parser, parser->input.current);
    }

    markdown_core_block_finalize(parser, parser->root);
}

/* A CELL READ AS BLOCKS reads its content as an input of its own, which
 * its blocks close in as the document's do, while the input it was read in
 * is put aside; the cell then completes, measured from where it starts. Its
 * kind and its range are final: it decides what it continues (5.9) before
 * its blocks do. Its content is spent: what it holds is its blocks. */
void markdown_core_parser_read_blocks(markdown_core_parser *parser, markdown_core_frame *frame) {
    markdown_core_node *cell = frame->node;
    const markdown_core_element_instance *document = parser->dialect->document_structure;
    document->element->decide_frame(document, parser, frame);
    if (parser->error) {
        return;
    }
    const markdown_core_parser_input outer = parser->input;
    parser->input = (markdown_core_parser_input){0};
    if (S_input_begin(parser)) {
        assert(cell->content.size);
        parser->input.block_root = frame;
        parser->input.current = frame;
        parser->input.line_number = parser->line_marks[cell->content_map.first].line - 1;
        parser->input.last_line_end = parser->line_marks[cell->content_map.first].source;
        cell->flags |= MARKDOWN_CORE_NODE__OPEN;
        const markdown_core_input content = markdown_core_input_buffer(cell->content.ptr, (size_t)cell->content.size);
        S_parse_source(parser, &content);
        while (parser->input.current != frame && !parser->error) {
            parser->input.current = markdown_core_block_finalize(parser, parser->input.current);
        }
        S_complete_trailed(parser, frame);
        cell->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    }
    markdown_core_strbuf_clear(&cell->content);
    cell->content_map.count = 0;
    cell->content_map.offset = 0;
    S_input_dispose(parser);
    parser->input = outer;
    if (!parser->error) {
        S_complete_node(parser, frame, cell->where.place.start);
    }
}

static const unsigned char *S_read_buffer(const markdown_core_input *input, size_t offset, size_t *size) {
    *size = input->size - offset;
    return (const unsigned char *)input->payload + offset;
}

markdown_core_input markdown_core_input_buffer(const unsigned char *bytes, size_t size) {
    return (markdown_core_input){S_read_buffer, bytes, size};
}

markdown_core_node *markdown_core_parser_parse(markdown_core_parser *parser, const markdown_core_input *input,
                                               markdown_core_revision *revision) {
    markdown_core_registry_begin(&revision->pool->registry);
    markdown_core_node *document = NULL;
    S_parse_begin(parser, revision);
    if (!parser->error) {
        S_parse_source(parser, input);
        document = S_finish_parse(parser);
    }
    S_parse_end(parser);
    return document;
}

/* Materializing a normalized view is an allocation boundary, separate from
 * the ordinary borrowed view resolved inline by both consumers. */
static const unsigned char *S_normalize_input_line(markdown_core_parser *parser, markdown_core_input_line *line,
                                                   markdown_core_line_facts *facts, bufsize_t length) {
    const unsigned char *raw = markdown_core_parser_line_bytes(parser, line);
    if (!raw) {
        return NULL;
    }
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
        unsigned char byte = raw[at - line->start];
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
    view->next = parser->input.normalized_lines;
    parser->input.normalized_lines = view;
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
        return markdown_core_parser_line_bytes(parser, line);
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

/* Makes the chunk that holds input byte `offset` the current one: a chunk
 * read before, found among them, or the chunk at `offset`, read now and kept
 * in order of its start. The chunks read leave out what the parse took
 * without reading (5.3), so a read may fall between them. False, with the
 * parse failed, when the chunk could not be recorded. */
static bool S_input_seek_chunk(markdown_core_parser *parser, size_t offset) {
    size_t lo = 0, hi = parser->input.chunk_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (parser->input.chunks[mid].start <= offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    /* `lo` chunks start at or before `offset`. */
    size_t index = lo;
    if (!lo || offset - parser->input.chunks[lo - 1].start >= parser->input.chunks[lo - 1].size) {
        assert(offset < parser->input.length);
        markdown_core_input_chunk *chunks = markdown_core_reserve(parser->input.chunks, &parser->input.chunk_capacity,
                                                                  parser->input.chunk_count + 1, sizeof(*chunks));
        if (!chunks) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        parser->input.chunks = chunks;
        size_t size;
        const unsigned char *bytes = parser->input.source.read(&parser->input.source, offset, &size);
        /* A chunk read later may start inside this one: it ends there. */
        if (lo < parser->input.chunk_count && chunks[lo].start - offset < size) {
            size = chunks[lo].start - offset;
        }
        memmove(chunks + lo + 1, chunks + lo, (parser->input.chunk_count - lo) * sizeof(*chunks));
        chunks[lo] = (markdown_core_input_chunk){offset, size, bytes};
        parser->input.chunk_count++;
    } else {
        index = lo - 1;
    }
    const markdown_core_input_chunk *chunk = &parser->input.chunks[index];
    parser->input.chunk = chunk->bytes;
    parser->input.chunk_start = chunk->start;
    parser->input.chunk_size = chunk->size;
    return true;
}

/* S_input_seek_chunk, for an offset the current chunk most often holds. */
static inline bool S_input_seek(markdown_core_parser *parser, size_t offset) {
    return offset - parser->input.chunk_start < parser->input.chunk_size || S_input_seek_chunk(parser, offset);
}

/* The bytes [start, end) of the input, which span chunks, joined into one
 * view that lives as long as the input. NULL, with the parse failed, when
 * the view could not be allocated. */
static const unsigned char *S_join_input(markdown_core_parser *parser, size_t start, size_t end) {
    markdown_core_normalized_line *view = markdown_core_alloc(1, sizeof(*view) + (end - start));
    if (!view) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    for (size_t at = start; at < end;) {
        if (!S_input_seek(parser, at)) {
            markdown_core_free(view);
            return NULL;
        }
        size_t from = at - parser->input.chunk_start;
        size_t take = parser->input.chunk_size - from < end - at ? parser->input.chunk_size - from : end - at;
        memcpy(view->bytes + (at - start), parser->input.chunk + from, take);
        at += take;
    }
    view->next = parser->input.normalized_lines;
    parser->input.normalized_lines = view;
    return view->bytes;
}

/* The sole physical-line scanner for root and mapped inputs. Grammar facts
 * live beside their line, so changing inputs drops them together. It reads
 * the input a chunk at a time and records where each line starts and ends;
 * markdown_core_parser_line_bytes gives a line's bytes. Inlining is explicit:
 * both GCC and Clang may otherwise outline this per-line step. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_input_line *S_extend_source_lines(markdown_core_parser *parser, size_t index) {
    assert(parser->input.length <= MARKDOWN_CORE_SOURCE_CAPACITY);
    while (index >= parser->input.line_count && parser->input.scanned < parser->input.length) {
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
        size_t at = parser->input.scanned;
        /* Scan spans ending at a line boundary or a normalization boundary,
         * chunk by chunk. The NUL count changes only at the latter, not on
         * every source byte. */
        while (at < parser->input.length) {
            if (!S_input_seek(parser, at)) {
                return NULL;
            }
            const unsigned char *chunk = parser->input.chunk + (at - parser->input.chunk_start);
            const unsigned char *end = chunk + parser->input.chunk_size - (at - parser->input.chunk_start);
            const unsigned char *cursor = S_source_span_end(chunk, end);
            at += (size_t)(cursor - chunk);
            if (cursor == end) {
                continue;
            }
            if (*cursor) {
                break;
            }
            nul_count++;
            at++;
        }
        entry.end = (uint32_t)at;
        for (unsigned char terminator = '\r'; at < parser->input.length; terminator = '\n') {
            if (!S_input_seek(parser, at)) {
                return NULL;
            }
            if (parser->input.chunk[at - parser->input.chunk_start] == terminator) {
                at++;
            }
            if (terminator == '\n') {
                break;
            }
        }
        if (nul_count) {
            markdown_core_line_facts *facts = markdown_core_parser_extend_line_facts(parser, &entry);
            if (!facts) {
                return NULL;
            }
            facts->nul_count = nul_count;
            parser->input.mapped = true;
        }
        parser->input.scanned = at;
        parser->input_line_work += parser->input.scanned - entry.start;
        parser->input.lines[parser->input.line_count++] = entry;
    }
    return index < parser->input.line_count ? &parser->input.lines[index] : NULL;
}

/* The input's bytes [start, end), within the chunk that holds `start`, or
 * NULL when they span chunks or the chunk could not be recorded. */
static inline const unsigned char *S_input_borrow(markdown_core_parser *parser, size_t start, size_t end) {
    if (!S_input_seek(parser, start) || end - parser->input.chunk_start > parser->input.chunk_size) {
        return NULL;
    }
    return parser->input.chunk + (start - parser->input.chunk_start);
}

const unsigned char *markdown_core_parser_input_view(markdown_core_parser *parser, int first, int last) {
    const size_t start = markdown_core_parser_visited_line(parser, first)->start;
    const size_t end = markdown_core_input_line_next(parser, markdown_core_parser_visited_line(parser, last));
    const unsigned char *bytes = S_input_borrow(parser, start, end);
    return bytes || parser->error ? bytes : S_join_input(parser, start, end);
}

const unsigned char *markdown_core_parser_line_bytes(markdown_core_parser *parser, markdown_core_input_line *line) {
    const size_t next = markdown_core_input_line_next(parser, line);
    const unsigned char *bytes = S_input_borrow(parser, line->start, next);
    if (bytes || parser->error) {
        return bytes;
    }
    markdown_core_line_facts *facts =
        line->facts ? &parser->input.facts[line->facts - 1] : markdown_core_parser_extend_line_facts(parser, line);
    if (facts && !facts->joined) {
        facts->joined = S_join_input(parser, line->start, next);
    }
    return facts ? facts->joined : NULL;
}

/* External speculative readers share the driver's scanner. The driver calls
 * the static inline body directly, including when it advances the frontier. */
markdown_core_input_line *markdown_core_parser_extend_source_lines(markdown_core_parser *parser, size_t index) {
    return S_extend_source_lines(parser, index);
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

/* The open blocks as a line that is not blank leaves them, after a run of
 * lines a parse took (5.3, E5). */
static void S_after_text(markdown_core_parser *parser) {
    parser->input.blank = false;
    for (markdown_core_frame *open = parser->input.current; open; open = open->owner) {
        open->node->flags &= ~(MARKDOWN_CORE_NODE__LAST_LINE_BLANK | MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK);
    }
}

/* The parse goes on after a candidate it took (5.3), at the line after the
 * candidate's last one, or at the line the untouched node after it begins
 * on, numbered after the line that took it: the index starts there, the open
 * blocks are as that node's entry records, and no line before it is read
 * again. */
static void S_resume(markdown_core_parser *parser) {
    parser->input.taken = false;
    parser->input.first_line = parser->input.line_number + 1;
    parser->input.line_count = 0;
    parser->input.scanned = parser->input.resume;
    parser->input.last_line_end = parser->input.resume_last_end;
    parser->input.lookahead_last_line_ready = false;
    S_after_text(parser);
    parser->input.blank = parser->input.resume_blank;
    parser->input.current->node->flags |= parser->input.resume_flags;
}

static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_parse_source(markdown_core_parser *parser,
                                                               const markdown_core_input *input) {
    assert(input->size <= MARKDOWN_CORE_SOURCE_CAPACITY);
    S_clear_normalized_lines(parser);
    parser->input.source = *input;
    parser->input.chunk = NULL;
    parser->input.chunk_start = parser->input.chunk_size = 0;
    parser->input.chunk_count = 0;
    parser->input.length = input->size;
    parser->input.scanned = 0;
    parser->input.line_count = 0;
    parser->input.fact_count = 0;
    bool at_root = parser->input.block_root == parser->root;
    parser->input.mapped = !at_root;
    parser->input.first_line = parser->input.line_number + 1;
    parser->input.lookahead_last_line_ready = false;
    if (at_root) {
        const markdown_core_element_instance *document = parser->dialect->document_structure;
        document->element->read_document_prefix(document, parser);
    }
    while (!parser->error) {
        size_t index = (size_t)(parser->input.line_number + 1 - parser->input.first_line);
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
        parser->input.line_start = (bufsize_t)found->start;
        /* Every decision on the line reads it through its terminator. */
        parser->input.line_reach = (uint32_t)markdown_core_input_line_next(parser, found);
        /* A document line's scan recorded where its content ends; a cell's
         * line ends where the cell's map places its last byte. */
        if (at_root) {
            parser->input.line_end = (bufsize_t)found->end;
        } else {
            parser->input.line_end =
                markdown_core_parser_mapped_source_end(parser, parser->input.line_number + 1, content_length);
        }
        markdown_core_frame *const before = parser->input.current;
        parser->input.plain.flags = 0;
        parser->input.lines_taken = parser->input.read_leaf = false;
        S_process_line(parser, content, content_length);
        if (at_root && parser->input.current) {
            S_record_line(parser, before);
            S_raise_reach(parser->input.current->node, parser->input.line_reach);
        }
        if (parser->input.taken) {
            S_resume(parser);
        }
        if (parser->input.claimed) {
            assert(parser->input.claimed_line >= parser->input.line_number);
            parser->input.line_number = parser->input.claimed_line;
            parser->input.line_start =
                (bufsize_t)markdown_core_parser_visited_line(parser, parser->input.line_number)->start;
            parser->input.last_line_end = parser->input.claimed_last_end;
            parser->input.claimed = false;
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
// parser->input.first_nonspace, parser->input.first_nonspace_column,
// parser->input.indent, and parser->input.blank. Does not advance parser->input.offset.
/* THE DISTANCE TO THE NEXT TAB STOP IS DERIVED, NOT CARRIED.
 *
 * This used to open with `chars_to_tab = TAB_STOP - (parser->input.column %
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

    if (parser->input.first_nonspace <= parser->input.offset) {
        parser->input.first_nonspace = parser->input.offset;
        parser->input.first_nonspace_column = parser->input.column;
        while ((c = peek_at(input, parser->input.first_nonspace))) {
            if (c == ' ') {
                parser->input.first_nonspace += 1;
                parser->input.first_nonspace_column += 1;
            } else if (c == '\t') {
                parser->input.first_nonspace += 1;
                parser->input.first_nonspace_column += TAB_STOP - (parser->input.first_nonspace_column % TAB_STOP);
            } else {
                break;
            }
        }
    }

    parser->input.indent = parser->input.first_nonspace_column - parser->input.column;
    parser->input.blank = markdown_core_is_line_end(peek_at(input, parser->input.first_nonspace));
}

// Advance parser->input.offset and parser->input.column.  parser->input.offset is the
// byte position in input; parser->input.column is a virtual column number
// that counts each Unicode scalar once and expands tabs to tab stops.
// Source positions remain byte-based. The count parameter indicates
// how far to advance the offset.  If columns is true, then count
// indicates a number of columns; otherwise, a number of bytes.
// If advancing a certain number of columns partially consumes
// a tab character, parser->input.partially_consumed_tab is set to true.
void markdown_core_block_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count,
                                        bool columns) {
    char c;
    int chars_to_tab;
    int chars_to_advance;
    while (count > 0 && parser->input.offset < input->len && (c = peek_at(input, parser->input.offset))) {
        if (c == '\t') {
            chars_to_tab = TAB_STOP - (parser->input.column % TAB_STOP);
            if (columns) {
                parser->input.partially_consumed_tab = chars_to_tab > count;
                chars_to_advance = MIN(count, chars_to_tab);
                parser->input.column += chars_to_advance;
                parser->input.offset += (parser->input.partially_consumed_tab ? 0 : 1);
                count -= chars_to_advance;
            } else {
                parser->input.partially_consumed_tab = false;
                parser->input.column += chars_to_tab;
                parser->input.offset += 1;
                count -= 1;
            }
        } else {
            parser->input.partially_consumed_tab = false;
            parser->input.offset += 1;
            /* Valid UTF-8 is a caller precondition. Complete a virtual column
             * only at the scalar's end. Byte-counted advances may split a
             * scalar; column-counted advances always consume it completely. */
            bool scalar_end =
                parser->input.offset == input->len || (peek_at(input, parser->input.offset) & 0xC0) != 0x80;
            parser->input.column += scalar_end;
            count -= columns ? scalar_end : 1;
        }
    }
}

static bool S_last_child_is_open(const markdown_core_frame *container) {
    return container->last && (container->last->node->flags & MARKDOWN_CORE_NODE__OPEN);
}

bool markdown_core_block_continue_indented(markdown_core_parser *parser, markdown_core_chunk *input, int continuation,
                                           bool accepts_blank) {
    bool res = false;

    if (parser->input.indent >= continuation) {
        markdown_core_block_advance_offset(parser, input, continuation, true);
        res = true;
    } else if (parser->input.blank && accepts_blank) {
        // Blankness is relative to the cursor after ancestor prefixes. Lists
        // require a first block; definition-list bodies first check whether
        // the blank run leads to a carried line. Footnotes and specimens
        // accept blank continuation directly, including during lookahead.
        markdown_core_block_advance_offset(parser, input, parser->input.first_nonspace - parser->input.offset, false);
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
                                       markdown_core_frame *container, markdown_core_chunk *input,
                                       const markdown_core_frame *joining, bool *taken) {
    return !structure || !structure->element->continue_container ||
           structure->element->continue_container(structure, parser, container, input, joining, taken);
}

static bool parse_element_block(markdown_core_parser *parser, const markdown_core_kind_record *kind,
                                markdown_core_frame *container, markdown_core_chunk *input, bool *should_continue,
                                markdown_core_frame **closing) {
    const markdown_core_element_instance *structure = kind->structure;
    int matched;

    if (!structure->element->last_block_matches) {
        return false;
    }

    matched = structure->element->last_block_matches(structure, parser, input->data, input->len, container);
    if (matched && structure->element->pending_close) {
        *closing = matched == MARKDOWN_CORE_BLOCK_PENDING_CLOSE ? container : NULL;
    } else if (matched && S_kind_accepts_lines(kind, container->node)) {
        *closing = NULL;
    }
    if (matched != MARKDOWN_CORE_BLOCK_CLOSED) {
        return matched != 0;
    }

    /* The container's own closing line. Everything still open inside it ended
     * on the line before, and the container ends here.
     *
     * `parser->input.current` is the deepest open block and `container` is on the
     * path from the root to it, so walking up through `markdown_core_block_finalize` reaches it.
     * Definition-only paragraphs stay attached until block parsing completes;
     * no block can disappear while it is still on the open spine. */
    *should_continue = false;
    while (parser->input.current != container) {
        parser->input.current = markdown_core_block_finalize(parser, parser->input.current);
        assert(parser->input.current != NULL);
    }
    /* A block survives its own finalization and can still be positioned. */
    assert(!(kind->flags & MARKDOWN_CORE_KIND_IS_PARAGRAPH));
    container->node->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
    parser->input.current = markdown_core_block_finalize(parser, container);
    return false;
}

static markdown_core_frame *check_open_blocks(markdown_core_parser *parser, markdown_core_chunk *input,
                                              bool *all_matched) {
    bool should_continue = true;
    *all_matched = false;
    markdown_core_frame *container = parser->input.block_root;
    markdown_core_frame *closing = NULL;

    while (S_last_child_is_open(container)) {
        /* Whatever a container's prefix consumed is that container's
         * MARKER: `> ` belongs to the block quote, the item's indent to the
         * list item. So the bytes of the block below begin where the
         * prefixes above it end: one claim per block, walking down the
         * spine, each naming the line by number, as matching a container can
         * read lines ahead, which can move the line table. */
        markdown_core_parser_visited_line(parser, parser->input.line_number)->own = (uint32_t)parser->input.offset;
        container = container->last;

        markdown_core_block_find_first_nonspace(parser, input);

        const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, container->node);
        const markdown_core_element_instance *structure = kind->structure;
        if (structure && structure->element->last_block_matches) {
            if (!parse_element_block(parser, kind, container, input, &should_continue, &closing)) {
                goto done;
            }
        } else {
            bool taken = false;
            if (structure && structure->element->accepts_blank && parser->input.blank &&
                !structure->element->accepts_blank(structure, parser, container)) {
                goto done;
            }
            if (!S_container_prefix_matches(parser, structure, container, input, NULL, &taken)) {
                goto done;
            }
            if (taken) {
                if (S_kind_accepts_lines(markdown_core_parser_kind(parser, parser->input.current->node),
                                         parser->input.current->node)) {
                    markdown_core_block_add_line(parser->input.current->node, input, parser);
                }
                return NULL;
            }
        }
    }

    *all_matched = true;

done:
    if (closing) {
        while (parser->input.current != closing) {
            parser->input.current = markdown_core_block_finalize(parser, parser->input.current);
        }
        /* The pending close is the container's own end condition: it ends on
         * the line in hand. */
        closing->node->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
        parser->input.current = markdown_core_block_finalize(parser, closing);
        return NULL;
    }
    /* A container whose prefix consumed bytes and then declined still read
     * them; they are its marker up to the point it gave up. */
    if (!*all_matched) {
        container = container->owner; // back up to last matching node
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
    markdown_core_frame **chain;
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
        if (!(markdown_core_parser_kind(parser, parser->lookahead_chain[i]->node)->flags &
              MARKDOWN_CORE_KIND_BLANK_RUNS)) {
            return false;
        }
    }
    return true;
}

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, markdown_core_frame *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead) {
    markdown_core_frame *parent = parent_container;
    markdown_core_frame *node;
    int depth = 0;
    int i;

    memset(lookahead, 0, sizeof(*lookahead));
    /* The block joins the nearest open container that can hold it, which is
     * where `markdown_core_parser_add_child` backs up to when it is opened. */
    while (parent != parser->input.block_root && !markdown_core_node_can_contain_type(parent->node, child)) {
        parent = parent->owner;
    }
    for (node = parent; node; node = node == parser->input.block_root ? NULL : node->owner) {
        depth++;
    }
    if (!S_lookahead_reserve_chain(parser, depth)) {
        return false;
    }
    i = depth;
    for (node = parent; node; node = node == parser->input.block_root ? NULL : node->owner) {
        i--;
        parser->lookahead_chain[i] = node;
        parser->lookahead_chain_flags[i] = node->node->flags;
    }

    lookahead->parser = parser;
    lookahead->parent = parent;
    lookahead->depth = depth;
    lookahead->line = parser->input.line_number + 1;
    lookahead->saved_offset = parser->input.offset;
    lookahead->saved_column = parser->input.column;
    lookahead->saved_first_nonspace = parser->input.first_nonspace;
    lookahead->saved_first_nonspace_column = parser->input.first_nonspace_column;
    lookahead->saved_indent = parser->input.indent;
    lookahead->saved_blank = parser->input.blank;
    lookahead->saved_partially_consumed_tab = parser->input.partially_consumed_tab;
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
        const size_t next = markdown_core_input_line_next(parser, geometry);
        parser->block_lookahead_work++;
        if (geometry->facts && parser->input.facts[geometry->facts - 1].nul_count) {
            input.data = (unsigned char *)content;
            input.len = content_length + 1;
        } else if (next == parser->input.length) {
            /* The input's last line, normalized once: the matchers read a line
             * through its terminator, and the source may not end in one. */
            if (!parser->input.lookahead_last_line_ready) {
                markdown_core_strbuf_set(&parser->input.lookahead_last_line, content, content_length);
                markdown_core_strbuf_putc(&parser->input.lookahead_last_line, '\n');
                if (parser->input.lookahead_last_line.oom) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    return 0;
                }
                parser->input.lookahead_last_line_ready = true;
            }
            input.data = parser->input.lookahead_last_line.ptr;
            input.len = parser->input.lookahead_last_line.size;
        } else {
            input.data = (unsigned char *)markdown_core_parser_line_bytes(parser, geometry);
            if (!input.data) {
                return 0;
            }
            input.len = (bufsize_t)(next - geometry->start);
        }
        input.alloc = 0;
        lookahead->line = this_line + 1;

        parser->input.offset = 0;
        parser->input.column = 0;
        parser->input.first_nonspace = 0;
        parser->input.first_nonspace_column = 0;
        parser->input.indent = 0;
        parser->input.blank = false;
        parser->input.partially_consumed_tab = false;

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
                parser->input.offset = entry->offset;
                parser->input.column = entry->column;
                parser->input.partially_consumed_tab = entry->partially_consumed_tab;
                parser->input.first_nonspace = parser->input.offset;
                parser->input.first_nonspace_column = parser->input.column;
            }
        }
        resumed_offset = parser->input.offset;
        for (i = from; i < lookahead->depth && carried && !taken; i++) {
            markdown_core_frame *container = parser->lookahead_chain[i];
            markdown_core_block_find_first_nonspace(parser, &input);
            const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, container->node);
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
        parser->block_lookahead_work += (size_t)(parser->input.offset - resumed_offset);
        if (!carried || closing) {
            S_lookahead_close_run(lookahead, this_line);
            lookahead->active = false;
            return 0;
        }
        if (!taken) {
            markdown_core_block_find_first_nonspace(parser, &input);
        }
        blank = taken || parser->input.blank;

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
            markdown_core_parser_visited_line(parser, this_line)->own = (uint32_t)parser->input.offset;
            entry->container = lookahead->parent;
            entry->depth = lookahead->depth - 1;
            entry->offset = parser->input.offset;
            entry->column = parser->input.column;
            entry->partially_consumed_tab = parser->input.partially_consumed_tab;
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
        *first_nonspace = parser->input.first_nonspace;
        *indent = parser->input.indent;
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
        markdown_core_node *node = parser->lookahead_chain[i]->node;
        const markdown_core_element_instance *structure = markdown_core_parser_structure(parser, node);
        unsigned mask = structure ? structure->element->speculative_flags : 0;
        node->flags =
            (markdown_core_node_internal_flags)((node->flags & ~mask) | (parser->lookahead_chain_flags[i] & mask));
    }
    parser->input.offset = lookahead->saved_offset;
    parser->input.column = lookahead->saved_column;
    parser->input.first_nonspace = lookahead->saved_first_nonspace;
    parser->input.first_nonspace_column = lookahead->saved_first_nonspace_column;
    parser->input.indent = lookahead->saved_indent;
    parser->input.blank = lookahead->saved_blank;
    parser->input.partially_consumed_tab = lookahead->saved_partially_consumed_tab;
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
bool markdown_core_block_start_refuses(block_start_context *context, bool paragraph, bool lazy) {
    bool refuses = (paragraph && context->paragraph) || (lazy && context->lazy);
    context->refused |= refuses;
    return refuses;
}

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

bool markdown_core_parser_has_block_start(markdown_core_parser *parser, markdown_core_frame *parent,
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

static void open_new_blocks(markdown_core_parser *parser, markdown_core_frame **container, markdown_core_chunk *input,
                            bool all_matched) {
    /* Two facts keep a line from interrupting text: `paragraph`, the matched
     * container is a paragraph the line would continue, and `maybe_lazy`, the
     * current block would take the line lazily. Only the first turn has
     * either: a block opened here holds the rest of the line. */
    bool maybe_lazy = S_may_be_lazy(parser, *container);
    size_t depth = 0;
    const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, (*container)->node);
    /* The instance's dialect is sealed: read its families through one local. */
    const markdown_core_dialect *const dialect = parser->dialect;

    while (!S_kind_accepts_lines(kind, (*container)->node)) {
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
                                       .first = parser->input.first_nonspace,
                                       .column = parser->input.first_nonspace_column,
                                       .indent = parser->input.indent,
                                       .paragraph = paragraph,
                                       .lazy = maybe_lazy,
                                       .all_matched = all_matched,
                                       .depth = depth,
                                       .thematic_kill = parser->input.thematic_break_kill_pos};
        scan_element_start(parser, &context, &start);
        if (parser->error || parser->input.taken) {
            return;
        }
        parser->input.thematic_break_kill_pos = context.thematic_kill;

        /* Dash-led tables precede thematic breaks and lists. An opener may
         * close the old path before an allocation fails; OOM is terminal,
         * never a grammar miss that can try another owner on that path. */
        const markdown_core_element_instance *const *interrupters =
            dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT];
        size_t interrupter_count;
        const uint8_t *interrupter_candidates = S_gate_candidates(
            dialect, MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT,
            S_gate_key(input, parser->input.first_nonspace, parser->input.indent), &interrupter_count);
        for (size_t element_index = 0; element_index < interrupter_count; element_index++) {
            const markdown_core_element_instance *owner =
                interrupters[interrupter_candidates ? interrupter_candidates[element_index] : element_index];
            markdown_core_frame *opened = owner->element->try_interrupting_block(owner, parser, &context);
            if (parser->error || parser->input.taken) {
                return;
            }
            if (opened) {
                *container = opened;
                return;
            }
        }
        /* The line is read in the context of the block it would continue
         * when a start that would open after a closed block was refused
         * (parser.h, `line_context`). An interrupter that opened came first,
         * so nothing it preceded was the line's reading. */
        parser->input.line_context |= context.refused;

        if (start.open) {
            if (!start.open(start.owner, parser, container, input, &start)) {
                return;
            }
        } else {
            markdown_core_frame *new_container = NULL;
            const markdown_core_element_instance *const *openers = dialect->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_OPEN];
            size_t opener_count;
            /* An opener is asked about an indented line too, and told so: its
             * key is the byte alone. */
            const uint8_t *opener_candidates =
                S_gate_candidates(dialect, MARKDOWN_CORE_BLOCK_HOOK_OPEN,
                                  S_gate_key(input, parser->input.first_nonspace, 0), &opener_count);

            for (size_t element_index = 0; element_index < opener_count; element_index++) {
                const markdown_core_element_instance *opener =
                    openers[opener_candidates ? opener_candidates[element_index] : element_index];

                new_container = opener->element->try_opening_block(
                    opener, parser->input.indent > opener->element->maximum_block_indent, parser, *container,
                    input->data, input->len);
                if (parser->error || parser->input.taken) {
                    return;
                }

                if (new_container) {
                    *container = new_container;
                    if (parser->input.claimed) {
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
                            opener, parser->input.indent > opener->element->maximum_block_indent, parser, *container,
                            input->data, input->len);
                        if (parser->error || parser->input.taken) {
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
        if (parser->input.taken) {
            return;
        }

        kind = markdown_core_parser_kind(parser, (*container)->node);
        if (S_kind_takes_text(kind, (*container)->node)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        maybe_lazy = false;
    }
}

/* The line in hand is a plain line of the current leaf (E5): where its
 * content begins, as markdown_core_block_add_line reads it. */
static void S_hold_plain(markdown_core_parser *parser) {
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, parser->input.line_number);
    /* A line with NUL is read through a normalized view of it, which its
     * record does not give; a line a decision read the leaf for is read
     * again. */
    if (parser->input.read_leaf || (geometry->facts && parser->input.facts[geometry->facts - 1].nul_count)) {
        return;
    }
    parser->input.plain =
        (markdown_core_line){.own = geometry->own,
                             .offset = (uint32_t)parser->input.offset,
                             .column = parser->input.column,
                             .indent = parser->input.indent,
                             .flags = MARKDOWN_CORE_LINE_PLAIN | (parser->input.blank ? MARKDOWN_CORE_LINE_BLANK : 0) |
                                      (parser->input.partially_consumed_tab ? MARKDOWN_CORE_LINE_TAB : 0)};
}

static void add_text_to_container(markdown_core_parser *parser, markdown_core_frame *container,
                                  markdown_core_chunk *input) {
    markdown_core_frame *const last_matched_container = parser->input.matched_container;
    markdown_core_frame *tmp;
    // what remains at parser->input.offset is a text line.  add the text to the
    // appropriate container.

    markdown_core_block_find_first_nonspace(parser, input);

    if (parser->input.blank && container->last) {
        S_set_last_line_blank(container->last->node, true);
        if (!(container->last->node->flags & MARKDOWN_CORE_NODE__OPEN)) {
            S_record(parser, container->last, MARKDOWN_CORE_NODE__TRAILED);
        }
    }

    /* A line the current block continues with every prefix matched, having
     * opened nothing, is a plain line of it (E5). */
    const bool continues = container == last_matched_container && parser->input.current == container;

    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    const markdown_core_kind_record *kind = markdown_core_parser_kind(parser, container->node);
    const markdown_core_element_instance *structure = kind->structure;
    const bool accepts_lines = S_kind_accepts_lines(kind, container->node);
    const bool last_line_blank =
        parser->input.blank && !(kind->flags & MARKDOWN_CORE_KIND_BLANK_OPAQUE) &&
        (!(kind->flags & MARKDOWN_CORE_KIND_BLANK_ASK) ? !accepts_lines
                                                       : structure->element->blank_line(structure, parser, container));

    S_set_last_line_blank(container->node, last_line_blank);

    tmp = container;
    while (tmp != parser->input.block_root && tmp->owner) {
        S_set_last_line_blank(tmp->owner->node, false);
        tmp = tmp->owner;
    }

    // A line that may be lazy, opened no block and is not blank is a lazy
    // line: the current block takes it.
    if (container == last_matched_container && !parser->input.blank && S_may_be_lazy(parser, last_matched_container)) {
        const markdown_core_element_instance *current =
            markdown_core_parser_structure(parser, parser->input.current->node);
        markdown_core_frame *lazy = current->element->open_lazy(current, parser, parser->input.current, input);
        if (!lazy) {
            return;
        }
        parser->input.current = lazy;
        /* A lazy line is text, so its indentation is not content, as it is not
         * on a line that continues a paragraph with every prefix. */
        markdown_core_block_advance_offset(parser, input, parser->input.first_nonspace - parser->input.offset, false);
        markdown_core_block_add_line(parser->input.current->node, input, parser);
    } else { // not a lazy continuation
        // Finalize any blocks that were not matched and set cur to container:
        markdown_core_parser_finalize_unmatched_blocks(parser);

        if (accepts_lines) {
            if (continues) {
                S_hold_plain(parser);
            }
            markdown_core_block_add_line(container->node, input, parser);
            if (structure->element->ends_block && structure->element->ends_block(structure, parser, container, input)) {
                container->node->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
                container = markdown_core_block_finalize(parser, container);
            }
        } else if (parser->input.blank) {
            // ??? do nothing
        } else if (kind->flags & MARKDOWN_CORE_KIND_PROSE) {
            markdown_core_block_advance_offset(parser, input, parser->input.first_nonspace - parser->input.offset,
                                               false);
            if (continues) {
                S_hold_plain(parser);
            }
            markdown_core_block_add_line(container->node, input, parser);
        } else {
            const markdown_core_element_instance *text_block = parser->dialect->text_block_structure;
            container = text_block->element->open_text_block(text_block, parser, container, input);
            if (!container) {
                return;
            }
            markdown_core_block_add_line(container->node, input, parser);
        }

        parser->input.current = container;
    }
}

/* The line in hand, `bytes` bytes at `buffer`, as the grammar reads it:
 * the content, an LF and a NUL. */
static bool S_hold_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes,
                        markdown_core_chunk *input) {
    assert(parser->input.curline.size == 0);
    assert(bytes >= 0);
    /* The shared input view excludes its physical terminator. Construct the
     * mutable grammar line (content + LF + NUL) with one reservation, rather
     * than appending content and then rediscovering/adding its terminator. */
    if (bytes >= INT32_MAX / 2) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    if (bytes + 1 >= parser->input.curline.asize) {
        markdown_core_strbuf_grow(&parser->input.curline, bytes + 1);
    }

    if (parser->input.curline.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    memcpy(parser->input.curline.ptr, buffer, (size_t)bytes);
    parser->input.curline.ptr[bytes] = '\n';
    parser->input.curline.ptr[bytes + 1] = 0;
    parser->input.curline.size = bytes + 1;
    *input = (markdown_core_chunk){parser->input.curline.ptr, parser->input.curline.size, 0};
    return true;
}

/* The index entry of input line `index`, a line a leaf takes (E5), which
 * starts at `start` as its record `line` says: the one a lookahead read
 * already, or the one the record gives, with no byte of it scanned. NULL,
 * with the parse failed, when the index could not grow. */
static markdown_core_input_line *S_index_taken_line(markdown_core_parser *parser, size_t index, uint32_t start,
                                                    const markdown_core_line *line) {
    if (index < parser->input.line_count) {
        return &parser->input.lines[index];
    }
    assert(index == parser->input.line_count && parser->input.scanned == start);
    if (parser->input.line_count == parser->input.line_capacity) {
        void *lines = markdown_core_reserve(parser->input.lines, &parser->input.line_capacity,
                                            parser->input.line_count + 1, sizeof(*parser->input.lines));
        if (!lines) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return NULL;
        }
        parser->input.lines = lines;
    }
    parser->input.scanned = start + line->span;
    parser->input_line_work += line->span;
    markdown_core_input_line *entry = &parser->input.lines[parser->input.line_count++];
    *entry = (markdown_core_input_line){.start = start, .end = start + line->text};
    return entry;
}

/* THE UNTOUCHED LINES OF A LEAF (E5). On its second line, with every prefix
 * matched, a leaf that reads its old node again, which it entered as the old
 * one did and opened on the same untouched line, takes the old node's plain
 * lines from there that no edit meets through their reach, up to the last
 * that is not blank. The line machine would read each of them as the old
 * parse did, so each is added to the leaf as it was then, with no decision
 * made again, and the parse goes on after the last. True when it took one;
 * the line in hand is then the last it took. */
static bool S_take_lines(markdown_core_parser *parser, markdown_core_chunk *input) {
    markdown_core_frame *leaf = parser->input.current;
    const markdown_core_lines *was = leaf->scan->lines, *lines = leaf->node->lines;
    if (!was || !lines || !leaf->scan_equal || was->broken || lines->broken || lines->count != 1 || was->count < 2 ||
        was->lead != lines->lead) {
        return false;
    }
    const int32_t lead = lines->lead;
    uint32_t at = (uint32_t)((int64_t)leaf->scan_start + lead);
    if (markdown_core_parser_touched(parser, at, at + was->items[0].span + was->items[0].reach)) {
        return false;
    }
    at += was->items[0].span;
    uint32_t count = 0;
    for (uint32_t i = 1, from = at; i < was->count; i++) {
        const markdown_core_line *line = &was->items[i];
        if (!(line->flags & MARKDOWN_CORE_LINE_PLAIN) ||
            markdown_core_parser_touched(parser, from, from + line->span + line->reach)) {
            break;
        }
        from += line->span;
        if (!(line->flags & MARKDOWN_CORE_LINE_BLANK)) {
            count = i;
        }
    }
    if (!count) {
        return false;
    }
    uint32_t reach = parser->input.line_reach;
    for (uint32_t i = 1; i <= count && !parser->error; i++) {
        const markdown_core_line *line = &was->items[i];
        if (i > 1) {
            markdown_core_strbuf_clear(&parser->input.curline);
            markdown_core_input_line *found = S_index_taken_line(
                parser, (size_t)(parser->input.line_number + 1 - parser->input.first_line), at, line);
            bufsize_t length = 0;
            const unsigned char *content = found ? S_input_line_content(parser, found, &length) : NULL;
            if (!content || !S_hold_line(parser, content, length, input)) {
                break;
            }
            parser->input.line_number++;
            parser->input.line_start = (bufsize_t)found->start;
            parser->input.line_end = (bufsize_t)found->end;
        }
        markdown_core_parser_visited_line(parser, parser->input.line_number)->own = line->own;
        parser->input.offset = (bufsize_t)line->offset;
        parser->input.column = line->column;
        parser->input.indent = line->indent;
        parser->input.partially_consumed_tab = (line->flags & MARKDOWN_CORE_LINE_TAB) != 0;
        parser->input.blank = (line->flags & MARKDOWN_CORE_LINE_BLANK) != 0;
        markdown_core_block_add_line(leaf->node, input, parser);
        at = (uint32_t)parser->input.line_start + line->span;
        reach = reach > at + line->reach ? reach : at + line->reach;
    }
    S_lines_append(parser, leaf->node, lead, was->items + 1, count);
    parser->input.line_reach = reach;
    S_after_text(parser);
    parser->input.lines_taken = true;
    return true;
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes) {
    markdown_core_frame *last_matched_container;
    bool all_matched = true;
    markdown_core_frame *container;
    markdown_core_chunk input;

    if (parser->error || parser->root == NULL) {
        return;
    }

    if (!S_hold_line(parser, buffer, bytes, &input)) {
        return;
    }

    parser->input.offset = 0;
    parser->input.column = 0;
    parser->input.first_nonspace = 0;
    parser->input.first_nonspace_column = 0;
    parser->input.thematic_break_kill_pos = 0;
    parser->input.indent = 0;
    parser->input.previous_blank = parser->input.blank;
    parser->input.line_context = false;
    parser->input.blank = false;
    parser->input.partially_consumed_tab = false;

    parser->input.line_number++;

    /* No block has matched the line yet. */
    parser->input.matched_container = NULL;
    last_matched_container = check_open_blocks(parser, &input, &all_matched);

    if (!last_matched_container) {
        goto finished;
    }
    if (all_matched && last_matched_container == parser->input.current && parser->input.current->scan &&
        S_take_lines(parser, &input)) {
        goto finished;
    }

    container = last_matched_container;
    parser->input.matched_container = last_matched_container;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->error || parser->input.taken) {
        goto finished;
    }

    if (parser->input.claimed) {
        parser->input.current = container->node->flags & MARKDOWN_CORE_NODE__OPEN ? container : container->owner;
        goto finished;
    }

    add_text_to_container(parser, container, &input);

finished:
    /* Block scopes cover the complete physical line, including closing
     * delimiters and attribute containers. Inline content trimming never
     * changes this source boundary. */
    parser->input.last_line_end = parser->input.line_end;

    markdown_core_strbuf_clear(&parser->input.curline);
}

bool markdown_core_parser_normalize_label(markdown_core_parser *parser, const markdown_core_chunk *label,
                                          markdown_core_chunk *normalized) {
    *normalized = (markdown_core_chunk){0};
    bool failed = false;
    const markdown_core_strbuf *read = markdown_core_registry_normalize(parser->registry, label, &failed);
    if (!read) {
        return !failed;
    }
    unsigned char *stored = markdown_core_node_pool_bytes(parser->pool, (size_t)read->size + 1);
    if (!stored) {
        return false;
    }
    memcpy(stored, read->ptr, (size_t)read->size);
    stored[read->size] = '\0';
    *normalized = (markdown_core_chunk){stored, read->size, 0};
    return true;
}

uint64_t markdown_core_source_key(const void *entry) { return ((const markdown_core_source_entry *)entry)->start; }

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

/* `markdown_core_node_check` is the one structural self-check this tree has.
 * It checks every stem of the tree and that every node in it is held, after
 * every inline root has completed; when it cannot allocate its work stack the
 * parse fails as any allocation failure does. This is compiled in only when
 * `MARKDOWN_CORE_DEBUG_NODES` is defined, which no shipping configuration
 * defines. */
#if MARKDOWN_CORE_DEBUG_NODES
#define MARKDOWN_CORE_CHECK_TREE(parser, root)                                                                         \
    do {                                                                                                               \
        int faults = markdown_core_node_check((root), stderr);                                                         \
        if (faults < 0) {                                                                                              \
            markdown_core_parser_fail((parser), MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);                                \
        } else if (faults) {                                                                                           \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)
#else
#define MARKDOWN_CORE_CHECK_TREE(parser, root) ((void)0)
#endif

/* THE PARSE ENDS AS ITS NODES COMPLETE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.8). Every block completed as it closed, its inline roots
 * and the cells it reads as blocks with it, the document last. The old
 * nodes the parse did not take leave the registry, resolution parses again
 * each root whose answers changed (5.7), the document is finished, and it is
 * published. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) markdown_core_node *S_finish_parse(markdown_core_parser *parser) {
    markdown_core_node *res;

    if (parser->root == NULL || parser->error) {
        return NULL;
    }

    finalize_document(parser);
    /* The block stage is over: the old nodes the parse did not take are no
     * longer the document's, and their facts leave the registry (5.7) before
     * the inline stage reads it, but for the content of their inline roots,
     * whose nodes the inline stage may take. The nodes go when their tree is
     * released. */
    if (!parser->error && parser->revision->previous &&
        !markdown_core_registry_retire(parser->registry, parser->revision->previous, &parser->old_content)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    const markdown_core_element_instance *document = parser->dialect->document_structure;
    if (!parser->error) {
        document->element->prepare_document(document, parser);
    }
    if (!parser->error) {
        MARKDOWN_CORE_CHECK_TREE(parser, parser->root->node);
    }
    if (!parser->error) {
        document->element->finish_document(document, parser);
    }
    /* Last, the finished tree is published: nothing changes it after this. */
    if (!parser->error) {
        document->element->publish_document(document, parser);
    }
    markdown_core_free(parser->walk_stack);
    parser->walk_stack = NULL;
    parser->walk_stack_size = 0;
    markdown_core_free(parser->complete_states);
    parser->complete_states = NULL;
    parser->complete_capacity = 0;
    if (parser->error) {
        goto failed;
    }

    /* The caller takes the document's reference from its frame. */
    res = parser->root->node;
    parser->root->held = false;
    markdown_core_parser_release_frame(parser, parser->root);
    parser->root = NULL;
    return res;

failed:
    document->element->dispose_document(document, parser);
    markdown_core_parser_release_frame(parser, parser->root);
    parser->root = NULL;
    return NULL;
}

int markdown_core_parser_get_line_number(markdown_core_parser *parser) { return parser->input.line_number; }

bufsize_t markdown_core_parser_get_offset(markdown_core_parser *parser) { return parser->input.offset; }

bufsize_t markdown_core_parser_get_column(markdown_core_parser *parser) { return parser->input.column; }

int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser) { return parser->input.first_nonspace; }

int markdown_core_parser_get_first_nonspace_column(markdown_core_parser *parser) {
    return parser->input.first_nonspace_column;
}

int markdown_core_parser_get_indent(markdown_core_parser *parser) { return parser->input.indent; }

int markdown_core_parser_is_blank(markdown_core_parser *parser) { return parser->input.blank; }

int markdown_core_parser_has_partially_consumed_tab(markdown_core_parser *parser) {
    return parser->input.partially_consumed_tab;
}

bufsize_t markdown_core_parser_get_last_line_end(markdown_core_parser *parser) { return parser->input.last_line_end; }

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    markdown_core_block_advance_offset(parser, &input_chunk, count, columns != 0);
}
