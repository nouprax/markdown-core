#include "alloc.h"
#include "table_scanners.h"
#include <markdown-core-element-api.h>
#include "element.h"
#include "block_internal.h"
#include <inlines.h>
#include <parser.h>
#include <map.h>
#include <string.h>
#include <limits.h>
#include "utf8.h"

#include "strikethrough.h"
#include "table.h"
#include "paragraph.h"
#include "markdown-core-elements.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { TABLE_PARAGRAPH };
static const markdown_core_element *const TABLE_PEERS[] = {[TABLE_PARAGRAPH] = &MARKDOWN_CORE_ELEMENT_PARAGRAPH, NULL};

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

/* A ROW'S ENTRY (docs/plans/2026-09-29-incremental-parsing.md, E6): the
 * state of its grid table's fold after the row's top border, which a later
 * parse compares before it takes the row. `values[0, shape)` are the fold's
 * SHAPE, each geometry column's class among the columns the '+' joins of the
 * lines so far put together, named by its least column; the rest is how it
 * reads rows, from the border's edges to the cells that run on past it
 * (`table_lattice_point`). A row's entry is made with the row, from the
 * state the fold kept in its workspace; rows with equal states share one,
 * and so does a table's tail. */
typedef struct table_entry {
    size_t refs;
    size_t shape, count;
    int values[];
} table_entry;

static table_entry *table_entry_retain(table_entry *entry) {
    if (entry) {
        entry->refs++;
    }
    return entry;
}

static void table_entry_release(table_entry *entry) {
    if (entry && !--entry->refs) {
        markdown_core_free(entry);
    }
}

/* A STATE OF THE FOLD that a parse kept (`table_lattice_point`): its
 * values, from `at` in the workspace's `point_values`, the first `shape` of
 * them its shape, and the entry that holds the same values, once there is
 * one: the old row's it stands at, or the one a row made from it took. */
typedef struct {
    size_t at, shape, count;
    table_entry *entry;
} table_point;

/* Whether an entry and a state read rows alike, and fold the same shape. */
static bool table_entry_reads(const table_entry *entry, const table_point *point, const int *values) {
    return entry->count - entry->shape == point->count - point->shape &&
           !memcmp(entry->values + entry->shape, values + point->shape, (entry->count - entry->shape) * sizeof(int));
}

static bool table_entry_shapes(const table_entry *entry, const table_point *point, const int *values) {
    return entry->shape == point->shape && !memcmp(entry->values, values, entry->shape * sizeof(int));
}

/* THE RECORD OF A TABLE'S FOLD (E5, E6): the table's form and margin; the
 * geometry columns of a grid table's walls, or the starts and ends of a
 * simple or multiline table's dash runs; a grid table's fold state after its
 * closing border, `tail_span` bytes past the end of its last row; and its
 * full '=' borders, each named by the rows above it, with the alignment it
 * reads. A later parse reads the table again against it: it steps over the
 * runs of rows it takes, and reads what the lines it stepped over decided
 * from here. */
enum { TABLE_FORM_GRID = 1, TABLE_FORM_SIMPLE, TABLE_FORM_MULTILINE };

struct markdown_core_table_fold {
    int form, margin;
    int *positions;
    size_t position_count;
    table_entry *tail;
    size_t tail_span;
    size_t equal_count, equal_rows[3];
    markdown_core_flow *equal_flows;
};

static void table_fold_free(struct markdown_core_table_fold *fold) {
    if (fold) {
        markdown_core_free(fold->positions);
        markdown_core_free(fold->equal_flows);
        table_entry_release(fold->tail);
        markdown_core_free(fold);
    }
}

static void free_node_table(markdown_core_table *table) {
    if (!table) {
        return;
    }
    markdown_core_free(table->columns);
    table_fold_free(table->fold);
    markdown_core_free(table);
}

static void init_cell(markdown_core_node *node) {
    node->as.table_cell->rowspan = 1;
    node->as.table_cell->colspan = 1;
}

/* A ROW OR CELL IS COMPLETE WHEN THE TABLE MAKES IT, and so is a caption: it
 * is never an open block. It starts and ends at `start` until its maker
 * places it, and joins `parent`'s children when there is one. */
static markdown_core_node *table_part(markdown_core_parser *parser, markdown_core_node *parent,
                                      markdown_core_node_type kind, bufsize_t start) {
    markdown_core_node *node = markdown_core_parser_make_node(parser, kind);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    if (kind == MARKDOWN_CORE_NODE_TABLE_ROW || kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        markdown_core_node_set_element(node, &MARKDOWN_CORE_ELEMENT_TABLE);
    }
    node->where.place = (markdown_core_place){(uint32_t)start, (uint32_t)start};
    if (parent) {
        /* Only TABLE -> ROW and ROW -> CELL reach this private constructor;
         * both owners carry this element's immutable kind domain. */
        assert(parent->element == &MARKDOWN_CORE_ELEMENT_TABLE);
        assert((parent->kind == MARKDOWN_CORE_NODE_TABLE && kind == MARKDOWN_CORE_NODE_TABLE_ROW) ||
               (parent->kind == MARKDOWN_CORE_NODE_TABLE_ROW && kind == MARKDOWN_CORE_NODE_TABLE_CELL));
        if (!markdown_core_parser_append(parser, parent, node)) {
            return NULL;
        }
    }
    return node;
}

static markdown_core_node *new_cell(markdown_core_parser *parser, markdown_core_node *row, int column) {
    markdown_core_node *cell = table_part(parser, row, MARKDOWN_CORE_NODE_TABLE_CELL,
                                          markdown_core_parser_source_offset(parser, parser->line_number, column));
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
static void set_cell_content(markdown_core_parser *parser, markdown_core_node *row, markdown_core_node *node,
                             const node_cell *cell, markdown_core_node *source, int line, bufsize_t line_start,
                             bufsize_t offset) {
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
    /* A pipe cell is complete once it holds its bytes. */
    markdown_core_parser_complete(parser, node, row);
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
        node->where.place.start = (uint32_t)source;
    }
    if (markdown_core_parser_content_end_place(parser, &owner->content_map, end_offset, &line, &source)) {
        node->where.place.end = (uint32_t)source;
    }
}

static void try_inserting_table_header_paragraph(const markdown_core_element_instance *paragraph_element,
                                                 markdown_core_parser *parser, markdown_core_node *parent_container,
                                                 unsigned char *parent_string, int paragraph_offset) {
    markdown_core_node *paragraph;
    bufsize_t first = 0;
    bufsize_t content_end = paragraph_offset;
    bufsize_t scope_end = content_end;
    int line;
    bufsize_t source;

    // Four allocations, and every one of them used to be trusted. The first was
    // a crash: an unchecked node reached markdown_core_node_set_string_content,
    // which dereferences it -- SIGSEGV on `lead text` above a two-column table
    // with the allocation refused. The other three lose the lead paragraph
    // WITHOUT setting parser->error, so the document comes back short and the
    // failure bit says everything was fine.
    paragraph = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!paragraph) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }

    /* THE LEAD KEEPS ITS AUTHORED SPELLING. This used to run the lead through
     * `unescape_pipes`, which is a CELL transformation: a pipe a cell escaped
     * is not a pipe the cell contains. The lead is not a cell -- it is the
     * paragraph the table was split out of -- so `pre \\| lead` above a table
     * lost one of its two backslashes here and the inline phase then read the
     * survivor as the escape, giving `pre | lead` where the author wrote an
     * escaped backslash followed by a pipe. */
    while (first < content_end && markdown_core_is_whitespace(parent_string[first])) {
        first++;
    }
    /* Paragraph finalization needs the complete terminated slice, including
     * the last reference definition's newline. Only the authored scope omits
     * line endings; inline parsing trims its own input after finalization. */
    while (scope_end > first && (parent_string[scope_end - 1] == '\n' || parent_string[scope_end - 1] == '\r')) {
        scope_end--;
    }
    markdown_core_strbuf_put(&paragraph->content, parent_string + first, content_end - first);
    if (paragraph->content.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, paragraph);
        return;
    }

    /* The lead is synthesized from a content offset and so has no position of
     * its own; before requirement 10 it kept the 0:0..0:0 sentinel, and every
     * inline in it inherited line zero. The map answers both ends. */
    if (markdown_core_parser_content_place(parser, &parent_container->content_map, first, &line, &source)) {
        paragraph->where.place = (markdown_core_place){(uint32_t)source, (uint32_t)source};
    }
    if (scope_end > first &&
        markdown_core_parser_content_end_place(parser, &parent_container->content_map, scope_end - 1, &line, &source)) {
        paragraph->where.place.end = (uint32_t)source;
    }
    /* The lead's content is a SLICE of the paragraph's, and it can be several
     * lines long, so it takes the marks for those lines rather than one mark
     * for the first of them. */
    markdown_core_parser_adopt_content_marks(parser, &parent_container->content_map, &paragraph->content_map, first,
                                             content_end - first);

    /* The lead goes just before the table, the open parent's last child. */
    markdown_core_node *parent = markdown_core_parser_open_parent(parser, parent_container);
    if (!markdown_core_node_attach_validated(parser->pool, parent, markdown_core_node_children_count(parent) - 1,
                                             paragraph)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, paragraph);
        return;
    }

    /* A table split completes this paragraph just as a later block start
     * would: reference definitions and anchor attachment share finalization.
     * A lead of only definitions is no paragraph, and the table after it
     * stands in its place. */
    markdown_core_paragraph_finalize(paragraph_element, parser, parent, paragraph);
    if (paragraph->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY) {
        markdown_core_node *lead =
            markdown_core_node_take_child(parser->pool, parent, markdown_core_node_children_count(parent) - 2);
        if (!lead) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        markdown_core_parser_release_node(parser, lead);
        return;
    }
    markdown_core_parser_close_lead(parser, paragraph);
}

/* Return NULL when the syntax does not match or the parent rejects the table
 * kind, so later elements can try the same line. Once the paragraph becomes
 * a table, return that container even if a later allocation fails; parser->error
 * then aborts the parse and destruction releases the partially built table. */
static markdown_core_node *try_opening_table_header(const markdown_core_element_instance *self,
                                                    markdown_core_parser *parser, markdown_core_node *parent_container,
                                                    unsigned char *input, int len) {
    markdown_core_node *table_header;
    pipe_row header_row, delimiter_row;
    const char *parent_string;
    uint16_t i;
    int header_line;
    bufsize_t header_start;

    if (parent_container->flags & MARKDOWN_CORE_NODE__TABLE_VISITED) {
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
    parent_string = markdown_core_node_get_string_content(parent_container);
    if (!recognize_pipe_row((unsigned char *)parent_string, (int)strlen(parent_string), &header_row) ||
        header_row.n_columns != delimiter_row.n_columns) {
        parent_container->flags |= MARKDOWN_CORE_NODE__TABLE_VISITED;
        return NULL;
    }

    /* The table, and a split's new paragraph sibling, must be kinds the parent
     * holds. Decide before converting or allocating, so refusal leaves the
     * complete original paragraph available to grammar. */
    markdown_core_node *table_parent = markdown_core_parser_open_parent(parser, parent_container);
    if (!markdown_core_node_can_contain_type(table_parent, MARKDOWN_CORE_NODE_TABLE) ||
        (header_row.paragraph_offset &&
         !markdown_core_node_can_contain_type(table_parent, MARKDOWN_CORE_NODE_PARAGRAPH))) {
        return NULL;
    }

    if (header_row.paragraph_offset) {
        try_inserting_table_header_paragraph(self->peers[TABLE_PARAGRAPH], parser, parent_container,
                                             (unsigned char *)parent_string, header_row.paragraph_offset);
        /* The table starts where its HEADER ROW was written, not where the
         * paragraph it was split out of did. Taken before its kind changes,
         * which places it, and before the row and cells below read
         * start_column, because they are placed against it. */
        if (markdown_core_parser_content_place(parser, &parent_container->content_map, header_row.paragraph_offset,
                                               &header_line, &header_start)) {
            parent_container->where.place.start = (uint32_t)header_start;
        }
    }

    if (parser->error || !markdown_core_parser_set_node_kind(parser, parent_container, MARKDOWN_CORE_NODE_TABLE)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }

    /* Table data belongs to the element. Its cleanup accepts partial
     * initialization when an allocation fails after the kind change. */
    markdown_core_node_set_element(parent_container, self->element);
    parent_container->opaque = markdown_core_alloc(1, sizeof(markdown_core_table));
    if (!parent_container->opaque) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);

        return parent_container;
    }
    markdown_core_table *table = parent_container->opaque;
    table->column_count = header_row.n_columns;
    table->columns = markdown_core_alloc(table->column_count, sizeof(*table->columns));
    if (!table->columns) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);

        return parent_container;
    }
    pipe_row_cursor markers = pipe_row_cells(&delimiter_row);
    node_cell geometry;
    for (i = 0; pipe_row_next(&markers, &geometry); ++i) {
        const markdown_core_chunk *cell = &geometry.content;
        bool left = cell->data[0] == ':', right = cell->data[cell->len - 1] == ':';
        table->columns[i].flow = left ? (right ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_LEFT)
                                      : (right ? MARKDOWN_CORE_FLOW_RIGHT : MARKDOWN_CORE_FLOW_NONE);
    }

    table_header = table_part(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW,
                              markdown_core_parser_source_offset(parser, parser->line_number, 1));
    if (!table_header) {
        return parent_container;
    }
    /* The header row and its cells are RECOVERED from the paragraph's content
     * buffer, and every offset below is an offset into that buffer. Adding one
     * to a column is only right while the buffer holds a single line starting
     * where the block does; with a lead split off, `| a | b |` on line three
     * was reported at 1:10, a column that is not on line one. The map turns
     * each offset back into the place it was written. */
    S_place_content_span(parser, parent_container, table_header, header_row.paragraph_offset,
                         (bufsize_t)strlen(parent_string) - 2);

    table->head_count = 1;

    pipe_row_cursor header = pipe_row_cells(&header_row);
    while (pipe_row_next(&header, &geometry)) {
        const node_cell *cell = &geometry;
        markdown_core_node *header_cell = new_cell(parser, table_header, 1);
        if (!header_cell) {
            break;
        }
        S_place_content_span(parser, parent_container, header_cell, cell->start_offset, cell->end_offset);
        set_cell_content(parser, table_header, header_cell, cell, parent_container, parser->line_number,
                         parser->line_start, (bufsize_t)(cell->content.data - (unsigned char *)parent_string));
    }
    markdown_core_parser_complete(parser, table_header, parent_container);

    markdown_core_parser_advance_offset(
        parser, (char *)input, (int)strlen((char *)input) - 1 - markdown_core_parser_get_offset(parser), false);

    return parent_container;
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
            markdown_core_node *node =
                new_cell(parser, table_row_block, parser->first_nonspace + 1 + cell->start_offset);
            if (!node) {
                break;
            }
            node->where.place.end = (uint32_t)markdown_core_parser_source_end(
                parser, parser->line_number, parser->first_nonspace + 1 + cell->end_offset);
            set_cell_content(parser, table_row_block, node, cell, NULL, parser->line_number, parser->line_start,
                             (bufsize_t)(cell->content.data - input));
        }

        table->content_count++;
        table->autocompleted_cells += (size_t)(table_columns - i);
        table_row_block->tally = (uint32_t)(table_columns - i);

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
            markdown_core_node *node = new_cell(parser, table_row_block, (int)completed_at);
            if (!node) {
                break;
            }
            node->where.place.end =
                (uint32_t)markdown_core_parser_source_end(parser, parser->line_number, (int)completed_at);
            markdown_core_parser_complete(parser, node, table_row_block);
        }
    }

    markdown_core_parser_advance_offset(parser, (char *)input, len - 1 - markdown_core_parser_get_offset(parser),
                                        false);

    return table_row_block;
}

typedef struct markdown_core_table_workspace table_workspace;
/* Recognize a complete source candidate before claiming any of its lines. */
static markdown_core_node *table_try_open(table_workspace *workspace, markdown_core_parser *parser,
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
        return table_try_open(self->state, parser, parent_container, input, len);
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

static int matches(const markdown_core_element_instance *self, markdown_core_parser *parser, unsigned char *input,
                   int len, markdown_core_node *parent_container) {
    int res = 0;

    if (parent_container->kind == MARKDOWN_CORE_NODE_TABLE) {
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
    /* Block inputs have consumed their source before the inline phase. Their
     * children, rather than the cell wrapper, own the remaining inline text. */
    if (node->kind == MARKDOWN_CORE_NODE_TABLE_CAPTION) {
        return true;
    }
    return node->kind == MARKDOWN_CORE_NODE_TABLE_CAPTION ||
           (node->kind == MARKDOWN_CORE_NODE_TABLE_CELL && node->content.size > 0);
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

/* A table owns its value; a row of a grid table, its entry. */
static void opaque_free(const markdown_core_element *self, markdown_core_node *node) {
    if (node->kind == MARKDOWN_CORE_NODE_TABLE) {
        free_node_table(node->opaque);
    } else if (node->kind == MARKDOWN_CORE_NODE_TABLE_ROW) {
        table_entry_release(node->opaque);
    }
}

/* An old row equal to a row a parse made takes its entry (5.9). */
static void take_record(markdown_core_node *old, const markdown_core_node *node) {
    if (node->kind == MARKDOWN_CORE_NODE_TABLE_ROW && old->opaque != node->opaque) {
        table_entry_release(old->opaque);
        old->opaque = table_entry_retain(node->opaque);
    }
}

static int visit_owned_subtrees(const markdown_core_element *self, markdown_core_node *node,
                                markdown_core_owned_subtree_visitor visitor, void *context) {
    markdown_core_table *table = node->kind == MARKDOWN_CORE_NODE_TABLE ? node->opaque : NULL;
    return !table || !table->caption || visitor(&table->caption, context);
}

/* Source candidates own geometry only. Public nodes are allocated after a
 * complete candidate has passed validation; failed candidates consume nothing. */
typedef struct {
    int start, end;
} table_interval;

struct markdown_core_table_workspace;

typedef struct markdown_core_table_source_line {
    const unsigned char *data;
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

typedef struct {
    markdown_core_parser *parser;
    struct markdown_core_table_workspace *workspace;
    markdown_core_block_lookahead lookahead;
    table_source_line *lines;
    size_t count;
    bool ended;
    /* The old grid table a fold reads rows against, where it starts, and
     * whether the fold stepped over lines the source then never holds:
     * failing then, the parse reads the table again without it (`redo`). */
    const markdown_core_node *old;
    int64_t old_start;
    bool redo;
} table_source;

typedef struct {
    size_t first, last;
    int left, right;
    size_t row;
    int64_t rowspan, colspan;
    int start_column, end_column;
} table_source_cell;
/* A row a grammar read. A grid table's rows and the runs of old rows its fold
 * took (`table_source_part`) are ordered by `key`, the top border of their
 * first row; a fresh grid row carries the fold's state it begins at
 * (`point`), its entry once it is made, and how far the input was read
 * when the fold decided it. */
typedef struct {
    size_t first, last, cell, count, key;
    size_t point;
    table_entry *entry;
    size_t reads;
} table_source_row;
typedef struct {
    size_t key, first, count, start, end;
} table_source_part;
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
     * body rows `try_opening_table_row` adds. */
    bool block_content, pipe;
    int padding_limit;
    /* The table's left edge, in geometry columns (`table_margin`): the
     * table, each row and the first column begin here, not at the
     * container's content. */
    int margin;
    /* A grid table's fold: the runs of old rows it took; where its last line
     * ends and that line's number, when the fold stepped over it (`end` 0
     * otherwise: line `last` is the last); its record, whose tail is made
     * with the table from the state after its closing border (`tail`). */
    table_source_part *parts;
    size_t part_count, part_capacity;
    size_t end;
    int end_line;
    struct markdown_core_table_fold fold;
    size_t tail;
    /* The rows the table took, and the lines at the margin in those of a
     * simple or multiline table (their tallies, node.h). */
    size_t taken_rows, taken_tally;
    size_t equal_capacity;
} table_candidate;

typedef struct {
    size_t runs, count, line;
} table_separator_key;
typedef struct {
    size_t first, count, digit;
} table_separator_group;

/* A PATCH OF A GRID TABLE (E6): cells of its bands that no wall separates,
 * which becomes one cell once it closes. It spans bands [top, bottom] and
 * columns [from, to] and covers `area` of their cells. Within a band it may
 * join another (`parent`); closed, it waits on its top border's list
 * (`next`) until its row is made. */
typedef struct {
    size_t top, bottom, area, stamp;
    int from, to, parent, next, slot;
} table_patch;

/* A BORDER OF A GRID TABLE that the fold read (E6): its line, whether the
 * whole width is a border, whether a row begins or ends at it (`kept`),
 * whether the fold took the old row below it (`taken`), the fold's state
 * after it, which is the entry of a row below it, the closed patches whose
 * top it is and the furthest border they reach. */
typedef struct {
    size_t line, deepest;
    size_t after;
    int closed;
    bool full, kept, taken;
} table_band;

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
    /* A grid fold's columns: its shape (`grid_parents`, `grid_sizes`, and
     * `grid_classes` while it is named), and per geometry column of a wall,
     * the border's edges, the band's walls, the patch at the column and an
     * order. A fold's record positions: a grid table's walls, a simple or
     * multiline table's dash runs (`fold_positions`). */
    int *grid_parents, *grid_sizes, *grid_classes, *fold_positions;
    size_t grid_capacity, fold_positions_capacity;
    unsigned char *lattice_edges;
    bool *lattice_walls;
    int *lattice_faces, *lattice_order, *lattice_losers;
    size_t lattice_capacity;
    table_patch *patches;
    size_t patch_count, patch_capacity;
    int patch_free;
    table_band *bands;
    size_t band_count, band_capacity;
    /* The states of the fold the sweep kept, and their values. */
    table_point *points;
    size_t point_count, point_capacity;
    int *point_values;
    size_t point_values_count, point_values_capacity;
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

static bool table_source_get(table_source *source, size_t index) {
    while (source->count <= index && !source->ended && !source->parser->error) {
        markdown_core_chunk line;
        int first, indent, blanks;
        if (!markdown_core_parser_lookahead_next(&source->lookahead, &line, &first, &indent, &blanks)) {
            source->ended = true;
            break;
        }
        if (!table_source_push(source, (table_source_line){.data = line.data,
                                                           .length = line.len,
                                                           .offset = source->parser->offset,
                                                           .first = first,
                                                           .first_column = source->parser->first_nonspace_column,
                                                           .indent = indent,
                                                           .blanks = blanks,
                                                           .line = source->lookahead.line - 1})) {
            return false;
        }
    }
    return index < source->count && !source->parser->error;
}

static void table_source_end(table_source *source) {
    markdown_core_parser_lookahead_end(&source->lookahead);
    table_workspace *workspace = source->workspace;
    workspace->bytes_count = workspace->dashes_count = 0;
}

static void table_candidate_reset(table_candidate *candidate) {
    for (size_t i = 0; i < candidate->row_count; i++) {
        table_entry_release(candidate->rows[i].entry);
    }
    table_entry_release(candidate->fold.tail);
    candidate->fold.tail = NULL;
    candidate->fold.positions = NULL;
    candidate->fold.position_count = candidate->fold.equal_count = candidate->taken_rows = candidate->taken_tally = 0;
    candidate->fold.form = candidate->fold.margin = 0;
    candidate->column_count = candidate->row_count = candidate->cell_count = candidate->part_count = 0;
    candidate->head_count = candidate->foot_count = candidate->first = candidate->last = candidate->end = 0;
    candidate->block_content = candidate->pipe = false;
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
    candidate->rows[candidate->row_count++] =
        (table_source_row){first, last, candidate->cell_count, 0, 0, SIZE_MAX, NULL, 0};
    return true;
}

static bool table_add_cell(table_source *source, table_candidate *candidate, size_t row, size_t first, size_t last,
                           int left, int right, int start, int end) {
    {
        void *grown = table_reserve(source, candidate->cells, &candidate->cell_capacity, candidate->cell_count + 1,
                                    sizeof(*candidate->cells));
        if (!grown) {
            return false;
        }
        candidate->cells = grown;
    }
    candidate->cells[candidate->cell_count++] = (table_source_cell){first, last, left, right, row, 1, 1, start, end};
    candidate->rows[row].count++;
    return true;
}

/* WHERE A TABLE STARTS. The lines of a simple, multiline or grid table
 * share one column geometry, so its left edge belongs to the table, not to a
 * line: the indentation all of those lines share. Columns before the margin
 * are blank on every one of them -- block indentation, which never changes a
 * block's meaning -- so the first column, the table and each row begin at the
 * margin wherever that indentation came from. The separator is one of these
 * lines, so the margin never lies right of the first dash run; a line whose
 * text starts left of that run moves the margin to its text, which the first
 * column owns. A row whose first line is indented further still begins at the
 * margin: its leading columns are the first cell's, and an empty first cell
 * sits there. A pipe row shares no geometry, so its margin is its own. */
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
 * margin lies in every table line's indentation, so the walk stays inside
 * the whitespace the block parser has already measured and needs no column
 * map; a multiline table's opening boundary never gets one. A tab that spans
 * the margin is the byte there, as the column map would say. */
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

/* The cells of row `row` of a simple or multiline table, once its margin is
 * known: its lines are read for their columns only then. */
static bool table_row_cells(table_source *source, table_candidate *candidate, size_t row, size_t runs) {
    size_t first = candidate->rows[row].first, last = candidate->rows[row].last;
    for (size_t i = first; i <= last; i++) {
        if (!table_source_columns(source, i)) {
            return false;
        }
    }
    candidate->rows[row].cell = candidate->cell_count;
    /* Interior columns meet at dash-run starts; the outer two reach the
     * table's edges, the margin and the line's end. */
    for (size_t column = 0; column < candidate->column_count; column++) {
        int left = column ? table_dash(source, runs, column).start : candidate->margin;
        int right = column + 1 < candidate->column_count ? table_dash(source, runs, column + 1).start : INT_MAX;
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
        if (!table_add_cell(source, candidate, row, cell_first, cell_last, left, right, start + 1, end)) {
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

/* Where the line holding `at` begins. */
static size_t table_line_begin(markdown_core_parser *parser, size_t at) {
    while (at > 0) {
        unsigned char byte = *markdown_core_parser_input_at(parser, at - 1);
        if (byte == '\n' || byte == '\r') {
            break;
        }
        at--;
    }
    return at;
}

/* THE OLD TABLE'S SECTIONS: its head, body and foot are each a relation,
 * whose first row's lead runs from the table's start. The rows of the
 * section that holds row `index`: from `*first` to `*end`. */
static void table_old_section(const markdown_core_node *old, size_t index, size_t *first, size_t *end) {
    const markdown_core_table *table = old->opaque;
    size_t head = table->head_count, body = head + table->content_count;
    *first = index < head ? 0 : index < body ? head : body;
    *end = index < head ? head : index < body ? body : markdown_core_children_count(old->children);
}

/* Where the lead of the old table's row `index` starts. */
static int64_t table_old_lead(const table_source *source, size_t index) {
    const markdown_core_run *children = source->old->children;
    size_t first, end;
    table_old_section(source->old, index, &first, &end);
    return source->old_start + markdown_core_children_length_before(children, index) -
           markdown_core_children_length_before(children, first);
}

/* Whether row `index` of a table whose head holds `head` rows and whose
 * foot begins at row `foot` is the first of its section. */
static bool table_section_first(size_t head, size_t foot, size_t index) {
    return index == 0 || index == head || index == foot;
}

/* The old row that ends first after `at`, child `*child` of the old table,
 * whose lead starts at `*lead`; false when none does. A section's rows are
 * measured from the table's start (above). */
static bool table_old_row(const table_source *source, int64_t at, size_t *child, int64_t *lead) {
    const markdown_core_node *old = source->old;
    size_t first = 0, end = 0, total = markdown_core_children_count(old->children);
    for (; first < total; first = end) {
        table_old_section(old, first, &first, &end);
        if (markdown_core_children_find(old->children,
                                        source->old_start - markdown_core_children_length_before(old->children, first),
                                        at, child, lead) &&
            *child >= first && *child < end) {
            return true;
        }
    }
    return false;
}

/* A run of `count` old rows from child `first` that the table takes, from
 * `start` to `end`, ordered with its rows by `key`; it joins the run before
 * it when they are adjacent. */
static void table_candidate_part(table_source *source, table_candidate *candidate, size_t key, size_t first,
                                 size_t count, size_t start, size_t end) {
    candidate->taken_rows += count;
    if (candidate->part_count) {
        table_source_part *last = &candidate->parts[candidate->part_count - 1];
        if (last->first + last->count == first) {
            last->count += count;
            last->end = end;
            return;
        }
    }
    void *grown = table_reserve(source, candidate->parts, &candidate->part_capacity, candidate->part_count + 1,
                                sizeof(*candidate->parts));
    if (!grown) {
        return;
    }
    candidate->parts = grown;
    candidate->parts[candidate->part_count++] = (table_source_part){key, first, count, start, end};
}

/* Whether the old rows the table takes keep their place in their sections'
 * chains: a row that begins a section of the old table begins one of the
 * new table, and the other way round, since its lead runs from the table's
 * start or from the row before it. */
static bool table_parts_chained(const table_source *source, const table_candidate *candidate, size_t rows) {
    const markdown_core_table *was = source->old ? source->old->opaque : NULL;
    size_t head = candidate->head_count, foot = rows - candidate->foot_count;
    size_t old_head = was ? was->head_count : 0, old_foot = was ? was->head_count + was->content_count : 0;
    for (size_t r = 0, p = 0, index = 0; p < candidate->part_count;) {
        if (r < candidate->row_count && candidate->rows[r].key <= candidate->parts[p].key) {
            r++;
            index++;
            continue;
        }
        const table_source_part *part = &candidate->parts[p++];
        size_t bounds[3] = {0, head, foot}, old_bounds[3] = {0, old_head, old_foot};
        for (size_t i = 0; i < 3; i++) {
            if ((bounds[i] >= index && bounds[i] < index + part->count &&
                 !table_section_first(old_head, old_foot, bounds[i] - index + part->first)) ||
                (old_bounds[i] >= part->first && old_bounds[i] < part->first + part->count &&
                 !table_section_first(head, foot, old_bounds[i] - part->first + index))) {
                return false;
            }
        }
        index += part->count;
    }
    return true;
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
static bool table_same_dashes(table_source *source, size_t left, size_t right) {
    size_t count = table_dash_count(source, left);
    if (count < 2 || table_dash_count(source, right) != count) {
        return false;
    }
    if (!table_prepare_dashes(source, left) || !table_prepare_dashes(source, right)) {
        return false;
    }
    size_t a = source->lines[left].dash_offset, b = source->lines[right].dash_offset;
    for (size_t i = 0; i < count; i++) {
        if (table_dash(source, a, i).start != table_dash(source, b, i).start ||
            table_dash(source, a, i).end != table_dash(source, b, i).end) {
            source->workspace->work.scan += i + 1;
            return false;
        }
    }
    source->workspace->work.scan += count;
    return true;
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

/* THE RECORD A SIMPLE OR MULTILINE TABLE IS READ AGAINST (E5): the old
 * table's, when it had this form and these dash runs, from `runs`. */
static const struct markdown_core_table_fold *table_lines_record(table_source *source, int form, size_t runs,
                                                                 size_t count) {
    const markdown_core_table *old = source->old ? source->old->opaque : NULL;
    const struct markdown_core_table_fold *record = old ? old->fold : NULL;
    if (!record || record->form != form || record->position_count != 2 * count) {
        return NULL;
    }
    source->workspace->work.scan += count;
    for (size_t i = 0; i < count; i++) {
        table_interval run = table_dash(source, runs, i);
        if (record->positions[2 * i] != run.start || record->positions[2 * i + 1] != run.end) {
            return NULL;
        }
    }
    return record;
}

/* THE FOLD OF A SIMPLE OR MULTILINE TABLE TAKES THE RUN OF UNCHANGED OLD ROWS
 * that begins on the line after line `index`, through the last line it has
 * read: it steps over their lines, which their reach covers, and reads on
 * from the line after them. Whether it took them. */
static bool table_lines_take(table_source *source, table_candidate *candidate, size_t index) {
    markdown_core_parser *parser = source->parser;
    const markdown_core_node *old = source->old;
    const table_source_line *line = &source->lines[index];
    int64_t at = markdown_core_parser_source_end(parser, line->line, line->length), lead;
    size_t child, section, section_end;
    if (index + 2 < source->count || !table_old_row(source, at, &child, &lead)) {
        return false;
    }
    const markdown_core_node *row = markdown_core_children_at(old->children, child);
    int64_t start = lead + row->where.extent.lead;
    if (at >= start || (row->flags & MARKDOWN_CORE_NODE__CHANGED) ||
        table_line_begin(parser, (size_t)start) != markdown_core_parser_line_after(parser, (size_t)at)) {
        return false;
    }
    table_old_section(old, child, &section, &section_end);
    markdown_core_run_sums sums;
    size_t count = markdown_core_children_take_run(old->children, child, section_end, &sums);
    if (!count) {
        return false;
    }
    size_t end = (size_t)(lead + (int64_t)sums.length);
    table_candidate_part(source, candidate, index, child, count, (size_t)start, end);
    candidate->taken_tally += sums.tally;
    markdown_core_parser_lookahead_skip(&source->lookahead, markdown_core_parser_line_after(parser, end),
                                        end + sums.reach);
    candidate->end = end;
    candidate->end_line = source->lookahead.line - 1;
    return !parser->error;
}

/* A SIMPLE OR MULTILINE TABLE'S MARGIN, its read lines from `first` to
 * `last` reaching `margin` at the least, and its record: the old table's
 * when it took rows, which are measured from it. False when it took rows
 * and its margin is another, and the table is read again without them;
 * the rows it took reach the old margin when their tallies do. Then each
 * row it read has its cells. */
static bool table_lines_finish(table_source *source, table_candidate *candidate,
                               const struct markdown_core_table_fold *record, int form, size_t runs, size_t count) {
    int margin = table_margin(source, candidate->first, candidate->last);
    if (candidate->part_count) {
        if (margin < record->margin || (margin > record->margin && !candidate->taken_tally)) {
            return false;
        }
        margin = record->margin;
    }
    candidate->margin = margin;
    if (!table_parts_chained(source, candidate, candidate->row_count + candidate->taken_rows)) {
        return false;
    }
    for (size_t r = 0; r < candidate->row_count; r++) {
        if (!table_row_cells(source, candidate, r, runs)) {
            return false;
        }
    }
    table_workspace *workspace = source->workspace;
    void *grown = table_reserve(source, workspace->fold_positions, &workspace->fold_positions_capacity, 2 * count,
                                sizeof(*workspace->fold_positions));
    if (!grown) {
        return false;
    }
    workspace->fold_positions = grown;
    for (size_t i = 0; i < count; i++) {
        table_interval run = table_dash(source, runs, i);
        workspace->fold_positions[2 * i] = run.start;
        workspace->fold_positions[2 * i + 1] = run.end;
    }
    candidate->fold.form = form;
    candidate->fold.margin = margin;
    candidate->fold.positions = workspace->fold_positions;
    candidate->fold.position_count = 2 * count;
    return true;
}

/* A row a simple or multiline table read, from line `first` to `last`,
 * decided once the input was read as far as it is now. */
static bool table_lines_row(table_source *source, table_candidate *candidate, size_t first, size_t last) {
    if (!table_add_row(source, candidate, first, last)) {
        return false;
    }
    table_source_row *row = &candidate->rows[candidate->row_count - 1];
    row->key = first;
    row->reads = source->parser->line_reads;
    return true;
}

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
    if (!table_prepare_dashes(source, delimiter)) {
        goto failed;
    }
    size_t runs = source->lines[delimiter].dash_offset;
    const struct markdown_core_table_fold *record = table_lines_record(source, TABLE_FORM_SIMPLE, runs, count);
    if (!headerless && !table_lines_row(source, candidate, start, start)) {
        goto failed;
    }
    bool footer = false;
    /* Simple rows own inline text until a blank/valid footer. Pandoc 3.11
     * retains heading, quote and fence markers here; the header/caption
     * paragraph-interruption rules do not apply to an existing body. Each
     * body line is a row, and the rows of the old table that no edit met
     * are taken after the first body line, which a headerless table reads
     * its alignment from. */
    for (size_t i = body;; i++) {
        if (record && i > body && i == source->count) {
            table_lines_take(source, candidate, i - 1);
        }
        if (!table_source_get(source, i) || source->lines[i].blanks) {
            break;
        }
        end = i;
        candidate->end = 0;
        if (table_same_dashes(source, delimiter, i)) {
            footer = true;
            break;
        }
        if (!table_lines_row(source, candidate, i, i)) {
            goto failed;
        }
    }
    if (source->parser->error) {
        goto failed;
    }
    if (headerless && !footer && !candidate->part_count) {
        table_simple_search_finish(source, delimiter, end);
    }
    if ((end == delimiter && !candidate->part_count) || (headerless && !footer) || source->parser->error) {
        goto failed;
    }
    if (!table_source_columns(source, start) || !table_source_columns(source, body) ||
        !table_set_columns(source, candidate, runs, count, headerless ? body : start, false)) {
        goto failed;
    }
    candidate->first = start;
    candidate->last = end;
    candidate->head_count = headerless ? 0 : 1;
    if (!table_lines_finish(source, candidate, record, TABLE_FORM_SIMPLE, runs, count)) {
        goto failed;
    }
    return true;
failed:
    source->redo |= candidate->part_count != 0;
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
    if (table_search_absent(source, delimiter, TABLE_NO_CLOSING_BOUNDARY) || !table_prepare_dashes(source, delimiter)) {
        goto failed;
    }
    size_t runs = source->lines[delimiter].dash_offset;
    const struct markdown_core_table_fold *record = table_lines_record(source, TABLE_FORM_MULTILINE, runs, count);
    if (header && !table_lines_row(source, candidate, start + 1, delimiter - 1)) {
        goto failed;
    }
    /* The table ends at a full boundary followed by a blank line or the end
     * of the input, and a blank line begins each row after the first. This
     * search judges with `table_full_boundary` and `.blanks`, both of which
     * read RAW BYTES; the rows' cells are made once the table is found, and
     * only then is the per-scalar column map built for their lines: the
     * lines it walks may never become part of a table. On `block-hr.x1` a
     * lone ` -  -  -  -  -` keeps a headerless multiline alive to EOF, and
     * the map built for it covered 56,680 of that document's 56,704
     * non-blank characters for a candidate that then failed. After a blank
     * line, the rows of the old table that no edit met are taken. */
    size_t first = delimiter + 1, end = delimiter + 1, body_count = 0;
    for (;; end++) {
        if (!table_source_get(source, end)) {
            if (!candidate->part_count) {
                table_search_finish(source, delimiter, source->count, TABLE_NO_CLOSING_BOUNDARY);
            }
            goto failed;
        }
        candidate->end = 0;
        if (table_full_boundary(source, end) && (!table_source_get(source, end + 1) || source->lines[end + 1].blanks)) {
            break;
        }
        if (source->lines[end].blanks && end > first) {
            if (!table_lines_row(source, candidate, first, end - 1)) {
                goto failed;
            }
            body_count++;
            first = end;
            if (record && table_lines_take(source, candidate, end - 1)) {
                first = end + 1;
            }
        }
    }
    if (end == delimiter + 1 || source->parser->error) {
        goto failed;
    }
    if (first < end) {
        if (!table_lines_row(source, candidate, first, end - 1)) {
            goto failed;
        }
        body_count++;
    }
    if (body_count + candidate->taken_rows == 1 && !source->lines[end].blanks) {
        goto failed;
    }
    candidate->block_content = true;
    candidate->padding_limit = INT_MAX;
    candidate->first = start;
    candidate->last = end;
    candidate->head_count = header ? 1 : 0;
    if (!table_set_columns(source, candidate, runs, count, header ? start + 1 : delimiter + 1, true) ||
        !table_lines_finish(source, candidate, record, TABLE_FORM_MULTILINE, runs, count)) {
        goto failed;
    }
    return true;
failed:
    source->redo |= candidate->part_count != 0;
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

/* THE GRID FOLD (docs/plans/2026-09-29-incremental-parsing.md, E6). A grid
 * table is a fold over its lines from the opening border. A line whose
 * walls carry a '+' is a BORDER; between two borders lies a BAND. Each band
 * starts as one patch per column, joins the patch above it where its top
 * border has no edge, and joins its neighbour where no wall runs the band's
 * height. A patch that does not go on past a border closes, and is a cell:
 * a rectangle, or the table is not one. Rows begin and end at the borders
 * cells begin or end at, and at the borders a cell's side is joined at by a
 * '+'; a row is made once no open patch can still change them.
 *
 * The walls are the columns the '+' joins of every line put with the
 * opening border's left wall. The fold reads with the walls it speculates
 * -- the old table's, or the opening border's -- and folds the joins as it
 * goes: its SHAPE. Where they turn out otherwise, it reads the lines again
 * with the walls they make.
 *
 * Each row records the fold's state after its top border as its entry
 * (`table_entry`). Against an old table, after a border where an old row
 * begins that no edit met and whose entry reads rows as the fold now does,
 * the fold takes the row: it steps over the run of unchanged rows from it
 * when its shape is the old one too and every row before it is made, and
 * reads on from the entry of the row after the run, or from the table's
 * tail; otherwise it reads the row's lines for what they decide of the rows
 * before it and of its shape, and makes no row of them. */
typedef struct {
    table_source *source;
    table_candidate *candidate;
    /* The old table's fold record, when the fold reads against it. */
    const struct markdown_core_table_fold *record;
    size_t start;
    int left, right;
    const int *positions;
    size_t width;
    /* The last line read was a border (`held`), whose whole width is `full`;
     * a patch was not a rectangle, or a head or foot border was crossed. */
    bool held, broken, skipped, misaligned, shape_dirty;
    unsigned char full;
    /* Borders read; the first one with a record; the top border of the
     * next row to make, and the border after it from which a bottom is
     * sought; rows made, taken or stepped over. */
    size_t count, base, next_top, scanned, rows, stamp;
    /* The last line read, and where the table's last line ends and its
     * number when the fold stepped over it (`end` 0 otherwise). */
    size_t last;
    size_t end;
    int end_line;
    /* The last state the fold kept. */
    size_t point;
} table_lattice;

static bool table_wall(const table_source_line *line, int column) {
    int character = table_character(line, column);
    return character == '|' || character == '+';
}

static table_band *table_band_at(const table_lattice *lattice, size_t border) {
    return &lattice->source->workspace->bands[border - lattice->base];
}

static int table_patch_new(table_lattice *lattice, size_t top, int column) {
    table_workspace *workspace = lattice->source->workspace;
    int index = workspace->patch_free;
    if (index >= 0) {
        workspace->patch_free = workspace->patches[index].next;
    } else {
        void *grown = table_reserve(lattice->source, workspace->patches, &workspace->patch_capacity,
                                    workspace->patch_count + 1, sizeof(*workspace->patches));
        if (!grown) {
            return -1;
        }
        workspace->patches = grown;
        index = (int)workspace->patch_count++;
        if (workspace->patch_count > workspace->work.frontier_peak) {
            workspace->work.frontier_peak = workspace->patch_count;
        }
    }
    workspace->patches[index] = (table_patch){top, top, 1, 0, column, column, index, -1, 0};
    return index;
}

static void table_patch_free(table_workspace *workspace, int index) {
    workspace->patches[index].next = workspace->patch_free;
    workspace->patch_free = index;
}

static int table_patch_root(table_workspace *workspace, int index) {
    table_patch *patches = workspace->patches;
    int root = index;
    while (patches[root].parent != root) {
        root = patches[root].parent;
    }
    while (patches[index].parent != root) {
        int next = patches[index].parent;
        patches[index].parent = root;
        index = next;
    }
    return root;
}

/* The bands and patches of a fold go from the workspace; the next border is
 * `base`. */
static void table_lattice_clear(table_lattice *lattice, size_t base) {
    table_workspace *workspace = lattice->source->workspace;
    workspace->band_count = 0;
    workspace->patch_count = 0;
    workspace->patch_free = -1;
    lattice->base = base;
}

static table_band *table_lattice_band(table_lattice *lattice, size_t line) {
    table_workspace *workspace = lattice->source->workspace;
    void *grown = table_reserve(lattice->source, workspace->bands, &workspace->band_capacity, workspace->band_count + 1,
                                sizeof(*workspace->bands));
    if (!grown) {
        return NULL;
    }
    workspace->bands = grown;
    table_band *band = &workspace->bands[workspace->band_count++];
    *band = (table_band){line, 0, SIZE_MAX, -1, false, false, false};
    lattice->count++;
    return band;
}

/* The '+' joins of a line: a run between two '+' of nothing but rule and
 * space joins them, as the opening border's walls are joined. */
static void table_lattice_shape(table_lattice *lattice, const table_source_line *line) {
    table_workspace *workspace = lattice->source->workspace;
    int *parents = workspace->grid_parents, *sizes = workspace->grid_sizes;
    int previous = -1;
    bool horizontal = true;
    workspace->work.scan += (size_t)(lattice->right - lattice->left) + 1;
    for (int c = lattice->left; c <= lattice->right; c++) {
        int ch = table_character(line, c);
        if (ch == '+') {
            if (previous >= 0 && horizontal) {
                int a = table_grid_root(lattice->source, parents, previous),
                    b = table_grid_root(lattice->source, parents, c);
                if (a != b) {
                    table_grid_join(lattice->source, parents, sizes, a, b);
                    lattice->shape_dirty = true;
                }
            }
            previous = c;
            horizontal = true;
        } else if (ch != '-' && ch != '=' && ch != ' ' && ch != ':') {
            horizontal = false;
        }
    }
}

/* The band below the border just read begins on this line: a column whose
 * edge on the border is open goes on in the patch above, any other starts
 * one. An '=' edge belongs to a whole '=' border, which no patch crosses. */
static void table_lattice_open(table_lattice *lattice) {
    table_workspace *workspace = lattice->source->workspace;
    size_t band = lattice->count - 1;
    bool equals = lattice->full == '=', starts = false;
    for (size_t c = 0; c < lattice->width && !lattice->source->parser->error; c++) {
        unsigned char edge = workspace->lattice_edges[c];
        if (edge == '=' && !equals) {
            lattice->broken = true;
        }
        if (!edge) {
            lattice->broken |= equals;
            table_patch *patch = &workspace->patches[workspace->lattice_faces[c]];
            patch->area++;
            patch->bottom = band;
        } else {
            workspace->lattice_faces[c] = table_patch_new(lattice, band, (int)c);
            starts = true;
        }
    }
    if (starts) {
        table_band_at(lattice, band)->kept = true;
    }
    lattice->held = false;
}

/* A patch that goes no further closes at `border`: a rectangle, whose top
 * and bottom are row borders, as is each border between where a '+' joins
 * its side. It waits for its row on its top border, unless that row is
 * taken or before the fold's records. */
static void table_lattice_close(table_lattice *lattice, int index, size_t border) {
    table_source *source = lattice->source;
    table_workspace *workspace = source->workspace;
    table_patch *patch = &workspace->patches[index];
    if (patch->area != (patch->bottom - patch->top + 1) * (size_t)(patch->to - patch->from + 1)) {
        lattice->broken = true;
    }
    table_band_at(lattice, border)->kept = true;
    size_t from = patch->top + 1 > lattice->base + 1 ? patch->top + 1 : lattice->base + 1;
    workspace->work.scan += 2 * (patch->bottom + 1 - (from < patch->bottom + 1 ? from : patch->bottom + 1));
    for (size_t j = from; j <= patch->bottom; j++) {
        table_band *band = table_band_at(lattice, j);
        const table_source_line *line = &source->lines[band->line];
        if (table_character(line, lattice->positions[patch->from]) == '+' ||
            table_character(line, lattice->positions[patch->to + 1]) == '+') {
            band->kept = true;
        }
    }
    table_band *top = patch->top >= lattice->base ? table_band_at(lattice, patch->top) : NULL;
    if (top && !top->taken) {
        if (top->deepest < border) {
            top->deepest = border;
        }
        patch->next = top->closed;
        top->closed = index;
    } else {
        table_patch_free(workspace, index);
    }
}

/* The row from border `top` to border `bottom`, with the cells whose top
 * is its top, in column order. A row the fold took is the old one. */
static void table_lattice_row(table_lattice *lattice, size_t top, size_t bottom) {
    table_source *source = lattice->source;
    table_workspace *workspace = source->workspace;
    table_candidate *candidate = lattice->candidate;
    table_band *band = table_band_at(lattice, top);
    const table_band *end = table_band_at(lattice, bottom);
    lattice->rows++;
    if (band->taken) {
        return;
    }
    size_t first = band->line + 1;
    size_t last = end->full && end->line > first ? end->line - 1 : end->line;
    if (!table_add_row(source, candidate, first, last)) {
        return;
    }
    table_source_row *row = &candidate->rows[candidate->row_count - 1];
    row->key = top;
    row->point = band->after;
    row->reads = source->parser->line_reads;
    int *order = workspace->lattice_order;
    for (int index = band->closed; index >= 0; index = workspace->patches[index].next) {
        order[workspace->patches[index].from] = index;
    }
    band->closed = -1;
    workspace->work.scan += lattice->width;
    for (size_t c = 0; c < lattice->width; c++) {
        int index = order[c];
        if (index < 0) {
            continue;
        }
        order[c] = -1;
        table_patch patch = workspace->patches[index];
        table_patch_free(workspace, index);
        size_t cell_last = table_band_at(lattice, patch.bottom + 1)->line - 1;
        table_source_line *begin = &source->lines[first],
                          *finish = &source->lines[cell_last < first ? first : cell_last];
        int left = lattice->positions[patch.from] + 1, right = lattice->positions[patch.to + 1];
        if (!table_add_cell(source, candidate, candidate->row_count - 1, first, cell_last, left, right,
                            table_byte(begin, left) + 1, table_byte(finish, right))) {
            return;
        }
        int64_t spans = 0;
        workspace->work.scan += patch.bottom + 1 - patch.top;
        for (size_t j = patch.top + 1; j <= patch.bottom + 1; j++) {
            spans += table_band_at(lattice, j)->kept;
        }
        table_source_cell *cell = &candidate->cells[candidate->cell_count - 1];
        cell->rowspan = spans;
        cell->colspan = patch.to - patch.from + 1;
    }
}

/* Every row whose borders and cells no open patch can still change: those
 * up to border `limit`. */
static void table_lattice_emit(table_lattice *lattice, size_t limit) {
    while (!lattice->source->parser->error) {
        size_t top = lattice->next_top;
        if (top >= lattice->count) {
            break;
        }
        size_t bottom = lattice->scanned > top + 1 ? lattice->scanned : top + 1;
        while (bottom <= limit && bottom < lattice->count && !table_band_at(lattice, bottom)->kept) {
            bottom++;
        }
        lattice->scanned = bottom;
        if (bottom > limit || bottom >= lattice->count) {
            break;
        }
        const table_band *band = table_band_at(lattice, top);
        if (!band->taken && band->deepest > limit) {
            break;
        }
        table_lattice_row(lattice, top, bottom);
        lattice->next_top = bottom;
        lattice->scanned = bottom + 1;
    }
}

/* THE FOLD'S STATE after the border just read (`table_entry`): its shape,
 * then the border's width and edges, the walls of the band below so far,
 * whether a row starts at the border, and the patches that go on past it,
 * named per column in column order, each with how many borders above this
 * one it began, its columns and its area. Kept in the workspace, and equal
 * to the last state kept, it is that state. SIZE_MAX when an allocation
 * failed. */
static size_t table_lattice_point(table_lattice *lattice) {
    table_source *source = lattice->source;
    table_workspace *workspace = source->workspace;
    size_t columns = (size_t)(lattice->right - lattice->left) + 1, width = lattice->width;
    size_t used = workspace->point_values_count, border = lattice->count - 1;
    {
        void *grown = table_reserve(source, workspace->point_values, &workspace->point_values_capacity,
                                    used + columns + 7 * width + 3, sizeof(*workspace->point_values));
        if (!grown) {
            return SIZE_MAX;
        }
        workspace->point_values = grown;
        grown = table_reserve(source, workspace->points, &workspace->point_capacity, workspace->point_count + 1,
                              sizeof(*workspace->points));
        if (!grown) {
            return SIZE_MAX;
        }
        workspace->points = grown;
    }
    int *values = workspace->point_values + used;
    const table_point *last = lattice->point == SIZE_MAX ? NULL : &workspace->points[lattice->point];
    if (lattice->shape_dirty || !last) {
        int *classes = workspace->grid_classes;
        for (int c = lattice->left; c <= lattice->right; c++) {
            classes[c] = -1;
        }
        for (int c = lattice->left; c <= lattice->right; c++) {
            int root = table_grid_root(source, workspace->grid_parents, c);
            if (classes[root] < 0) {
                classes[root] = c;
            }
            values[c - lattice->left] = classes[root];
        }
        lattice->shape_dirty = false;
        workspace->work.scan += 2 * columns;
    } else {
        memcpy(values, workspace->point_values + last->at, columns * sizeof(*values));
    }
    size_t at = columns;
    values[at++] = lattice->full;
    for (size_t c = 0; c < width; c++) {
        values[at++] = workspace->lattice_edges[c];
    }
    for (size_t c = 1; c < width; c++) {
        values[at++] = workspace->lattice_walls[c];
    }
    values[at++] = table_band_at(lattice, border)->kept;
    size_t slots = at++, faces = at;
    at += width;
    int count = 0;
    size_t stamp = ++lattice->stamp;
    for (size_t c = 0; c < width; c++) {
        int index = workspace->lattice_faces[c];
        if (index < 0) {
            values[faces + c] = -1;
            continue;
        }
        table_patch *patch = &workspace->patches[index];
        if (patch->stamp != stamp) {
            patch->stamp = stamp;
            patch->slot = count++;
            values[at++] = (int)(border - patch->top);
            values[at++] = patch->from;
            values[at++] = patch->to;
            values[at++] = (int)patch->area;
        }
        values[faces + c] = patch->slot;
    }
    values[slots] = count;
    workspace->work.scan += at;
    if (last && last->shape == columns && last->count == at &&
        !memcmp(workspace->point_values + last->at, values, at * sizeof(int))) {
        return lattice->point;
    }
    workspace->points[workspace->point_count] = (table_point){used, columns, at, NULL};
    workspace->point_values_count = used + at;
    return lattice->point = workspace->point_count++;
}

/* The entry of state `index`, which the caller holds: the one that holds
 * its values already, or a new one. */
static table_entry *table_point_entry(table_source *source, size_t index) {
    table_point *point = &source->workspace->points[index];
    if (point->entry) {
        return table_entry_retain(point->entry);
    }
    table_entry *entry = markdown_core_alloc(1, sizeof(*entry) + point->count * sizeof(int));
    if (!entry) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    entry->refs = 1;
    entry->shape = point->shape;
    entry->count = point->count;
    memcpy(entry->values, source->workspace->point_values + point->at, point->count * sizeof(int));
    return point->entry = entry;
}

/* The fold stands after a border again at `entry`, the state an old row's
 * top border or the old table's closing border left: the next row begins
 * there, on the line after `line`. */
static void table_lattice_restore(table_lattice *lattice, table_entry *entry, size_t line) {
    table_source *source = lattice->source;
    table_workspace *workspace = source->workspace;
    size_t columns = entry->shape, width = lattice->width;
    const int *values = entry->values;
    int *parents = workspace->grid_parents, *sizes = workspace->grid_sizes;
    for (int c = lattice->left; c <= lattice->right; c++) {
        parents[c] = values[c - lattice->left];
        sizes[c] = 0;
    }
    for (int c = lattice->left; c <= lattice->right; c++) {
        sizes[values[c - lattice->left]]++;
    }
    lattice->shape_dirty = false;
    size_t used = workspace->point_values_count;
    {
        void *grown = table_reserve(source, workspace->point_values, &workspace->point_values_capacity,
                                    used + entry->count, sizeof(*workspace->point_values));
        if (!grown) {
            return;
        }
        workspace->point_values = grown;
        grown = table_reserve(source, workspace->points, &workspace->point_capacity, workspace->point_count + 1,
                              sizeof(*workspace->points));
        if (!grown) {
            return;
        }
        workspace->points = grown;
    }
    memcpy(workspace->point_values + used, values, entry->count * sizeof(int));
    workspace->point_values_count = used + entry->count;
    workspace->points[workspace->point_count] = (table_point){used, entry->shape, entry->count, entry};
    lattice->point = workspace->point_count++;
    table_lattice_clear(lattice, lattice->count);
    size_t at = columns;
    lattice->full = (unsigned char)values[at++];
    for (size_t c = 0; c < width; c++) {
        workspace->lattice_edges[c] = (unsigned char)values[at++];
    }
    for (size_t c = 1; c < width; c++) {
        workspace->lattice_walls[c] = values[at++] != 0;
    }
    table_band *band = table_lattice_band(lattice, line);
    if (!band) {
        return;
    }
    band->after = lattice->point;
    band->full = lattice->full != 0;
    band->kept = values[at++] != 0;
    int count = values[at++];
    size_t faces = at, border = lattice->count - 1;
    at += width;
    for (int slot = 0; slot < count; slot++, at += 4) {
        int index = table_patch_new(lattice, border - (size_t)values[at], values[at + 1]);
        if (index < 0) {
            return;
        }
        table_patch *patch = &workspace->patches[index];
        patch->bottom = border - 1;
        patch->to = values[at + 2];
        patch->area = (size_t)values[at + 3];
    }
    for (size_t c = 0; c < width; c++) {
        workspace->lattice_faces[c] = values[faces + c];
    }
    workspace->work.scan += entry->count + columns;
    lattice->held = true;
    lattice->next_top = border;
    lattice->scanned = border + 1;
}

/* A whole '=' border: the rows above it, and the alignment it reads. */
static void table_lattice_equal(table_lattice *lattice, size_t rows, const markdown_core_flow *flows,
                                const table_source_line *line) {
    table_candidate *candidate = lattice->candidate;
    size_t index = candidate->fold.equal_count++;
    if (index >= 3) {
        return;
    }
    size_t width = lattice->width;
    {
        void *grown = table_reserve(lattice->source, candidate->fold.equal_flows, &candidate->equal_capacity, 3 * width,
                                    sizeof(*candidate->fold.equal_flows));
        if (!grown) {
            return;
        }
        candidate->fold.equal_flows = grown;
    }
    candidate->fold.equal_rows[index] = rows;
    markdown_core_flow *into = candidate->fold.equal_flows + index * width;
    for (size_t c = 0; c < width; c++) {
        if (flows) {
            into[c] = flows[c];
            continue;
        }
        bool l = table_character(line, lattice->positions[c] + 1) == ':',
             r = table_character(line, lattice->positions[c + 1] - 1) == ':';
        into[c] = l ? (r ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_LEFT)
                    : (r ? MARKDOWN_CORE_FLOW_RIGHT : MARKDOWN_CORE_FLOW_NONE);
    }
    lattice->source->workspace->work.scan += 2 * width;
}

/* THE FOLD STEPS OVER THE RUN OF UNCHANGED OLD ROWS from child `first` of
 * the old table, whose lead starts at `lead`: through the top border of the
 * row after it, which the run's reach covers, or through the old table's
 * closing border. The '=' borders it steps over are the old table's. */
static void table_lattice_skip(table_lattice *lattice, size_t border, size_t first, int64_t lead, int64_t start) {
    table_source *source = lattice->source;
    markdown_core_parser *parser = source->parser;
    const markdown_core_node *old = source->old;
    const struct markdown_core_table_fold *record = lattice->record;
    size_t total = markdown_core_children_count(old->children), section, section_end;
    table_old_section(old, first, &section, &section_end);
    markdown_core_run_sums sums;
    size_t count = markdown_core_children_take_run(old->children, first, section_end, &sums);
    if (!count) {
        return;
    }
    size_t end = (size_t)(lead + (int64_t)sums.length);
    table_candidate_part(source, lattice->candidate, border, first, count, (size_t)start, end);
    for (size_t i = 0; i < record->equal_count; i++) {
        size_t rows = record->equal_rows[i];
        if (rows > first && rows <= first + count) {
            table_lattice_equal(lattice, lattice->rows + rows - first, record->equal_flows + i * lattice->width, NULL);
        }
    }
    lattice->rows += count;
    size_t next;
    table_entry *entry;
    if (first + count < total) {
        const markdown_core_node *row = markdown_core_children_at(old->children, first + count);
        int64_t from = first + count < section_end ? (int64_t)end : source->old_start;
        next = table_line_begin(parser, (size_t)(from + row->where.extent.lead));
        entry = row->opaque;
    } else {
        next = markdown_core_parser_line_after(parser, end + record->tail_span);
        entry = record->tail;
    }
    assert(entry);
    size_t last = next;
    if (last > end && *markdown_core_parser_input_at(parser, last - 1) == '\n') {
        last--;
    }
    if (last > end && *markdown_core_parser_input_at(parser, last - 1) == '\r') {
        last--;
    }
    lattice->end = last;
    markdown_core_parser_lookahead_skip(&source->lookahead, next, end + sums.reach);
    lattice->end_line = source->lookahead.line - 1;
    lattice->skipped = true;
    table_lattice_restore(lattice, entry, source->count - 1);
}

/* After border `border`, on line `index`: the old row that begins after it,
 * when the fold takes it (above). The fold steps over lines only from the
 * front of what its source has read. */
static void table_lattice_take(table_lattice *lattice, size_t border, size_t index) {
    table_source *source = lattice->source;
    const markdown_core_node *old = source->old;
    const table_source_line *line = &source->lines[index];
    int64_t at = markdown_core_parser_source_end(source->parser, line->line, line->length), lead;
    size_t child;
    if (!table_old_row(source, at, &child, &lead)) {
        return;
    }
    const markdown_core_node *row = markdown_core_children_at(old->children, child);
    int64_t start = lead + row->where.extent.lead;
    table_band *band = table_band_at(lattice, border);
    const table_entry *entry = row->opaque;
    const table_point *point = &source->workspace->points[band->after];
    const int *values = source->workspace->point_values + point->at;
    if (at >= start || (row->flags & MARKDOWN_CORE_NODE__CHANGED) || !entry ||
        !table_entry_reads(entry, point, values)) {
        return;
    }
    if (lattice->next_top == border && index + 1 == source->count && table_entry_shapes(entry, point, values)) {
        table_lattice_skip(lattice, border, child, lead, start);
        return;
    }
    band->taken = true;
    table_candidate_part(source, lattice->candidate, border, child, 1, (size_t)start,
                         (size_t)start + row->where.extent.span);
}

/* A border, on line `index`: the band above it ends, its walls joining its
 * patches where they do not run its height, and the patches that do not go
 * on past it close. The opening border ends no band, and the band below it
 * starts a patch in every column. */
static void table_lattice_border(table_lattice *lattice, size_t index) {
    table_source *source = lattice->source;
    table_workspace *workspace = source->workspace;
    table_source_line *line = &source->lines[index];
    size_t border = lattice->count, width = lattice->width;
    const int *positions = lattice->positions;
    unsigned char *edges = workspace->lattice_edges;
    bool *walls = workspace->lattice_walls;
    int *faces = workspace->lattice_faces;
    table_band *band = table_lattice_band(lattice, index);
    if (!band) {
        return;
    }
    unsigned char full = (unsigned char)table_horizontal(line, lattice->left, lattice->right);
    band->full = full != 0;
    size_t top = SIZE_MAX;
    workspace->work.scan += 3 * width;
    if (border) {
        size_t losers = 0;
        for (size_t c = 1; c < width; c++) {
            walls[c] = walls[c] && table_wall(line, positions[c]);
            if (!walls[c]) {
                int a = table_patch_root(workspace, faces[c - 1]), b = table_patch_root(workspace, faces[c]);
                if (a != b) {
                    table_patch *keep = &workspace->patches[a], *drop = &workspace->patches[b];
                    keep->top = keep->top < drop->top ? keep->top : drop->top;
                    keep->bottom = keep->bottom > drop->bottom ? keep->bottom : drop->bottom;
                    keep->from = keep->from < drop->from ? keep->from : drop->from;
                    keep->to = keep->to > drop->to ? keep->to : drop->to;
                    keep->area += drop->area;
                    drop->parent = a;
                    workspace->lattice_losers[losers++] = b;
                }
            }
        }
        for (size_t c = 0; c < width; c++) {
            faces[c] = table_patch_root(workspace, faces[c]);
        }
        for (size_t i = 0; i < losers; i++) {
            table_patch_free(workspace, workspace->lattice_losers[i]);
        }
        for (size_t c = 0; c < width; c++) {
            edges[c] = (unsigned char)table_horizontal(line, positions[c], positions[c + 1]);
        }
        size_t on = ++lattice->stamp, closed = ++lattice->stamp;
        for (size_t c = 0; c < width; c++) {
            if (!edges[c]) {
                workspace->patches[faces[c]].stamp = on;
            }
        }
        for (size_t c = 0; c < width && !source->parser->error; c++) {
            table_patch *patch = &workspace->patches[faces[c]];
            if (patch->stamp == on) {
                top = patch->top < top ? patch->top : top;
            } else if (patch->stamp != closed) {
                patch->stamp = closed;
                table_lattice_close(lattice, faces[c], border);
            }
        }
        for (size_t c = 0; c < width; c++) {
            if (workspace->patches[faces[c]].stamp != on) {
                faces[c] = -1;
            }
        }
    } else {
        memset(edges, '-', width);
    }
    lattice->full = full;
    lattice->held = true;
    for (size_t c = 1; c < width; c++) {
        walls[c] = table_wall(line, positions[c]);
    }
    table_lattice_emit(lattice, top == SIZE_MAX ? border : top);
    if (full == '=') {
        table_lattice_equal(lattice, lattice->rows, NULL, line);
    }
    size_t after = table_lattice_point(lattice);
    table_band_at(lattice, border)->after = after;
    if (source->old && lattice->record && after != SIZE_MAX) {
        table_lattice_take(lattice, border, index);
    }
}

/* Reads the table's lines from its opening border with the walls at
 * `positions`, `width` columns between them. */
static void table_lattice_sweep(table_lattice *lattice) {
    table_source *source = lattice->source;
    table_workspace *workspace = source->workspace;
    for (int c = lattice->left; c <= lattice->right; c++) {
        workspace->grid_parents[c] = c;
        workspace->grid_sizes[c] = 1;
    }
    for (size_t c = 0; c < lattice->width; c++) {
        workspace->lattice_faces[c] = workspace->lattice_order[c] = -1;
    }
    table_lattice_clear(lattice, 0);
    workspace->point_count = workspace->point_values_count = 0;
    lattice->point = SIZE_MAX;
    lattice->held = lattice->broken = lattice->skipped = lattice->misaligned = false;
    lattice->shape_dirty = true;
    lattice->full = 0;
    lattice->count = lattice->next_top = lattice->rows = 0;
    lattice->scanned = 1;
    lattice->last = lattice->start;
    lattice->end = 0;
    for (size_t i = lattice->start; table_source_get(source, i) && !source->parser->error; i++) {
        if (i > lattice->start && source->lines[i].blanks) {
            break;
        }
        if (!table_source_columns(source, i)) {
            return;
        }
        table_source_line *line = &source->lines[i];
        workspace->work.scan++;
        /* A grid line BEGINS with its wall: its indentation reaches the
         * wall's column exactly. Text left of the wall is not a wall-led
         * line; it ends the candidate instead of being dropped from a table
         * that starts after it. */
        int first = table_character(line, lattice->left);
        if ((first != '+' && first != '|') || line->indent != lattice->left) {
            break;
        }
        int last = table_trim_spaces(line, lattice->left + 1, line->columns) - 1;
        workspace->work.scan++;
        if (last != lattice->right || !table_wall(line, lattice->right)) {
            lattice->misaligned = true;
            return;
        }
        table_lattice_shape(lattice, line);
        bool border = false;
        workspace->work.scan += lattice->width + 1;
        for (size_t c = 0; c <= lattice->width; c++) {
            border |= table_character(line, lattice->positions[c]) == '+';
        }
        if (lattice->held) {
            table_lattice_open(lattice);
        }
        lattice->last = i;
        lattice->end = 0;
        if (!border) {
            workspace->work.scan += lattice->width;
            for (size_t c = 1; c < lattice->width; c++) {
                workspace->lattice_walls[c] = workspace->lattice_walls[c] && table_wall(line, lattice->positions[c]);
            }
        } else {
            table_lattice_border(lattice, i);
        }
    }
}

static bool table_parse_grid(table_source *source, size_t start, table_candidate *candidate) {
    int left, right;
    if (!table_source_get(source, start) || table_search_absent(source, start, TABLE_NO_GRID) ||
        !table_grid_opening(source, start, &left, &right)) {
        return false;
    }
    table_workspace *workspace = source->workspace;
    size_t columns = (size_t)right + 1;
    if (columns > workspace->grid_capacity) {
        size_t capacity = workspace->grid_capacity;
        void *grown = table_reserve(source, workspace->grid_parents, &capacity, columns, sizeof(int));
        if (!grown) {
            return false;
        }
        workspace->grid_parents = grown;
        capacity = workspace->grid_capacity;
        grown = table_reserve(source, workspace->grid_sizes, &capacity, columns, sizeof(int));
        if (!grown) {
            return false;
        }
        workspace->grid_sizes = grown;
        capacity = workspace->grid_capacity;
        grown = table_reserve(source, workspace->grid_classes, &capacity, columns, sizeof(int));
        if (!grown) {
            return false;
        }
        workspace->grid_classes = grown;
        workspace->grid_capacity = capacity;
    }
    {
        void *grown = table_reserve(source, workspace->fold_positions, &workspace->fold_positions_capacity, columns,
                                    sizeof(*workspace->fold_positions));
        if (!grown) {
            return false;
        }
        workspace->fold_positions = grown;
    }
    int *positions = workspace->fold_positions;
    const markdown_core_table *old = source->old ? source->old->opaque : NULL;
    const struct markdown_core_table_fold *record = old ? old->fold : NULL;
    size_t count = 0;
    /* The walls the fold speculates: the old table's, when it had the
     * opening border's outer walls, or the opening border's own. */
    if (record && record->form == TABLE_FORM_GRID && record->positions[0] == left &&
        record->positions[record->position_count - 1] == right) {
        count = record->position_count;
        memcpy(positions, record->positions, count * sizeof(*positions));
    } else {
        record = NULL;
        const table_source_line *line = &source->lines[start];
        for (int c = left; c <= right; c++) {
            if (table_character(line, c) == '+') {
                positions[count++] = c;
            }
        }
    }
    table_lattice lattice = {.source = source,
                             .candidate = candidate,
                             .record = record,
                             .start = start,
                             .left = left,
                             .right = right,
                             .positions = positions,
                             .point = SIZE_MAX};
    bool valid = false, searched = false;
    for (;;) {
        lattice.width = count - 1;
        if (lattice.width + 1 > workspace->lattice_capacity) {
            size_t capacity = workspace->lattice_capacity, needed = lattice.width + 1;
            void *grown = table_reserve(source, workspace->lattice_edges, &capacity, needed, sizeof(unsigned char));
            if (!grown) {
                goto done;
            }
            workspace->lattice_edges = grown;
            capacity = workspace->lattice_capacity;
            grown = table_reserve(source, workspace->lattice_walls, &capacity, needed, sizeof(bool));
            if (!grown) {
                goto done;
            }
            workspace->lattice_walls = grown;
            capacity = workspace->lattice_capacity;
            grown = table_reserve(source, workspace->lattice_faces, &capacity, needed, sizeof(int));
            if (!grown) {
                goto done;
            }
            workspace->lattice_faces = grown;
            capacity = workspace->lattice_capacity;
            grown = table_reserve(source, workspace->lattice_order, &capacity, needed, sizeof(int));
            if (!grown) {
                goto done;
            }
            workspace->lattice_order = grown;
            capacity = workspace->lattice_capacity;
            grown = table_reserve(source, workspace->lattice_losers, &capacity, needed, sizeof(int));
            if (!grown) {
                goto done;
            }
            workspace->lattice_losers = grown;
            workspace->lattice_capacity = capacity;
        }
        table_lattice_sweep(&lattice);
        if (source->parser->error) {
            goto done;
        }
        if (lattice.misaligned) {
            if (!lattice.skipped) {
                table_grid_search_finish(source, start, lattice.last, left, right, false);
            }
            goto done;
        }
        /* A fold that read every line proves the closing border and the '='
         * borders for the openers its run holds, as the search does. */
        if (!lattice.skipped && !searched) {
            searched = true;
            if (!table_grid_search_finish(source, start, lattice.last, left, right, true)) {
                goto done;
            }
        }
        int *parents = workspace->grid_parents;
        int root = table_grid_root(source, parents, left);
        if (table_grid_root(source, parents, right) != root) {
            goto done;
        }
        bool same = true;
        size_t walls = 0;
        workspace->work.scan += (size_t)(right - left) + 1;
        for (int c = left; c <= right; c++) {
            if (table_grid_root(source, parents, c) == root) {
                same &= walls < count && positions[walls] == c;
                walls++;
            }
        }
        same &= walls == count;
        if (same) {
            break;
        }
        if (lattice.skipped) {
            goto done;
        }
        /* The lines join other walls: read them again with those. */
        count = 0;
        for (int c = left; c <= right; c++) {
            if (table_grid_root(source, parents, c) == root) {
                positions[count++] = c;
            }
        }
        lattice.record = NULL;
        table_candidate_reset(candidate);
    }
    if (count < 2) {
        goto done;
    }
    for (size_t c = 1; c < count; c++) {
        if (positions[c] - positions[c - 1] <= 1) {
            goto done;
        }
    }
    {
        /* The patches that go on past the closing border close at it. */
        size_t stamp = ++lattice.stamp;
        for (size_t c = 0; c < lattice.width && lattice.held && !source->parser->error; c++) {
            int index = workspace->lattice_faces[c];
            if (index >= 0 && workspace->patches[index].stamp != stamp) {
                workspace->patches[index].stamp = stamp;
                table_lattice_close(&lattice, index, lattice.count - 1);
            }
        }
    }
    /* The table ends on its closing border, after at least one other; no
     * patch broke the grammar, and its '=' borders are where they may be. */
    size_t equals = candidate->fold.equal_count;
    if (source->parser->error || lattice.broken || !lattice.held || lattice.count < 2 || !lattice.full ||
        (lattice.full == '=' ? equals < 2 || equals > 3 : equals > 1)) {
        goto done;
    }
    table_lattice_emit(&lattice, SIZE_MAX);
    if (source->parser->error || lattice.next_top != lattice.count - 1) {
        goto done;
    }
    size_t width = lattice.width, rows = lattice.rows;
    candidate->column_count = width;
    if (!table_candidate_columns(source, candidate, width)) {
        markdown_core_parser_fail(source->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        goto done;
    }
    candidate->first = start;
    candidate->last = lattice.last;
    candidate->end = lattice.end;
    candidate->end_line = lattice.end_line;
    /* Every grid line is indented exactly to the wall (above), so the shared
     * indentation `table_margin` would find is the wall's column. */
    candidate->margin = left;
    candidate->block_content = true;
    candidate->padding_limit = 1;
    bool closes_equal = lattice.full == '=';
    candidate->head_count = equals && !(closes_equal && equals == 1) ? candidate->fold.equal_rows[0] : 0;
    candidate->foot_count = closes_equal && equals >= 2 ? rows - candidate->fold.equal_rows[equals - 2] : 0;
    if (!table_parts_chained(source, candidate, rows)) {
        source->redo = true;
        goto done;
    }
    double total = 0;
    for (size_t c = 0; c < width; c++) {
        total += positions[c + 1] - positions[c] - 1;
    }
    const table_source_line *opening = &source->lines[start];
    source->workspace->work.scan += 2 * width;
    for (size_t c = 0; c < width; c++) {
        if (candidate->head_count) {
            candidate->columns[c].flow = candidate->fold.equal_flows[c];
        } else {
            bool l = table_character(opening, positions[c] + 1) == ':',
                 r = table_character(opening, positions[c + 1] - 1) == ':';
            candidate->columns[c].flow = l ? (r ? MARKDOWN_CORE_FLOW_CENTER : MARKDOWN_CORE_FLOW_LEFT)
                                           : (r ? MARKDOWN_CORE_FLOW_RIGHT : MARKDOWN_CORE_FLOW_NONE);
        }
        candidate->columns[c].relative =
            (markdown_core_optional_double){true, (positions[c + 1] - positions[c] - 1) / total};
    }
    candidate->fold.form = TABLE_FORM_GRID;
    candidate->fold.margin = left;
    candidate->fold.positions = positions;
    candidate->fold.position_count = count;
    candidate->tail = table_band_at(&lattice, lattice.count - 1)->after;
    valid = !source->parser->error;
done:
    if (!valid) {
        source->redo |= lattice.skipped;
        table_candidate_reset(candidate);
    }
    table_lattice_clear(&lattice, 0);
    return valid;
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
        if (!table_add_cell(source, candidate, candidate->row_count - 1, start, start, table_column(head, from),
                            table_column(head, to), from + 1, to)) {
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
                                                                                 bool pipe) {
    markdown_core_input_line *line = markdown_core_parser_source_line(parser, next_line);
    if (!line) {
        return false;
    }
    const unsigned char *byte = markdown_core_parser_line_bytes(parser, line);
    const unsigned char *end = byte + (line->end - line->start);
    while (byte < end && markdown_core_is_space_or_tab(*byte)) {
        byte++;
    }
    if (byte == end) {
        return false;
    }
    if ((first < length && input[first] == '+') || runs >= 1) {
        return true;
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
    /* A form that took old rows and then failed read speculatively: the
     * table is read again without them (`redo`) before another form. */
    if (table_parse_grid(source, start, candidate) ||
        (!source->redo && boundary && table_parse_multiline(source, start, candidate)) ||
        (!source->redo && table_parse_simple(source, start, candidate)) ||
        (!source->redo && !boundary && table_parse_multiline(source, start, candidate))) {
        return true;
    }
    return !source->redo && pipe && table_parse_pipe_header(source, start, candidate);
}

static bool table_parse_candidate(table_source *source, size_t start, table_candidate *candidate, bool pipe) {
    if (!table_source_get(source, start) || source->lines[start].indent >= 4) {
        return false;
    }
    size_t runs = table_dash_count(source, start);
    table_source_line *line = &source->lines[start];
    return table_grammar_admits(source->parser, line->data, line->length, line->first, runs, line->line + 1, pipe) &&
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

static void table_fill_cell(table_source *source, markdown_core_node *row, markdown_core_node *node,
                            const table_source_cell *cell, bool blocks, int padding_limit) {
    /* A cell of inline content is complete once it holds its bytes. A cell of
     * blocks holds no inlines: it completes once its blocks are read, after
     * the document's. */
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
    if (blocks) {
        markdown_core_parser_queue_block_input(source->parser, node, row);
    } else {
        markdown_core_parser_complete(source->parser, node, row);
    }
}

/* A pipe header found after a caption is filled as every pipe row is. The
 * iterator that recognized its line (`table_parse_pipe_header`) delimits its
 * cells again, in the order they were added as `row`'s cells, and each holds
 * its bytes. */
static void table_fill_pipe_row(markdown_core_parser *parser, markdown_core_node *row, const table_source_line *line) {
    pipe_row_cursor cells =
        pipe_row_begin((unsigned char *)line->data + line->first, line->input_length - line->first, 0);
    node_cell cell;
    markdown_core_children_cursor at;
    markdown_core_children_seek(&at, row->children, 0);
    for (markdown_core_node *node = markdown_core_children_next(&at);
         node && !parser->error && pipe_row_next(&cells, &cell); node = markdown_core_children_next(&at)) {
        set_cell_content(parser, row, node, &cell, NULL, line->line,
                         markdown_core_parser_line_start(parser, line->line),
                         (bufsize_t)(cell.content.data - line->data));
    }
}

static markdown_core_node *table_child(markdown_core_parser *parser, markdown_core_node *parent,
                                       markdown_core_node_type kind, int first_line, int first_column, int last_line,
                                       int last_column) {
    markdown_core_node *node =
        table_part(parser, parent, kind, markdown_core_parser_source_offset(parser, first_line, first_column));
    if (node) {
        node->where.place.end = (uint32_t)markdown_core_parser_source_end(parser, last_line, last_column);
    }
    return node;
}

/* Where the table's last line ends, and its number: the source's line
 * `last`, unless the fold stepped over the table's last lines. */
static size_t table_candidate_end(table_source *source, const table_candidate *candidate) {
    if (candidate->end) {
        return candidate->end;
    }
    const table_source_line *last = &source->lines[candidate->last];
    return (size_t)markdown_core_parser_source_end(source->parser, last->line, last->length);
}

static int table_candidate_end_line(const table_source *source, const table_candidate *candidate) {
    return candidate->end ? candidate->end_line : source->lines[candidate->last].line;
}

/* A run of old rows the table takes, its first at row `index` of the table:
 * a take holds siblings of one section, so the run splits where a section
 * of the table or of the old table begins. */
static void table_take_part(table_source *source, markdown_core_node *node, const markdown_core_table *table,
                            const table_source_part *part, size_t index) {
    markdown_core_parser *parser = source->parser;
    const markdown_core_node *old = source->old;
    const markdown_core_table *was = old->opaque;
    size_t rows = table->head_count + table->content_count;
    size_t bounds[4] = {table->head_count + part->first - index, rows + part->first - index, was->head_count,
                        was->head_count + was->content_count};
    size_t from = part->first, last = part->first + part->count;
    while (from < last && !parser->error) {
        size_t to = last;
        for (size_t i = 0; i < 4; i++) {
            if (bounds[i] > from && bounds[i] < to) {
                to = bounds[i];
            }
        }
        const markdown_core_node *before = markdown_core_children_at(old->children, to - 1);
        size_t start = from == part->first
                           ? part->start
                           : (size_t)(table_old_lead(source, from) +
                                      markdown_core_children_at(old->children, from)->where.extent.lead);
        size_t end = to == last ? part->end
                                : (size_t)(table_old_lead(source, to - 1) + before->where.extent.lead +
                                           (int64_t)before->where.extent.span);
        markdown_core_parser_take_parts(parser, node, old, from, to - from, start, end);
        from = to;
    }
}

/* THE RECORD OF A GRID TABLE'S FOLD: its walls, its tail, `tail_span`
 * bytes past its last row, and its '=' borders. */
static void table_fold_build(markdown_core_parser *parser, markdown_core_table *table, const table_candidate *candidate,
                             size_t tail_span) {
    const struct markdown_core_table_fold *from = &candidate->fold;
    struct markdown_core_table_fold *fold = markdown_core_alloc(1, sizeof(*fold));
    size_t equals = from->equal_count, width = candidate->column_count;
    if (fold) {
        fold->positions = markdown_core_alloc(from->position_count, sizeof(*fold->positions));
        fold->equal_flows = equals ? markdown_core_alloc(equals * width, sizeof(*fold->equal_flows)) : NULL;
    }
    if (!fold || !fold->positions || (equals && !fold->equal_flows)) {
        table_fold_free(fold);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    memcpy(fold->positions, from->positions, from->position_count * sizeof(*fold->positions));
    fold->form = from->form;
    fold->margin = from->margin;
    fold->position_count = from->position_count;
    fold->tail = table_entry_retain(from->tail);
    fold->tail_span = tail_span;
    fold->equal_count = equals;
    memcpy(fold->equal_rows, from->equal_rows, equals * sizeof(*fold->equal_rows));
    if (equals) {
        memcpy(fold->equal_flows, from->equal_flows, equals * width * sizeof(*fold->equal_flows));
    }
    table->fold = fold;
}

/* The table's rows in source order: the ones its grammar read, and the runs
 * of old rows its fold took (`table_source_part`), ordered by key. */
static markdown_core_node *table_build(table_source *source, markdown_core_node *parent, table_candidate *candidate) {
    markdown_core_parser *parser = source->parser;
    table_source_line *first = &source->lines[candidate->first];
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
    size_t rows = candidate->row_count + candidate->taken_rows;
    /* A grid row's entry, and the table's tail, are made from the states
     * the fold kept, before a cell's content reads another table. */
    for (size_t r = 0; r < candidate->row_count && !parser->error; r++) {
        if (candidate->rows[r].point != SIZE_MAX) {
            candidate->rows[r].entry = table_point_entry(source, candidate->rows[r].point);
        }
    }
    if (candidate->fold.form == TABLE_FORM_GRID && !parser->error) {
        candidate->fold.tail = table_point_entry(source, candidate->tail);
    }
    table->column_count = candidate->column_count;
    table->head_count = candidate->head_count;
    table->foot_count = candidate->foot_count;
    table->content_count = rows - table->head_count - table->foot_count;
    /* The table and each of its rows begin at the margin on their first line;
     * the table ends where its maker says, once it has closed it. */
    node->where.place.start = (uint32_t)markdown_core_parser_source_offset(
        parser, first->line, table_margin_byte(first, candidate->margin) + 1);
    size_t last_end = 0, index = 0;
    for (size_t r = 0, p = 0; (r < candidate->row_count || p < candidate->part_count) && !parser->error;) {
        if (p < candidate->part_count &&
            (r == candidate->row_count || candidate->parts[p].key < candidate->rows[r].key)) {
            const table_source_part *part = &candidate->parts[p++];
            table_take_part(source, node, table, part, index);
            index += part->count;
            last_end = part->end;
            continue;
        }
        index++;
        table_source_row *row = &candidate->rows[r++];
        table_source_line *begin = &source->lines[row->first], *end = &source->lines[row->last];
        markdown_core_node *row_node =
            table_child(parser, node, MARKDOWN_CORE_NODE_TABLE_ROW, begin->line,
                        table_margin_byte(begin, candidate->margin) + 1, end->line, end->length);
        if (!row_node) {
            break;
        }
        last_end = row_node->where.place.end;
        /* A row of a fold carries how far past its end the fold read before
         * it made the row; a grid row, its entry; and a simple or multiline
         * row, its lines at the table's margin. */
        row_node->opaque = table_entry_retain(row->entry);
        if (candidate->fold.form && parser->block_root == parser->root) {
            row_node->reach = row->reads > last_end ? (uint32_t)(row->reads - last_end) : 0;
        }
        if (candidate->fold.form == TABLE_FORM_SIMPLE || candidate->fold.form == TABLE_FORM_MULTILINE) {
            uint32_t tally = 0;
            for (size_t i = row->first; i <= row->last; i++) {
                tally += source->lines[i].indent == candidate->margin;
            }
            row_node->tally = tally;
        }
        for (size_t j = 0; j < row->count && !parser->error; j++) {
            table_source_cell *cell = &candidate->cells[row->cell + j];
            size_t last_line = cell->last < cell->first ? cell->first : cell->last;
            markdown_core_node *cell_node =
                table_child(parser, row_node, MARKDOWN_CORE_NODE_TABLE_CELL, source->lines[cell->first].line,
                            cell->start_column, source->lines[last_line].line, cell->end_column);
            if (!cell_node) {
                break;
            }
            cell_node->as.table_cell->rowspan = cell->rowspan;
            cell_node->as.table_cell->colspan = cell->colspan;
            if (!candidate->pipe) {
                table_fill_cell(source, row_node, cell_node, cell, candidate->block_content, candidate->padding_limit);
            }
        }
        if (candidate->pipe && row_node && !parser->error) {
            table_fill_pipe_row(parser, row_node, begin);
        }
        markdown_core_parser_complete(parser, row_node, node);
    }
    if (candidate->fold.form && !parser->error) {
        size_t end = table_candidate_end(source, candidate);
        table_fold_build(parser, table, candidate, end > last_end ? end - last_end : 0);
    }
    return node;
}

static markdown_core_node *table_caption_build(table_source *source, size_t last, int content) {
    table_source_line *first = &source->lines[0], *end = &source->lines[last];
    markdown_core_node *node = table_child(source->parser, NULL, MARKDOWN_CORE_NODE_TABLE_CAPTION, first->line,
                                           first->first + 1, end->line, end->length);
    if (!node) {
        return NULL;
    }
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
                                       int indent) {
    markdown_core_parser *parser = lookahead->parser;
    table_workspace *workspace = table->state;
    table_source source = {
        .parser = parser, .workspace = workspace, .lookahead = *lookahead, .lines = workspace->lines};
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
                                                       .line = source.lookahead.line - 1})) {
        matched = table_after_caption(&source, &last, candidate, true);
    }
    table_candidate_reset(candidate);
    table_source_end(&source);
    return matched;
}

/* Captions are a separate entry grammar; the body shares candidate admission
 * with the definition-precedence query and every other table producer. */
static bool table_open_admits(markdown_core_parser *parser, const unsigned char *input, int length) {
    bufsize_t trimmed = length;
    while (trimmed > 0 && (input[trimmed - 1] == '\n' || input[trimmed - 1] == '\r')) {
        trimmed--;
    }
    if (table_caption_start(input, trimmed, parser->first_nonspace, parser->indent) >= 0) {
        return true;
    }
    return table_grammar_admits(parser, input, trimmed, parser->first_nonspace,
                                table_dash_count_raw(input, parser->offset, trimmed), parser->line_number + 1, false);
}

static markdown_core_node *table_try_open(table_workspace *workspace, markdown_core_parser *parser,
                                          markdown_core_node *parent, unsigned char *input, int length) {
    if (parser->indent > 3 || parser->blank || parent->kind == MARKDOWN_CORE_NODE_TABLE ||
        parent->kind == MARKDOWN_CORE_NODE_TABLE_ROW) {
        return NULL;
    }
    if (parent->kind == MARKDOWN_CORE_NODE_PARAGRAPH) {
        if (table_open_admits(parser, input, length)) {
            markdown_core_parser_refuse(parser);
        }
        return NULL;
    }
    /* Every opening grammar needs a later physical line. At EOF only an
     * existing eligible table can claim a trailing caption. */
    if (parser->lookahead_cursor == parser->input_text.size &&
        (!parent->children || markdown_core_node_last_child(parent)->kind != MARKDOWN_CORE_NODE_TABLE)) {
        return NULL;
    }
    if (!table_open_admits(parser, input, length)) {
        return NULL;
    }
    /* A grid table that starts where an old one did reads its rows against
     * it (E6). A fold that stepped over the old rows and then failed reads
     * the lines again without it, from this line. */
    int64_t old_start = 0;
    const markdown_core_node *old = markdown_core_parser_old_block(
        parser, parent, MARKDOWN_CORE_NODE_TABLE, markdown_core_parser_line_start(parser, parser->line_number),
        (size_t)markdown_core_parser_source_offset(parser, parser->line_number, parser->first_nonspace + 1),
        &old_start);
    if (old && (!old->opaque || !((const markdown_core_table *)old->opaque)->fold)) {
        old = NULL;
    }
    table_source source;
    table_candidate *candidate = &workspace->candidate;
    markdown_core_node *result = NULL;
    int caption;
    size_t caption_last;
    bool trailing, matched;
    markdown_core_node *preceding = markdown_core_node_last_child(parent);
    for (;;) {
        source = (table_source){
            .parser = parser, .workspace = workspace, .lines = workspace->lines, .old = old, .old_start = old_start};
        if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_TABLE, &source.lookahead)) {
            return NULL;
        }
        if (!table_source_push(&source, (table_source_line){.data = input,
                                                            .length = length,
                                                            .offset = parser->offset,
                                                            .first = parser->first_nonspace,
                                                            .first_column = parser->first_nonspace_column,
                                                            .indent = parser->indent,
                                                            .line = parser->line_number})) {
            goto done;
        }
        caption = table_caption_start(source.lines[0].data, source.lines[0].length, source.lines[0].first,
                                      source.lines[0].indent);
        caption_last = 0;
        trailing = caption >= 0 && preceding && preceding->kind == MARKDOWN_CORE_NODE_TABLE && preceding->opaque &&
                   !((markdown_core_table *)preceding->opaque)->caption;
        if (caption >= 0) {
            matched = table_after_caption(&source, &caption_last, candidate, !trailing);
        } else {
            matched = table_parse_admitted_candidate(&source, 0, candidate, false);
        }
        if (matched || !source.redo || parser->error) {
            break;
        }
        table_source_end(&source);
        markdown_core_parser_unread_lines(parser);
        old = NULL;
    }
    markdown_core_parser_lookahead_end(&source.lookahead);
    if (parser->error || (!matched && !trailing)) {
        goto done;
    }
    markdown_core_parser_finalize_to(parser, parent);
    if (parser->error) {
        goto done;
    }
    if (trailing) {
        /* The table above was complete when it closed: its caption is a field
         * that joins it now, and is measured in it. */
        result = markdown_core_parser_write_closed(
            parser, parent,
            (size_t)markdown_core_parser_source_end(parser, source.lines[caption_last].line,
                                                    source.lines[caption_last].length));
        table_candidate_reset(candidate);
        markdown_core_node *caption_node = table_caption_build(&source, caption_last, caption);
        ((markdown_core_table *)result->opaque)->caption = caption_node;
        if (caption_node) {
            markdown_core_parser_complete_field(parser, caption_node, result);
            markdown_core_parser_publish_field(parser, result, caption_node);
        }
        parser->claimed = true;
        parser->claimed_line = source.lines[caption_last].line;
        parser->claimed_last_end = result->where.place.end;
    } else {
        result = table_build(&source, parent, candidate);
        if (result && result->opaque && caption >= 0) {
            markdown_core_node *caption_node = table_caption_build(&source, caption_last, caption);
            ((markdown_core_table *)result->opaque)->caption = caption_node;
            result->where.place.start =
                (uint32_t)markdown_core_parser_source_offset(parser, source.lines[0].line, source.lines[0].first + 1);
            if (caption_node) {
                markdown_core_parser_complete_field(parser, caption_node, result);
            }
        }
        /* A table without pipes is complete once built: no later line reads
         * into it. It ends on its last line. */
        if (result && !candidate->pipe) {
            markdown_core_block_finalize(parser, result);
        }
        if (result) {
            size_t end = table_candidate_end(&source, candidate);
            result->where.place.end = (uint32_t)end;
            parser->claimed = true;
            parser->claimed_line = table_candidate_end_line(&source, candidate);
            parser->claimed_last_end = (bufsize_t)end;
        }
    }
done:
    table_candidate_reset(candidate);
    table_source_end(&source);
    return result;
}

/* A block-only element: no byte ends a text run for it, no byte is offered to an
 * inline hook it does not have, and no byte is transparent to flanking. */
static markdown_core_node *try_interrupting_block(const markdown_core_element_instance *self,
                                                  markdown_core_parser *parser, markdown_core_node *node,
                                                  markdown_core_chunk *input, bool lazy) {
    if (parser->indent >= 4 || input->data[parser->first_nonspace] != '-') {
        return NULL;
    }
    if (lazy || node->kind == MARKDOWN_CORE_NODE_PARAGRAPH) {
        if (table_open_admits(parser, input->data, input->len)) {
            markdown_core_parser_refuse(parser);
        }
        return NULL;
    }
    return table_try_open(self->state, parser, node, input->data, input->len);
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
    markdown_core_free(workspace->candidate.parts);
    markdown_core_free(workspace->candidate.fold.equal_flows);
    markdown_core_free(workspace->grid_parents);
    markdown_core_free(workspace->grid_sizes);
    markdown_core_free(workspace->grid_classes);
    markdown_core_free(workspace->fold_positions);
    markdown_core_free(workspace->lattice_edges);
    markdown_core_free(workspace->lattice_walls);
    markdown_core_free(workspace->lattice_faces);
    markdown_core_free(workspace->lattice_order);
    markdown_core_free(workspace->lattice_losers);
    markdown_core_free(workspace->patches);
    markdown_core_free(workspace->bands);
    markdown_core_free(workspace->points);
    markdown_core_free(workspace->point_values);
    memset(workspace, 0, sizeof(*workspace));
}

/* A TABLE WITHOUT A CAPTION may take a trailing one from the lines after
 * it, a later line's write (markdown_core_parser_write_closed): no run of
 * taken blocks ends at it. */
static markdown_core_finish_result finish_step(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node *node, markdown_core_event_type event,
                                               markdown_core_node *parent, void **state) {
    (void)self;
    (void)parser;
    (void)event;
    (void)parent;
    (void)state;
    if (!node->opaque || !((markdown_core_table *)node->opaque)->caption) {
        node->flags |= MARKDOWN_CORE_NODE__EXIT_FRAGILE;
    }
    return MARKDOWN_CORE_FINISH_CONTINUE;
}
static const markdown_core_node_type TABLE_EXIT_KINDS[] = {MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_NONE};

/* A PIPE TABLE'S ROWS ARE READ AS ITS HEADER DECIDED: as many cells as its
 * columns. */
static bool carries_as(const markdown_core_node *node, const markdown_core_node *old) {
    return ((const markdown_core_table *)node->opaque)->column_count ==
           ((const markdown_core_table *)old->opaque)->column_count;
}

/* A PIPE TABLE'S LATER ROWS JOIN ITS BODY, the relation after its head
 * rows (canonical-ast.md). */
static void children_relation(const markdown_core_node *node, const markdown_core_node *old,
                              markdown_core_children_relation *relation) {
    if (old && node->kind == MARKDOWN_CORE_NODE_TABLE) {
        const markdown_core_table *table = old->opaque;
        relation->first = table->head_count;
        relation->end = table->head_count + table->content_count;
    }
}

/* THE STATE A PIPE TABLE'S ROWS CHANGE (E3): the cells completed so far,
 * past MAX_AUTOCOMPLETED_CELLS of which a row is refused, and which each row
 * tallies its own of. A run of rows is admitted when its last row is, with
 * the cells the rows before it in the run completed. */
static bool take_children(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, const markdown_core_node *old, size_t first, size_t count,
                          uint32_t tally) {
    (void)self;
    (void)parser;
    markdown_core_table *table = node->opaque;
    const markdown_core_node *last = markdown_core_children_at(old->children, first + count - 1);
    if (table->autocompleted_cells + tally - last->tally > MAX_AUTOCOMPLETED_CELLS) {
        return false;
    }
    table->content_count += count;
    table->autocompleted_cells += tally;
    return true;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_TABLE = {
    .finish_step = finish_step,
    .finish_exit_kinds = TABLE_EXIT_KINDS,
    .carries_as = carries_as,
    .children_relation = children_relation,
    .take_children = take_children,
    .take_record = take_record,
    .peers = TABLE_PEERS,
    .dispose_parser = dispose_parser,
    .state_size = sizeof(table_workspace),

    .try_interrupting_block = try_interrupting_block,
    /* A delimiter row leading a dash-led table. */
    .interrupt_block_gate = {.bytes = "-"},

    .name = "table",
    .last_block_matches = matches,
    .maximum_block_indent = 3,
    .try_opening_block = try_opening_table_block,
    .containment_kinds = containment_kinds,
    .contains_inlines_func = contains_inlines,
    .opaque_alloc_func = opaque_alloc,
    .opaque_free_func = opaque_free,
    .visit_owned_subtrees_func = visit_owned_subtrees,
};
