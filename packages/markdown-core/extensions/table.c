#include <markdown-core-extension-api.h>
#include "syntax_extension.h"
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

/* A cell -- and the header row -- is COMPLETE the moment it is built, because
 * a GFM row is one line. Neither is ever on the open spine, so `finalize`, the
 * only other clearer of `__OPEN`, never reaches them: without this, `finish`
 * returned a tree still carrying open blocks, and the flag is the closed
 * signal projections schedule on (§12.8 Q3). A body ROW stays open here -- it
 * is the block the parser returns to the spine, and `finalize` closes it.
 *
 * The one exception is the parser's own anchor: when a cell's allocation is
 * lost, `add_child` re-anchors `parser->current` at the nearest open ancestor,
 * which can be the header row being built, and the spine's finalize walk then
 * expects to close it itself. Such a parse fails at `finish`, so the flag in
 * that tree is never read. */
static void close_built_block(markdown_core_parser *parser, markdown_core_node *node) {
    if (parser->current == node) {
        return;
    }
    node->flags &= ~MARKDOWN_CORE_NODE__OPEN;
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

static table_row *row_from_string(
    const markdown_core_syntax_extension *self,
    markdown_core_parser *parser,
    unsigned char *string,
    int len
) {
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

/* Mark a cell's content with the place its first byte was written, read out of
 * the row's own map. */
static void S_mark_cell_content(
    markdown_core_parser *parser,
    markdown_core_node *owner,
    markdown_core_node *cell,
    bufsize_t content_offset
) {
    int line, column;

    if (markdown_core_parser_content_place(parser, owner, content_offset, &line, &column)) {
        markdown_core_parser_mark_content(parser, cell, line, column);
    }
}

/* Give `node` the source span of [start_offset, end_offset] in `owner`'s
 * content buffer. Every table position recovered from that buffer goes through
 * here, so there is one place that knows a content offset is not a column. */
static void S_place_content_span(
    markdown_core_parser *parser,
    markdown_core_node *owner,
    markdown_core_node *node,
    bufsize_t start_offset,
    bufsize_t end_offset
) {
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

static void try_inserting_table_header_paragraph(
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *parent_string,
    int paragraph_offset
) {
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
    if (paragraph) {
        /* Born outside `add_child`, so stamped here or not at all (T3), and
         * minted here for the same reason (T2). */
        markdown_core_parser_touch(parser, paragraph);
        markdown_core_parser_mint_block_id(parser, paragraph);
    }
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
        return;
    }
    /* D4's fork 1 (§4): the lead is the text the reader saw FIRST and which
     * did not change, so it keeps the identity of the paragraph the table was
     * split out of; the table -- the element that is new to the consumer --
     * leaves with the lead's fresh mint. Swapped, not copied, so the ids stay
     * unique. `parent_container` is that paragraph, already retyped in place. */
    {
        uint32_t fresh = paragraph->identifier;
        paragraph->identifier = parent_container->identifier;
        parent_container->identifier = fresh;
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
static markdown_core_node *try_opening_table_header(
    const markdown_core_syntax_extension *self,
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
    int header_line, header_column;

    if (parent_container->flags & MARKDOWN_CORE_NODE__TABLE_VISITED) {
        return NULL;
    }

    if (!scan_table_start(input, len, markdown_core_parser_get_first_nonspace(parser))) {
        return NULL;
    }

    // Since scan_table_start was successful, we must have a delimiter row.
    delimiter_row = row_from_string(
        self,
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
    header_row = row_from_string(self, parser, (unsigned char *)parent_string, (int)strlen(parent_string));
    if (!header_row || header_row->n_columns != delimiter_row->n_columns) {
        /* A DELIMITER ROW WAS FOUND AND THE HEADER ABOVE IT DOES NOT MATCH, so
         * the paragraph stays a paragraph and reads exactly like prose that
         * happens to contain pipes. The delimiter row is the evidence -- it is
         * a row of nothing but `-`, `:` and `|`, which nobody writes by
         * accident -- and it is also the line the table would have opened on,
         * so it is the place a reader is sent to.
         *
         * `MARKDOWN_CORE_NODE__TABLE_VISITED` below is what keeps this to one
         * report per paragraph rather than one per line of it. */
        markdown_core_parser_diagnose_line(
            parser,
            MARKDOWN_CORE_DIAGNOSTIC_WARNING,
            MARKDOWN_CORE_DIAGNOSTIC_TABLE_REJECTED,
            input,
            (bufsize_t)len,
            (bufsize_t)markdown_core_parser_get_first_nonspace(parser),
            "the delimiter row's column count does not match the header row's",
            NULL,
            0
        );
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
    /* A retype is a write to the block (T3); the node object survives it. */
    markdown_core_parser_touch(parser, parent_container);

    if (header_row->paragraph_offset) {
        try_inserting_table_header_paragraph(
            parser,
            parent_container,
            (unsigned char *)parent_string,
            header_row->paragraph_offset
        );
        /* The table starts where its HEADER ROW was written, not where the
         * paragraph it was split out of did. Taken before the row and cells
         * below read start_column, because they are placed against it. */
        if (markdown_core_parser_content_place(
                parser,
                parent_container,
                header_row->paragraph_offset,
                &header_line,
                &header_column
            )) {
            parent_container->start_line = header_line;
            parent_container->start_column = header_column;
        }
    }

    /* The paragraph is already rewritten into a table node here.  On
     * allocation failure the half-converted node stays behind with a NULL
     * payload -- every table helper tolerates that -- and the sticky flag
     * makes the parse fail, so nothing downstream trusts the node. */
    markdown_core_node_set_syntax_extension(parent_container, self);
    // From here down the node IS a table, so every remaining
    // `return parent_container` means "opened, then failed" rather than
    // "declined". Do not turn these into NULL with the six above it.
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

    table_header = markdown_core_parser_add_child(
        parser,
        parent_container,
        MARKDOWN_CORE_NODE_TABLE_ROW,
        parent_container->start_column
    );
    if (!table_header) {
        free_table_row(parser->mem, header_row);
        free_table_row(parser->mem, delimiter_row);
        return parent_container;
    }
    markdown_core_node_set_syntax_extension(table_header, self);
    /* The header row and its cells are RECOVERED from the paragraph's content
     * buffer, and every offset below is an offset into that buffer. Adding one
     * to a column is only right while the buffer holds a single line starting
     * where the block does; with a lead split off, `| a | b |` on line three
     * was reported at 1:10, a column that is not on line one. The map turns
     * each offset back into the place it was written. */
    S_place_content_span(
        parser,
        parent_container,
        table_header,
        header_row->paragraph_offset,
        (bufsize_t)strlen(parent_string) - 2
    );

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
            parser,
            table_header,
            MARKDOWN_CORE_NODE_TABLE_CELL,
            parent_container->start_column + cell->start_offset
        );
        if (!header_cell) {
            break;
        }
        header_cell->internal_offset = cell->internal_offset;
        S_place_content_span(parser, parent_container, header_cell, cell->start_offset, cell->end_offset);
        /* The cell's content was SET from `cell->buf`, so it has no marks of
         * its own and every inline inside it would be placed by arithmetic on
         * the cell's start column. It begins where its first content byte was
         * written; one mark says so, because a cell is one line long. */
        S_mark_cell_content(parser, parent_container, header_cell, cell->start_offset + cell->internal_offset);
        markdown_core_node_set_string_content(header_cell, (char *)cell->buf->ptr);
        markdown_core_node_set_syntax_extension(header_cell, self);
        set_cell_index(header_cell, i);
        close_built_block(parser, header_cell);
    }
    close_built_block(parser, table_header);

    incr_table_row_count(parent_container, i);

    markdown_core_parser_advance_offset(
        parser,
        (char *)input,
        (int)strlen((char *)input) - 1 - markdown_core_parser_get_offset(parser),
        false
    );

    free_table_row(parser->mem, header_row);
    free_table_row(parser->mem, delimiter_row);
    return parent_container;
}

static markdown_core_node *try_opening_table_row(
    const markdown_core_syntax_extension *self,
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
    markdown_core_node_set_syntax_extension(table_row_block, self);
    table_row_block->end_column = parent_container->end_column;
    table_row_block->as.opaque = parser->mem->calloc(1, sizeof(node_table_row));
    if (!table_row_block->as.opaque) {
        parser->oom = true;
        markdown_core_node_free(table_row_block);
        return NULL;
    }

    row = row_from_string(
        self,
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
            /* A body row is not fed through `add_line`, so there is no run to
             * read: the line is the one in hand and the column is where the
             * cell's content starts on it. */
            markdown_core_parser_mark_content(
                parser,
                node,
                markdown_core_parser_get_line_number(parser),
                markdown_core_parser_get_first_nonspace(parser) + 1 + cell->start_offset + cell->internal_offset
            );
            markdown_core_node_set_syntax_extension(node, self);
            set_cell_index(node, i);
            close_built_block(parser, node);
        }

        incr_table_row_count(parent_container, i);

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
            markdown_core_node *node = markdown_core_parser_add_child(
                parser,
                table_row_block,
                MARKDOWN_CORE_NODE_TABLE_CELL,
                (int)completed_at
            );
            if (!node) {
                break;
            }
            node->end_column = (int)completed_at;
            markdown_core_node_set_syntax_extension(node, self);
            set_cell_index(node, i);
            close_built_block(parser, node);
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
    const markdown_core_syntax_extension *self,
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

/* The accept/reject half of `row_from_string`, with nothing materialized
 * (#137): `matches` runs on EVERY line inside an open table and used to
 * build a full row -- a calloc'd strbuf per cell, filled by
 * `unescape_pipes` -- only to free it all and answer a boolean, then
 * `try_opening_table_row` immediately reparsed the same bytes for real.
 * The decision never reads a cell's bytes: it is the scanners' walk, the
 * column count, the paragraph-offset reset, and the end-of-input test, and
 * this function is that walk verbatim -- same scans in the same order,
 * same UINT16_MAX column cap, same rejects -- so accept/reject stays
 * byte-identical while the builder stays the one place a row is made.
 * (The builder's allocation-failure aborts have no counterpart here: this
 * walk allocates nothing, and a lost allocation still poisons the parse at
 * the build that follows a match.) */
static int row_matches(unsigned char *string, int len) {
    bufsize_t cell_matched = 1, pipe_matched = 1, offset;
    int expect_more_cells = 1;
    int row_end_offset = 0;
    uint32_t n_columns = 0;

    // Scan past the (optional) leading pipe.
    offset = scan_table_cell_end(string, len, 0);

    while (offset < len && expect_more_cells) {
        cell_matched = scan_table_cell(string, len, offset);
        pipe_matched = scan_table_cell_end(string, len, offset + cell_matched);

        if (cell_matched || pipe_matched) {
            if (n_columns + 1 > UINT16_MAX) {
                return 0;
            }
            n_columns++;
        }

        offset += cell_matched + pipe_matched;

        if (pipe_matched) {
            expect_more_cells = 1;
        } else {
            row_end_offset = scan_table_row_end(string, len, offset);
            offset += row_end_offset;

            if (row_end_offset && offset != len) {
                n_columns = 0;
                offset += scan_table_cell_end(string, len, offset);
                expect_more_cells = 1;
            } else {
                expect_more_cells = 0;
            }
        }
    }

    return offset == len && n_columns != 0;
}

static int matches(
    const markdown_core_syntax_extension *self,
    markdown_core_parser *parser,
    unsigned char *input,
    int len,
    markdown_core_node *parent_container
) {
    int res = 0;
    (void)self;

    if (markdown_core_node_get_type(parent_container) == MARKDOWN_CORE_NODE_TABLE) {
        res = row_matches(
            input + markdown_core_parser_get_first_nonspace(parser),
            len - markdown_core_parser_get_first_nonspace(parser)
        );
    }

    return res;
}

static const char *get_type_string(const markdown_core_syntax_extension *self, markdown_core_node *node) {
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
    const markdown_core_syntax_extension *extension,
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

static int contains_inlines(const markdown_core_syntax_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_TABLE_CELL;
}

static void opaque_free(const markdown_core_syntax_extension *self, markdown_core_mem *mem, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        free_node_table(mem, node->as.opaque);
    } else if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        free_node_table_row(mem, node->as.opaque);
    }
}

/* The AST derivation clones the block skeleton, and these three types keep
 * their state in `node.as`: the table its column count and OWNED alignments
 * array, the row its header bit, the cell its index -- which is a plain union
 * arm, not an opaque payload, and would otherwise be lost to a zeroed node. */
static int opaque_copy(
    const markdown_core_syntax_extension *self,
    markdown_core_mem *mem,
    markdown_core_node *dst,
    const markdown_core_node *src
) {
    if (src->type == MARKDOWN_CORE_NODE_TABLE) {
        const node_table *from = (const node_table *)src->as.opaque;
        node_table *to;
        if (!from) {
            return 1;
        }
        to = (node_table *)mem->calloc(1, sizeof(*to));
        if (!to) {
            return 0;
        }
        *to = *from;
        to->alignments = NULL;
        if (from->alignments && from->n_columns > 0) {
            to->alignments = (uint8_t *)mem->calloc(from->n_columns, sizeof(uint8_t));
            if (!to->alignments) {
                mem->free(to);
                return 0;
            }
            memcpy(to->alignments, from->alignments, from->n_columns);
        }
        dst->as.opaque = to;
    } else if (src->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        const node_table_row *from = (const node_table_row *)src->as.opaque;
        node_table_row *to;
        if (!from) {
            return 1;
        }
        to = (node_table_row *)mem->calloc(1, sizeof(*to));
        if (!to) {
            return 0;
        }
        *to = *from;
        dst->as.opaque = to;
    } else if (src->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        dst->as.cell_index = src->as.cell_index;
    }
    return 1;
}

/* A block-only extension: no byte ends a text run for it, no byte is offered to an
 * inline hook it does not have, and no byte is transparent to flanking. */
const markdown_core_syntax_extension MARKDOWN_CORE_EXTENSION_TABLE = {
    .name = "table",
    .last_block_matches = matches,
    .try_opening_block = try_opening_table_block,
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .contains_inlines_func = contains_inlines,
    .opaque_free_func = opaque_free,
    .opaque_copy_func = opaque_copy,
};

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
