#include <markdown-core-extension-api.h>
#include "extension.h"
#include <inlines.h>
#include <parser.h>
#include <references.h>
#include <string.h>

#include "ext_scanners.h"
#include "strikethrough.h"
#include "table.h"
#include "markdown-core-extensions.h"

// Limit to prevent a malicious input from causing a denial of service.
#define MAX_AUTOCOMPLETED_CELLS 0x80000

// Custom node flag, initialized in `create_table_extension`.
/* The one extension node flag, as a COMPILE-TIME CONSTANT. It used to be a
 * zero-initialised global filled in by `markdown_core_register_node_flag`,
 * which aborts if it is called twice and hands out bits in call order. One
 * bit, one owner, one value known at compile time (Q16). */
enum { MARKDOWN_CORE_NODE__TABLE_VISITED = MARKDOWN_CORE_NODE__EXTENSION_FIRST };

typedef struct {
    /* Borrowed trimmed authored bytes, valid for this row parse only. */
    markdown_core_chunk content;
    int start_offset, end_offset, internal_offset;
} node_cell;

typedef struct {
    uint16_t n_columns;
    int paragraph_offset;
    node_cell *cells;
} table_row;

static void free_row_cells(markdown_core_mem *mem, table_row *row) {
    row->n_columns = 0;
    mem->free(row->cells);
    row->cells = NULL;
}

static void free_table_row(markdown_core_mem *mem, table_row *row) {
    if (!row) {
        return;
    }

    free_row_cells(mem, row);
    mem->free(row);
}

static void free_node_table(markdown_core_mem *mem, markdown_core_table *table) {
    if (!table) {
        return;
    }
    mem->free(table->columns);
    mem->free(table);
}

static void init_cell(markdown_core_node *node) {
    node->as.table_cell->rowspan = 1;
    node->as.table_cell->colspan = 1;
}

static markdown_core_node *new_cell(markdown_core_parser *parser, markdown_core_node *row, int column) {
    markdown_core_node *cell = markdown_core_parser_add_child(parser, row, MARKDOWN_CORE_NODE_TABLE_CELL, column);
    if (cell) {
        init_cell(cell);
        markdown_core_node_set_extension(cell, &MARKDOWN_CORE_EXTENSION_TABLE);
    }
    return cell;
}

/* Assemble every cell once, preserving a source run at each contraction.
 * A logical pipe represents both authored bytes of its escape. There is no
 * alternate inline parser and no position repair after parsing. */
static void set_cell_content(markdown_core_parser *parser, markdown_core_node *node, const node_cell *cell, int line,
                             int column) {
    bufsize_t from = 0;
    while (from < cell->content.len) {
        bufsize_t to = from;
        int width = 1;
        if (cell->content.data[from] == '\\' && from + 1 < cell->content.len && cell->content.data[from + 1] == '|') {
            width = 2;
            to = from + 2;
        } else {
            do {
                to++;
            } while (to < cell->content.len && !(cell->content.data[to] == '\\' && to + 1 < cell->content.len &&
                                                 cell->content.data[to + 1] == '|'));
        }
        markdown_core_parser_append_content_mark(parser, node, node->content.size, line, column + from, width, width);
        markdown_core_strbuf_put(&node->content, cell->content.data + from + width - 1, to - from - width + 1);
        if (node->content.oom || parser->oom) {
            parser->oom = true;
            return;
        }
        from = to;
    }
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
        node_cell *cells = (node_cell *)mem->realloc(row->cells, (2 * n_columns - 1) * sizeof(node_cell));
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

static table_row *row_from_string(const markdown_core_extension *self, markdown_core_parser *parser,
                                  unsigned char *string, int len) {
    // Parses a single table row. It has the following form:
    // `delim? table_cell (delim table_cell)* delim? newline`
    // Note that cells are allowed to be empty.
    //
    // From the GitHub-flavored Markdown specification:
    //
    // > Each row consists of cells containing arbitrary text, in which inlines
    // > are parsed, separated by pipes (|). A leading and trailing pipe is also
    // > recommended for clarity of reading, and if there’s otherwise parsing
    // > ambiguity.

    table_row *row = NULL;
    bufsize_t cell_matched = 1, pipe_matched = 1, offset;
    int expect_more_cells = 1;
    int row_end_offset = 0;
    int int_overflow_abort = 0;

    row = (table_row *)parser->mem->calloc(1, sizeof(table_row));
    if (!row) {
        parser->oom = true;
        return NULL;
    }
    row->n_columns = 0;
    row->cells = NULL;

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
            {
                int cell_oom = 0;
                node_cell *cell = append_row_cell(parser->mem, row, &cell_oom);
                if (cell_oom) {
                    parser->oom = true;
                }
                if (!cell) {
                    int_overflow_abort = 1;
                    break;
                }
                cell->content = (markdown_core_chunk){string + offset, cell_matched, 0};
                markdown_core_chunk_trim(&cell->content);
                cell->start_offset = offset;
                cell->end_offset = offset + cell_matched - 1;
                cell->internal_offset = 0;

                while (cell->start_offset > row->paragraph_offset && string[cell->start_offset - 1] != '|') {
                    --cell->start_offset;
                    ++cell->internal_offset;
                }
                /* An adjacent `||` cell has no byte with which to form an
                 * inclusive span. Source positions are location metadata
                 * rather than substring bounds, so point it at the delimiter
                 * that completed the cell instead of manufacturing the
                 * reversed interval `offset..offset-1`. Do this after the
                 * start rewind: whitespace before a separator is authored
                 * cell source and already gives the cell an ordered span. */
                if (cell_matched == 0 && cell->start_offset == offset) {
                    cell->end_offset = cell->start_offset;
                }
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

                free_row_cells(parser->mem, row);

                // Scan past the (optional) leading pipe.
                offset += scan_table_cell_end(string, len, offset);

                expect_more_cells = 1;
            } else {
                expect_more_cells = 0;
            }
        }
    }

    if (offset != len || row->n_columns == 0 || int_overflow_abort) {
        free_table_row(parser->mem, row);
        row = NULL;
    }

    return row;
}

/* Give `node` the source span of [start_offset, end_offset] in `owner`'s
 * content buffer. Every table position recovered from that buffer goes through
 * here, so there is one place that knows a content offset is not a column. */
static void S_place_content_span(markdown_core_parser *parser, markdown_core_node *owner, markdown_core_node *node,
                                 bufsize_t start_offset, bufsize_t end_offset) {
    int line, column;

    if (markdown_core_parser_content_place(parser, owner, start_offset, &line, &column)) {
        node->start_line = line;
        node->start_column = column;
    }
    if (markdown_core_parser_content_place(parser, owner, end_offset, &line, &column)) {
        node->end_line = line;
        node->end_column = column;
    }
}

static void try_inserting_table_header_paragraph(markdown_core_parser *parser, markdown_core_node *parent_container,
                                                 unsigned char *parent_string, int paragraph_offset) {
    markdown_core_node *paragraph;
    bufsize_t first = 0;
    bufsize_t last = paragraph_offset;
    int line, column;

    // Four allocations, and every one of them used to be trusted. The first was
    // a crash: an unchecked node reached markdown_core_node_set_string_content,
    // which dereferences it -- SIGSEGV on `lead text` above a two-column table
    // with the allocation refused. The other three lose the lead paragraph
    // WITHOUT setting parser->oom, so the document comes back short and the
    // failure bit says everything was fine.
    paragraph = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, parser->mem);
    if (!paragraph) {
        parser->oom = true;
        return;
    }

    /* THE LEAD KEEPS ITS AUTHORED SPELLING. This used to run the lead through
     * `unescape_pipes`, which is a CELL transformation: a pipe a cell escaped
     * is not a pipe the cell contains. The lead is not a cell -- it is the
     * paragraph the table was split out of -- so `pre \\| lead` above a table
     * lost one of its two backslashes here and the inline phase then read the
     * survivor as the escape, giving `pre | lead` where the author wrote an
     * escaped backslash followed by a pipe. */
    while (first < last && markdown_core_isspace(parent_string[first])) {
        first++;
    }
    while (last > first && markdown_core_isspace(parent_string[last - 1])) {
        last--;
    }
    markdown_core_strbuf_put(&paragraph->content, parent_string + first, last - first);
    if (paragraph->content.oom) {
        parser->oom = true;
        markdown_core_node_free(paragraph);
        return;
    }

    /* The lead is synthesized from a content offset and so has no position of
     * its own; before requirement 10 it kept the 0:0..0:0 sentinel, and every
     * inline in it inherited line zero. The map answers both ends. */
    if (markdown_core_parser_content_place(parser, parent_container, first, &line, &column)) {
        paragraph->start_line = line;
        paragraph->start_column = column;
    }
    if (last > first && markdown_core_parser_content_place(parser, parent_container, last - 1, &line, &column)) {
        paragraph->end_line = line;
        paragraph->end_column = column;
    }
    /* The lead's content is a SLICE of the paragraph's, and it can be several
     * lines long, so it takes the marks for those lines rather than one mark
     * for the first of them. */
    markdown_core_parser_adopt_content_marks(parser, parent_container, paragraph, first, last - first);

    if (!markdown_core_node_insert_before(parent_container, paragraph)) {
        // markdown_core_node_free, not mem->free: the node owns a content
        // buffer by now, and freeing the struct alone leaks it.
        parser->oom = true;
        markdown_core_node_free(paragraph);
    }
}

// A decline is NULL. `core/blocks.c` offers each attached extension a turn at
// this line in attach order and stops at the first non-NULL answer, so
// returning `parent_container` when nothing was opened takes away every later
// extension's turn -- and this function used to do that on every path,
// including "there is no table here". Enabling tables then changed the parse of
// input containing no table at all: a directive or formula block could not
// interrupt a paragraph.
//
// TWO KINDS OF `return parent_container` BELOW MUST STAY, and both say
// something this one does not:
//
//   the four allocation failures AFTER markdown_core_node_set_type succeeds.
//   The paragraph has already become a TABLE by then, so the container really
//   was opened; answering NULL would leave a retyped node behind and tell the
//   caller nothing happened. They set parser->oom and the parse is abandoned.
//
//   the final return, which is the genuine opening path.
//
// D8 turned SIX wrong declines in this function from `return parent_container`
// into `return NULL`. Step 3a deleted the arena and with it one of the six --
// the retry that re-parsed both rows because the arena's pop had just freed
// them, and whose mismatch answered NULL. FIVE remain. The line is gone; the
// property is not, and `extensions-conflicts.txt` is what re-proves it.
//
// `table` is the only extension with this shape; directive and formula already
// answer NULL on every decline.
static markdown_core_node *try_opening_table_header(const markdown_core_extension *self, markdown_core_parser *parser,
                                                    markdown_core_node *parent_container, unsigned char *input,
                                                    int len) {
    markdown_core_node *table_header;
    table_row *header_row = NULL;
    table_row *delimiter_row = NULL;
    const char *parent_string;
    uint16_t i;
    int header_line, header_column;

    if (parent_container->flags & MARKDOWN_CORE_NODE__TABLE_VISITED) {
        return NULL;
    }

    if (!scan_table_start(input, len, markdown_core_parser_get_first_nonspace(parser))) {
        return NULL;
    }

    // Since scan_table_start was successful, we must have a delimiter row.
    delimiter_row = row_from_string(self, parser, input + markdown_core_parser_get_first_nonspace(parser),
                                    len - markdown_core_parser_get_first_nonspace(parser));
    // assert may be optimized out, don't rely on it for security boundaries
    if (!delimiter_row) {
        return NULL;
    }

    assert(delimiter_row);

    // Check for a matching header row. We call `row_from_string` with the entire
    // (potentially long) parent container as input, but this should be safe since
    // `row_from_string` bails out early if it does not find a row.
    parent_string = markdown_core_node_get_string_content(parent_container);
    header_row = row_from_string(self, parser, (unsigned char *)parent_string, (int)strlen(parent_string));
    if (!header_row || header_row->n_columns != delimiter_row->n_columns) {
        free_table_row(parser->mem, delimiter_row);
        free_table_row(parser->mem, header_row);
        parent_container->flags |= MARKDOWN_CORE_NODE__TABLE_VISITED;
        return NULL;
    }

    if (!markdown_core_node_set_type(parent_container, MARKDOWN_CORE_NODE_TABLE)) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return NULL;
    }

    if (header_row->paragraph_offset) {
        try_inserting_table_header_paragraph(parser, parent_container, (unsigned char *)parent_string,
                                             header_row->paragraph_offset);
        /* The table starts where its HEADER ROW was written, not where the
         * paragraph it was split out of did. Taken before the row and cells
         * below read start_column, because they are placed against it. */
        if (markdown_core_parser_content_place(parser, parent_container, header_row->paragraph_offset, &header_line,
                                               &header_column)) {
            parent_container->start_line = header_line;
            parent_container->start_column = header_column;
        }
    }

    /* The paragraph is already rewritten into a table node here.  On
     * allocation failure the half-converted node stays behind with a NULL
     * payload -- every table helper tolerates that -- and the sticky flag
     * makes the parse fail, so nothing downstream trusts the node. */
    markdown_core_node_set_extension(parent_container, self);
    // From here down the node IS a table, so every remaining
    // `return parent_container` means "opened, then failed" rather than
    // "declined". Do not turn these into NULL with the six above it.
    parent_container->opaque = parser->mem->calloc(1, sizeof(markdown_core_table));
    if (!parent_container->opaque) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    markdown_core_table *table = parent_container->opaque;
    table->column_count = header_row->n_columns;
    table->columns = parser->mem->calloc(table->column_count, sizeof(*table->columns));
    if (!table->columns) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    for (i = 0; i < delimiter_row->n_columns; ++i) {
        const markdown_core_chunk *cell = &delimiter_row->cells[i].content;
        bool left = cell->data[0] == ':', right = cell->data[cell->len - 1] == ':';
        table->columns[i].alignment =
            left ? (right ? MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER : MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT)
                 : (right ? MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT : MARKDOWN_CORE_TABLE_ALIGNMENT_NONE);
    }

    table_header = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                  parent_container->start_column);
    if (!table_header) {
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    markdown_core_node_set_extension(table_header, self);
    /* The header row and its cells are RECOVERED from the paragraph's content
     * buffer, and every offset below is an offset into that buffer. Adding one
     * to a column is only right while the buffer holds a single line starting
     * where the block does; with a lead split off, `| a | b |` on line three
     * was reported at 1:10, a column that is not on line one. The map turns
     * each offset back into the place it was written. */
    S_place_content_span(parser, parent_container, table_header, header_row->paragraph_offset,
                         (bufsize_t)strlen(parent_string) - 2);

    table->head_count = 1;

    for (i = 0; i < header_row->n_columns; ++i) {
        node_cell *cell = &header_row->cells[i];
        markdown_core_node *header_cell =
            new_cell(parser, table_header, parent_container->start_column + cell->start_offset);
        if (!header_cell) {
            break;
        }
        header_cell->internal_offset = cell->internal_offset;
        S_place_content_span(parser, parent_container, header_cell, cell->start_offset, cell->end_offset);
        int line, column;
        if (markdown_core_parser_content_place(parser, parent_container,
                                               (bufsize_t)(cell->content.data - (unsigned char *)parent_string), &line,
                                               &column)) {
            set_cell_content(parser, header_cell, cell, line, column);
        }
    }

    markdown_core_parser_advance_offset(
        parser, (char *)input, (int)strlen((char *)input) - 1 - markdown_core_parser_get_offset(parser), false);

    free_table_row(parser->mem, header_row);
    free_table_row(parser->mem, delimiter_row);
    return parent_container;
}

static markdown_core_node *try_opening_table_row(const markdown_core_extension *self, markdown_core_parser *parser,
                                                 markdown_core_node *parent_container, unsigned char *input, int len) {
    markdown_core_node *table_row_block;
    table_row *row;

    if (markdown_core_parser_is_blank(parser)) {
        return NULL;
    }

    markdown_core_table *table = parent_container->opaque;
    if (!table || table->autocompleted_cells > MAX_AUTOCOMPLETED_CELLS) {
        return NULL;
    }

    table_row_block = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                     parent_container->start_column);
    if (!table_row_block) {
        return NULL;
    }
    markdown_core_node_set_extension(table_row_block, self);
    table_row_block->end_column = parent_container->end_column;

    row = row_from_string(self, parser, input + markdown_core_parser_get_first_nonspace(parser),
                          len - markdown_core_parser_get_first_nonspace(parser));

    if (!row) {
        // clean up the dangling node
        markdown_core_node_free(table_row_block);
        return NULL;
    }

    {
        int i, table_columns = (int)table->column_count;

        for (i = 0; i < row->n_columns && i < table_columns; ++i) {
            node_cell *cell = &row->cells[i];
            markdown_core_node *node =
                new_cell(parser, table_row_block, parent_container->start_column + cell->start_offset);
            if (!node) {
                break;
            }
            node->internal_offset = cell->internal_offset;
            node->end_column = parent_container->start_column + cell->end_offset;
            set_cell_content(parser, node, cell, markdown_core_parser_get_line_number(parser),
                             1 + (int)(cell->content.data - input));
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
            markdown_core_node *node = new_cell(parser, table_row_block, (int)completed_at);
            if (!node) {
                break;
            }
            node->end_column = (int)completed_at;
        }
    }

    free_table_row(parser->mem, row);

    markdown_core_parser_advance_offset(parser, (char *)input, len - 1 - markdown_core_parser_get_offset(parser),
                                        false);

    return table_row_block;
}

static markdown_core_node *try_opening_table_block(const markdown_core_extension *self, int indented,
                                                   markdown_core_parser *parser, markdown_core_node *parent_container,
                                                   unsigned char *input, int len) {
    markdown_core_node_type parent_type = markdown_core_node_get_type(parent_container);

    if (!indented && parent_type == MARKDOWN_CORE_NODE_PARAGRAPH) {
        return try_opening_table_header(self, parser, parent_container, input, len);
    } else if (!indented && parent_type == MARKDOWN_CORE_NODE_TABLE) {
        return try_opening_table_row(self, parser, parent_container, input, len);
    }

    return NULL;
}

static int matches(const markdown_core_extension *self, markdown_core_parser *parser, unsigned char *input, int len,
                   markdown_core_node *parent_container) {
    int res = 0;

    if (markdown_core_node_get_type(parent_container) == MARKDOWN_CORE_NODE_TABLE) {
        table_row *new_row = row_from_string(self, parser, input + markdown_core_parser_get_first_nonspace(parser),
                                             len - markdown_core_parser_get_first_nonspace(parser));
        if (new_row && new_row->n_columns) {
            res = 1;
        }
        free_table_row(parser->mem, new_row);
    }

    return res;
}

static const char *get_type_string(const markdown_core_extension *self, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        return "table";
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        return "table_row";
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return "table_cell";
    }

    return "<unknown>";
}

static int can_contain(const markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        return child_type == MARKDOWN_CORE_NODE_TABLE_ROW;
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        return child_type == MARKDOWN_CORE_NODE_TABLE_CELL;
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type) || MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type);
    }
    return false;
}

static int contains_inlines(const markdown_core_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_TABLE_CELL;
}

static void opaque_alloc(const markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    /* A NULL payload makes the table facade accessors fail; no incomplete
     * table is returned by a successful parse. */
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        node->opaque = mem->calloc(1, sizeof(markdown_core_table));
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        init_cell(node);
    }
}

static void opaque_free(const markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        free_node_table(mem, node->opaque);
    }
}

/* A block-only extension: no byte ends a text run for it, no byte is offered to an
 * inline hook it does not have, and no byte is transparent to flanking. */
const markdown_core_extension MARKDOWN_CORE_EXTENSION_TABLE = {
    .name = "table",
    .last_block_matches = matches,
    .try_opening_block = try_opening_table_block,
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .contains_inlines_func = contains_inlines,
    .opaque_alloc_func = opaque_alloc,
    .opaque_free_func = opaque_free,
};
