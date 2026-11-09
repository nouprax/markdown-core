#include "alloc.h"
#include "table_scanners.h"
#include <markdown-core-element-api.h>
#include "element.h"
#include <inlines.h>
#include <parser.h>
#include "roster.h"
#include <assert.h>
#include <string.h>
#include <limits.h>
#include "utf8.h"
#include "parts.h"

#include "strikethrough.h"
#include "table.h"
#include "directive.h"
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

/* A recognized final row borrows its source; no cell array is retained. */
typedef struct {
    unsigned char *source;
    int length, paragraph_offset;
    uint16_t n_columns;
} pipe_row;

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
                                                                                       int first_line, int first_column,
                                                                                       int last_line, int last_column) {
    markdown_core_node *node = markdown_core_parser_make_node(parser, kind);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    if (kind == MARKDOWN_CORE_NODE_TABLE_ROW || kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        markdown_core_node_set_element(node, &MARKDOWN_CORE_ELEMENT_TABLE);
    }
    node->place.start = (uint32_t)markdown_core_parser_source_offset(parser, first_line, first_column);
    node->place.end = (uint32_t)markdown_core_parser_source_end(parser, last_line, last_column);
    return node;
}

/* A pipe cell is made whole, from its row's line, by the table that owns
 * it: like every row and cell but a pipe body row, it is never an open block.
 * Its end is its caller's to set. THE ROWS AND CELLS A TABLE MAKES settle as
 * every block of the tree does (block_internal.h, markdown_core_block_settle),
 * each once it is whole: a cell into its row once its line is read, a row
 * made whole into its table after its cells, and a pipe body row, an open
 * block, as the table takes its next row or closes. */
static markdown_core_node *new_cell(markdown_core_parser *parser, int column) {
    return table_child(parser, MARKDOWN_CORE_NODE_TABLE_CELL, parser->at.line_number, column, parser->at.line_number,
                       column);
}

/* A PIPE CELL HOLDS ITS BYTES, whichever path found its row: trimmed of the
 * whitespace around them, its escaped pipes contracted, a tab kept a tab.
 * Assemble every cell once, preserving a source run at each contraction.
 * A logical pipe represents both authored bytes of its escape. There is no
 * alternate inline parser and no position repair after parsing. `offset`
 * indexes the content of `source`, whose map places it, when the row was
 * recovered from a paragraph, and otherwise the input of physical `line`. */
static void set_cell_content(markdown_core_parser *parser, markdown_core_node *node, const node_cell *cell,
                             markdown_core_node *source, int line, bufsize_t line_start, bufsize_t offset) {
    node->internal_offset = cell->internal_offset;
    for (bufsize_t from = 0; from < cell->content.len && !parser->error;) {
        bufsize_t to = from;
        bool escaped =
            cell->content.data[from] == '\\' && from + 1 < cell->content.len && cell->content.data[from + 1] == '|';
        if (escaped) {
            int place = line;
            bufsize_t first, end;
            if (source) {
                markdown_core_parser_content_place(parser, &source->content_map, offset + from, &place, &first);
                markdown_core_parser_content_end_place(parser, &source->content_map, offset + from + 1, &place, &end);
            } else {
                first = markdown_core_parser_source_offset(parser, line, offset + from + 1);
                end = markdown_core_parser_source_end(parser, line, offset + from + 2);
            }
            markdown_core_parser_append_content_mark(parser, node, node->content.size, place, first, (int)(end - first),
                                                     (int)(end - first));
            markdown_core_strbuf_putc(&node->content, '|');
            to = from + 2;
        } else {
            do {
                to++;
            } while (to < cell->content.len && !(cell->content.data[to] == '\\' && to + 1 < cell->content.len &&
                                                 cell->content.data[to + 1] == '|'));
            if (source) {
                markdown_core_parser_append_content_marks(parser, &source->content_map, &node->content_map,
                                                          offset + from, to - from, node->content.size);
            } else {
                markdown_core_parser_append_line_marks(parser, node, line, line_start, offset + from + 1, to - from,
                                                       node->content.size);
            }
            markdown_core_strbuf_put(&node->content, cell->content.data + from, to - from);
        }
        if (node->content.oom) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        from = to;
    }
}

/* A pipe-row cursor borrows source bytes. Recognition and materialization
 * use the same cell iterator; failed candidates never allocate row geometry. */
typedef struct {
    unsigned char *source;
    int length, offset, row_start;
    uint32_t columns;
    bool finished, valid;
} pipe_row_cursor;

static pipe_row_cursor pipe_row_begin(unsigned char *source, int length, int start) {
    return (pipe_row_cursor){.source = source,
                             .length = length,
                             .offset = start + scan_table_cell_end(source, length, start),
                             .row_start = start};
}

static bool pipe_row_next(pipe_row_cursor *cursor, node_cell *cell) {
    while (!cursor->finished && cursor->offset < cursor->length) {
        int offset = cursor->offset;
        int content = scan_table_cell(cursor->source, cursor->length, offset);
        int separator = scan_table_cell_end(cursor->source, cursor->length, offset + content);
        bool present = content || separator;
        if (present) {
            if (++cursor->columns > UINT16_MAX) {
                cursor->finished = true;
                return false;
            }
            *cell = (node_cell){.content = {cursor->source + offset, content, 0},
                                .start_offset = offset,
                                .end_offset = offset + content - 1};
            markdown_core_chunk_trim(&cell->content);
            while (cell->start_offset > cursor->row_start && cursor->source[cell->start_offset - 1] != '|') {
                --cell->start_offset;
                ++cell->internal_offset;
            }
            if (!content && cell->start_offset == offset) {
                cell->end_offset = offset;
            }
        }
        cursor->offset += content + separator;
        if (!separator) {
            int ending = scan_table_row_end(cursor->source, cursor->length, cursor->offset);
            cursor->offset += ending;
            if (ending && cursor->offset != cursor->length) {
                cursor->row_start = cursor->offset;
                cursor->columns = 0;
                cursor->offset += scan_table_cell_end(cursor->source, cursor->length, cursor->offset);
            } else {
                cursor->finished = true;
            }
        }
        if (present) {
            return true;
        }
    }
    cursor->finished = true;
    cursor->valid = cursor->offset == cursor->length && cursor->columns != 0;
    return false;
}

static bool recognize_pipe_row(unsigned char *source, int length, pipe_row *row) {
    pipe_row_cursor cursor = pipe_row_begin(source, length, 0);
    node_cell cell;
    while (pipe_row_next(&cursor, &cell)) {
    }
    if (!cursor.valid) {
        return false;
    }
    *row = (pipe_row){.source = source,
                      .length = length,
                      .paragraph_offset = cursor.row_start,
                      .n_columns = (uint16_t)cursor.columns};
    return true;
}

static pipe_row_cursor pipe_row_cells(const pipe_row *row) {
    return pipe_row_begin(row->source, row->length, row->paragraph_offset);
}

/* Give `node` the source span of [start_offset, end_offset] in `owner`'s
 * content buffer. Every table position recovered from that buffer goes through
 * here, so there is one place that knows a content offset is not a column. */
static void S_place_content_span(markdown_core_parser *parser, markdown_core_node *owner, markdown_core_node *node,
                                 bufsize_t start_offset, bufsize_t end_offset) {
    int line;
    bufsize_t source;

    if (markdown_core_parser_content_place(parser, &owner->content_map, start_offset, &line, &source)) {
        node->place.start = (uint32_t)source;
    }
    if (markdown_core_parser_content_end_place(parser, &owner->content_map, end_offset, &line, &source)) {
        node->place.end = (uint32_t)source;
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
    int line;
    bufsize_t source;

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
        markdown_core_parser_content_end_place(parser, &paragraph->content_map, scope_end - 1, &line, &source)) {
        paragraph->place.end = (uint32_t)source;
    }
    /* What is cut from the front moves the paragraph's start, which its
     * close reads from the map. */
    if (lead > first) {
        markdown_core_parser_adopt_content_marks(parser, &paragraph->content_map, &paragraph->content_map, first,
                                                 lead - first);
    } else {
        paragraph->content_map.count = 0;
        paragraph->content_map.offset = 0;
    }
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
    int line;
    bufsize_t source;
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
    markdown_core_parser_adopt_content_marks(parser, &paragraph->content_map, &table->content_map, header,
                                             text.len - header);
    if (markdown_core_parser_content_place(parser, &paragraph->content_map, header, &line, &source)) {
        table->place = (markdown_core_place){(uint32_t)source, (uint32_t)source};
    }
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

/* Return NULL when the syntax does not match or the parent rejects the table
 * kind, so later elements can try the same line. A pipe table opens at its
 * delimiter row, and its header row is the last line of the paragraph above
 * it (S_open_table). Once the table has opened, return it even if a later
 * allocation fails; parser->error then aborts the parse and destruction
 * releases the partially built table. */
static markdown_core_node *try_opening_table_header(const markdown_core_element_instance *self,
                                                    markdown_core_parser *parser, markdown_core_node *paragraph,
                                                    unsigned char *input, int len) {
    markdown_core_node *table_header;
    pipe_row header_row, delimiter_row;
    markdown_core_chunk text;
    uint16_t i;

    if (paragraph->flags & MARKDOWN_CORE_NODE__TABLE_VISITED) {
        return NULL;
    }

    if (!scan_table_start(input, len, markdown_core_parser_get_first_nonspace(parser))) {
        return NULL;
    }
    /* A delimiter row that opens with `:` is one only while the input's
     * end leaves it be: written on, it may be a directive's name. */
    const int first = markdown_core_parser_get_first_nonspace(parser);
    if (parser->sink && input[first] == ':' && first + 1 < len && !markdown_core_is_whitespace(input[first + 1])) {
        int end = first;
        while (end < len && !markdown_core_is_whitespace(input[end])) {
            end++;
        }
        if (end == len || markdown_core_is_line_end(input[end])) {
            markdown_core_directive_report_line(parser, first);
        }
    }

    if (!recognize_pipe_row(input + markdown_core_parser_get_first_nonspace(parser),
                            len - markdown_core_parser_get_first_nonspace(parser), &delimiter_row)) {
        return NULL;
    }

    // Select the final header row and verify width before committing a table.
    /* The header row is the last line of the paragraph's text. */
    text = markdown_core_node_text(paragraph);
    if (!recognize_pipe_row(text.data, text.len, &header_row) || header_row.n_columns != delimiter_row.n_columns) {
        paragraph->flags |= MARKDOWN_CORE_NODE__TABLE_VISITED;
        return NULL;
    }

    markdown_core_node *node = S_open_table(parser, paragraph, markdown_core_parser_parent(parser, paragraph), text,
                                            header_row.paragraph_offset);
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
    header_row.source = text.data;
    header_row.length = text.len;
    header_row.paragraph_offset = 0;

    /* Table data belongs to the element. Its cleanup accepts partial
     * initialization when an allocation fails. */
    markdown_core_node_set_element(node, self->element);
    node->opaque = markdown_core_alloc(1, sizeof(markdown_core_table));
    if (!node->opaque) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);

        return node;
    }
    markdown_core_table *table = node->opaque;
    table->column_count = header_row.n_columns;
    table->columns = markdown_core_alloc(table->column_count, sizeof(*table->columns));
    if (!table->columns) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);

        return node;
    }
    pipe_row_cursor markers = pipe_row_cells(&delimiter_row);
    node_cell geometry;
    for (i = 0; pipe_row_next(&markers, &geometry); ++i) {
        const markdown_core_chunk *cell = &geometry.content;
        bool left = cell->data[0] == ':', right = cell->data[cell->len - 1] == ':';
        table->columns[i].flow = left ? (right ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_LEFT)
                                      : (right ? MARKDOWN_CORE_FLOW_RIGHT : MARKDOWN_CORE_FLOW_NONE);
    }

    /* The header row is made whole from the table's content, as its cells
     * are: it is never an open block. */
    table_header =
        table_child(parser, MARKDOWN_CORE_NODE_TABLE_ROW, parser->at.line_number, 1, parser->at.line_number, 1);
    if (!table_header) {

        return node;
    }
    S_place_content_span(parser, node, table_header, 0, header_row.length - 2);

    table->head_count = 1;

    pipe_row_cursor header = pipe_row_cells(&header_row);
    while (pipe_row_next(&header, &geometry)) {
        const node_cell *cell = &geometry;
        markdown_core_node *header_cell = new_cell(parser, 1);
        if (!header_cell) {
            break;
        }
        S_place_content_span(parser, node, header_cell, cell->start_offset, cell->end_offset);
        set_cell_content(parser, header_cell, cell, node, parser->at.line_number, parser->at.line_start,
                         (bufsize_t)(cell->content.data - header_row.source));
        markdown_core_block_settle(parser, header_cell, table_header);
    }
    markdown_core_block_settle(parser, table_header, node);

    markdown_core_parser_advance_offset(
        parser, (char *)input, (int)strlen((char *)input) - 1 - markdown_core_parser_get_offset(parser), false);

    return node;
}

static markdown_core_node *try_opening_table_row(const markdown_core_element *self, markdown_core_parser *parser,
                                                 markdown_core_node *parent_container, unsigned char *input, int len) {
    markdown_core_node *table_row_block;
    pipe_row row;

    if (markdown_core_parser_is_blank(parser)) {
        return NULL;
    }

    markdown_core_table *table = parent_container->opaque;
    if (!table || table->autocompleted_cells > MAX_AUTOCOMPLETED_CELLS) {
        return NULL;
    }

    if (!recognize_pipe_row(input + markdown_core_parser_get_first_nonspace(parser),
                            len - markdown_core_parser_get_first_nonspace(parser), &row)) {
        return NULL;
    }
    /* A pipe row begins at its own first non-space byte, as its header does:
     * `parser->at.offset` is the container's content, which holds the row's
     * indentation or the tail of a tab its container prefix shares. */
    /* The row is whole once its line is read: it settles into the table,
     * which stays the current block, and a reopen at the next row finds
     * none open inside it. */
    bufsize_t completed_at = len;
    while (completed_at > 0 && (input[completed_at - 1] == '\n' || input[completed_at - 1] == '\r')) {
        completed_at--;
    }
    table_row_block = table_child(parser, MARKDOWN_CORE_NODE_TABLE_ROW, parser->at.line_number,
                                  parser->at.first_nonspace + 1, parser->at.line_number, (int)completed_at);
    if (!table_row_block) {
        return NULL;
    }
    (void)self;
    /* The count before the row, which the cap is no further past than one
     * row's width. */
    table_row_block->internal_offset = table->autocompleted_cells < INT_MAX ? (int)table->autocompleted_cells : INT_MAX;

    {
        int i, table_columns = (int)table->column_count;

        pipe_row_cursor cells = pipe_row_cells(&row);
        node_cell geometry;
        for (i = 0; i < table_columns && pipe_row_next(&cells, &geometry); ++i) {
            const node_cell *cell = &geometry;
            markdown_core_node *node = new_cell(parser, parser->at.first_nonspace + 1 + cell->start_offset);
            if (!node) {
                break;
            }
            node->place.end = (uint32_t)markdown_core_parser_source_end(
                parser, parser->at.line_number, parser->at.first_nonspace + 1 + cell->end_offset);
            set_cell_content(parser, node, cell, NULL, parser->at.line_number, parser->at.line_start,
                             (bufsize_t)(cell->content.data - input));
            markdown_core_block_settle(parser, node, table_row_block);
        }

        table->autocompleted_cells += (size_t)(table_columns - i);

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
            markdown_core_node *node = new_cell(parser, (int)completed_at);
            if (!node) {
                break;
            }
            node->place.end =
                (uint32_t)markdown_core_parser_source_end(parser, parser->at.line_number, (int)completed_at);
            markdown_core_block_settle(parser, node, table_row_block);
        }
    }

    markdown_core_block_settle(parser, table_row_block, parent_container);
    markdown_core_parser_advance_offset(parser, (char *)input, len - 1 - markdown_core_parser_get_offset(parser),
                                        false);

    return parent_container;
}

/* A pipe table opens at its delimiter row, whose header is the last line of
 * the paragraph above it, and takes a row on each later line it matches. */
static markdown_core_node *try_opening_table_block(const markdown_core_element_instance *self, int indented,
                                                   markdown_core_parser *parser, markdown_core_node *parent_container,
                                                   unsigned char *input, int len) {
    markdown_core_node_type parent_type = (markdown_core_node_type)parent_container->kind;

    if (!indented && parent_type == MARKDOWN_CORE_NODE_PARAGRAPH) {
        return try_opening_table_header(self, parser, parent_container, input, len);
    } else if (!indented && parent_type == MARKDOWN_CORE_NODE_TABLE) {
        return try_opening_table_row(self->element, parser, parent_container, input, len);
    }

    return NULL;
}

/* A table goes on at any line a pipe row reads: a blank line or a block
 * start that interrupts it ends it. */
static int matches(const markdown_core_element_instance *self, markdown_core_parser *parser, unsigned char *input,
                   int len, markdown_core_node *parent_container) {
    (void)self;
    if (parent_container->kind != MARKDOWN_CORE_NODE_TABLE) {
        return 0;
    }
    pipe_row row;
    return recognize_pipe_row(input + markdown_core_parser_get_first_nonspace(parser),
                              len - markdown_core_parser_get_first_nonspace(parser), &row);
}

static const markdown_core_node_type containment_kinds[] = {MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                            MARKDOWN_CORE_NODE_TABLE_CELL, MARKDOWN_CORE_NODE_NONE};

static int contains_inlines(const markdown_core_element *element, markdown_core_node *node) {
    /* An empty cell has no inlines to read. */
    return node->kind == MARKDOWN_CORE_NODE_TABLE_CELL && markdown_core_node_text(node).len > 0;
}

static void opaque_alloc(const markdown_core_element *self, markdown_core_node *node) {
    /* A payload that could not be allocated fails the parse: no incomplete
     * table is returned by a successful parse. */
    if (node->kind == MARKDOWN_CORE_NODE_TABLE) {
        node->opaque = markdown_core_alloc(1, sizeof(markdown_core_table));
    }
}

static void opaque_free(const markdown_core_element *self, markdown_core_node *node) { free_node_table(node->opaque); }

static bool opaque_view(const markdown_core_element *self, markdown_core_node *copy, const markdown_core_node *node) {
    markdown_core_table *made = markdown_core_realloc(NULL, sizeof(*made));
    if (!made) {
        copy->opaque = NULL;
        return false;
    }
    *made = *(const markdown_core_table *)node->opaque;
    made->views_columns = made->columns != NULL;
    copy->opaque = made;
    return true;
}

static void opaque_adopt(const markdown_core_element *self, markdown_core_node *view, markdown_core_node *node) {
    markdown_core_table *made = view->opaque, *table = node->opaque;
    if (made->views_columns && made->columns == table->columns && !table->views_columns) {
        made->views_columns = false;
        table->views_columns = true;
    }
}

/* A reopen at the old row `next` counts what the rows before it invented
 * again, as the row holds it. */
static void reopen_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, const markdown_core_node *next, bool blank) {
    (void)self;
    (void)parser;
    (void)blank;
    if (node->kind != MARKDOWN_CORE_NODE_TABLE) {
        return;
    }
    assert(next && next->kind == MARKDOWN_CORE_NODE_TABLE_ROW);
    ((markdown_core_table *)node->opaque)->autocompleted_cells = next ? (size_t)next->internal_offset : 0;
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

static const markdown_core_edit_ops TABLE_EDIT = {
    /* A block identifier on the line after it writes it
     * (block_identifier.c). */
    .trailed_kinds = TABLE_KINDS,
    /* An edit reads again from a row, the rows before it kept: each is one
     * line, and none takes the next. */
    .reopens = true,
    .reopen_block = reopen_block,
    .resumes = resumes,
    .abuts = TABLE_KINDS,
    .headed = true,
};

static const markdown_core_opaque_ops TABLE_OPAQUE = {
    .alloc = opaque_alloc,
    .free = opaque_free,
    .view = opaque_view,
    .adopt = opaque_adopt,
};

static const markdown_core_contain_ops TABLE_CONTAIN = {
    .kinds = containment_kinds,
};

static const markdown_core_content_ops TABLE_CONTENT = {
    .holds_inlines = contains_inlines,
};

static const markdown_core_open_ops TABLE_OPENS = {
    /* A delimiter row makes the paragraph line before it the head. */
    .claims = " \t|:-",
    .indent = 3,
    .open = try_opening_table_block,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_TABLE = {
    .edit = &TABLE_EDIT,
    .name = "table",
    .last_block_matches = matches,
    .opaque = &TABLE_OPAQUE,
    .contain = &TABLE_CONTAIN,
    .content = &TABLE_CONTENT,
    .opens = &TABLE_OPENS,
};

/* A pipe row's part: the cells written past its table's width, which the
 * row drops, as one field of no slot kind. */
void markdown_core_table_row_parts(markdown_core_parts *parts) {
    const unsigned char *text = parts->text;
    bufsize_t start = markdown_core_parts_last(parts, 0), end = parts->length;
    while (start < end && markdown_core_is_space_or_tab(text[start])) {
        start++;
    }
    if (start == 0 || start >= end || text[start] != '|') {
        return;
    }
    start++;
    for (bufsize_t at = start; at < end; at++) {
        if (markdown_core_is_line_end(text[at])) {
            return;
        }
    }
    while (end > start && markdown_core_is_whitespace(text[end - 1])) {
        end--;
    }
    if (end > start && text[end - 1] == '|' && !(end - 1 > start && text[end - 2] == '\\')) {
        end--;
    }
    while (start < end && markdown_core_is_space_or_tab(text[start])) {
        start++;
    }
    while (end > start && markdown_core_is_space_or_tab(text[end - 1])) {
        end--;
    }
    markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, start, end);
}
