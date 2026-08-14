#include <markdown-core-extension-api.h>
#include <parser.h>
#include <assert.h>
#include <string.h>

#include "ext_scanners.h"
#include "table.h"
#include "markdown-core-extensions.h"
#include "extension.h"

// Limit to prevent a malicious input from causing a denial of service.
#define MAX_AUTOCOMPLETED_CELLS 0x80000

// Extension-owned node flag; the first (and only) compile-time extension bit.
#define MARKDOWN_CORE_NODE__TABLE_VISITED ((markdown_core_node_internal_flags)MARKDOWN_CORE_NODE__EXTENSION_FIRST)

typedef struct {
    markdown_core_strbuf *buf;
    int start_offset, end_offset, internal_offset;
} node_cell;

typedef struct {
    uint16_t n_columns;
    int paragraph_offset;
    node_cell *cells;
} table_row;

typedef struct {
    uint16_t n_columns;
    uint8_t *alignments;
    int n_rows;
    int n_nonempty_cells;
} node_table;

typedef struct {
    bool is_header;
} node_table_row;

static void free_table_cell(markdown_core_mem *mem, node_cell *cell) {
    markdown_core_strbuf_free((markdown_core_strbuf *)cell->buf);
    mem->free(mem, cell->buf);
}

static void free_row_cells(markdown_core_mem *mem, table_row *row) {
    while (row->n_columns > 0) {
        free_table_cell(mem, &row->cells[--row->n_columns]);
    }
    mem->free(mem, row->cells);
    row->cells = NULL;
}

static void free_table_row(markdown_core_mem *mem, table_row *row) {
    if (!row) {
        return;
    }

    free_row_cells(mem, row);
    mem->free(mem, row);
}

static void free_node_table(markdown_core_mem *mem, void *ptr) {
    node_table *t = (node_table *)ptr;
    mem->free(mem, t->alignments);
    mem->free(mem, t);
}

static void free_node_table_row(markdown_core_mem *mem, void *ptr) { mem->free(mem, ptr); }

static int get_n_table_columns(markdown_core_node *node) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !node->as.opaque) {
        return -1;
    }

    return (int)((node_table *)node->as.opaque)->n_columns;
}

static int set_n_table_columns(markdown_core_node *node, uint16_t n_columns) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !node->as.opaque) {
        return 0;
    }

    ((node_table *)node->as.opaque)->n_columns = n_columns;
    return 1;
}

// Increment the number of rows in the table. Also update n_nonempty_cells,
// which keeps track of the number of cells which were parsed from the
// input file. (If one of the rows is too short, then the trailing cells
// are autocompleted. Autocompleted cells are not counted in n_nonempty_cells.)
// The purpose of this is to prevent a malicious input from generating a very
// large number of autocompleted cells, which could cause a denial of service
// vulnerability.
static int incr_table_row_count(markdown_core_node *node, int i) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !node->as.opaque) {
        return 0;
    }

    ((node_table *)node->as.opaque)->n_rows++;
    ((node_table *)node->as.opaque)->n_nonempty_cells += i;
    return 1;
}

// Calculate the number of autocompleted cells.
static int get_n_autocompleted_cells(markdown_core_node *node) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !node->as.opaque) {
        return 0;
    }

    const node_table *nt = (node_table *)node->as.opaque;
    return (nt->n_columns * nt->n_rows) - nt->n_nonempty_cells;
}

static int set_table_alignments(markdown_core_node *node, uint8_t *alignments) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !node->as.opaque) {
        return 0;
    }

    ((node_table *)node->as.opaque)->alignments = alignments;
    return 1;
}

static int set_cell_index(markdown_core_node *node, int i) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE_CELL) {
        return 0;
    }

    node->as.cell_index = i;
    return 1;
}

/* Captures the pipes a scanned row spelled (11.1: a row's separators are
 * the TableRow's own marker material), derived from the row's final cells:
 * each cell whose walk-back stopped against a `|` names the pipe before
 * it, and a trailing pipe sits just past the last cell's raw extent —
 * `scan_table_cell_end` consumed it after that cell, so no cell names it.
 * Cells discarded into `paragraph_offset` never reach here, which is what
 * keeps a mid-buffer restart from spelling ghost pipes.
 *
 * Coordinates map through a line mark: a data row scans the current line
 * at first_nonspace, described by a synthetic mark whose byte_offset is
 * that base (origin 0, no pad); the look-back header row scans the
 * paragraph's content buffer, whose last line began at the mark the
 * paragraph recorded — including the stand-in pad a mid-tab lazy
 * continuation buffered, which markdown_core_line_mark_extent undoes. A
 * pipe or backslash is never one of the pad's stand-in spaces, so every
 * extent mapped here starts past the pad. The mark's `line` is the
 * absolute line the row was spelled on. */
static void capture_row_pipes(
    markdown_core_parser *parser,
    markdown_core_node *row_node,
    const unsigned char *string,
    const table_row *row,
    const struct markdown_core_line_mark *mark
) {
    uint16_t i;
    const node_cell *last;
    markdown_core_bufsize origin = mark->content_offset;
    markdown_core_bufsize column;

    for (i = 0; i < row->n_columns; i++) {
        const node_cell *cell = &row->cells[i];
        /* The walk-back that produced start_offset stops only at the row
         * origin or just past a `|`, so start_offset > origin already
         * proves the preceding byte is the pipe. */
        if ((markdown_core_bufsize)cell->start_offset > origin) {
            markdown_core_line_mark_extent(
                mark,
                (markdown_core_bufsize)cell->start_offset - 1,
                (markdown_core_bufsize)cell->start_offset,
                &column
            );
            markdown_core_parser_capture_marker_at(
                parser,
                row_node,
                MARKDOWN_CORE_CONCRETE_TABLE_PIPE,
                mark->line,
                column,
                1
            );
        }
    }
    last = &row->cells[row->n_columns - 1];
    if (string[last->end_offset + 1] == '|') {
        markdown_core_line_mark_extent(
            mark,
            (markdown_core_bufsize)last->end_offset + 1,
            (markdown_core_bufsize)last->end_offset + 2,
            &column
        );
        markdown_core_parser_capture_marker_at(
            parser,
            row_node,
            MARKDOWN_CORE_CONCRETE_TABLE_PIPE,
            mark->line,
            column,
            1
        );
    }
}

/* Captures the backslash of every `\|` unescape_pipes collapses out of one
 * cell's rebuilt buffer. A plain adjacent-pair scan equals that function's
 * skip-ahead traversal: a collapsed pair's second byte is a `|`, which can
 * never begin another pair, so no pair unescape_pipes skips over could
 * have matched here. The record rides the TableCell — the cell owns the
 * source backing its inline sequence, and the rebuilt buffer the inline
 * records index no longer holds the backslash. */
static void capture_cell_escapes(
    markdown_core_parser *parser,
    markdown_core_node *cell_node,
    const unsigned char *string,
    const node_cell *cell,
    const struct markdown_core_line_mark *mark
) {
    int r;
    markdown_core_bufsize column;

    for (r = cell->start_offset + cell->internal_offset; r < cell->end_offset; r++) {
        if (string[r] == '\\' && string[r + 1] == '|') {
            markdown_core_line_mark_extent(mark, (markdown_core_bufsize)r, (markdown_core_bufsize)r + 1, &column);
            markdown_core_parser_capture_marker_at(
                parser,
                cell_node,
                MARKDOWN_CORE_CONCRETE_TABLE_CELL_ESCAPE,
                mark->line,
                column,
                1
            );
        }
    }
}

static markdown_core_strbuf *unescape_pipes(markdown_core_mem *mem, unsigned char *string, markdown_core_bufsize len) {
    markdown_core_strbuf *res = (markdown_core_strbuf *)mem->calloc(mem, 1, sizeof(markdown_core_strbuf));
    markdown_core_bufsize r, w;

    if (!res) {
        return NULL;
    }
    markdown_core_strbuf_init(mem, res, len + 1);
    markdown_core_strbuf_put(res, string, len);
    markdown_core_strbuf_putc(res, '\0');

    if (res->oom) {
        return res;
    }

    for (r = 0, w = 0; r < len; ++r) {
        if (res->ptr[r] == '\\' && res->ptr[r + 1] == '|') {
            r++;
        }

        res->ptr[w++] = res->ptr[r];
    }

    markdown_core_strbuf_truncate(res, w);

    return res;
}

// Adds a new cell to the end of the row. A pointer to the new cell is returned
// for the caller to initialize.
static node_cell *append_row_cell(markdown_core_mem *mem, table_row *row, int *oom) {
    const uint32_t n_columns = row->n_columns + 1;
    // realloc when n_columns is a power of 2
    if ((n_columns & (n_columns - 1)) == 0) {
        // make sure we never wrap row->n_columns
        // offset will != len and our exit will clean up as intended
        if (n_columns > UINT16_MAX) {
            return NULL;
        }
        // Use realloc to double the size of the buffer.
        node_cell *cells = (node_cell *)mem->realloc(mem, row->cells, (2 * n_columns - 1) * sizeof(node_cell));
        if (!cells) {
            /* Allocation loss, not the column limit: report it so the parse
             * fails instead of silently degrading the table to a paragraph. */
            *oom = 1;
            return NULL;
        }
        row->cells = cells;
    }
    row->n_columns = (uint16_t)n_columns;
    return &row->cells[n_columns - 1];
}

static bool scan_row_from_string(
    markdown_core_parser *parser,
    unsigned char *string,
    int len,
    table_row *row,
    bool materialize
) {
    // Scans a single table row. It has the following form:
    // `delim? table_cell (delim table_cell)* delim? newline`
    // Note that cells are allowed to be empty.
    //
    // From the GitHub-flavored Markdown specification:
    //
    // > Each row consists of cells containing arbitrary text, in which inlines
    // > are parsed, separated by pipes (|). A leading and trailing pipe is also
    // > recommended for clarity of reading, and if there’s otherwise parsing
    // > ambiguity.

    markdown_core_bufsize cell_matched = 1, pipe_matched = 1, offset;
    int expect_more_cells = 1;
    int row_end_offset = 0;

    // Scan past the (optional) leading pipe.
    offset = scan_table_cell_end(string, len, 0);

    // Parse the cells of the row. Stop if we reach the end of the input, or if we
    // cannot detect any more cells.
    while (offset < len && expect_more_cells) {
        cell_matched = scan_table_cell(string, len, offset);
        pipe_matched = scan_table_cell_end(string, len, offset + cell_matched);

        if (cell_matched || pipe_matched) {
            // We are guaranteed to have a cell, since (1) either we found some
            // content and cell_matched, or (2) we found an empty cell followed by a
            // pipe.
            if (materialize) {
                markdown_core_strbuf *cell_buf = unescape_pipes(parser->mem, string + offset, cell_matched);
                if (!cell_buf) {
                    parser->oom = true;
                    return false;
                }
                if (cell_buf->oom) {
                    parser->oom = true;
                    markdown_core_strbuf_free(cell_buf);
                    parser->mem->free(parser->mem, cell_buf);
                    return false;
                }
                markdown_core_strbuf_trim(cell_buf);

                int cell_oom = 0;
                node_cell *cell = append_row_cell(parser->mem, row, &cell_oom);
                if (cell_oom) {
                    parser->oom = true;
                }
                if (!cell) {
                    markdown_core_strbuf_free(cell_buf);
                    parser->mem->free(parser->mem, cell_buf);
                    return false;
                }
                cell->buf = cell_buf;
                cell->start_offset = offset;
                cell->end_offset = offset + cell_matched - 1;
                cell->internal_offset = 0;

                while (cell->start_offset > row->paragraph_offset && string[cell->start_offset - 1] != '|') {
                    --cell->start_offset;
                    ++cell->internal_offset;
                }
            } else {
                if (row->n_columns == UINT16_MAX) {
                    return false;
                }
                row->n_columns++;
            }
        }

        offset += cell_matched + pipe_matched;

        if (pipe_matched) {
            expect_more_cells = 1;
        } else {
            // We've scanned the last cell. Check if we have reached the end of the row
            row_end_offset = scan_table_row_end(string, len, offset);
            offset += row_end_offset;

            // If the end of the row is not the end of the input,
            // the row is not a real row but potentially part of the paragraph
            // preceding the table.
            if (row_end_offset && offset != len) {
                row->paragraph_offset = offset;

                if (materialize) {
                    free_row_cells(parser->mem, row);
                } else {
                    row->n_columns = 0;
                }

                // Scan past the (optional) leading pipe.
                offset += scan_table_cell_end(string, len, offset);

                expect_more_cells = 1;
            } else {
                expect_more_cells = 0;
            }
        }
    }

    return offset == len && row->n_columns != 0;
}

static table_row *row_from_string(markdown_core_parser *parser, unsigned char *string, int len) {
    table_row *row = (table_row *)parser->mem->calloc(parser->mem, 1, sizeof(table_row));
    if (!row) {
        parser->oom = true;
        return NULL;
    }

    if (!scan_row_from_string(parser, string, len, row, true)) {
        free_table_row(parser->mem, row);
        row = NULL;
    }

    return row;
}

/* Re-emits the paragraph lines that preceded the header row. The prefix is
 * ordinary paragraph text, so it keeps its authored spelling — its escapes
 * belong to the inline pass, which records them; pipe collapsing is cell
 * treatment. Upstream cmark-gfm pipe-unescapes this prefix too, so its
 * double processing renders a lead's \\| as `|` where a plain paragraph
 * renders `\|` — a registered deliberate divergence (micromark reads the
 * prefix as a plain paragraph, as this does now). The node is positioned
 * from the paragraph's line marks exactly the way a harvested reference
 * definition is: first byte at the original paragraph's start, last
 * surviving byte mapped through the mark of the line that produced it. */
static void try_inserting_table_header_paragraph(
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *parent_string,
    int paragraph_offset
) {
    markdown_core_node *paragraph;
    markdown_core_strbuf raw;
    markdown_core_bufsize last = (markdown_core_bufsize)paragraph_offset;
    size_t cursor = 0;

    paragraph = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, parser->mem);
    if (!paragraph) {
        parser->oom = true;
        return;
    }

    markdown_core_strbuf_init(parser->mem, &raw, (markdown_core_bufsize)paragraph_offset + 1);
    markdown_core_strbuf_put(&raw, parent_string, (markdown_core_bufsize)paragraph_offset);
    markdown_core_strbuf_trim(&raw);
    markdown_core_strbuf_putc(&raw, '\0');
    if (raw.oom) {
        parser->oom = true;
        markdown_core_strbuf_free(&raw);
        markdown_core_node_free(paragraph);
        return;
    }
    markdown_core_node_set_string_content(paragraph, (char *)raw.ptr);
    markdown_core_strbuf_free(&raw);
    /* set_string_content reports nothing; its strbuf carries the loss. A
     * silently empty lead would commit a thinner tree than the same input
     * parses without the failure — exactly what the OOM sweep compares. */
    if (paragraph->content.oom) {
        parser->oom = true;
        markdown_core_node_free(paragraph);
        return;
    }

    /* The prefix holds at least one non-blank line (a blank line would have
     * closed the paragraph before the header ever buffered), so the
     * walk-back stops at that line's last real byte before it can run off
     * the front, and the header line's mark — whose content_offset is
     * paragraph_offset, past every prefix byte — bounds the cursor walk
     * before the mark count can. Content positions are in tab-expanded
     * column space, which is what node positions carry. */
    while (parent_string[last - 1] == '\n' || parent_string[last - 1] == ' ' || parent_string[last - 1] == '\t') {
        last--;
    }
    while (parser->line_marks[cursor + 1].content_offset <= last - 1) {
        cursor++;
    }
    paragraph->start_line = parent_container->start_line;
    paragraph->start_column = parent_container->start_column;
    paragraph->end_line = parser->line_marks[cursor].line;
    /* Node columns are byte-based (start_column is first_nonspace + 1, a
     * finalized end_column is the line's byte length), while the marks'
     * `column` tab-expands — so the last byte maps back through the
     * byte-exact extent helper, pad included, not through the expanded
     * column. */
    {
        markdown_core_bufsize end_byte;
        markdown_core_line_mark_extent(&parser->line_marks[cursor], last - 1, last, &end_byte);
        paragraph->end_column = (int)end_byte + 1;
    }

    /* The retyped table sits in a container that holds paragraphs — it was
     * one — so the checked insert's refusal arms are unreachable here; the
     * unchecked variant asserts exactly that in debug builds. */
    markdown_core_node_insert_before_unchecked(parent_container, paragraph);
}

static markdown_core_node *try_opening_table_header(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
) {
    markdown_core_node *table_header;
    table_row *header_row = NULL;
    table_row *delimiter_row = NULL;
    node_table_row *ntr;
    const char *parent_string;
    uint16_t i;

    if (parent_container->flags & MARKDOWN_CORE_NODE__TABLE_VISITED) {
        return NULL;
    }

    if (!scan_table_start(input, len, markdown_core_parser_get_first_nonspace(parser))) {
        return NULL;
    }

    // Since scan_table_start was successful, we must have a delimiter row.
    delimiter_row = row_from_string(
        parser,
        input + markdown_core_parser_get_first_nonspace(parser),
        len - markdown_core_parser_get_first_nonspace(parser)
    );
    // assert may be optimized out, don't rely on it for security boundaries
    if (!delimiter_row) {
        return NULL;
    }

    assert(delimiter_row);

    // Check for a matching header row. We call `row_from_string` with the entire
    // (potentially long) parent container as input, but this should be safe since
    // `row_from_string` bails out early if it does not find a row.
    parent_string = markdown_core_node_get_string_content(parent_container);
    header_row = row_from_string(parser, (unsigned char *)parent_string, (int)strlen(parent_string));
    if (!header_row || header_row->n_columns != delimiter_row->n_columns) {
        free_table_row(parser->mem, delimiter_row);
        free_table_row(parser->mem, header_row);
        parent_container->flags |= MARKDOWN_CORE_NODE__TABLE_VISITED;
        return NULL;
    }

    if (!markdown_core_node_set_type(parent_container, MARKDOWN_CORE_NODE_TABLE)) {
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return NULL;
    }

    if (header_row->paragraph_offset) {
        try_inserting_table_header_paragraph(
            parser,
            parent_container,
            (unsigned char *)parent_string,
            header_row->paragraph_offset
        );
        /* The prefix lines belong to the split-off paragraph now, so the
         * retyped node's span starts where its header row was spelled —
         * the buffer's last line, whose mark carries the tab-expanded
         * column node positions use. */
        parent_container->start_line = parser->line_marks[parser->line_mark_count - 1].line;
        /* The byte column, not the mark's tab-expanded one: every node
         * position in the engine is byte-based, and the two disagree the
         * moment a tab precedes the row. */
        parent_container->start_column = (int)parser->line_marks[parser->line_mark_count - 1].byte_offset + 1;
    }

    /* The paragraph is already rewritten into a table node here.  On
     * allocation failure the half-converted node stays behind with a NULL
     * payload -- every table helper tolerates that -- and the sticky flag
     * makes the parse fail, so nothing downstream trusts the node. */
    markdown_core_node_set_extension(parent_container, self);
    parent_container->as.opaque = parser->mem->calloc(parser->mem, 1, sizeof(node_table));
    if (!parent_container->as.opaque) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return NULL;
    }
    set_n_table_columns(parent_container, header_row->n_columns);

    // allocate alignments based on delimiter_row->n_columns
    // since we populate the alignments array based on delimiter_row->cells
    uint8_t *alignments = (uint8_t *)parser->mem->calloc(parser->mem, delimiter_row->n_columns, sizeof(uint8_t));
    if (!alignments) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return NULL;
    }
    for (i = 0; i < delimiter_row->n_columns; ++i) {
        node_cell *node = &delimiter_row->cells[i];
        bool left = node->buf->ptr[0] == ':', right = node->buf->ptr[node->buf->size - 1] == ':';

        if (left && right) {
            alignments[i] = 'c';
        } else if (left) {
            alignments[i] = 'l';
        } else if (right) {
            alignments[i] = 'r';
        }
    }
    set_table_alignments(parent_container, alignments);

    table_header = markdown_core_parser_add_child(
        parser,
        parent_container,
        MARKDOWN_CORE_NODE_TABLE_ROW,
        parent_container->start_column
    );
    if (!table_header) {
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return NULL;
    }
    markdown_core_node_set_extension(table_header, self);
    /* Buffer offsets are content-absolute; the header row's own bytes begin
     * at its mark's content_offset, which is 0 exactly when the paragraph
     * was single-line. Subtracting it keeps the direct case byte-identical
     * and puts the split case's columns on the header line. */
    table_header->end_column =
        parent_container->start_column +
        (int)(strlen(parent_string) - parser->line_marks[parser->line_mark_count - 1].content_offset) - 2;
    table_header->start_line = table_header->end_line = parent_container->start_line;

    table_header->as.opaque = ntr = (node_table_row *)parser->mem->calloc(parser->mem, 1, sizeof(node_table_row));
    if (!ntr) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    ntr->is_header = true;

    /* Concrete capture. The header row was recovered from the paragraph's
     * accumulated content, so its buffer offsets map to normalized-line
     * columns through the mark the paragraph recorded when that line was
     * appended: the header row is by construction the buffer's last line,
     * and line_mark_count is nonzero here because a lost mark set
     * parser->oom, after which S_process_line refuses every later line —
     * this delimiter line included. */
    {
        const struct markdown_core_line_mark *mark = &parser->line_marks[parser->line_mark_count - 1];
        markdown_core_bufsize delimiter_end = (markdown_core_bufsize)len;
        /* Trim the EOL and trailing whitespace off the record's extent.
         * scan_table_start matched a marker byte at first_nonspace, so the
         * walk stops there at the latest and needs no bound of its own; a
         * '\r' never appears — the feed splits lines on either terminator
         * and S_process_line re-appends only '\n'. */
        while (input[delimiter_end - 1] == '\n' || input[delimiter_end - 1] == ' ' ||
               input[delimiter_end - 1] == '\t') {
            delimiter_end--;
        }
        markdown_core_parser_capture_marker(
            parser,
            parent_container,
            MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW,
            (markdown_core_bufsize)markdown_core_parser_get_first_nonspace(parser),
            delimiter_end - (markdown_core_bufsize)markdown_core_parser_get_first_nonspace(parser)
        );
        capture_row_pipes(parser, table_header, (const unsigned char *)parent_string, header_row, mark);

        for (i = 0; i < header_row->n_columns; ++i) {
            node_cell *cell = &header_row->cells[i];
            markdown_core_node *header_cell = markdown_core_parser_add_child(
                parser,
                table_header,
                MARKDOWN_CORE_NODE_TABLE_CELL,
                parent_container->start_column + cell->start_offset - (int)mark->content_offset
            );
            if (!header_cell) {
                break;
            }
            header_cell->start_line = header_cell->end_line = parent_container->start_line;
            header_cell->internal_offset = cell->internal_offset;
            header_cell->end_column = parent_container->start_column + cell->end_offset - (int)mark->content_offset;
            markdown_core_node_set_string_content(header_cell, (char *)cell->buf->ptr);
            markdown_core_node_set_extension(header_cell, self);
            set_cell_index(header_cell, i);
            capture_cell_escapes(parser, header_cell, (const unsigned char *)parent_string, cell, mark);
        }
    }

    incr_table_row_count(parent_container, i);

    markdown_core_parser_advance_offset(
        parser,
        (char *)input,
        (int)strlen((char *)input) - 1 - markdown_core_parser_get_offset(parser),
        false
    );

    free_table_row(parser->mem, header_row);
    free_table_row(parser->mem, delimiter_row);
    /* The only claiming return: the paragraph node was retyped in place, so
     * the container the caller already holds *is* the table. Every path above
     * returns NULL, because returning the container unchanged would tell the
     * dispatcher a block had been opened when none had. */
    return parent_container;
}

static markdown_core_node *try_opening_table_row(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
) {
    markdown_core_node *table_row_block;
    table_row *row;

    if (markdown_core_parser_is_blank(parser)) {
        return NULL;
    }

    if (get_n_autocompleted_cells(parent_container) > MAX_AUTOCOMPLETED_CELLS) {
        return NULL;
    }

    table_row_block = markdown_core_parser_add_child(
        parser,
        parent_container,
        MARKDOWN_CORE_NODE_TABLE_ROW,
        parent_container->start_column
    );
    if (!table_row_block) {
        return NULL;
    }
    markdown_core_node_set_extension(table_row_block, self);
    table_row_block->end_column = parent_container->end_column;
    table_row_block->as.opaque = parser->mem->calloc(parser->mem, 1, sizeof(node_table_row));
    if (!table_row_block->as.opaque) {
        parser->oom = true;
        markdown_core_node_free(table_row_block);
        return NULL;
    }

    row = row_from_string(
        parser,
        input + markdown_core_parser_get_first_nonspace(parser),
        len - markdown_core_parser_get_first_nonspace(parser)
    );

    if (!row) {
        // clean up the dangling node
        markdown_core_node_free(table_row_block);
        return NULL;
    }

    {
        int i, table_columns = get_n_table_columns(parent_container);

        /* Concrete capture: this row is the current line, scanned from
         * first_nonspace, so a synthetic mark with that base and no pad
         * maps string offsets to columns. Pipes of cells past the table's
         * width still spell row material and still record; only node
         * creation truncates. */
        struct markdown_core_line_mark live_mark;
        live_mark.content_offset = 0;
        live_mark.line = markdown_core_parser_get_line_number(parser);
        live_mark.column = 0;
        live_mark.byte_offset = (markdown_core_bufsize)markdown_core_parser_get_first_nonspace(parser);
        live_mark.pad = 0;
        capture_row_pipes(
            parser,
            table_row_block,
            input + markdown_core_parser_get_first_nonspace(parser),
            row,
            &live_mark
        );

        for (i = 0; i < row->n_columns && i < table_columns; ++i) {
            node_cell *cell = &row->cells[i];
            markdown_core_node *node = markdown_core_parser_add_child(
                parser,
                table_row_block,
                MARKDOWN_CORE_NODE_TABLE_CELL,
                parent_container->start_column + cell->start_offset
            );
            if (!node) {
                break;
            }
            node->internal_offset = cell->internal_offset;
            node->end_column = parent_container->start_column + cell->end_offset;
            markdown_core_node_set_string_content(node, (char *)cell->buf->ptr);
            markdown_core_node_set_extension(node, self);
            set_cell_index(node, i);
            capture_cell_escapes(
                parser,
                node,
                input + markdown_core_parser_get_first_nonspace(parser),
                cell,
                &live_mark
            );
        }

        incr_table_row_count(parent_container, i);

        for (; i < table_columns; ++i) {
            markdown_core_node *node =
                markdown_core_parser_add_child(parser, table_row_block, MARKDOWN_CORE_NODE_TABLE_CELL, 0);
            if (!node) {
                break;
            }
            markdown_core_node_set_extension(node, self);
            set_cell_index(node, i);
        }
    }

    free_table_row(parser->mem, row);

    markdown_core_parser_advance_offset(
        parser,
        (char *)input,
        len - 1 - markdown_core_parser_get_offset(parser),
        false
    );

    return table_row_block;
}

static markdown_core_node *try_opening_table_block(
    markdown_core_extension *self,
    int indented,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
) {
    markdown_core_node_type parent_type = markdown_core_node_get_type(parent_container);

    if (!indented && parent_type == MARKDOWN_CORE_NODE_PARAGRAPH) {
        return try_opening_table_header(self, parser, parent_container, input, len);
    } else if (!indented && parent_type == MARKDOWN_CORE_NODE_TABLE) {
        return try_opening_table_row(self, parser, parent_container, input, len);
    }

    return NULL;
}

static int matches(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    unsigned char *input,
    int len,
    markdown_core_node *parent_container
) {
    int res = 0;

    if (markdown_core_node_get_type(parent_container) == MARKDOWN_CORE_NODE_TABLE) {
        table_row row = {0};
        res = scan_row_from_string(
            parser,
            input + markdown_core_parser_get_first_nonspace(parser),
            len - markdown_core_parser_get_first_nonspace(parser),
            &row,
            false
        );
    }

    return res;
}

static const char *get_type_string(markdown_core_extension *self, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        return "table";
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        if (node->as.opaque && ((node_table_row *)node->as.opaque)->is_header) {
            return "table_header";
        } else {
            return "table_row";
        }
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return "table_cell";
    }

    return "<unknown>";
}

static int can_contain(
    markdown_core_extension *extension,
    markdown_core_node *node,
    markdown_core_node_type child_type
) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        return child_type == MARKDOWN_CORE_NODE_TABLE_ROW;
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        return child_type == MARKDOWN_CORE_NODE_TABLE_CELL;
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
    }
    return false;
}

static int contains_inlines(markdown_core_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_TABLE_CELL;
}

static markdown_core_node *prepare_inline_domain(
    markdown_core_extension *extension,
    const markdown_core_node *committed_owner
) {
    markdown_core_node *clone;

    if (!committed_owner || committed_owner->type != MARKDOWN_CORE_NODE_TABLE_CELL ||
        committed_owner->extension != extension) {
        assert(0 && "table reparse requested for an unsupported owner");
        return NULL;
    }
    clone = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TABLE_CELL, committed_owner->content.mem);
    if (!clone) {
        return NULL;
    }
    clone->extension = extension;
    clone->as.cell_index = committed_owner->as.cell_index;
    clone->start_line = clone->end_line = 1;
    clone->start_column = committed_owner->start_column;
    clone->end_column = committed_owner->end_column;
    clone->internal_offset = committed_owner->internal_offset;
    markdown_core_strbuf_put(&clone->content, committed_owner->content.ptr, committed_owner->content.size);
    if (clone->content.oom) {
        markdown_core_node_free(clone);
        return NULL;
    }

    return clone;
}

static void opaque_alloc(markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    /* A NULL payload is tolerated by every table property helper; the node
     * then reports zero columns/alignments. */
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        node->as.opaque = mem->calloc(mem, 1, sizeof(node_table));
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        node->as.opaque = mem->calloc(mem, 1, sizeof(node_table_row));
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        node->as.opaque = mem->calloc(mem, 1, sizeof(node_cell));
    }
}

static void opaque_free(markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        free_node_table(mem, node->as.opaque);
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        free_node_table_row(mem, node->as.opaque);
    }
}

/* A table's alignments and a row's header bit are what
 * markdown_core_ast_projection_changed compares for these kinds, and they live
 * here rather than in the core union -- so without this the diff pairs a
 * left-aligned table with a right-aligned one and hands over the id. */
static uint64_t table_hash_value(markdown_core_extension *extension, const markdown_core_node *node, uint64_t h) {
    (void)extension;
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        const node_table *table = (const node_table *)node->as.opaque;
        uint16_t column;
        if (!table) {
            return markdown_core_hash_mix(h, 0);
        }
        h = markdown_core_hash_mix(h, (uint64_t)table->n_columns);
        if (table->alignments) {
            for (column = 0; column < table->n_columns; column++) {
                h = markdown_core_hash_mix(h, (uint64_t)table->alignments[column]);
            }
        }
        return h;
    }
    if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        const node_table_row *row = (const node_table_row *)node->as.opaque;
        return markdown_core_hash_mix(h, row && row->is_header ? 1u : 0u);
    }
    return h;
}

static const markdown_core_extension table_extension = {
    .name = "table",
    .last_block_matches = matches,
    .try_opening_block = try_opening_table_block,
    .get_type_string = get_type_string,
    .can_contain = can_contain,
    .contains_inlines = contains_inlines,
    .prepare_inline_domain = prepare_inline_domain,
    .alloc_opaque = opaque_alloc,
    .free_opaque = opaque_free,
    .hash_value = table_hash_value,
};

markdown_core_extension *markdown_core_table_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&table_extension;
}

uint16_t markdown_core_extensions_get_table_columns(markdown_core_node *node) {
    if (node->type != MARKDOWN_CORE_NODE_TABLE || !node->as.opaque) {
        return 0;
    }

    return ((node_table *)node->as.opaque)->n_columns;
}

uint8_t *markdown_core_extensions_get_table_alignments(markdown_core_node *node) {
    if (node->type != MARKDOWN_CORE_NODE_TABLE || !node->as.opaque) {
        return 0;
    }

    return ((node_table *)node->as.opaque)->alignments;
}

int markdown_core_extensions_get_table_row_is_header(markdown_core_node *node) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE_ROW || !node->as.opaque) {
        return 0;
    }

    return ((node_table_row *)node->as.opaque)->is_header;
}
