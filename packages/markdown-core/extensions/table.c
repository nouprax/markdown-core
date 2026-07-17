#include <markdown-core-extension-api.h>
#include <inlines.h>
#include <parser.h>
#include <references.h>
#include <string.h>

#include "ext_scanners.h"
#include "strikethrough.h"
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
    mem->free(cell->buf);
}

static void free_row_cells(markdown_core_mem *mem, table_row *row) {
    while (row->n_columns > 0) {
        free_table_cell(mem, &row->cells[--row->n_columns]);
    }
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

static void free_node_table(markdown_core_mem *mem, void *ptr) {
    node_table *t = (node_table *)ptr;
    mem->free(t->alignments);
    mem->free(t);
}

static void free_node_table_row(markdown_core_mem *mem, void *ptr) { mem->free(ptr); }

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

static markdown_core_strbuf *unescape_pipes(markdown_core_mem *mem, unsigned char *string, bufsize_t len) {
    markdown_core_strbuf *res = (markdown_core_strbuf *)mem->calloc(1, sizeof(markdown_core_strbuf));
    bufsize_t r, w;

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

static table_row *row_from_string(markdown_core_extension *self, markdown_core_parser *parser, unsigned char *string,
                                  int len) {
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
            markdown_core_strbuf *cell_buf = unescape_pipes(parser->mem, string + offset, cell_matched);
            if (!cell_buf) {
                parser->oom = true;
                int_overflow_abort = 1;
                break;
            }
            if (cell_buf->oom) {
                parser->oom = true;
                int_overflow_abort = 1;
                markdown_core_strbuf_free(cell_buf);
                parser->mem->free(cell_buf);
                break;
            }
            markdown_core_strbuf_trim(cell_buf);

            {
                int cell_oom = 0;
                node_cell *cell = append_row_cell(parser->mem, row, &cell_oom);
                if (cell_oom) {
                    parser->oom = true;
                }
                if (!cell) {
                    int_overflow_abort = 1;
                    markdown_core_strbuf_free(cell_buf);
                    parser->mem->free(cell_buf);
                    break;
                }
                cell->buf = cell_buf;
                cell->start_offset = offset;
                cell->end_offset = offset + cell_matched - 1;
                cell->internal_offset = 0;

                while (cell->start_offset > row->paragraph_offset && string[cell->start_offset - 1] != '|') {
                    --cell->start_offset;
                    ++cell->internal_offset;
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

static void try_inserting_table_header_paragraph(markdown_core_parser *parser, markdown_core_node *parent_container,
                                                 unsigned char *parent_string, int paragraph_offset) {
    markdown_core_node *paragraph;
    markdown_core_strbuf *paragraph_content;

    paragraph = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, parser->mem);

    paragraph_content = unescape_pipes(parser->mem, parent_string, paragraph_offset);
    if (!paragraph_content) {
        markdown_core_node_free(paragraph);
        return;
    }
    markdown_core_strbuf_trim(paragraph_content);
    markdown_core_node_set_string_content(paragraph, (char *)paragraph_content->ptr);
    markdown_core_strbuf_free(paragraph_content);
    parser->mem->free(paragraph_content);

    if (!markdown_core_node_insert_before(parent_container, paragraph)) {
        parser->mem->free(paragraph);
    }
}

static markdown_core_node *try_opening_table_header(markdown_core_extension *self, markdown_core_parser *parser,
                                                    markdown_core_node *parent_container, unsigned char *input,
                                                    int len) {
    markdown_core_node *table_header;
    table_row *header_row = NULL;
    table_row *delimiter_row = NULL;
    node_table_row *ntr;
    const char *parent_string;
    uint16_t i;

    if (parent_container->flags & MARKDOWN_CORE_NODE__TABLE_VISITED) {
        return parent_container;
    }

    if (!scan_table_start(input, len, markdown_core_parser_get_first_nonspace(parser))) {
        return parent_container;
    }

    // Since scan_table_start was successful, we must have a delimiter row.
    delimiter_row = row_from_string(self, parser, input + markdown_core_parser_get_first_nonspace(parser),
                                    len - markdown_core_parser_get_first_nonspace(parser));
    // assert may be optimized out, don't rely on it for security boundaries
    if (!delimiter_row) {
        return parent_container;
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
        return parent_container;
    }

    if (!markdown_core_node_set_type(parent_container, MARKDOWN_CORE_NODE_TABLE)) {
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }

    if (header_row->paragraph_offset) {
        try_inserting_table_header_paragraph(parser, parent_container, (unsigned char *)parent_string,
                                             header_row->paragraph_offset);
        /* The split-off paragraph shares the retyped node's original span, so
         * the table's first line no longer begins a document child: it must
         * not remain an incremental restart point. */
        parent_container->flags &= ~(markdown_core_node_internal_flags)MARKDOWN_CORE_NODE__CLEAN_START;
    }

    /* The paragraph is already rewritten into a table node here.  On
     * allocation failure the half-converted node stays behind with a NULL
     * payload -- every table helper tolerates that -- and the sticky flag
     * makes the parse fail, so nothing downstream trusts the node. */
    markdown_core_node_set_extension(parent_container, self);
    parent_container->as.opaque = parser->mem->calloc(1, sizeof(node_table));
    if (!parent_container->as.opaque) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    set_n_table_columns(parent_container, header_row->n_columns);

    // allocate alignments based on delimiter_row->n_columns
    // since we populate the alignments array based on delimiter_row->cells
    uint8_t *alignments = (uint8_t *)parser->mem->calloc(delimiter_row->n_columns, sizeof(uint8_t));
    if (!alignments) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
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

    table_header = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                  parent_container->start_column);
    if (!table_header) {
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    markdown_core_node_set_extension(table_header, self);
    table_header->end_column = parent_container->start_column + (int)strlen(parent_string) - 2;
    table_header->start_line = table_header->end_line = parent_container->start_line;

    table_header->as.opaque = ntr = (node_table_row *)parser->mem->calloc(1, sizeof(node_table_row));
    if (!ntr) {
        parser->oom = true;
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    ntr->is_header = true;

    for (i = 0; i < header_row->n_columns; ++i) {
        node_cell *cell = &header_row->cells[i];
        markdown_core_node *header_cell = markdown_core_parser_add_child(
            parser, table_header, MARKDOWN_CORE_NODE_TABLE_CELL, parent_container->start_column + cell->start_offset);
        if (!header_cell) {
            break;
        }
        header_cell->start_line = header_cell->end_line = parent_container->start_line;
        header_cell->internal_offset = cell->internal_offset;
        header_cell->end_column = parent_container->start_column + cell->end_offset;
        markdown_core_node_set_string_content(header_cell, (char *)cell->buf->ptr);
        markdown_core_node_set_extension(header_cell, self);
        set_cell_index(header_cell, i);
    }

    incr_table_row_count(parent_container, i);

    markdown_core_parser_advance_offset(
        parser, (char *)input, (int)strlen((char *)input) - 1 - markdown_core_parser_get_offset(parser), false);

    free_table_row(parser->mem, header_row);
    free_table_row(parser->mem, delimiter_row);
    return parent_container;
}

static markdown_core_node *try_opening_table_row(markdown_core_extension *self, markdown_core_parser *parser,
                                                 markdown_core_node *parent_container, unsigned char *input, int len) {
    markdown_core_node *table_row_block;
    table_row *row;

    if (markdown_core_parser_is_blank(parser)) {
        return NULL;
    }

    if (get_n_autocompleted_cells(parent_container) > MAX_AUTOCOMPLETED_CELLS) {
        return NULL;
    }

    table_row_block = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_TABLE_ROW,
                                                     parent_container->start_column);
    if (!table_row_block) {
        return NULL;
    }
    markdown_core_node_set_extension(table_row_block, self);
    table_row_block->end_column = parent_container->end_column;
    table_row_block->as.opaque = parser->mem->calloc(1, sizeof(node_table_row));
    if (!table_row_block->as.opaque) {
        parser->oom = true;
        markdown_core_node_free(table_row_block);
        return NULL;
    }

    row = row_from_string(self, parser, input + markdown_core_parser_get_first_nonspace(parser),
                          len - markdown_core_parser_get_first_nonspace(parser));

    if (!row) {
        // clean up the dangling node
        markdown_core_node_free(table_row_block);
        return NULL;
    }

    {
        int i, table_columns = get_n_table_columns(parent_container);

        for (i = 0; i < row->n_columns && i < table_columns; ++i) {
            node_cell *cell = &row->cells[i];
            markdown_core_node *node =
                markdown_core_parser_add_child(parser, table_row_block, MARKDOWN_CORE_NODE_TABLE_CELL,
                                               parent_container->start_column + cell->start_offset);
            if (!node) {
                break;
            }
            node->internal_offset = cell->internal_offset;
            node->end_column = parent_container->start_column + cell->end_offset;
            markdown_core_node_set_string_content(node, (char *)cell->buf->ptr);
            markdown_core_node_set_extension(node, self);
            set_cell_index(node, i);
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

    markdown_core_parser_advance_offset(parser, (char *)input, len - 1 - markdown_core_parser_get_offset(parser),
                                        false);

    return table_row_block;
}

static markdown_core_node *try_opening_table_block(markdown_core_extension *self, int indented,
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

static int matches(markdown_core_extension *self, markdown_core_parser *parser, unsigned char *input, int len,
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

static int can_contain(markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
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

static void opaque_alloc(markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    /* A NULL payload is tolerated by every table property helper; the node
     * then reports zero columns/alignments. */
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        node->as.opaque = mem->calloc(1, sizeof(node_table));
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        node->as.opaque = mem->calloc(1, sizeof(node_table_row));
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        node->as.opaque = mem->calloc(1, sizeof(node_cell));
    }
}

static void opaque_free(markdown_core_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        free_node_table(mem, node->as.opaque);
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        free_node_table_row(mem, node->as.opaque);
    }
}

static const markdown_core_extension table_extension = {
    .name = "table",
    .last_block_matches = matches,
    .try_opening_block = try_opening_table_block,
    .get_type_string = get_type_string,
    .can_contain = can_contain,
    .contains_inlines = contains_inlines,
    .alloc_opaque = opaque_alloc,
    .free_opaque = opaque_free,
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

int markdown_core_extensions_set_table_columns(markdown_core_node *node, uint16_t n_columns) {
    return set_n_table_columns(node, n_columns);
}

int markdown_core_extensions_set_table_alignments(markdown_core_node *node, uint16_t ncols, uint8_t *alignments) {
    uint8_t *a = (uint8_t *)markdown_core_node_mem(node)->calloc(1, ncols);
    if (!a) {
        return 0;
    }
    memcpy(a, alignments, ncols);
    return set_table_alignments(node, a);
}

int markdown_core_extensions_get_table_row_is_header(markdown_core_node *node) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE_ROW || !node->as.opaque) {
        return 0;
    }

    return ((node_table_row *)node->as.opaque)->is_header;
}

int markdown_core_extensions_set_table_row_is_header(markdown_core_node *node, int is_header) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE_ROW || !node->as.opaque) {
        return 0;
    }

    ((node_table_row *)node->as.opaque)->is_header = (is_header != 0);
    return 1;
}
