#include "alloc.h"
#include "table_scanners.h"
#include <markdown-core-element-api.h>
#include "element.h"
#include <inlines.h>
#include <parser.h>
#include "registry.h"
#include <string.h>
#include <limits.h>
#include "utf8.h"

#include "strikethrough.h"
#include "table.h"
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
        markdown_core_free(table->runs);
    }
    markdown_core_free(table);
}

static void init_cell(markdown_core_node *node) {
    node->as.table_cell->rowspan = 1;
    node->as.table_cell->colspan = 1;
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) markdown_core_node *table_child(markdown_core_parser *parser,
                                                                                       markdown_core_node_type kind,
                                                                                       int first_line, int first_column,
                                                                                       int last_line, int last_column);

/* A pipe cell is made whole, from its row's line, by the table that owns
 * it: like every row and cell but a pipe body row, it is never an open block.
 * Its end is its caller's to set. THE ROWS AND CELLS A TABLE MAKES settle as
 * every block of the tree does (block_internal.h, markdown_core_block_settle),
 * each once it is whole: a cell into its row once its line is read, a row
 * made whole into its table after its cells, and a pipe body row, an open
 * block, as the table takes its next row or closes. */
static markdown_core_node *new_cell(markdown_core_parser *parser, int column) {
    markdown_core_node *cell =
        table_child(parser, MARKDOWN_CORE_NODE_TABLE_CELL, parser->line_number, column, parser->line_number, column);
    if (cell) {
        init_cell(cell);
    }
    return cell;
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

    assert(parser->current == paragraph);
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

    /* Decide before allocating, so refusal leaves the complete paragraph
     * available to grammar. */
    markdown_core_node *parent = markdown_core_parser_parent(parser, paragraph);
    if (!markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_TABLE)) {
        return NULL;
    }
    markdown_core_node *node = S_open_table(parser, paragraph, parent, text, header_row.paragraph_offset);
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
    table_header = table_child(parser, MARKDOWN_CORE_NODE_TABLE_ROW, parser->line_number, 1, parser->line_number, 1);
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
        set_cell_content(parser, header_cell, cell, node, parser->line_number, parser->line_start,
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
     * `parser->offset` is the container's content, which holds the row's
     * indentation or the tail of a tab its container prefix shares. */
    table_row_block = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                     parser->first_nonspace + 1);
    if (!table_row_block) {

        return NULL;
    }
    markdown_core_node_set_element(table_row_block, self);

    {
        int i, table_columns = (int)table->column_count;

        pipe_row_cursor cells = pipe_row_cells(&row);
        node_cell geometry;
        for (i = 0; i < table_columns && pipe_row_next(&cells, &geometry); ++i) {
            const node_cell *cell = &geometry;
            markdown_core_node *node = new_cell(parser, parser->first_nonspace + 1 + cell->start_offset);
            if (!node) {
                break;
            }
            node->place.end = (uint32_t)markdown_core_parser_source_end(parser, parser->line_number,
                                                                        parser->first_nonspace + 1 + cell->end_offset);
            set_cell_content(parser, node, cell, NULL, parser->line_number, parser->line_start,
                             (bufsize_t)(cell->content.data - input));
            markdown_core_block_settle(parser, node, table_row_block);
        }

        table->content_count++;
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
        bufsize_t completed_at = len;
        while (completed_at > 0 && (input[completed_at - 1] == '\n' || input[completed_at - 1] == '\r')) {
            completed_at--;
        }
        for (; i < table_columns; ++i) {
            markdown_core_node *node = new_cell(parser, (int)completed_at);
            if (!node) {
                break;
            }
            node->place.end = (uint32_t)markdown_core_parser_source_end(parser, parser->line_number, (int)completed_at);
            markdown_core_block_settle(parser, node, table_row_block);
        }
    }

    markdown_core_parser_advance_offset(parser, (char *)input, len - 1 - markdown_core_parser_get_offset(parser),
                                        false);

    return table_row_block;
}

typedef struct markdown_core_table_workspace table_workspace;
/* Recognize a complete source candidate before claiming any of its lines. */
static markdown_core_node *table_try_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                          markdown_core_node *parent, unsigned char *input, int length);

static markdown_core_node *try_opening_table_block(const markdown_core_element_instance *self, int indented,
                                                   markdown_core_parser *parser, markdown_core_node *parent_container,
                                                   unsigned char *input, int len) {
    markdown_core_node_type parent_type = (markdown_core_node_type)parent_container->kind;

    if (!indented && parent_type == MARKDOWN_CORE_NODE_PARAGRAPH) {
        return try_opening_table_header(self, parser, parent_container, input, len);
    } else if (!indented && parent_type == MARKDOWN_CORE_NODE_TABLE) {
        return try_opening_table_row(self->element, parser, parent_container, input, len);
    } else if (!indented) {
        return table_try_open(self, parser, parent_container, input, len);
    }

    return NULL;
}

static int table_caption_start(const unsigned char *data, int length, int first, int indent) {
    if (indent > 3) {
        return -1;
    }
    int marker = 0;
    if (length - first >= 6 && (!memcmp(data + first, "Table:", 6) || !memcmp(data + first, "table:", 6))) {
        marker = 6;
    } else if (first < length && data[first] == ':') {
        if (first + 1 < length) {
            int32_t scalar;
            markdown_core_utf8proc_decode(data + first + 1, length - first - 1, &scalar);
            if (markdown_core_utf8proc_classes(scalar) & MARKDOWN_CORE_UNICODE_PUNCTUATION) {
                return -1;
            }
        }
        marker = 1;
    }
    if (!marker) {
        return -1;
    }
    int content = first + marker;
    while (content < length && markdown_core_is_whitespace(data[content])) {
        content++;
    }
    return content;
}

/* The line after a simple table's last one: a blank line ends the table, a
 * footer closes it, and any other line is its next row. */
static int table_simple_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                const markdown_core_table *table, unsigned char *input, int len);

static int matches(const markdown_core_element_instance *self, markdown_core_parser *parser, unsigned char *input,
                   int len, markdown_core_node *parent_container) {
    int res = 0;

    if (parent_container->kind == MARKDOWN_CORE_NODE_TABLE) {
        const markdown_core_table *table = parent_container->opaque;
        if (table->runs) {
            return table_simple_matches(self, parser, table, input, len);
        }
        if (table_caption_start(input, len, parser->first_nonspace, parser->indent) >= 0) {
            return 0;
        }
        pipe_row row;
        res = recognize_pipe_row(input + markdown_core_parser_get_first_nonspace(parser),
                                 len - markdown_core_parser_get_first_nonspace(parser), &row);
    }

    return res;
}

static const markdown_core_node_type containment_kinds[] = {MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                            MARKDOWN_CORE_NODE_TABLE_CELL, MARKDOWN_CORE_NODE_NONE};

static int contains_inlines(const markdown_core_element *element, markdown_core_node *node) {
    /* A cell whose content was read as blocks has no text of its own: its
     * blocks hold it. */
    return node->kind == MARKDOWN_CORE_NODE_TABLE_CAPTION ||
           (node->kind == MARKDOWN_CORE_NODE_TABLE_CELL && markdown_core_node_text(node).len > 0);
}

static void opaque_alloc(const markdown_core_element *self, markdown_core_node *node) {
    /* A payload that could not be allocated fails the parse: no incomplete
     * table is returned by a successful parse. */
    if (node->kind == MARKDOWN_CORE_NODE_TABLE) {
        node->opaque = markdown_core_alloc(1, sizeof(markdown_core_table));
    } else if (node->kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        init_cell(node);
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

static int visit_owned_subtrees(const markdown_core_element *self, markdown_core_node *node,
                                markdown_core_owned_subtree_visitor visitor, void *context) {
    markdown_core_table *table = node->opaque;
    return !table || !table->caption || visitor(&table->caption, context);
}

/* Source candidates own geometry only. Public nodes are allocated after a
 * complete candidate has passed validation; failed candidates consume nothing. */
typedef markdown_core_table_interval table_interval;

struct markdown_core_table_workspace;

typedef struct markdown_core_table_source_line {
    const unsigned char *data, *after;
    struct markdown_core_table_workspace *workspace;
    int length, input_length, offset, first, first_column, indent, line, blanks, horizontal_end;
    /* Horizontal grammar returns only zero, '-' or '='. Group the byte facts
     * with the final offset so the cache fits the former LP64 padding. */
    bool dashes_scanned, full_boundary, horizontal_scanned;
    unsigned char horizontal_kind;
    size_t dash_count;
    size_t dash_offset, byte_offset;
    bool dashes_ready, columns_ready;
    int columns;
} table_source_line;

/* THE OPENER'S CANDIDATE (blocks.c, THE CANDIDATES). In a commit more text
 * may follow, the scan of a grid whose top border is the opener's own line,
 * `holds`, reads no end (markdown_core_block_lookahead, `limit`): it waits
 * where the input ends, `exhausted`, with `blanks` blank lines before it,
 * which the line read next carries, and at a line the text has not finished
 * whose bytes do not tell, and decides nothing: the call is `undecided`.
 * Every other reader reads on as an ordinary reading, which reads the end
 * at `incomplete`, the first line the text has not finished, plus one. A
 * commit's call that can wait sets aside the end it reads
 * (markdown_core_parser_set_end_aside), `aside`, and records no facts once
 * it read it; a candidate's decision, `decides`, writes nothing. */
typedef struct {
    markdown_core_parser *parser;
    struct markdown_core_table_workspace *workspace;
    markdown_core_block_lookahead lookahead;
    table_source_line *lines;
    size_t count, incomplete;
    int blanks;
    bool ended, holds, exhausted, undecided, aside, decides;
} table_source;

typedef struct {
    size_t first, last;
    int left, right;
    size_t row;
    int64_t rowspan, colspan;
    int start_column, end_column;
} table_source_cell;
typedef struct {
    size_t first, last, cell, count;
} table_source_row;
typedef struct {
    markdown_core_table_column *columns;
    size_t column_count, column_capacity;
    table_source_row *rows;
    size_t row_count, row_capacity, head_count, foot_count;
    table_source_cell *cells;
    size_t cell_count, cell_capacity;
    size_t first, last;
    /* `pipe`: a pipe table's header. Its cells hold their bytes, as every
     * pipe cell does (`set_cell_content`), and the table stays open for the
     * body rows `try_opening_table_row` adds. `simple`: a simple table its
     * opening lines leave open. It reads each later row on its own
     * (`table_take_line`), cut at its dash runs, which start at `runs` in
     * the workspace's dashes. */
    bool block_content, pipe, simple;
    size_t runs;
    int padding_limit;
    /* The table's left edge, in geometry columns (`table_margin`): the
     * table, each row and the first column begin here, not at the
     * container's content. */
    int margin;
} table_candidate;

typedef struct {
    size_t runs, count, line;
} table_separator_key;
typedef struct {
    size_t first, count, digit;
} table_separator_group;
typedef struct {
    size_t top, bottom, left, right, area;
} table_grid_region;

/* A query borrows this workspace. Only its used ranges are reset; committed
 * AST values own copies. All references into growing line geometry are
 * offsets, so growth cannot invalidate a previously captured line or key. */
struct markdown_core_table_workspace {
    table_source_line *lines;
    size_t lines_capacity;
    int *bytes;
    size_t bytes_count, bytes_capacity;
    table_interval *dashes;
    size_t dashes_count, dashes_capacity;
    table_candidate candidate;
    table_separator_key *separator_keys;
    size_t separator_keys_capacity;
    table_separator_key *separator_scratch;
    size_t separator_scratch_capacity;
    table_separator_group *separator_groups;
    size_t separator_groups_capacity;
    int *grid_parents;
    size_t grid_parents_capacity;
    int *grid_sizes;
    size_t grid_sizes_capacity;
    int *grid_positions;
    size_t grid_positions_capacity;
    size_t *boundaries;
    size_t boundaries_capacity;
    size_t *row_indices;
    size_t row_indices_capacity;
    int *region_parents;
    size_t region_parents_capacity;
    int *region_sizes;
    size_t region_sizes_capacity;
    int *region_next;
    size_t region_next_capacity;
    int *region_previous;
    size_t region_previous_capacity;
    table_grid_region *regions;
    size_t regions_capacity;
    table_grid_region *region_scratch;
    size_t region_scratch_capacity;
    table_grid_region *closed;
    size_t closed_capacity;
    markdown_core_table_work work;
};

/* The workspace is the table element's parse record: zeroed with the
 * parser, grown by the transactions that borrow it, released by
 * `dispose_parser`. Another element reaches it through its table peer. */
const markdown_core_table_work *markdown_core_table_work_in(const markdown_core_element_instance *table) {
    return &((const table_workspace *)table->state)->work;
}

static void *table_reserve(table_source *source, void *values, size_t *capacity, size_t count, size_t size) {
    size_t previous = *capacity;
    void *grown = markdown_core_reserve(values, capacity, count, size);
    if (!grown) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    source->workspace->work.scratch_growth += *capacity != previous;
    return grown;
}

static bool table_candidate_columns(table_source *source, table_candidate *candidate, size_t count) {
    {
        void *grown =
            table_reserve(source, candidate->columns, &candidate->column_capacity, count, sizeof(*candidate->columns));
        if (!grown) {
            return false;
        }
        candidate->columns = grown;
    }
    memset(candidate->columns, 0, count * sizeof(*candidate->columns));
    return true;
}

static bool table_source_push(table_source *source, table_source_line line) {
    table_workspace *workspace = source->workspace;
    size_t capacity = workspace->lines_capacity;
    {
        void *grown = table_reserve(source, workspace->lines, &workspace->lines_capacity, source->count + 1,
                                    sizeof(*source->lines));
        if (!grown) {
            return false;
        }
        workspace->lines = grown;
    }
    source->lines = workspace->lines;
    workspace->work.workspace_growth += workspace->lines_capacity != capacity;
    line.input_length = line.length;
    while (line.length && (line.data[line.length - 1] == '\n' || line.data[line.length - 1] == '\r')) {
        line.length--;
    }
    /* The current streaming line, immutable document source and normalized
     * EOF line all outlive this non-nested query, including its commitment. */
    line.workspace = source->workspace;
    source->lines[source->count++] = line;
    return true;
}

/* The reading's next line, pushed: false when it has none, because a line
 * ended the construct, the scan that holds waits, or the parse failed. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool table_source_read(table_source *source) {
    markdown_core_block_lookahead *lookahead = &source->lookahead;
    while (!source->ended && !source->exhausted && !source->parser->error) {
        markdown_core_chunk line;
        int first, indent, blanks;
        if (!markdown_core_parser_lookahead_next(lookahead, &line, &first, &indent, &blanks)) {
            if (!lookahead->exhausted && !lookahead->waited) {
                source->ended = true;
                break;
            }
            source->blanks += blanks;
            if (source->holds) {
                source->exhausted = true;
                break;
            }
            lookahead->limit = NULL;
            lookahead->waits = lookahead->exhausted = lookahead->waited = false;
            continue;
        }
        if (!table_source_push(source, (table_source_line){.data = line.data,
                                                           .length = line.len,
                                                           .offset = source->parser->offset,
                                                           .first = first,
                                                           .first_column = source->parser->first_nonspace_column,
                                                           .indent = indent,
                                                           .blanks = blanks + source->blanks,
                                                           .line = source->lookahead.line - 1,
                                                           .after = source->lookahead.cursor})) {
            return false;
        }
        source->blanks = 0;
        if (lookahead->incomplete && !source->incomplete) {
            source->incomplete = source->count;
        }
        return true;
    }
    return false;
}

/* Whether the reading has line `index`, reading on to it. */
static inline bool table_source_get(table_source *source, size_t index) {
    while (source->count <= index) {
        if (!table_source_read(source)) {
            break;
        }
    }
    if (source->incomplete && index + 1 >= source->incomplete && !source->holds) {
        markdown_core_parser_see_end(source->parser);
    }
    return index < source->count && !source->parser->error;
}

/* The line at the parser's cursor, `data` and `length` its input. */
static table_source_line table_cursor_line(markdown_core_parser *parser, const unsigned char *data, int length) {
    return (table_source_line){.data = data,
                               .length = length,
                               .offset = parser->offset,
                               .first = parser->first_nonspace,
                               .first_column = parser->first_nonspace_column,
                               .indent = parser->indent,
                               .line = parser->line_number,
                               .after = parser->lookahead_cursor};
}

static void table_source_end(table_source *source) {
    markdown_core_parser_lookahead_end(&source->lookahead);
    table_workspace *workspace = source->workspace;
    workspace->bytes_count = workspace->dashes_count = 0;
}

static void table_candidate_reset(table_candidate *candidate) {
    candidate->column_count = candidate->row_count = candidate->cell_count = 0;
    candidate->head_count = candidate->foot_count = candidate->first = candidate->last = 0;
    candidate->block_content = candidate->pipe = candidate->simple = false;
    candidate->padding_limit = candidate->margin = 0;
}

/* WHERE GEOMETRY IS MEASURED FROM. A line's geometry column 0 is its
 * container's content -- the virtual column the block parser reached after
 * the container prefixes, `first_column - indent` -- so a table reads the same
 * inside a list item, a quote, or a grid cell as at the top level. Its tab
 * stops are still the PHYSICAL line's, as for block indentation: a tab
 * after `- x` or `> `, or one a prefix consumed in part, contributes only the
 * columns left to its stop. Counting stops from the content instead gave the
 * same tab four columns on one line and two on the next. */
static int table_line_origin(const table_source_line *line) { return line->first_column - line->indent; }

static int table_tab_width(int origin, int column) { return 4 - (origin + column) % 4; }

static int table_column(const table_source_line *line, int byte);

/* Grid columns count scalars, with tabs expanded at four-column stops. Each
 * position retains the input byte that authored it; slicing never loses tabs
 * or UTF-8 provenance and never consults terminal display width. */
static bool table_source_columns(table_source *source, size_t index) {
    if (!table_source_get(source, index)) {
        return false;
    }
    table_source_line *line = &source->lines[index];
    if (line->columns_ready) {
        return true;
    }
    table_workspace *workspace = source->workspace;
    size_t length = (size_t)(line->length - line->offset);
    if (length > (SIZE_MAX - workspace->bytes_count - 1) / 4) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    {
        void *grown = table_reserve(source, workspace->bytes, &workspace->bytes_capacity,
                                    workspace->bytes_count + 4 * length + 1, sizeof(*workspace->bytes));
        if (!grown) {
            markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        workspace->bytes = grown;
    }
    source->workspace->work.geometry_lines++;
    line->byte_offset = workspace->bytes_count;
    int *bytes = workspace->bytes + line->byte_offset;
    /* The line's extent is read once: every position stored below is an
     * `int`, which the compiler must otherwise assume could be the line's own
     * length and reload it for every character. Only a tab needs the line's
     * origin, so only a tab reads it. */
    const unsigned char *data = line->data;
    const int end = line->length;
    for (int byte = line->offset, column = 0;;) {
        /* A character's width comes from its first byte alone, so on input
         * that is not UTF-8 the last one can claim bytes past the end. The
         * walk stops there all the same, and the end is where it stops. */
        if (byte >= end) {
            bytes[column] = end;
            line->columns = column;
            line->columns_ready = true;
            workspace->bytes_count += (size_t)column + 1;
            /* The block parser measured this line's indentation with the
             * same stops: geometry and indentation are one measurement. */
            assert(table_column(line, line->first) == line->indent);
            return true;
        }
        if (column > INT_MAX - 4) {
            markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        if (data[byte] == '\t') {
            int spaces = table_tab_width(table_line_origin(line), column);
            while (spaces--) {
                bytes[column++] = byte;
            }
            byte++;
        } else {
            bytes[column++] = byte;
            byte += markdown_core_utf8proc_width(data[byte]);
        }
    }
}

static const int *table_line_bytes(const table_source_line *line) { return line->workspace->bytes + line->byte_offset; }

static int table_character(const table_source_line *line, int column) {
    if (column < 0 || column >= line->columns) {
        return 0;
    }
    int c = line->data[table_line_bytes(line)[column]];
    return c == '\t' ? ' ' : c;
}

/* Accessors are pure. Charge a scan once for its visited span, including
 * the non-space probe that terminates it, instead of writing the counter
 * for each character. Other loops charge their bounded probe range. */
static int table_skip_spaces(const table_source_line *line, int first, int end) {
    int start = first;
    while (first < end && table_character(line, first) == ' ') {
        first++;
    }
    line->workspace->work.scan += (size_t)(first - start) + (first < end);
    return first;
}

static int table_trim_spaces(const table_source_line *line, int first, int end) {
    int last = end;
    while (end > first && table_character(line, end - 1) == ' ') {
        end--;
    }
    line->workspace->work.scan += (size_t)(last - end) + (end > first);
    return end;
}

static int table_byte(const table_source_line *line, int column) {
    return column >= line->columns ? line->length : table_line_bytes(line)[column < 0 ? 0 : column];
}

static int table_column(const table_source_line *line, int byte) {
    int low = 0, high = line->columns;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (table_line_bytes(line)[middle] < byte) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* Cache lexical facts before allocating any separator geometry. The same
 * re2c iterator validates a line and later materializes its authored intervals. */
static size_t table_dash_count(table_source *source, size_t index) {
    if (!table_source_get(source, index)) {
        return 0;
    }
    table_source_line *line = &source->lines[index];
    if (!line->dashes_scanned) {
        line->dashes_scanned = true;
        source->workspace->work.separator_scans++;
        if (line->indent > 3) {
            return 0;
        }
        const unsigned char *p = line->data + line->offset, *from, *before;
        int result;
        do {
            before = p;
            result = scan_table_dash(&p, line->data + line->length, &from);
            source->workspace->work.scan += (size_t)(p - before) + 1;
            if (result > 0) {
                line->dash_count++;
                line->full_boundary = line->dash_count == 1 && p - from >= 3;
            }
        } while (result > 0);
        if (result < 0) {
            line->dash_count = 0;
            line->full_boundary = false;
        }
    }
    return line->dash_count;
}

static bool table_prepare_dashes(table_source *source, size_t index) {
    size_t count = table_dash_count(source, index);
    if (!count) {
        return false;
    }
    table_source_line *line = &source->lines[index];
    table_workspace *workspace = source->workspace;
    if (!line->dashes_ready) {
        if (count > SIZE_MAX - workspace->dashes_count) {
            markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        {
            void *grown = table_reserve(source, workspace->dashes, &workspace->dashes_capacity,
                                        workspace->dashes_count + count, sizeof(*workspace->dashes));
            if (!grown) {
                markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return false;
            }
            workspace->dashes = grown;
        }
        line->dash_offset = workspace->dashes_count;
        workspace->dashes_count += count;
        line->dashes_ready = true;
        const unsigned char *p = line->data + line->offset, *from, *before = p;
        int column = 0, origin = table_line_origin(line);
        for (size_t i = 0; i < count; i++) {
            scan_table_dash(&p, line->data + line->length, &from);
            source->workspace->work.scan += (size_t)(p - before);
            while (before < from) {
                column += *before++ == '\t' ? table_tab_width(origin, column) : 1;
            }
            int start = column;
            column += (int)(p - from);
            workspace->dashes[line->dash_offset + i] = (table_interval){start, column};
            before = p;
        }
    }
    return true;
}

/* Dash slices are offsets into the parse workspace, including across further
 * separator discovery. A caller obtains one value, never a reallocable view. */
static table_interval table_dash(const table_source *source, size_t runs, size_t index) {
    return source->workspace->dashes[runs + index];
}

static bool table_full_boundary(table_source *source, size_t index) {
    return table_dash_count(source, index) == 1 && source->lines[index].full_boundary;
}

static bool table_add_row(table_source *source, table_candidate *candidate, size_t first, size_t last) {
    {
        void *grown = table_reserve(source, candidate->rows, &candidate->row_capacity, candidate->row_count + 1,
                                    sizeof(*candidate->rows));
        if (!grown) {
            return false;
        }
        candidate->rows = grown;
    }
    candidate->rows[candidate->row_count++] = (table_source_row){first, last, candidate->cell_count, 0};
    return true;
}

static bool table_add_cell(table_source *source, table_candidate *candidate, size_t first, size_t last, int left,
                           int right, int start, int end) {
    {
        void *grown = table_reserve(source, candidate->cells, &candidate->cell_capacity, candidate->cell_count + 1,
                                    sizeof(*candidate->cells));
        if (!grown) {
            return false;
        }
        candidate->cells = grown;
    }
    candidate->cells[candidate->cell_count++] =
        (table_source_cell){first, last, left, right, candidate->row_count - 1, 1, 1, start, end};
    candidate->rows[candidate->row_count - 1].count++;
    return true;
}

/* WHERE A TABLE STARTS. The lines of a simple, multiline or grid table
 * share one column geometry, so its left edge belongs to the table, not to a
 * line: the indentation the lines that fix that geometry share -- all of a
 * multiline or grid table's lines, and a simple table's header, dashes and
 * first row, the lines it opens with. Columns before the margin are blank on
 * every one of them -- block indentation, which never changes a block's
 * meaning -- so the first column, the table and each row begin at the margin
 * wherever that indentation came from. The separator is one of these lines,
 * so the margin never lies right of the first dash run; a line whose text
 * starts left of that run moves the margin to its text, which the first
 * column owns. A row whose first line is indented further still begins at the
 * margin: its leading columns are the first cell's, and an empty first cell
 * sits there. A later simple row indented less begins at its own text, which
 * its first cell owns (`table_rectangular_row`). A pipe row shares no
 * geometry, so its margin is its own. */
static int table_margin(table_source *source, size_t first, size_t last) {
    int margin = INT_MAX;
    for (size_t i = first; i <= last; i++) {
        if (source->lines[i].indent < margin) {
            margin = source->lines[i].indent;
        }
    }
    source->workspace->work.scan += last - first + 1;
    return margin;
}

/* The byte at a line's margin: where the table, or a row, begins on it. The
 * walk stays inside the indentation the block parser has already measured
 * and needs no column map; a multiline table's opening boundary never gets
 * one. A tab that spans the margin is the byte there, as the column map
 * would say, and a line indented less than the margin -- a later simple row
 * -- begins at its first non-space byte. */
static int table_margin_byte(const table_source_line *line, int margin) {
    const unsigned char *data = line->data;
    int byte = line->offset;
    for (int column = 0, first = line->first; byte < first; byte++) {
        column += data[byte] == '\t' ? table_tab_width(table_line_origin(line), column) : 1;
        if (column > margin) {
            break;
        }
    }
    return byte;
}

/* `runs` are the dash runs the row is cut at, one per column: the
 * workspace's while the table is a candidate, the table's own once it reads
 * its rows one at a time. */
static bool table_rectangular_row(table_source *source, table_candidate *candidate, size_t first, size_t last,
                                  const table_interval *runs) {
    for (size_t i = first; i <= last; i++) {
        if (!table_source_columns(source, i)) {
            return false;
        }
    }
    if (!table_add_row(source, candidate, first, last)) {
        return false;
    }
    /* Interior columns meet at dash-run starts; the outer two reach the
     * table's edges, the margin and the line's end. A row whose text lies
     * left of the margin -- a later simple row indented less -- begins at its
     * text instead, as the row itself does (`table_margin_byte`). */
    int row_left = source->lines[first].indent < candidate->margin ? source->lines[first].indent : candidate->margin;
    for (size_t column = 0; column < candidate->column_count; column++) {
        int left = column ? runs[column].start : row_left;
        int right = column + 1 < candidate->column_count ? runs[column + 1].start : INT_MAX;
        size_t cell_first = first, cell_last = last;
        if (candidate->block_content) {
            while (cell_first < cell_last && source->lines[cell_first].columns <= left) {
                cell_first++;
            }
            while (cell_last > cell_first && source->lines[cell_last].columns <= left) {
                cell_last--;
            }
        }
        table_source_line *begin = &source->lines[cell_first], *finish = &source->lines[cell_last];
        int start = table_byte(begin, left), end = table_byte(finish, right);
        if (!candidate->block_content) {
            while (start < end && markdown_core_is_whitespace(begin->data[start])) {
                start++;
            }
            while (end > start && markdown_core_is_whitespace(begin->data[end - 1])) {
                end--;
            }
        }
        if (end <= start) {
            start = table_byte(begin, left);
            end = start + 1;
        }
        if (start >= begin->length) {
            start = begin->length ? begin->length - 1 : 0;
        }
        if (end > finish->length) {
            end = finish->length;
        }
        if (!table_add_cell(source, candidate, cell_first, cell_last, left, right, start + 1, end)) {
            return false;
        }
    }
    return true;
}

static bool table_set_columns(table_source *source, table_candidate *candidate, size_t runs, size_t count,
                              size_t alignment_line, bool widths) {
    if (!table_source_columns(source, alignment_line)) {
        return false;
    }
    candidate->column_count = count;
    if (!table_candidate_columns(source, candidate, count)) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    table_source_line *line = &source->lines[alignment_line];
    double total = 0;
    for (size_t i = 0; i < count; i++) {
        total += (i + 1 < count ? table_dash(source, runs, i + 1).start : table_dash(source, runs, i).end) -
                 table_dash(source, runs, i).start;
    }
    /* ALIGNMENT IS READ AGAINST EACH COLUMN'S OWN DASH RUN, the first
     * column's included: the text has leading space when the run's first
     * column is blank, and room on the right when it ends before the run
     * does -- the segment Pandoc's `alignType` reads. The first column used to
     * be probed at the container's content instead, where an indented table
     * has its indentation, so indenting a table one to three spaces made its
     * first column right- or center-aligned. Where the table's text begins
     * is the margin's business (`table_margin`), not alignment's. */
    for (size_t i = 0; i < count; i++) {
        table_interval run = table_dash(source, runs, i);
        int right = i + 1 < count ? table_dash(source, runs, i + 1).start : line->columns;
        if (right > line->columns) {
            right = line->columns;
        }
        right = table_trim_spaces(line, run.start, right);
        bool occupied = right > run.start;
        bool left_space = table_character(line, run.start) == ' ';
        bool right_space = right < run.end;
        candidate->columns[i].flow = !occupied    ? MARKDOWN_CORE_FLOW_NONE
                                     : left_space ? (right_space ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_RIGHT)
                                                  : (right_space ? MARKDOWN_CORE_FLOW_LEFT : MARKDOWN_CORE_FLOW_NONE);
        if (widths) {
            candidate->columns[i].relative.has_value = true;
            candidate->columns[i].relative.value =
                ((i + 1 < count ? table_dash(source, runs, i + 1).start : table_dash(source, runs, i).end) -
                 table_dash(source, runs, i).start) /
                total;
        }
    }
    source->workspace->work.scan += count; /* One left-edge probe per column. */
    return true;
}

typedef struct {
    table_source *source;
    size_t index;
} table_block_reader;

static int table_read_block_line(void *context, markdown_core_chunk *input, int *first, int *indent) {
    table_block_reader *reader = context;
    if (!table_source_get(reader->source, ++reader->index)) {
        return 0;
    }
    table_source_line *line = &reader->source->lines[reader->index];
    *input = (markdown_core_chunk){(unsigned char *)line->data, line->input_length, 0};
    *first = line->first;
    *indent = line->indent;
    return 1;
}

static bool table_has_block_start(table_source *source, size_t index, bool paragraph) {
    table_source_line *line = &source->lines[index];
    markdown_core_chunk chunk = {(unsigned char *)line->data, line->input_length, 0};
    table_block_reader context = {source, index};
    markdown_core_block_reader reader = {&context, table_read_block_line};
    return markdown_core_parser_has_block_start(source->parser, source->lookahead.parent, &chunk, line->first,
                                                line->first_column, line->indent, paragraph, &reader);
}

static bool table_header_allowed(table_source *source, size_t index) {
    return !table_has_block_start(source, index, false);
}

enum {
    TABLE_NO_SEGMENTED_SEPARATOR = 1u,
    TABLE_NO_CLOSING_BOUNDARY = 2u,
    TABLE_NO_SIMPLE_FOOTER = 4u,
    TABLE_NO_GRID = 8u
};

static markdown_core_line_facts *table_search_fact(table_source *source, size_t index) {
    if (source->decides || (source->aside && markdown_core_parser_end_read(source->parser))) {
        return NULL;
    }
    table_source_line *line = &source->lines[index];
    markdown_core_line_facts *fact = markdown_core_parser_get_line_facts(source->parser, line->line);
    if (fact && (fact->table_container != source->lookahead.parent || fact->table_offset != line->offset)) {
        fact->table_container = source->lookahead.parent;
        fact->table_offset = line->offset;
        fact->table_absent = 0;
    }
    return fact;
}

static bool table_search_absent(table_source *source, size_t index, unsigned grammar) {
    markdown_core_line_facts *fact = table_search_fact(source, index);
    return fact && (fact->table_absent & grammar);
}

/* A failed search establishes absence for every suffix it traversed. Publish
 * that grammatical fact once, so another opener cannot rescan the same run. */
static void table_search_finish(table_source *source, size_t first, size_t after, unsigned grammar) {
    for (size_t i = first; i < after; i++) {
        markdown_core_line_facts *fact = table_search_fact(source, i);
        if (fact) {
            fact->table_absent |= grammar;
        }
    }
}

/* A simple footer must match every authored interval, not merely the number
 * of columns or a hash. Each comparison visits at most the opener's bytes. */
static bool table_same_runs(table_workspace *workspace, const table_interval *runs, const table_interval *others,
                            size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (runs[i].start != others[i].start || runs[i].end != others[i].end) {
            workspace->work.scan += i + 1;
            return false;
        }
    }
    workspace->work.scan += count;
    return true;
}

static bool table_same_dashes(table_source *source, size_t left, size_t right) {
    size_t count = table_dash_count(source, left);
    if (count < 2 || table_dash_count(source, right) != count) {
        return false;
    }
    if (!table_prepare_dashes(source, left) || !table_prepare_dashes(source, right)) {
        return false;
    }
    const table_interval *dashes = source->workspace->dashes;
    return table_same_runs(source->workspace, dashes + source->lines[left].dash_offset,
                           dashes + source->lines[right].dash_offset, count);
}

/* Exact interval keys use sixteen radix digits per start/end pair and a
 * terminator. A variable-length radix traversal visits only existing key
 * digits, so distinct widths, offsets and column counts cost source-linear
 * work without hashing or repeatedly comparing long shared prefixes. */
static unsigned table_separator_digit(table_source *source, table_separator_key key, size_t digit) {
    if (digit / 16 >= key.count) {
        return 0;
    }
    table_interval run = source->workspace->dashes[key.runs + digit / 16];
    uint32_t position = (uint32_t)(digit % 16 < 8 ? run.start : run.end);
    return 1 + ((position >> (28 - 4 * (digit % 8))) & 15);
}

/* A failed headerless search has captured the whole nonblank run. Group its
 * separator geometries once: only the final occurrence of each exact key has
 * no later footer. Other occurrences remain eligible, including valid suffix
 * tables. Facts retain the shared container/offset ownership. */
static void table_simple_search_finish(table_source *source, size_t first, size_t last) {
    size_t capacity = last - first + 1, count = 0;
    table_workspace *workspace = source->workspace;
    {
        void *grown = table_reserve(source, workspace->separator_keys, &workspace->separator_keys_capacity, capacity,
                                    sizeof(*workspace->separator_keys));
        if (!grown) {
            return;
        }
        workspace->separator_keys = grown;
    }
    {
        void *grown = table_reserve(source, workspace->separator_scratch, &workspace->separator_scratch_capacity,
                                    capacity, sizeof(*workspace->separator_scratch));
        if (!grown) {
            return;
        }
        workspace->separator_scratch = grown;
    }
    {
        void *grown = table_reserve(source, workspace->separator_groups, &workspace->separator_groups_capacity,
                                    capacity, sizeof(*workspace->separator_groups));
        if (!grown) {
            return;
        }
        workspace->separator_groups = grown;
    }
    table_separator_key *keys = workspace->separator_keys, *scratch = workspace->separator_scratch;
    table_separator_group *groups = workspace->separator_groups;
    for (size_t i = first; i <= last; i++) {
        size_t columns = table_dash_count(source, i);
        if (columns > 1) {
            if (!table_prepare_dashes(source, i)) {
                goto done;
            }
            keys[count++] = (table_separator_key){source->lines[i].dash_offset, columns, i};
        }
    }
    size_t pending = 1;
    groups[0] = (table_separator_group){0, count, 0};
    while (pending) {
        table_separator_group group = groups[--pending];
        if (group.count == 1) {
            markdown_core_line_facts *fact = table_search_fact(source, keys[group.first].line);
            if (fact) {
                fact->table_absent |= TABLE_NO_SIMPLE_FOOTER;
            }
            continue;
        }
        size_t lengths[17] = {0}, offsets[17], cursors[17];
        source->workspace->work.scan += 2 * group.count;
        for (size_t i = group.first; i < group.first + group.count; i++) {
            lengths[table_separator_digit(source, keys[i], group.digit)]++;
        }
        size_t offset = group.first;
        for (size_t b = 0; b < 17; b++) {
            cursors[b] = offsets[b] = offset;
            offset += lengths[b];
        }
        for (size_t i = group.first; i < group.first + group.count; i++) {
            unsigned bucket = table_separator_digit(source, keys[i], group.digit);
            scratch[cursors[bucket]++] = keys[i];
        }
        memcpy(keys + group.first, scratch + group.first, group.count * sizeof(*keys));
        if (lengths[0]) {
            size_t final = 0;
            for (size_t i = offsets[0]; i < offsets[0] + lengths[0]; i++) {
                if (keys[i].line > final) {
                    final = keys[i].line;
                }
            }
            markdown_core_line_facts *fact = table_search_fact(source, final);
            if (fact) {
                fact->table_absent |= TABLE_NO_SIMPLE_FOOTER;
            }
        }
        for (size_t b = 1; b < 17; b++) {
            if (lengths[b]) {
                groups[pending++] = (table_separator_group){offsets[b], lengths[b], group.digit + 1};
            }
        }
    }
done:
    return;
}

/* A SIMPLE TABLE OPENS ON THE LINES THAT DECIDE IT: its header, its dashes
 * and the line after them, which is its first row or its footer, or, with no
 * header, its dashes and every line through the footer such a table needs.
 * It makes a row of each line it read after its dashes, and a footer closes
 * it. A table its opening lines leave open reads each later line on its own
 * as that line comes (`matches`, `table_take_line`). */
static bool table_parse_simple(table_source *source, size_t start, table_candidate *candidate) {
    size_t count = table_dash_count(source, start);
    bool headerless = count > 1;
    if (headerless && table_search_absent(source, start, TABLE_NO_SIMPLE_FOOTER)) {
        goto failed;
    }
    if (!headerless) {
        if (!table_source_get(source, start + 1) || source->lines[start + 1].blanks ||
            (count = table_dash_count(source, start + 1)) < 2 || !table_header_allowed(source, start)) {
            goto failed;
        }
    }
    size_t delimiter = start + (headerless ? 0 : 1), body = delimiter + 1, end = delimiter;
    size_t deciding = headerless ? SIZE_MAX : body;
    if (!table_prepare_dashes(source, delimiter)) {
        goto failed;
    }
    bool footer = false;
    for (size_t i = body; i <= deciding && !footer && table_source_get(source, i) && !source->lines[i].blanks; i++) {
        end = i;
        footer = table_same_dashes(source, delimiter, i);
    }
    if (source->parser->error) {
        goto failed;
    }
    if (headerless && !footer) {
        table_simple_search_finish(source, delimiter, end);
        goto failed;
    }
    if (end == delimiter) {
        goto failed;
    }
    size_t runs = source->lines[delimiter].dash_offset;
    if (!table_set_columns(source, candidate, runs, count, headerless ? body : start, false)) {
        goto failed;
    }
    candidate->simple = !footer;
    candidate->runs = runs;
    candidate->first = start;
    candidate->last = end;
    /* The line after the dashes is the first row, or a footer, which has the
     * indentation of the dashes it repeats. */
    candidate->margin = table_margin(source, start, body);
    candidate->head_count = headerless ? 0 : 1;
    if (!headerless && !table_rectangular_row(source, candidate, start, start, source->workspace->dashes + runs)) {
        goto failed;
    }
    for (size_t i = body; i <= end - (footer ? 1u : 0u); i++) {
        if (!table_rectangular_row(source, candidate, i, i, source->workspace->dashes + runs)) {
            goto failed;
        }
    }
    return true;
failed:
    table_candidate_reset(candidate);
    return false;
}

static bool table_parse_multiline(table_source *source, size_t start, table_candidate *candidate) {
    size_t count = 0;
    bool header = table_full_boundary(source, start);
    size_t delimiter = start;
    if (header) {
        if (table_search_absent(source, start, TABLE_NO_SEGMENTED_SEPARATOR)) {
            goto failed;
        }
        for (delimiter = start + 1; table_source_get(source, delimiter); delimiter++) {
            if (source->lines[delimiter].blanks) {
                table_search_finish(source, start, delimiter, TABLE_NO_SEGMENTED_SEPARATOR);
                goto failed;
            }
            if ((count = table_dash_count(source, delimiter)) > 1) {
                break;
            }
        }
        if (delimiter >= source->count) {
            table_search_finish(source, start, source->count, TABLE_NO_SEGMENTED_SEPARATOR);
        }
        if (delimiter == start + 1 || delimiter >= source->count || count < 2) {
            goto failed;
        }
    } else if ((count = table_dash_count(source, start)) < 2 || !table_source_get(source, start + 1) ||
               source->lines[start + 1].blanks) {
        /* A headless table's body starts on the line after its opening
         * separator, as Pandoc's `multilineRow` requires: a blank line there
         * leaves the dashes a rule and what follows ordinary blocks. Rows are
         * separated by blank lines only once the body has begun. */
        goto failed;
    }
    if (table_search_absent(source, delimiter, TABLE_NO_CLOSING_BOUNDARY)) {
        goto failed;
    }
    size_t end = delimiter + 1;
    /* This search judges with `table_full_boundary` and `.blanks`, both of
     * which read RAW BYTES. It must not build the per-scalar column map: the
     * lines it walks may never become part of a table, and every routine that
     * does read the map builds it for the lines it reads. On `block-hr.x1` a
     * lone ` -  -  -  -  -` keeps a headerless multiline alive to EOF, and the
     * map built here covered 56,680 of that document's 56,704 non-blank
     * characters for a candidate that then failed. */
    for (; table_source_get(source, end); end++) {
        if (table_full_boundary(source, end) && (!table_source_get(source, end + 1) || source->lines[end + 1].blanks)) {
            break;
        }
    }
    if (end >= source->count) {
        table_search_finish(source, delimiter, source->count, TABLE_NO_CLOSING_BOUNDARY);
    }
    if (end >= source->count || end == delimiter + 1) {
        goto failed;
    }
    if (!table_prepare_dashes(source, delimiter)) {
        goto failed;
    }
    size_t runs = source->lines[delimiter].dash_offset;
    candidate->block_content = true;
    candidate->padding_limit = INT_MAX;
    candidate->first = start;
    candidate->last = end;
    candidate->margin = table_margin(source, start, end);
    candidate->head_count = header ? 1 : 0;
    if (!table_set_columns(source, candidate, runs, count, header ? start + 1 : delimiter + 1, true)) {
        goto failed;
    }
    if (header &&
        !table_rectangular_row(source, candidate, start + 1, delimiter - 1, source->workspace->dashes + runs)) {
        goto failed;
    }
    size_t first = delimiter + 1, body_count = 0;
    for (size_t i = first + 1; i <= end; i++) {
        if (i == end || source->lines[i].blanks) {
            if (!table_rectangular_row(source, candidate, first, i - 1, source->workspace->dashes + runs)) {
                goto failed;
            }
            body_count++;
            first = i;
        }
    }
    if (body_count == 1 && !source->lines[end].blanks) {
        goto failed;
    }
    return true;
failed:
    table_candidate_reset(candidate);
    return false;
}

static int table_grid_root(table_source *source, int *parents, int column) {
    int root = column;
    size_t work = 1;
    while (parents[root] != root) {
        work++;
        root = parents[root];
    }
    while (parents[column] != column) {
        work++;
        int next = parents[column];
        parents[column] = root;
        column = next;
    }
    source->workspace->work.scan += work;
    return root;
}

static int table_horizontal_bytes(const table_source_line *line, int first, int last) {
    line->workspace->work.scan += (size_t)(last - first);
    line->workspace->work.horizontal += (size_t)(last - first);
    return scan_table_horizontal(line->data, last, first);
}

/* A whole-line border is one immutable lexical fact. Opening, suffix proof,
 * group discovery and region construction all consume it. A cell interval is
 * a different grammar extent: alignment colons or partial walls forbid
 * inferring its result from the whole line. Both extents use the same scanner. */
static int table_full_horizontal(table_source_line *line) {
    if (!line->horizontal_scanned) {
        int end = line->length;
        while (end > line->first && markdown_core_is_space_or_tab(line->data[end - 1])) {
            end--;
        }
        line->workspace->work.scan += (size_t)(line->length - end);
        line->horizontal_end = end;
        line->horizontal_kind = (unsigned char)table_horizontal_bytes(line, line->first, end);
        line->horizontal_scanned = true;
    }
    return line->horizontal_kind;
}

static int table_horizontal(table_source_line *line, int left, int right) {
    if (left < 0 || right >= line->columns || left >= right) {
        return 0;
    }
    int first = table_byte(line, left), end = table_byte(line, right) + 1;
    if (first == line->first) {
        int kind = table_full_horizontal(line);
        if (end == line->horizontal_end) {
            return kind;
        }
    }
    return table_horizontal_bytes(line, first, end);
}

static void table_grid_join(table_source *source, int *parents, int *sizes, int a, int b) {
    a = table_grid_root(source, parents, a);
    b = table_grid_root(source, parents, b);
    if (a == b) {
        return;
    }
    if (sizes[a] < sizes[b]) {
        int swap = a;
        a = b;
        b = swap;
    }
    parents[b] = a;
    sizes[a] += sizes[b];
}

static void table_region_join(table_source *source, int *parents, int *sizes, table_grid_region *regions, int a,
                              int b) {
    a = table_grid_root(source, parents, a);
    b = table_grid_root(source, parents, b);
    if (a == b) {
        return;
    }
    table_grid_join(source, parents, sizes, a, b);
    int root = table_grid_root(source, parents, a), other = root == a ? b : a;
    table_grid_region *keep = &regions[root], *drop = &regions[other];
    if (drop->top < keep->top) {
        keep->top = drop->top;
    }
    if (drop->bottom > keep->bottom) {
        keep->bottom = drop->bottom;
    }
    if (drop->left < keep->left) {
        keep->left = drop->left;
    }
    if (drop->right > keep->right) {
        keep->right = drop->right;
    }
    keep->area += drop->area;
    drop->area = 0;
}

static bool table_region_complete(table_source *source, table_candidate *candidate, table_grid_region region,
                                  size_t rows, table_grid_region **closed, size_t *count) {
    if (region.area != (region.bottom - region.top + 1) * (region.right - region.left + 1) ||
        (region.top < candidate->head_count && region.bottom >= candidate->head_count) ||
        (region.top < rows - candidate->foot_count && region.bottom >= rows - candidate->foot_count)) {
        return false;
    }
    table_workspace *workspace = source->workspace;
    {
        void *grown = table_reserve(source, workspace->closed, &workspace->closed_capacity, *count + 1,
                                    sizeof(*workspace->closed));
        if (!grown) {
            return false;
        }
        workspace->closed = grown;
    }
    *closed = workspace->closed;
    (*closed)[(*count)++] = region;
    return true;
}

static uint64_t table_region_source_key(const void *entry) {
    const table_grid_region *region = entry;
    return ((uint64_t)region->top << 32) | region->left;
}

/* Candidate subdivisions are not yet logical rows: a '+' inside a completed
 * cell is content. Derive row coordinates from the cell perimeters, retaining
 * intermediate '+' markers on vertical edges even when no cell starts there.
 * Disjoint regions bound the total perimeter work by the source grid area. */
static bool table_grid_rows(table_source *source, table_candidate *candidate, const int *columns, size_t *boundaries,
                            size_t row_count, table_grid_region *regions, size_t count) {
    table_workspace *workspace = source->workspace;
    {
        void *grown = table_reserve(source, workspace->row_indices, &workspace->row_indices_capacity, row_count + 1,
                                    sizeof(*workspace->row_indices));
        if (!grown) {
            return false;
        }
        workspace->row_indices = grown;
    }
    size_t *indices = workspace->row_indices;
    memset(indices, 0, (row_count + 1) * sizeof(*indices));
    for (size_t i = 0; i < count; i++) {
        table_grid_region *region = &regions[i];
        indices[region->top] = 1;
        indices[region->bottom + 1] = 1;
        source->workspace->work.scan += 2 * (region->bottom - region->top);
        for (size_t b = region->top + 1; b <= region->bottom; b++) {
            table_source_line *line = &source->lines[boundaries[b]];
            if (table_character(line, columns[region->left]) == '+' ||
                table_character(line, columns[region->right + 1]) == '+') {
                indices[b] = 1;
            }
        }
    }
    size_t boundary_count = 0;
    for (size_t b = 0; b <= row_count; b++) {
        bool keep = indices[b] != 0;
        indices[b] = boundary_count;
        if (keep) {
            boundaries[boundary_count++] = boundaries[b];
        }
    }
    candidate->head_count = indices[candidate->head_count];
    candidate->foot_count = boundary_count - 1 - indices[row_count - candidate->foot_count];
    for (size_t i = 0; i < count; i++) {
        regions[i].top = indices[regions[i].top];
        regions[i].bottom = indices[regions[i].bottom + 1] - 1;
    }
    size_t cell_index = 0, width = candidate->column_count;
    for (size_t r = 0; r + 1 < boundary_count; r++) {
        size_t first = boundaries[r] + 1, next_line = boundaries[r + 1];
        size_t last = table_horizontal(&source->lines[next_line], columns[0], columns[width]) && next_line > first
                          ? next_line - 1
                          : next_line;
        if (!table_add_row(source, candidate, first, last)) {
            return false;
        }
        while (cell_index < count && regions[cell_index].top == r) {
            table_grid_region *region = &regions[cell_index++];
            size_t end = boundaries[region->bottom + 1] - 1;
            table_source_line *begin = &source->lines[first], *finish = &source->lines[end < first ? first : end];
            int left = columns[region->left] + 1, right = columns[region->right + 1];
            if (!table_add_cell(source, candidate, first, end, left, right, table_byte(begin, left) + 1,
                                table_byte(finish, right))) {
                return false;
            }
            table_source_cell *cell = &candidate->cells[candidate->cell_count - 1];
            cell->rowspan = (int64_t)(region->bottom - region->top + 1);
            cell->colspan = (int64_t)(region->right - region->left + 1);
        }
    }
    return true;
}

/* A connected region can acquire missing parts through a later row, so test
 * its rectangle only when it leaves the frontier. Compact surviving roots at
 * each row: union state is bounded by two row widths, even when one cell covers
 * the whole source. Closed regions are exactly the candidate's output cells. */
static bool table_grid_cells(table_source *source, table_candidate *candidate, const int *columns, size_t *boundaries,
                             size_t row_count) {
    size_t width = candidate->column_count;
    if (row_count > SIZE_MAX / width || row_count * width > INT_MAX || width > INT_MAX / 2) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    size_t capacity = 2 * width, active_count = 0, closed_count = 0;
    table_workspace *workspace = source->workspace;
    {
        void *grown = table_reserve(source, workspace->region_parents, &workspace->region_parents_capacity, capacity,
                                    sizeof(*workspace->region_parents));
        if (!grown) {
            return false;
        }
        workspace->region_parents = grown;
    }
    {
        void *grown = table_reserve(source, workspace->region_sizes, &workspace->region_sizes_capacity, capacity,
                                    sizeof(*workspace->region_sizes));
        if (!grown) {
            return false;
        }
        workspace->region_sizes = grown;
    }
    {
        void *grown = table_reserve(source, workspace->region_next, &workspace->region_next_capacity, capacity,
                                    sizeof(*workspace->region_next));
        if (!grown) {
            return false;
        }
        workspace->region_next = grown;
    }
    {
        void *grown = table_reserve(source, workspace->region_previous, &workspace->region_previous_capacity, width,
                                    sizeof(*workspace->region_previous));
        if (!grown) {
            return false;
        }
        workspace->region_previous = grown;
    }
    {
        void *grown = table_reserve(source, workspace->regions, &workspace->regions_capacity, capacity,
                                    sizeof(*workspace->regions));
        if (!grown) {
            return false;
        }
        workspace->regions = grown;
    }
    {
        void *grown = table_reserve(source, workspace->region_scratch, &workspace->region_scratch_capacity, width,
                                    sizeof(*workspace->region_scratch));
        if (!grown) {
            return false;
        }
        workspace->region_scratch = grown;
    }
    int *parents = workspace->region_parents, *sizes = workspace->region_sizes;
    int *next = workspace->region_next, *previous = workspace->region_previous;
    table_grid_region *regions = workspace->regions, *scratch = workspace->region_scratch, *closed = workspace->closed;
    bool valid = false;
    if (capacity > source->workspace->work.frontier_peak) {
        source->workspace->work.frontier_peak = capacity;
    }
    for (size_t r = 0; r < row_count; r++) {
        size_t used = active_count + width;
        for (size_t i = 0; i < used; i++) {
            parents[i] = (int)i;
            sizes[i] = 1;
            next[i] = -1;
        }
        for (size_t c = 0; c < width; c++) {
            regions[active_count + c] = (table_grid_region){r, r, c, c, 1};
        }
        source->workspace->work.scan += (width - 1) * (boundaries[r + 1] - boundaries[r] + 1);
        for (size_t c = 1; c < width; c++) {
            bool wall = true;
            for (size_t line = boundaries[r]; line <= boundaries[r + 1]; line++) {
                int ch = table_character(&source->lines[line], columns[c]);
                if (ch != '|' && ch != '+') {
                    wall = false;
                }
            }
            if (!wall) {
                table_region_join(source, parents, sizes, regions, (int)(active_count + c - 1),
                                  (int)(active_count + c));
            }
        }
        if (r) {
            table_source_line *line = &source->lines[boundaries[r]];
            bool group = table_horizontal(line, columns[0], columns[width]) == '=';
            for (size_t c = 0; c < width; c++) {
                int wall = table_horizontal(line, columns[c], columns[c + 1]);
                if (wall == '=' && !group) {
                    goto done;
                }
                if (!wall) {
                    table_region_join(source, parents, sizes, regions, previous[c], (int)(active_count + c));
                }
            }
        }
        size_t survivors = 0;
        for (size_t c = 0; c < width; c++) {
            int root = table_grid_root(source, parents, (int)(active_count + c));
            if (next[root] < 0) {
                next[root] = (int)survivors;
                scratch[survivors++] = regions[root];
            }
            previous[c] = next[root];
        }
        for (size_t i = 0; i < used; i++) {
            if (parents[i] == (int)i && next[i] < 0 && regions[i].area &&
                !table_region_complete(source, candidate, regions[i], row_count, &closed, &closed_count)) {
                goto done;
            }
        }
        memcpy(regions, scratch, survivors * sizeof(*regions));
        active_count = survivors;
    }
    for (size_t i = 0; i < active_count; i++) {
        if (!table_region_complete(source, candidate, regions[i], row_count, &closed, &closed_count)) {
            goto done;
        }
    }
    size_t order_work = source->parser->source_order.work;
    if (!markdown_core_order_source_entries(&source->parser->source_order, closed, closed_count, sizeof(*closed),
                                            table_region_source_key)) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        goto done;
    }
    source->workspace->work.scan += source->parser->source_order.work - order_work;
    valid = table_grid_rows(source, candidate, columns, boundaries, row_count, closed, closed_count);
done:
    return valid;
}

/* Establish the opening grammar in borrowed source bytes before allocating
 * scalar columns or topology. Interior whitespace is not a horizontal border;
 * expanding its tabs first would amplify ordinary paragraph fallback storage. */
static bool table_grid_opening(table_source *source, size_t index, int *left, int *right) {
    if (!table_source_get(source, index) || source->lines[index].indent > 3 ||
        source->lines[index].first >= source->lines[index].length ||
        source->lines[index].data[source->lines[index].first] != '+') {
        return false;
    }
    table_source_line *line = &source->lines[index];
    if (!table_full_horizontal(line) || !table_source_columns(source, index)) {
        return false;
    }
    *left = table_column(line, line->first);
    *right = table_column(line, line->horizontal_end - 1);
    return true;
}

/* A run's extent and full-width '=' separators are shared by suffix openers
 * with the same outer margins. Walk backwards once to prove which suffixes
 * cannot satisfy the closing/head/foot grammar. Different margins and suffixes
 * with valid separator counts remain eligible for full region validation. */
static bool table_grid_search_finish(table_source *source, size_t first, size_t last, int left, int right,
                                     bool complete) {
    int closing = complete ? table_horizontal(&source->lines[last], left, right) : 0;
    size_t equals = 0;
    bool valid = false;
    for (size_t i = last;; i--) {
        if (closing && table_horizontal(&source->lines[i], left, right) == '=') {
            equals++;
        }
        valid = i < last && closing && (closing == '=' ? equals >= 2 && equals <= 3 : equals <= 1);
        int a, b;
        if (!valid && table_grid_opening(source, i, &a, &b) && a == left && b == right) {
            markdown_core_line_facts *fact = table_search_fact(source, i);
            if (fact) {
                fact->table_absent |= TABLE_NO_GRID;
            }
        }
        if (i == first) {
            break;
        }
    }
    return valid;
}

static bool table_parse_grid(table_source *source, size_t start, table_candidate *candidate) {
    int *parents = NULL, *positions = NULL, *sizes = NULL;
    size_t *boundaries = NULL;
    size_t boundary_count = 0;
    int left, right;
    if (!table_source_get(source, start) || table_search_absent(source, start, TABLE_NO_GRID) ||
        !table_grid_opening(source, start, &left, &right)) {
        return false;
    }
    table_workspace *workspace = source->workspace;
    {
        void *grown = table_reserve(source, workspace->grid_parents, &workspace->grid_parents_capacity,
                                    (size_t)right + 1, sizeof(*workspace->grid_parents));
        if (!grown) {
            goto failed;
        }
        workspace->grid_parents = grown;
    }
    {
        void *grown = table_reserve(source, workspace->grid_positions, &workspace->grid_positions_capacity,
                                    (size_t)right + 1, sizeof(*workspace->grid_positions));
        if (!grown) {
            goto failed;
        }
        workspace->grid_positions = grown;
    }
    {
        void *grown = table_reserve(source, workspace->grid_sizes, &workspace->grid_sizes_capacity, (size_t)right + 1,
                                    sizeof(*workspace->grid_sizes));
        if (!grown) {
            goto failed;
        }
        workspace->grid_sizes = grown;
    }
    parents = workspace->grid_parents;
    positions = workspace->grid_positions;
    sizes = workspace->grid_sizes;
    boundaries = workspace->boundaries;
    for (int i = 0; i <= right; i++) {
        parents[i] = i;
        sizes[i] = 1;
    }
    size_t end = start;
    /* The scan from the opener's own line in a call that can wait reads no
     * end (THE OPENER'S CANDIDATE). */
    source->holds = start == 0 && source->lookahead.limit;
    for (size_t i = start; table_source_get(source, i); i++) {
        if (i > start && source->lines[i].blanks) {
            break;
        }
        if (!table_source_columns(source, i)) {
            goto failed;
        }
        table_source_line *line = &source->lines[i];
        source->workspace->work.scan++;
        /* A grid line BEGINS with its wall: its indentation reaches the
         * wall's column exactly. Text left of the wall is not a wall-led
         * line; it ends the candidate instead of being dropped from a table
         * that starts after it. */
        int first = table_character(line, left);
        if ((first != '+' && first != '|') || line->indent != left) {
            break;
        }
        /* A wall-led line the text has not finished does not tell where
         * its walls end. */
        if (source->holds && source->incomplete == i + 1) {
            source->exhausted = true;
            break;
        }
        int last = table_trim_spaces(line, left + 1, line->columns) - 1;
        source->workspace->work.scan++;
        int edge = table_character(line, right);
        if (last != right || (edge != '+' && edge != '|')) {
            table_grid_search_finish(source, start, end, left, right, false);
            goto failed;
        }
        int previous = -1;
        bool horizontal = true;
        source->workspace->work.scan += (size_t)(right - left) + 1;
        for (int c = left; c <= right; c++) {
            int ch = table_character(line, c);
            if (ch == '+') {
                if (previous >= 0 && horizontal) {
                    table_grid_join(source, parents, sizes, previous, c);
                }
                previous = c;
                horizontal = true;
            } else if (ch != '-' && ch != '=' && ch != ' ' && ch != ':') {
                horizontal = false;
            }
        }
        end = i;
    }
    /* A scan that waits where the text read so far ends, with no blank line
     * before it, decides nothing until a later line does. */
    if (source->holds && source->exhausted && !source->blanks) {
        source->undecided = true;
        goto failed;
    }
    source->holds = source->exhausted = false;
    if (source->parser->error || !table_grid_search_finish(source, start, end, left, right, true) ||
        table_grid_root(source, parents, left) != table_grid_root(source, parents, right)) {
        goto failed;
    }
    int root = table_grid_root(source, parents, left);
    size_t count = 0;
    for (int c = left; c <= right; c++) {
        if (table_grid_root(source, parents, c) == root) {
            positions[count++] = c;
        }
    }
    if (count < 2) {
        goto failed;
    }
    for (size_t c = 1; c < count; c++) {
        if (positions[c] - positions[c - 1] <= 1) {
            goto failed;
        }
    }
    candidate->column_count = count - 1;
    if (!table_candidate_columns(source, candidate, count - 1)) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        goto failed;
    }
    for (size_t i = start; i <= end; i++) {
        bool boundary = false;
        source->workspace->work.scan += count;
        for (size_t c = 0; c < count; c++) {
            boundary |= table_character(&source->lines[i], positions[c]) == '+';
        }
        if (boundary) {
            {
                void *grown = table_reserve(source, workspace->boundaries, &workspace->boundaries_capacity,
                                            boundary_count + 1, sizeof(*boundaries));
                if (!grown) {
                    goto failed;
                }
                workspace->boundaries = grown;
            }
            boundaries = workspace->boundaries;
            boundaries[boundary_count++] = i;
        }
    }
    if (boundary_count < 2 || boundaries[boundary_count - 1] != end) {
        goto failed;
    }
    candidate->first = start;
    candidate->last = end;
    /* Every grid line is indented exactly to the wall (above), so the shared
     * indentation `table_margin` would find is the wall's column. */
    candidate->margin = left;
    candidate->block_content = true;
    candidate->padding_limit = 1;
    size_t first_equal = SIZE_MAX, last_equal = SIZE_MAX, previous_equal = SIZE_MAX;
    for (size_t b = 0; b < boundary_count; b++) {
        size_t boundary = boundaries[b];
        table_source_line *line = &source->lines[boundary];
        bool equal = table_horizontal(line, left, right) == '=';
        if (equal) {
            if (first_equal == SIZE_MAX) {
                first_equal = b;
            }
            previous_equal = last_equal;
            last_equal = b;
        }
    }
    if (first_equal != SIZE_MAX && first_equal < boundary_count - 1) {
        candidate->head_count = first_equal;
    }
    if (last_equal == boundary_count - 1 && previous_equal != SIZE_MAX) {
        candidate->foot_count = boundary_count - 1 - previous_equal;
    }
    if (!table_grid_cells(source, candidate, positions, boundaries, boundary_count - 1)) {
        goto failed;
    }
    table_source_line *alignment = &source->lines[candidate->head_count ? boundaries[candidate->head_count] : start];
    double total = 0;
    for (size_t c = 0; c + 1 < count; c++) {
        total += positions[c + 1] - positions[c] - 1;
    }
    source->workspace->work.scan += 2 * (count - 1);
    for (size_t c = 0; c + 1 < count; c++) {
        bool l = table_character(alignment, positions[c] + 1) == ':',
             r = table_character(alignment, positions[c + 1] - 1) == ':';
        candidate->columns[c].flow = l ? (r ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_LEFT)
                                       : (r ? MARKDOWN_CORE_FLOW_RIGHT : MARKDOWN_CORE_FLOW_NONE);
        candidate->columns[c].relative =
            (markdown_core_optional_double){true, (positions[c + 1] - positions[c] - 1) / total};
    }
    return true;
failed:
    source->holds = source->exhausted = false;
    table_candidate_reset(candidate);
    return false;
}

static bool table_parse_pipe_header(table_source *source, size_t start, table_candidate *candidate) {
    if (!table_source_get(source, start + 1) || source->lines[start + 1].blanks) {
        return false;
    }
    table_source_line *head = &source->lines[start], *delimiter = &source->lines[start + 1];
    if (!scan_table_start(delimiter->data, delimiter->input_length, delimiter->first) ||
        !table_header_allowed(source, start) || !table_source_columns(source, start)) {
        return false;
    }
    head = &source->lines[start];
    delimiter = &source->lines[start + 1];
    pipe_row header, markers;
    bool matches =
        recognize_pipe_row((unsigned char *)head->data + head->first, head->input_length - head->first, &header) &&
        recognize_pipe_row((unsigned char *)delimiter->data + delimiter->first,
                           delimiter->input_length - delimiter->first, &markers) &&
        header.n_columns == markers.n_columns;
    if (!matches) {
        goto done;
    }
    candidate->column_count = header.n_columns;
    if (!table_candidate_columns(source, candidate, candidate->column_count)) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        matches = false;
        goto done;
    }
    candidate->first = start;
    candidate->last = start + 1;
    /* Pipes, not columns, delimit a pipe row's cells: no other line shares
     * its geometry, so its margin is its own indentation, as for the body
     * rows `try_opening_table_row` adds. */
    candidate->margin = table_margin(source, start, start);
    candidate->head_count = 1;
    candidate->pipe = true;
    if (!table_add_row(source, candidate, start, start)) {
        matches = false;
        goto done;
    }
    pipe_row_cursor header_cells = pipe_row_cells(&header), marker_cells = pipe_row_cells(&markers);
    node_cell geometry, marker_geometry;
    for (size_t i = 0; pipe_row_next(&header_cells, &geometry) && pipe_row_next(&marker_cells, &marker_geometry); i++) {
        const markdown_core_chunk *marker = &marker_geometry.content;
        bool l = marker->data[0] == ':', r = marker->data[marker->len - 1] == ':';
        candidate->columns[i].flow = l ? (r ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_LEFT)
                                       : (r ? MARKDOWN_CORE_FLOW_RIGHT : MARKDOWN_CORE_FLOW_NONE);
        const node_cell *cell = &geometry;
        int from = head->first + cell->start_offset, to = head->first + cell->end_offset + 1;
        if (!table_add_cell(source, candidate, start, start, table_column(head, from), table_column(head, to), from + 1,
                            to)) {
            matches = false;
            goto done;
        }
    }
done:

    if (!matches) {
        table_candidate_reset(candidate);
    }
    return matches;
}

/* An uncaptured line uses the same all-or-nothing separator grammar. */
static size_t table_dash_count_raw(const unsigned char *data, bufsize_t from, bufsize_t length) {
    const unsigned char *p = data + from, *end = data + length, *run;
    size_t count = 0;
    int result;
    do {
        result = scan_table_dash(&p, end, &run);
        if (result > 0) {
            count++;
        }
    } while (result > 0);
    return result < 0 ? 0 : count;
}

/* A separator line is the raw successor's SUFFIX. Container continuation only
 * strips a prefix -- indentation and container markers, never a byte of the
 * separator alphabet other than indentation -- so the stripped line a
 * recognizer reads is a suffix of the physical line, and when that stripped
 * line is a separator every one of its dash runs lies in the physical line's
 * longest trailing run of separator bytes. Counting there, backwards from the
 * line's end, is therefore a necessary condition; a prose line fails it at its
 * last letter instead of being read to its end, and a line whose dashes sit
 * among words (`well-known and so-called`) is no separator at all. The pipe
 * delimiter row (`[|]? :?-+:? ([|] :?-+:?)* [|]?`) adds `|` and `:` to the
 * simple separator's dashes and blanks. */
static size_t table_trailing_dash_runs(const unsigned char *start, const unsigned char *end, bool pipe) {
    size_t runs = 0;
    bool dash = false;
    while (end > start) {
        unsigned char c = end[-1];
        if (c == '-') {
            runs += !dash;
            dash = true;
        } else if (markdown_core_is_space_or_tab(c) || (pipe && (c == '|' || c == ':'))) {
            dash = false;
        } else {
            break;
        }
        end--;
    }
    return runs;
}

/* One necessary-condition predicate for every candidate entry. Every opener
 * needs a nonblank physical successor: a grid's next row or border, the first
 * body row of a headless table (which follows its separator directly), the
 * header of a full-boundary multiline table, or the separator under a
 * header. A blank or missing successor rules each out, and so the rest of
 * the predicate is asked only of a successor with a byte on it. A grid then
 * starts with '+', a headerless table has at least two dash runs, and a
 * single run may be a full boundary whose separator comes after its header.
 * Otherwise only a headed simple or pipe table remains, and the successor
 * must be its separator: respectively two dash runs or one, counted as the
 * suffix rule above allows. A true result grants no grammar or containment
 * decision; the ordinary recognizer still owns it. Read the indexed physical
 * extent so CR, CRLF and EOF agree with the source driver, without replaying
 * container continuation. Keep this predicate in its caller: all arguments
 * are already-loaded source geometry, not a separate per-candidate
 * out-of-line operation. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool table_grammar_admits(markdown_core_parser *parser,
                                                                                 const unsigned char *input, int length,
                                                                                 int first, size_t runs, int next_line,
                                                                                 const unsigned char *cursor,
                                                                                 bool pipe) {
    if (!cursor || markdown_core_parser_input_ends(parser, cursor)) {
        return false;
    }
    markdown_core_input_line *line = markdown_core_parser_source_line(parser, next_line);
    if (!line) {
        return false;
    }
    const unsigned char *end = parser->input.source + line->end;
    /* A line the text has not finished ends where the input does: a reading
     * of it to its end reads the end. */
    const bool unfinished = parser->input.source + line->start >= markdown_core_parser_complete_end(parser);
    const unsigned char *byte = cursor;
    while (byte < end && markdown_core_is_space_or_tab(*byte)) {
        byte++;
    }
    if (byte == end) {
        if (unfinished) {
            markdown_core_parser_see_end(parser);
        }
        return false;
    }
    if ((first < length && input[first] == '+') || runs >= 1) {
        return true;
    }
    if (unfinished) {
        markdown_core_parser_see_end(parser);
    }
    size_t required = pipe ? 1u : 2u;
    return table_trailing_dash_runs(byte, end, pipe) >= required;
}

/* Admission and recognition have separate lifetimes. A top-level opener
 * admits borrowed input before acquiring a workspace; a caption admits an
 * already captured line. Both enter this one recognizer with that proof, so
 * the successful opener never repeats its raw-source lookahead. */
static bool table_parse_admitted_candidate(table_source *source, size_t start, table_candidate *candidate, bool pipe) {
    assert(start < source->count && source->lines[start].indent < 4);
    bool boundary = table_full_boundary(source, start);
    if (table_parse_grid(source, start, candidate)) {
        return true;
    }
    if (source->undecided || source->parser->error) {
        return false;
    }
    if ((boundary && table_parse_multiline(source, start, candidate)) || table_parse_simple(source, start, candidate) ||
        (!boundary && table_parse_multiline(source, start, candidate))) {
        return true;
    }
    return pipe && table_parse_pipe_header(source, start, candidate);
}

static bool table_parse_candidate(table_source *source, size_t start, table_candidate *candidate, bool pipe) {
    if (!table_source_get(source, start) || source->lines[start].indent >= 4) {
        return false;
    }
    size_t runs = table_dash_count(source, start);
    table_source_line *line = &source->lines[start];
    return table_grammar_admits(source->parser, line->data, line->length, line->first, runs, line->line + 1,
                                line->after, pipe) &&
           table_parse_admitted_candidate(source, start, candidate, pipe);
}

static void table_append_range(table_source *source, markdown_core_node *node, size_t index, int left, int right,
                               bool escapes) {
    table_source_line *line = &source->lines[index];
    markdown_core_parser *parser = source->parser;
    if (right > line->columns) {
        right = line->columns;
    }
    if (left > right) {
        left = right;
    }
    assert(left >= 0);
    /* A scan position has one tab probe and at most two escape probes. A
     * run-ending position can be inspected again by the outer loop, so allow
     * two visits per position. Charge once, even if allocation stops copying. */
    source->workspace->work.scan += (escapes ? 6u : 2u) * (size_t)(right - left);
    /* Every column scanned has a byte, `left <= column < right <= columns`, so
     * the line's byte map is read directly; only the run's end, which can be
     * the end of the line, needs `table_byte`. An escape is a backslash whose
     * next column, if the line has one, is a pipe. */
    const int *bytes = table_line_bytes(line), columns = line->columns;
    const unsigned char *data = line->data;
    for (int column = left; column < right && !parser->error;) {
        int byte = bytes[column];
        if (data[byte] == '\t') {
            bufsize_t original = markdown_core_parser_source_offset(parser, line->line, byte + 1);
            markdown_core_parser_append_content_mark(parser, node, node->content.size, line->line, original, 1, 0);
            markdown_core_strbuf_putc(&node->content, ' ');
            column++;
        } else if (escapes && column + 1 < right && data[byte] == '\\' && data[bytes[column + 1]] == '|') {
            bufsize_t first = markdown_core_parser_source_offset(parser, line->line, byte + 1);
            bufsize_t end = markdown_core_parser_source_end(parser, line->line, byte + 2);
            markdown_core_parser_append_content_mark(parser, node, node->content.size, line->line, first,
                                                     (int)(end - first), (int)(end - first));
            markdown_core_strbuf_putc(&node->content, '|');
            column += 2;
        } else {
            int end = column + 1;
            while (end < right && data[bytes[end]] != '\t' &&
                   !(escapes && data[bytes[end]] == '\\' && end + 1 < columns && data[bytes[end + 1]] == '|')) {
                end++;
            }
            int length = table_byte(line, end) - byte;
            markdown_core_parser_append_source_marks(parser, node, line->line, byte + 1, length, node->content.size);
            markdown_core_strbuf_put(&node->content, line->data + byte, length);
            column = end;
        }
    }
    if (node->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

static void table_append_newline(table_source *source, markdown_core_node *node, size_t index) {
    table_source_line *line = &source->lines[index];
    markdown_core_parser_append_source_marks(source->parser, node, line->line, line->length + 1, 1, node->content.size);
    markdown_core_strbuf_putc(&node->content, '\n');
    if (node->content.oom) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

/* A cell whose content is blocks is read inside the blocks `enclosing` names:
 * its table and the blocks open around it. */
static void table_fill_cell(table_source *source, markdown_core_node *node, const table_source_cell *cell, bool blocks,
                            int padding_limit, uint32_t enclosing) {
    int padding = padding_limit;
    for (size_t i = cell->first; i <= cell->last; i++) {
        table_source_line *line = &source->lines[i];
        int end = cell->right < line->columns ? cell->right : line->columns;
        int first = table_skip_spaces(line, cell->left, end);
        if (first < end && first - cell->left < padding) {
            padding = first - cell->left;
        }
    }
    for (size_t i = cell->first; i <= cell->last && !source->parser->error; i++) {
        table_source_line *line = &source->lines[i];
        int first = cell->left, end = cell->right < line->columns ? cell->right : line->columns;
        end = table_trim_spaces(line, first, end);
        if (blocks) {
            first += padding < end - first ? padding : end - first;
        } else {
            first = table_skip_spaces(line, first, end);
        }
        table_append_range(source, node, i, first, end, !blocks);
        table_append_newline(source, node, i);
    }
    if (blocks && !source->parser->error) {
        markdown_core_parser_queue_block_input(source->parser, node, enclosing);
    }
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

/* A cell of a grid or multiline table lies in its columns on each of its
 * lines: one place of length 0 per line, from its left column to its right
 * one or the line's end. */
static void table_cell_places(table_source *source, markdown_core_node *node, const table_source_cell *cell) {
    markdown_core_parser *parser = source->parser;
    if (cell->last <= cell->first) {
        return;
    }
    uint32_t count = (uint32_t)(cell->last - cell->first + 1);
    markdown_core_places *places = markdown_core_node_pool_bytes(
        parser->pool, sizeof(*places) + (size_t)count * sizeof(markdown_core_source_place));
    if (!places) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    places->count = count;
    places->content = 0;
    for (uint32_t i = 0; i < count; i++) {
        const table_source_line *line = &source->lines[cell->first + i];
        int right = cell->right < line->columns ? cell->right : line->columns;
        int left = cell->left < right ? cell->left : right;
        bufsize_t start = left < right
                              ? markdown_core_parser_source_offset(parser, line->line, table_byte(line, left) + 1)
                              : markdown_core_parser_source_end(parser, line->line, table_byte(line, left));
        bufsize_t end = markdown_core_parser_source_end(parser, line->line, table_byte(line, right));
        places->items[i] = (markdown_core_source_place){(uint32_t)start, (uint32_t)end, 0};
    }
    if (node->source.places) {
        markdown_core_node_pool_bytes_free(parser->pool, node->source.places);
    }
    node->source.places = places;
}

/* A row the candidate cut, made whole and settled into `table`. */
static void table_build_row(table_source *source, markdown_core_node *table, const table_candidate *candidate,
                            const table_source_row *row, uint32_t enclosing) {
    markdown_core_parser *parser = source->parser;
    table_source_line *begin = &source->lines[row->first], *end = &source->lines[row->last];
    markdown_core_node *row_node = table_child(parser, MARKDOWN_CORE_NODE_TABLE_ROW, begin->line,
                                               table_margin_byte(begin, candidate->margin) + 1, end->line, end->length);
    if (!row_node) {
        return;
    }
    /* A pipe row's cells hold the bytes the iterator that recognized its
     * line (`table_parse_pipe_header`) delimits again, in order. */
    pipe_row_cursor pipe_cells = {0};
    node_cell geometry;
    bool pipe_more = candidate->pipe;
    if (pipe_more) {
        pipe_cells = pipe_row_begin((unsigned char *)begin->data + begin->first, begin->input_length - begin->first, 0);
    }
    for (size_t j = 0; j < row->count && !parser->error; j++) {
        table_source_cell *cell = &candidate->cells[row->cell + j];
        size_t last_line = cell->last < cell->first ? cell->first : cell->last;
        markdown_core_node *cell_node =
            table_child(parser, MARKDOWN_CORE_NODE_TABLE_CELL, source->lines[cell->first].line, cell->start_column,
                        source->lines[last_line].line, cell->end_column);
        if (!cell_node) {
            break;
        }
        cell_node->as.table_cell->rowspan = cell->rowspan;
        cell_node->as.table_cell->colspan = cell->colspan;
        if (!candidate->pipe) {
            table_cell_places(source, cell_node, cell);
            table_fill_cell(source, cell_node, cell, candidate->block_content, candidate->padding_limit, enclosing);
        } else if (pipe_more && (pipe_more = pipe_row_next(&pipe_cells, &geometry))) {
            set_cell_content(parser, cell_node, &geometry, NULL, begin->line,
                             markdown_core_parser_line_start(parser, begin->line),
                             (bufsize_t)(geometry.content.data - begin->data));
        }
        markdown_core_block_settle(parser, cell_node, row_node);
    }
    markdown_core_block_settle(parser, row_node, table);
}

static markdown_core_node *table_build(table_source *source, markdown_core_node *parent, table_candidate *candidate) {
    markdown_core_parser *parser = source->parser;
    table_source_line *first = &source->lines[candidate->first], *last = &source->lines[candidate->last];
    markdown_core_node *node =
        markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_TABLE, source->lines[0].first + 1);
    if (!node) {
        return NULL;
    }
    markdown_core_node_set_element(node, &MARKDOWN_CORE_ELEMENT_TABLE);
    node->opaque = markdown_core_alloc(1, sizeof(markdown_core_table));
    if (!node->opaque) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return node;
    }
    markdown_core_table *table = node->opaque;
    table->columns = markdown_core_alloc(candidate->column_count, sizeof(*table->columns));
    if (!table->columns) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return node;
    }
    memcpy(table->columns, candidate->columns, candidate->column_count * sizeof(*table->columns));
    table->column_count = candidate->column_count;
    if (candidate->simple) {
        table->runs = markdown_core_alloc(candidate->column_count, sizeof(*table->runs));
        if (!table->runs) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return node;
        }
        memcpy(table->runs, source->workspace->dashes + candidate->runs,
               candidate->column_count * sizeof(*table->runs));
        table->left = candidate->margin;
    }
    table->head_count = (uint32_t)candidate->head_count;
    table->foot_count = (uint32_t)candidate->foot_count;
    table->content_count = (uint32_t)(candidate->row_count - candidate->head_count - candidate->foot_count);
    /* The table and each of its rows begin at the margin on their first line. */
    node->place.start = (uint32_t)markdown_core_parser_source_offset(parser, first->line,
                                                                     table_margin_byte(first, candidate->margin) + 1);
    node->place.end = (uint32_t)markdown_core_parser_source_end(parser, last->line, last->length);
    const uint32_t enclosing = candidate->block_content ? markdown_core_parser_open_kinds(parser) : 0;
    for (size_t i = 0; i < candidate->row_count && !parser->error; i++) {
        table_build_row(source, node, candidate, &candidate->rows[i], enclosing);
    }
    /* A table read whole closes as it is made; a pipe table, and a simple
     * table its opening lines leave open, stay open for the rows after them. */
    if (!candidate->pipe && !candidate->simple && !parser->error) {
        markdown_core_block_close_current(parser);
    }
    return node;
}

static markdown_core_node *table_caption_build(table_source *source, const markdown_core_node *container, size_t last,
                                               int content) {
    table_source_line *first = &source->lines[0], *end = &source->lines[last];
    markdown_core_node *node = table_child(source->parser, MARKDOWN_CORE_NODE_TABLE_CAPTION, first->line,
                                           first->first + 1, end->line, end->length);
    if (!node) {
        return NULL;
    }
    int from = first->line;
    markdown_core_parser_line_places(source->parser, node, container, &from);
    for (size_t i = 0; i <= last && !source->parser->error; i++) {
        if (!table_source_columns(source, i)) {
            break;
        }
        table_source_line *line = &source->lines[i];
        int left = table_column(line, i ? line->first : content);
        table_append_range(source, node, i, left, line->columns, false);
        table_append_newline(source, node, i);
    }
    return node;
}

/* The producer and definition-term precedence query share this grammar. A
 * query owns one lookahead transaction, and never opens a node or claims input. */
/* A CAPTION ENDS AT A BLANK LINE, so the line after the blank is never a
 * caption line, and it is a candidate only for a leading caption
 * (`after_blank`): a trailing caption belongs to the table above, and what
 * follows the blank is the next construct, parsed when its own line comes.
 * The blank is tested before the candidate is parsed at that line, so the
 * table after a trailing caption is parsed once, by its own opener, and the
 * candidate after a leading caption's blank is parsed once, in the tail
 * below -- it used to be parsed in the loop, thrown away, and parsed again. */
static bool table_after_caption(table_source *source, size_t *caption_last, table_candidate *candidate,
                                bool after_blank) {
    size_t next = 1;
    for (; table_source_get(source, next); next++) {
        if (source->lines[next].blanks) {
            break;
        }
        if (table_parse_candidate(source, next, candidate, true)) {
            return true;
        }
        if (table_has_block_start(source, next, true) || source->parser->error) {
            break;
        }
        *caption_last = next;
    }
    return after_blank && table_source_get(source, next) && source->lines[next].blanks &&
           table_parse_candidate(source, next, candidate, true);
}

bool markdown_core_table_caption_probe(const markdown_core_element_instance *table,
                                       markdown_core_block_lookahead *lookahead, markdown_core_chunk *input, int first,
                                       int indent, bool decides) {
    markdown_core_parser *parser = lookahead->parser;
    table_workspace *workspace = table->state;
    table_source source = {.parser = parser,
                           .workspace = workspace,
                           .lookahead = *lookahead,
                           .lines = workspace->lines,
                           .aside = !markdown_core_parser_candidate_final(parser) && !decides,
                           .decides = decides};
    lookahead->active = false; /* Transfer the transaction; source_free ends it. */
    table_candidate *candidate = &workspace->candidate;
    size_t last = 0;
    bool matched = false;
    if (table_caption_start(input->data, input->len, first, indent) >= 0 &&
        table_source_push(&source, (table_source_line){.data = input->data,
                                                       .length = input->len,
                                                       .offset = parser->offset,
                                                       .first = first,
                                                       .first_column = parser->first_nonspace_column,
                                                       .indent = indent,
                                                       .line = source.lookahead.line - 1,
                                                       .after = source.lookahead.cursor})) {
        matched = table_after_caption(&source, &last, candidate, true);
    }
    table_candidate_reset(candidate);
    table_source_end(&source);
    return matched;
}

/* WHAT THE OPENER READ (table_opener_reads): its lines, and the caption the
 * first of them starts, with the last line of the caption and whether it
 * trails the table before it. */
typedef struct {
    table_source source;
    int caption;
    size_t caption_last;
    bool trailing;
} table_read;

/* THE TABLE A CAPTION CAN TRAIL in `parent`, which the tree tells at the
 * call: its last child, when that is a table without a caption. */
static markdown_core_node *table_preceding(const markdown_core_node *parent) {
    markdown_core_node *last = markdown_core_parser_last_child(parent);
    return last && last->kind == MARKDOWN_CORE_NODE_TABLE && last->opaque &&
                   !((markdown_core_table *)last->opaque)->caption
               ? last
               : NULL;
}

/* THE OPENER'S PLACE, which reads no byte of the input: whether a table or
 * a caption can start on the line at the parser's cursor in `parent` -- its
 * indentation, a blank line, and the containers no table opens in. */
static inline bool table_opener_fits(const markdown_core_parser *parser, const markdown_core_node *parent) {
    return parser->indent <= 3 && !parser->blank && parent->kind != MARKDOWN_CORE_NODE_TABLE &&
           parent->kind != MARKDOWN_CORE_NODE_TABLE_ROW && parent->kind != MARKDOWN_CORE_NODE_PARAGRAPH;
}

/* THE OPENER'S ADMISSION at a place that fits, which reads only the line,
 * `input`, and the first bytes of the line after it, and so can read the
 * end: whether the line's bytes can start a table or a caption at all.
 * Captions are a separate entry grammar; the body shares candidate
 * admission with the definition-precedence query and every other table
 * producer. The reading (table_opener_reads) is begun only on a line it
 * admits and the end admits (table_opener_ends_admit). */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool table_opener_admits(markdown_core_parser *parser,
                                                                                const unsigned char *input,
                                                                                int length) {
    bufsize_t trimmed = length;
    while (trimmed > 0 && (input[trimmed - 1] == '\n' || input[trimmed - 1] == '\r')) {
        trimmed--;
    }
    if (table_caption_start(input, trimmed, parser->first_nonspace, parser->indent) >= 0) {
        return true;
    }
    return table_grammar_admits(parser, input, trimmed, parser->first_nonspace,
                                table_dash_count_raw(input, parser->offset, trimmed), parser->line_number + 1,
                                parser->lookahead_cursor, false);
}

/* THE END'S ADMISSION of a line the opener admits: every opening grammar
 * needs a later physical line, and at EOF only a caption the table before
 * it takes, when the call found one it can trail (`trailing`), can be
 * claimed. The line's bytes refuse almost every line, so a call asks the
 * tree for the table a caption can trail only on a line they admit; both
 * are conjuncts of one decision, so the order changes nothing else. */
static inline bool table_opener_ends_admit(markdown_core_parser *parser, bool trailing) {
    return trailing || !markdown_core_parser_input_ends(parser, parser->lookahead_cursor);
}

/* THE OPENER'S READING of a line its admission admits, which writes nothing
 * in the tree: whether the line opens a table or a caption of the table
 * before it, when the call found one it can trail (`trailing`). A
 * candidate's decision, `decides`, records no facts either (THE OPENER'S
 * CANDIDATE). */
static bool table_opener_reads(table_workspace *workspace, markdown_core_parser *parser, markdown_core_node *parent,
                               unsigned char *input, int length, bool decides, bool trailing, table_read *found) {
    const bool waits = !markdown_core_parser_candidate_final(parser);
    *found = (table_read){.source = {.parser = parser,
                                     .workspace = workspace,
                                     .lines = workspace->lines,
                                     .aside = waits && !decides,
                                     .decides = decides},
                          .caption = -1};
    table_source *source = &found->source;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_TABLE, &source->lookahead)) {
        return false;
    }
    if (waits) {
        markdown_core_parser_lookahead_wait(&source->lookahead);
    }
    bool matched = false;
    if (table_source_push(source, table_cursor_line(parser, input, length))) {
        found->caption = table_caption_start(source->lines[0].data, source->lines[0].length, source->lines[0].first,
                                             source->lines[0].indent);
        found->trailing = found->caption >= 0 && trailing;
        if (found->caption >= 0) {
            matched = table_after_caption(source, &found->caption_last, &workspace->candidate, !found->trailing);
        } else {
            matched = table_parse_admitted_candidate(source, 0, &workspace->candidate, false);
        }
    }
    markdown_core_parser_lookahead_end(&source->lookahead);
    return !parser->error && (matched || found->trailing);
}

/* THE OPENER'S GRAMMARS THAT READ ON (THE OPENER'S CANDIDATE), as its
 * candidate's feed keeps them in the candidate's words. The opener tries a
 * grid on a line that opens one, a multiline table with a header on a full
 * boundary, a simple table, and a multiline table without one on two dash
 * runs or more, in that order, and the first that claims lines wins; so the
 * call's own line names the grammars that can claim and their order. Each
 * is ALIVE while a later line may change what it makes, CLAIMED once it
 * claims lines, DEAD once it cannot, and DONE once a grid's lines end, which
 * the opener's reading alone tells claimed or not. A simple table with a
 * header has read its separator, `separated`. A multiline table has read
 * its header's first line, `headed`, and its separator, `body`; of the line
 * it read last, whether that was the separator, `after`, the first body
 * line, `first_body`, a full boundary, `boundary`, and after blank lines,
 * `blanks`; and whether a body line after the first came after blank lines
 * before that line, `split`, which tells one row from more. A simple table
 * without a header compares a line with the runs of the call's line, `line`,
 * whose content starts at `offset` with its first non-space byte at `first`
 * in column `column`, indented `indent`, and holds `runs` runs, the last
 * ending at `last_end`. A caption's line names no grammar: its own reads
 * on to the first line or blank lines after it, and the decision reads it
 * whole; the call keeps whether it found a table before it the caption can
 * trail, `trailing`, as the tree told it there. */
enum { TABLE_DEAD, TABLE_ALIVE, TABLE_CLAIMED, TABLE_DONE };
enum { TABLE_GRID, TABLE_SIMPLE, TABLE_HEADERLESS, TABLE_MULTILINE, TABLE_HEADLESS };

typedef struct {
    int left, right;
    int line, offset, first, column, indent, runs, last_end;
    unsigned char grammars[2], states[2];
    bool separated, headed, body, after, first_body, boundary, blanks, split, trailing;
} table_feed;

typedef char table_feed_fits_its_words[sizeof(table_feed) <= sizeof(int[MARKDOWN_CORE_CANDIDATE_WORDS]) ? 1 : -1];

/* The grammars the line `source` holds first names, each ALIVE, as the feed
 * starts at the line after it, a call that found a table a caption can
 * trail when `trailing`. A caption's line names none (it reads its own
 * grammar). */
static table_feed table_feed_start(table_source *source, bool trailing) {
    table_feed feed = {0};
    table_source_line *line = &source->lines[0];
    const size_t runs = table_dash_count(source, 0);
    if (table_caption_start(line->data, line->length, line->first, line->indent) >= 0) {
        feed.trailing = trailing;
        return feed;
    }
    if (table_grid_opening(source, 0, &feed.left, &feed.right)) {
        feed.grammars[0] = TABLE_GRID;
        feed.grammars[1] = TABLE_SIMPLE;
    } else if (table_full_boundary(source, 0)) {
        feed.grammars[0] = TABLE_MULTILINE;
        feed.grammars[1] = TABLE_SIMPLE;
    } else if (runs > 1 && table_prepare_dashes(source, 0)) {
        feed.grammars[0] = TABLE_HEADERLESS;
        feed.grammars[1] = TABLE_HEADLESS;
        feed.body = feed.after = true;
        feed.line = line->line;
        feed.offset = line->offset;
        feed.first = line->first;
        feed.column = line->first_column;
        feed.indent = line->indent;
        feed.runs = (int)runs;
        feed.last_end = table_dash(source, line->dash_offset, runs - 1).end;
    } else {
        feed.grammars[0] = TABLE_SIMPLE;
        feed.states[0] = TABLE_ALIVE;
        return feed;
    }
    feed.states[0] = feed.states[1] = TABLE_ALIVE;
    return feed;
}

/* THE OPENER'S REQUEST: its call on the line `source` holds first asks to
 * be a candidate, with the grammars that line names; a caption that trails
 * a table is claimed were the text to end after its line. */
static void table_request(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *parent, table_source *source, bool trailing) {
    int words[MARKDOWN_CORE_CANDIDATE_WORDS] = {0};
    const table_feed feed = table_feed_start(source, trailing);
    if (!parser->error) {
        memcpy(words, &feed, sizeof(feed));
        markdown_core_parser_request_candidate(parser, self, parent, MARKDOWN_CORE_NODE_TABLE, words, feed.trailing);
    }
}

/* THE OPENER. A call in a commit more text may follow sets the end it reads
 * aside: one that reads it, or whose grid waits, decides nothing, and asks
 * to be a candidate (THE OPENER'S CANDIDATE); while it waits, it declines. */
static markdown_core_node *table_try_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                          markdown_core_node *parent, unsigned char *input, int length) {
    if (!table_opener_fits(parser, parent) ||
        (parser->stream && markdown_core_parser_candidate_live(parser, self, parent))) {
        return NULL;
    }
    const bool aside = !markdown_core_parser_candidate_final(parser);
    const markdown_core_end_aside end =
        aside ? markdown_core_parser_set_end_aside(parser) : (markdown_core_end_aside){0};
    table_workspace *workspace = self->state;
    const bool admitted = table_opener_admits(parser, input, length);
    markdown_core_node *const preceding = admitted ? table_preceding(parent) : NULL;
    if (!admitted || !table_opener_ends_admit(parser, preceding != NULL)) {
        if (aside && markdown_core_parser_put_end_back(parser, end) && !parser->error) {
            table_source source = {.parser = parser, .workspace = workspace, .lines = workspace->lines};
            if (table_source_push(&source, table_cursor_line(parser, input, length))) {
                table_request(self, parser, parent, &source, preceding != NULL);
            }
            table_source_end(&source);
        }
        return NULL;
    }
    table_candidate *candidate = &workspace->candidate;
    table_read found;
    const bool claims = table_opener_reads(workspace, parser, parent, input, length, false, preceding != NULL, &found);
    table_source *source = &found.source;
    markdown_core_node *result = NULL;
    if (aside && markdown_core_parser_put_end_back(parser, end)) {
        source->undecided = true;
    }
    if (source->undecided) {
        if (!parser->error) {
            table_request(self, parser, parent, source, preceding != NULL);
        }
        goto done;
    }
    if (!claims) {
        goto done;
    }
    markdown_core_parser_close_below(parser, parent);
    if (parser->error) {
        goto done;
    }
    const int caption = found.caption;
    const size_t caption_last = found.caption_last;
    if (found.trailing) {
        result = preceding;
        table_candidate_reset(candidate);
        markdown_core_table *table = result->opaque;
        table->caption = table_caption_build(source, parent, caption_last, caption);
        if (table->caption) {
            markdown_core_block_settle_field(parser, table->caption, result);
        }
        result->place.end = (uint32_t)markdown_core_parser_source_end(parser, source->lines[caption_last].line,
                                                                      source->lines[caption_last].length);
        parser->claimed_cursor = source->lines[caption_last].after;
        parser->claimed_line = source->lines[caption_last].line;
        parser->claimed_last_end = result->place.end;
    } else {
        result = table_build(source, parent, candidate);
        if (result && result->opaque && caption >= 0) {
            markdown_core_table *table = result->opaque;
            table->caption = table_caption_build(source, parent, caption_last, caption);
            if (table->caption) {
                markdown_core_block_settle_field(parser, table->caption, result);
            }
            result->place.start =
                (uint32_t)markdown_core_parser_source_offset(parser, source->lines[0].line, source->lines[0].first + 1);
        }
        if (result) {
            parser->claimed_cursor = source->lines[candidate->last].after;
            parser->claimed_line = source->lines[candidate->last].line;
            parser->claimed_last_end =
                markdown_core_parser_source_end(parser, parser->claimed_line, source->lines[candidate->last].length);
        }
    }
done:
    table_candidate_reset(candidate);
    table_source_end(source);
    return result;
}

/* A GRAMMAR'S STEP over the line `source` holds first, after `blanks`
 * blank lines, which the text has not finished when `unfinished`: false
 * when the line's bytes do not tell yet; otherwise its state moves on, and
 * `claims` says whether it would claim the lines through this one were the
 * text to end after it. A grid goes on while its walls lead and end the
 * line and could end after a border; any other line ends its lines, and the
 * bytes up to its wall tell that. Every other grammar reads a line whole,
 * and the reading that decides it reads the line after a blank one, so a
 * line the text has not finished never tells it. */
static bool table_grid_step(table_feed *feed, unsigned char *state, table_source *source, int blanks, bool unfinished,
                            bool *claims) {
    table_source_line *line = &source->lines[0];
    const int wall = blanks || !table_source_columns(source, 0) ? 0 : table_character(line, feed->left);
    if ((wall != '+' && wall != '|') || line->indent != feed->left) {
        *state = TABLE_DONE;
        return true;
    }
    if (unfinished) {
        return false;
    }
    const int edge = table_character(line, feed->right);
    if (table_trim_spaces(line, feed->left + 1, line->columns) - 1 != feed->right || (edge != '+' && edge != '|')) {
        *state = TABLE_DONE;
        return true;
    }
    *claims = table_horizontal(line, feed->left, feed->right) != 0;
    return true;
}

/* A simple table with a header claims its separator's line and the row
 * after it; a blank line before either, or a separator of fewer than two
 * runs, leaves it none. */
static bool table_simple_step(table_feed *feed, unsigned char *state, table_source *source, int blanks,
                              bool unfinished) {
    if (unfinished) {
        return false;
    }
    if (blanks) {
        *state = TABLE_DEAD;
    } else if (feed->separated) {
        *state = TABLE_CLAIMED;
    } else if (table_dash_count(source, 0) < 2) {
        *state = TABLE_DEAD;
    } else {
        feed->separated = true;
    }
    return true;
}

/* A simple table without a header claims its lines through its footer,
 * the line of the call's runs, and a blank line before it leaves it none.
 * A line is compared run by run with the call's line only when it holds as
 * many runs, the last ending where the call's does. */
static bool table_headerless_step(const table_feed *feed, unsigned char *state, table_source *source, int blanks,
                                  bool unfinished) {
    if (unfinished) {
        return false;
    }
    if (blanks) {
        *state = TABLE_DEAD;
        return true;
    }
    if (table_dash_count(source, 0) != (size_t)feed->runs || !table_prepare_dashes(source, 0) ||
        table_dash(source, source->lines[0].dash_offset, (size_t)feed->runs - 1).end != feed->last_end) {
        return true;
    }
    bufsize_t length;
    const unsigned char *data = markdown_core_parser_line_content(source->parser, feed->line, &length);
    if (data &&
        table_source_push(source, (table_source_line){.data = data,
                                                      .length = (int)length,
                                                      .offset = feed->offset,
                                                      .first = feed->first,
                                                      .first_column = feed->column,
                                                      .indent = feed->indent,
                                                      .line = feed->line}) &&
        table_same_dashes(source, 1, 0)) {
        *state = TABLE_CLAIMED;
    }
    return true;
}

/* A multiline table reads its header's lines up to a separator of two
 * runs or more, which may not be the line right after the call's, and a
 * blank line in its header leaves it none; one without a header starts its
 * body on the line after the call's, which may not follow a blank line.
 * Its body closes at the first full boundary a blank line follows, or the
 * end, and it claims its lines when that is not the first body line and its
 * body holds more than one row or the boundary came after blank lines. */
static bool table_multiline_step(table_feed *feed, unsigned char *state, table_source *source, int blanks,
                                 bool unfinished, bool headless, bool *claims) {
    if (unfinished) {
        return false;
    }
    if (!feed->body) {
        if (blanks) {
            *state = TABLE_DEAD;
        } else if (table_dash_count(source, 0) < 2) {
            feed->headed = true;
        } else if (!feed->headed) {
            *state = TABLE_DEAD;
        } else {
            feed->body = feed->after = true;
        }
        return true;
    }
    if (feed->after && headless && blanks) {
        *state = TABLE_DEAD;
        return true;
    }
    if (feed->boundary && blanks) {
        *state = !feed->first_body && (feed->split || feed->blanks) ? TABLE_CLAIMED : TABLE_DEAD;
        return true;
    }
    feed->split = feed->split || (!feed->after && !feed->first_body && feed->blanks);
    feed->first_body = feed->after;
    feed->after = false;
    feed->boundary = table_full_boundary(source, 0);
    feed->blanks = blanks != 0;
    *claims = feed->boundary && !feed->first_body && (feed->split || feed->blanks);
    return true;
}

/* THE FEED of the opener's candidate (blocks.c, THE CANDIDATES): each
 * grammar still ALIVE steps over the line (THE OPENER'S GRAMMARS THAT READ
 * ON), and the line ends the construct once no grammar before the first that
 * claimed is ALIVE, or none is, as the opener's reading then decides from
 * the lines through it alone. Until then the line goes on, which a line the
 * text has not finished never does, and the opener could claim were the text
 * to end after it when a grammar would or a grid's lines ended there. Blank
 * lines the text read so far ends after leave each grammar as the line
 * before them did; the line after them tells. */
static markdown_core_candidate_step feed_candidate(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, int *words,
                                                   const markdown_core_chunk *input, int first, int indent, int blanks,
                                                   bool unfinished, bool *ends) {
    table_feed feed;
    memcpy(&feed, words, sizeof(feed));
    if (!input) {
        /* Blank lines tell a grammar nothing until the line after them, and
         * end a caption's lines. */
        return feed.states[0] == TABLE_ALIVE || feed.states[1] == TABLE_ALIVE ? MARKDOWN_CORE_CANDIDATE_CONTINUES
                                                                              : MARKDOWN_CORE_CANDIDATE_ENDS;
    }
    table_workspace *workspace = self->state;
    table_source source = {.parser = parser, .workspace = workspace, .lines = workspace->lines};
    markdown_core_candidate_step step = MARKDOWN_CORE_CANDIDATE_ENDS;
    bool claims = false;
    if (!table_source_push(&source, (table_source_line){.data = input->data,
                                                        .length = input->len,
                                                        .offset = parser->offset,
                                                        .first = first,
                                                        .first_column = parser->first_nonspace_column,
                                                        .indent = indent})) {
        goto done;
    }
    for (size_t i = 0; i < 2; i++) {
        unsigned char *state = &feed.states[i];
        bool told = true, claim = false;
        if (*state == TABLE_ALIVE) {
            switch (feed.grammars[i]) {
            case TABLE_GRID:
                told = table_grid_step(&feed, state, &source, blanks, unfinished, &claim);
                break;
            case TABLE_SIMPLE:
                told = table_simple_step(&feed, state, &source, blanks, unfinished);
                break;
            case TABLE_HEADERLESS:
                told = table_headerless_step(&feed, state, &source, blanks, unfinished);
                break;
            default:
                told = table_multiline_step(&feed, state, &source, blanks, unfinished,
                                            feed.grammars[i] == TABLE_HEADLESS, &claim);
                break;
            }
        }
        if (!told || parser->error) {
            step = MARKDOWN_CORE_CANDIDATE_WAITS;
            goto done;
        }
        claims = claims || claim || *state == TABLE_CLAIMED || *state == TABLE_DONE;
    }
    for (size_t i = 0; i < 2 && feed.states[i] != TABLE_CLAIMED; i++) {
        if (feed.states[i] == TABLE_ALIVE) {
            step = unfinished ? MARKDOWN_CORE_CANDIDATE_WAITS : MARKDOWN_CORE_CANDIDATE_CONTINUES;
            break;
        }
    }
    if (step == MARKDOWN_CORE_CANDIDATE_CONTINUES) {
        memcpy(words, &feed, sizeof(feed));
        *ends = claims;
    }
done:
    table_source_end(&source);
    return step;
}

/* The opener's call decided as it would read now (blocks.c, THE
 * CANDIDATES), with the table before it as the call found it. */
static markdown_core_candidate_decision decide_candidate(const markdown_core_element_instance *self,
                                                         markdown_core_parser *parser, const int *words) {
    table_feed feed;
    memcpy(&feed, words, sizeof(feed));
    markdown_core_chunk line;
    markdown_core_node *container;
    if (!markdown_core_parser_candidate_start(parser, &line, &container)) {
        return MARKDOWN_CORE_CANDIDATE_REJECT;
    }
    table_workspace *workspace = self->state;
    bool claims = false, undecided = false;
    if (table_opener_fits(parser, container) && table_opener_admits(parser, line.data, (int)line.len) &&
        table_opener_ends_admit(parser, feed.trailing)) {
        table_read found;
        claims =
            table_opener_reads(workspace, parser, container, line.data, (int)line.len, true, feed.trailing, &found);
        undecided = found.source.undecided;
        table_candidate_reset(&workspace->candidate);
        table_source_end(&found.source);
    }
    markdown_core_parser_candidate_end(parser);
    return undecided ? MARKDOWN_CORE_CANDIDATE_WAIT
           : claims  ? MARKDOWN_CORE_CANDIDATE_ACCEPT
                     : MARKDOWN_CORE_CANDIDATE_REJECT;
}

/* Simple rows own inline text until a blank line or a footer. Pandoc 3.11
 * retains heading, quote, fence and caption markers here; the header and
 * caption paragraph-interruption rules do not apply to an open body. A
 * footer has the table's own dash runs, every interval of them. */
static int table_simple_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                const markdown_core_table *table, unsigned char *input, int len) {
    if (parser->blank) {
        return 0;
    }
    table_workspace *workspace = self->state;
    table_source source = {.parser = parser, .workspace = workspace, .lines = workspace->lines};
    bool footer =
        table_source_push(&source, table_cursor_line(parser, input, len)) &&
        table_dash_count(&source, 0) == table->column_count && table_prepare_dashes(&source, 0) &&
        table_same_runs(workspace, workspace->dashes + source.lines[0].dash_offset, table->runs, table->column_count);
    table_source_end(&source);
    return parser->error ? 0 : footer ? MARKDOWN_CORE_BLOCK_CLOSED : MARKDOWN_CORE_BLOCK_TAKEN;
}

/* A ROW OF A SIMPLE TABLE, read on its own: its line is cut at the table's
 * dash runs, its first column taken from the margin, or from its text where
 * that lies left of the margin, and the row settles into the open table. */
static void table_take_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *container, markdown_core_chunk *input) {
    markdown_core_table *table = container->opaque;
    table_workspace *workspace = self->state;
    table_source source = {.parser = parser, .workspace = workspace, .lines = workspace->lines};
    table_candidate *candidate = &workspace->candidate;
    if (table_source_push(&source, table_cursor_line(parser, input->data, input->len))) {
        candidate->column_count = table->column_count;
        candidate->margin = table->left;
        if (table_rectangular_row(&source, candidate, 0, 0, table->runs)) {
            table_build_row(&source, container, candidate, &candidate->rows[0], 0);
            table->content_count++;
        }
    }
    table_candidate_reset(candidate);
    table_source_end(&source);
}

/* A block-only element: no byte ends a text run for it, no byte is offered to an
 * inline hook it does not have, and no byte is transparent to flanking. */
static markdown_core_node *try_interrupting_block(const markdown_core_element_instance *self,
                                                  markdown_core_parser *parser, markdown_core_node *node,
                                                  markdown_core_chunk *input, bool lazy) {
    if (parser->indent >= 4 || lazy || node->kind == MARKDOWN_CORE_NODE_PARAGRAPH ||
        input->data[parser->first_nonspace] != '-') {
        return NULL;
    }
    return table_try_open(self, parser, node, input->data, input->len);
}

static void dispose_parser(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)parser;
    table_workspace *workspace = self->state;
    markdown_core_free(workspace->lines);
    markdown_core_free(workspace->bytes);
    markdown_core_free(workspace->dashes);
    markdown_core_free(workspace->candidate.columns);
    markdown_core_free(workspace->candidate.rows);
    markdown_core_free(workspace->candidate.cells);
    markdown_core_free(workspace->separator_keys);
    markdown_core_free(workspace->separator_scratch);
    markdown_core_free(workspace->separator_groups);
    markdown_core_free(workspace->grid_parents);
    markdown_core_free(workspace->grid_sizes);
    markdown_core_free(workspace->grid_positions);
    markdown_core_free(workspace->boundaries);
    markdown_core_free(workspace->row_indices);
    markdown_core_free(workspace->region_parents);
    markdown_core_free(workspace->region_sizes);
    markdown_core_free(workspace->region_next);
    markdown_core_free(workspace->region_previous);
    markdown_core_free(workspace->regions);
    markdown_core_free(workspace->region_scratch);
    markdown_core_free(workspace->closed);
    memset(workspace, 0, sizeof(*workspace));
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_TABLE = {
    .dispose_parser = dispose_parser,
    .state_size = sizeof(table_workspace),

    .try_interrupting_block = try_interrupting_block,
    /* A delimiter row leading a dash-led table. */
    .interrupt_block_gate = {.bytes = "-"},

    .name = "table",
    .last_block_matches = matches,
    .take_line = table_take_line,
    .maximum_block_indent = 3,
    .try_opening_block = try_opening_table_block,
    .feed_candidate = feed_candidate,
    .decide_candidate = decide_candidate,
    .containment_kinds = containment_kinds,
    .contains_inlines_func = contains_inlines,
    .opaque_alloc_func = opaque_alloc,
    .opaque_free_func = opaque_free,
    .opaque_view_func = opaque_view,
    .opaque_adopt_func = opaque_adopt,
    .visit_owned_subtrees_func = visit_owned_subtrees,
};
