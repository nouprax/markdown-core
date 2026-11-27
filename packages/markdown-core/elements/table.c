#include "alloc.h"
#include <markdown-core-element-api.h>
#include "element.h"
#include <inlines.h>
#include <parser.h>
#include "labels.h"
#include <assert.h>
#include <string.h>
#include <limits.h>
#include "utf8.h"

#include "table.h"
#include "services.h"

#include "block_internal.h"
#include "markdown-core-elements.h"

// Limit to prevent a malicious input from causing a denial of service.
#define MAX_AUTOCOMPLETED_CELLS 0x80000

// Custom node flag, initialized in `create_table_element`.
/* The one element node flag, as a COMPILE-TIME CONSTANT. It used to be a
 * zero-initialised global filled in by `markdown_core_register_node_flag`,
 * which aborts if it is called twice and hands out bits in call order. One
 * bit, one owner, one value known at compile time (Q16). */
enum { MARKDOWN_CORE_NODE__TABLE_VISITED = MARKDOWN_CORE_NODE__ELEMENT_FIRST };

typedef struct {
    /* Borrowed trimmed authored bytes, valid for this row parse only. */
    markdown_core_chunk content;
    int start_offset, end_offset, internal_offset;
} node_cell;

static void free_node_table(markdown_core_table *table) {
    if (!table) {
        return;
    }
    if (!table->views_columns) {
        markdown_core_free(table->columns);
    }
    markdown_core_free(table);
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) markdown_core_node *table_child(markdown_core_parser *parser,
                                                                                       markdown_core_node_type kind,
                                                                                       markdown_core_place place) {
    markdown_core_node *node = markdown_core_parser_make_node(parser, kind);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    if (kind == MARKDOWN_CORE_NODE_TABLE_ROW || kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        markdown_core_node_set_element(node, &MARKDOWN_CORE_ELEMENT_TABLE);
    }
    node->place = place;
    return node;
}

/* A pipe cell is made whole, from its row's line, by the table that owns
 * it: like every row and cell but a pipe body row, it is never an open block.
 * Its end is its caller's to set. THE ROWS AND CELLS A TABLE MAKES settle as
 * every block of the tree does (block_internal.h, markdown_core_block_settle),
 * each once it is whole: a cell into its row once its line is read, a row
 * made whole into its table after its cells, and a pipe body row, an open
 * block, as the table takes its next row or closes. */
static markdown_core_node *new_cell(markdown_core_parser *parser, markdown_core_reader *reader, bufsize_t at) {
    return table_child(parser, MARKDOWN_CORE_NODE_TABLE_CELL, markdown_core_reader_span(reader, parser, at, at + 1));
}

/* A PIPE CELL HOLDS ITS BYTES, whichever path found its row: trimmed of the
 * whitespace around them, its escaped pipes contracted, a tab kept a tab.
 * Assemble every cell once, preserving a source run at each contraction.
 * A logical pipe represents both authored bytes of its escape. There is no
 * alternate inline parser and no position repair after parsing. `offset`
 * indexes the content of `source`, whose map places it, when the row was
 * recovered from a paragraph, and otherwise the reader's line. */
static void set_cell_content(markdown_core_parser *parser, markdown_core_reader *reader, markdown_core_node *node,
                             const node_cell *cell, markdown_core_node *source, bufsize_t offset) {
    node->internal_offset = cell->internal_offset;
    for (bufsize_t from = 0; from < cell->content.len && !parser->error;) {
        bufsize_t to = from;
        bool escaped =
            cell->content.data[from] == '\\' && from + 1 < cell->content.len && cell->content.data[from + 1] == '|';
        if (escaped) {
            int line = reader->number;
            markdown_core_place place;
            if (source) {
                line = markdown_core_text_line(parser, &source->content_map, offset + from);
                markdown_core_text_place(parser, &source->content_map, offset + from, offset + from + 2, &place);
            } else {
                place = markdown_core_reader_span(reader, parser, offset + from, offset + from + 2);
            }
            const int width = (int)(place.end - place.start);
            markdown_core_parser_append_content_mark(parser, node, node->content.size, line, place.start, width, width);
            markdown_core_strbuf_putc(&node->content, '|');
            to = from + 2;
        } else {
            do {
                to++;
            } while (to < cell->content.len && !(cell->content.data[to] == '\\' && to + 1 < cell->content.len &&
                                                 cell->content.data[to + 1] == '|'));
            if (source) {
                markdown_core_text_append(parser, &source->content_map, &node->content_map, offset + from, to - from,
                                          node->content.size);
            } else {
                markdown_core_reader_marks(reader, parser, node, offset + from, to - from, node->content.size);
            }
            markdown_core_strbuf_put(&node->content, cell->content.data + from, to - from);
        }
        if (node->content.oom) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        from = to;
    }
}

/* THE ROW (micromark-extension-gfm-table, code by code): a line of cells
 * between pipes, from its first byte that is no space to its ending, as a
 * row token holding a token for each pipe that divides it. A pipe a
 * backslash escapes is a cell's byte, and a row has a cell. The delimiter row is a row whose cells are each a
 * dash run, a colon at either end of it, and spaces around it.
 *
 * The frame's words: the cells counted, whether a cell is open and holds a
 * byte that is no space, the row token's kind and, in a delimiter row,
 * where its cell is. */
enum { TABLE_HEAD, TABLE_ROW, TABLE_PIPE, TABLE_BLOCKS };

/* What a table's start found: the cells of its delimiter row, and where
 * the header row starts in the paragraph's text. */
typedef struct {
    uint32_t columns;
    bufsize_t header;
} table_found;
MARKDOWN_CORE_BLOCK_FOUND_FITS(table_found, table_found_fits);
enum { ROW_CELLS, ROW_OPEN, ROW_FILLED, ROW_KIND, ROW_PHASE };
/* Where a delimiter cell is: before its marker, past its opening colon, in
 * its dashes, past them. */
enum { MARK_BEFORE, MARK_COLON, MARK_DASHES, MARK_AFTER };

static markdown_core_state S_head_yes(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_head_no(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_row_cell(markdown_core_tokenizer *tokenizer, int code);
static markdown_core_state S_mark_cell(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_row_end(markdown_core_tokenizer *tokenizer) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (!words[ROW_CELLS] || words[ROW_CELLS] > UINT16_MAX) {
        return markdown_core_nok(tokenizer);
    }
    /* A delimiter row's count outlives its frame in the start's record. */
    if (words[ROW_KIND] == markdown_core_block_token(tokenizer, TABLE_HEAD)) {
        MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, table_found)->columns =
            words[ROW_CELLS];
    }
    markdown_core_exit(tokenizer, (uint16_t)words[ROW_KIND]);
    return markdown_core_ok(tokenizer);
}

/* A pipe ends the cell open before it and opens the next. */
static void S_row_pipe(markdown_core_tokenizer *tokenizer) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    words[ROW_CELLS] += words[ROW_OPEN];
    words[ROW_OPEN] = 1;
    words[ROW_FILLED] = 0;
    const uint16_t pipe = markdown_core_block_token(tokenizer, TABLE_PIPE);
    markdown_core_enter(tokenizer, pipe);
    markdown_core_consume(tokenizer);
    markdown_core_exit(tokenizer, pipe);
}

/* A pipe right after a backslash is the cell's, whatever is before it. */
static markdown_core_state S_row_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '|') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_row_cell);
    }
    return S_row_cell(tokenizer, code);
}

static markdown_core_state S_row_cell(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (markdown_core_code_ending(code)) {
        words[ROW_CELLS] += words[ROW_OPEN] && words[ROW_FILLED];
        return S_row_end(tokenizer);
    }
    if (code == '|') {
        S_row_pipe(tokenizer);
        return markdown_core_state_of(S_row_cell);
    }
    words[ROW_FILLED] |= !markdown_core_code_space(code);
    markdown_core_consume(tokenizer);
    return code == '\\' ? markdown_core_state_of(S_row_escape) : markdown_core_state_of(S_row_cell);
}

static markdown_core_state S_row_first(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_enter(tokenizer, (uint16_t)words[ROW_KIND]);
    if (code == '|') {
        S_row_pipe(tokenizer);
        return markdown_core_state_of(S_row_cell);
    }
    words[ROW_OPEN] = 1;
    return S_row_cell(tokenizer, code);
}

static markdown_core_state S_row(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_frame_local(tokenizer)[ROW_KIND] = markdown_core_block_token(tokenizer, TABLE_ROW);
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_row_first), 0, UINT32_MAX, code);
}

static const markdown_core_construct TABLE_ROW_READ = {.start = S_row};

static markdown_core_state S_mark_cell(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    const uint32_t phase = words[ROW_PHASE];
    const bool marked = phase == MARK_DASHES || phase == MARK_AFTER;
    if (markdown_core_code_ending(code)) {
        if (!marked && phase != MARK_BEFORE) {
            return markdown_core_nok(tokenizer);
        }
        words[ROW_CELLS] += marked;
        return S_row_end(tokenizer);
    }
    if (code == '|') {
        if (!marked) {
            return markdown_core_nok(tokenizer);
        }
        words[ROW_OPEN] = 1;
        S_row_pipe(tokenizer);
        words[ROW_PHASE] = MARK_BEFORE;
        return markdown_core_state_of(S_mark_cell);
    }
    if (markdown_core_code_space(code)) {
        if (phase == MARK_COLON) {
            return markdown_core_nok(tokenizer);
        }
        words[ROW_PHASE] = phase == MARK_BEFORE ? MARK_BEFORE : MARK_AFTER;
    } else if (code == ':' && (phase == MARK_BEFORE || phase == MARK_DASHES)) {
        words[ROW_PHASE] = phase == MARK_BEFORE ? MARK_COLON : MARK_AFTER;
    } else if (code == '-' && phase != MARK_AFTER) {
        words[ROW_PHASE] = MARK_DASHES;
    } else {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_mark_cell);
}

static markdown_core_state S_mark_first(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    markdown_core_enter(tokenizer, (uint16_t)words[ROW_KIND]);
    words[ROW_PHASE] = MARK_BEFORE;
    if (code == '|') {
        S_row_pipe(tokenizer);
        return markdown_core_state_of(S_mark_cell);
    }
    return S_mark_cell(tokenizer, code);
}

/* THE CELLS OF A ROW, from the row token at `row` of `events` and the
 * pipe tokens after it, over the bytes `data` its offsets index: the bytes
 * between two pipes are a cell, those before the first one when the row
 * does not start with it, and those after the last one when they hold a
 * byte that is no space. A cell starts past the pipe before it, its
 * content past the spaces after that pipe; its content is trimmed. */
typedef struct {
    const unsigned char *data;
    const markdown_core_token *tokens;
    size_t next, count;
    uint16_t pipe;
    uint32_t at, end;
} S_cells;

static S_cells S_cells_of(const markdown_core_events *events, size_t row, uint16_t pipe, const unsigned char *data) {
    S_cells cells = {.data = data,
                     .tokens = events->tokens,
                     .next = row + 1,
                     .count = events->token_count,
                     .pipe = pipe,
                     .at = events->tokens[row].start,
                     .end = events->tokens[row].end};
    if (cells.next < cells.count && cells.tokens[cells.next].kind == pipe &&
        cells.tokens[cells.next].start == cells.at) {
        cells.at = cells.tokens[cells.next++].end;
    }
    return cells;
}

/* The next cell, its offsets `base` before the data's. */
static bool S_cells_next(S_cells *cells, node_cell *cell, uint32_t base) {
    const bool piped = cells->next < cells->count && cells->tokens[cells->next].kind == cells->pipe &&
                       cells->tokens[cells->next].start < cells->end;
    const uint32_t from = cells->at, to = piped ? cells->tokens[cells->next].start : cells->end;
    if (!piped && from >= cells->end) {
        return false;
    }
    uint32_t inner = from;
    while (inner < to && markdown_core_is_space_or_tab(cells->data[inner])) {
        inner++;
    }
    if (!piped && inner == to) {
        cells->at = cells->end;
        return false;
    }
    *cell = (node_cell){.content = {(unsigned char *)cells->data + inner, (bufsize_t)(to - inner), 0},
                        .start_offset = (int)(from - base),
                        .end_offset = (int)((to > from ? to - 1 : from) - base),
                        .internal_offset = (int)(inner - from)};
    markdown_core_chunk_trim(&cell->content);
    cells->at = piped ? cells->tokens[cells->next++].end : cells->end;
    return true;
}

/* The row the `size` bytes at `data` start with, read alone (THE ROW):
 * false when they are none, the reading `alone` then ended. */
static bool S_row_alone(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_alone *alone, const unsigned char *data, bufsize_t size) {
    markdown_core_construct row = TABLE_ROW_READ;
    row.context = self;
    *alone = (markdown_core_alone){.turn = {.parser = parser}};
    if (!markdown_core_alone_read(parser, alone, &row, data, (uint32_t)size) || !alone->found) {
        markdown_core_alone_end(parser, alone);
        return false;
    }
    return true;
}

/* Give `node` the source span of [start_offset, end_offset] in `owner`'s
 * content buffer. Every table position recovered from that buffer goes through
 * here, so there is one place that knows a content offset is not a column. */
static void S_place_content_span(markdown_core_parser *parser, markdown_core_node *owner, markdown_core_node *node,
                                 bufsize_t start_offset, bufsize_t end_offset) {
    markdown_core_place place;
    if (markdown_core_text_place(parser, &owner->content_map, start_offset, start_offset, &place)) {
        node->place.start = place.start;
    }
    if (markdown_core_text_place(parser, &owner->content_map, end_offset, end_offset + 1, &place)) {
        node->place.end = place.end;
    }
}

/* THE LEAD (docs/plans/2026-09-29-incremental-parsing.md, 5.7): the paragraph
 * whose last line is a table's header keeps the `lead` bytes of its text
 * before that line and closes where they end, as a later block start would
 * close it -- reference definitions, anchor attachment and the runs of its
 * own lines share the close -- and a paragraph whose text was the header
 * alone yields no node. The lead keeps its authored spelling: a pipe a cell
 * escapes is a cell's transformation, and the lead is no cell, so `pre \\|
 * lead` above a table keeps both of its bytes. The paragraph is the current
 * block. */
static void S_close_lead(markdown_core_parser *parser, markdown_core_node *paragraph, markdown_core_chunk text,
                         bufsize_t lead) {
    bufsize_t first = 0;
    bufsize_t scope_end = lead;
    markdown_core_place place;

    assert(parser->at.current == paragraph);
    while (first < lead && markdown_core_is_whitespace(text.data[first])) {
        first++;
    }
    /* The close reads the lead with its last line ending, which ends the
     * last reference definition; the authored scope omits line endings. */
    while (scope_end > first && (text.data[scope_end - 1] == '\n' || text.data[scope_end - 1] == '\r')) {
        scope_end--;
    }
    if (scope_end > first &&
        markdown_core_text_place(parser, &paragraph->content_map, scope_end - 1, scope_end, &place)) {
        paragraph->place.end = place.end;
    }
    /* What is cut from the front moves the paragraph's start, which its
     * close reads from the map. */
    markdown_core_text_share(parser, &paragraph->content_map, &paragraph->content_map, first, lead - first);
    paragraph->cut_front += first;
    paragraph->cut_back += text.len - lead;
    markdown_core_block_close_current(parser);
}

/* The table a paragraph's last line heads, made from `text`, the
 * paragraph's text, whose header row starts at `header`: the row's bytes
 * are its content, and their marks its map, and it starts where the row was
 * written. NULL, with the parse failed, when an allocation failed. */
static markdown_core_node *S_header_table(markdown_core_parser *parser, const markdown_core_node *paragraph,
                                          markdown_core_chunk text, bufsize_t header) {
    markdown_core_node *table = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_TABLE);
    if (!table) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    markdown_core_strbuf_put(&table->content, text.data + header, text.len - header);
    if (table->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, table);
        return NULL;
    }
    markdown_core_text_share(parser, &paragraph->content_map, &table->content_map, header, text.len - header);
    markdown_core_text_place(parser, &paragraph->content_map, header, header, &table->place);
    return table;
}

/* THE TABLE A PARAGRAPH'S LAST LINE HEADS (5.7) is the block of that line
 * on, and the paragraph's object stays with the block that holds its first
 * line (5.9): a paragraph whose text was the header alone becomes the table,
 * as an underline makes a paragraph a Setext heading, and any other keeps
 * the `header` bytes of its text before the header (S_close_lead) as the
 * table opens after it, a block of its own. The table is the current block.
 * NULL, with the parse failed when an allocation failed, when no table
 * opened. */
static markdown_core_node *S_open_table(markdown_core_parser *parser, markdown_core_node *paragraph,
                                        markdown_core_node *parent, markdown_core_chunk text, bufsize_t header) {
    if (!header) {
        markdown_core_node_set_kind_result result =
            markdown_core_parser_set_node_kind(parser, paragraph, parent, MARKDOWN_CORE_NODE_TABLE);
        if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
            if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
            return NULL;
        }
        return paragraph;
    }
    /* A parent that holds no table refuses it before anything is made, so
     * the paragraph stays whole for the grammar after it; the change of kind
     * refuses it so for a paragraph that becomes the table. */
    if (!markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_TABLE)) {
        return NULL;
    }
    markdown_core_node *table = S_header_table(parser, paragraph, text, header);
    if (!table) {
        return NULL;
    }
    S_close_lead(parser, paragraph, text, header);
    if (parser->error) {
        markdown_core_parser_release_node(parser, table);
        return NULL;
    }
    return markdown_core_parser_open_child(parser, parent, table);
}

/* Where the last line of `text` starts. */
static bufsize_t S_last_line(markdown_core_chunk text) {
    bufsize_t end = text.len;
    if (end && text.data[end - 1] == '\n') {
        end--;
    }
    if (end && text.data[end - 1] == '\r') {
        end--;
    }
    while (end && text.data[end - 1] != '\n' && text.data[end - 1] != '\r') {
        end--;
    }
    return end;
}

/* The cells of the row the `size` bytes at `data` start with, 0 when they
 * are none. */
static uint32_t S_row_width(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            const unsigned char *data, bufsize_t size) {
    markdown_core_alone alone;
    if (!S_row_alone(self, parser, &alone, data, size)) {
        return 0;
    }
    uint32_t columns = 0;
    S_cells cells = S_cells_of(alone.events, 0, (uint16_t)(self->block_base + TABLE_PIPE), data);
    node_cell cell;
    while (S_cells_next(&cells, &cell, 0)) {
        columns++;
    }
    markdown_core_alone_end(parser, &alone);
    return columns;
}

/* A delimiter row as wide as its header, the last line of the paragraph's
 * text, opens the table. A header of another width marks the paragraph,
 * which no later line makes a table then. */
static markdown_core_state S_head_read(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_node *paragraph = turn->context->container;
    table_found *found = MARKDOWN_CORE_BLOCK_FOUND(turn->start, table_found);
    const markdown_core_chunk text = markdown_core_node_text(paragraph);
    found->header = S_last_line(text);
    const uint32_t width =
        S_row_width(markdown_core_self(tokenizer), turn->parser, text.data + found->header, text.len - found->header);
    if (turn->parser->error) {
        return markdown_core_nok(tokenizer);
    }
    if (width != found->columns) {
        paragraph->flags |= MARKDOWN_CORE_NODE__TABLE_VISITED;
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_head_row(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_frame_local(tokenizer)[ROW_KIND] = markdown_core_block_token(tokenizer, TABLE_HEAD);
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_mark_first), 0, UINT32_MAX, code);
}

static const markdown_core_construct TABLE_MARKS = {.start = S_head_row};

/* The marks counted, the header is read: the frame of the delimiter row
 * is gone, so its count is handed on in the start's record. */
static markdown_core_state S_head_marked(markdown_core_tokenizer *tokenizer, int code) {
    return S_head_read(tokenizer, code);
}

/* A delimiter row under a paragraph's last line opens the table. */
static markdown_core_state S_head_start(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_node *container = markdown_core_block_turn_of(tokenizer)->context->container;
    if (container->kind != MARKDOWN_CORE_NODE_PARAGRAPH || (container->flags & MARKDOWN_CORE_NODE__TABLE_VISITED)) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_attempt(tokenizer, &TABLE_MARKS, markdown_core_state_of(S_head_marked),
                                 markdown_core_state_of(S_head_no), code);
}

static markdown_core_node *S_head_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_node *paragraph, markdown_core_reader *reader,
                                       const markdown_core_compiler *compiler, uint32_t index,
                                       const table_found *found) {
    markdown_core_node *table_header;
    markdown_core_chunk text = markdown_core_node_text(paragraph);
    const uint16_t pipe = (uint16_t)(self->block_base + TABLE_PIPE);

    markdown_core_node *node =
        S_open_table(parser, paragraph, markdown_core_parser_parent(parser, paragraph), text, found->header);
    if (!node) {
        return NULL;
    }
    /* The header row and its cells are RECOVERED from the table's text,
     * the row as it was written, and every offset below is an offset into it.
     * Adding one to a column is only right while the buffer holds a single
     * line starting where the block does; with a lead split off, `| a | b |`
     * on line three was reported at 1:10, a column that is not on line one.
     * The map turns each offset back into the place it was written. */
    text = markdown_core_node_text(node);

    /* Table data belongs to the element. Its cleanup accepts partial
     * initialization when an allocation fails. */
    markdown_core_node_set_element(node, self->element);
    node->opaque = markdown_core_alloc(1, sizeof(markdown_core_table));
    if (!node->opaque) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);

        return node;
    }
    markdown_core_table *table = node->opaque;
    table->column_count = (uint16_t)found->columns;
    table->columns = markdown_core_alloc(table->column_count, sizeof(*table->columns));
    if (!table->columns) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);

        return node;
    }
    S_cells markers = S_cells_of(compiler->events, index, pipe, compiler->data);
    node_cell geometry;
    for (uint16_t i = 0; i < table->column_count && S_cells_next(&markers, &geometry, 0); ++i) {
        const markdown_core_chunk *cell = &geometry.content;
        bool left = cell->data[0] == ':', right = cell->data[cell->len - 1] == ':';
        table->columns[i].flow = left ? (right ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_LEFT)
                                      : (right ? MARKDOWN_CORE_FLOW_RIGHT : MARKDOWN_CORE_FLOW_NONE);
    }

    /* The header row is made whole from the table's content, as its cells
     * are: it is never an open block. */
    table_header = table_child(parser, MARKDOWN_CORE_NODE_TABLE_ROW, markdown_core_reader_span(reader, parser, 0, 1));
    if (!table_header) {

        return node;
    }
    S_place_content_span(parser, node, table_header, 0, text.len - 2);

    table->head_count = 1;

    markdown_core_alone alone;
    if (S_row_alone(self, parser, &alone, text.data, text.len)) {
        S_cells header = S_cells_of(alone.events, 0, pipe, text.data);
        while (S_cells_next(&header, &geometry, 0)) {
            const node_cell *cell = &geometry;
            markdown_core_node *header_cell = new_cell(parser, reader, 0);
            if (!header_cell) {
                break;
            }
            S_place_content_span(parser, node, header_cell, cell->start_offset, cell->end_offset);
            set_cell_content(parser, reader, header_cell, cell, node, (bufsize_t)(cell->content.data - text.data));
            markdown_core_block_settle(parser, header_cell, table_header);
        }
        markdown_core_alone_end(parser, &alone);
    }
    markdown_core_block_settle(parser, table_header, node);

    markdown_core_reader_end(reader);

    return node;
}

/* The cells a row writes past its table's width, from `start`, where its
 * last cell ends, to `end`, where its line's content ends: one field of no
 * slot kind, past the pipe after that cell, without the row's closing pipe
 * and the whitespace around them. */
static void S_extra_field(markdown_core_parser *parser, markdown_core_reader *reader, markdown_core_node *row,
                          bufsize_t start, bufsize_t end) {
    while (start < end && markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, start))) {
        start++;
    }
    if (start >= end || markdown_core_reader_peek(reader, start) != '|') {
        return;
    }
    start++;
    while (end > start && markdown_core_is_whitespace(markdown_core_reader_peek(reader, end - 1))) {
        end--;
    }
    if (end > start && markdown_core_reader_peek(reader, end - 1) == '|' &&
        !(end - 1 > start && markdown_core_reader_peek(reader, end - 2) == '\\')) {
        end--;
    }
    while (start < end && markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, start))) {
        start++;
    }
    while (end > start && markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, end - 1))) {
        end--;
    }
    const markdown_core_field_to fields = {parser, row, NULL, reader, 0, NULL};
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_NONE, start, end);
}

static markdown_core_node *S_row_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                      markdown_core_node *parent_container, markdown_core_reader *reader,
                                      const markdown_core_compiler *compiler, uint32_t index, uint32_t base) {
    markdown_core_node *table_row_block;
    markdown_core_table *table = parent_container->opaque;
    const markdown_core_token *token = &compiler->events->tokens[index];
    /* A pipe row begins at its own first non-space byte, as its header does:
     * `reader->offset` is the container's content, which holds the row's
     * indentation or the tail of a tab its container prefix shares. */
    /* The row is whole once its line is read: it settles into the table,
     * which stays the current block, and a reopen at the next row finds
     * none open inside it. */
    const bufsize_t first = (bufsize_t)(token->start - base), completed_at = (bufsize_t)(token->end - base);
    table_row_block = table_child(parser, MARKDOWN_CORE_NODE_TABLE_ROW,
                                  markdown_core_reader_span(reader, parser, first, completed_at));
    if (!table_row_block) {
        return NULL;
    }
    /* The count before the row, which the cap is no further past than one
     * row's width. */
    table_row_block->internal_offset = table->autocompleted_cells < INT_MAX ? (int)table->autocompleted_cells : INT_MAX;

    {
        int i, table_columns = (int)table->column_count;

        S_cells cells = S_cells_of(compiler->events, index, (uint16_t)(self->block_base + TABLE_PIPE), compiler->data);
        node_cell geometry;
        bufsize_t past = -1;
        for (i = 0; i < table_columns && S_cells_next(&cells, &geometry, base); ++i) {
            const node_cell *cell = &geometry;
            const bufsize_t from = cell->start_offset, to = cell->end_offset;
            past = to + 1;
            markdown_core_node *node = table_child(parser, MARKDOWN_CORE_NODE_TABLE_CELL,
                                                   markdown_core_reader_span(reader, parser, from, to + 1));
            if (!node) {
                break;
            }
            set_cell_content(parser, reader, node, cell, NULL, (bufsize_t)(cell->content.data - compiler->data - base));
            markdown_core_block_settle(parser, node, table_row_block);
        }

        table->autocompleted_cells += (size_t)(table_columns - i);
        if (i == table_columns && past >= 0) {
            S_extra_field(parser, reader, table_row_block, past, completed_at);
        }

        /* AUTOCOMPLETED CELLS SIT WHERE THEY WERE COMPLETED (Q44, answered
         * 2026-08-23). A row shorter than its header is completed to the
         * header's width, and the cells that completion invents were never
         * written -- they have no source bytes at all. They used to carry
         * `L:0..L:0`, and column 0 is not a byte.
         *
         * A scope is what a consumer follows to map a node back to the source,
         * so the answer is the place the completion happened: the end of the
         * row. That is the row's last byte, which the previous cell also ends
         * on -- an empty range there would need column len+1, which is off the
         * line. The overlap is the honest cost of pointing AT something rather
         * than at nothing, and it is registered in
         * specs/positions/containment.json rather than hidden. */
        for (; i < table_columns; ++i) {
            markdown_core_node *node = new_cell(parser, reader, completed_at - 1);
            if (!node) {
                break;
            }
            markdown_core_block_settle(parser, node, table_row_block);
        }
    }

    markdown_core_block_settle(parser, table_row_block, parent_container);
    markdown_core_reader_end(reader);

    return parent_container;
}

/* A pipe row in an open table that takes more rows is its next row. */
static markdown_core_state S_row_start(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_table *table = markdown_core_block_turn_of(tokenizer)->context->container->opaque;
    if (!table || table->autocompleted_cells > MAX_AUTOCOMPLETED_CELLS) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_attempt(tokenizer, &TABLE_ROW_READ, markdown_core_state_of(S_head_yes),
                                 markdown_core_state_of(S_head_no), code);
}

/* The delimiter row opens the table, or, refused, is the line's text. */
static void S_head(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_node *node =
        S_head_open(markdown_core_compile_self(compiler), turn->parser, *turn->container, turn->context->reader,
                    compiler, index, MARKDOWN_CORE_BLOCK_FOUND(turn->start, table_found));
    if (node) {
        *turn->container = node;
        turn->opened = true;
    } else if (!turn->parser->error) {
        turn->gives = token->start;
    }
}

static void S_row_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_node *node = S_row_open(markdown_core_compile_self(compiler), turn->parser, *turn->container,
                                          turn->context->reader, compiler, index, turn->base);
    if (node) {
        *turn->container = node;
        turn->opened = true;
    }
}

static const markdown_core_node_type containment_kinds[] = {MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                            MARKDOWN_CORE_NODE_TABLE_CELL, MARKDOWN_CORE_NODE_NONE};

static int contains_inlines(const markdown_core_element *element, markdown_core_node *node) {
    /* An empty cell has no inlines to read. */
    return node->kind == MARKDOWN_CORE_NODE_TABLE_CELL && markdown_core_node_text(node).len > 0;
}

static void opaque_alloc(markdown_core_node *node) {
    /* A payload that could not be allocated fails the parse: no incomplete
     * table is returned by a successful parse. */
    if (node->kind == MARKDOWN_CORE_NODE_TABLE) {
        node->opaque = markdown_core_alloc(1, sizeof(markdown_core_table));
    }
}

static void opaque_free(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    free_node_table(node->opaque);
}

/* A reopen at the old row `next` counts what the rows before it invented
 * again, as the row holds it; one past its last row, which its close
 * counted, keeps the count it closed with. */
static void reopen_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, const markdown_core_node *next, bool blank) {
    (void)self;
    (void)parser;
    (void)blank;
    if (node->kind != MARKDOWN_CORE_NODE_TABLE) {
        return;
    }
    if (!next) {
        return;
    }
    assert(next->kind == MARKDOWN_CORE_NODE_TABLE_ROW);
    ((markdown_core_table *)node->opaque)->autocompleted_cells = (size_t)next->internal_offset;
}

/* A row read again stands as the old one did when the rows before it
 * invented as many cells: the cap refuses the same rows after it, and the
 * counts the old rows hold stay exact. */
static bool resumes(const markdown_core_element_instance *self, const markdown_core_parser *parser,
                    const markdown_core_node *node, const markdown_core_node *next) {
    (void)self;
    (void)parser;
    const size_t now = ((const markdown_core_table *)node->opaque)->autocompleted_cells;
    return (now < INT_MAX ? (int)now : INT_MAX) == next->internal_offset;
}

static const markdown_core_node_type TABLE_KINDS[] = {MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_NONE};

/* A DELIMITER ROW FOLLOWS A PARAGRAPH (element.h, markdown_core_follow):
 * the paragraph's last line heads a table (S_open_table), the paragraph
 * itself when that line was all of it. An edit reads the table's lines as
 * its own, so it names no former kind. */
static const markdown_core_node_type TABLE_LEADERS[] = {MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_follow TABLE_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_LINE, .kinds = TABLE_LEADERS, .makes = MARKDOWN_CORE_NODE_TABLE},
    {0},
};

static const markdown_core_edit_ops TABLE_EDIT = {
    /* An edit reads again from a row, the rows before it kept: each is one
     * line, and none takes the next. */
    .reopens = true, .reopen_block = reopen_block, .resumes = resumes, .abuts = TABLE_KINDS, .headed = true,
};

static const markdown_core_record_ops TABLE_PAYLOAD = {
    .init = opaque_alloc,
    .free = opaque_free,
};

static const markdown_core_contain_ops TABLE_CONTAIN = {
    .kinds = containment_kinds,
};

static const markdown_core_content_ops TABLE_CONTENT = {
    .holds_inlines = contains_inlines,
};

/* A delimiter row begins with a pipe, a colon or a dash. */
static const markdown_core_block_construct TABLE_STARTS[] = {
    {.bytes = "|:-", .indent = 3, .precedence = 1, .construct = {.start = S_head_start}},
    {.indent = 3, .precedence = 1, .within = MARKDOWN_CORE_NODE_TABLE, .construct = {.start = S_row_start}},
};

static const markdown_core_compile_ops TABLE_BLOCK_TOKENS[TABLE_BLOCKS] = {
    [TABLE_HEAD] = {.exit = S_head},
    [TABLE_ROW] = {.exit = S_row_made},
};

static const markdown_core_open_ops TABLE_OPENS = {
    .starts = TABLE_STARTS,
    .start_count = 2,
    .tokens = TABLE_BLOCK_TOKENS,
    .token_count = TABLE_BLOCKS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`): a
 * table goes on at any line a pipe row reads; a blank line or a block
 * start that interrupts it ends it. A block of its kind holds no block a
 * lookahead adds. */
static markdown_core_state S_goes(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_block_continued(tokenizer, MARKDOWN_CORE_TOKEN_PREFIX);
}

static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (turn->look || turn->node->kind != MARKDOWN_CORE_NODE_TABLE) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_check(tokenizer, &TABLE_ROW_READ, markdown_core_state_of(S_goes),
                               markdown_core_state_of(S_head_no), code);
}

static const markdown_core_line_ops TABLE_LINES = {
    .continuation = {.start = S_continue},
    .beyond = true,
    .uncut = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_TABLE = {
    .edit = &TABLE_EDIT,
    .follows = TABLE_FOLLOWS,
    .name = "table",
    .payload = &TABLE_PAYLOAD,
    .contain = &TABLE_CONTAIN,
    .content = &TABLE_CONTENT,
    .opens = &TABLE_OPENS,
    .lines = &TABLE_LINES,
};
