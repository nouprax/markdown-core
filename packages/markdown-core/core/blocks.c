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
#include "extension.h"
#include "../extensions/markdown-core-extensions.h"
#include "../extensions/tasklist.h"
#include "../extensions/directive.h"
#include "../extensions/table.h"
#include "config.h"
#include "parser.h"
#include "markdown-core.h"
#include "node.h"
#include "references.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"
#include "iterator.h"

#define CODE_INDENT 4
#define TAB_STOP 4

/**
 * cmark-gfm limits nested footnote definitions to bound its extension path.
 * Lists are not capped: CommonMark permits arbitrary nesting and the core
 * traversal below carries cmark's linear-time blank-line optimization.
 */
#define MAX_FOOTNOTE_DEPTH 100

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

#define peek_at(i, n) (i)->data[n]

static void dispose_headings(markdown_core_parser *parser, markdown_core_heading_collection *headings);

static bool S_last_line_blank(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0;
}

static bool S_last_line_checked(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_CHECKED) != 0;
}

static MARKDOWN_CORE_INLINE markdown_core_node_type S_type(const markdown_core_node *node) {
    return (markdown_core_node_type)node->kind;
}

static void S_set_last_line_blank(markdown_core_node *node, bool is_blank) {
    if (is_blank) {
        node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    } else {
        node->flags &= ~MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    }
}

static void S_set_last_line_checked(markdown_core_node *node) { node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_CHECKED; }

static MARKDOWN_CORE_INLINE bool S_is_line_end_char(char c) { return (c == '\n' || c == '\r'); }

static MARKDOWN_CORE_INLINE bool S_is_space_or_tab(char c) { return (c == ' ' || c == '\t'); }

static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length);
static markdown_core_node *S_finish_parse(markdown_core_parser *parser);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);

static markdown_core_node *make_block(markdown_core_mem *mem, markdown_core_node_type tag, int start_line,
                                      int start_column) {
    markdown_core_node *e;

    e = markdown_core_node_new_with_mem(tag, mem);
    if (!e) {
        return NULL;
    }
    markdown_core_strbuf_grow(&e->content, 32);
    e->flags = MARKDOWN_CORE_NODE__OPEN;
    e->start_line = start_line;
    e->start_column = start_column;
    e->end_line = start_line;

    return e;
}

// Create a root document node.
static markdown_core_node *make_document(markdown_core_mem *mem) {
    markdown_core_node *e = make_block(mem, MARKDOWN_CORE_NODE_DOCUMENT, 1, 1);
    return e;
}

/* Both extension lists hold pointers to `static const` descriptors, and every
 * reader casts `data` straight back to a
 * `const markdown_core_extension *`. The const is discarded here and
 * nowhere else because markdown_core_llist is a generic list that cannot
 * carry it; typing the parameter keeps the cast to this one line. */
static int S_extension_list_append(markdown_core_mem *mem, markdown_core_llist **head,
                                   const markdown_core_extension *extension) {
    markdown_core_llist *node = (markdown_core_llist *)mem->calloc(1, sizeof(*node));
    markdown_core_llist *tail;
    if (!node) {
        return 0;
    }
    node->data = (void *)(uintptr_t)extension;
    node->next = NULL;
    if (!*head) {
        *head = node;
        return 1;
    }
    for (tail = *head; tail->next; tail = tail->next)
        ;
    tail->next = node;
    return 1;
}

int markdown_core_parser_attach_extension(markdown_core_parser *parser, const markdown_core_extension *extension) {
    if (!S_extension_list_append(parser->mem, &parser->extensions, extension)) {
        return 0;
    }
    if (extension->match_inline || extension->insert_inline_from_delim) {
        if (!S_extension_list_append(parser->mem, &parser->inline_extensions, extension)) {
            return 0;
        }
    }

    return 1;
}

static void S_parser_dispose(markdown_core_parser *parser) {
    dispose_headings(parser, &parser->headings);
    /* This index never owns nodes and is never read during destruction. */
    parser->mem->free(parser->footnotes.values);
    parser->mem->free(parser->specimens.values);
    parser->mem->free(parser->block_inputs);
    parser->mem->free(parser->input_line_offsets);
    markdown_core_key_index_free(&parser->specimen_ids);
    if (parser->root) {
        markdown_core_node_free(parser->root);
    }

    if (parser->refmap) {
        markdown_core_map_free(parser->refmap);
    }

    /* The definition set holds labels and no nodes, so freeing it here cannot
     * reach the tree -- which is the whole difference between it and the map
     * `process_footnotes` used to build (D11). */
    if (parser->footnote_defs) {
        markdown_core_map_free(parser->footnote_defs);
        parser->footnote_defs = NULL;
    }

    /* The content-to-source map outlives every block that indexes it and
     * nothing else does, so it is released here rather than with the node. */
    parser->mem->free(parser->line_marks);
    parser->line_marks = NULL;
    parser->line_marks_size = 0;
    parser->line_marks_alloc = 0;

    /* The block-start lookahead's chain and resume cache are parser state of
     * the same kind: indexed by open containers and source lines, owned by no
     * node, and dead with the parse. */
    parser->mem->free(parser->lookahead_chain);
    parser->mem->free(parser->lookahead_chain_flags);
    parser->mem->free(parser->lookahead_entries);
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
    markdown_core_strbuf_init(parser->mem, &parser->curline, 256);
    markdown_core_strbuf_init(parser->mem, &parser->line_scratch, 0);
    markdown_core_strbuf_init(parser->mem, &parser->lookahead_last_line, 0);

    document = make_document(parser->mem);
    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs = markdown_core_footnote_definition_map_new(parser->mem);
    parser->root = document;
    parser->block_root = document;
    parser->current = document;

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. */
    if (!parser->root || !parser->refmap || !parser->footnote_defs || parser->curline.oom || parser->line_scratch.oom ||
        parser->lookahead_last_line.oom || parser->root->content.oom) {
        parser->oom = true;
    }

    markdown_core_inlines_reset_special_chars(parser);
    return parser;
}

static void S_parser_free(markdown_core_parser *parser) {
    markdown_core_mem *mem;
    if (!parser) {
        return;
    }
    mem = parser->mem;
    S_parser_dispose(parser);
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->line_scratch);
    markdown_core_strbuf_free(&parser->lookahead_last_line);
    markdown_core_llist_free(parser->mem, parser->extensions);
    markdown_core_llist_free(parser->mem, parser->inline_extensions);
    mem->free(parser);
}

/* Registration is a block-construction fact, not a finished-tree search.
 * No postprocessor observes this collection or changes its borrowed nodes. */
static void register_heading(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_heading_collection *headings = &parser->headings;
    if (headings->count == headings->capacity) {
        size_t capacity = headings->capacity ? headings->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*headings->values)) {
            parser->oom = true;
            return;
        }
        void *values = parser->mem->realloc(headings->values, capacity * sizeof(*headings->values));
        if (!values) {
            parser->oom = true;
            return;
        }
        headings->values = values;
        headings->capacity = capacity;
    }
    headings->values[headings->count++] = (markdown_core_heading_parse){.node = node};
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b);

/* "This block ends on the line being processed", lifted out of `finalize` so
 * that the extension close path can say the same thing. The three kinds that
 * take it there — the document, a closed fenced code block, a setext heading —
 * are the ones whose last line IS the line in hand; every other block ended on
 * the line before. An extension container closing on its own fence is a fourth,
 * and `finalize` cannot know that from the type alone. */
static void S_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b) {
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
static bool is_blank(markdown_core_strbuf *s, bufsize_t offset) {
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

static MARKDOWN_CORE_INLINE bool extension_accepts_lines(markdown_core_node *node) {
    return node->extension && node->extension->accepts_lines_func &&
           node->extension->accepts_lines_func(node->extension, node) != 0;
}

static MARKDOWN_CORE_INLINE bool accepts_lines(markdown_core_node *node) {
    markdown_core_node_type block_type = S_type(node);

    if (extension_accepts_lines(node)) {
        return true;
    }

    return (block_type == MARKDOWN_CORE_NODE_PARAGRAPH || block_type == MARKDOWN_CORE_NODE_HEADING ||
            block_type == MARKDOWN_CORE_NODE_CODE_BLOCK);
}

static MARKDOWN_CORE_INLINE bool contains_inlines(markdown_core_node *node) {
    if (node->extension && node->extension->contains_inlines_func) {
        return node->extension->contains_inlines_func(node->extension, node) != 0;
    }

    return (node->kind == MARKDOWN_CORE_NODE_PARAGRAPH || node->kind == MARKDOWN_CORE_NODE_HEADING ||
            node->kind == MARKDOWN_CORE_NODE_TABLE_CAPTION);
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
    markdown_core_parser_append_source_marks(parser, node, parser->line_number, column, length, node->content.size);
}

static void add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
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

/* Find the immutable run containing an offset, shared by slice and lookup. */
static int S_content_mark_at(markdown_core_parser *parser, const markdown_core_node *node, bufsize_t offset) {
    int lo = node->content_mark, hi = lo + node->content_mark_count - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (parser->line_marks[mid].content_offset <= offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

/* A map slice is a view into parser-owned immutable runs. Neither the source
 * nor the slice owns the vector; both end with the parse transaction. */
int markdown_core_parser_adopt_content_marks(markdown_core_parser *parser, markdown_core_node *owner,
                                             markdown_core_node *node, bufsize_t from, bufsize_t length) {
    if (!parser || !owner || !node || !owner->content_mark_count || length <= 0) {
        return 0;
    }
    from += owner->content_mark_offset;
    int first = S_content_mark_at(parser, owner, from);
    int last = S_content_mark_at(parser, owner, from + length - 1);
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
    int first = S_content_mark_at(parser, owner, from);
    int last = S_content_mark_at(parser, owner, from + length - 1);
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
    if (!owner->content.size) {
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

/* Requirement 10: for any block with a content buffer and any byte offset
 * within it, name the source line and column of that byte.
 *
 * The answer is a projection of the block's mark run, not a counter anyone
 * maintains: find the slice the offset falls in and add the distance from its
 * start. Binary search, so a caller that asks once per inline node pays
 * log(lines in the block) rather than re-walking it. */
static int S_content_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t content_offset, bool end,
                           int *line, int *column) {
    const markdown_core_line_mark *mark;

    if (!parser || !node || node->content_mark_count <= 0 || content_offset < 0) {
        return 0;
    }

    content_offset += node->content_mark_offset;
    mark = &parser->line_marks[S_content_mark_at(parser, node, content_offset)];
    *line = mark->line;
    *column = mark->column + (int)(content_offset - mark->content_offset) * mark->source_step +
              (end ? mark->source_width - 1 : 0);
    return 1;
}

int markdown_core_parser_content_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                       int *line, int *column) {
    return S_content_place(parser, node, offset, false, line, column);
}

int markdown_core_parser_content_end_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                           int *line, int *column) {
    return S_content_place(parser, node, offset, true, line, column);
}

/* Drop `dropped` bytes off the FRONT of `node`'s content, leaving `remaining`
 * bytes, and keep the map describing what is left. The marks stay where they are in the vector: the
 * run's head moves past the slices that went away, and the slice the cut
 * landed inside keeps its line with its column advanced to the cut. */
static void S_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
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

static void remove_trailing_blank_lines(markdown_core_strbuf *ln) {
    bufsize_t i;
    unsigned char c;

    for (i = ln->size - 1; i >= 0; --i) {
        c = ln->ptr[i];

        if (c != ' ' && c != '\t' && !S_is_line_end_char(c)) {
            break;
        }
    }

    if (i < 0) {
        markdown_core_strbuf_clear(ln);
        return;
    }

    for (; i < ln->size; ++i) {
        c = ln->ptr[i];

        if (!S_is_line_end_char(c)) {
            continue;
        }

        markdown_core_strbuf_truncate(ln, i);
        break;
    }
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
static bool S_ends_with_blank_line(markdown_core_node *node) {
    markdown_core_node *last = node;
    while (!S_last_line_checked(last) &&
           (S_type(last) == MARKDOWN_CORE_NODE_LIST || S_type(last) == MARKDOWN_CORE_NODE_LIST_ITEM) &&
           last->last_child) {
        last = last->last_child;
    }
    bool blank = S_last_line_blank(last);
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

/* A definition has no semantic node (M2). A link reference definition read off the
 * front of `b`'s content goes into the parser's map, which owns the resource it
 * states once, and every reference that resolves to it is the `Link` or
 * `Media` it names, sharing that resource. This is the inherited grammar's
 * model: the bytes are consumed and remaining content is rebased onto where
 * it was written. An invalid definition stays paragraph text. A paragraph
 * consumed entirely by definitions retains its position in the block tree
 * until anchor decisions finish; semantic cleanup then removes it. */
// returns true if content remains after link defs are resolved.
static bool resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_strbuf *node_content = &b->content;
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0};
    markdown_core_attribute_parser attributes = {.mem = parser->mem, .data = chunk.data, .length = chunk.len};
    while (chunk.len && chunk.data[0] == '[') {
        int line = b->start_line, column = b->start_column;
        markdown_core_parser_content_place(parser, b, (bufsize_t)(chunk.data - node_content->ptr), &line, &column);
        uint64_t source_key = ((uint64_t)(uint32_t)line << 32) | (uint32_t)column;
        pos = markdown_core_parse_reference_inline(parser->mem, &chunk, parser->refmap, &attributes, source_key);
        if (!pos) {
            break;
        }
        chunk.data += pos;
        chunk.len -= pos;
    }
    if (attributes.oom) {
        parser->oom = true;
    }
    parser->attribute_work += attributes.work;
    markdown_core_attribute_parser_free(&attributes);
    // The definitions are dropped off the FRONT of the block's content, so what
    // is left starts further down the source than the block was told it did.
    // Without this a paragraph whose leading definitions were consumed keeps the
    // DEFINITION's position, and so does every inline in it, because
    // markdown_core_parse_inlines seeds the subject from b->start_line and
    // b->start_column.
    //
    // D18 corrected the LINE here by counting the line endings in the prefix
    // that goes away, and left the column alone with the note that it was
    // right wherever the remaining first line has the same stripped prefix as
    // the definition's line. Requirement 10 removes both the count and the
    // caveat: the map says where the surviving first byte was written, so the
    // column is answered rather than assumed, and the marks are rebased so the
    // inline phase reads the same map against the shortened buffer.
    bufsize_t dropped = node_content->size - chunk.len;
    int line, column;
    S_rebase_content_marks(parser, b, dropped, chunk.len);
    markdown_core_strbuf_drop(node_content, dropped);
    /* The block now begins where its FIRST SURVIVING line was written, and
     * that is asked of the map rather than derived: this function can be
     * reached twice on one paragraph -- once at the setext-underline check and
     * again at finalize -- and the first call can consume everything recorded
     * so far, leaving the line that carries what is left still unread. Taking
     * the answer from the surviving run rather than from the size of the cut
     * is what makes both arrivals give the same result. On a block with no
     * definitions in front of it this is what the block already said. */
    if (markdown_core_parser_content_place(parser, b, 0, &line, &column)) {
        b->start_line = line;
        b->start_column = column;
    }
    return !is_blank(&b->content, 0);
}

/* M0: an HTML block that opened with `<!--` and whose end line holds only
 * whitespace after the first `-->` is a block `Comment`, and its literal is
 * the bytes between `<!--` and that `-->`, line endings included. Every other
 * type-2 block -- `<!-- a --> b`, or one the input ended before a `-->` line --
 * stays an HTML block as written.
 *
 * The block's literal is its lines after container-prefix removal, so the
 * opener may sit behind up to three spaces of indentation and the block is
 * still the comment; `  <!-- x -->` is a comment whose literal is ` x `.
 *
 * The `-->` is searched from two bytes into the opener, so `<!-->` and
 * `<!--->` -- the two tokens the inherited grammar names as comments with
 * nothing inside -- find the closer overlapping the opener and give the empty
 * literal the inline rule gives them. The caller has established that the
 * block's own end condition closed it, which is what makes "the first `-->`"
 * the one on the end line: no earlier line held one, or the block would have
 * ended there. */
static void S_convert_comment_block(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_chunk *literal = &b->as.html_block->literal;
    unsigned char *data = literal->data;
    bufsize_t len = literal->len;
    bufsize_t open = 0;
    bufsize_t close;
    bufsize_t body_start;
    bufsize_t body_len;
    bufsize_t rest;

    while (open < len && (data[open] == ' ' || data[open] == '\t')) {
        open++;
    }
    if (len - open < 4 || memcmp(data + open, "<!--", 4) != 0) {
        return;
    }
    for (close = open + 2; close + 3 <= len; close++) {
        if (data[close] == '-' && data[close + 1] == '-' && data[close + 2] == '>') {
            break;
        }
    }
    if (close + 3 > len) {
        return;
    }
    rest = close + 3;
    while (rest < len && (data[rest] == ' ' || data[rest] == '\t')) {
        rest++;
    }
    if (rest < len && data[rest] == '\r') {
        rest++;
    }
    if (rest < len && data[rest] == '\n') {
        rest++;
    }
    if (rest != len) {
        return;
    }

    body_start = open + 4;
    body_len = close > body_start ? close - body_start : 0;
    /* Keep ownership of the HTML literal across the kind change. Restore it
     * on failure; on success the comment record takes it before trimming. */
    assert(literal->alloc);
    markdown_core_chunk owned_literal = *literal;
    *literal = (markdown_core_chunk)MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_node_set_kind_result result = markdown_core_node_set_kind(b, MARKDOWN_CORE_NODE_COMMENT_BLOCK);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        *literal = owned_literal;
        if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
            parser->oom = true;
        }
        return;
    }
    *b->as.literal = owned_literal;
    literal = b->as.literal;
    memmove(data, data + body_start, body_len);
    data[body_len] = '\0';
    literal->len = body_len;
}

/* One lexical candidate for every placement. The identifier borrows the input
 * until attachment copies it; content_end excludes separating whitespace.
 * Scan backwards once, so failed or adjacent candidates never rescan a suffix. */
typedef struct {
    markdown_core_chunk identifier;
    bufsize_t content_end;
    bool own_line;
} block_identifier;

static bool S_scan_block_identifier(markdown_core_parser *parser, const unsigned char *data, bufsize_t length,
                                    block_identifier *candidate) {
    bufsize_t end = length;
    while (end && S_is_line_end_char(data[end - 1])) {
        parser->block_identifier_work++;
        end--;
    }
    while (end && S_is_space_or_tab(data[end - 1])) {
        parser->block_identifier_work++;
        end--;
    }
    parser->block_identifier_work++;
    if (end < 3 || data[end - 1] != '#') {
        return false;
    }
    bufsize_t start = end - 1;
    while (start) {
        unsigned char c = data[start - 1];
        parser->block_identifier_work++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            break;
        }
        start--;
    }
    if (!start || start == end - 1 || data[start - 1] != '#') {
        return false;
    }
    candidate->identifier = (markdown_core_chunk){(unsigned char *)data + start, end - start - 1, 0};
    bufsize_t cut = start - 1;
    while (cut && S_is_space_or_tab(data[cut - 1])) {
        parser->block_identifier_work++;
        cut--;
    }
    candidate->own_line = !cut || S_is_line_end_char(data[cut - 1]);
    if (!candidate->own_line && cut == start - 1) {
        return false;
    }
    if (candidate->own_line && cut) {
        if (data[cut - 1] == '\n') {
            cut--;
        }
        if (cut && data[cut - 1] == '\r') {
            cut--;
        }
    }
    candidate->content_end = cut;
    return true;
}

/* Commit one owner-held value before deleting any source bytes. OOM leaves
 * both the owner and input intact and fails the enclosing parse transaction. */
static bool S_attach_block_identifier(markdown_core_parser *parser, markdown_core_node *owner,
                                      const block_identifier *candidate) {
    if (owner->attributes.anchor.len) {
        return false;
    }
    markdown_core_chunk identifier = candidate->identifier;
    if (!markdown_core_chunk_to_cstr(parser->mem, &identifier)) {
        parser->oom = true;
        return false;
    }
    markdown_core_chunk_free(parser->mem, &owner->attributes.anchor);
    owner->attributes.anchor = identifier;
    return true;
}

static void S_attach_paragraph_identifier(markdown_core_parser *parser, markdown_core_node *paragraph) {
    block_identifier candidate;
    if (!S_scan_block_identifier(parser, paragraph->content.ptr, paragraph->content.size, &candidate)) {
        return;
    }
    markdown_core_node *owner = paragraph;
    markdown_core_node *parent = paragraph->parent;
    int line, column;
    if (parent && S_type(parent) == MARKDOWN_CORE_NODE_LIST_ITEM && parent->first_child == paragraph &&
        markdown_core_parser_content_place(
            parser, paragraph, (bufsize_t)(candidate.identifier.data - paragraph->content.ptr), &line, &column) &&
        line == parent->start_line) {
        owner = parent;
    }
    bufsize_t at = (bufsize_t)(candidate.identifier.data - paragraph->content.ptr) + paragraph->content_mark_offset;
    int indent =
        paragraph->content_mark_count ? parser->line_marks[S_content_mark_at(parser, paragraph, at)].indent : 0;
    if (candidate.own_line && ((indent >= CODE_INDENT) || (!candidate.content_end && owner == paragraph))) {
        return;
    }
    if (S_attach_block_identifier(parser, owner, &candidate)) {
        markdown_core_strbuf_truncate(&paragraph->content, candidate.content_end);
    }
}

void markdown_core_parser_finalize_paragraph(markdown_core_parser *parser, markdown_core_node *paragraph) {
    if (!resolve_reference_link_definitions(parser, paragraph)) {
        paragraph->flags |= MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY;
        return;
    }
    S_attach_paragraph_identifier(parser, paragraph);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_node *parent;

    parent = b->parent;
    assert(b->flags & MARKDOWN_CORE_NODE__OPEN); // shouldn't call finalize on closed blocks
    b->flags &= ~MARKDOWN_CORE_NODE__OPEN;

    if (parser->curline.size == 0) {
        // end of input - line number has not been incremented
        b->end_line = parser->line_number;
        b->end_column = parser->last_line_length;
    } else if (S_type(b) == MARKDOWN_CORE_NODE_DOCUMENT ||
               (S_type(b) == MARKDOWN_CORE_NODE_CODE_BLOCK && b->as.code->fenced && b->as.code->fence_closed) ||
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
        S_set_end_to_current_line(parser, b);
    } else {
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }

    /* The extension's one chance to read its own block as a finished thing.
     * Placed after the scope is settled and before the switch, because what a
     * close hook has to say is about the whole block. */

    markdown_core_strbuf *node_content = &b->content;

    switch (S_type(b)) {
    case MARKDOWN_CORE_NODE_DEFINITION_BODY:
        if (!b->last_child) {
            b->end_line = b->start_line;
            b->end_column = b->internal_offset;
        }
        break;
    case MARKDOWN_CORE_NODE_HEADING:
        register_heading(parser, b);
        break;

    case MARKDOWN_CORE_NODE_PARAGRAPH:
        markdown_core_parser_finalize_paragraph(parser, b);
        break;

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        if (!b->as.code->fenced) { // indented code
            remove_trailing_blank_lines(node_content);
            markdown_core_strbuf_putc(node_content, '\n');
        } else {
            // first line of contents becomes info
            for (pos = 0; pos < node_content->size; ++pos) {
                if (S_is_line_end_char(node_content->ptr[pos])) {
                    break;
                }
            }
            assert(pos < node_content->size);

            markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT(parser->mem);
            bufsize_t info_end = pos, attribute_end;
            while (info_end > 0 && S_is_space_or_tab(node_content->ptr[info_end - 1])) {
                info_end--;
            }
            markdown_core_attribute_parser attributes = {.mem = parser->mem, .data = node_content->ptr, .length = pos};
            bufsize_t attribute_start = markdown_core_attributes_tail(&attributes, 0, info_end);
            if (attribute_start >= 0 &&
                markdown_core_attributes_parse(&attributes, attribute_start, &b->attributes, &attribute_end)) {
                info_end = attribute_start;
            }
            if (attributes.oom) {
                parser->oom = true;
            }
            parser->attribute_work += attributes.work;
            markdown_core_attribute_parser_free(&attributes);
            houdini_unescape_html_f(&tmp, node_content->ptr, info_end);
            markdown_core_strbuf_trim(&tmp);
            markdown_core_strbuf_unescape(&tmp);
            /* WHETHER THE SOURCE WROTE AN INFO STRING IS DECIDED HERE, ONCE.
             * A fence with nothing but whitespace after it wrote none, and
             * this is the only place that still knows the difference between
             * that and the `js` in ```` ```js ````. The facade used to decide
             * it again by testing the length, which is the fold requirement 14
             * forbids. */
            if (tmp.oom) {
                /* A buffer that could not be grown has `size == 0` and it is
                 * NOT an absent info string -- it is an info string the parse
                 * lost. The strict OOM sweep requires that loss to terminate
                 * the parse. */
                parser->oom = true;
                markdown_core_strbuf_free(&tmp);
                b->as.code->info = markdown_core_optional_chunk_absent();
            } else if (tmp.size == 0) {
                markdown_core_strbuf_free(&tmp);
                b->as.code->info = markdown_core_optional_chunk_absent();
            } else {
                markdown_core_chunk info = markdown_core_chunk_buf_detach(&tmp);
                if (!info.data) {
                    parser->oom = true;
                }
                b->as.code->info = markdown_core_optional_chunk_present(info);
            }

            if (node_content->ptr[pos] == '\r') {
                pos += 1;
            }
            if (node_content->ptr[pos] == '\n') {
                pos += 1;
            }
            markdown_core_strbuf_drop(node_content, pos);
        }
        b->as.code->literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.code->literal.data) {
            parser->oom = true;
        }
        break;

    case MARKDOWN_CORE_NODE_HTML_BLOCK: {
        int html_block_type = b->as.html_block->block_type;
        b->as.html_block->literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.html_block->literal.data) {
            parser->oom = true;
            break;
        }
        if (html_block_type == 2 && (b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0) {
            S_convert_comment_block(parser, b);
        }
        break;
    }

    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        /* O3: a `%%` block comment arrives here with its lines in `content`:
         * the opener line contributed nothing, because the extension that
         * opened it consumed the line, and the closer line is not there,
         * because its matcher closed the block before the line could be
         * added. The literal is those lines, indentation and line endings as
         * written after container-prefix removal. An HTML block comment never
         * takes this arm: it is finalized as the HTML block it was parsed as
         * and retyped above. */
        *b->as.literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.literal->data) {
            parser->oom = true;
        }
        break;

    default:
        break;
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
        parser->current = finalize(parser, parser->current);
        assert(parser->current);
    }
}

static markdown_core_node *S_parent_for_block(markdown_core_parser *parser, markdown_core_node *parent,
                                              markdown_core_node_type block_type) {
    assert(parent);

    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!markdown_core_node_can_contain_type(parent, block_type)) {
        parent = finalize(parser, parent);
    }
    return parent;
}

// Add a node as child of another.  Return pointer to child.
static markdown_core_node *add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                     markdown_core_node_type block_type, int start_column) {
    parent = S_parent_for_block(parser, parent, block_type);

    markdown_core_node *child =
        make_block(parser->mem, block_type, parser->line_number,
                   markdown_core_parser_source_column(parser, parser->line_number, start_column));
    if (!child || child->content.oom) {
        parser->oom = true;
        if (child) {
            markdown_core_node_free(child);
        }
        /* The loop above may have finalized blocks; keep the parser anchored
         * at a still-open ancestor so the finish path stays consistent. */
        parser->current = parent;
        return NULL;
    }
    child->parent = parent;

    if (parent->last_child) {
        parent->last_child->next = child;
        child->prev = parent->last_child;
    } else {
        parent->first_child = child;
        child->prev = NULL;
    }
    parent->last_child = child;
    return child;
}

/* Two of the three byte sets are folded into parser tables here; the third,
 * `dispatch`, is asked directly because it also answers ownership questions
 * that a merged table cannot. The two folds are now independent, which is the
 * whole point: before this, one list fed both tables and whether it fed the
 * second was a single `emphasis` bool covering every byte the extension named.
 * That is D1. */
void markdown_core_manage_extensions_special_characters(markdown_core_parser *parser, int add) {
    markdown_core_llist *tmp_ext;

    for (tmp_ext = parser->inline_extensions; tmp_ext; tmp_ext = tmp_ext->next) {
        const markdown_core_extension *ext = (const markdown_core_extension *)tmp_ext->data;
        const unsigned char *c;

        for (c = (const unsigned char *)ext->terminates_text; c && *c; c++) {
            if (add) {
                markdown_core_inlines_add_text_terminator(parser, *c);
            } else {
                markdown_core_inlines_remove_text_terminator(parser, *c);
            }
        }
        for (c = (const unsigned char *)ext->flanking_transparent; c && *c; c++) {
            if (add) {
                markdown_core_inlines_add_flanking_transparent(parser, *c);
            } else {
                markdown_core_inlines_remove_flanking_transparent(parser, *c);
            }
        }
    }
}

/* Registry keys borrow final node anchors until synthesis finishes. Suffix
 * cursors live in the index itself; no entry allocation or stable pointer is
 * needed across insertions. Inherited anchors are indexed by resource identity
 * first so a long definition is hashed once, regardless of occurrence count. */
typedef struct {
    markdown_core_key_index index;
    markdown_core_key_index resources;
} anchor_registry;

static markdown_core_key_index_slot *anchor_slot(markdown_core_parser *parser, anchor_registry *registry,
                                                 markdown_core_chunk key) {
    parser->anchor_work += (size_t)key.len + 1;
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&registry->index, key.data, key.len);
    if (!slot) {
        parser->oom = true;
    }
    return slot;
}

static void reserve_node_anchor(markdown_core_parser *parser, anchor_registry *registry, markdown_core_node *node) {
    const markdown_core_chunk *anchor = markdown_core_node_anchor_chunk(node);
    if (!anchor->len) {
        return;
    }
    parser->anchor_work++;
    if (anchor != &node->attributes.anchor) {
        const unsigned char *identity = (const unsigned char *)&node->as.link->resource;
        void *existing = NULL;
        if (!markdown_core_key_index_insert(&registry->resources, identity, sizeof(node->as.link->resource),
                                            node->as.link->resource, 0, &existing)) {
            parser->oom = true;
            return;
        }
        if (existing) {
            return;
        }
    }
    markdown_core_key_index_slot *slot = anchor_slot(parser, registry, *anchor);
    if (slot && !slot->key) {
        markdown_core_key_index_commit(&registry->index, slot, anchor->data);
        slot->value.counter = 1;
    }
}

/* Parse each source buffer once. Inline parsing completes fields at their
 * owning token; the structural walk therefore skips the emitted inline tree. */
static bool process_inline_tree(markdown_core_parser *parser, markdown_core_node *root, markdown_core_map *refmap) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;
    bool whitespace = false;

    if (!iter) {
        parser->oom = true;
        return false;
    }

    while (!parser->oom && (ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            if (contains_inlines(cur)) {
                if (cur->kind != MARKDOWN_CORE_NODE_HEADING) {
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

/* Core and extension fields participate in the same parser phases. The
 * private title root stays owned here from source capture through cleanup. */
int markdown_core_visit_inline_subtrees(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor,
                                        void *context) {
    if (S_type(node) == MARKDOWN_CORE_NODE_DEFINITION && node->as.definition->term &&
        !visitor(&node->as.definition->term, context)) {
        return 0;
    }
    if (S_type(node) == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->title &&
        !visitor(&node->as.callout->title, context)) {
        return 0;
    }
    if (node->kind == MARKDOWN_CORE_NODE_CITE) {
        for (markdown_core_node *item = node->as.cite->citations; item; item = item->next) {
            if ((item->as.citation->prefix && !visitor(&item->as.citation->prefix, context)) ||
                (item->as.citation->suffix && !visitor(&item->as.citation->suffix, context))) {
                return 0;
            }
        }
    }
    const markdown_core_extension *extension = node->extension;
    return !extension || !extension->visit_owned_subtrees_func ||
           extension->visit_owned_subtrees_func(extension, node, visitor, context);
}

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

typedef struct {
    markdown_core_node **slot;
    markdown_core_iter *iter;
    int script_depth;
} owned_tree_frame;

typedef struct {
    markdown_core_parser *parser;
    owned_tree_frame *frames;
    size_t count, capacity;
    int script_depth;
} owned_tree_walk;

static int push_owned_tree(markdown_core_node **slot, void *context) {
    owned_tree_walk *walk = context;
    if (!slot || !*slot || walk->parser->oom) {
        return !walk->parser->oom;
    }
    if (walk->count == walk->capacity) {
        size_t capacity = walk->capacity ? 2 * walk->capacity : 8;
        if (capacity > SIZE_MAX / sizeof(*walk->frames)) {
            walk->parser->oom = true;
            return 0;
        }
        void *frames = walk->parser->mem->realloc(walk->frames, capacity * sizeof(*walk->frames));
        if (!frames) {
            walk->parser->oom = true;
            return 0;
        }
        walk->frames = frames;
        walk->capacity = capacity;
    }
    walk->frames[walk->count++] = (owned_tree_frame){slot, NULL, walk->script_depth};
    return 1;
}

/* Every independent inline tree uses the same explicit continuation stack.
 * Field roots remain owned by their live slots until their children finish;
 * a completion phase may then replace that root. Depth never uses C frames. */
static int walk_owned_trees(markdown_core_parser *parser, markdown_core_node **slot, tree_node_func enter,
                            tree_phase_func finish, void *context, int script_depth) {
    owned_tree_walk walk = {.parser = parser, .script_depth = script_depth};
    push_owned_tree(slot, &walk);
    while (walk.count && !parser->oom) {
        owned_tree_frame *frame = &walk.frames[walk.count - 1];
        if (!frame->iter) {
            frame->iter = markdown_core_iter_new(*frame->slot);
            if (!frame->iter) {
                parser->oom = true;
                break;
            }
        }
        markdown_core_event_type event = markdown_core_iter_next(frame->iter);
        if (event == MARKDOWN_CORE_EVENT_DONE) {
            markdown_core_node **completed = frame->slot;
            markdown_core_iter_free(frame->iter);
            walk.count--;
            if (finish && !finish(parser, completed, context)) {
                parser->oom = true;
            }
            continue;
        }
        markdown_core_node *node = markdown_core_iter_get_node(frame->iter);
        if (node->kind == MARKDOWN_CORE_NODE_SUPERSCRIPT || node->kind == MARKDOWN_CORE_NODE_SUBSCRIPT) {
            frame->script_depth += event == MARKDOWN_CORE_EVENT_ENTER ? 1 : -1;
        }
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        walk.script_depth = frame->script_depth;
        if (enter) {
            enter(parser, node, frame->script_depth, context);
        }
        size_t first = walk.count;
        if (!markdown_core_visit_inline_subtrees(node, push_owned_tree, &walk)) {
            parser->oom = true;
        }
        /* The visitor reports fields in source order; a stack consumes their
         * reversed registration order. No field is visited or scanned twice. */
        for (size_t left = first, right = walk.count; left < right && left < --right; left++) {
            owned_tree_frame swap = walk.frames[left];
            walk.frames[left] = walk.frames[right];
            walk.frames[right] = swap;
        }
    }
    for (size_t i = 0; i < walk.count; i++) {
        if (walk.frames[i].iter) {
            markdown_core_iter_free(walk.frames[i].iter);
        }
    }
    parser->mem->free(walk.frames);
    return !parser->oom;
}

static void complete_inline_node(markdown_core_parser *parser, markdown_core_node *node, int script_depth,
                                 void *context) {
    if (node->flags & MARKDOWN_CORE_NODE__ESCAPED_SPACE) {
        if (script_depth > 0) {
            markdown_core_chunk_free(parser->mem, node->as.literal);
            *node->as.literal = markdown_core_chunk_literal("\xC2\xA0");
        }
        node->flags &= ~MARKDOWN_CORE_NODE__ESCAPED_SPACE;
    }
    if (context) {
        reserve_node_anchor(parser, context, node);
    }
}

static int process_inline_fields(markdown_core_parser *parser, markdown_core_node *root, anchor_registry *anchors,
                                 int script_depth) {
    return walk_owned_trees(parser, &root, complete_inline_node, NULL, anchors, script_depth);
}

// Parse each source buffer with its owned fields, then complete final owners.
static void process_inlines(markdown_core_parser *parser, markdown_core_map *refmap, anchor_registry *anchors) {
    process_inline_tree(parser, parser->root, refmap);
    if (!parser->oom) {
        process_inline_fields(parser, parser->root, anchors, 0);
    }

    for (markdown_core_node *note = parser->root->as.document->footnotes; note && !parser->oom; note = note->next) {
        process_inline_fields(parser, note, anchors, 0);
    }

    markdown_core_manage_extensions_special_characters(parser, false);
}

/* A specimen marker owns only this line. Labels use exact Unicode letters
 * and numbers, with a single '_' or '-' between non-empty alphanumeric runs. */
static bufsize_t parse_specimen_marker(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t pos,
                                       markdown_core_specimen_value *value) {
    bufsize_t begin = pos;
    *value = (markdown_core_specimen_value){0};
    parser->specimen_work++;
    if (peek_at(input, pos++) != '(') {
        return 0;
    }
    int digits = 0;
    while (digits < 9 && markdown_core_isdigit(peek_at(input, pos))) {
        parser->specimen_work++;
        value->start = value->start * 10 + input->data[pos++] - '0';
        digits++;
    }
    value->has_start = digits > 0;
    if ((digits && !value->start) || peek_at(input, pos++) != '@') {
        return 0;
    }
    bufsize_t label = pos;
    bool alnum = false;
    while (pos < input->len) {
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(input->data + pos, input->len - pos, &scalar);
        parser->specimen_work++;
        if (markdown_core_utf8proc_is_letter(scalar) || markdown_core_utf8proc_is_number(scalar)) {
            alnum = true;
        } else if ((scalar == '_' || scalar == '-') && alnum) {
            alnum = false;
        } else {
            break;
        }
        pos += width;
    }
    if ((pos != label && !alnum) || peek_at(input, pos) != ')' || !markdown_core_isspace(peek_at(input, pos + 1))) {
        return 0;
    }
    if (pos > label) {
        value->id = markdown_core_optional_chunk_present(markdown_core_chunk_dup(input, label, pos - label));
    }
    return pos + 1 - begin;
}

/* Read a numeral in its committed variant. The same bounded accumulation is
 * used for every Roman component; no input cardinality selects another path. */
static bool ordered_numeral(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t begin, bufsize_t end,
                            markdown_core_ordered_list_variant variant, int *value) {
    int number = 0;
    // An automatic marker has value 1 in every variant, including the
    // variant inherited from a preceding authored marker.
    if (end == begin + 1 && input->data[begin] == '#') {
        *value = 1;
        return true;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT) {
        return false;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA) {
        unsigned char first = variant.lowercased ? 'a' : 'A';
        if (end != begin + 1 || input->data[begin] < first || input->data[begin] > first + 25) {
            return false;
        }
        *value = input->data[begin] - first + 1;
        return true;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL) {
        if (end - begin > 9) {
            return false;
        }
        for (bufsize_t at = begin; at < end; at++) {
            parser->list_marker_work++;
            if (!markdown_core_isdigit(input->data[at])) {
                return false;
            }
            number = number * 10 + input->data[at] - '0';
        }
    } else {
        static const struct {
            const char *text;
            int value;
            bool repeat;
        } terms[] = {{"M", 1000, true}, {"CM", 900, false}, {"D", 500, false}, {"CD", 400, false}, {"C", 100, true},
                     {"XC", 90, false}, {"L", 50, false},   {"XL", 40, false}, {"X", 10, true},    {"IX", 9, false},
                     {"V", 5, false},   {"IV", 4, false},   {"I", 1, true}};
        bufsize_t at = begin;
        for (size_t term = 0; term < sizeof(terms) / sizeof(terms[0]); term++) {
            bufsize_t width = terms[term].text[1] ? 2 : 1;
            unsigned char offset = variant.lowercased ? 'a' - 'A' : 0;
            while (at + width <= end) {
                parser->list_marker_work++;
                if (input->data[at] != terms[term].text[0] + offset ||
                    (width == 2 && input->data[at + 1] != terms[term].text[1] + offset)) {
                    break;
                }
                if (number > 999999999 - terms[term].value) {
                    return false;
                }
                number += terms[term].value;
                at += width;
                if (!terms[term].repeat) {
                    break;
                }
            }
        }
        if (at != end) {
            return false;
        }
    }
    *value = number;
    return end > begin;
}

static bool list_facts_match(const markdown_core_list *list, const markdown_core_list *item) {
    return list->list_type == item->list_type && list->bullet_char == item->bullet_char &&
           list->variant.kind == item->variant.kind && list->variant.lowercased == item->variant.lowercased &&
           list->delimiter.kind == item->delimiter.kind && list->delimiter.closed == item->delimiter.closed;
}

/* Recognition is non-consuming and allocation-free. Only a complete marker
 * returns authored facts to the ordinary block ownership/padding operation. */
static bufsize_t parse_list_marker(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t pos,
                                   markdown_core_node *container, int first_column, markdown_core_list *data) {
    bufsize_t startpos = pos;
    unsigned char c = peek_at(input, pos);
    bool interrupts_paragraph = container->kind == MARKDOWN_CORE_NODE_PARAGRAPH;
    const markdown_core_list *committed = container->kind == MARKDOWN_CORE_NODE_LIST ? container->as.list : NULL;
    *data = (markdown_core_list){0};
    parser->list_marker_work++;
    if (c == '*' || c == '-' || c == '+') {
        data->list_type = MARKDOWN_CORE_BULLET_LIST;
        data->bullet_char = c;
        pos++;
    } else {
        bool closed = c == '(';
        pos += closed;
        bufsize_t begin = pos;
        c = peek_at(input, pos);
        if (c == '#') {
            pos++;
        } else {
            while (markdown_core_isalnum(peek_at(input, pos))) {
                parser->list_marker_work++;
                pos++;
            }
        }
        if (pos == begin) {
            return 0;
        }
        bufsize_t end = pos;
        unsigned char delim = peek_at(input, pos++);
        if (delim != ')' && (closed || delim != '.')) {
            return 0;
        }
        data->list_type = MARKDOWN_CORE_ORDERED_LIST;
        data->delimiter =
            (markdown_core_ordered_list_delimiter){delim == '.' ? MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
                                                                : MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS,
                                                   closed};
        data->variant.lowercased = c >= 'a' && c <= 'z';
        data->variant.kind = c == '#'                                   ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT
                             : markdown_core_isdigit(c)                 ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL
                             : end == begin + 1 && c != 'i' && c != 'I' ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA
                                                                        : MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN;
        if (committed && committed->list_type == MARKDOWN_CORE_ORDERED_LIST &&
            ordered_numeral(parser, input, begin, end, committed->variant, &data->start)) {
            data->variant = committed->variant;
        } else if (!ordered_numeral(parser, input, begin, end, data->variant, &data->start)) {
            return 0;
        }
        if (c == '#' && delim == '.' &&
            (!committed || committed->delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT)) {
            data->delimiter.kind = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT;
        }
        if (interrupts_paragraph && data->start != 1) {
            return 0;
        }
        if (!committed || !list_facts_match(committed, data)) {
            for (markdown_core_node *ancestor = container; ancestor; ancestor = ancestor->parent) {
                if ((ancestor->kind == MARKDOWN_CORE_NODE_LIST_ITEM || ancestor->kind == MARKDOWN_CORE_NODE_SPECIMEN) &&
                    data->start != 1) {
                    return 0;
                }
            }
        }
        if (end == begin + 1 && c >= 'A' && c <= 'Z' && delim == '.') {
            int column = first_column + (pos - startpos);
            int initial_column = column;
            bufsize_t at = pos;
            while (S_is_space_or_tab(peek_at(input, at))) {
                parser->list_marker_work++;
                column += input->data[at++] == '\t' ? 4 - column % 4 : 1;
                if (column - initial_column >= 2) {
                    break;
                }
            }
            if (column - initial_column < 2 && !S_is_line_end_char(peek_at(input, at))) {
                return 0;
            }
        }
    }
    if (!markdown_core_isspace(peek_at(input, pos))) {
        return 0;
    }
    if (interrupts_paragraph) {
        bufsize_t at = pos;
        while (S_is_space_or_tab(peek_at(input, at))) {
            parser->list_marker_work++;
            at++;
        }
        if (S_is_line_end_char(peek_at(input, at))) {
            return 0;
        }
    }
    return pos - startpos;
}

/* A captured table header must be eligible for the same block-start slot as
 * a streaming header. Reuse core marker grammars without opening any block. */
bool markdown_core_parser_table_header_allowed(markdown_core_parser *parser, markdown_core_node *parent,
                                               markdown_core_chunk *input, int first, int column, int indent) {
    markdown_core_list list;
    markdown_core_specimen_value specimen;
    return indent < 4 && peek_at(input, first) != '>' && !scan_atx_heading_start(input, first) &&
           !scan_open_code_fence(input, first) && !scan_html_block_start(input, first) &&
           !scan_html_block_start_7(input, first) && !scan_footnote_definition(input, first) &&
           !parse_specimen_marker(parser, input, first, &specimen) &&
           !parse_list_marker(parser, input, first, parent, column, &list);
}

/* List layout depends on semantic children, after definitions are removed.
 * Some openers close a list before its unmatched item finishes, so deriving
 * layout when that list's source scope closes observes incomplete children. */
static void S_finalize_list(markdown_core_node *list) {
    list->as.list->tight = true;
    for (markdown_core_node *item = list->first_child; item; item = item->next) {
        if (S_last_line_blank(item) && item->next) {
            list->as.list->tight = false;
            return;
        }
        for (markdown_core_node *child = item->first_child; child; child = child->next) {
            if ((item->next || child->next) && S_ends_with_blank_line(child)) {
                list->as.list->tight = false;
                return;
            }
        }
    }
}

/* Block syntax and all anchor decisions finish before definitions disappear.
 * Walk in postorder so list layout sees the cleaned children. Each tree edge
 * is followed at most once in each direction, with no recursion or extra allocation;
 * the document owns every pending definition even if parsing fails earlier. */
static void S_complete_block_tree(markdown_core_node *root) {
    markdown_core_node *node = root;
    while (node->first_child) {
        node = node->first_child;
    }
    while (node) {
        markdown_core_node *parent = node->parent;
        markdown_core_node *next = node->next;
        if (node->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY) {
            markdown_core_node_free(node);
        } else if (S_type(node) == MARKDOWN_CORE_NODE_LIST) {
            S_finalize_list(node);
        } else if ((node->kind == MARKDOWN_CORE_NODE_DEFINITION_LIST || node->kind == MARKDOWN_CORE_NODE_DEFINITION ||
                    node->kind == MARKDOWN_CORE_NODE_DEFINITION_BODY) &&
                   node->last_child) {
            node->end_line = node->last_child->end_line;
            node->end_column = node->last_child->end_column;
        }
        node = next ? next : parent;
        if (next) {
            while (node->first_child) {
                node = node->first_child;
            }
        }
    }
}

static markdown_core_node *finalize_document(markdown_core_parser *parser) {
    while (parser->current != parser->root) {
        parser->current = finalize(parser, parser->current);
    }

    finalize(parser, parser->root);

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
        for (bufsize_t offset = 0; offset < owner->content.size && !parser->oom;) {
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
            while (offset < owner->content.size && !S_is_line_end_char(owner->content.ptr[offset])) {
                offset++;
            }
            if (offset < owner->content.size && owner->content.ptr[offset] == '\r') {
                offset++;
            }
            if (offset < owner->content.size && owner->content.ptr[offset] == '\n') {
                offset++;
            }
        }
        owner->flags |= MARKDOWN_CORE_NODE__OPEN;
        S_parse_source(parser, owner->content.ptr, (size_t)owner->content.size);
        while (parser->current != owner && !parser->oom) {
            parser->current = finalize(parser, parser->current);
        }
        owner->flags &= ~MARKDOWN_CORE_NODE__OPEN;
        markdown_core_strbuf_clear(&owner->content);
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
    if (!markdown_core_core_extensions_attach(parser) || (setup && !setup(parser, context))) {
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
    size_t metadata_length =
        parser->block_root == parser->root ? markdown_core_metadata_parse(parser, source, length) : 0;
    const unsigned char *cursor = source + metadata_length;
    const unsigned char *end = source + length;
    static const uint8_t repl[] = {239, 191, 189};

    while (cursor < end && !parser->oom) {
        const unsigned char *eol;
        bufsize_t segment_length;
        bool line_complete;

        for (eol = cursor; eol < end; ++eol) {
            if (S_is_line_end_char(*eol) || *eol == '\0') {
                break;
            }
        }
        line_complete = eol == end || S_is_line_end_char(*eol);
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
                S_process_line(parser, parser->line_scratch.ptr, parser->line_scratch.size);
                markdown_core_strbuf_clear(&parser->line_scratch);
            } else {
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
static int S_scan_thematic_break(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t offset) {
    bufsize_t i;
    char c;
    char nextc = '\0';
    int count;
    i = offset;
    c = peek_at(input, i);
    if (!(c == '*' || c == '_' || c == '-')) {
        parser->thematic_break_kill_pos = i;
        return 0;
    }
    count = 1;
    while ((nextc = peek_at(input, ++i))) {
        if (nextc == c) {
            count++;
        } else if (nextc != ' ' && nextc != '\t') {
            break;
        }
    }
    if (count >= 3 && (nextc == '\r' || nextc == '\n')) {
        return (i - offset) + 1;
    } else {
        parser->thematic_break_kill_pos = i;
        return 0;
    }
}

// Find first nonspace character from current offset, setting
// parser->first_nonspace, parser->first_nonspace_column,
// parser->indent, and parser->blank. Does not advance parser->offset.
static void S_find_first_nonspace(markdown_core_parser *parser, markdown_core_chunk *input) {
    char c;
    int chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);

    if (parser->first_nonspace <= parser->offset) {
        parser->first_nonspace = parser->offset;
        parser->first_nonspace_column = parser->column;
        while ((c = peek_at(input, parser->first_nonspace))) {
            if (c == ' ') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += 1;
                chars_to_tab = chars_to_tab - 1;
                if (chars_to_tab == 0) {
                    chars_to_tab = TAB_STOP;
                }
            } else if (c == '\t') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += chars_to_tab;
                chars_to_tab = TAB_STOP;
            } else {
                break;
            }
        }
    }

    parser->indent = parser->first_nonspace_column - parser->column;
    parser->blank = S_is_line_end_char(peek_at(input, parser->first_nonspace));
}

// Advance parser->offset and parser->column.  parser->offset is the
// byte position in input; parser->column is a virtual column number
// that counts each Unicode scalar once and expands tabs to tab stops.
// Source positions remain byte-based. The count parameter indicates
// how far to advance the offset.  If columns is true, then count
// indicates a number of columns; otherwise, a number of bytes.
// If advancing a certain number of columns partially consumes
// a tab character, parser->partially_consumed_tab is set to true.
static void S_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count, bool columns) {
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

static bool parse_callout_prefix(markdown_core_parser *parser, markdown_core_chunk *input) {
    bool res = false;
    bufsize_t matched = 0;

    matched = parser->indent <= 3 && peek_at(input, parser->first_nonspace) == '>';
    if (matched) {

        S_advance_offset(parser, input, parser->indent + 1, true);

        if (S_is_space_or_tab(peek_at(input, parser->offset))) {
            S_advance_offset(parser, input, 1, true);
        }

        res = true;
    }
    return res;
}

static bool parse_indented_container_prefix(markdown_core_parser *parser, markdown_core_chunk *input, int continuation,
                                            bool accepts_blank) {
    bool res = false;

    if (parser->indent >= continuation) {
        S_advance_offset(parser, input, continuation, true);
        res = true;
    } else if (parser->blank && accepts_blank) {
        // Blankness is relative to the cursor after ancestor prefixes. Lists
        // require a first block; definition-list bodies first check whether
        // the blank run leads to a carried line. Footnotes and specimens
        // accept blank continuation directly, including during lookahead.
        S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
        res = true;
    }
    return res;
}

static bool parse_code_block_prefix(markdown_core_parser *parser, markdown_core_chunk *input,
                                    markdown_core_node *container, bool *should_continue) {
    bool res = false;

    if (!container->as.code->fenced) { // indented
        if (parser->indent >= CODE_INDENT) {
            S_advance_offset(parser, input, CODE_INDENT, true);
            res = true;
        } else if (parser->blank) {
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            res = true;
        }
    } else { // fenced
        bufsize_t matched = 0;

        if (parser->indent <= 3 && (peek_at(input, parser->first_nonspace) == container->as.code->fence_char)) {
            matched = scan_close_code_fence(input, parser->first_nonspace);
        }

        if (matched >= container->as.code->fence_length) {
            // closing fence - and since we're at
            // the end of a line, we can stop processing it:
            *should_continue = false;
            container->as.code->fence_closed = true;
            S_advance_offset(parser, input, matched, false);
            parser->current = finalize(parser, container);
        } else {
            // skip opt. spaces of fence parser->offset
            int i = container->as.code->fence_offset;

            while (i > 0 && S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
                i--;
            }
            res = true;
        }
    }

    return res;
}

static bool parse_html_block_prefix(markdown_core_parser *parser, markdown_core_node *container) {
    bool res = false;
    int html_block_type = container->as.html_block->block_type;

    assert(html_block_type >= 1 && html_block_type <= 7);
    switch (html_block_type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        // these types of blocks can accept blanks
        res = true;
        break;
    case 6:
    case 7:
        res = !parser->blank;
        break;
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
    switch (S_type(container)) {
    case MARKDOWN_CORE_NODE_CALLOUT:
        return parse_callout_prefix(parser, input);
    case MARKDOWN_CORE_NODE_LIST:
        if (parser->blank) {
            if ((container->flags & MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK) && parser->indent == 0) {
                *taken = true;
                return true;
            }
            container->flags |= MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
        } else {
            container->flags &= ~MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
        }
        return true;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return parse_indented_container_prefix(parser, input,
                                               container->as.list->marker_offset + container->as.list->padding,
                                               container->first_child != NULL || joining == container);
    case MARKDOWN_CORE_NODE_DEFINITION_BODY:
        return parse_indented_container_prefix(parser, input, container->as.definition_body->continuation, true);
    case MARKDOWN_CORE_NODE_FOOTNOTE:
    case MARKDOWN_CORE_NODE_SPECIMEN:
        return parse_indented_container_prefix(parser, input, 4, true);
    default:
        return true;
    }
}

static bool parse_extension_block(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_chunk *input, bool *should_continue, markdown_core_node **closing) {
    int matched;

    if (!container->extension->last_block_matches) {
        return false;
    }

    matched =
        container->extension->last_block_matches(container->extension, parser, input->data, input->len, container);
    if (matched && container->kind == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        *closing = matched == MARKDOWN_CORE_BLOCK_PENDING_CLOSE ? container : NULL;
    } else if (matched && extension_accepts_lines(container)) {
        *closing = NULL;
    }
    if (matched != MARKDOWN_CORE_BLOCK_CLOSED) {
        return matched != 0;
    }

    /* The container's own closing line. Everything still open inside it ended
     * on the line before, and the container ends here.
     *
     * `parser->current` is the deepest open block and `container` is on the
     * path from the root to it, so walking up through `finalize` reaches it.
     * Definition-only paragraphs stay attached until block parsing completes;
     * no block can disappear while it is still on the open spine. */
    *should_continue = false;
    while (parser->current != container) {
        parser->current = finalize(parser, parser->current);
        assert(parser->current != NULL);
    }
    /* A block survives its own finalization and can still be positioned. */
    assert(S_type(container) != MARKDOWN_CORE_NODE_PARAGRAPH);
    parser->current = finalize(parser, container);
    S_set_end_to_current_line(parser, container);
    return false;
}

/**
 * For each containing node, try to parse the associated line start.
 *
 * Will not close unmatched blocks, as we may have a lazy continuation
 * line -> http://spec.commonmark.org/0.24/#lazy-continuation-line
 *
 * Returns: The last matching node, or NULL
 */
/* A body's trailing blank run belongs only when a later indented line
 * continues it. Decide before closing children, so their scopes stop at the
 * same source boundary. Cache the accepted run, not a second parsing path. */
static bool definition_body_blank_continues(markdown_core_parser *parser, markdown_core_node *body) {
    parser->definition_list_work++;
    if (body->as.definition_body->continuation_line > parser->line_number) {
        return true;
    }
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, body->parent, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
        return false;
    }
    markdown_core_chunk next;
    int first, indent, blanks;
    bool continues = markdown_core_parser_lookahead_next(&lookahead, &next, &first, &indent, &blanks) &&
                     indent >= body->as.definition_body->continuation;
    if (continues) {
        body->as.definition_body->continuation_line = lookahead.line - 1;
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return continues;
}

static markdown_core_node *check_open_blocks(markdown_core_parser *parser, markdown_core_chunk *input,
                                             bool *all_matched) {
    bool should_continue = true;
    *all_matched = false;
    markdown_core_node *container = parser->block_root;
    markdown_core_node *closing = NULL;
    markdown_core_node_type cont_type;

    while (S_last_child_is_open(container)) {
        container = container->last_child;
        cont_type = S_type(container);

        S_find_first_nonspace(parser, input);

        if (container->extension) {
            if (!parse_extension_block(parser, container, input, &should_continue, &closing)) {
                goto done;
            }
            continue;
        }

        switch (cont_type) {
        case MARKDOWN_CORE_NODE_CODE_BLOCK:
            if (!parse_code_block_prefix(parser, input, container, &should_continue)) {
                goto done;
            }
            closing = NULL;
            break;
        case MARKDOWN_CORE_NODE_HEADING:
            // a heading can never contain more than one line
            goto done;
        case MARKDOWN_CORE_NODE_HTML_BLOCK:
            if (!parse_html_block_prefix(parser, container)) {
                goto done;
            }
            closing = NULL;
            break;
        case MARKDOWN_CORE_NODE_PARAGRAPH:
            if (parser->blank) {
                goto done;
            }
            break;
        default: {
            bool taken = false;
            if (cont_type == MARKDOWN_CORE_NODE_DEFINITION_BODY && parser->blank &&
                !definition_body_blank_continues(parser, container)) {
                goto done;
            }
            if (!S_container_prefix_matches(parser, container, input, NULL, &taken)) {
                goto done;
            }
            /* A second consecutive blank line inside a deeply nested list
             * cannot open a block and all closable descendants were already
             * closed by the first. Returning NULL avoids walking the same
             * nesting spine for every remaining blank line. Raw-line leaves
             * still own the blank line, including extension-provided leaves. */
            if (taken) {
                if (S_type(parser->current) == MARKDOWN_CORE_NODE_CODE_BLOCK ||
                    S_type(parser->current) == MARKDOWN_CORE_NODE_HTML_BLOCK ||
                    extension_accepts_lines(parser->current)) {
                    add_line(parser->current, input, parser);
                }
                return NULL;
            }
            break;
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
            parser->current = finalize(parser, parser->current);
        }
        parser->current = finalize(parser, closing);
        S_set_end_to_current_line(parser, closing);
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
 * saved and restored around the lookahead. A container an extension owns is
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
        markdown_core_node_type type = S_type(parser->lookahead_chain[i]);
        if (type != MARKDOWN_CORE_NODE_LIST && type != MARKDOWN_CORE_NODE_LIST_ITEM) {
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
     * where `add_child` backs up to when it is opened. */
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
        bufsize_t resumed_offset;
        bool carried = true;
        bool closing = false;
        bool taken = false;
        bool resumed = false;
        bool blank;
        int i;

        while (eol < end && !S_is_line_end_char((char)*eol)) {
            eol++;
        }
        next = eol;
        if (next < end && *next == '\r') {
            next++;
        }
        if (next < end && *next == '\n') {
            next++;
        }
        parser->block_lookahead_work++;
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
                parser->first_nonspace = parser->offset;
                parser->first_nonspace_column = parser->column;
            }
        }
        resumed_offset = parser->offset;
        for (i = from; i < lookahead->depth && carried && !taken; i++) {
            markdown_core_node *container = parser->lookahead_chain[i];
            S_find_first_nonspace(parser, &input);
            if (container->extension) {
                int match = container->extension->continues_block
                                ? container->extension->continues_block(container->extension, parser, input.data,
                                                                        (int)input.len, container)
                                : 0;
                carried = match != 0;
                if (carried && container->kind == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
                    closing = match == MARKDOWN_CORE_BLOCK_PENDING_CLOSE;
                }
            } else {
                carried = S_container_prefix_matches(parser, container, &input, lookahead->parent, &taken);
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
            S_find_first_nonspace(parser, &input);
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
        node->flags = (markdown_core_node_internal_flags)((node->flags & ~MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK) |
                                                          (parser->lookahead_chain_flags[i] &
                                                           MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK));
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

/* Called exactly once, when the quote prefix opens its container. Recognition
 * scans only this line and commits before any body block can claim its bytes.
 * No body paragraph exists until a later line actually supplies body text. */
static bool S_parse_callout_metadata(markdown_core_parser *parser, markdown_core_node *node,
                                     markdown_core_chunk *input) {
    bufsize_t pos = parser->offset;
    bufsize_t begin = pos;
    while (pos < input->len && input->data[pos] == ' ' && pos - begin < 3) {
        pos++;
        parser->callout_scan_work++;
    }
    parser->callout_scan_work++;
    if (parser->partially_consumed_tab || pos + 2 >= input->len || input->data[pos] != '[' ||
        input->data[pos + 1] != '!') {
        return false;
    }
    pos += 2;
    begin = pos;
    while (pos < input->len) {
        unsigned char c = input->data[pos];
        parser->callout_scan_work++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            break;
        }
        pos++;
    }
    if (pos == begin || pos >= input->len || input->data[pos] != ']') {
        return false;
    }
    markdown_core_chunk variant = {input->data + begin, pos - begin, 0};
    pos++;
    bool has_fold = pos < input->len && (input->data[pos] == '+' || input->data[pos] == '-');
    bool collapsed = has_fold && input->data[pos] == '-';
    pos += has_fold;
    if (pos < input->len && !S_is_space_or_tab(input->data[pos]) && !S_is_line_end_char(input->data[pos])) {
        return false;
    }
    while (pos < input->len && S_is_space_or_tab(input->data[pos])) {
        pos++;
        parser->callout_scan_work++;
    }
    bufsize_t end = input->len;
    while (end > pos && (S_is_space_or_tab(input->data[end - 1]) || S_is_line_end_char(input->data[end - 1]))) {
        end--;
        parser->callout_scan_work++;
    }
    if (!markdown_core_chunk_to_cstr(parser->mem, &variant)) {
        parser->oom = true;
        return true;
    }
    node->as.callout->variant = markdown_core_optional_chunk_present(variant);
    node->as.callout->collapsed = (markdown_core_optional_bool){has_fold, collapsed};
    if (end > pos) {
        markdown_core_node *title = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, parser->mem);
        if (!title) {
            parser->oom = true;
            return true;
        }
        node->as.callout->title = title;
        title->start_line = title->end_line = parser->line_number;
        title->start_column = markdown_core_parser_source_column(parser, parser->line_number, pos + 1);
        title->end_column = markdown_core_parser_source_column(parser, parser->line_number, end);
        markdown_core_strbuf_put(&title->content, input->data + pos, end - pos);
        if (title->content.oom || !markdown_core_parser_append_source_marks(parser, title, parser->line_number, pos + 1,
                                                                            title->content.size, 0)) {
            parser->oom = true;
        }
    }
    S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    return true;
}

static int consume_item_marker(markdown_core_parser *parser, markdown_core_chunk *input, int marker_width) {
    S_advance_offset(parser, input, parser->first_nonspace + marker_width - parser->offset, false);
    int offset = parser->offset, column = parser->column;
    bool partial = parser->partially_consumed_tab;
    while (parser->column - column < 5 && S_is_space_or_tab(peek_at(input, parser->offset))) {
        S_advance_offset(parser, input, 1, true);
    }
    int padding = parser->column - column;
    if (padding < 1 || padding >= 5 || S_is_line_end_char(peek_at(input, parser->offset))) {
        parser->offset = offset;
        parser->column = column;
        parser->partially_consumed_tab = partial;
        if (padding > 0) {
            S_advance_offset(parser, input, 1, true);
        }
        padding = 1;
    }
    return marker_width + padding;
}

static bool definition_marker(markdown_core_chunk *input, int at, int indent) {
    return indent < 4 && at + 1 < input->len && (input->data[at] == ':' || input->data[at] == '~') &&
           (S_is_space_or_tab(input->data[at + 1]) || S_is_line_end_char(input->data[at + 1]));
}

/* Only paragraph fallback asks this question. The shared lookahead carries
 * container prefixes, reads one optional gap and one marker, and restores the
 * cursor; committed terms are never visited again as candidates. */
static bool definition_prefix(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_chunk *input,
                              bool *compact) {
    parser->definition_list_work++;
    if (parser->blank || parser->indent >= 4 || definition_marker(input, parser->first_nonspace, parser->indent)) {
        return false;
    }
    markdown_core_chunk term = {input->data + parser->first_nonspace, input->len - parser->first_nonspace, 0};
    if (term.data[0] == '[') {
        parser->definition_list_work += term.len;
        markdown_core_attribute_parser attributes = {.mem = parser->mem, .data = term.data, .length = term.len};
        bool reference = markdown_core_parse_reference_inline(parser->mem, &term, NULL, &attributes, 0) != 0;
        parser->attribute_work += attributes.work;
        parser->oom |= attributes.oom;
        markdown_core_attribute_parser_free(&attributes);
        if (reference || parser->oom) {
            return false;
        }
    }
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, &lookahead)) {
        return false;
    }
    markdown_core_chunk next;
    int first, indent, blanks;
    bool matched = markdown_core_parser_lookahead_next(&lookahead, &next, &first, &indent, &blanks) && blanks <= 1 &&
                   definition_marker(&next, first, indent);
    if (matched && next.data[first] == ':') {
        matched = !markdown_core_table_caption_probe(&lookahead, &next, first, indent);
    }
    if (matched) {
        *compact = blanks == 0;
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return matched;
}

static markdown_core_node *open_definition(markdown_core_parser *parser, markdown_core_node *parent,
                                           markdown_core_chunk *input, bool compact) {
    /* A new term requires the separating blank run. If the preceding body's
     * prefix declined it, append at the existing list's definition boundary. */
    if (parent->kind == MARKDOWN_CORE_NODE_DEFINITION) {
        parent = finalize(parser, parent);
    }
    if (parent->kind != MARKDOWN_CORE_NODE_DEFINITION_LIST) {
        parent = add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, parser->first_nonspace + 1);
        if (!parent) {
            return NULL;
        }
    }
    markdown_core_node *definition =
        add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION, parser->first_nonspace + 1);
    if (!definition) {
        return NULL;
    }
    definition->as.definition->compact = compact;
    markdown_core_node *term = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, parser->mem);
    if (!term) {
        parser->oom = true;
        return definition;
    }
    definition->as.definition->term = term;
    int begin = parser->first_nonspace, end = input->len;
    while (end > begin && (S_is_space_or_tab(input->data[end - 1]) || S_is_line_end_char(input->data[end - 1]))) {
        end--;
    }
    parser->definition_list_work += end - begin;
    term->start_line = term->end_line = parser->line_number;
    term->start_column = markdown_core_parser_source_column(parser, parser->line_number, begin + 1);
    term->end_column = markdown_core_parser_source_column(parser, parser->line_number, end);
    markdown_core_strbuf_put(&term->content, input->data + begin, end - begin);
    if (term->content.oom || !markdown_core_parser_append_source_marks(parser, term, parser->line_number, begin + 1,
                                                                       term->content.size, 0)) {
        parser->oom = true;
    }
    S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    return definition;
}

static void open_new_blocks(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                            bool all_matched) {
    bool indented;
    markdown_core_node *candidate_table;
    markdown_core_specimen_value specimen;
    markdown_core_list marker;
    markdown_core_list *data = &marker;
    bool maybe_lazy = S_type(parser->current) == MARKDOWN_CORE_NODE_PARAGRAPH;
    markdown_core_node_type cont_type = S_type(*container);
    bufsize_t matched = 0;
    int lev = 0;
    bool has_content;
    size_t depth = 0;

    while (cont_type != MARKDOWN_CORE_NODE_CODE_BLOCK && cont_type != MARKDOWN_CORE_NODE_HTML_BLOCK &&
           !extension_accepts_lines(*container)) {
        depth++;
        S_find_first_nonspace(parser, input);
        /* Indentation ahead of whatever opens here is the CONTAINER's, not the
         * new block's: a block begins at its own first non-space byte, so
         * giving the spaces to the block being opened would make its first
         * region start before its own scope. Measured before it was fixed --
         * 52 rows of an indented code block's four spaces alone. */
        indented = parser->indent >= CODE_INDENT;

        if (cont_type == MARKDOWN_CORE_NODE_DEFINITION &&
            definition_marker(input, parser->first_nonspace, parser->indent)) {
            int continuation = parser->indent + consume_item_marker(parser, input, 1);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_DEFINITION_BODY, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            (*container)->as.definition_body->continuation = continuation;
            (*container)->internal_offset =
                markdown_core_parser_source_column(parser, parser->line_number, input->len - 1);
        } else if (!indented && peek_at(input, parser->first_nonspace) == '>') {

            bufsize_t blockquote_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + 1 - parser->offset, false);
            // optional following character
            if (S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
            }
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_CALLOUT, blockquote_startpos + 1);
            if (!*container) {
                return;
            }

            if (S_parse_callout_metadata(parser, *container, input)) {
                return;
            }

        } else if (!indented && (matched = scan_atx_heading_start(input, parser->first_nonspace))) {
            bufsize_t hashpos;
            int level = 0;
            bufsize_t heading_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
            if (!*container) {
                return;
            }

            hashpos = markdown_core_chunk_strchr(input, '#', parser->first_nonspace);

            while (peek_at(input, hashpos) == '#') {
                level++;
                hashpos++;
            }

            (*container)->as.heading->level = level;
            (*container)->as.heading->setext = false;
            (*container)->internal_offset = matched;

        } else if (!indented && (matched = scan_open_code_fence(input, parser->first_nonspace))) {
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            (*container)->as.code->fenced = true;
            (*container)->as.code->fence_char = peek_at(input, parser->first_nonspace);
            (*container)->as.code->fence_length = (matched > 255) ? 255 : (uint8_t)matched;
            (*container)->as.code->fence_offset = (int8_t)(parser->first_nonspace - parser->offset);
            (*container)->as.code->fence_closed = false;
            /* Nothing is known about an info string until the fence line is
             * read; ABSENT is the honest state, and the close either replaces
             * it or leaves it. It used to open as an empty STRING, which said
             * the source had written one. */
            (*container)->as.code->info = markdown_core_optional_chunk_absent();
            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);

        } else if (!indented && ((matched = scan_html_block_start(input, parser->first_nonspace)) ||
                                 (cont_type != MARKDOWN_CORE_NODE_PARAGRAPH && !maybe_lazy &&
                                  (matched = scan_html_block_start_7(input, parser->first_nonspace))))) {
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HTML_BLOCK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            (*container)->as.html_block->block_type = matched;
            // note, we don't adjust parser->offset because the tag is part of the
            // text
        } else if (!indented && cont_type == MARKDOWN_CORE_NODE_PARAGRAPH &&
                   (lev = scan_setext_heading_line(input, parser->first_nonspace))) {
            // finalize paragraph, resolving reference links
            has_content = resolve_reference_link_definitions(parser, *container);

            if (has_content) {

                markdown_core_node_set_kind_result result =
                    markdown_core_node_set_kind(*container, MARKDOWN_CORE_NODE_HEADING);
                if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
                    if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
                        parser->oom = true;
                    }
                    return;
                }
                (*container)->as.heading->level = lev;
                (*container)->as.heading->setext = true;
                S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
            }
        } else if (!indented && !maybe_lazy && cont_type != MARKDOWN_CORE_NODE_PARAGRAPH &&
                   peek_at(input, parser->first_nonspace) == '-' &&
                   (candidate_table = markdown_core_table_try_open(parser, *container, input->data, input->len))) {
            *container = candidate_table;
            return;
        } else if (!indented && !(cont_type == MARKDOWN_CORE_NODE_PARAGRAPH && !all_matched) &&
                   (parser->thematic_break_kill_pos <= parser->first_nonspace) &&
                   (matched = S_scan_thematic_break(parser, input, parser->first_nonspace))) {
            // it's only now that we know the line is not part of a setext heading:
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_THEMATIC_BREAK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
        } else if (!indented && depth < MAX_FOOTNOTE_DEPTH &&
                   (matched = scan_footnote_definition(input, parser->first_nonspace))) {
            markdown_core_chunk c = markdown_core_chunk_dup(input, parser->first_nonspace + 2, matched - 2);
            unsigned char *id;
            int lost = 0;

            while (c.data[c.len - 1] != ']') {
                --c.len;
            }
            --c.len;

            if (!markdown_core_chunk_to_cstr(parser->mem, &c)) {
                /* The label would keep borrowing the transient line buffer. */
                parser->oom = true;
                return;
            }

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            /* THE ANCHOR RULE (§5.1): a definition is a block node at the byte
             * where its OPENING BRACKET was written. It used to start at the
             * byte after `[^label]:`, which is a column that need not exist --
             * `[^footnote]:` alone on a line is twelve bytes and the definition
             * began at column 13. Every other block in this engine starts at
             * its own first byte and the marker is inside it; a footnote
             * definition was the one that started after its own marker. */
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_FOOTNOTE, parser->first_nonspace + 1);
            if (!*container) {
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }
            /* The id is the label under the map's own normalization and
             * WITHOUT a caret (M4): the key every call's referent names. The
             * caret that kept a footnote apart from a link definition in a
             * consumer's single map went with the association -- a
             * `Footnote` and a resolved `Link` are different values now. */
            id = normalize_map_label(parser->mem, &c, &lost);
            if (!id) {
                parser->oom = true;
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }
            (*container)->as.footnote->id.data = id;
            (*container)->as.footnote->id.len = (bufsize_t)strlen((const char *)id);
            (*container)->as.footnote->id.alloc = 1;
            if (!markdown_core_parser_register_definition(parser, *container, NULL)) {
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }

            /* The document defines this label from here on.
             *
             * Registered where the label is READ, which is here. Whether it is
             * registered at open or at close is NOT observable and that was
             * measured, not assumed: moving this call into `finalize` leaves
             * every suite and every oracle green. It used to matter, and the
             * reason it stopped is the shape rather than the timing -- the map
             * this replaced held a NODE per entry and used registration order
             * as the tie-break for a repeated label, so on EXIT a definition
             * nested inside another closed first, won the label, and the outer
             * one was freed with everything written in it (D11). A set of
             * labels owns no node and picks no winner, so order decides
             * nothing left to get wrong. */
            markdown_core_footnote_definition_create(parser->footnote_defs, &c);
            markdown_core_chunk_free(parser->mem, &c);

            (*container)->internal_offset = matched;
        } else if (!indented && (*container)->kind != MARKDOWN_CORE_NODE_PARAGRAPH &&
                   (matched = parse_specimen_marker(parser, input, parser->first_nonspace, &specimen))) {
            if (specimen.id.has_value && !markdown_core_chunk_to_cstr(parser->mem, &specimen.id.value)) {
                parser->oom = true;
                return;
            }
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_SPECIMEN, parser->first_nonspace + 1);
            if (!*container) {
                markdown_core_optional_chunk_free(parser->mem, &specimen.id);
                return;
            }
            if ((*container)->prev && (*container)->prev->kind == MARKDOWN_CORE_NODE_SPECIMEN) {
                specimen.has_start = false;
                specimen.start = 0;
            }
            *(*container)->as.specimen = specimen;
            if (!markdown_core_parser_register_definition(parser, *container, NULL)) {
                return;
            }
            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            while (S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
                parser->specimen_work++;
            }
        } else if ((!indented || cont_type == MARKDOWN_CORE_NODE_LIST) && parser->indent < 4 &&
                   (matched = parse_list_marker(parser, input, parser->first_nonspace, *container,
                                                parser->first_nonspace_column, data))) {

            data->padding = consume_item_marker(parser, input, matched);

            // check container; if it's a list, see if this list item
            // can continue the list; otherwise, create a list container.

            data->marker_offset = parser->indent;

            if (cont_type != MARKDOWN_CORE_NODE_LIST || !list_facts_match((*container)->as.list, data)) {
                *container = add_child(parser, *container, MARKDOWN_CORE_NODE_LIST, parser->first_nonspace + 1);
                if (!*container) {
                    return;
                }

                memcpy((*container)->as.list, data, sizeof(*data));
            }

            // add the list item
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_LIST_ITEM, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            memcpy((*container)->as.list, data, sizeof(*data));
            S_find_first_nonspace(parser, input);
            markdown_core_parse_task_prefix(parser, *container, input->data, input->len);
            if (parser->oom) {
                return;
            }
        } else if (indented && !maybe_lazy && !parser->blank) {
            S_advance_offset(parser, input, CODE_INDENT, true);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->offset + 1);
            if (!*container) {
                return;
            }
            (*container)->as.code->fenced = false;
            (*container)->as.code->fence_char = 0;
            (*container)->as.code->fence_length = 0;
            (*container)->as.code->fence_offset = 0;
            (*container)->as.code->fence_closed = false;
            /* An indented code block has no fence and therefore no info
             * string, ever. */
            (*container)->as.code->info = markdown_core_optional_chunk_absent();
        } else {
            markdown_core_llist *tmp;
            markdown_core_node *new_container = NULL;

            for (tmp = parser->extensions; tmp; tmp = tmp->next) {
                const markdown_core_extension *ext = (const markdown_core_extension *)tmp->data;

                if (ext->try_opening_block) {
                    new_container = ext->try_opening_block(ext, indented, parser, *container, input->data, input->len);

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
                bool compact;
                if (!maybe_lazy && cont_type != MARKDOWN_CORE_NODE_PARAGRAPH &&
                    definition_prefix(parser, *container, input, &compact)) {
                    *container = open_definition(parser, *container, input, compact);
                    return;
                }
                break;
            }
        }

        /* What this opener consumed made the block it just opened, so the
         * block owns it: `> `, `- `, the `#`s of a heading, the opening fence,
         * `[^label]:`. Claimed once per turn of the loop -- once per block
         * opened -- and before `accepts_lines` breaks out. */

        if (accepts_lines(*container)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        cont_type = S_type(*container);
        maybe_lazy = false;
    }
}

/* Step 14, after every ordinary block opener has declined the line. The
 * preceding block has finalized under this same parent. Definition-only
 * paragraphs still occupy their source position until block parsing ends.
 * The shared lookahead proves a following blank line belongs to that parent,
 * not an outer one. */
static bool S_attach_identifier_line(markdown_core_parser *parser, markdown_core_node *parent,
                                     markdown_core_chunk *input) {
    markdown_core_node *owner = parent->last_child;
    block_identifier candidate;
    if (parser->indent >= CODE_INDENT || input->data[parser->first_nonspace] != '#' || !owner ||
        owner->attributes.anchor.len ||
        (S_type(owner) != MARKDOWN_CORE_NODE_LIST && S_type(owner) != MARKDOWN_CORE_NODE_CALLOUT &&
         S_type(owner) != MARKDOWN_CORE_NODE_TABLE) ||
        !S_scan_block_identifier(parser, input->data + parser->first_nonspace, input->len - parser->first_nonspace,
                                 &candidate) ||
        !candidate.own_line || candidate.content_end || !S_ends_with_blank_line(owner)) {
        return false;
    }
    bool followed_by_boundary = parser->lookahead_cursor == parser->lookahead_end;
    if (!followed_by_boundary) {
        markdown_core_block_lookahead lookahead;
        markdown_core_chunk next;
        int first_nonspace, indent, blank_lines;
        if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
            return false;
        }
        markdown_core_parser_lookahead_next(&lookahead, &next, &first_nonspace, &indent, &blank_lines);
        followed_by_boundary = blank_lines > 0;
        markdown_core_parser_lookahead_end(&lookahead);
    }
    if (!followed_by_boundary || parser->oom || !S_attach_block_identifier(parser, owner, &candidate)) {
        return false;
    }
    S_set_end_to_current_line(parser, owner);
    return true;
}

static void add_text_to_container(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_node *last_matched_container, markdown_core_chunk *input) {
    markdown_core_node *tmp;
    // what remains at parser->offset is a text line.  add the text to the
    // appropriate container.

    S_find_first_nonspace(parser, input);

    if (parser->blank && container->last_child) {
        S_set_last_line_blank(container->last_child, true);
    }

    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    const markdown_core_node_type ctype = S_type(container);
    const bool last_line_blank =
        (parser->blank && ctype != MARKDOWN_CORE_NODE_CALLOUT && ctype != MARKDOWN_CORE_NODE_HEADING &&
         ctype != MARKDOWN_CORE_NODE_THEMATIC_BREAK && !extension_accepts_lines(container) &&
         !(ctype == MARKDOWN_CORE_NODE_CODE_BLOCK && container->as.code->fenced) &&
         !(ctype == MARKDOWN_CORE_NODE_LIST_ITEM && container->first_child == NULL &&
           container->start_line == parser->line_number));

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
    if (parser->current != last_matched_container && container == last_matched_container && !parser->blank &&
        (S_type(parser->current) == MARKDOWN_CORE_NODE_PARAGRAPH ||
         (S_type(parser->current) == MARKDOWN_CORE_NODE_CALLOUT && parser->current->as.callout->variant.has_value &&
          parser->current->start_line == parser->line_number - 1 && !parser->current->first_child))) {
        /* Metadata is not body text. A lazy line immediately after it opens
         * the body paragraph here, at its own source position; later lazy
         * lines continue that paragraph through the same add_line operation. */
        if (S_type(parser->current) == MARKDOWN_CORE_NODE_CALLOUT) {
            markdown_core_node *paragraph =
                add_child(parser, parser->current, MARKDOWN_CORE_NODE_PARAGRAPH, parser->offset + 1);
            if (!paragraph) {
                return;
            }
            parser->current = paragraph;
        }
        add_line(parser->current, input, parser);
    } else { // not a lazy continuation
        // Finalize any blocks that were not matched and set cur to container:
        markdown_core_parser_finalize_unmatched_blocks(parser);

        if (S_type(container) == MARKDOWN_CORE_NODE_CODE_BLOCK) {
            add_line(container, input, parser);
        } else if (S_type(container) == MARKDOWN_CORE_NODE_HTML_BLOCK) {
            add_line(container, input, parser);

            int matches_end_condition;
            switch (container->as.html_block->block_type) {
            case 1:
                // </script>, </style>, </pre>
                matches_end_condition = scan_html_block_end_1(input, parser->first_nonspace);
                break;
            case 2:
                // -->
                matches_end_condition = scan_html_block_end_2(input, parser->first_nonspace);
                break;
            case 3:
                // ?>
                matches_end_condition = scan_html_block_end_3(input, parser->first_nonspace);
                break;
            case 4:
                // >
                matches_end_condition = scan_html_block_end_4(input, parser->first_nonspace);
                break;
            case 5:
                // ]]>
                matches_end_condition = scan_html_block_end_5(input, parser->first_nonspace);
                break;
            default:
                matches_end_condition = 0;
                break;
            }

            if (matches_end_condition) {
                container->flags |= MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION;
                container = finalize(parser, container);
                assert(parser->current != NULL);
            }
        } else if (extension_accepts_lines(container)) {
            add_line(container, input, parser);
        } else if (parser->blank) {
            // ??? do nothing
        } else if (accepts_lines(container)) {
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            add_line(container, input, parser);
        } else {
            container = S_parent_for_block(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH);
            parser->current = container;
            if (S_attach_identifier_line(parser, container, input) || parser->oom) {
                return;
            }
            // create paragraph container for line
            container = add_child(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, parser->first_nonspace + 1);
            if (!container) {
                return;
            }
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            add_line(container, input, parser);
        }

        parser->current = container;
    }
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes) {
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
    if (bytes == 0 || !S_is_line_end_char(parser->curline.ptr[bytes - 1])) {
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

/* Document definitions are independent roots, followed by the content tree.
 * Each family advances through the live slot after a phase replaces its root. */
static int S_apply_tree_phase(markdown_core_parser *parser, markdown_core_node **root_slot, tree_phase_func phase,
                              void *context) {
    markdown_core_node *root = root_slot ? *root_slot : NULL;
    if (!root || parser->oom) {
        return !parser->oom;
    }
    if (root->kind == MARKDOWN_CORE_NODE_DOCUMENT) {
        markdown_core_node **families[] = {&root->as.document->footnotes, &root->as.document->specimens};
        for (size_t family = 0; family < sizeof(families) / sizeof(families[0]); family++) {
            for (markdown_core_node **slot = families[family]; *slot; slot = &(*slot)->next) {
                if (!walk_owned_trees(parser, slot, NULL, phase, context, 0)) {
                    return 0;
                }
            }
        }
    }
    return walk_owned_trees(parser, root_slot, NULL, phase, context, 0);
}

/* Register at syntax commitment; no completed-tree discovery pass is needed.
 * Inline bodies enter the document's value chain immediately, and the index
 * borrows only until finalization, before consolidation or postprocessing. */
bool markdown_core_parser_register_definition(markdown_core_parser *parser, markdown_core_node *definition,
                                              markdown_core_node *citation) {
    assert(definition &&
           (definition->kind == MARKDOWN_CORE_NODE_FOOTNOTE || definition->kind == MARKDOWN_CORE_NODE_SPECIMEN));
    markdown_core_definition_collection *collection =
        definition->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? &parser->footnotes : &parser->specimens;
    assert(!citation || definition->kind == MARKDOWN_CORE_NODE_FOOTNOTE);
    assert(citation ? !definition->parent : definition->parent != NULL);
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
    parser->definition_registration_work++;
    if (citation) {
        definition->prev = collection->last_inline;
        if (collection->last_inline) {
            collection->last_inline->next = definition;
        } else {
            parser->root->as.document->footnotes = definition;
        }
        collection->last_inline = definition;
    }
    return true;
}

static uint64_t definition_source_key(const void *entry) {
    markdown_core_node *node;
    memcpy(&node, entry, sizeof(node));
    return ((uint64_t)(uint32_t)node->start_line << 32) | (uint32_t)node->start_column;
}

/* Eight stable byte passes order the two nonnegative 32-bit coordinates.
 * This bound holds for every source shape on every libc; there is no
 * comparison-sort worst case or input-size-dependent alternate path. */
int markdown_core_order_source_entries(markdown_core_mem *mem, void *entries, size_t count, size_t stride,
                                       uint64_t (*key)(const void *)) {
    if (!count) {
        return 1;
    }
    if (count > SIZE_MAX / stride) {
        return 0;
    }
    unsigned char *scratch = mem->calloc(count, stride);
    unsigned char *source = entries;
    unsigned char *target = scratch;
    if (!scratch) {
        return 0;
    }
    for (unsigned shift = 0; shift < 64; shift += 8) {
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
    assert(source == entries);
    mem->free(scratch);
    return 1;
}

static int order_definitions(markdown_core_mem *mem, markdown_core_definition_collection *collection) {
    return markdown_core_order_source_entries(mem, collection->values, collection->count, sizeof(*collection->values),
                                              definition_source_key);
}

static void own_definitions(markdown_core_definition_collection *collection, markdown_core_node **slot) {
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

/* The complete block definition set is known before any heading or inline is
 * parsed. Its exact-byte index borrows authored ids and keeps the first node. */
static void prepare_specimens(markdown_core_parser *parser) {
    markdown_core_definition_collection *collection = &parser->specimens;
    if (!markdown_core_key_index_init(&parser->specimen_ids, parser->mem, collection->count) ||
        (collection->count && !order_definitions(parser->mem, collection))) {
        parser->oom = true;
        return;
    }
    for (size_t i = 0; i < collection->count; i++) {
        markdown_core_node *definition = collection->values[i].definition;
        markdown_core_optional_chunk *id = &definition->as.specimen->id;
        if (id->has_value && !markdown_core_key_index_insert(&parser->specimen_ids, id->value.data, id->value.len,
                                                             definition, 0, NULL)) {
            parser->oom = true;
            return;
        }
    }
}

/* Every authored id is reserved before generating any inline id. A collision
 * probe consumes an authored id from this candidate's namespace: inline-N and
 * inline-M never share suffix candidates when N != M. Thus total probes are
 * bounded by F plus the authored-id count, including adversarial suffix runs. */
static void finalize_footnotes(markdown_core_parser *parser) {
    markdown_core_definition_collection *collection = &parser->footnotes;
    markdown_core_key_index ids = {0};
    size_t index, ordinal = 0;
    if (!collection->count) {
        goto done;
    }
    if (!order_definitions(parser->mem, collection) ||
        !markdown_core_key_index_init(&ids, parser->mem, collection->count)) {
        goto failed;
    }
    for (index = 0; index < collection->count; index++) {
        markdown_core_node *footnote = collection->values[index].definition;
        markdown_core_chunk *id = &footnote->as.footnote->id;
        if (id->data && !markdown_core_key_index_insert(&ids, id->data, id->len, footnote, 0, NULL)) {
            goto failed;
        }
    }
    for (index = 0; index < collection->count; index++) {
        markdown_core_node *footnote = collection->values[index].definition;
        markdown_core_chunk *id = &footnote->as.footnote->id;
        if (!id->data) {
            /* Each decimal size_t takes at most 3 * sizeof(size_t) bytes. */
            char candidate[sizeof("inline--") + 6 * sizeof(size_t)];
            size_t suffix = 0;
            markdown_core_node *citation = collection->values[index].citation;
            assert(citation && citation->kind == MARKDOWN_CORE_NODE_CITATION);
            ordinal++;
            snprintf(candidate, sizeof(candidate), "inline-%zu", ordinal);
            while (
                markdown_core_key_index_lookup(&ids, (const unsigned char *)candidate, (bufsize_t)strlen(candidate))) {
                snprintf(candidate, sizeof(candidate), "inline-%zu-%zu", ordinal, ++suffix);
            }
            if (!markdown_core_chunk_set_cstr(parser->mem, id, candidate) ||
                !markdown_core_chunk_set_cstr(parser->mem, &citation->as.citation->value, candidate) ||
                !markdown_core_key_index_insert(&ids, id->data, id->len, footnote, 0, NULL)) {
                goto failed;
            }
        }
    }
    /* No allocation or fallible work remains once ownership starts moving. */
    own_definitions(collection, &parser->root->as.document->footnotes);
    goto done;
failed:
    parser->oom = true;
done:
    markdown_core_key_index_free(&ids);
    parser->mem->free(collection->values);
    memset(collection, 0, sizeof(*collection));
}

static void prepare_headings(markdown_core_parser *parser, markdown_core_heading_collection *headings) {
    if (!markdown_core_order_source_entries(parser->mem, headings->values, headings->count, sizeof(*headings->values),
                                            definition_source_key)) {
        parser->oom = true;
        return;
    }
    /* The reference map compares explicitness and original source positions,
     * independently of mapped-input scheduling and declaration closure order. */
    for (size_t i = 0; i < headings->count && !parser->oom; i++) {
        markdown_core_prepare_heading(parser, &headings->values[i]);
        if (parser->refmap->oom) {
            parser->oom = true;
        }
    }
    for (size_t i = 0; i < headings->count && !parser->oom; i++) {
        markdown_core_finish_heading(parser, &headings->values[i]);
    }
}

static void dispose_headings(markdown_core_parser *parser, markdown_core_heading_collection *headings) {
    for (size_t i = 0; i < headings->count; i++) {
        markdown_core_dispose_heading(&headings->values[i]);
    }
    parser->mem->free(headings->values);
    *headings = (markdown_core_heading_collection){0};
}

static void project_anchor_literal(markdown_core_parser *parser, markdown_core_strbuf *base, const unsigned char *text,
                                   bufsize_t length) {
    parser->anchor_work += (size_t)length;
    markdown_core_utf8proc_anchor(base, text, length);
}

typedef enum { ANCHOR_CONTENT, ANCHOR_CITATIONS, ANCHOR_KEY } anchor_projection_kind;
typedef struct {
    markdown_core_node *node;
    anchor_projection_kind kind;
} anchor_projection;

typedef struct {
    anchor_projection *values;
    size_t count, capacity;
} anchor_projection_stack;

static bool push_anchor_projection(markdown_core_parser *parser, anchor_projection_stack *stack,
                                   markdown_core_node *node, anchor_projection_kind kind) {
    if (!node) {
        return true;
    }
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*stack->values)) {
            parser->oom = true;
            return false;
        }
        void *values = parser->mem->realloc(stack->values, capacity * sizeof(*stack->values));
        if (!values) {
            parser->oom = true;
            return false;
        }
        stack->values = values;
        stack->capacity = capacity;
    }
    stack->values[stack->count++] = (anchor_projection){node, kind};
    return true;
}

/* Source-order projection visits content and value-owned fields through one
 * explicit continuation stack. A bibliography item contributes prefix/key/
 * suffix; a definition body is never a reference's projected text. */
static void heading_anchor_base(markdown_core_parser *parser, markdown_core_node *heading, markdown_core_strbuf *base) {
    anchor_projection_stack stack = {0};
    push_anchor_projection(parser, &stack, heading->first_child, ANCHOR_CONTENT);
    while (stack.count && !parser->oom && !base->oom) {
        anchor_projection projection = stack.values[--stack.count];
        markdown_core_node *node = projection.node;
        parser->anchor_work++;
        if (projection.kind == ANCHOR_KEY) {
            project_anchor_literal(parser, base, (const unsigned char *)"@", 1);
            project_anchor_literal(parser, base, node->as.citation->value.data, node->as.citation->value.len);
            continue;
        }
        push_anchor_projection(parser, &stack, node->next, projection.kind);
        if (projection.kind == ANCHOR_CITATIONS) {
            markdown_core_citation_item *item = node->as.citation;
            if (item->referent == MARKDOWN_CORE_NODE_REFERENT_BIB) {
                push_anchor_projection(parser, &stack, item->suffix ? item->suffix->first_child : NULL, ANCHOR_CONTENT);
                push_anchor_projection(parser, &stack, node, ANCHOR_KEY);
                push_anchor_projection(parser, &stack, item->prefix ? item->prefix->first_child : NULL, ANCHOR_CONTENT);
            } else if (item->referent == MARKDOWN_CORE_NODE_REFERENT_SPECIMEN) {
                push_anchor_projection(parser, &stack, node, ANCHOR_KEY);
            }
            continue;
        }
        switch (node->kind) {
        case MARKDOWN_CORE_NODE_TEXT:
        case MARKDOWN_CORE_NODE_CODE:
            project_anchor_literal(parser, base, node->as.literal->data, node->as.literal->len);
            break;
        case MARKDOWN_CORE_NODE_FORMULA: {
            const char *literal = markdown_core_extensions_get_formula_literal(node);
            project_anchor_literal(parser, base, (const unsigned char *)literal, (bufsize_t)strlen(literal));
            break;
        }
        case MARKDOWN_CORE_NODE_SOFT_BREAK:
        case MARKDOWN_CORE_NODE_LINE_BREAK:
            markdown_core_strbuf_putc(base, '-');
            break;
        case MARKDOWN_CORE_NODE_CROSS_LINK:
        case MARKDOWN_CORE_NODE_CROSS_EMBEDDED: {
            markdown_core_cross_reference *cross = markdown_core_node_cross_reference(node);
            if (cross->label.has_value) {
                project_anchor_literal(parser, base, cross->label.value.data, cross->label.value.len);
            } else {
                project_anchor_literal(parser, base, cross->path.data, cross->path.len);
                if (cross->anchor.has_value) {
                    project_anchor_literal(parser, base, cross->anchor.value.data, cross->anchor.value.len);
                }
            }
            break;
        }
        case MARKDOWN_CORE_NODE_CITE:
            push_anchor_projection(parser, &stack, node->as.cite->citations, ANCHOR_CITATIONS);
            break;
        case MARKDOWN_CORE_NODE_EMPHASIS:
        case MARKDOWN_CORE_NODE_STRONG:
        case MARKDOWN_CORE_NODE_STRIKETHROUGH:
        case MARKDOWN_CORE_NODE_MARK:
        case MARKDOWN_CORE_NODE_INSERTION:
        case MARKDOWN_CORE_NODE_SPAN:
        case MARKDOWN_CORE_NODE_SUPERSCRIPT:
        case MARKDOWN_CORE_NODE_SUBSCRIPT:
        case MARKDOWN_CORE_NODE_LINK:
        case MARKDOWN_CORE_NODE_MEDIA:
        case MARKDOWN_CORE_NODE_DIRECTIVE_LABEL:
            push_anchor_projection(parser, &stack, node->first_child, ANCHOR_CONTENT);
            break;
        case MARKDOWN_CORE_NODE_DIRECTIVE: {
            markdown_core_node *label = markdown_core_directive_label(node);
            push_anchor_projection(parser, &stack, label ? label->first_child : NULL, ANCHOR_CONTENT);
            break;
        }
        default:
            /* HTML, comments and other opaque values contribute no text. */
            break;
        }
    }
    parser->mem->free(stack.values);
    if (!base->size) {
        markdown_core_strbuf_puts(base, "section");
    }
    if (base->oom) {
        parser->oom = true;
    }
}

/* A size_t needs at most 3 * sizeof(size_t) decimal digits. Suffix spelling
 * is always ASCII and has no locale or format-string interpretation. */
static void append_anchor_suffix(markdown_core_strbuf *base, size_t ordinal) {
    char suffix[3 * sizeof(size_t) + 1];
    char *end = suffix + sizeof(suffix), *start = end;
    do {
        *--start = (char)('0' + ordinal % 10);
        ordinal /= 10;
    } while (ordinal);
    *--start = '-';
    markdown_core_strbuf_put(base, (const unsigned char *)start, (bufsize_t)(end - start));
}

static void finalize_heading_anchors(markdown_core_parser *parser, markdown_core_heading_collection *headings,
                                     anchor_registry *registry) {
    markdown_core_strbuf base = MARKDOWN_CORE_BUF_INIT(parser->mem);
    for (size_t i = 0; i < headings->count && !parser->oom; i++) {
        markdown_core_heading_parse *heading = &headings->values[i];
        markdown_core_chunk *anchor = &heading->node->attributes.anchor;
        if (!anchor->len) {
            markdown_core_strbuf_clear(&base);
            heading_anchor_base(parser, heading->node, &base);
            if (parser->oom) {
                break;
            }
            bufsize_t base_length = base.size;
            markdown_core_key_index_slot *entry =
                anchor_slot(parser, registry, (markdown_core_chunk){base.ptr, base.size, 0});
            markdown_core_key_index_slot *candidate = entry;
            if (entry && entry->key) {
                do {
                    markdown_core_strbuf_truncate(&base, base_length);
                    append_anchor_suffix(&base, entry->value.counter++);
                    if (base.oom) {
                        parser->oom = true;
                        break;
                    }
                    /* Only a vacant candidate can grow the index. The base
                     * cursor is updated before that call and never used after
                     * it returns a vacant entry, so no pointer survives growth. */
                    candidate = anchor_slot(parser, registry, (markdown_core_chunk){base.ptr, base.size, 0});
                } while (candidate && candidate->key);
            }
            if (parser->oom) {
                break;
            }
            markdown_core_chunk_free(parser->mem, anchor);
            *anchor = (markdown_core_chunk){base.ptr, base.size, 0};
            if (!markdown_core_chunk_to_cstr(parser->mem, anchor)) {
                parser->oom = true;
                break;
            }
            markdown_core_key_index_commit(&registry->index, candidate, anchor->data);
            candidate->value.counter = 1;
        }
        markdown_core_resource *resource = heading->resource;
        if (resource && !parser->oom) {
            markdown_core_strbuf_clear(&base);
            markdown_core_strbuf_putc(&base, '#');
            markdown_core_strbuf_put(&base, anchor->data, anchor->len);
            if (base.oom) {
                parser->oom = true;
                break;
            }
            markdown_core_chunk_free(parser->mem, &resource->url);
            resource->url = (markdown_core_chunk){base.ptr, base.size, 0};
            if (!markdown_core_chunk_to_cstr(parser->mem, &resource->url)) {
                parser->oom = true;
            }
        }
    }
    markdown_core_strbuf_free(&base);
}

static int S_consolidate_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    (void)context;
    return markdown_core_consolidate_text_nodes_with_parser(parser, *root_slot);
}

#if MARKDOWN_CORE_DEBUG_NODES
static int S_check_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    (void)parser;
    (void)context;
    return markdown_core_node_check(*root_slot, stderr) == 0;
}
#endif

static int S_postprocess_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    const markdown_core_extension *extension = (const markdown_core_extension *)context;
    markdown_core_node *processed = extension->postprocess_func(extension, parser, *root_slot);
    if (processed) {
        *root_slot = processed;
    }
    return !parser->oom;
}

static markdown_core_node *S_finish_parse(markdown_core_parser *parser) {
    markdown_core_node *res;
    markdown_core_llist *extensions;
    markdown_core_heading_collection *headings = &parser->headings;
    anchor_registry anchors = {0};

    if (parser->root == NULL || parser->oom) {
        return NULL;
    }

    finalize_document(parser);
    S_parse_block_inputs(parser);
    S_complete_block_tree(parser->root);
    if (!parser->oom) {
        prepare_specimens(parser);
    }
    if (!parser->oom) {
        markdown_core_manage_extensions_special_characters(parser, true);
        prepare_headings(parser, headings);
    }
    if (!parser->oom) {
        if (!markdown_core_key_index_init(&anchors.index, parser->mem, headings->count) ||
            !markdown_core_key_index_init(&anchors.resources, parser->mem, 0)) {
            parser->oom = true;
        } else {
            process_inlines(parser, parser->refmap, headings->count ? &anchors : NULL);
        }
    }

    /* Map failures are sticky on the maps because reference resolution owns
     * those allocations. Pull them into the transaction flag at every phase
     * boundary so no later transform runs on a failed parse. */
    if ((parser->refmap && parser->refmap->oom) || (parser->footnote_defs && parser->footnote_defs->oom)) {
        parser->oom = true;
    }
    if (parser->oom) {
        goto failed;
    }

    /* Complete the document model and discard parse-time edges before any
     * tree transform. Postprocessors receive resolved ids and owned values. */
    finalize_footnotes(parser);
    if (!parser->oom) {
        own_definitions(&parser->specimens, &parser->root->as.document->specimens);
        parser->mem->free(parser->specimens.values);
        parser->specimens = (markdown_core_definition_collection){0};
        markdown_core_key_index_free(&parser->specimen_ids);
    }
    if (!parser->oom) {
        finalize_heading_anchors(parser, headings, &anchors);
    }
    markdown_core_key_index_free(&anchors.index);
    markdown_core_key_index_free(&anchors.resources);
    dispose_headings(parser, headings);
    if (parser->oom) {
        goto failed;
    }

    if (!S_apply_tree_phase(parser, &parser->root, S_consolidate_tree, NULL)) {
        parser->oom = true;
    }
    if (parser->oom) {
        goto failed;
    }

#if MARKDOWN_CORE_DEBUG_NODES
    if (!S_apply_tree_phase(parser, &parser->root, S_check_tree, NULL)) {
        abort();
    }
#endif

    for (extensions = parser->extensions; extensions && !parser->oom; extensions = extensions->next) {
        const markdown_core_extension *ext = (const markdown_core_extension *)extensions->data;
        if (ext->postprocess_func) {
            if (!S_apply_tree_phase(parser, &parser->root, S_postprocess_tree, (void *)ext)) {
                parser->oom = true;
            }
        }
    }
    if (parser->oom) {
        goto failed;
    }

    res = parser->root;
    parser->root = NULL;
    return res;

failed:
    markdown_core_key_index_free(&anchors.index);
    markdown_core_key_index_free(&anchors.resources);
    dispose_headings(parser, headings);
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

markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column) {
    return add_child(parser, parent, block_type, start_column);
}

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    S_advance_offset(parser, &input_chunk, count, columns != 0);
}

void markdown_core_parser_set_backslash_ispunct_func(markdown_core_parser *parser, markdown_core_ispunct_func func) {
    parser->backslash_ispunct = func;
}
