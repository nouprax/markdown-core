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
static void S_project_block_hooks(markdown_core_parser *parser);
static markdown_core_node *S_finish_parse(markdown_core_parser *parser);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);

static markdown_core_node *make_block(markdown_core_parser *parser, markdown_core_node_type tag, int start_line,
                                      int start_column) {
    markdown_core_node *e;

    e = markdown_core_parser_make_node(parser, tag);
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
static markdown_core_node *make_document(markdown_core_parser *parser) {
    markdown_core_node *e = make_block(parser, MARKDOWN_CORE_NODE_DOCUMENT, 1, 1);
    return e;
}

/* Why a descriptor is refused, or NULL. The rule: an element takes part in
 * the finish stage as a LOCAL step or as a GLOBAL pass, never both
 * (markdown-core-element-api.h states the invariant). A descriptor that
 * declares both would run its step from inside the walk and its pass after it,
 * and nothing in either hook's contract says what the second may assume about
 * the first's work; that is two concerns, which is two elements. A step is
 * asked once per event, so a kind in both of its lists -- one EXIT declared
 * twice -- is refused rather than delivered twice. A step asked at no kind
 * would never be called, and is refused rather than silently kept. And a kind
 * the dispatch table cannot index -- an ordinal at or past
 * MARKDOWN_CORE_NODE_KIND_COUNT, or a value of neither class -- would share
 * the table's one out-of-table key and be asked at every such node's events
 * instead of its own, so it is refused too: that key is never declared. */
static bool S_finish_kind_indexable(markdown_core_node_type kind) {
    unsigned class = (unsigned)kind & MARKDOWN_CORE_NODE_TYPE_MASK;
    return (class == MARKDOWN_CORE_NODE_TYPE_BLOCK || class == MARKDOWN_CORE_NODE_TYPE_INLINE) &&
           ((unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK) < MARKDOWN_CORE_NODE_KIND_COUNT;
}

static const char *S_element_rejection(const markdown_core_element *element) {
    if (element->finish_step && element->postprocess_func) {
        return "declares both a finish step and a postprocess pass";
    }
    if ((element->finish_exit_kinds || element->finish_scope_kinds) && !element->finish_step) {
        return "declares where a finish step is asked without a finish step";
    }
    if (element->finish_step) {
        const markdown_core_node_type *lists[] = {element->finish_exit_kinds, element->finish_scope_kinds};
        bool asked = false;
        for (size_t list = 0; list < 2; list++) {
            for (const markdown_core_node_type *kind = lists[list]; kind && *kind; kind++) {
                if (!S_finish_kind_indexable(*kind)) {
                    return "declares a finish step at a kind outside the kind table";
                }
                asked = true;
            }
        }
        if (!asked) {
            return "declares a finish step asked at no kind";
        }
    }
    for (const markdown_core_node_type *exit = element->finish_exit_kinds; exit && *exit; exit++) {
        for (const markdown_core_node_type *scope = element->finish_scope_kinds; scope && *scope; scope++) {
            if (*exit == *scope) {
                return "declares a kind as both a finish exit kind and a finish scope kind";
            }
        }
    }
    return NULL;
}

static void S_register_element(markdown_core_parser *parser, const markdown_core_element *element) {
    assert(!S_element_rejection(element));
    if (element->parse_text) {
        parser->text_structure = element;
    }
    if (element->delimiter_rule != MARKDOWN_CORE_DELIM_RULE_NONE) {
        parser->delimiter_owners[element->delimiter_rule] = element;
        if (element->delimiter_character) {
            parser->delimiter_chars[element->delimiter_character] = element->delimiter_rule;
        }
    }
}

/* Setup extends the same registry read by every phase. The fixed dialect is
 * borrowed; an extension acquires an independent contiguous snapshot before
 * replacing it. Allocation failure, and a descriptor the one registration
 * rule refuses, leave the previous registry intact. */
int markdown_core_parser_attach_element(markdown_core_parser *parser, const markdown_core_element *element) {
    size_t count = parser->element_count;
    if (S_element_rejection(element) || count >= MARKDOWN_CORE_ELEMENT_LIMIT) {
        return 0;
    }
    const markdown_core_element **entries = markdown_core_alloc(count + 1, sizeof(*entries));
    if (!entries) {
        return 0;
    }
    if (count) {
        memcpy(entries, parser->elements, count * sizeof(*entries));
    }
    entries[count] = element;
    markdown_core_free(parser->element_allocation);
    parser->element_allocation = entries;
    parser->elements = entries;
    parser->element_count = count + 1;
    S_register_element(parser, element);
    return 1;
}

/* A physical line containing NUL owns one immutable normalized view. Both
 * the driver and speculative readers borrow it until the active input ends;
 * queued mapped inputs therefore receive the same bytes the driver would.
 * Only transformed lines allocate, and the ownership chain releases only
 * those allocations rather than visiting every untransformed input line. */
typedef struct markdown_core_normalized_line {
    struct markdown_core_normalized_line *next;
    unsigned char bytes[];
} markdown_core_normalized_line;

static void S_clear_normalized_lines(markdown_core_parser *parser) {
    while (parser->normalized_lines) {
        markdown_core_normalized_line *line = parser->normalized_lines;
        parser->normalized_lines = line->next;
        markdown_core_free(line);
    }
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
    markdown_core_source_order_dispose(&parser->source_order);
    markdown_core_free(parser->block_inputs);
    S_clear_normalized_lines(parser);
    markdown_core_free(parser->input_lines);
    markdown_core_free(parser->input_facts);
    markdown_core_free(parser->inline_dispatch);
    parser->inline_dispatch = NULL;
    markdown_core_free(parser->block_hook_allocation);
    parser->block_hook_allocation = NULL;
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

    /* The block-start lookahead's chain and resume cache are parser state of
     * the same kind: indexed by open containers and source lines, owned by no
     * node, and dead with the parse. */
    markdown_core_free(parser->lookahead_chain);
    markdown_core_free(parser->lookahead_chain_flags);
    parser->lookahead_chain = NULL;
    parser->lookahead_chain_flags = NULL;
    parser->lookahead_chain_alloc = 0;

    /* Last, after every release above: the cells the parse gave back, and the
     * slab it was taking from. The finished tree, returned before this, holds
     * its own slabs. */
    markdown_core_node_pool_dispose(&parser->nodes);
}

static markdown_core_parser *S_parser_new(void) {
    markdown_core_parser *parser;
    markdown_core_node *document;

    parser = (markdown_core_parser *)markdown_core_alloc(1, sizeof(*parser));
    if (!parser) {
        return NULL;
    }
    markdown_core_strbuf_init(&parser->curline, 256);
    markdown_core_strbuf_init(&parser->lookahead_last_line, 0);
    /* The line index is a parse-owned workspace, like curline. Establish its
     * initial capacity before setup; inputs reset length, never ownership. */
    parser->input_lines = markdown_core_reserve(NULL, &parser->input_line_capacity, 1, sizeof(*parser->input_lines));

    document = make_document(parser);
    parser->document_structure = markdown_core_structure_for_kind(MARKDOWN_CORE_NODE_DOCUMENT);
    parser->document_structure->init_document(parser);
    parser->root = document;
    parser->block_root = document;
    parser->current = document;

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. */
    if (!parser->root || !parser->input_lines || parser->curline.oom || parser->lookahead_last_line.oom ||
        parser->root->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }

    markdown_core_inlines_reset_special_chars(parser);
    return parser;
}

static void S_parser_free(markdown_core_parser *parser) {
    if (!parser) {
        return;
    }
    S_parser_dispose(parser);
    markdown_core_free(parser->element_allocation);
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->lookahead_last_line);
    markdown_core_free(parser);
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

static bool element_accepts_lines(markdown_core_node *node) {
    const markdown_core_element *structure = markdown_core_node_structure(node);
    return structure && (structure->content_mode == MARKDOWN_CORE_CONTENT_LITERAL ||
                         (structure->accepts_lines_func && structure->accepts_lines_func(structure, node)));
}
bool markdown_core_block_accepts_lines(markdown_core_node *node) {
    const markdown_core_element *structure = markdown_core_node_structure(node);
    return element_accepts_lines(node) || (structure && structure->content_mode == MARKDOWN_CORE_CONTENT_PROSE);
}
static bool contains_inlines(markdown_core_node *node) {
    const markdown_core_element *structure = markdown_core_node_structure(node);
    return structure && (structure->inline_content ||
                         (structure->contains_inlines_func && structure->contains_inlines_func(structure, node)));
}
static bool is_paragraph(markdown_core_node *node) {
    const markdown_core_element *structure = markdown_core_node_structure(node);
    return structure && structure->paragraph;
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
                                             int line, int column, int source_width, int source_step) {
    return S_append_content_mark(parser, &node->content_map,
                                 (markdown_core_line_mark){offset, line, column, source_width, source_step, 0});
}

static void S_record_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t column,
                                  bufsize_t length) {
    markdown_core_parser_append_source_marks(parser, node, parser->line_number, column, length, node->content.size);
}

void markdown_core_block_add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
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
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
    node->content_map.count = 0;
    node->content_map.offset = 0;
    return markdown_core_parser_append_content_mark(parser, node, 0, line, column, 1, 1);
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
        mark.column += (start - mark.content_offset) * mark.source_step;
        mark.content_offset = offset + start - from;
        if (!S_append_content_mark(parser, map, mark)) {
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
    if (!markdown_core_parser_content_end_place(parser, &parser->block_root->content_map,
                                                (bufsize_t)parser->input_lines[index].start + (column ? column - 1 : 0),
                                                &source_line, &source_column)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return column;
    }
    assert(source_line == line);
    return source_column - (column == 0 ? 1 : 0);
}

int markdown_core_parser_append_source_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                             int column, bufsize_t length, bufsize_t offset) {
    if (parser->block_root == parser->root) {
        return S_append_content_mark(parser, &node->content_map,
                                     (markdown_core_line_mark){offset, line, column, 1, 1, parser->indent});
    }
    size_t index = (size_t)(line - parser->input_first_line);
    assert(line >= parser->input_first_line && index < parser->input_line_count);
    return markdown_core_parser_append_content_marks(parser, &parser->block_root->content_map, &node->content_map,
                                                     (bufsize_t)parser->input_lines[index].start + column - 1, length,
                                                     offset);
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
                           bool end, int *line, int *column) {
    const markdown_core_line_mark *mark;

    if (!parser || !map || map->count <= 0 || content_offset < 0) {
        return 0;
    }

    content_offset += map->offset;
    mark = &parser->line_marks[markdown_core_block_content_mark_at(parser, map, content_offset)];
    *line = mark->line;
    *column = mark->column + (int)(content_offset - mark->content_offset) * mark->source_step +
              (end ? mark->source_width - 1 : 0);
    return 1;
}

int markdown_core_parser_content_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                       bufsize_t offset, int *line, int *column) {
    return S_content_place(parser, map, offset, false, line, column);
}

int markdown_core_parser_content_end_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                           bufsize_t offset, int *line, int *column) {
    return S_content_place(parser, map, offset, true, line, column);
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
        parent = markdown_core_block_finalize(parser, parent);
    }
    return parent;
}

// Add a node as child of another.  Return pointer to child.
markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column) {
    parent = markdown_core_block_parent_for(parser, parent, block_type);

    markdown_core_node *child =
        make_block(parser, block_type, parser->line_number,
                   markdown_core_parser_source_column(parser, parser->line_number, start_column));
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
    /* block_parent_for already established containment. Commit that decision
     * without re-entering a possibly stateful containment predicate. */
    markdown_core_node_attach_validated(parent, child, NULL);
    return child;
}

/* Project all three independent byte sets before inline parsing. Dispatch
 * retains every candidate in descriptor order, including overlapping owners.
 * The flattened index allocates once per parse, never once per token. */
void markdown_core_manage_elements_special_characters(markdown_core_parser *parser, int add) {
    size_t next[256];

    markdown_core_free(parser->inline_dispatch);
    parser->inline_dispatch = NULL;
    memset(parser->inline_dispatch_offsets, 0, sizeof(parser->inline_dispatch_offsets));

    for (size_t element_index = 0; element_index < parser->element_count; element_index++) {
        const markdown_core_element *element = parser->elements[element_index];
        if (!element->match_inline && !element->insert_inline_from_delim) {
            continue;
        }
        const unsigned char *c;

        if (add && element->match_inline) {
            bool seen[256] = {false};
            for (c = (const unsigned char *)element->dispatch; c && *c; c++) {
                if (!seen[*c]) {
                    parser->inline_dispatch_offsets[*c + 1]++;
                    seen[*c] = true;
                }
            }
        }

        for (c = (const unsigned char *)element->terminates_text; c && *c; c++) {
            if (add) {
                if (!parser->special_chars[*c]) {
                    parser->inline_start_predicates[*c] = element->is_inline_start;
                } else if (parser->inline_start_predicates[*c] != element->is_inline_start) {
                    parser->inline_start_predicates[*c] = NULL;
                }
                markdown_core_inlines_add_text_terminator(parser, *c);
            } else {
                parser->inline_start_predicates[*c] = NULL;
                markdown_core_inlines_remove_text_terminator(parser, *c);
            }
        }
        for (c = (const unsigned char *)element->flanking_transparent; c && *c; c++) {
            if (add) {
                markdown_core_inlines_add_flanking_transparent(parser, *c);
            } else {
                markdown_core_inlines_remove_flanking_transparent(parser, *c);
            }
        }
    }

    if (!add || parser->error) {
        return;
    }
    for (size_t c = 0; c < 256; c++) {
        parser->inline_dispatch_offsets[c + 1] += parser->inline_dispatch_offsets[c];
        next[c] = parser->inline_dispatch_offsets[c];
    }
    size_t count = parser->inline_dispatch_offsets[256];
    if (!count) {
        return;
    }
    parser->inline_dispatch = markdown_core_alloc(count, sizeof(*parser->inline_dispatch));
    if (!parser->inline_dispatch) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    for (size_t element_index = 0; element_index < parser->element_count; element_index++) {
        const markdown_core_element *element = parser->elements[element_index];
        if (!element->match_inline && !element->insert_inline_from_delim) {
            continue;
        }
        bool seen[256] = {false};
        if (!element->match_inline) {
            continue;
        }
        for (const unsigned char *c = (const unsigned char *)element->dispatch; c && *c; c++) {
            if (!seen[*c]) {
                parser->inline_dispatch[next[*c]++] = element;
                seen[*c] = true;
            }
        }
    }
    for (size_t c = 0; c < 256; c++) {
        size_t first = parser->inline_dispatch_offsets[c], end = parser->inline_dispatch_offsets[c + 1];
        for (size_t i = first + 1; i < end; i++) {
            const markdown_core_element *element = parser->inline_dispatch[i];
            size_t at = i;
            while (at > first && parser->inline_dispatch[at - 1]->inline_precedence > element->inline_precedence) {
                parser->inline_dispatch[at] = parser->inline_dispatch[at - 1];
                at--;
            }
            parser->inline_dispatch[at] = element;
        }
    }
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
            if (contains_inlines(cur)) {
                if (!markdown_core_node_structure(cur)->deferred_inlines) {
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
    /* Consolidation's buffer, allocated once for the walk rather than once
     * per merged run. */
    markdown_core_strbuf scratch;
} owned_tree_walk;

static int push_owned_root(markdown_core_node *root, owned_tree_walk *walk) {
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
        void *frames = markdown_core_realloc(walk->frames, capacity * sizeof(*walk->frames));
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
    walk->frames[walk->count++] = (owned_tree_frame){
        .root = root, .parsed = NULL, .script_depth = walk->script_depth, .parses = walk->parses, .started = false};
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
static void complete_inline_node(markdown_core_parser *parser, markdown_core_node *node, int script_depth) {
    const markdown_core_element *structure = markdown_core_node_structure(node);
    if (structure && structure->complete_inline) {
        structure->complete_inline(parser, node, script_depth);
    }
    if (parser->document_structure->observe_inline) {
        parser->document_structure->observe_inline(parser, node);
    }
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
    for (; entry->element; entry++) {
        if (!markdown_core_finish_step_admitted(entry, parser)) {
            continue;
        }
        result = entry->element->finish_step(entry->element, parser, node, event, is_root, &states[entry->slot]);
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
    return push_owned_root(slot ? *slot : NULL, context);
}

/* THE ROOTS THE FINISH WALK COMPLETED, in completion order, kept only while a
 * global pass is declared. A pass runs after the document's finalization, on
 * the finalized roots, and a root the walk found is not found again by
 * walking: the field roots and the document root are recorded here as they
 * complete, and the definition chains -- which finalization rebuilds, moving
 * the block definitions into them -- are enumerated afresh when the passes
 * run. A step never frees a node that owns field roots (the LOCAL contract),
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
static int walk_owned_trees(markdown_core_parser *parser, markdown_core_node *root, bool parses, tree_phase_func finish,
                            void *context, finish_roots *record, int record_initial) {
    owned_tree_walk walk = {.parser = parser,
                            .slots = parser->finish_step_slots,
                            .script_depth = 0,
                            .parses = parses,
                            .scratch = MARKDOWN_CORE_BUF_INIT()};
    finish_count count = {0, 0, 0};
    void (*const observe)(markdown_core_parser *, markdown_core_node *) = parser->document_structure->observe_inline;
    const markdown_core_finish_kind *const kinds = parser->finish_kinds;
    markdown_core_finish_step_entry *const *const dispatch = parser->finish_dispatch;

    push_owned_root(root, &walk);
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
                markdown_core_node *completed = frame->root;
                walk.count--;
                count.roots++;
                flush_finish_count(parser, &count);
                if (finish && !finish(parser, completed, context)) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                }
                /* A field root, or the initial root when asked: the chain roots
                 * are enumerated again after finalization, their fields are not. */
                if (record && (walk.count || record_initial) && !record_finish_root(record, completed)) {
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
                    result = markdown_core_consolidate_text_step(parser, iter, node, &walk.scratch,
                                                                 complete_inline_node, frame->script_depth);
                }
                if (result == MARKDOWN_CORE_FINISH_CONTINUE && dispatch[2 * index + 1]) {
                    result =
                        run_finish_steps(parser, dispatch[2 * index + 1], node, event, node == frame->root, states);
                }
                if (result == MARKDOWN_CORE_FINISH_FAILED) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
                continue;
            }
            /* A node is consumed only at its EXIT, so every ENTER reaches here. */
            count.entered++;
            const markdown_core_finish_kind *facts = &kinds[index];
            uint8_t flags = facts->flags;
            bool reach = frame->parses && !frame->parsed;
            if ((flags & MARKDOWN_CORE_FINISH_KIND_PARSES) && reach && contains_inlines(node)) {
                if (!(flags & MARKDOWN_CORE_FINISH_KIND_DEFERRED)) {
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
                result = run_finish_steps(parser, dispatch[2 * index], node, event, node == frame->root, states);
                if (result == MARKDOWN_CORE_FINISH_FAILED) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
            }
            if (facts->complete) {
                facts->complete(parser, node, frame->script_depth);
            }
            if (observe) {
                observe(parser, node);
            }
            /* The field roots, of a kind that owns them through its record
             * or of an element that owns them through its hook: the answer is
             * no for almost every node, and it is one flag and one load. */
            if (!(flags & MARKDOWN_CORE_FINISH_KIND_FIELDS) && !node->element) {
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
    markdown_core_strbuf_free(&walk.scratch);
    markdown_core_free(walk.frames);
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
    while (parser->block_input_cursor < parser->block_input_count && !parser->error) {
        markdown_core_node *owner = parser->block_inputs[parser->block_input_cursor++];
        parser->block_root = owner;
        parser->current = owner;
        parser->input_line_count = 0;
        parser->input_fact_count = 0;
        parser->input_first_line = parser->line_marks[owner->content_map.first].line;
        parser->line_number = parser->input_first_line - 1;
        parser->last_line_length = 0;
        owner->flags |= MARKDOWN_CORE_NODE__OPEN;
        S_parse_source(parser, owner->content.ptr, (size_t)owner->content.size);
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

markdown_core_node *markdown_core_parse_document(const char *buffer, size_t len) {
    return markdown_core_parse_document_with_setup(buffer, len, NULL, NULL);
}

markdown_core_node *markdown_core_parse_document_with_setup(const char *source, size_t length,
                                                            markdown_core_parser_setup_func setup, void *context) {
    static const unsigned char empty[] = "";
    markdown_core_parser *parser;
    markdown_core_node *document;

    if ((!source && length != 0) || length > (size_t)(INT32_MAX / 2)) {
        return NULL;
    }
    parser = S_parser_new();
    if (!parser) {
        return NULL;
    }
    parser->elements = markdown_core_core_elements(&parser->element_count);
    for (size_t i = 0; i < parser->element_count; i++) {
        S_register_element(parser, parser->elements[i]);
    }
    /* Setup borrows a fully initialized root, never a failed transaction. */
    if (parser->error || (setup && !setup(parser, context))) {
        S_parser_free(parser);
        return NULL;
    }
    /* After setup, because setup is the only thing that may still add an
     * element, and before the source stage, because that is the first reader. */
    S_project_block_hooks(parser);

    S_parse_source(parser, source ? (const unsigned char *)source : empty, length);
    document = S_finish_parse(parser);
    S_parser_free(parser);
    return document;
}

/* Materializing a normalized view is an allocation boundary, separate from
 * the ordinary borrowed view resolved inline by both consumers. */
static const unsigned char *S_normalize_input_line(markdown_core_parser *parser, const markdown_core_input_line *line,
                                                   markdown_core_line_facts *facts, bufsize_t length) {
    markdown_core_normalized_line *view = markdown_core_alloc(1, sizeof(*view) + (size_t)length + 2);
    if (!view) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    unsigned char *out = view->bytes;
    for (size_t at = line->start; at < line->end; at++) {
        unsigned char byte = parser->input_source[at];
        if (byte) {
            *out++ = byte;
        } else {
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
        return parser->input_source + line->start;
    }
    if (nul_count > ((size_t)(INT32_MAX / 2) - size) / 2) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    *length += (bufsize_t)(2 * nul_count);
    return facts->normalized ? facts->normalized->bytes : S_normalize_input_line(parser, line, facts, *length);
}

/* The sole physical-line scanner for root and mapped inputs. Grammar facts
 * live beside their line, so changing inputs drops them together. Inlining
 * is explicit: both GCC and Clang may otherwise outline this per-line step. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_input_line *S_extend_source_lines(markdown_core_parser *parser, size_t index) {
    assert(parser->input_length <= (size_t)(INT32_MAX / 2));
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
        const unsigned char *source = parser->input_source;
        const unsigned char *cursor = source + entry.start;
        const unsigned char *end = source + parser->input_length;
        /* Scan spans ending at a line boundary or a normalization boundary.
         * The NUL count changes only at the latter, not on every source byte. */
        for (;;) {
            for (; cursor < end; cursor++) {
                if (markdown_core_is_line_end(*cursor) || *cursor == 0) {
                    break;
                }
            }
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
        }
        parser->input_scanned = (size_t)(cursor - source);
        parser->input_line_work += parser->input_scanned - entry.start;
        parser->input_lines[parser->input_line_count++] = entry;
    }
    return index < parser->input_line_count ? &parser->input_lines[index] : NULL;
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

static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length) {
    if (length > (size_t)(INT32_MAX / 2)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    S_clear_normalized_lines(parser);
    parser->input_source = source;
    parser->input_length = length;
    parser->input_scanned = 0;
    parser->input_line_count = 0;
    parser->input_fact_count = 0;
    parser->input_first_line = parser->line_number + 1;
    parser->lookahead_last_line_ready = false;
    parser->lookahead_end = source + length;
    if (parser->block_root == parser->root) {
        parser->document_structure->read_document_prefix(parser, source, length);
    }
    while (!parser->error) {
        size_t index = (size_t)(parser->line_number + 1 - parser->input_first_line);
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
        S_process_line(parser, content, content_length);
        if (parser->claimed_cursor) {
            assert(parser->claimed_cursor >= source + next && parser->claimed_cursor <= source + length);
            parser->line_number = parser->claimed_line;
            parser->last_line_length = parser->claimed_last_column;
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
        const markdown_core_element *structure = markdown_core_node_structure(parser->lookahead_chain[i]);
        if (!structure || !structure->blank_runs) {
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
    while (lookahead->cursor && lookahead->cursor < end && !parser->error) {
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
        next = parser->input_source + markdown_core_input_line_next(parser, geometry);
        parser->block_lookahead_work++;
        if (geometry->facts && parser->input_facts[geometry->facts - 1].nul_count) {
            input.data = (unsigned char *)content;
            input.len = content_length + 1;
        } else if (next == end) {
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
        const markdown_core_element *structure = markdown_core_node_structure(node);
        unsigned mask = structure ? structure->speculative_flags : 0;
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

/* Whether `element` implements `hook`. The PROBE family carries a second
 * static condition: `markdown_core_parser_has_block_start` only ever asked a
 * probe that also interrupts a paragraph, and that is a descriptor property,
 * so it belongs in the projection rather than in the loop. */
static bool S_element_implements(const markdown_core_element *element, markdown_core_block_hook hook) {
    switch (hook) {
    case MARKDOWN_CORE_BLOCK_HOOK_SCAN:
        return element->scan_block_start != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT:
        return element->try_interrupting_block != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_OPEN:
        return element->try_opening_block != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH:
        return element->try_opening_paragraph != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_PROBE:
        return element->probe_block != NULL && element->interrupts_paragraph;
    case MARKDOWN_CORE_BLOCK_HOOK_COUNT:
        break;
    }
    return false;
}

/* The gate `element` declares for `hook`, or an ungated one. A family gains a
 * gate by adding a descriptor field and a case here; the dispatcher below does
 * not change and never learns an element's name. */
static markdown_core_block_gate S_element_gate(const markdown_core_element *element, markdown_core_block_hook hook) {
    markdown_core_block_gate ungated = {NULL};

    switch (hook) {
    case MARKDOWN_CORE_BLOCK_HOOK_SCAN:
        return element->scan_block_gate;
    case MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT:
        return element->interrupt_block_gate;
    case MARKDOWN_CORE_BLOCK_HOOK_OPEN:
        return element->open_block_gate;
    case MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH:
    case MARKDOWN_CORE_BLOCK_HOOK_PROBE:
    case MARKDOWN_CORE_BLOCK_HOOK_COUNT:
        break;
    }
    return ungated;
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
 * Each list is a count followed by owner indices, `count + 1` bytes, so a
 * family's table is `BLOCK_GATE_KEYS * (owners + 1)` bytes. */
#define BLOCK_GATE_KEY_NONE 256
#define BLOCK_GATE_KEY_INDENTED 257
#define BLOCK_GATE_KEYS 258

/* The owners of `hook`'s family a line with key `key` is asked, and how many;
 * NULL with `count` = the whole family when the family has no table. */
static const uint8_t *S_gate_candidates(const markdown_core_parser *parser, markdown_core_block_hook hook, int key,
                                        size_t *count) {
    const uint8_t *table = parser->block_gate_lists[hook];
    if (!table) {
        *count = parser->block_hook_counts[hook];
        return NULL;
    }
    const uint8_t *list = table + (size_t)key * (parser->block_hook_counts[hook] + 1);
    *count = *list;
    return list + 1;
}

/* The key of the line at `first` for a family that asks an indented line by
 * indent bound rather than by byte (scan and interrupt). */
static int S_gate_key(const markdown_core_chunk *input, int first, int indent) {
    if (indent >= CODE_INDENT) {
        return BLOCK_GATE_KEY_INDENTED;
    }
    return first < input->len ? (int)(unsigned char)input->data[first] : BLOCK_GATE_KEY_NONE;
}

/* Project the registry into one list per block-start hook family, in
 * descriptor order, once per parse. Built after setup has attached everything,
 * because an extension element must appear in the same families as a core one.
 *
 * One allocation holds all five lists back to back: the families are read
 * together, once per line, and separate blocks would scatter them. Failure
 * leaves every list empty, which parses as "no element opens a block" rather
 * than as a wrong grammar, and is reported through parser->error. */
static bool S_element_implements_inline(const markdown_core_element *element, markdown_core_inline_hook hook) {
    switch (hook) {
    case MARKDOWN_CORE_INLINE_HOOK_INIT:
        return element->init_inline != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_FINISH:
        return element->finish_inline != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_DISPOSE:
        return element->dispose_inline != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_COUNT:
        break;
    }
    return false;
}

/* The (event, kind) keys a finish step's declaration projects to, counted
 * into `counts`: the EXIT of each kind it is asked at, and the ENTER and EXIT
 * of each kind whose extent it tracks. An element without a step projects to
 * none. Registration refused a step asked at no kind and a kind outside the
 * table (S_element_rejection), so every key counted here is one the dispatch
 * indexes and none is the out-of-table key. Returns how many keys the element
 * added. */
static size_t S_count_finish_keys(const markdown_core_element *element, size_t *counts) {
    size_t keys = 0;
    if (!element->finish_step) {
        return 0;
    }
    for (const markdown_core_node_type *kind = element->finish_exit_kinds; kind && *kind; kind++) {
        counts[markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind)]++;
        keys++;
    }
    for (const markdown_core_node_type *kind = element->finish_scope_kinds; kind && *kind; kind++) {
        counts[markdown_core_finish_key(MARKDOWN_CORE_EVENT_ENTER, *kind)]++;
        counts[markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind)]++;
        keys += 2;
    }
    return keys;
}

/* Append `element` under `key`, once: a kind written twice in one list is
 * one declaration, and the step is asked once per event. `acts_on` is the
 * element's declared acted-on kinds as a set and `gated` whether this entry
 * reads it (markdown_core_finish_step_entry). */
static void S_append_finish_step(markdown_core_parser *parser, markdown_core_finish_step_entry **next, size_t key,
                                 const markdown_core_element *element, size_t slot, markdown_core_node_kind_set acts_on,
                                 bool gated) {
    if (next[key] != parser->finish_dispatch[key] && next[key][-1].element == element) {
        return;
    }
    *next[key]++ = (markdown_core_finish_step_entry){element, slot, acts_on, gated};
}

/* THE WALK'S PER-KIND RECORD (parser.h, markdown_core_finish_kind): one
 * record per kind index, each fact a constant of the kind's structure
 * element (element.h). Projected here beside the step lists so that the walk
 * reads one record where it asked three descriptor fields per event. */
static void S_project_finish_kinds(markdown_core_parser *parser) {
    for (size_t index = 0; index <= MARKDOWN_CORE_FINISH_KIND_COUNT; index++) {
        markdown_core_finish_kind record = {NULL, 0};
        if (index < MARKDOWN_CORE_FINISH_KIND_COUNT) {
            markdown_core_node_type kind = index < MARKDOWN_CORE_NODE_KIND_COUNT
                                               ? (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | index)
                                               : (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE |
                                                                           (index - MARKDOWN_CORE_NODE_KIND_COUNT));
            const markdown_core_element *structure = markdown_core_structure_for_kind(kind);
            if (structure) {
                if (structure->inline_content || structure->contains_inlines_func) {
                    record.flags |= MARKDOWN_CORE_FINISH_KIND_PARSES;
                }
                if (structure->deferred_inlines) {
                    record.flags |= MARKDOWN_CORE_FINISH_KIND_DEFERRED;
                }
                record.complete = structure->complete_inline;
            }
            if (markdown_core_kind_owns_fields(kind)) {
                record.flags |= MARKDOWN_CORE_FINISH_KIND_FIELDS;
            }
        }
        parser->finish_kinds[index] = record;
    }
}

static void S_project_block_hooks(markdown_core_parser *parser) {
    size_t totals[MARKDOWN_CORE_BLOCK_HOOK_COUNT] = {0};
    size_t inline_totals[MARKDOWN_CORE_INLINE_HOOK_COUNT] = {0};
    size_t total = 0, step_total = 0;

    markdown_core_free(parser->block_hook_allocation);
    parser->block_hook_allocation = NULL;
    memset(parser->block_hooks, 0, sizeof(parser->block_hooks));
    memset(parser->block_hook_counts, 0, sizeof(parser->block_hook_counts));
    memset(parser->block_gate_lists, 0, sizeof(parser->block_gate_lists));
    memset(parser->inline_hooks, 0, sizeof(parser->inline_hooks));
    memset(parser->inline_hook_counts, 0, sizeof(parser->inline_hook_counts));
    memset(parser->finish_dispatch, 0, sizeof(parser->finish_dispatch));
    parser->finish_step_slots = 0;
    S_project_finish_kinds(parser);
    /* What a container continuation may strip, as one table over the byte:
     * indentation, which every continuation strips, and the bytes each
     * container element declares for its own. */
    memset(parser->container_prefix, 0, sizeof(parser->container_prefix));
    parser->container_prefix[' '] = parser->container_prefix['\t'] = true;
    for (size_t i = 0; i < parser->element_count; i++) {
        const char *bytes = parser->elements[i]->container_prefix_bytes;
        for (const unsigned char *c = (const unsigned char *)bytes; bytes && *c; c++) {
            parser->container_prefix[*c] = true;
        }
    }

    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        for (size_t i = 0; i < parser->element_count; i++) {
            if (S_element_implements(parser->elements[i], (markdown_core_block_hook)hook)) {
                totals[hook]++;
            }
        }
        total += totals[hook];
    }
    /* The inline-content families and the finish steps share this one
     * allocation rather than taking their own. Not tidiness: a second small
     * block here lands between a grown-by-realloc parse buffer and its
     * headroom, and the realloc that used to extend in place starts copying
     * instead. On `chain-link-candidates` that showed up as +466,301 Ir of
     * `memcpy` for a projection that saves that document almost nothing. */
    for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
        for (size_t i = 0; i < parser->element_count; i++) {
            if (S_element_implements_inline(parser->elements[i], (markdown_core_inline_hook)hook)) {
                inline_totals[hook]++;
            }
        }
        total += inline_totals[hook];
    }
    /* The finish steps by key: a count per key, then each declared key's list
     * laid out in descriptor order with a terminator, then the table. */
    size_t key_counts[MARKDOWN_CORE_FINISH_KEY_COUNT] = {0};
    for (size_t i = 0; i < parser->element_count; i++) {
        step_total += S_count_finish_keys(parser->elements[i], key_counts);
    }
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        step_total += key_counts[key] != 0; /* the terminator */
    }
    /* The gate tables share the allocation too (see below): a family with no
     * declared gate keeps no table and every owner is asked, so adding the
     * first declaration to a family is what turns gating on for it -- an
     * element that declares nothing is never skipped. The count is taken
     * here, before the lists exist, from the same membership test. */
    size_t table_bytes = 0;
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        bool declared = false;
        for (size_t i = 0; i < parser->element_count; i++) {
            const markdown_core_element *element = parser->elements[i];
            if (S_element_implements(element, (markdown_core_block_hook)hook) &&
                S_element_gate(element, (markdown_core_block_hook)hook).bytes) {
                declared = true;
            }
        }
        if (declared) {
            table_bytes += BLOCK_GATE_KEYS * (totals[hook] + 1);
        }
    }
    if (!total && !step_total) {
        return;
    }

    /* The element-pointer lists first, then the step entries, then the gate
     * tables: the first two are pointer-aligned, so the second region starts
     * where the first ends, and the tables are bytes with no alignment of
     * their own. One block rather than three: the projection has one
     * lifecycle, and one allocation is that lifecycle made literal. */
    size_t pointer_bytes = total * sizeof(const markdown_core_element *);
    size_t step_bytes = step_total * sizeof(markdown_core_finish_step_entry);
    void *block = markdown_core_alloc(1, pointer_bytes + step_bytes + table_bytes);
    if (!block) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    parser->block_hook_allocation = block;
    uint8_t *tables = (uint8_t *)block + pointer_bytes + step_bytes;
    const markdown_core_element **entries = block;
    markdown_core_finish_step_entry *steps = (markdown_core_finish_step_entry *)((char *)block + pointer_bytes);

    markdown_core_finish_step_entry *next[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t step_at = 0;
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        if (!key_counts[key]) {
            next[key] = NULL;
            continue;
        }
        parser->finish_dispatch[key] = next[key] = steps + step_at;
        step_at += key_counts[key] + 1;
        steps[step_at - 1] = (markdown_core_finish_step_entry){NULL, 0, {0, 0}, false};
    }
    assert(step_at == step_total);
    for (size_t i = 0; i < parser->element_count; i++) {
        const markdown_core_element *element = parser->elements[i];
        size_t slot = parser->finish_step_slots;
        bool projected = false;
        if (!element->finish_step) {
            continue;
        }
        /* The gate: the acted-on kinds as a set, read at an entry whose
         * asked-at kind is not one of them -- at one that is, the node being
         * exited is the proof the parse produced one. */
        markdown_core_node_kind_set acts_on = {0, 0};
        for (const markdown_core_node_type *kind = element->finish_acts_on_kinds; kind && *kind; kind++) {
            markdown_core_node_kind_set_add(&acts_on, *kind);
        }
        for (const markdown_core_node_type *kind = element->finish_exit_kinds; kind && *kind; kind++) {
            markdown_core_node_kind_set asked = {0, 0};
            markdown_core_node_kind_set_add(&asked, *kind);
            bool gated = element->finish_acts_on_kinds && !markdown_core_node_kind_set_intersects(&acts_on, &asked);
            S_append_finish_step(parser, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind), element, slot,
                                 acts_on, gated);
            projected = true;
        }
        for (const markdown_core_node_type *kind = element->finish_scope_kinds; kind && *kind; kind++) {
            S_append_finish_step(parser, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_ENTER, *kind), element,
                                 slot, acts_on, false);
            S_append_finish_step(parser, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind), element, slot,
                                 acts_on, false);
            projected = true;
        }
        /* A state word per element that projected anything, so the slot an
         * entry names is always one the walk keeps. */
        parser->finish_step_slots += projected;
    }
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        /* A kind written twice in one list was counted twice and appended
         * once; the terminator moves up to close the list where it ends. */
        if (next[key]) {
            *next[key] = (markdown_core_finish_step_entry){NULL, 0, {0, 0}, false};
        }
    }

    size_t at = 0;
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        parser->block_hooks[hook] = entries + at;
        parser->block_hook_counts[hook] = totals[hook];
        for (size_t i = 0; i < parser->element_count; i++) {
            if (S_element_implements(parser->elements[i], (markdown_core_block_hook)hook)) {
                entries[at++] = parser->elements[i];
            }
        }
    }
    for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
        parser->inline_hooks[hook] = entries + at;
        parser->inline_hook_counts[hook] = inline_totals[hook];
        for (size_t i = 0; i < parser->element_count; i++) {
            if (S_element_implements_inline(parser->elements[i], (markdown_core_inline_hook)hook)) {
                entries[at++] = parser->elements[i];
            }
        }
    }

    if (!table_bytes) {
        return;
    }

    size_t table_at = 0;
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        bool declared = false;
        for (size_t i = 0; i < totals[hook]; i++) {
            if (S_element_gate(parser->block_hooks[hook][i], (markdown_core_block_hook)hook).bytes) {
                declared = true;
            }
        }
        if (!declared) {
            continue;
        }
        size_t stride = totals[hook] + 1;
        uint8_t *table = tables + table_at;
        parser->block_gate_lists[hook] = table;
        /* Counts and indices are bytes: the attachment API bounds the
         * registry so that they fit (MARKDOWN_CORE_ELEMENT_LIMIT). */
        assert(totals[hook] <= MARKDOWN_CORE_ELEMENT_LIMIT);
        /* Owners are appended in family order, so each list keeps it. */
        for (size_t i = 0; i < totals[hook]; i++) {
            const markdown_core_element *element = parser->block_hooks[hook][i];
            markdown_core_block_gate gate = S_element_gate(element, (markdown_core_block_hook)hook);
            if (!gate.bytes) {
                for (size_t key = 0; key <= BLOCK_GATE_KEY_NONE; key++) {
                    uint8_t *list = table + key * stride;
                    list[1 + list[0]++] = (uint8_t)i;
                }
            } else {
                for (const unsigned char *c = (const unsigned char *)gate.bytes; *c; c++) {
                    uint8_t *list = table + *c * stride;
                    if (!list[0] || list[list[0]] != (uint8_t)i) {
                        list[1 + list[0]++] = (uint8_t)i;
                    }
                }
            }
            /* An indented line is decided by the indent bound alone. */
            if (element->maximum_block_indent >= CODE_INDENT) {
                uint8_t *list = table + BLOCK_GATE_KEY_INDENTED * stride;
                list[1 + list[0]++] = (uint8_t)i;
            }
        }
        table_at += BLOCK_GATE_KEYS * stride;
    }
}

/* Ask the scan family about the line, in family order, and leave what the
 * claiming owner wrote in `start`; `start->open` is NULL when none claimed.
 * The payload fields are the claiming owner's to write before its `open` reads
 * them, so nothing is cleared per line beyond the three the dispatcher reads. */
static bool scan_element_start(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    const markdown_core_element *const *owners = parser->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_SCAN];
    size_t count;
    const uint8_t *candidates = S_gate_candidates(parser, MARKDOWN_CORE_BLOCK_HOOK_SCAN,
                                                  S_gate_key(context->input, context->first, context->indent), &count);

    start->open = NULL;
    start->kind = MARKDOWN_CORE_NODE_NONE;
    start->matched = 0;
    for (size_t i = 0; i < count; i++) {
        const markdown_core_element *element = owners[candidates ? candidates[i] : i];
        /* The indent bound stays a runtime test rather than part of the
         * projection: `context->indent` is a property of the line, not of the
         * element set, so it cannot be folded into a table built once. */
        if (context->indent <= element->maximum_block_indent && element->scan_block_start(parser, context, start)) {
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
                                   .lazy = paragraph,
                                   .all_matched = true,
                                   .depth = 1};
    block_start start;
    if (scan_element_start(parser, &context, &start)) {
        return true;
    }
    if (parser->error) {
        return false;
    }
    const markdown_core_element *const *probes = parser->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_PROBE];
    size_t probe_count = parser->block_hook_counts[MARKDOWN_CORE_BLOCK_HOOK_PROBE];
    for (size_t element_index = 0; element_index < probe_count; element_index++) {
        const markdown_core_element *element = probes[element_index];
        if (element->probe_block(parser, input, first, indent, reader)) {
            return true;
        }
        if (parser->error) {
            break;
        }
    }
    return false;
}

/* The container's structure, resolved once per turn of the loop below and
 * read for the three facts the turn needs; the node-taking forms above serve
 * the callers that ask one question of one node. */
static bool S_structure_accepts_lines(const markdown_core_element *structure, markdown_core_node *node) {
    return structure && (structure->content_mode == MARKDOWN_CORE_CONTENT_LITERAL ||
                         (structure->accepts_lines_func && structure->accepts_lines_func(structure, node)));
}

static void open_new_blocks(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                            bool all_matched) {
    bool maybe_lazy = is_paragraph(parser->current);
    size_t depth = 0;
    const markdown_core_element *structure = markdown_core_node_structure(*container);

    while (!S_structure_accepts_lines(structure, *container)) {
        bool paragraph = structure && structure->paragraph;
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
        const markdown_core_element *const *interrupters = parser->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT];
        size_t interrupter_count;
        const uint8_t *interrupter_candidates =
            S_gate_candidates(parser, MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT,
                              S_gate_key(input, parser->first_nonspace, parser->indent), &interrupter_count);
        for (size_t element_index = 0; element_index < interrupter_count; element_index++) {
            const markdown_core_element *owner =
                interrupters[interrupter_candidates ? interrupter_candidates[element_index] : element_index];
            markdown_core_node *opened = owner->try_interrupting_block(parser, *container, input, maybe_lazy);
            if (parser->error) {
                return;
            }
            if (opened) {
                *container = opened;
                return;
            }
        }

        if (start.open) {
            if (!start.open(parser, container, input, &start)) {
                return;
            }
        } else {
            markdown_core_node *new_container = NULL;
            const markdown_core_element *const *openers = parser->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_OPEN];
            size_t opener_count;
            /* An opener is asked about an indented line too, and told so: its
             * key is the byte alone. */
            const uint8_t *opener_candidates = S_gate_candidates(
                parser, MARKDOWN_CORE_BLOCK_HOOK_OPEN, S_gate_key(input, parser->first_nonspace, 0), &opener_count);

            for (size_t element_index = 0; element_index < opener_count; element_index++) {
                const markdown_core_element *element =
                    openers[opener_candidates ? opener_candidates[element_index] : element_index];

                new_container = element->try_opening_block(element, parser->indent > element->maximum_block_indent,
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
                    const markdown_core_element *const *last_chance =
                        parser->block_hooks[MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH];
                    size_t last_chance_count = parser->block_hook_counts[MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH];
                    for (size_t element_index = 0; element_index < last_chance_count; element_index++) {
                        const markdown_core_element *element = last_chance[element_index];
                        new_container =
                            element->try_opening_paragraph(element, parser->indent > element->maximum_block_indent,
                                                           parser, *container, input->data, input->len);
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
         * opened -- and before `markdown_core_block_accepts_lines` breaks out. */

        structure = markdown_core_node_structure(*container);
        if (S_structure_accepts_lines(structure, *container) ||
            (structure && structure->content_mode == MARKDOWN_CORE_CONTENT_PROSE)) {
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
    markdown_core_node *last_matched_container;
    bool all_matched = true;
    markdown_core_node *container;
    markdown_core_chunk input;

    if (parser->error || parser->root == NULL) {
        return;
    }

    assert(parser->curline.size == 0);

    markdown_core_strbuf_put(&parser->curline, buffer, bytes);

    bytes = parser->curline.size;

    // ensure line ends with a newline:
    if (bytes == 0 || !markdown_core_is_line_end(parser->curline.ptr[bytes - 1])) {
        markdown_core_strbuf_putc(&parser->curline, '\n');
    }

    if (parser->curline.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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

    if (container == NULL || parser->error) {
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
    finish_roots *record;
} tree_phase_context;

static int walk_owned_definition(markdown_core_node **slot, void *context) {
    const tree_phase_context *phase = context;
    return walk_owned_trees(phase->parser, *slot, false, phase->phase, phase->context, phase->record, 0);
}

/* THE DEFINITIONS THE DOCUMENT OWNS AS ROOTS OF THEIR OWN -- the inline
 * footnote bodies, which have no parent -- walked from where the previous
 * call left off (node.h, markdown_core_visit_block_subtrees_since). A body
 * is registered at the end of the document's chain by the inline parse that
 * declares it, and the finish walk parses inline content as it goes, so
 * bodies appear while the content tree is walked: this runs before the tree,
 * for the bodies a heading's inlines declared during the document's
 * preparation, and again after it, until a call finds nothing new. A body's
 * own content is made by the parse that made the body, so a walk of a body
 * finds no body it did not already know; the loop is what makes that a fact
 * the caller need not rely on. */
static int S_walk_owned_definitions(markdown_core_parser *parser, tree_phase_context *phase,
                                    markdown_core_node *last[MARKDOWN_CORE_DOCUMENT_CHAINS]) {
    bool found;
    do {
        if (!markdown_core_visit_block_subtrees_since(parser->root, last, walk_owned_definition, phase, &found)) {
            return 0;
        }
    } while (found && !parser->error);
    return !parser->error;
}

/* THE FINISH STAGE'S ONE WALK. The definitions the document already owns as
 * independent roots come first, then the content tree, which still holds the
 * block definitions as children -- the document's finalization moves them
 * into their chains after this walk, and moves nothing else -- and then the
 * definitions the tree's own inline parsing declared. Every root is walked
 * once, with inline parsing and completion at each ENTER, the finish steps
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
    tree_phase_context phase_context = {parser, phase, context, record};
    markdown_core_node *last[MARKDOWN_CORE_DOCUMENT_CHAINS] = {NULL, NULL};
    return S_walk_owned_definitions(parser, &phase_context, last) &&
           walk_owned_trees(parser, root, true, phase, context, record, 1) &&
           S_walk_owned_definitions(parser, &phase_context, last);
}

/* Register at syntax commitment; no completed-tree discovery pass is needed.
 * Inline bodies enter the document's value chain immediately, and the index
 * borrows only until finalization, which follows the finish walk. */
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
    collection->values[collection->count++] = (markdown_core_definition_entry){definition, citation};
    parser->definition_registration_work++;
    if (inline_owner) {
        definition->prev = collection->last_inline;
        if (collection->last_inline) {
            collection->last_inline->next = definition;
        } else {
            *inline_owner = definition;
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

/* A STABLE LINEAR ORDERING BY SOURCE COORDINATE, keyed once.
 *
 * The key packs two nonnegative 32-bit coordinates, and a counting pass per
 * key byte orders any set of them in linear work with no comparison-sort worst
 * case. What is not constant is WHICH bytes carry information: the entries a
 * grid table closes, or the headings of one document, differ in their low line
 * and column bytes and agree on every other. A pass over a byte on which every
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
    const markdown_core_element **passes;
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
        const markdown_core_element *element = phases->passes[i];
        if (!element->postprocess_func(element, parser, root) || parser->error) {
            return 0;
        }
        MARKDOWN_CORE_CHECK_TREE(root);
    }
    return 1;
}

static int S_run_passes_on_slot(markdown_core_node **slot, void *context) {
    return S_run_passes(((tree_phase_context *)context)->parser, *slot, ((tree_phase_context *)context)->context);
}

static markdown_core_node *S_finish_parse(markdown_core_parser *parser) {
    markdown_core_node *res;

    if (parser->root == NULL || parser->error) {
        return NULL;
    }

    finalize_document(parser);
    S_parse_block_inputs(parser);
    if (!parser->error) {
        markdown_core_manage_elements_special_characters(parser, true);
        if (!parser->error) {
            parser->document_structure->prepare_document(parser);
        }
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
    bool passes_declared = false;
    for (size_t i = 0; i < parser->element_count; i++) {
        passes_declared |= parser->elements[i]->postprocess_func != NULL;
    }
    finish_roots record = {NULL, 0, 0};
    if (!S_apply_tree_phase(parser, parser->root, S_check_root, NULL, passes_declared ? &record : NULL)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_manage_elements_special_characters(parser, false);

    finish_phases phases = {NULL, 0};
    if (!parser->error && parser->element_count) {
        phases.passes = markdown_core_alloc(parser->element_count, sizeof(*phases.passes));
        if (!phases.passes) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    for (size_t i = 0; !parser->error && i < parser->element_count; i++) {
        const markdown_core_element *element = parser->elements[i];
        if (element->postprocess_func && S_finish_hook_selected(parser, element)) {
            phases.passes[phases.pass_count++] = element;
        }
    }

    /* The document's finalization reads the finished tree: the headings take
     * their anchors once every explicit anchor has been reserved, which the
     * walk did at each node's ENTER, and the block definitions move into the
     * chains the walk already visited the inline ones through. */
    if (!parser->error) {
        parser->document_structure->finish_document(parser);
    }

    /* Then the global passes, each on every finalized root: the chains first,
     * as the walk took them, then the field roots and the document in the
     * order the walk completed them. */
    if (!parser->error && phases.pass_count) {
        tree_phase_context pass_context = {parser, NULL, &phases, NULL};
        int ok = markdown_core_visit_block_subtrees(parser->root, S_run_passes_on_slot, &pass_context);
        for (size_t i = 0; ok && i < record.count; i++) {
            ok = S_run_passes(parser, record.roots[i], &phases);
        }
        if (!ok) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    markdown_core_free(record.roots);
    markdown_core_free((void *)phases.passes);
    if (parser->error) {
        goto failed;
    }

    res = parser->root;
    parser->root = NULL;
    return res;

failed:
    parser->document_structure->dispose_document(parser);
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

int markdown_core_parser_get_last_line_length(markdown_core_parser *parser) { return parser->last_line_length; }

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    markdown_core_block_advance_offset(parser, &input_chunk, count, columns != 0);
}

void markdown_core_parser_set_backslash_ispunct_func(markdown_core_parser *parser, markdown_core_ispunct_func func) {
    parser->backslash_ispunct = func;
}
