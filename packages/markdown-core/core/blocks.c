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
#include "../elements/markdown-core-elements.h"
#include "config.h"
#include "parser.h"
#include "markdown-core.h"
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

bool markdown_core_block_last_line_blank(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0;
}

static bool S_last_line_checked(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_CHECKED) != 0;
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

static void S_set_last_line_checked(markdown_core_node *node) { node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_CHECKED; }

bool markdown_core_block_is_space_or_tab(char c) { return (c == ' ' || c == '\t'); }

static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length);
static markdown_core_node *S_finish_parse(markdown_core_parser *parser);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);

static markdown_core_node *make_block(markdown_core_parser *parser, markdown_core_node_type tag, int start_line,
                                      int start_column, const markdown_core_element *element) {
    markdown_core_node *e;

    e = markdown_core_node_create(parser->arena, parser->mem, tag, element);
    if (!e) {
        return NULL;
    }
    /* Content storage grows with the first line fed to the block; a container
     * that never receives one keeps the shared empty buffer. */
    e->flags = MARKDOWN_CORE_NODE__OPEN;
    e->start_line = start_line;
    e->start_column = start_column;
    e->end_line = start_line;

    return e;
}

/* Create the root document node: made in the transaction's arena, it is the
 * document that owns the arena. */
static markdown_core_node *make_document(markdown_core_parser *parser) {
    return make_block(parser, MARKDOWN_CORE_NODE_DOCUMENT, 1, 1, NULL);
}

/* The registry a parser dispatches on, with the mirrors its phases walk. */
static void S_install_registry(markdown_core_parser *parser, const markdown_core_registry *registry) {
    parser->registry = registry;
    parser->elements = registry->elements;
    parser->element_count = registry->element_count;
}

/* Setup extends the same registry read by every phase. The fixed dialect and
 * its prepared projection are borrowed; an extension gets its own projection,
 * built in one allocation and published with the registry it projects, so an
 * allocation failure leaves the previous registry, its projection and the
 * dispatch it drives all intact. */
int markdown_core_parser_attach_element(markdown_core_parser *parser, const markdown_core_element *element) {
    markdown_core_registry extended;
    if (!markdown_core_registry_prepare(parser->mem, parser->elements, parser->element_count, element, &extended)) {
        return 0;
    }
    if (parser->registry == &parser->owned_registry) {
        markdown_core_registry_release(parser->mem, &parser->owned_registry);
    }
    parser->owned_registry = extended;
    S_install_registry(parser, &parser->owned_registry);
    return 1;
}

static unsigned char S_block_start_byte(const markdown_core_chunk *input, int first) {
    return first < input->len ? input->data[first] : 0;
}

static void S_parser_dispose(markdown_core_parser *parser) {
    for (size_t element_index = 0; element_index < parser->element_count; element_index++) {
        const markdown_core_element *structure = parser->elements[element_index];
        if (structure->dispose_parser) {
            structure->dispose_parser(parser);
        }
    }
    if (parser->document_structure) {
        parser->document_structure->dispose_document(parser);
    }
    markdown_core_mem_release(parser->mem, parser->block_inputs);
    markdown_core_mem_release(parser->mem, parser->input_line_offsets);
    if (parser->root) {
        /* The root owns the arena from its creation on. */
        parser->arena = NULL;
        markdown_core_node_free(parser->root);
    }

    /* The content-to-source map outlives every block that indexes it and
     * nothing else does, so it is released here rather than with the node. */
    markdown_core_mem_release(parser->mem, parser->line_marks);
    parser->line_marks = NULL;
    parser->line_marks_size = 0;
    parser->line_marks_alloc = 0;

    /* The block-start lookahead's chain and resume cache are parser state of
     * the same kind: indexed by open containers and source lines, owned by no
     * node, and dead with the parse. */
    markdown_core_mem_release(parser->mem, parser->lookahead_chain);
    markdown_core_mem_release(parser->mem, parser->lookahead_chain_flags);
    markdown_core_mem_release(parser->mem, parser->lookahead_entries);
    parser->lookahead_chain = NULL;
    parser->lookahead_chain_flags = NULL;
    parser->lookahead_chain_alloc = 0;
    parser->lookahead_entries = NULL;
    parser->lookahead_entries_alloc = 0;
}

static markdown_core_parser *S_parser_new(markdown_core_mem *mem) {
    markdown_core_parser *parser;
    markdown_core_node *document;

    if (!mem) {
        return NULL;
    }
    parser = (markdown_core_parser *)mem->calloc(1, sizeof(*parser));
    if (!parser) {
        return NULL;
    }
    parser->mem = mem;
    parser->arena = markdown_core_arena_new(mem);
    if (!parser->arena) {
        mem->free(parser);
        return NULL;
    }
    markdown_core_strbuf_init(parser->mem, &parser->curline, 256);
    markdown_core_strbuf_init(parser->mem, &parser->paragraph_line_copy, 0);
    markdown_core_strbuf_init(parser->mem, &parser->line_scratch, 0);
    markdown_core_strbuf_init(parser->mem, &parser->lookahead_last_line, 0);
    markdown_core_strbuf_init(parser->mem, &parser->url_scratch, 0);

    document = make_document(parser);
    parser->document_structure = markdown_core_structure_for_kind(MARKDOWN_CORE_NODE_DOCUMENT);
    parser->document_structure->init_document(parser);
    parser->root = document;
    parser->block_root = document;
    parser->current = document;

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. */
    if (!parser->root || parser->curline.oom || parser->line_scratch.oom || parser->lookahead_last_line.oom ||
        parser->root->content->oom) {
        parser->oom = true;
    }

    return parser;
}

static void S_parser_free(markdown_core_parser *parser) {
    markdown_core_mem *mem;
    if (!parser) {
        return;
    }
    mem = parser->mem;
    S_parser_dispose(parser);
    /* Only a parser whose root never existed still holds the arena here. */
    markdown_core_arena_free(parser->arena);
    parser->arena = NULL;
    if (parser->registry == &parser->owned_registry) {
        markdown_core_registry_release(mem, &parser->owned_registry);
    }
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->paragraph_line_copy);
    markdown_core_strbuf_free(&parser->line_scratch);
    markdown_core_strbuf_free(&parser->lookahead_last_line);
    markdown_core_strbuf_free(&parser->url_scratch);
    markdown_core_mem_release(mem, parser->completions);
    mem->free(parser);
}

/* "This block ends on the line being processed", lifted out of `markdown_core_block_finalize` so
 * that the element close path can say the same thing. The three kinds that
 * take it there — the document, a closed fenced code block, a setext heading —
 * are the ones whose last line IS the line in hand; every other block ended on
 * the line before. An element container closing on its own fence is a fourth,
 * and `markdown_core_block_finalize` cannot know that from the type alone. */
void markdown_core_block_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b) {
    b->end_line = parser->line_number;
    b->end_column = parser->curline.size;
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\n') {
        b->end_column -= 1;
    }
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\r') {
        b->end_column -= 1;
    }
    b->end_column = markdown_core_parser_source_column(parser, b->end_line, b->end_column);
}

// Returns true if line has only space characters, else false.
bool markdown_core_block_is_blank(markdown_core_strbuf *s, bufsize_t offset) {
    while (offset < s->size) {
        switch (s->ptr[offset]) {
        case '\r':
        case '\n':
            return true;
        case ' ':
            offset++;
            break;
        case '\t':
            offset++;
            break;
        default:
            return false;
        }
    }

    return true;
}

/* The per-line predicates read the kind's traits (markdown_core_node_traits);
 * only a kind whose structure decides per node reaches the structure. */
static bool element_accepts_lines(markdown_core_node *node) {
    unsigned traits = markdown_core_node_traits(node);
    if (traits & MARKDOWN_CORE_TRAIT_LITERAL) {
        return true;
    }
    if (!(traits & MARKDOWN_CORE_TRAIT_LINES_FUNC)) {
        return false;
    }
    const markdown_core_element *structure = markdown_core_node_structure(node);
    return structure->accepts_lines_func(structure, node);
}
bool markdown_core_block_accepts_lines(markdown_core_node *node) {
    return element_accepts_lines(node) || (markdown_core_node_traits(node) & MARKDOWN_CORE_TRAIT_PROSE);
}
static bool contains_inlines(markdown_core_node *node) {
    unsigned traits = markdown_core_node_traits(node);
    if (traits & MARKDOWN_CORE_TRAIT_INLINE_CONTENT) {
        return true;
    }
    if (!(traits & MARKDOWN_CORE_TRAIT_INLINES_FUNC)) {
        return false;
    }
    const markdown_core_element *structure = markdown_core_node_structure(node);
    return structure->contains_inlines_func(structure, node);
}
static bool is_paragraph(markdown_core_node *node) {
    return (markdown_core_node_traits(node) & MARKDOWN_CORE_TRAIT_PARAGRAPH) != 0;
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
        parser->oom = true;
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
        parser->oom = true;
        return false;
    }
    markdown_core_line_mark *grown = parser->mem->realloc(parser->line_marks, (size_t)capacity * sizeof(*grown));
    if (!grown) {
        parser->oom = true;
        return false;
    }
    parser->line_marks = grown;
    parser->line_marks_alloc = capacity;
    return true;
}

static int S_append_content_mark(markdown_core_parser *parser, markdown_core_node *node, markdown_core_line_mark mark) {
    if (!parser || !node) {
        return 0;
    }
    mark.content_offset += node->content_mark_offset;
    if (node->content_mark_count && node->content_mark + node->content_mark_count == parser->line_marks_size &&
        parser->line_marks[parser->line_marks_size - 1].content_offset == mark.content_offset) {
        parser->line_marks[parser->line_marks_size - 1] = mark;
        return 1;
    }
    if (!S_reserve_content_marks(parser, 1)) {
        return 0;
    }
    if (node->content_mark_count == 0) {
        node->content_mark = parser->line_marks_size;
    } else {
        assert(node->content_mark + node->content_mark_count == parser->line_marks_size);
        assert(parser->line_marks[parser->line_marks_size - 1].content_offset < mark.content_offset);
    }
    assert(mark.source_width >= 1);
    parser->line_marks[parser->line_marks_size++] = mark;
    node->content_mark_count++;
    return 1;
}

int markdown_core_parser_append_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                             int line, int column, int source_width, int source_step) {
    return S_append_content_mark(parser, node,
                                 (markdown_core_line_mark){offset, line, column, source_width, source_step, 0});
}

static void S_record_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t column,
                                  bufsize_t length) {
    markdown_core_parser_append_source_marks(parser, node, parser->line_number, column, length, node->content->size);
}

/* A block's content lives in the transaction's arena: its first line
 * reserves the bytes it brings, and each later line extends that reservation
 * in place, which the arena can do while the block is its latest allocation
 * -- true between the lines of one block, since nothing else is created
 * there. Only a reservation the arena cannot extend moves to the allocator,
 * by the buffer's ordinary growth, and a node outside a transaction keeps an
 * allocator buffer. A failed reservation costs nothing either: the buffer
 * grows as it always did. */
static void S_reserve_content(markdown_core_parser *parser, markdown_core_node *node, bufsize_t add) {
    markdown_core_strbuf *content = node->content;
    if (!node->arena_owned || !parser->arena || add <= 0 || content->oom ||
        add > (bufsize_t)(INT32_MAX / 2) - content->size || content->asize - content->size > add) {
        return;
    }
    size_t needed = (size_t)content->size + (size_t)add + 1;
    if (content->asize == 0) {
        unsigned char *storage = markdown_core_arena_alloc(parser->arena, needed);
        if (storage) {
            markdown_core_strbuf_borrow(content, storage, (bufsize_t)needed);
        }
    } else if (content->borrowed &&
               markdown_core_arena_extend(parser->arena, content->ptr, (size_t)content->asize, needed)) {
        content->asize = (bufsize_t)needed;
    }
}

/* The literal a finalized block's content becomes. Content the arena holds
 * stays there, borrowed: the literal, the buffer and the node go with the
 * arena together, and the block's bytes are copied nowhere. Content the arena
 * could not hold, or a node's outside a transaction, is taken over as it is;
 * a buffer that lost bytes reports the loss as an empty literal with NULL
 * data, as markdown_core_chunk_buf_detach does. */
markdown_core_chunk markdown_core_block_take_literal(markdown_core_node *b) {
    markdown_core_strbuf *content = b->content;
    if (b->arena_owned && content->borrowed && !content->oom) {
        markdown_core_chunk literal = {content->ptr, content->size, 0};
        return literal;
    }
    return markdown_core_chunk_buf_detach(content);
}

void markdown_core_block_add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
    if (parser->offset < ch->len) {
        bufsize_t tab = parser->partially_consumed_tab ? TAB_STOP - (parser->column % TAB_STOP) : 0;
        S_reserve_content(parser, node, ch->len - parser->offset + tab);
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
            markdown_core_strbuf_putc(node->content, ' ');
        }
    }
    /* A line consumed to its end (a fence line whose info string was read
     * at the fence) contributes no bytes and therefore no run. */
    if (parser->offset >= ch->len) {
        return;
    }
    S_record_content_mark(parser, node, parser->offset + 1, ch->len - parser->offset);
    markdown_core_strbuf_put(node->content, ch->data + parser->offset, ch->len - parser->offset);
    if (node->content->oom) {
        parser->oom = true;
    }
}

/* Seed a map for one unchanged source line assembled by a producer rather
 * than fed through add_line. Transformed content appends its own source runs;
 * slices share their owner's runs through adopt_content_marks. A failed map
 * allocation loses the parse transaction, just like a failed content buffer. */
int markdown_core_parser_mark_content(markdown_core_parser *parser, markdown_core_node *node, int line, int column) {
    if (!parser || !node) {
        return 0;
    }
    node->content_mark_count = 0;
    node->content_mark_offset = 0;
    return markdown_core_parser_append_content_mark(parser, node, 0, line, column, 1, 1);
}

/* A forward cursor visits each immutable run at most once. Revisited source
 * ranges use binary search without moving that cursor backwards. Indexes, not
 * pointers, survive growth of the parser-owned mark vector. */
static int content_mark_at(markdown_core_parser *parser, const markdown_core_node *node, bufsize_t offset,
                           int *cursor) {
    int lo = node->content_mark, hi = lo + node->content_mark_count - 1;
    MARKDOWN_CORE_DIAGNOSTIC(size_t work = 0;)
    if (cursor) {
        if (*cursor < lo || *cursor > hi) {
            *cursor = lo;
        }
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
        if (offset >= parser->line_marks[*cursor].content_offset) {
            MARKDOWN_CORE_DIAGNOSTIC(int first = *cursor;)
            while (*cursor < hi) {
                if (parser->line_marks[*cursor + 1].content_offset > offset) {
                    break;
                }
                ++*cursor;
            }
            /* Current mark, each advance, and the stopping comparison when
             * the cursor has not reached the last mark. */
            MARKDOWN_CORE_DIAGNOSTIC(parser->content_map_work += work + (size_t)(*cursor - first) + (*cursor < hi);)
            return *cursor;
        }
        hi = *cursor > lo ? *cursor - 1 : lo;
    }
    while (lo < hi) {
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
        int mid = lo + (hi - lo + 1) / 2;
        if (parser->line_marks[mid].content_offset <= offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->content_map_work += work;)
    return lo;
}

int markdown_core_block_content_mark_at(markdown_core_parser *parser, const markdown_core_node *node,
                                        bufsize_t offset) {
    return content_mark_at(parser, node, offset, NULL);
}

/* A map slice is a view into parser-owned immutable runs. Neither the source
 * nor the slice owns the vector; both end with the parse transaction. */
int markdown_core_parser_adopt_content_marks(markdown_core_parser *parser, markdown_core_node *owner,
                                             markdown_core_node *node, bufsize_t from, bufsize_t length) {
    if (!parser || !owner || !node || !owner->content_mark_count || length <= 0) {
        return 0;
    }
    from += owner->content_mark_offset;
    int first = markdown_core_block_content_mark_at(parser, owner, from);
    int last = markdown_core_block_content_mark_at(parser, owner, from + length - 1);
    node->content_mark = first;
    node->content_mark_count = last - first + 1;
    node->content_mark_offset = from;
    return 1;
}

int markdown_core_parser_append_content_marks(markdown_core_parser *parser, markdown_core_node *owner,
                                              markdown_core_node *node, bufsize_t from, bufsize_t length,
                                              bufsize_t offset) {
    if (length <= 0) {
        return 1;
    }
    if (!owner->content_mark_count) {
        parser->oom = true;
        return 0;
    }
    from += owner->content_mark_offset;
    int first = markdown_core_block_content_mark_at(parser, owner, from);
    int last = markdown_core_block_content_mark_at(parser, owner, from + length - 1);
    for (int i = first; i <= last; i++) {
        markdown_core_line_mark mark = parser->line_marks[i];
        bufsize_t start = mark.content_offset < from ? from : mark.content_offset;
        mark.column += (start - mark.content_offset) * mark.source_step;
        mark.content_offset = offset + start - from;
        if (!S_append_content_mark(parser, node, mark)) {
            return 0;
        }
    }
    return 1;
}

int markdown_core_parser_source_column(markdown_core_parser *parser, int line, int column) {
    if (column < 0 || parser->block_root == parser->root) {
        return column;
    }
    size_t index = (size_t)(line - parser->input_first_line);
    assert(line >= parser->input_first_line && index < parser->input_line_count);
    int source_line, source_column;
    if (!markdown_core_parser_content_end_place(parser, parser->block_root,
                                                parser->input_line_offsets[index] + (column ? column - 1 : 0),
                                                &source_line, &source_column)) {
        parser->oom = true;
        return column;
    }
    assert(source_line == line);
    return source_column - (column == 0 ? 1 : 0);
}

int markdown_core_parser_append_source_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                             int column, bufsize_t length, bufsize_t offset) {
    if (parser->block_root == parser->root) {
        return S_append_content_mark(parser, node,
                                     (markdown_core_line_mark){offset, line, column, 1, 1, parser->indent});
    }
    size_t index = (size_t)(line - parser->input_first_line);
    assert(line >= parser->input_first_line && index < parser->input_line_count);
    return markdown_core_parser_append_content_marks(parser, parser->block_root, node,
                                                     parser->input_line_offsets[index] + column - 1, length, offset);
}

bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_node *owner) {
    if (!owner->content->size) {
        return true;
    }
    assert(owner->content_mark_count && owner->parent);
    if (parser->block_input_count == parser->block_input_capacity) {
        size_t capacity = parser->block_input_capacity ? 2 * parser->block_input_capacity : 16;
        if (capacity > SIZE_MAX / sizeof(*parser->block_inputs)) {
            parser->oom = true;
            return false;
        }
        void *inputs = parser->mem->realloc(parser->block_inputs, capacity * sizeof(*parser->block_inputs));
        if (!inputs) {
            parser->oom = true;
            return false;
        }
        parser->block_inputs = inputs;
        parser->block_input_capacity = capacity;
    }
    parser->block_inputs[parser->block_input_count++] = owner;
    return true;
}

/* Resolve an endpoint once: its run also identifies the immutable slice a
 * source-backed Text node adopts. A missing map returns -1, never a run index. */
int markdown_core_parser_project_content(markdown_core_parser *parser, const markdown_core_node *node, bufsize_t offset,
                                         bool end, int *cursor, int *line, int *column) {
    if (!parser || !node || node->content_mark_count <= 0 || offset < 0) {
        return -1;
    }
    offset += node->content_mark_offset;
    int index = content_mark_at(parser, node, offset, cursor);
    const markdown_core_line_mark *mark = &parser->line_marks[index];
    *line = mark->line;
    *column =
        mark->column + (int)(offset - mark->content_offset) * mark->source_step + (end ? mark->source_width - 1 : 0);
    return index;
}

int markdown_core_parser_content_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                       int *line, int *column) {
    return markdown_core_parser_project_content(parser, node, offset, false, NULL, line, column) >= 0;
}

int markdown_core_parser_content_end_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                           int *line, int *column) {
    return markdown_core_parser_project_content(parser, node, offset, true, NULL, line, column) >= 0;
}

/* Drop `dropped` bytes off the FRONT of `node`'s content, leaving `remaining`
 * bytes, and keep the map describing what is left. The marks stay where they are in the vector: the
 * run's head moves past the slices that went away, and the slice the cut
 * landed inside keeps its line with its column advanced to the cut. */
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
                                              bufsize_t remaining) {
    if (node->content_mark_count <= 0 || dropped <= 0) {
        return;
    }
    if (remaining <= 0) {
        node->content_mark_count = 0;
        node->content_mark_offset = 0;
        return;
    }
    markdown_core_parser_adopt_content_marks(parser, node, node, dropped, remaining);
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
bool markdown_core_block_ends_with_blank_line(markdown_core_node *node) {
    markdown_core_node *last = node;
    while (!S_last_line_checked(last) &&
           (markdown_core_node_structure(last) && markdown_core_node_structure(last)->propagates_child_blank) &&
           last->last_child) {
        last = last->last_child;
    }
    bool blank = markdown_core_block_last_line_blank(last);
    /* Cache the answer as well as the fact that it was checked. Both list
     * finalization and detached identifiers ask this of finalized blocks. */
    for (;;) {
        S_set_last_line_blank(last, blank);
        S_set_last_line_checked(last);
        if (last == node) {
            return blank;
        }
        last = last->parent;
    }
}

/* A finalized block its element completes once block parsing ends joins the
 * queue in finalization order, which puts every block after the blocks it
 * contains -- the order a post-order walk of the finished tree would visit
 * them in, without the walk. A queued record is never recycled while the
 * queue stands (S_free_nodes), so an entry always finds its own node. */
void markdown_core_block_queue_completion(markdown_core_parser *parser, markdown_core_node *b) {
    if (b->flags & MARKDOWN_CORE_NODE__COMPLETION_QUEUED) {
        return;
    }
    if (parser->completion_count == parser->completion_capacity) {
        size_t capacity = parser->completion_capacity ? 2 * parser->completion_capacity : 64;
        if (capacity > SIZE_MAX / sizeof(*parser->completions)) {
            parser->oom = true;
            return;
        }
        markdown_core_node **grown = parser->mem->realloc(parser->completions, capacity * sizeof(*grown));
        if (!grown) {
            parser->oom = true;
            return;
        }
        parser->completions = grown;
        parser->completion_capacity = capacity;
    }
    b->flags |= MARKDOWN_CORE_NODE__COMPLETION_QUEUED;
    parser->completions[parser->completion_count++] = b;
}

markdown_core_node *markdown_core_block_finalize(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_node *parent;

    parent = b->parent;
    assert(b->flags & MARKDOWN_CORE_NODE__OPEN); // shouldn't call markdown_core_block_finalize on closed blocks
    b->flags &= ~MARKDOWN_CORE_NODE__OPEN;

    if (parser->curline.size == 0) {
        // end of input - line number has not been incremented
        b->end_line = parser->line_number;
        b->end_column = parser->last_line_length;
    } else if (markdown_core_block_type(b) == MARKDOWN_CORE_NODE_DOCUMENT ||
               /* D35: a block finalized on the line it OPENED did not end on
                * the previous one. `line_number - 1` below assumes the block
                * was closed by a later line, which is true of every block that
                * needs a following line to end it -- and false of an HTML block
                * of type 2 to 5, whose terminator can be on its own first line.
                * Measured: `<!-- c -->` alone on line 3 gave
                * `HTMLBlock scope=3:1..2:0` for a literal whose last byte is at
                * 3:10, and `last_line_length` there is the length of the BLANK
                * line before it. Four of the eleven observed negative rows
                * were this. */
               parser->line_number == b->start_line ||
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
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }

    /* The element's one chance to read its own block as a finished thing.
     * Placed after the scope is settled and before the switch, because what a
     * close hook has to say is about the whole block. */

    const markdown_core_element *structure = markdown_core_node_structure(b);
    if (structure && structure->finalize_block) {
        structure->finalize_block(parser, b);
    }
    /* The block's completion, and the discarding of a paragraph reference
     * definitions consumed, wait for block parsing to end: the block keeps
     * its place among its siblings until then (S_complete_blocks). */
    if ((structure && structure->complete_block) || (b->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY)) {
        markdown_core_block_queue_completion(parser, b);
    }

    return parent;
}

/* Finalize to the container that will own the next block-level construct,
 * including a detached identifier, which contributes no child node. */
/* A source-owning block candidate commits only after the prior open path has
 * closed at this line's matched boundary. This is also the ordinary text path's
 * transition; caption attachment therefore cannot strand an open preceding table. */
void markdown_core_parser_finalize_unmatched_blocks(markdown_core_parser *parser) {
    while (parser->current != parser->matched_container && !parser->oom) {
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
        parent = markdown_core_block_finalize(parser, parent);
    }
    return parent;
}

// Add a node as child of another.  Return pointer to child.
/* The shared tail of block creation: `child` is the block just made for
 * `parent` (already the deepest open container that takes `block_type`), or
 * NULL when it could not be made. The constructor call stays at each entry so
 * the engine's own blocks, which carry no element payload, keep a constant-
 * propagated constructor. */
static markdown_core_node *attach_new_block(markdown_core_parser *parser, markdown_core_node *parent,
                                            markdown_core_node *child) {
    if (!child || child->content->oom) {
        parser->oom = true;
        if (child) {
            markdown_core_node_recycle(parser->arena, child);
        }
        /* The loop above may have finalized blocks; keep the parser anchored
         * at a still-open ancestor so the finish path stays consistent. */
        parser->current = parent;
        return NULL;
    }
    if (!markdown_core_node_attach_owned(parent, child, NULL)) {
        markdown_core_node_recycle(parser->arena, child);
        parser->oom = true;
        return NULL;
    }
    return child;
}

markdown_core_node *markdown_core_parser_add_element_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                           markdown_core_node_type block_type, int start_column,
                                                           const markdown_core_element *element) {
    parent = markdown_core_block_parent_for(parser, parent, block_type);
    return attach_new_block(parser, parent,
                            make_block(parser, block_type, parser->line_number,
                                       markdown_core_parser_source_column(parser, parser->line_number, start_column),
                                       element));
}

markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column) {
    parent = markdown_core_block_parent_for(parser, parent, block_type);
    return attach_new_block(parser, parent,
                            make_block(parser, block_type, parser->line_number,
                                       markdown_core_parser_source_column(parser, parser->line_number, start_column),
                                       NULL));
}

/* Project the union of dispatch and terminator ownership once. Flanking
 * transparency stays independent of both. An owner appears at most once per
 * byte, including repeated set members and overlapping roles. */
enum { INLINE_TERMINATES = 1, INLINE_DISPATCHES = 2 };

static size_t inline_candidate_roles(const markdown_core_element *element, unsigned char roles[256],
                                     unsigned char bytes[256]) {
    if (!element->match_inline && !element->insert_inline_from_delim) {
        return 0;
    }
    memset(roles, 0, 256);
    size_t count = 0;
    for (const unsigned char *c = (const unsigned char *)element->terminates_text; c && *c; c++) {
        if (!roles[*c]) {
            bytes[count++] = *c;
        }
        roles[*c] |= INLINE_TERMINATES;
    }
    if (element->match_inline) {
        for (const unsigned char *c = (const unsigned char *)element->dispatch; c && *c; c++) {
            if (!roles[*c]) {
                bytes[count++] = *c;
            }
            roles[*c] |= INLINE_DISPATCHES;
        }
    }
    return count;
}

static bool S_owns_block_starts(const markdown_core_element *element) {
    return element->scan_block_start || element->try_interrupting_block || element->try_opening_block ||
           element->try_opening_paragraph;
}

/* Whether a hook accepts at a byte: a declared set names its bytes, and no
 * set at all means every byte, a line end included. */
static bool S_accepts_block_start_byte(const unsigned char *accepted, size_t byte) {
    if (!accepted) {
        return true;
    }
    for (const unsigned char *b = accepted; *b; b++) {
        if (*b == byte) {
            return true;
        }
    }
    return false;
}

static bool S_implements_block_hook(const markdown_core_element *element, size_t hook) {
    switch (hook) {
    case MARKDOWN_CORE_BLOCK_HOOK_SCAN:
        return element->scan_block_start != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT:
        return element->try_interrupting_block != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_OPEN:
        return element->try_opening_block != NULL;
    default:
        return element->try_opening_paragraph != NULL;
    }
}

/* The bytes one hook accepts at: its own where it narrowed the element's,
 * the element's otherwise. */
static const unsigned char *S_block_hook_bytes(const markdown_core_element *element, size_t hook) {
    const char *narrowed = element->block_start_hook_bytes[hook];
    return (const unsigned char *)(narrowed ? narrowed : element->block_start_bytes);
}

/* A registry's projection is a pure function of its descriptors: the core
 * registry's is generated once from them (core-registry.inc, which the api
 * tests hold to this builder), and this builds the projection of a registry
 * a setup extends. Every table shares one allocation, the pointer-aligned
 * tables first and the byte sets last. Within a byte, inline candidates keep
 * registry order among equal precedence; protected tokens precede ordinary
 * alternatives, which precede literal fallbacks. */
bool markdown_core_registry_prepare(markdown_core_mem *mem, const markdown_core_element *const *elements, size_t count,
                                    const markdown_core_element *extra, markdown_core_registry *registry) {
    unsigned char roles[256], bytes[256];
    size_t total = count + (extra != NULL);
    size_t candidates = 0, owners = 0, hooks = 0, indent_rows = 0;
    for (size_t i = 0; i < total; i++) {
        const markdown_core_element *element = i < count ? elements[i] : extra;
        candidates += inline_candidate_roles(element, roles, bytes);
        owners += S_owns_block_starts(element);
        hooks += (element->init_inline != NULL) + (element->finish_inline != NULL) + (element->dispose_inline != NULL);
        /* An upper bound on the indent rows: one per owner that declares an
         * indent, before the duplicates among them are known. Counting the
         * distinct ones here would mean reading every earlier descriptor
         * again for each of these, and a setup attaches its owners one at a
         * time, each attachment preparing the whole registry again. The rows
         * the duplicates would have taken are left unbuilt below, and
         * `block_owner_sets.rows` names the ones that exist. */
        indent_rows += S_owns_block_starts(element) && element->block_start_indent > 0;
    }
    size_t elements_size = total * sizeof(*registry->elements);
    size_t offsets_size = 257 * sizeof(size_t);
    size_t candidates_size = candidates * sizeof(markdown_core_inline_candidate);
    size_t owners_size = owners * sizeof(const markdown_core_element *);
    /* One word per byte holds 64 owners; a registry with more gets more
     * words per byte, never a limit. */
    size_t words = owners / 64 + 1;
    size_t rows = MARKDOWN_CORE_BLOCK_OWNER_BYTES + indent_rows;
    size_t sets_size = 4 * rows * words * sizeof(uint64_t);
    size_t thresholds_size = indent_rows * sizeof(int);
    size_t hooks_size = hooks * sizeof(const markdown_core_element *);
    /* The pointer tables before the sets end at a pointer boundary, which an
     * ABI with 4-byte pointers and 8-byte words (armeabi-v7a) does not accept
     * for a word: the sets begin at the next multiple of their word size. */
    size_t sets_align = sizeof(uint64_t);
    unsigned char *storage = mem->calloc(1, elements_size + offsets_size + candidates_size + owners_size + hooks_size +
                                                thresholds_size + (sets_align - 1) + sets_size + 2 * 256);
    if (!storage) {
        return false;
    }
    unsigned char *at = storage;
    const markdown_core_element **list = (const markdown_core_element **)at;
    size_t *offsets = (size_t *)(at += elements_size);
    markdown_core_inline_candidate *dispatch = (markdown_core_inline_candidate *)(at += offsets_size);
    const markdown_core_element **owner_table = (const markdown_core_element **)(at += candidates_size);
    const markdown_core_element **hook_table = (const markdown_core_element **)(at += owners_size);
    int *thresholds = (int *)(at += hooks_size);
    at += thresholds_size;
    at += (sets_align - (size_t)((uintptr_t)at % sets_align)) % sets_align;
    uint64_t *sets = (uint64_t *)at;
    int8_t *special = (int8_t *)(at += sets_size);
    int8_t *skip = special + 256;
    /* The declared indents, ascending and distinct, settled before a single
     * row is built: an owner then joins its own row and every row above it,
     * so the rows accumulate without any of them being copied or moved. Each
     * insertion walks only the indents already kept, never the descriptors. */
    size_t indent_used = 0;
    for (size_t i = 0; i < total; i++) {
        const markdown_core_element *element = i < count ? elements[i] : extra;
        if (!S_owns_block_starts(element) || element->block_start_indent <= 0) {
            continue;
        }
        size_t place = 0;
        while (place < indent_used && thresholds[place] < element->block_start_indent) {
            place++;
        }
        if (place < indent_used && thresholds[place] == element->block_start_indent) {
            continue;
        }
        for (size_t move = indent_used; move > place; move--) {
            thresholds[move] = thresholds[move - 1];
        }
        thresholds[place] = element->block_start_indent;
        indent_used++;
    }
    markdown_core_registry prepared = {
        .elements = list,
        .element_count = total,
        .inline_dispatch_offsets = offsets,
        .inline_dispatch = dispatch,
        .block_owners = owner_table,
        .block_owner_sets = {sets, sets + rows * words, sets + 2 * rows * words, sets + 3 * rows * words, words,
                             MARKDOWN_CORE_BLOCK_OWNER_BYTES + indent_used, indent_used ? thresholds : NULL},
        .inline_hooks = {hook_table, 0, 0, 0},
        .special_chars = special,
        .skip_chars = skip,
        .storage = storage};
    for (size_t i = 0; i < total; i++) {
        const markdown_core_element *element = i < count ? elements[i] : extra;
        list[i] = element;
        if (element->parse_text) {
            prepared.text_structure = element;
        }
        if (element->delimiter_rule != MARKDOWN_CORE_DELIM_RULE_NONE) {
            prepared.delimiter_owners[element->delimiter_rule] = element;
        }
        prepared.inline_hooks.init_count += element->init_inline != NULL;
        prepared.inline_hooks.finish_count += element->finish_inline != NULL;
        prepared.inline_hooks.dispose_count += element->dispose_inline != NULL;
        prepared.inline_completion_walk |= element->complete_inline && !element->complete_inline_on_request;
        if (S_owns_block_starts(element)) {
            size_t owner = prepared.block_owner_count++;
            uint64_t bit = (uint64_t)1 << (owner % 64);
            size_t word = owner / 64;
            owner_table[owner] = element;
            size_t stride = rows * words;
            /* The row this owner's declared indent took above, and so the
             * first row of the ones it joins: a line that reaches a higher
             * indent has reached this one, so the rows accumulate upward. */
            size_t indent_row = indent_used;
            if (element->block_start_indent > 0) {
                indent_row = 0;
                while (thresholds[indent_row] != element->block_start_indent) {
                    indent_row++;
                }
            }
            /* Each hook the element implements joins the rows of the bytes
             * that hook accepts at -- its own set where it declared one, the
             * element's otherwise -- in that hook's bank, and the indent rows
             * the element reaches. */
            for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
                if (!S_implements_block_hook(element, hook)) {
                    continue;
                }
                const unsigned char *accepted = S_block_hook_bytes(element, hook);
                uint64_t *bank = sets + hook * stride;
                for (size_t c = 0; c < MARKDOWN_CORE_BLOCK_OWNER_BYTES; c++) {
                    if (S_accepts_block_start_byte(accepted, c)) {
                        bank[c * words + word] |= bit;
                    }
                }
                for (size_t g = indent_row; element->block_start_indent > 0 && g < indent_used; g++) {
                    bank[(MARKDOWN_CORE_BLOCK_OWNER_BYTES + g) * words + word] |= bit;
                }
            }
        }
        if (!element->match_inline && !element->insert_inline_from_delim) {
            continue;
        }
        size_t used = inline_candidate_roles(element, roles, bytes);
        for (size_t j = 0; j < used; j++) {
            unsigned char c = bytes[j];
            offsets[c + 1]++;
            if (roles[c] & INLINE_TERMINATES) {
                special[c] = 1;
            }
        }
        for (const unsigned char *c = (const unsigned char *)element->flanking_transparent; c && *c; c++) {
            skip[*c] = 1;
        }
    }
    size_t next[256];
    for (size_t c = 0; c < 256; c++) {
        offsets[c + 1] += offsets[c];
        next[c] = offsets[c];
    }
    const markdown_core_element **init = hook_table, **finish = init + prepared.inline_hooks.init_count,
                                **dispose = finish + prepared.inline_hooks.finish_count;
    for (size_t i = 0; i < total; i++) {
        const markdown_core_element *element = list[i];
        if (element->init_inline) {
            *init++ = element;
        }
        if (element->finish_inline) {
            *finish++ = element;
        }
        if (element->dispose_inline) {
            *dispose++ = element;
        }
        size_t used = inline_candidate_roles(element, roles, bytes);
        for (size_t j = 0; j < used; j++) {
            unsigned char c = bytes[j];
            dispatch[next[c]++] = (markdown_core_inline_candidate){element, (roles[c] & INLINE_DISPATCHES) != 0,
                                                                   (roles[c] & INLINE_TERMINATES) != 0};
        }
    }
    for (size_t c = 0; c < 256; c++) {
        size_t first = offsets[c], end = offsets[c + 1];
        for (size_t i = first + 1; i < end; i++) {
            markdown_core_inline_candidate candidate = dispatch[i];
            size_t slot = i;
            while (slot > first &&
                   dispatch[slot - 1].element->inline_precedence > candidate.element->inline_precedence) {
                dispatch[slot] = dispatch[slot - 1];
                slot--;
            }
            dispatch[slot] = candidate;
        }
    }
    *registry = prepared;
    return true;
}

void markdown_core_registry_release(markdown_core_mem *mem, markdown_core_registry *registry) {
    mem->free(registry->storage);
    *registry = (markdown_core_registry){0};
}

/* Parse each source buffer once. Inline parsing completes fields at their
 * owning token; the structural walk therefore skips the emitted inline tree. */
static bool process_inline_tree(markdown_core_parser *parser, markdown_core_node *root, markdown_core_map *refmap) {
    markdown_core_iter walker;
    markdown_core_iter *iter = &walker;
    markdown_core_node *cur;
    markdown_core_event_type ev_type;
    bool whitespace = false;

    markdown_core_iter_init(iter, root);

    while (!parser->oom && (ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            if (contains_inlines(cur)) {
                if (!markdown_core_node_structure(cur)->deferred_inlines) {
                    whitespace |= markdown_core_parse_inlines(parser, cur, refmap);
                }
                markdown_core_iter_reset(iter, cur, MARKDOWN_CORE_EVENT_EXIT);
            }
            if (markdown_core_node_may_own_inline_subtrees(cur)) {
                whitespace |= markdown_core_parse_inline_subtrees(parser, cur, refmap);
            }
            /* A block's own attributes, attached while its lines were read,
             * are observed here, the way the completion walk observes an
             * inline node of a root that asked for it; a root's own tail
             * attributes were observed by that walk already. The reset above
             * consumed the root's EXIT, so ENTER is the one event a block
             * is seen at. */
            if (cur->attributes && parser->document_structure->observe_inline &&
                parser->document_structure->complete_inline_on_request) {
                parser->document_structure->observe_inline(parser, cur);
            }
        }
    }

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
    if (root_slot && *root_slot && !fields->parser->oom) {
        fields->whitespace |= process_inline_tree(fields->parser, *root_slot, fields->refmap);
    }
    return !fields->parser->oom;
}

bool markdown_core_parse_inline_subtrees(markdown_core_parser *parser, markdown_core_node *node,
                                         markdown_core_map *refmap) {
    inline_parse_context context = {parser, refmap, false};
    if (!markdown_core_visit_inline_subtrees(node, parse_inline_field, &context)) {
        parser->oom = true;
    }
    return context.whitespace;
}

typedef int (*tree_phase_func)(markdown_core_parser *parser, markdown_core_node **root_slot, void *context);
typedef void (*tree_node_func)(markdown_core_parser *parser, markdown_core_node *node, int script_depth, void *context);
/* Runs at a node's EXIT and returns the node now in the position, or NULL
 * once the position is released. `*successor` names the sibling the walk
 * continues at: the node's next sibling when called, which the hook moves
 * past any operand it merges into the node before it releases anything. */
typedef markdown_core_node *(*tree_exit_func)(markdown_core_parser *parser, markdown_core_node *node, int claim_depth,
                                              void *context, markdown_core_node **successor);

/* One owned tree being walked: its root's slot, and the cursor -- the node
 * whose ENTER (`entering`) or EXIT comes next. */
typedef struct {
    markdown_core_node **slot;
    markdown_core_node *root;
    markdown_core_node *cursor;
    bool entering;
    int script_depth, claim_depth;
} owned_tree_frame;

/* The frames start in the walk's own record, enough for a document whose
 * fields do not nest, and move to the heap only when they outgrow it. */
#define OWNED_TREE_FRAMES 8

typedef struct {
    markdown_core_parser *parser;
    owned_tree_frame *frames;
    size_t count, capacity;
    int script_depth;
    owned_tree_frame first_frames[OWNED_TREE_FRAMES];
} owned_tree_walk;

static int push_owned_tree(markdown_core_node **slot, void *context) {
    owned_tree_walk *walk = context;
    if (!slot || !*slot || walk->parser->oom) {
        return !walk->parser->oom;
    }
    if (walk->count == walk->capacity) {
        size_t capacity = 2 * walk->capacity;
        if (capacity > SIZE_MAX / sizeof(*walk->frames)) {
            walk->parser->oom = true;
            return 0;
        }
        bool inline_frames = walk->frames == walk->first_frames;
        void *frames =
            walk->parser->mem->realloc(inline_frames ? NULL : walk->frames, capacity * sizeof(*walk->frames));
        if (!frames) {
            walk->parser->oom = true;
            return 0;
        }
        if (inline_frames) {
            memcpy(frames, walk->first_frames, sizeof(walk->first_frames));
        }
        walk->frames = frames;
        walk->capacity = capacity;
    }
    walk->frames[walk->count++] = (owned_tree_frame){
        .slot = slot, .root = *slot, .cursor = *slot, .entering = true, .script_depth = walk->script_depth};
    return 1;
}

/* Every independent inline tree uses the same explicit continuation stack.
 * Field roots remain owned by their live slots until their children finish;
 * an exit or completion phase may then replace that root. Depth never uses C
 * recursion, and the walk steps through the tree's own links: a node is
 * entered, its children follow, and it is exited once they are. The frame's
 * position is held in locals while its tree is walked and returns to the
 * frame only when a field tree is pushed above it, so an event costs no
 * frame traffic and no iterator record. */
static int walk_owned_trees(markdown_core_parser *parser, markdown_core_node **slot, tree_node_func enter,
                            tree_exit_func exit, tree_phase_func finish, void *context, int script_depth) {
    owned_tree_walk walk = {.parser = parser, .script_depth = script_depth};
    walk.frames = walk.first_frames;
    walk.capacity = OWNED_TREE_FRAMES;
    push_owned_tree(slot, &walk);
    while (walk.count && !parser->oom) {
        owned_tree_frame *frame = &walk.frames[walk.count - 1];
        markdown_core_node *root = frame->root;
        markdown_core_node *node = frame->cursor;
        bool entering = frame->entering;
        int depth = frame->script_depth, claim = frame->claim_depth;
        for (;;) {
            bool word_body = node->element && node->element->delimiter.body == DELIMITER_WORD_BODY;
            bool claims = markdown_core_node_type_claims_text((markdown_core_node_type)node->kind);
            if (entering) {
                depth += word_body;
                claim += claims;
                if (enter) {
                    enter(parser, node, depth, context);
                    if (parser->oom) {
                        break;
                    }
                }
                if (markdown_core_node_may_own_inline_subtrees(node)) {
                    /* The fields finish before the node's children and its
                     * EXIT: the frame keeps the next event, the field trees
                     * go above it, and the top frame resumes. Settled before
                     * pushing, because pushing may move the frames. */
                    frame->cursor = node->first_child ? node->first_child : node;
                    frame->entering = node->first_child != NULL;
                    frame->script_depth = depth;
                    frame->claim_depth = claim;
                    walk.script_depth = depth;
                    size_t first = walk.count;
                    if (!markdown_core_visit_inline_subtrees(node, push_owned_tree, &walk)) {
                        parser->oom = true;
                    }
                    /* The visitor reports fields in source order; a stack
                     * consumes their reversed registration order. No field is
                     * visited or scanned twice. */
                    for (size_t left = first, right = walk.count; left < right && left < --right; left++) {
                        owned_tree_frame swap = walk.frames[left];
                        walk.frames[left] = walk.frames[right];
                        walk.frames[right] = swap;
                    }
                    break;
                }
                if (node->first_child) {
                    node = node->first_child;
                } else {
                    entering = false;
                }
                continue;
            }
            depth -= word_body;
            claim -= claims;
            markdown_core_node *parent = node->parent;
            markdown_core_node *successor = node->next;
            markdown_core_node *now = node;
            if (exit) {
                now = exit(parser, node, claim, context, &successor);
                if (parser->oom) {
                    break;
                }
            }
            if (node == root) {
                /* The frame root's EXIT is its last event, so the slot may
                 * take a replacement here; a failed walk keeps the tree for
                 * the transaction's cleanup. */
                *frame->slot = now;
                walk.count--;
                if (finish && !finish(parser, frame->slot, context)) {
                    parser->oom = true;
                }
                break;
            }
            if (successor) {
                node = successor;
                entering = true;
            } else {
                node = parent;
            }
        }
    }
    if (walk.frames != walk.first_frames) {
        parser->mem->free(walk.frames);
    }
    return !parser->oom;
}

static void complete_inline_node(markdown_core_parser *parser, markdown_core_node *node, int script_depth,
                                 void *context) {
    MARKDOWN_CORE_DIAGNOSTIC(parser->completion_work++;)
    const markdown_core_element *structure = markdown_core_node_structure(node);
    if (structure && structure->complete_inline) {
        structure->complete_inline(parser, node, script_depth);
    }
    if (parser->document_structure->observe_inline) {
        parser->document_structure->observe_inline(parser, node);
    }
}

static int process_inline_fields(markdown_core_parser *parser, markdown_core_node *root, void *context,
                                 int script_depth) {
    return walk_owned_trees(parser, &root, complete_inline_node, NULL, NULL, context, script_depth);
}

static int complete_independent_inlines(markdown_core_node **slot, void *context) {
    markdown_core_parser *parser = context;
    return parser->oom ? 0 : process_inline_fields(parser, *slot, NULL, 0);
}

void markdown_core_block_complete_inline_root(markdown_core_parser *parser, markdown_core_node *root) {
    markdown_core_node *slot = root;
    walk_owned_trees(parser, &slot, complete_inline_node, NULL, NULL, NULL, 0);
    /* The bodies this root's parse handed to the document -- inline
     * footnotes -- are independent contexts, walked from depth 0 as the
     * document-wide walk did; the collection remembers where the last such
     * walk stopped, so each body is walked once. */
    markdown_core_definition_collection *footnotes = &parser->footnotes;
    markdown_core_node *definition =
        footnotes->last_completed ? footnotes->last_completed->next : footnotes->first_inline;
    for (; definition && !parser->oom; definition = definition->next) {
        markdown_core_node *body = definition;
        walk_owned_trees(parser, &body, complete_inline_node, NULL, NULL, NULL, 0);
        footnotes->last_completed = definition;
    }
}

/* Whether some hook is delivered by walking the whole document rather than
 * the roots whose parse asked for a completion walk. */
static bool S_completion_walks_whole_tree(const markdown_core_parser *parser) {
    const markdown_core_element *document = parser->document_structure;
    return parser->registry->inline_completion_walk ||
           (document->observe_inline && !document->complete_inline_on_request);
}

/* Parse each source buffer with its owned fields. A root's completion walk
 * runs as its parse ends (markdown_core_inline_finish_inlines), for the
 * roots that asked for it; only an element that does not ask makes the
 * whole document walk. */
static void process_inlines(markdown_core_parser *parser, markdown_core_map *refmap, void *context) {
    process_inline_tree(parser, parser->root, refmap);
    if (parser->oom || !S_completion_walks_whole_tree(parser)) {
        return;
    }
    process_inline_fields(parser, parser->root, context, 0);
    markdown_core_visit_block_subtrees(parser->root, complete_independent_inlines, parser);
}

/* Block syntax and all anchor decisions finish before definitions disappear.
 * Walk in postorder so list layout sees the cleaned children. Each tree edge
 * is followed at most once in each direction, with no recursion or extra allocation;
 * the document owns every pending definition even if parsing fails earlier. */
/* Serve the completion queue: every block its element completes, and every
 * paragraph consumed entirely by reference definitions, which retained block
 * adjacency until now and leaves before list layout and inline parsing
 * observe the semantic children. Finalization order puts a block after the
 * blocks it contains, so a container completes over completed children. */
static void S_complete_blocks(markdown_core_parser *parser) {
    for (size_t i = 0; i < parser->completion_count && !parser->oom; i++) {
        markdown_core_node *node = parser->completions[i];
        if (!(node->flags & MARKDOWN_CORE_NODE__COMPLETION_QUEUED)) {
            continue;
        }
        node->flags &= ~MARKDOWN_CORE_NODE__COMPLETION_QUEUED;
        if (node->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY) {
            markdown_core_node_recycle(parser->arena, node);
            continue;
        }
        const markdown_core_element *structure = markdown_core_node_structure(node);
        if (structure && structure->complete_block) {
            structure->complete_block(parser, node);
        }
    }
    parser->completion_count = 0;
}

static markdown_core_node *finalize_document(markdown_core_parser *parser) {
    while (parser->current != parser->root) {
        parser->current = markdown_core_block_finalize(parser, parser->current);
    }

    markdown_core_block_finalize(parser, parser->root);

    return parser->root;
}

static void S_parse_block_inputs(markdown_core_parser *parser) {
    while (parser->block_input_cursor < parser->block_input_count && !parser->oom) {
        markdown_core_node *owner = parser->block_inputs[parser->block_input_cursor++];
        parser->block_root = owner;
        parser->current = owner;
        parser->input_line_count = 0;
        parser->input_first_line = parser->line_marks[owner->content_mark].line;
        parser->line_number = parser->input_first_line - 1;
        parser->last_line_length = 0;
        parser->lookahead_base_line = 0;
        parser->lookahead_last_line_ready = false;
        if (parser->lookahead_entries_used) {
            memset(parser->lookahead_entries, 0,
                   (size_t)parser->lookahead_entries_used * sizeof(*parser->lookahead_entries));
            parser->lookahead_entries_used = 0;
        }
        for (bufsize_t offset = 0; offset < owner->content->size && !parser->oom;) {
            if (parser->input_line_count == parser->input_line_capacity) {
                size_t capacity = parser->input_line_capacity ? parser->input_line_capacity * 2 : 16;
                if (capacity > SIZE_MAX / sizeof(*parser->input_line_offsets)) {
                    parser->oom = true;
                    break;
                }
                void *offsets =
                    parser->mem->realloc(parser->input_line_offsets, capacity * sizeof(*parser->input_line_offsets));
                if (!offsets) {
                    parser->oom = true;
                    break;
                }
                parser->input_line_offsets = offsets;
                parser->input_line_capacity = capacity;
            }
            parser->input_line_offsets[parser->input_line_count++] = offset;
            while (offset < owner->content->size && !markdown_core_is_line_end(owner->content->ptr[offset])) {
                offset++;
            }
            if (offset < owner->content->size && owner->content->ptr[offset] == '\r') {
                offset++;
            }
            if (offset < owner->content->size && owner->content->ptr[offset] == '\n') {
                offset++;
            }
        }
        owner->flags |= MARKDOWN_CORE_NODE__OPEN;
        S_parse_source(parser, owner->content->ptr, (size_t)owner->content->size);
        while (parser->current != owner && !parser->oom) {
            parser->current = markdown_core_block_finalize(parser, parser->current);
        }
        owner->flags &= ~MARKDOWN_CORE_NODE__OPEN;
        markdown_core_strbuf_clear(owner->content);
        owner->content_mark_count = 0;
        owner->content_mark_offset = 0;
    }
    parser->block_root = parser->root;
    parser->current = parser->root;
}

markdown_core_node *markdown_core_parse_document(const char *buffer, size_t len) {
    return markdown_core_parse_document_with_mem(buffer, len, markdown_core_get_default_mem_allocator(), NULL, NULL);
}

markdown_core_node *markdown_core_parse_document_with_mem(const char *source, size_t length, markdown_core_mem *mem,
                                                          markdown_core_parser_setup_func setup, void *context) {
    static const unsigned char empty[] = "";
    markdown_core_parser *parser;
    markdown_core_node *document;

    if ((!source && length != 0) || length > (size_t)(INT32_MAX / 2)) {
        return NULL;
    }
    parser = S_parser_new(mem);
    if (!parser) {
        return NULL;
    }
    /* The complete immutable dialect, prepared: nothing is projected here. */
    S_install_registry(parser, markdown_core_core_registry());
    if (setup && !setup(parser, context)) {
        S_parser_free(parser);
        return NULL;
    }

    S_parse_source(parser, source ? (const unsigned char *)source : empty, length);
    document = S_finish_parse(parser);
    S_parser_free(parser);
    return document;
}

/* One reservation for the whole contribution to a normalized line, and then
 * a test.
 *
 * Reserving first is what makes the refusal atomic: the NUL path writes twice,
 * and a failure between the two writes leaves a line that is neither the old
 * one nor the new one. The arithmetic is done in 64 bits because `bufsize_t` is
 * int32_t and `size + add` is exactly the overflow A4 closed one level down. */
static bool S_line_scratch_reserve(markdown_core_parser *parser, int64_t add) {
    int64_t target = (int64_t)parser->line_scratch.size + add;

    if (add < 0 || target > (int64_t)(INT32_MAX / 2)) {
        parser->line_scratch.oom = 1;
    } else if (add > 0) {
        markdown_core_strbuf_grow(&parser->line_scratch, (bufsize_t)target);
    }
    if (parser->line_scratch.oom) {
        parser->oom = true;
        return false;
    }
    return true;
}

static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length) {
    size_t metadata_length = parser->block_root == parser->root
                                 ? parser->document_structure->read_document_prefix(parser, source, length)
                                 : 0;
    const unsigned char *cursor = source + metadata_length;
    const unsigned char *end = source + length;
    static const uint8_t repl[] = {239, 191, 189};
    /* The line ends at the first LF, CR or NUL, each found with a vector
     * search. A source without any CR or NUL -- the common one -- is told so
     * by one pass each up front and then pays one search per line; the
     * others bound the CR and NUL searches by the LF found, so a byte is
     * examined at most three times and never one at a time. */
    const bool has_cr = memchr(cursor, '\r', (size_t)(end - cursor)) != NULL;
    const bool has_nul = memchr(cursor, '\0', (size_t)(end - cursor)) != NULL;

    while (cursor < end && !parser->oom) {
        const unsigned char *eol;
        bufsize_t segment_length;
        bool line_complete;

        {
            const unsigned char *limit = memchr(cursor, '\n', (size_t)(end - cursor));
            limit = limit ? limit : end;
            if (has_cr) {
                const unsigned char *cr = memchr(cursor, '\r', (size_t)(limit - cursor));
                limit = cr ? cr : limit;
            }
            if (has_nul) {
                const unsigned char *nul = memchr(cursor, '\0', (size_t)(limit - cursor));
                limit = nul ? nul : limit;
            }
            eol = limit;
        }
        line_complete = eol == end || markdown_core_is_line_end(*eol);
        segment_length = (bufsize_t)(eol - cursor);
        if (line_complete) {
            /* Where the next raw line begins, for a block start that must look
             * past its own line before it opens (markdown_core_parser_lookahead_begin). */
            const unsigned char *next = eol;
            if (next < end && *next == '\r') {
                next++;
            }
            if (next < end && *next == '\n') {
                next++;
            }
            parser->lookahead_cursor = next;
            parser->lookahead_end = end;
            if (parser->line_scratch.size > 0) {
                if (!S_line_scratch_reserve(parser, segment_length)) {
                    return;
                }
                markdown_core_strbuf_put(&parser->line_scratch, cursor, segment_length);
                parser->line_source = NULL;
                S_process_line(parser, parser->line_scratch.ptr, parser->line_scratch.size);
                markdown_core_strbuf_clear(&parser->line_scratch);
            } else {
                parser->line_source = cursor;
                parser->line_source_length = (bufsize_t)(next - cursor);
                S_process_line(parser, cursor, segment_length);
            }
        } else {
            /* Omit the NUL byte and put U+FFFD in its place. */
            if (!S_line_scratch_reserve(parser, (int64_t)segment_length + 3)) {
                return;
            }
            markdown_core_strbuf_put(&parser->line_scratch, cursor, segment_length);
            markdown_core_strbuf_put(&parser->line_scratch, repl, 3);
        }

        cursor += segment_length;
        if (cursor < end) {
            if (*cursor == '\0') {
                cursor++;
            } else {
                if (*cursor == '\r') {
                    cursor++;
                }
                if (cursor < end && *cursor == '\n') {
                    cursor++;
                }
            }
        }
        if (parser->claimed_cursor) {
            assert(parser->claimed_cursor >= cursor && parser->claimed_cursor <= end);
            cursor = parser->claimed_cursor;
            parser->line_number = parser->claimed_line;
            parser->last_line_length = parser->claimed_last_column;
            parser->claimed_cursor = NULL;
        }
    }

    /* A final NUL has no line terminator to trigger the completed line. */
    if (!parser->oom && parser->line_scratch.size > 0) {
        parser->lookahead_cursor = end;
        parser->lookahead_end = end;
        parser->line_source = NULL;
        S_process_line(parser, parser->line_scratch.ptr, parser->line_scratch.size);
        markdown_core_strbuf_clear(&parser->line_scratch);
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
void markdown_core_block_find_first_nonspace(markdown_core_parser *parser, markdown_core_chunk *input) {
    if (parser->first_nonspace <= parser->offset) {
        bufsize_t at = parser->offset;
        bufsize_t column = parser->column;
        /* A run of spaces is as long in columns as it is in bytes, so the
         * whole of an ordinary indent is crossed by finding where it ends --
         * the line's terminator stops the run like any other byte. Only a tab
         * needs a column of its own, and that is the tab stop above the column
         * the run before it reached: the running `chars_to_tab` this loop used
         * to carry was always `TAB_STOP - column % TAB_STOP`, for spaces (one
         * column each) as much as for tabs (which land on a stop). */
        for (;;) {
            bufsize_t run = at;
            while (peek_at(input, at) == ' ') {
                at++;
            }
            column += at - run;
            if (peek_at(input, at) != '\t') {
                break;
            }
            at++;
            column += TAB_STOP - (column % TAB_STOP);
        }
        parser->first_nonspace = at;
        parser->first_nonspace_column = column;
    }

    parser->indent = (int)(parser->first_nonspace_column - parser->column);
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
    /* An ASCII byte other than a tab is one byte, one column and one scalar,
     * so a run of them advances by its length in either unit -- the whole of
     * an indent of spaces, in one step. */
    {
        bufsize_t at = parser->offset;
        bufsize_t stop = count < input->len - at ? at + count : input->len;
        while (at < stop && (unsigned char)(input->data[at] - 1) < 0x7f && input->data[at] != '\t') {
            at++;
        }
        if (at > parser->offset) {
            bufsize_t advanced = at - parser->offset;
            parser->partially_consumed_tab = false;
            parser->offset = at;
            parser->column += advanced;
            count -= advanced;
        }
    }
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
static bool S_container_prefix_matches(markdown_core_parser *parser, markdown_core_node *container,
                                       markdown_core_chunk *input, const markdown_core_node *joining, bool *taken) {
    const markdown_core_element *structure = markdown_core_node_structure(container);
    return !structure || !structure->continue_container ||
           structure->continue_container(parser, container, input, joining, taken);
}

static bool parse_element_block(markdown_core_parser *parser, markdown_core_node *container, markdown_core_chunk *input,
                                bool *should_continue, markdown_core_node **closing) {
    int matched;
    const markdown_core_element *structure = markdown_core_node_structure(container);

    if (!structure->last_block_matches) {
        return false;
    }

    matched = structure->last_block_matches(structure, parser, input->data, input->len, container);
    if (matched && structure->pending_close) {
        *closing = matched == MARKDOWN_CORE_BLOCK_PENDING_CLOSE ? container : NULL;
    } else if (matched && element_accepts_lines(container)) {
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
    assert(!is_paragraph(container));
    container->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
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

        markdown_core_block_find_first_nonspace(parser, input);

        const markdown_core_element *structure = markdown_core_node_structure(container);
        if (structure && structure->last_block_matches) {
            if (!parse_element_block(parser, container, input, &should_continue, &closing)) {
                goto done;
            }
        } else {
            bool taken = false;
            if (structure && structure->accepts_blank && parser->blank &&
                !structure->accepts_blank(parser, container)) {
                goto done;
            }
            if (!S_container_prefix_matches(parser, container, input, NULL, &taken)) {
                goto done;
            }
            if (taken) {
                if (element_accepts_lines(parser->current)) {
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
    chain = parser->mem->realloc(parser->lookahead_chain, (size_t)capacity * sizeof(*chain));
    if (!chain) {
        parser->oom = true;
        return false;
    }
    parser->lookahead_chain = chain;
    flags = parser->mem->realloc(parser->lookahead_chain_flags, (size_t)capacity * sizeof(*flags));
    if (!flags) {
        parser->oom = true;
        return false;
    }
    parser->lookahead_chain_flags = flags;
    parser->lookahead_chain_alloc = capacity;
    return true;
}

/* The cache entry of a source line, growing the cache to reach it. Lines are
 * numbered from the first line any lookahead visited: candidates come in
 * source order and each begins at the line after its own, so no lookahead
 * asks about an earlier line. NULL when the cache could not grow, with the
 * parse marked lost. */
markdown_core_lookahead_entry *markdown_core_parser_lookahead_entry(markdown_core_parser *parser, int line) {
    int index;

    if (parser->lookahead_base_line == 0) {
        parser->lookahead_base_line = line;
    }
    index = line - parser->lookahead_base_line;
    if (index < 0) {
        /* Unreachable by the ordering argument above; a line before the base
         * is matched without the cache rather than through it. */
        return NULL;
    }
    if (index >= parser->lookahead_entries_alloc) {
        int capacity = parser->lookahead_entries_alloc ? parser->lookahead_entries_alloc : 64;
        markdown_core_lookahead_entry *entries;
        while (capacity <= index) {
            capacity = capacity > INT_MAX / 2 ? INT_MAX : capacity * 2;
        }
        if ((size_t)capacity > SIZE_MAX / sizeof(*entries)) {
            parser->oom = true;
            return NULL;
        }
        entries = parser->mem->realloc(parser->lookahead_entries, (size_t)capacity * sizeof(*entries));
        if (!entries) {
            parser->oom = true;
            return NULL;
        }
        memset(entries + parser->lookahead_entries_alloc, 0,
               (size_t)(capacity - parser->lookahead_entries_alloc) * sizeof(*entries));
        parser->lookahead_entries = entries;
        parser->lookahead_entries_alloc = capacity;
    }
    if (parser->lookahead_entries_used <= index) {
        parser->lookahead_entries_used = index + 1;
    }
    return &parser->lookahead_entries[index];
}

/* The blank run that began at `run_start` ends before `line`, which begins at `cursor`. */
static void S_lookahead_close_run(markdown_core_block_lookahead *lookahead, int line, const unsigned char *cursor) {
    if (lookahead->run_start) {
        markdown_core_lookahead_entry *entry =
            markdown_core_parser_lookahead_entry(lookahead->parser, lookahead->run_start);
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
        const markdown_core_element *structure = markdown_core_node_structure(parser->lookahead_chain[i]);
        if (!structure || !structure->blank_runs) {
            return false;
        }
    }
    return true;
}

static markdown_core_node *S_lookahead_parent(markdown_core_node *parent, markdown_core_node_type child) {
    while (parent->parent && !markdown_core_node_can_contain_type(parent, child)) {
        parent = parent->parent;
    }
    return parent;
}

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, markdown_core_node *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead) {
    markdown_core_node *parent = S_lookahead_parent(parent_container, child);
    markdown_core_node *node;
    int depth = 0;
    int i;

    memset(lookahead, 0, sizeof(*lookahead));
    /* The block joins the nearest open container that can hold it, which is
     * where `markdown_core_parser_add_child` backs up to when it is opened. */
    for (node = parent; node; node = node == parser->block_root ? NULL : node->parent) {
        depth++;
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->block_lookahead_work += (size_t)depth;)
    if (!S_lookahead_reserve_chain(parser, depth)) {
        return false;
    }
    i = depth;
    for (node = parent; node; node = node == parser->block_root ? NULL : node->parent) {
        i--;
        parser->lookahead_chain[i] = node;
        parser->lookahead_chain_flags[i] = node->flags;
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->block_lookahead_work += (size_t)depth;)

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
    const unsigned char *end = parser->lookahead_end;

    *blank_lines = 0;
    if (!lookahead->active) {
        return 0;
    }
    while (lookahead->cursor && lookahead->cursor < end && !parser->oom) {
        const unsigned char *start = lookahead->cursor;
        const unsigned char *eol = start;
        const unsigned char *next;
        markdown_core_chunk input;
        markdown_core_lookahead_entry *entry;
        int this_line = lookahead->line;
        int from = 1;
        MARKDOWN_CORE_DIAGNOSTIC(bufsize_t resumed_offset;)
        bool carried = true;
        bool closing = false;
        bool taken = false;
        bool resumed = false;
        bool blank;
        int i;

        while (eol < end && !markdown_core_is_line_end((char)*eol)) {
            eol++;
        }
        next = eol;
        if (next < end && *next == '\r') {
            next++;
        }
        if (next < end && *next == '\n') {
            next++;
        }
        MARKDOWN_CORE_DIAGNOSTIC(parser->block_lookahead_work++;)
        if (next == end) {
            /* The input's last line, normalized once: the matchers read a line
             * through its terminator, and the source may not end in one. */
            if (!parser->lookahead_last_line_ready) {
                markdown_core_strbuf_set(&parser->lookahead_last_line, start, (bufsize_t)(eol - start));
                markdown_core_strbuf_putc(&parser->lookahead_last_line, '\n');
                if (parser->lookahead_last_line.oom) {
                    parser->oom = true;
                    return 0;
                }
                parser->lookahead_last_line_ready = true;
            }
            input.data = parser->lookahead_last_line.ptr;
            input.len = parser->lookahead_last_line.size;
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

        entry = markdown_core_parser_lookahead_entry(parser, this_line);
        if (!entry && parser->oom) {
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
                parser->first_nonspace = entry->first_nonspace;
                parser->first_nonspace_column = entry->first_nonspace_column;
            }
        }
        MARKDOWN_CORE_DIAGNOSTIC(resumed_offset = parser->offset;)
        for (i = from; i < lookahead->depth && carried && !taken; i++) {
            markdown_core_node *container = parser->lookahead_chain[i];
            markdown_core_block_find_first_nonspace(parser, &input);
            const markdown_core_element *structure = markdown_core_node_structure(container);
            if (structure && structure->last_block_matches) {
                int match = structure->continues_block
                                ? structure->continues_block(structure, parser, input.data, (int)input.len, container)
                                : 0;
                carried = match != 0;
                if (carried && structure->pending_close) {
                    closing = match == MARKDOWN_CORE_BLOCK_PENDING_CLOSE;
                }
            } else {
                carried = S_container_prefix_matches(parser, container, &input, lookahead->parent, &taken);
            }
        }
        /* The work of this visit is the prefix bytes it had to match itself:
         * what the resumed state did not already cover. */
        MARKDOWN_CORE_DIAGNOSTIC(parser->block_lookahead_work += (size_t)(parser->offset - resumed_offset);)
        if (!carried || closing) {
            S_lookahead_close_run(lookahead, this_line, start);
            lookahead->active = false;
            return 0;
        }
        if (!taken) {
            markdown_core_block_find_first_nonspace(parser, &input);
        }
        blank = taken || parser->blank;

        /* Record the deepest result for the scans that come after this one.
         * A run an earlier scan recorded from this line stays until this scan
         * closes its own, which ends where that one did. */
        if (entry) {
            entry->container = lookahead->parent;
            entry->depth = lookahead->depth - 1;
            entry->offset = parser->offset;
            entry->column = parser->column;
            entry->first_nonspace = parser->first_nonspace;
            entry->first_nonspace_column = parser->first_nonspace_column;
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
        markdown_core_node *node = parser->lookahead_chain[i];
        const markdown_core_element *structure = markdown_core_node_structure(node);
        unsigned mask = structure ? structure->speculative_flags : 0;
        node->flags =
            (markdown_core_node_internal_flags)((node->flags & ~mask) | (parser->lookahead_chain_flags[i] & mask));
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->block_lookahead_work += (size_t)lookahead->depth;)
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

const markdown_core_block_peek *markdown_core_parser_peek_block_line(markdown_core_parser *parser,
                                                                     markdown_core_node *parent,
                                                                     markdown_core_node_type child) {
    parent = S_lookahead_parent(parent, child);
    markdown_core_block_peek *peek = &parser->block_peek;
    if (peek->parent == parent) {
        return peek;
    }
    *peek = (markdown_core_block_peek){.parent = parent};
    markdown_core_block_lookahead lookahead;
    if (markdown_core_parser_lookahead_begin(parser, parent, child, &lookahead)) {
        peek->available = markdown_core_parser_lookahead_next(&lookahead, &peek->input, &peek->first, &peek->indent,
                                                              &peek->blanks) != 0;
        markdown_core_parser_lookahead_end(&lookahead);
    }
    return peek;
}

void markdown_core_parser_note_paragraph_line(markdown_core_parser *parser, markdown_core_node *paragraph,
                                              const markdown_core_chunk *input) {
    const unsigned char *data = parser->line_source;
    bufsize_t length = parser->line_source_length;
    if (!data) {
        /* A rewritten line has no source bytes of its own shape: keep the
         * line the parser built, which is what this line's grammar read. */
        markdown_core_strbuf_set(&parser->paragraph_line_copy, input->data, input->len);
        if (parser->paragraph_line_copy.oom) {
            parser->oom = true;
            return;
        }
        data = parser->paragraph_line_copy.ptr;
        length = input->len;
    }
    parser->paragraph_line = (markdown_core_paragraph_line){.node = paragraph,
                                                            .data = data,
                                                            .length = length,
                                                            .offset = parser->offset,
                                                            .first = parser->first_nonspace,
                                                            .first_column = parser->first_nonspace_column,
                                                            .indent = parser->indent,
                                                            .line = parser->line_number,
                                                            .after = parser->lookahead_cursor};
}

const markdown_core_paragraph_line *markdown_core_parser_paragraph_line(const markdown_core_parser *parser,
                                                                        const markdown_core_node *paragraph) {
    const markdown_core_paragraph_line *line = &parser->paragraph_line;
    if (line->node != paragraph) {
        return NULL;
    }
    bool open = (paragraph->flags & MARKDOWN_CORE_NODE__OPEN) != 0;
    if (open ? paragraph->start_line != parser->line_number - 1 : paragraph->start_line != paragraph->end_line) {
        return NULL;
    }
    return line;
}

/* The second row an arbitration reads for a line: the indent row of the
 * highest declared indent the line reaches -- the rows accumulate, so that
 * one row names every owner whose indent it reached and no owner that asked
 * for more -- and the line's own byte row when it reaches none, where oring
 * it in changes nothing. */
static size_t S_block_owner_row(const markdown_core_block_owner_sets *sets, unsigned char byte, int indent) {
    size_t row = byte;
    for (size_t g = MARKDOWN_CORE_BLOCK_OWNER_BYTES; g < sets->rows; g++) {
        if (sets->indent_thresholds[g - MARKDOWN_CORE_BLOCK_OWNER_BYTES] > indent) {
            break;
        }
        row = g;
    }
    return row;
}

/* Each arbitration loop visits the owners of the line's first byte, and of
 * its indent, that implement its hook, in registry order, and no other. */
static bool scan_element_start(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    const markdown_core_registry *registry = parser->registry;
    const markdown_core_block_owner_sets *sets = &registry->block_owner_sets;
    unsigned char byte = S_block_start_byte(context->input, context->first);
    size_t row = S_block_owner_row(sets, byte, context->indent);
    for (size_t word = 0; word < sets->words; word++) {
        for (uint64_t owners = sets->scan[byte * sets->words + word] | sets->scan[row * sets->words + word]; owners;
             owners &= owners - 1) {
            const markdown_core_element *element = registry->block_owners[word * 64 + markdown_core_lowest_bit(owners)];
            MARKDOWN_CORE_DIAGNOSTIC(parser->block_dispatch_work++;)
            if (context->indent > element->maximum_block_indent) {
                continue;
            }
            if (element->scan_block_start(parser, context, start)) {
                return true;
            }
            if (parser->oom) {
                return false;
            }
        }
    }
    return false;
}

static block_start scan_block_start(markdown_core_parser *parser, block_start_context *context) {
    block_start start = {0};
    scan_element_start(parser, context, &start);
    return start;
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
                                   .lazy = paragraph,
                                   .all_matched = true,
                                   .speculative = true,
                                   .depth = 1};
    block_start start = scan_block_start(parser, &context);
    if (parser->oom) {
        return false;
    }
    if (start.kind != MARKDOWN_CORE_NODE_NONE) {
        return true;
    }
    for (size_t element_index = 0; element_index < parser->element_count; element_index++) {
        const markdown_core_element *element = parser->elements[element_index];
        if (element->interrupts_paragraph && element->probe_block &&
            element->probe_block(parser, input, first, indent, reader)) {
            return true;
        }
        if (parser->oom) {
            break;
        }
    }
    return false;
}

static void open_new_blocks(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                            bool all_matched) {
    bool maybe_lazy = is_paragraph(parser->current);
    size_t depth = 0;

    while (!element_accepts_lines(*container)) {
        parser->block_peek.parent = NULL;
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
                                       .paragraph = is_paragraph(*container),
                                       .lazy = maybe_lazy,
                                       .all_matched = all_matched,
                                       .depth = depth,
                                       .thematic_kill = parser->thematic_break_kill_pos};
        block_start start = scan_block_start(parser, &context);
        if (parser->oom) {
            return;
        }
        parser->thematic_break_kill_pos = context.thematic_kill;
        const markdown_core_registry *registry = parser->registry;
        const markdown_core_element *const *owners = registry->block_owners;
        const markdown_core_block_owner_sets *sets = &registry->block_owner_sets;
        size_t words = sets->words;
        unsigned char byte = S_block_start_byte(input, parser->first_nonspace);
        size_t row = S_block_owner_row(sets, byte, parser->indent);

        /* Dash-led tables precede thematic breaks and lists. An opener may
         * close the old path before an allocation fails; OOM is terminal,
         * never a grammar miss that can try another owner on that path. */
        for (size_t word = 0; word < words; word++) {
            for (uint64_t set = sets->interrupt[byte * words + word] | sets->interrupt[row * words + word]; set;
                 set &= set - 1) {
                const markdown_core_element *owner = owners[word * 64 + markdown_core_lowest_bit(set)];
                MARKDOWN_CORE_DIAGNOSTIC(parser->block_dispatch_work++;)
                markdown_core_node *opened = owner->try_interrupting_block(parser, *container, input, maybe_lazy);
                if (parser->oom) {
                    return;
                }
                if (opened) {
                    *container = opened;
                    return;
                }
            }
        }

        if (start.open) {
            if (!start.open(parser, container, input, &start)) {
                return;
            }
        } else {
            markdown_core_node *new_container = NULL;

            for (size_t word = 0; word < words && !new_container; word++) {
                for (uint64_t set = sets->open[byte * words + word] | sets->open[row * words + word]; set;
                     set &= set - 1) {
                    const markdown_core_element *element = owners[word * 64 + markdown_core_lowest_bit(set)];
                    MARKDOWN_CORE_DIAGNOSTIC(parser->block_dispatch_work++;)
                    new_container = element->try_opening_block(element, parser->indent > element->maximum_block_indent,
                                                               parser, *container, input->data, input->len);
                    if (parser->oom) {
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
            }

            if (!new_container) {
                if (!maybe_lazy && !is_paragraph(*container)) {
                    for (size_t word = 0; word < words; word++) {
                        for (uint64_t set = sets->paragraph[byte * words + word] | sets->paragraph[row * words + word];
                             set; set &= set - 1) {
                            const markdown_core_element *element = owners[word * 64 + markdown_core_lowest_bit(set)];
                            MARKDOWN_CORE_DIAGNOSTIC(parser->block_dispatch_work++;)
                            new_container =
                                element->try_opening_paragraph(element, parser->indent > element->maximum_block_indent,
                                                               parser, *container, input->data, input->len);
                            if (parser->oom) {
                                return;
                            }
                            if (new_container) {
                                *container = new_container;
                                return;
                            }
                        }
                    }
                }
                break;
            }
        }

        /* What this opener consumed made the block it just opened, so the
         * block owns it: `> `, `- `, the `#`s of a heading, the opening fence,
         * `[^label]:`. Claimed once per turn of the loop -- once per block
         * opened -- and before `markdown_core_block_accepts_lines` breaks out. */

        if (markdown_core_block_accepts_lines(*container)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        maybe_lazy = false;
    }
}

static void add_text_to_container(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_node *last_matched_container, markdown_core_chunk *input) {
    markdown_core_node *tmp;
    // what remains at parser->offset is a text line.  add the text to the
    // appropriate container.

    markdown_core_block_find_first_nonspace(parser, input);

    if (parser->blank && container->last_child) {
        S_set_last_line_blank(container->last_child, true);
    }

    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    const markdown_core_element *structure = markdown_core_node_structure(container);
    const bool last_line_blank = parser->blank && !(structure && structure->blank_opaque) &&
                                 (!element_accepts_lines(container) || (structure && structure->blank_line)) &&
                                 (!structure || !structure->blank_line || structure->blank_line(parser, container));

    S_set_last_line_blank(container, last_line_blank);

    tmp = container;
    while (tmp != parser->block_root && tmp->parent) {
        S_set_last_line_blank(tmp->parent, false);
        tmp = tmp->parent;
    }

    // If the last line processed belonged to a paragraph node,
    // and we didn't match all of the line prefixes for the open containers,
    // and we didn't start any new containers,
    // and the line isn't blank,
    // then treat this as a "lazy continuation line" and add it to
    // the open paragraph.
    const markdown_core_element *current_structure = markdown_core_node_structure(parser->current);
    if (parser->current != last_matched_container && container == last_matched_container && !parser->blank &&
        current_structure && current_structure->accepts_lazy &&
        current_structure->accepts_lazy(parser, parser->current)) {
        parser->current = current_structure->open_lazy(parser, parser->current);
        if (!parser->current) {
            return;
        }
        markdown_core_block_add_line(parser->current, input, parser);
    } else { // not a lazy continuation
        // Finalize any blocks that were not matched and set cur to container:
        markdown_core_parser_finalize_unmatched_blocks(parser);

        if (element_accepts_lines(container)) {
            markdown_core_block_add_line(container, input, parser);
            if (structure && structure->ends_block && structure->ends_block(parser, container, input)) {
                container->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
                container = markdown_core_block_finalize(parser, container);
            }
        } else if (parser->blank) {
            // ??? do nothing
        } else if (markdown_core_block_accepts_lines(container)) {
            markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            markdown_core_block_add_line(container, input, parser);
        } else {
            container = parser->document_structure->open_text_block(parser, container, input);
            if (!container) {
                return;
            }
            markdown_core_block_add_line(container, input, parser);
        }

        parser->current = container;
    }
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes) {
    parser->block_peek.parent = NULL;
    markdown_core_node *last_matched_container;
    bool all_matched = true;
    markdown_core_node *container;
    markdown_core_chunk input;

    if (parser->oom || parser->root == NULL) {
        return;
    }

    markdown_core_strbuf_clear(&parser->curline);

    markdown_core_strbuf_put(&parser->curline, buffer, bytes);

    bytes = parser->curline.size;

    // ensure line ends with a newline:
    if (bytes == 0 || !markdown_core_is_line_end(parser->curline.ptr[bytes - 1])) {
        markdown_core_strbuf_putc(&parser->curline, '\n');
    }

    if (parser->curline.oom) {
        parser->oom = true;
        return;
    }

    parser->offset = 0;
    parser->column = 0;
    parser->first_nonspace = 0;
    parser->first_nonspace_column = 0;
    parser->thematic_break_kill_pos = 0;
    parser->table_separator_kill_pos = 0;
    parser->indent = 0;
    parser->blank = false;
    parser->partially_consumed_tab = false;

    input.data = parser->curline.ptr;
    input.len = parser->curline.size;
    input.alloc = 0;

    // Skip UTF-8 BOM.
    if (parser->line_number == 0 && input.len >= 3 && memcmp(input.data, "\xef\xbb\xbf", 3) == 0) {
        parser->offset += 3;
    }

    parser->line_number++;

    last_matched_container = check_open_blocks(parser, &input, &all_matched);

    if (!last_matched_container) {
        goto finished;
    }

    container = last_matched_container;
    parser->matched_container = last_matched_container;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->oom) {
        goto finished;
    }

    if (parser->claimed_cursor) {
        parser->current = container->flags & MARKDOWN_CORE_NODE__OPEN ? container : container->parent;
        goto finished;
    }

    add_text_to_container(parser, container, last_matched_container, &input);

finished:
    /* Block scopes cover the complete physical line, including closing
     * delimiters and attribute containers. Inline content trimming never
     * changes this source boundary. */
    parser->last_line_length = parser->curline.size;
    if (parser->last_line_length && parser->curline.ptr[parser->last_line_length - 1] == '\n') {
        parser->last_line_length -= 1;
    }
    if (parser->last_line_length && parser->curline.ptr[parser->last_line_length - 1] == '\r') {
        parser->last_line_length -= 1;
    }
    parser->last_line_length =
        markdown_core_parser_source_column(parser, parser->line_number, parser->last_line_length);

    markdown_core_strbuf_clear(&parser->curline);
}

typedef struct {
    markdown_core_parser *parser;
    tree_phase_func phase;
    void *context;
} tree_phase_context;
static int apply_independent_phase(markdown_core_node **slot, void *context) {
    tree_phase_context *phase = context;
    return walk_owned_trees(phase->parser, slot, NULL, NULL, phase->phase, phase->context, 0);
}

/* Document definitions are independent roots, followed by the content tree.
 * Each family advances through the live slot after a phase replaces its root. */
static int S_apply_tree_phase(markdown_core_parser *parser, markdown_core_node **root_slot, tree_phase_func phase,
                              void *context) {
    markdown_core_node *root = root_slot ? *root_slot : NULL;
    if (!root || parser->oom) {
        return !parser->oom;
    }
    tree_phase_context phase_context = {parser, phase, context};
    if (!markdown_core_visit_block_subtrees(root, apply_independent_phase, &phase_context)) {
        return 0;
    }
    return walk_owned_trees(parser, root_slot, NULL, NULL, phase, context, 0);
}

/* Register at syntax commitment; no completed-tree discovery pass is needed.
 * Inline bodies enter the document's value chain immediately, and the index
 * borrows only until finalization, before consolidation or postprocessing. */
bool markdown_core_parser_register_definition(markdown_core_parser *parser,
                                              markdown_core_definition_collection *collection,
                                              markdown_core_node *definition, markdown_core_node *citation,
                                              markdown_core_node **inline_owner) {
    assert(definition && collection);
    assert(citation ? inline_owner != NULL : inline_owner == NULL);
    assert(inline_owner ? !definition->parent : definition->parent != NULL);
    if (collection->count == collection->capacity) {
        size_t capacity = collection->capacity ? collection->capacity * 2 : 8;
        markdown_core_definition_entry *values;
        if (capacity > SIZE_MAX / sizeof(*values)) {
            parser->oom = true;
            return false;
        }
        values = parser->mem->realloc(collection->values, capacity * sizeof(*values));
        if (!values) {
            parser->oom = true;
            return false;
        }
        collection->values = values;
        collection->capacity = capacity;
    }
    collection->values[collection->count++] = (markdown_core_definition_entry){definition, citation};
    MARKDOWN_CORE_DIAGNOSTIC(parser->definition_registration_work++;)
    if (inline_owner) {
        definition->prev = collection->last_inline;
        if (collection->last_inline) {
            collection->last_inline->next = definition;
        } else {
            *inline_owner = definition;
            collection->first_inline = definition;
        }
        collection->last_inline = definition;
    }
    return true;
}

uint64_t markdown_core_source_key(const void *entry) {
    markdown_core_node *node;
    memcpy(&node, entry, sizeof(node));
    return ((uint64_t)(uint32_t)node->start_line << 32) | (uint32_t)node->start_column;
}

/* Stable ordering by a 64-bit source key, in as few byte passes as the keys
 * need. One read of every key first answers two questions about the keys
 * themselves, neither of them about how many there are: whether the entries
 * are ordered already (the case for keys produced in source order, and
 * nothing is moved then), and which byte positions differ between keys (a
 * position where every key agrees is an identity pass for a stable sort, so
 * it is skipped -- a key built from two grid coordinates leaves six of the
 * eight constant). What remains is the same least-significant-byte-first
 * radix sort over the bytes that differ, and its result is what all eight
 * passes would produce. */
int markdown_core_order_source_entries(markdown_core_mem *mem, void *entries, size_t count, size_t stride,
                                       uint64_t (*key)(const void *)) {
    if (count < 2) {
        return 1;
    }
    if (count > SIZE_MAX / stride) {
        return 0;
    }
    unsigned char *values = entries;
    uint64_t differ = 0, previous = key(values);
    bool ordered = true;
    for (size_t i = 1; i < count; i++) {
        uint64_t current = key(values + i * stride);
        differ |= previous ^ current;
        ordered = ordered && previous <= current;
        previous = current;
    }
    if (ordered) {
        return 1;
    }
    unsigned char *scratch = mem->realloc(NULL, count * stride);
    unsigned char *source = entries;
    unsigned char *target = scratch;
    if (!scratch) {
        return 0;
    }
    for (unsigned shift = 0; shift < 64; shift += 8) {
        if (!((differ >> shift) & 255)) {
            /* Every key agrees on this byte: the pass would move nothing. */
            continue;
        }
        size_t offsets[256] = {0};
        size_t offset = 0;
        for (size_t i = 0; i < count; i++) {
            offsets[(key(source + i * stride) >> shift) & 255]++;
        }
        for (size_t byte = 0; byte < 256; byte++) {
            size_t length = offsets[byte];
            offsets[byte] = offset;
            offset += length;
        }
        for (size_t i = 0; i < count; i++) {
            size_t destination = offsets[(key(source + i * stride) >> shift) & 255]++;
            memcpy(target + destination * stride, source + i * stride, stride);
        }
        unsigned char *swap = source;
        source = target;
        target = swap;
    }
    /* An odd number of passes leaves the order in the scratch. */
    if (source != entries) {
        memcpy(entries, source, count * stride);
    }
    mem->free(scratch);
    return 1;
}

int markdown_core_block_order_definitions(markdown_core_mem *mem, markdown_core_definition_collection *collection) {
    return markdown_core_order_source_entries(mem, collection->values, collection->count, sizeof(*collection->values),
                                              markdown_core_source_key);
}

void markdown_core_block_own_definitions(markdown_core_definition_collection *collection, markdown_core_node **slot) {
    markdown_core_node *last = NULL;
    *slot = NULL;
    for (size_t i = 0; i < collection->count; i++) {
        markdown_core_node *definition = collection->values[i].definition;
        markdown_core_node_unlink(definition);
        definition->prev = last;
        if (last) {
            last->next = definition;
        } else {
            *slot = definition;
        }
        last = definition;
    }
}

/* THE ONE FINISHING WALK. Text consolidation and every element's per-node
 * hook run at each node's EXIT of a single post-order pass over the document
 * definitions and the content tree, so the number of passes after inline
 * parsing does not depend on the attached elements. A Text first absorbs the
 * run that follows it and is released when it owns no bytes; the hooks then
 * see the run's survivor, exactly as the former whole-tree passes did. */
/* An element with a finish hook and the kinds it acts on as a bit per kind
 * (S_kind_bit); a hook whose kinds do not fit the mask is offered every
 * node, which is what declaring no kinds means. */
typedef struct {
    const markdown_core_element *element;
    uint64_t kinds;
} finisher;

typedef struct {
    markdown_core_parser *parser;
    finisher *finishers;
    size_t count;
} finishing_walk;

/* One bit per node kind: the block kinds count from 0 and the inline kinds
 * from 32, so the dialect's kinds all fit one word. */
static uint64_t S_kind_bit(markdown_core_node_type kind) {
    unsigned value = markdown_core_kind_value(kind);
    unsigned index = value + (MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind) ? 32u : 0u);
    return value < 32 && index < 64 ? (uint64_t)1 << index : 0;
}

static uint64_t S_finished_kinds(const markdown_core_node_type *kinds) {
    uint64_t mask = 0;
    if (!kinds) {
        return ~(uint64_t)0;
    }
    for (; *kinds != MARKDOWN_CORE_NODE_NONE; kinds++) {
        uint64_t bit = S_kind_bit(*kinds);
        if (!bit) {
            return ~(uint64_t)0;
        }
        mask |= bit;
    }
    return mask;
}

static markdown_core_node *S_finish_node(markdown_core_parser *parser, markdown_core_node *node, int claim_depth,
                                         void *context, markdown_core_node **successor) {
    const finishing_walk *walk = context;
    MARKDOWN_CORE_DIAGNOSTIC(parser->finishing_work++;)
    if (node->kind == MARKDOWN_CORE_NODE_TEXT) {
        if (node->next && node->next->kind == MARKDOWN_CORE_NODE_TEXT &&
            !markdown_core_consolidate_text_run(parser, node)) {
            parser->oom = true;
            return NULL;
        }
        /* The operands of a merged run are gone; the walk goes on after it. */
        *successor = node->next;
        if (node->as.literal->len == 0) {
            markdown_core_chunk_free(parser->mem, node->as.literal);
            markdown_core_node_recycle(parser->arena, node);
            return NULL;
        }
    }
    /* A hook answers the node now in the position, which may be of another
     * kind than the one it was offered; the hooks after it are chosen by
     * that node's kind, as they would see it. */
    uint64_t bit = S_kind_bit((markdown_core_node_type)node->kind);
    for (const finisher *hook = walk->finishers, *end = hook + walk->count; hook != end && node; hook++) {
        if (!(hook->kinds & bit)) {
            continue;
        }
        const markdown_core_element *element = hook->element;
        MARKDOWN_CORE_DIAGNOSTIC(parser->finisher_work++;)
        markdown_core_node *finished = element->finish_node(element, parser, node, claim_depth);
        if (parser->oom) {
            return NULL;
        }
        if (finished != node) {
            node = finished;
            if (node) {
                bit = S_kind_bit((markdown_core_node_type)node->kind);
            }
        }
    }
    return node;
}

static int finish_independent_tree(markdown_core_node **slot, void *context) {
    finishing_walk *walk = context;
    return walk_owned_trees(walk->parser, slot, NULL, S_finish_node, NULL, walk, 0);
}

static int S_finish_tree(markdown_core_parser *parser) {
    finishing_walk walk = {.parser = parser};
    for (size_t i = 0; i < parser->element_count; i++) {
        walk.count += parser->elements[i]->finish_node != NULL;
    }
    /* The table lives in the transaction's arena for the walk: no heap
     * allocation per parse, and its record returns to the arena's pool. */
    size_t table_size = walk.count * sizeof(*walk.finishers);
    if (walk.count) {
        walk.finishers = markdown_core_arena_take(parser->arena, table_size);
        if (!walk.finishers) {
            parser->oom = true;
            return 0;
        }
        for (size_t i = 0, at = 0; i < parser->element_count; i++) {
            const markdown_core_element *element = parser->elements[i];
            if (element->finish_node) {
                walk.finishers[at++] = (finisher){element, S_finished_kinds(element->finish_node_kinds)};
            }
        }
    }
    int ok = markdown_core_visit_block_subtrees(parser->root, finish_independent_tree, &walk) &&
             walk_owned_trees(parser, &parser->root, NULL, S_finish_node, NULL, &walk, 0);
    markdown_core_arena_recycle(parser->arena, walk.finishers, table_size);
    return ok && !parser->oom;
}

#if MARKDOWN_CORE_DEBUG_NODES
static int S_check_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    (void)parser;
    (void)context;
    return markdown_core_node_check(*root_slot, stderr) == 0;
}
#endif

static int S_postprocess_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    const markdown_core_element *element = (const markdown_core_element *)context;
    markdown_core_node *processed = element->postprocess_func(element, parser, *root_slot);
    if (processed) {
        *root_slot = processed;
    }
    return !parser->oom;
}

/* One reading of the installed phase clock at a sequence point; the clock is
 * dereferenced only once it is known to be installed. */
#define S_PHASE(parser, field)                                                                                         \
    do {                                                                                                               \
        if ((parser)->phase_clock) {                                                                                   \
            (parser)->phase_clock->field = (parser)->phase_clock->now((parser)->phase_clock->context);                 \
        }                                                                                                              \
    } while (0)

static markdown_core_node *S_finish_parse(markdown_core_parser *parser) {
    markdown_core_node *res;

    if (parser->root == NULL || parser->oom) {
        return NULL;
    }

    S_PHASE(parser, blocks);
    finalize_document(parser);
    S_parse_block_inputs(parser);
    S_complete_blocks(parser);
    if (!parser->oom) {
        parser->document_structure->prepare_document(parser);
    }
    S_PHASE(parser, prepared);
    if (!parser->oom) {
        process_inlines(parser, parser->refmap, NULL);
    }
    S_PHASE(parser, inlines);
    if (!parser->oom) {
        parser->document_structure->finish_document(parser);
    }
    if (parser->oom) {
        goto failed;
    }

    if (!S_finish_tree(parser)) {
        parser->oom = true;
        goto failed;
    }
    S_PHASE(parser, finished);

#if MARKDOWN_CORE_DEBUG_NODES
    if (!S_apply_tree_phase(parser, &parser->root, S_check_tree, NULL)) {
        abort();
    }
#endif

    /* Whole-tree postprocessors are a tooling hook; no built-in element
     * declares one, so an ordinary parse walks nothing here. */
    for (size_t i = 0; i < parser->element_count && !parser->oom; i++) {
        const markdown_core_element *element = parser->elements[i];
        if (element->postprocess_func) {
            if (!S_apply_tree_phase(parser, &parser->root, S_postprocess_tree, (void *)element)) {
                parser->oom = true;
            }
        }
    }
    if (parser->oom) {
        goto failed;
    }

    res = parser->root;
    parser->root = NULL;
    parser->arena = NULL;
    return res;

failed:
    parser->completion_count = 0;
    parser->document_structure->dispose_document(parser);
    parser->arena = NULL;
    markdown_core_node_free(parser->root);
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

int markdown_core_parser_get_last_line_length(markdown_core_parser *parser) { return parser->last_line_length; }

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    markdown_core_block_advance_offset(parser, &input_chunk, count, columns != 0);
}

void markdown_core_parser_set_backslash_ispunct_func(markdown_core_parser *parser, markdown_core_ispunct_func func) {
    parser->backslash_ispunct = func;
}
