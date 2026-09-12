#include <markdown-core-extension-api.h>
#include "extension.h"
#include <inlines.h>
#include <parser.h>
#include <references.h>
#include <string.h>
#include <limits.h>
#include "utf8.h"
#include "scanners.h"

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
static void set_cell_content(markdown_core_parser *parser, markdown_core_node *node, const node_cell *cell,
                             markdown_core_node *source, bufsize_t offset) {
    for (bufsize_t from = 0; from < cell->content.len && !parser->oom;) {
        bufsize_t to = from;
        bool escaped =
            cell->content.data[from] == '\\' && from + 1 < cell->content.len && cell->content.data[from + 1] == '|';
        if (escaped) {
            int line = parser->line_number, first, last;
            if (source) {
                markdown_core_parser_content_place(parser, source, offset + from, &line, &first);
                markdown_core_parser_content_end_place(parser, source, offset + from + 1, &line, &last);
            } else {
                first = markdown_core_parser_source_column(parser, line, offset + from + 1);
                last = markdown_core_parser_source_column(parser, line, offset + from + 2);
            }
            markdown_core_parser_append_content_mark(parser, node, node->content.size, line, first, last - first + 1,
                                                     last - first + 1);
            markdown_core_strbuf_putc(&node->content, '|');
            to = from + 2;
        } else {
            do {
                to++;
            } while (to < cell->content.len && !(cell->content.data[to] == '\\' && to + 1 < cell->content.len &&
                                                 cell->content.data[to + 1] == '|'));
            if (source) {
                markdown_core_parser_append_content_marks(parser, source, node, offset + from, to - from,
                                                          node->content.size);
            } else {
                markdown_core_parser_append_source_marks(parser, node, parser->line_number, offset + from + 1,
                                                         to - from, node->content.size);
            }
            markdown_core_strbuf_put(&node->content, cell->content.data + from, to - from);
        }
        if (node->content.oom) {
            parser->oom = true;
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
    bufsize_t content_end = paragraph_offset;
    bufsize_t scope_end = content_end;
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
    while (first < content_end && markdown_core_isspace(parent_string[first])) {
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
    if (scope_end > first &&
        markdown_core_parser_content_place(parser, parent_container, scope_end - 1, &line, &column)) {
        paragraph->end_line = line;
        paragraph->end_column = column;
    }
    /* The lead's content is a SLICE of the paragraph's, and it can be several
     * lines long, so it takes the marks for those lines rather than one mark
     * for the first of them. */
    markdown_core_parser_adopt_content_marks(parser, parent_container, paragraph, first, content_end - first);

    if (!markdown_core_node_insert_before(parent_container, paragraph)) {
        // markdown_core_node_free, not mem->free: the node owns a content
        // buffer by now, and freeing the struct alone leaks it.
        parser->oom = true;
        markdown_core_node_free(paragraph);
        return;
    }
    /* A table split completes this paragraph just as a later block start
     * would: reference definitions and anchor attachment share finalization. */
    markdown_core_parser_finalize_paragraph(parser, paragraph);
}

/* Return NULL when the syntax does not match or the parent rejects the table
 * kind, so later extensions can try the same line. Once the paragraph becomes
 * a table, return that container even if a later allocation fails; parser->oom
 * then aborts the parse and destruction releases the partially built table. */
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

    markdown_core_node_set_kind_result result = markdown_core_node_set_kind(parent_container, MARKDOWN_CORE_NODE_TABLE);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
            parser->oom = true;
        }
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

    /* Table data belongs to the extension. Its cleanup accepts partial
     * initialization when an allocation fails after the kind change. */
    markdown_core_node_set_extension(parent_container, self);
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

    table_header = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW, 1);
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
        markdown_core_node *header_cell = new_cell(parser, table_header, 1);
        if (!header_cell) {
            break;
        }
        header_cell->internal_offset = cell->internal_offset;
        S_place_content_span(parser, parent_container, header_cell, cell->start_offset, cell->end_offset);
        set_cell_content(parser, header_cell, cell, parent_container,
                         (bufsize_t)(cell->content.data - (unsigned char *)parent_string));
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

    table_row_block =
        markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW, parser->offset + 1);
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
                new_cell(parser, table_row_block, parser->first_nonspace + 1 + cell->start_offset);
            if (!node) {
                break;
            }
            node->internal_offset = cell->internal_offset;
            node->end_column = markdown_core_parser_source_column(parser, parser->line_number,
                                                                  parser->first_nonspace + 1 + cell->end_offset);
            set_cell_content(parser, node, cell, NULL, (bufsize_t)(cell->content.data - input));
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
            node->end_column = markdown_core_parser_source_column(parser, parser->line_number, (int)completed_at);
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
    } else if (!indented) {
        return markdown_core_table_try_open(parser, parent_container, input, len);
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
        int32_t scalar = 0;
        if (first + 1 < length) {
            markdown_core_utf8proc_iterate(data + first + 1, length - first - 1, &scalar);
        }
        if (scalar && markdown_core_utf8proc_is_punctuation(scalar)) {
            return -1;
        }
        marker = 1;
    }
    if (!marker) {
        return -1;
    }
    int content = first + marker;
    while (content < length && markdown_core_isspace(data[content])) {
        content++;
    }
    return content;
}

static int matches(const markdown_core_extension *self, markdown_core_parser *parser, unsigned char *input, int len,
                   markdown_core_node *parent_container) {
    int res = 0;

    if (markdown_core_node_get_type(parent_container) == MARKDOWN_CORE_NODE_TABLE) {
        if (table_caption_start(input, len, parser->first_nonspace, parser->indent) >= 0) {
            return 0;
        }
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
    if (node->kind == MARKDOWN_CORE_NODE_TABLE) {
        return "table";
    } else if (node->kind == MARKDOWN_CORE_NODE_TABLE_ROW) {
        return "table_row";
    } else if (node->kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return "table_cell";
    }

    return "<unknown>";
}

static int can_contain(const markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->kind == MARKDOWN_CORE_NODE_TABLE) {
        return child_type == MARKDOWN_CORE_NODE_TABLE_ROW;
    } else if (node->kind == MARKDOWN_CORE_NODE_TABLE_ROW) {
        return child_type == MARKDOWN_CORE_NODE_TABLE_CELL;
    } else if (node->kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type) || MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type);
    }
    return false;
}

static int contains_inlines(const markdown_core_extension *extension, markdown_core_node *node) {
    /* Block inputs have consumed their source before the inline phase. Their
     * children, rather than the cell wrapper, own the remaining inline text. */
    return node->kind == MARKDOWN_CORE_NODE_TABLE_CAPTION ||
           (node->kind == MARKDOWN_CORE_NODE_TABLE_CELL && node->content.size > 0);
}

static void opaque_alloc(const markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    /* A NULL payload makes the table facade accessors fail; no incomplete
     * table is returned by a successful parse. */
    if (node->kind == MARKDOWN_CORE_NODE_TABLE) {
        node->opaque = mem->calloc(1, sizeof(markdown_core_table));
    } else if (node->kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        init_cell(node);
    }
}

static void opaque_free(const markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    free_node_table(mem, node->opaque);
}

static int visit_owned_subtrees(const markdown_core_extension *self, markdown_core_node *node,
                                markdown_core_owned_subtree_visitor visitor, void *context) {
    markdown_core_table *table = node->opaque;
    return !table || !table->caption || visitor(&table->caption, context);
}

/* Source candidates own geometry only. Public nodes are allocated after a
 * complete candidate has passed validation; failed candidates consume nothing. */
typedef struct {
    int start, end;
} table_interval;

typedef struct markdown_core_table_source_line {
    const unsigned char *data, *after;
    markdown_core_parser *parser;
    int length, input_length, offset, first, first_column, indent, line, blanks;
    bool dashes_scanned, full_boundary;
    size_t dash_count;
    table_interval *dashes;
    int *bytes;
    int columns;
} table_source_line;

typedef struct {
    markdown_core_parser *parser;
    markdown_core_block_lookahead lookahead;
    table_source_line *lines;
    size_t count;
    bool ended;
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
    size_t column_count;
    table_source_row *rows;
    size_t row_count, row_capacity, head_count, foot_count;
    table_source_cell *cells;
    size_t cell_count, cell_capacity;
    size_t first, last;
    bool block_content, open;
    int padding_limit;
} table_candidate;

static bool table_reserve(markdown_core_parser *parser, void **values, size_t *capacity, size_t count, size_t size) {
    if (count <= *capacity) {
        return true;
    }
    size_t next = *capacity ? *capacity : 8;
    while (next < count) {
        if (next > SIZE_MAX / 2) {
            parser->oom = true;
            return false;
        }
        next *= 2;
    }
    if (next > SIZE_MAX / size) {
        parser->oom = true;
        return false;
    }
    void *grown = parser->mem->realloc(*values, next * size);
    if (!grown) {
        parser->oom = true;
        return false;
    }
    *values = grown;
    *capacity = next;
    return true;
}

static bool table_source_push(table_source *source, table_source_line line) {
    markdown_core_parser *parser = source->parser;
    size_t capacity = parser->table_lines_capacity;
    if (!table_reserve(parser, (void **)&parser->table_lines, &parser->table_lines_capacity, source->count + 1,
                       sizeof(*source->lines))) {
        return false;
    }
    source->lines = parser->table_lines;
    parser->table_workspace_growth += parser->table_lines_capacity != capacity;
    line.input_length = line.length;
    while (line.length && (line.data[line.length - 1] == '\n' || line.data[line.length - 1] == '\r')) {
        line.length--;
    }
    /* The current streaming line, immutable document source and normalized
     * EOF line all outlive this non-nested query, including its commitment. */
    line.parser = source->parser;
    source->lines[source->count++] = line;
    return true;
}

static bool table_source_get(table_source *source, size_t index) {
    while (source->count <= index && !source->ended && !source->parser->oom) {
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
                                                           .line = source->lookahead.line - 1,
                                                           .after = source->lookahead.cursor})) {
            return false;
        }
    }
    return index < source->count && !source->parser->oom;
}

static void table_source_free(table_source *source) {
    markdown_core_parser_lookahead_end(&source->lookahead);
    for (size_t i = 0; i < source->count; i++) {
        source->parser->mem->free(source->lines[i].bytes);
        source->parser->mem->free(source->lines[i].dashes);
    }
}

static void table_candidate_free(markdown_core_parser *parser, table_candidate *candidate) {
    parser->mem->free(candidate->columns);
    parser->mem->free(candidate->rows);
    parser->mem->free(candidate->cells);
    *candidate = (table_candidate){0};
}

/* Grid columns count scalars, with tabs expanded at four-column stops. Each
 * position retains the input byte that authored it; slicing never loses tabs
 * or UTF-8 provenance and never consults terminal display width. */
static bool table_source_columns(table_source *source, size_t index) {
    if (!table_source_get(source, index)) {
        return false;
    }
    table_source_line *line = &source->lines[index];
    if (line->bytes) {
        return true;
    }
    source->parser->table_geometry_lines++;
    size_t capacity = 0;
    for (int byte = line->offset, column = 0;;) {
        if (!table_reserve(source->parser, (void **)&line->bytes, &capacity, (size_t)column + 5,
                           sizeof(*line->bytes))) {
            return false;
        }
        if (byte == line->length) {
            line->bytes[column] = byte;
            line->columns = column;
            return true;
        }
        if (line->data[byte] == '\t') {
            int spaces = 4 - column % 4;
            while (spaces--) {
                line->bytes[column++] = byte;
            }
            byte++;
        } else {
            int32_t scalar;
            int width = markdown_core_utf8proc_iterate(line->data + byte, line->length - byte, &scalar);
            line->bytes[column++] = byte;
            byte += width > 0 ? width : 1;
        }
    }
}

static int table_character(const table_source_line *line, int column) {
    line->parser->table_scan_work++;
    if (column < 0 || column >= line->columns) {
        return 0;
    }
    int c = line->data[line->bytes[column]];
    return c == '\t' ? ' ' : c;
}

static int table_byte(const table_source_line *line, int column) {
    return column >= line->columns ? line->length : line->bytes[column < 0 ? 0 : column];
}

static int table_column(const table_source_line *line, int byte) {
    int low = 0, high = line->columns;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (line->bytes[middle] < byte) {
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
        source->parser->table_separator_scans++;
        if (line->indent > 3) {
            return 0;
        }
        const unsigned char *p = line->data + line->offset, *from, *before;
        int result;
        do {
            before = p;
            result = _scan_table_dash(&p, line->data + line->length, &from);
            source->parser->table_scan_work += (size_t)(p - before) + 1;
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

static const table_interval *table_dashes(table_source *source, size_t index) {
    size_t count = table_dash_count(source, index);
    if (!count) {
        return NULL;
    }
    table_source_line *line = &source->lines[index];
    if (!line->dashes) {
        line->dashes = source->parser->mem->calloc(count, sizeof(*line->dashes));
        if (!line->dashes) {
            source->parser->oom = true;
            return NULL;
        }
        const unsigned char *p = line->data + line->offset, *from, *before = p;
        int column = 0;
        for (size_t i = 0; i < count; i++) {
            _scan_table_dash(&p, line->data + line->length, &from);
            source->parser->table_scan_work += (size_t)(p - before);
            while (before < from) {
                column += *before++ == '\t' ? 4 - column % 4 : 1;
            }
            int start = column;
            column += (int)(p - from);
            line->dashes[i] = (table_interval){start, column};
            before = p;
        }
    }
    return line->dashes;
}

static bool table_full_boundary(table_source *source, size_t index) {
    return table_dash_count(source, index) == 1 && source->lines[index].full_boundary;
}

static bool table_add_row(table_source *source, table_candidate *candidate, size_t first, size_t last) {
    if (!table_reserve(source->parser, (void **)&candidate->rows, &candidate->row_capacity, candidate->row_count + 1,
                       sizeof(*candidate->rows))) {
        return false;
    }
    candidate->rows[candidate->row_count++] = (table_source_row){first, last, candidate->cell_count, 0};
    return true;
}

static bool table_add_cell(table_source *source, table_candidate *candidate, size_t first, size_t last, int left,
                           int right, int start, int end) {
    if (!table_reserve(source->parser, (void **)&candidate->cells, &candidate->cell_capacity, candidate->cell_count + 1,
                       sizeof(*candidate->cells))) {
        return false;
    }
    candidate->cells[candidate->cell_count++] =
        (table_source_cell){first, last, left, right, candidate->row_count - 1, 1, 1, start, end};
    candidate->rows[candidate->row_count - 1].count++;
    return true;
}

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
    for (size_t column = 0; column < candidate->column_count; column++) {
        int left = column ? runs[column].start : 0;
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
            while (start < end && markdown_core_isspace(begin->data[start])) {
                start++;
            }
            while (end > start && markdown_core_isspace(begin->data[end - 1])) {
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

static bool table_set_columns(table_source *source, table_candidate *candidate, const table_interval *runs,
                              size_t count, size_t alignment_line, bool widths) {
    if (!table_source_columns(source, alignment_line)) {
        return false;
    }
    candidate->column_count = count;
    candidate->columns = source->parser->mem->calloc(count, sizeof(*candidate->columns));
    if (!candidate->columns) {
        source->parser->oom = true;
        return false;
    }
    table_source_line *line = &source->lines[alignment_line];
    double total = 0;
    for (size_t i = 0; i < count; i++) {
        total += (i + 1 < count ? runs[i + 1].start : runs[i].end) - runs[i].start;
    }
    for (size_t i = 0; i < count; i++) {
        int left = i ? runs[i].start : 0;
        int right = i + 1 < count ? runs[i + 1].start : line->columns;
        if (right > line->columns) {
            right = line->columns;
        }
        while (right > left && table_character(line, right - 1) == ' ') {
            right--;
        }
        bool occupied = right > left;
        bool left_space = table_character(line, left) == ' ';
        bool right_space = right - runs[i].start < runs[i].end - runs[i].start;
        candidate->columns[i].alignment =
            !occupied    ? MARKDOWN_CORE_TABLE_ALIGNMENT_NONE
            : left_space ? (right_space ? MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER : MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT)
                         : (right_space ? MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT : MARKDOWN_CORE_TABLE_ALIGNMENT_NONE);
        if (widths) {
            candidate->columns[i].relative.has_value = true;
            candidate->columns[i].relative.value =
                ((i + 1 < count ? runs[i + 1].start : runs[i].end) - runs[i].start) / total;
        }
    }
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

static markdown_core_lookahead_entry *table_search_fact(table_source *source, size_t index) {
    table_source_line *line = &source->lines[index];
    markdown_core_lookahead_entry *fact = markdown_core_parser_lookahead_entry(source->parser, line->line);
    if (fact && (fact->table_container != source->lookahead.parent || fact->table_offset != line->offset)) {
        fact->table_container = source->lookahead.parent;
        fact->table_offset = line->offset;
        fact->table_absent = 0;
    }
    return fact;
}

static bool table_search_absent(table_source *source, size_t index, unsigned grammar) {
    markdown_core_lookahead_entry *fact = table_search_fact(source, index);
    return fact && (fact->table_absent & grammar);
}

/* A failed search establishes absence for every suffix it traversed. Publish
 * that grammatical fact once, so another opener cannot rescan the same run. */
static void table_search_finish(table_source *source, size_t first, size_t after, unsigned grammar) {
    for (size_t i = first; i < after; i++) {
        markdown_core_lookahead_entry *fact = table_search_fact(source, i);
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
    const table_interval *a = table_dashes(source, left), *b = table_dashes(source, right);
    if (!a || !b) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        source->parser->table_scan_work++;
        if (a[i].start != b[i].start || a[i].end != b[i].end) {
            return false;
        }
    }
    return true;
}

typedef struct {
    const table_interval *runs;
    size_t count, line;
} table_separator_key;

typedef struct {
    size_t first, count, digit;
} table_separator_group;

/* Exact interval keys use sixteen radix digits per start/end pair and a
 * terminator. A variable-length radix traversal visits only existing key
 * digits, so distinct widths, offsets and column counts cost source-linear
 * work without hashing or repeatedly comparing long shared prefixes. */
static unsigned table_separator_digit(table_source *source, table_separator_key key, size_t digit) {
    source->parser->table_scan_work++;
    if (digit / 16 >= key.count) {
        return 0;
    }
    table_interval run = key.runs[digit / 16];
    uint32_t position = (uint32_t)(digit % 16 < 8 ? run.start : run.end);
    return 1 + ((position >> (28 - 4 * (digit % 8))) & 15);
}

/* A failed headerless search has captured the whole nonblank run. Group its
 * separator geometries once: only the final occurrence of each exact key has
 * no later footer. Other occurrences remain eligible, including valid suffix
 * tables. Facts retain the shared container/offset ownership. */
static void table_simple_search_finish(table_source *source, size_t first, size_t last) {
    size_t capacity = last - first + 1, count = 0;
    table_separator_key *keys = source->parser->mem->calloc(capacity, sizeof(*keys));
    table_separator_key *scratch = source->parser->mem->calloc(capacity, sizeof(*scratch));
    table_separator_group *groups = source->parser->mem->calloc(capacity, sizeof(*groups));
    if (!keys || !scratch || !groups) {
        source->parser->oom = true;
        goto done;
    }
    for (size_t i = first; i <= last; i++) {
        size_t columns = table_dash_count(source, i);
        if (columns > 1) {
            const table_interval *runs = table_dashes(source, i);
            if (!runs) {
                goto done;
            }
            keys[count++] = (table_separator_key){runs, columns, i};
        }
    }
    size_t pending = 1;
    groups[0] = (table_separator_group){0, count, 0};
    while (pending) {
        table_separator_group group = groups[--pending];
        if (group.count == 1) {
            markdown_core_lookahead_entry *fact = table_search_fact(source, keys[group.first].line);
            if (fact) {
                fact->table_absent |= TABLE_NO_SIMPLE_FOOTER;
            }
            continue;
        }
        size_t lengths[17] = {0}, offsets[17], cursors[17];
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
            markdown_core_lookahead_entry *fact = table_search_fact(source, final);
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
    source->parser->mem->free(keys);
    source->parser->mem->free(scratch);
    source->parser->mem->free(groups);
}

static bool table_parse_simple(table_source *source, size_t start, table_candidate *candidate) {
    const table_interval *runs = NULL;
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
    runs = table_dashes(source, delimiter);
    if (!runs) {
        goto failed;
    }
    bool footer = false;
    /* Simple rows own inline text until a blank/valid footer. Pandoc 3.11
     * retains heading, quote and fence markers here; the header/caption
     * paragraph-interruption rules do not apply to an existing body. */
    for (size_t i = body; table_source_get(source, i) && !source->lines[i].blanks; i++) {
        end = i;
        if (table_same_dashes(source, delimiter, i)) {
            footer = true;
            break;
        }
    }
    if (source->parser->oom) {
        goto failed;
    }
    if (headerless && !footer) {
        table_simple_search_finish(source, delimiter, end);
    }
    if (end == delimiter || (headerless && !footer) || source->parser->oom) {
        goto failed;
    }
    if (!table_source_columns(source, start) || !table_source_columns(source, body)) {
        goto failed;
    }
    if (!table_set_columns(source, candidate, runs, count, headerless ? body : start, false)) {
        goto failed;
    }
    candidate->first = start;
    candidate->last = end;
    candidate->head_count = headerless ? 0 : 1;
    if (!headerless && !table_rectangular_row(source, candidate, start, start, runs)) {
        goto failed;
    }
    for (size_t i = body; i <= end - (footer ? 1u : 0u); i++) {
        if (!table_rectangular_row(source, candidate, i, i, runs)) {
            goto failed;
        }
    }
    return true;
failed:
    table_candidate_free(source->parser, candidate);
    return false;
}

static bool table_parse_multiline(table_source *source, size_t start, table_candidate *candidate) {
    const table_interval *runs = NULL;
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
    } else if ((count = table_dash_count(source, start)) < 2) {
        goto failed;
    }
    if (table_search_absent(source, delimiter, TABLE_NO_CLOSING_BOUNDARY)) {
        goto failed;
    }
    size_t end = delimiter + 1;
    for (; table_source_get(source, end); end++) {
        if (table_full_boundary(source, end) && (!table_source_get(source, end + 1) || source->lines[end + 1].blanks)) {
            break;
        }
        if (!table_source_columns(source, end)) {
            goto failed;
        }
    }
    if (end >= source->count) {
        table_search_finish(source, delimiter, source->count, TABLE_NO_CLOSING_BOUNDARY);
    }
    if (end >= source->count || end == delimiter + 1) {
        goto failed;
    }
    runs = table_dashes(source, delimiter);
    if (!runs) {
        goto failed;
    }
    candidate->block_content = true;
    candidate->padding_limit = INT_MAX;
    candidate->first = start;
    candidate->last = end;
    candidate->head_count = header ? 1 : 0;
    if (!table_set_columns(source, candidate, runs, count, header ? start + 1 : delimiter + 1, true)) {
        goto failed;
    }
    if (header && !table_rectangular_row(source, candidate, start + 1, delimiter - 1, runs)) {
        goto failed;
    }
    size_t first = delimiter + 1, body_count = 0;
    for (size_t i = first + 1; i <= end; i++) {
        if (i == end || source->lines[i].blanks) {
            if (!table_rectangular_row(source, candidate, first, i - 1, runs)) {
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
    table_candidate_free(source->parser, candidate);
    return false;
}

static int table_grid_root(table_source *source, int *parents, int column) {
    int root = column;
    source->parser->table_scan_work++;
    while (parents[root] != root) {
        source->parser->table_scan_work++;
        root = parents[root];
    }
    while (parents[column] != column) {
        source->parser->table_scan_work++;
        int next = parents[column];
        parents[column] = root;
        column = next;
    }
    return root;
}

static int table_horizontal_bytes(const table_source_line *line, int first, int last) {
    line->parser->table_scan_work += (size_t)(last - first);
    return _scan_table_horizontal(line->data + first, line->data + last);
}

static int table_horizontal(const table_source_line *line, int left, int right) {
    if (left < 0 || right >= line->columns || left >= right) {
        return 0;
    }
    return table_horizontal_bytes(line, table_byte(line, left), table_byte(line, right) + 1);
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

typedef struct {
    size_t top, bottom, left, right, area;
} table_grid_region;

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
                                  size_t rows, table_grid_region **closed, size_t *count, size_t *capacity) {
    if (region.area != (region.bottom - region.top + 1) * (region.right - region.left + 1) ||
        (region.top < candidate->head_count && region.bottom >= candidate->head_count) ||
        (region.top < rows - candidate->foot_count && region.bottom >= rows - candidate->foot_count)) {
        return false;
    }
    if (!table_reserve(source->parser, (void **)closed, capacity, *count + 1, sizeof(**closed))) {
        return false;
    }
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
    size_t *indices = source->parser->mem->calloc(row_count + 1, sizeof(*indices));
    if (!indices) {
        source->parser->oom = true;
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        table_grid_region *region = &regions[i];
        indices[region->top] = 1;
        indices[region->bottom + 1] = 1;
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
    source->parser->mem->free(indices);
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
        source->parser->oom = true;
        return false;
    }
    size_t capacity = 2 * width, active_count = 0, closed_count = 0, closed_capacity = 0;
    int *parents = source->parser->mem->calloc(capacity, sizeof(*parents));
    int *sizes = source->parser->mem->calloc(capacity, sizeof(*sizes));
    int *next = source->parser->mem->calloc(capacity, sizeof(*next));
    int *previous = source->parser->mem->calloc(width, sizeof(*previous));
    table_grid_region *regions = source->parser->mem->calloc(capacity, sizeof(*regions));
    table_grid_region *scratch = source->parser->mem->calloc(width, sizeof(*scratch)), *closed = NULL;
    bool valid = false;
    if (!parents || !sizes || !next || !previous || !regions || !scratch) {
        source->parser->oom = true;
        goto done;
    }
    if (capacity > source->parser->table_frontier_peak) {
        source->parser->table_frontier_peak = capacity;
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
                !table_region_complete(source, candidate, regions[i], row_count, &closed, &closed_count,
                                       &closed_capacity)) {
                goto done;
            }
        }
        memcpy(regions, scratch, survivors * sizeof(*regions));
        active_count = survivors;
    }
    for (size_t i = 0; i < active_count; i++) {
        if (!table_region_complete(source, candidate, regions[i], row_count, &closed, &closed_count,
                                   &closed_capacity)) {
            goto done;
        }
    }
    if (!markdown_core_order_source_entries(source->parser->mem, closed, closed_count, sizeof(*closed),
                                            table_region_source_key)) {
        source->parser->oom = true;
        goto done;
    }
    source->parser->table_scan_work += 16 * closed_count;
    valid = table_grid_rows(source, candidate, columns, boundaries, row_count, closed, closed_count);
done:
    source->parser->mem->free(parents);
    source->parser->mem->free(sizes);
    source->parser->mem->free(next);
    source->parser->mem->free(previous);
    source->parser->mem->free(regions);
    source->parser->mem->free(scratch);
    source->parser->mem->free(closed);
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
    int last = line->length;
    while (last > line->first && (line->data[last - 1] == ' ' || line->data[last - 1] == '\t')) {
        line->parser->table_scan_work++;
        last--;
    }
    if (!table_horizontal_bytes(line, line->first, last) || !table_source_columns(source, index)) {
        return false;
    }
    *left = table_column(line, line->first);
    *right = table_column(line, last - 1);
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
            markdown_core_lookahead_entry *fact = table_search_fact(source, i);
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
    size_t boundary_count = 0, boundary_capacity = 0;
    int left, right;
    if (!table_source_get(source, start) || table_search_absent(source, start, TABLE_NO_GRID) ||
        !table_grid_opening(source, start, &left, &right)) {
        return false;
    }
    parents = source->parser->mem->calloc((size_t)right + 1, sizeof(*parents));
    positions = source->parser->mem->calloc((size_t)right + 1, sizeof(*positions));
    sizes = source->parser->mem->calloc((size_t)right + 1, sizeof(*sizes));
    if (!parents || !positions || !sizes) {
        source->parser->oom = true;
        goto failed;
    }
    for (int i = 0; i <= right; i++) {
        parents[i] = i;
        sizes[i] = 1;
    }
    size_t end = start;
    for (size_t i = start; table_source_get(source, i); i++) {
        if (i > start && source->lines[i].blanks) {
            break;
        }
        if (!table_source_columns(source, i)) {
            goto failed;
        }
        table_source_line *line = &source->lines[i];
        int first = table_character(line, left);
        if (first != '+' && first != '|') {
            break;
        }
        int last = line->columns - 1;
        while (last > left && table_character(line, last) == ' ') {
            last--;
        }
        if (last != right || (table_character(line, right) != '+' && table_character(line, right) != '|')) {
            table_grid_search_finish(source, start, end, left, right, false);
            goto failed;
        }
        int previous = -1;
        bool horizontal = true;
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
    if (source->parser->oom || !table_grid_search_finish(source, start, end, left, right, true) ||
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
    candidate->columns = source->parser->mem->calloc(count - 1, sizeof(*candidate->columns));
    if (!candidate->columns) {
        source->parser->oom = true;
        goto failed;
    }
    for (size_t i = start; i <= end; i++) {
        bool boundary = false;
        for (size_t c = 0; c < count; c++) {
            boundary |= table_character(&source->lines[i], positions[c]) == '+';
        }
        if (boundary) {
            if (!table_reserve(source->parser, (void **)&boundaries, &boundary_capacity, boundary_count + 1,
                               sizeof(*boundaries))) {
                goto failed;
            }
            boundaries[boundary_count++] = i;
        }
    }
    if (boundary_count < 2 || boundaries[boundary_count - 1] != end) {
        goto failed;
    }
    candidate->first = start;
    candidate->last = end;
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
    for (size_t c = 0; c + 1 < count; c++) {
        bool l = table_character(alignment, positions[c] + 1) == ':',
             r = table_character(alignment, positions[c + 1] - 1) == ':';
        candidate->columns[c].alignment =
            l ? (r ? MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER : MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT)
              : (r ? MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT : MARKDOWN_CORE_TABLE_ALIGNMENT_NONE);
        candidate->columns[c].relative =
            (markdown_core_optional_double){true, (positions[c + 1] - positions[c] - 1) / total};
    }
    source->parser->mem->free(sizes);
    source->parser->mem->free(parents);
    source->parser->mem->free(positions);
    source->parser->mem->free(boundaries);
    return true;
failed:
    source->parser->mem->free(sizes);
    source->parser->mem->free(parents);
    source->parser->mem->free(positions);
    source->parser->mem->free(boundaries);
    table_candidate_free(source->parser, candidate);
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
    table_row *header = row_from_string(&MARKDOWN_CORE_EXTENSION_TABLE, source->parser,
                                        (unsigned char *)head->data + head->first, head->input_length - head->first);
    table_row *markers = row_from_string(&MARKDOWN_CORE_EXTENSION_TABLE, source->parser,
                                         (unsigned char *)delimiter->data + delimiter->first,
                                         delimiter->input_length - delimiter->first);
    bool matches = header && markers && header->n_columns == markers->n_columns;
    if (!matches) {
        goto done;
    }
    candidate->column_count = header->n_columns;
    candidate->columns = source->parser->mem->calloc(candidate->column_count, sizeof(*candidate->columns));
    if (!candidate->columns) {
        source->parser->oom = true;
        matches = false;
        goto done;
    }
    candidate->first = start;
    candidate->last = start + 1;
    candidate->head_count = 1;
    candidate->open = true;
    if (!table_add_row(source, candidate, start, start)) {
        matches = false;
        goto done;
    }
    for (size_t i = 0; i < candidate->column_count; i++) {
        const markdown_core_chunk *marker = &markers->cells[i].content;
        bool l = marker->data[0] == ':', r = marker->data[marker->len - 1] == ':';
        candidate->columns[i].alignment =
            l ? (r ? MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER : MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT)
              : (r ? MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT : MARKDOWN_CORE_TABLE_ALIGNMENT_NONE);
        node_cell *cell = &header->cells[i];
        int from = head->first + cell->start_offset, to = head->first + cell->end_offset + 1;
        if (!table_add_cell(source, candidate, start, start, table_column(head, from), table_column(head, to), from + 1,
                            to)) {
            matches = false;
            goto done;
        }
    }
done:
    free_table_row(source->parser->mem, header);
    free_table_row(source->parser->mem, markers);
    if (!matches) {
        table_candidate_free(source->parser, candidate);
    }
    return matches;
}

static bool table_parse_candidate(table_source *source, size_t start, table_candidate *candidate, bool pipe) {
    if (!table_source_get(source, start) || source->lines[start].indent >= 4) {
        return false;
    }
    bool boundary = table_full_boundary(source, start);
    if (table_parse_grid(source, start, candidate) || (boundary && table_parse_multiline(source, start, candidate)) ||
        table_parse_simple(source, start, candidate) ||
        (!boundary && table_parse_multiline(source, start, candidate))) {
        return true;
    }
    return pipe && table_parse_pipe_header(source, start, candidate);
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
    for (int column = left; column < right && !parser->oom;) {
        int byte = table_byte(line, column);
        if (line->data[byte] == '\t') {
            int original = markdown_core_parser_source_column(parser, line->line, byte + 1);
            markdown_core_parser_append_content_mark(parser, node, node->content.size, line->line, original, 1, 0);
            markdown_core_strbuf_putc(&node->content, ' ');
            column++;
        } else if (escapes && column + 1 < right && table_character(line, column) == '\\' &&
                   table_character(line, column + 1) == '|') {
            int first = markdown_core_parser_source_column(parser, line->line, byte + 1);
            int end = markdown_core_parser_source_column(parser, line->line, byte + 2);
            markdown_core_parser_append_content_mark(parser, node, node->content.size, line->line, first,
                                                     end - first + 1, end - first + 1);
            markdown_core_strbuf_putc(&node->content, '|');
            column += 2;
        } else {
            int end = column + 1;
            while (end < right && line->data[table_byte(line, end)] != '\t' &&
                   !(escapes && table_character(line, end) == '\\' && table_character(line, end + 1) == '|')) {
                end++;
            }
            int length = table_byte(line, end) - byte;
            markdown_core_parser_append_source_marks(parser, node, line->line, byte + 1, length, node->content.size);
            markdown_core_strbuf_put(&node->content, line->data + byte, length);
            column = end;
        }
    }
    if (node->content.oom) {
        parser->oom = true;
    }
}

static void table_append_newline(table_source *source, markdown_core_node *node, size_t index) {
    table_source_line *line = &source->lines[index];
    markdown_core_parser_append_source_marks(source->parser, node, line->line, line->length + 1, 1, node->content.size);
    markdown_core_strbuf_putc(&node->content, '\n');
    if (node->content.oom) {
        source->parser->oom = true;
    }
}

static void table_fill_cell(table_source *source, markdown_core_node *node, const table_source_cell *cell, bool blocks,
                            int padding_limit) {
    int padding = padding_limit;
    for (size_t i = cell->first; i <= cell->last; i++) {
        table_source_line *line = &source->lines[i];
        int end = cell->right < line->columns ? cell->right : line->columns;
        int first = cell->left;
        while (first < end && table_character(line, first) == ' ') {
            first++;
        }
        if (first < end && first - cell->left < padding) {
            padding = first - cell->left;
        }
    }
    for (size_t i = cell->first; i <= cell->last && !source->parser->oom; i++) {
        table_source_line *line = &source->lines[i];
        int first = cell->left, end = cell->right < line->columns ? cell->right : line->columns;
        while (end > first && table_character(line, end - 1) == ' ') {
            end--;
        }
        if (blocks) {
            first += padding < end - first ? padding : end - first;
        } else {
            while (first < end && table_character(line, first) == ' ') {
                first++;
            }
        }
        table_append_range(source, node, i, first, end, !blocks);
        table_append_newline(source, node, i);
    }
    if (blocks && !source->parser->oom) {
        markdown_core_parser_queue_block_input(source->parser, node);
    }
}

static markdown_core_node *table_child(markdown_core_parser *parser, markdown_core_node *parent,
                                       markdown_core_node_type kind, int first_line, int first_column, int last_line,
                                       int last_column) {
    markdown_core_node *node = markdown_core_node_new_with_mem(kind, parser->mem);
    if (!node) {
        parser->oom = true;
        return NULL;
    }
    if (kind == MARKDOWN_CORE_NODE_TABLE_ROW || kind == MARKDOWN_CORE_NODE_TABLE_CELL) {
        markdown_core_node_set_extension(node, &MARKDOWN_CORE_EXTENSION_TABLE);
    }
    node->start_line = first_line;
    node->end_line = last_line;
    node->start_column = markdown_core_parser_source_column(parser, first_line, first_column);
    node->end_column = markdown_core_parser_source_column(parser, last_line, last_column);
    if (parent && !markdown_core_node_append_child(parent, node)) {
        markdown_core_node_free(node);
        parser->oom = true;
        return NULL;
    }
    return node;
}

static markdown_core_node *table_build(table_source *source, markdown_core_node *parent, table_candidate *candidate) {
    markdown_core_parser *parser = source->parser;
    table_source_line *first = &source->lines[candidate->first], *last = &source->lines[candidate->last];
    markdown_core_node *node =
        markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_TABLE, source->lines[0].first + 1);
    if (!node) {
        return NULL;
    }
    markdown_core_node_set_extension(node, &MARKDOWN_CORE_EXTENSION_TABLE);
    node->opaque = parser->mem->calloc(1, sizeof(markdown_core_table));
    if (!node->opaque) {
        parser->oom = true;
        return node;
    }
    markdown_core_table *table = node->opaque;
    table->columns = candidate->columns;
    candidate->columns = NULL;
    table->column_count = candidate->column_count;
    table->head_count = candidate->head_count;
    table->foot_count = candidate->foot_count;
    table->content_count = candidate->row_count - table->head_count - table->foot_count;
    node->start_line = first->line;
    node->start_column = markdown_core_parser_source_column(parser, first->line, first->offset + 1);
    node->end_line = last->line;
    node->end_column = markdown_core_parser_source_column(parser, last->line, last->length);
    if (!candidate->open) {
        node->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    }
    for (size_t i = 0; i < candidate->row_count && !parser->oom; i++) {
        table_source_row *row = &candidate->rows[i];
        table_source_line *begin = &source->lines[row->first], *end = &source->lines[row->last];
        markdown_core_node *row_node = table_child(parser, node, MARKDOWN_CORE_NODE_TABLE_ROW, begin->line,
                                                   begin->offset + 1, end->line, end->length);
        if (!row_node) {
            break;
        }
        for (size_t j = 0; j < row->count && !parser->oom; j++) {
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
            table_fill_cell(source, cell_node, cell, candidate->block_content, candidate->padding_limit);
        }
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
    for (size_t i = 0; i <= last && !source->parser->oom; i++) {
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
static bool table_after_caption(table_source *source, size_t *caption_last, table_candidate *candidate,
                                bool after_blank) {
    size_t next = 1;
    for (; table_source_get(source, next); next++) {
        if (table_parse_candidate(source, next, candidate, true)) {
            return true;
        }
        table_source_line *line = &source->lines[next];
        if (line->blanks || table_has_block_start(source, next, true) || source->parser->oom) {
            break;
        }
        *caption_last = next;
    }
    return after_blank && table_source_get(source, next) && source->lines[next].blanks &&
           table_parse_candidate(source, next, candidate, true);
}

bool markdown_core_table_caption_probe(markdown_core_block_lookahead *lookahead, markdown_core_chunk *input, int first,
                                       int indent) {
    markdown_core_parser *parser = lookahead->parser;
    table_source source = {.parser = parser, .lookahead = *lookahead, .lines = parser->table_lines};
    lookahead->active = false; /* Transfer the transaction; source_free ends it. */
    table_candidate candidate = {0};
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
        matched = table_after_caption(&source, &last, &candidate, true);
    }
    table_candidate_free(parser, &candidate);
    table_source_free(&source);
    return matched;
}

markdown_core_node *markdown_core_table_try_open(markdown_core_parser *parser, markdown_core_node *parent,
                                                 unsigned char *input, int length) {
    if (parser->indent > 3 || parser->blank || parent->kind == MARKDOWN_CORE_NODE_TABLE ||
        parent->kind == MARKDOWN_CORE_NODE_TABLE_ROW || parent->kind == MARKDOWN_CORE_NODE_PARAGRAPH) {
        return NULL;
    }
    /* Every opening grammar needs a later physical line. At EOF only an
     * existing eligible table can claim a trailing caption. */
    if (parser->lookahead_cursor == parser->lookahead_end &&
        (!parent->last_child || parent->last_child->kind != MARKDOWN_CORE_NODE_TABLE)) {
        return NULL;
    }
    table_source source = {.parser = parser, .lines = parser->table_lines};
    table_candidate candidate = {0};
    markdown_core_node *result = NULL;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_TABLE, &source.lookahead)) {
        return NULL;
    }
    if (!table_source_push(&source, (table_source_line){.data = input,
                                                        .length = length,
                                                        .offset = parser->offset,
                                                        .first = parser->first_nonspace,
                                                        .first_column = parser->first_nonspace_column,
                                                        .indent = parser->indent,
                                                        .line = parser->line_number,
                                                        .after = parser->lookahead_cursor})) {
        goto done;
    }
    int caption = table_caption_start(source.lines[0].data, source.lines[0].length, source.lines[0].first,
                                      source.lines[0].indent);
    size_t caption_last = 0;
    markdown_core_node *preceding = parent->last_child;
    bool trailing = caption >= 0 && preceding && preceding->kind == MARKDOWN_CORE_NODE_TABLE && preceding->opaque &&
                    !((markdown_core_table *)preceding->opaque)->caption;
    bool matched = false;
    if (caption >= 0) {
        matched = table_after_caption(&source, &caption_last, &candidate, !trailing);
    } else {
        matched = table_parse_candidate(&source, 0, &candidate, false);
    }
    markdown_core_parser_lookahead_end(&source.lookahead);
    if (parser->oom || (!matched && !trailing)) {
        goto done;
    }
    markdown_core_parser_finalize_unmatched_blocks(parser);
    if (parser->oom) {
        goto done;
    }
    if (trailing) {
        result = preceding;
        table_candidate_free(parser, &candidate);
        ((markdown_core_table *)result->opaque)->caption = table_caption_build(&source, caption_last, caption);
        result->end_line = source.lines[caption_last].line;
        result->end_column =
            markdown_core_parser_source_column(parser, result->end_line, source.lines[caption_last].length);
        parser->claimed_cursor = source.lines[caption_last].after;
        parser->claimed_line = result->end_line;
        parser->claimed_last_column = result->end_column;
    } else {
        result = table_build(&source, parent, &candidate);
        if (result && result->opaque && caption >= 0) {
            ((markdown_core_table *)result->opaque)->caption = table_caption_build(&source, caption_last, caption);
            result->start_line = source.lines[0].line;
            result->start_column =
                markdown_core_parser_source_column(parser, result->start_line, source.lines[0].first + 1);
        }
        if (result) {
            parser->claimed_cursor = source.lines[candidate.last].after;
            parser->claimed_line = source.lines[candidate.last].line;
            parser->claimed_last_column =
                markdown_core_parser_source_column(parser, parser->claimed_line, source.lines[candidate.last].length);
        }
    }
done:
    table_candidate_free(parser, &candidate);
    table_source_free(&source);
    return result;
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
    .visit_owned_subtrees_func = visit_owned_subtrees,
};
