#ifndef MARKDOWN_CORE_TABLE_H
#define MARKDOWN_CORE_TABLE_H

#include "markdown-core-elements.h"
#include "../include/markdown_core.h"
#include "../core/parser.h"

/* THE RECORD OF A TABLE'S FOLD over its lines (docs/plans/2026-09-29-
 * incremental-parsing.md, E6), which a later parse reads the table's rows
 * against: elements/table.c owns it. */
struct markdown_core_table_fold;

/* Children are one owned row chain; group counts partition it. The parser
 * appends head, content, then foot rows in that order. `fold` is the record
 * of the fold that made a grid table, NULL for every other table. */
typedef struct {
    size_t column_count;
    markdown_core_table_column *columns;
    size_t head_count, content_count, foot_count;
    size_t autocompleted_cells;
    markdown_core_node *caption;
    struct markdown_core_table_fold *fold;
} markdown_core_table;

/* THE TABLE GRAMMAR'S WORK in one parse, for its complexity gates: scalar and
 * byte probe ranges plus union-find and ordering visits (`scan`, each span
 * charged once, short-circuited ranges possibly overcounted); the widest
 * search frontier; the bytes submitted to the horizontal-border grammar,
 * cached facts charging zero; and the growths, geometry lines and separator
 * scans of the workspace the table element keeps. */
typedef struct {
    size_t scan, frontier_peak, horizontal;
    size_t workspace_growth, geometry_lines, separator_scans, scratch_growth;
} markdown_core_table_work;

/* The work counted in the parse record of `table`, the table element's
 * instance; the record itself is the table's own. */
const markdown_core_table_work *markdown_core_table_work_in(const markdown_core_element_instance *table);

/* Consumes the active lookahead transaction at its current caption line,
 * with `table` the table element's instance. */
bool markdown_core_table_caption_probe(const markdown_core_element_instance *table,
                                       markdown_core_block_lookahead *lookahead, markdown_core_chunk *input, int first,
                                       int indent);

/* C LINKAGE, AND WINDOWS IS THE ONLY PLACE THIS SHOWS. The Itanium ABI does not
 * mangle a variable at global scope, so `MARKDOWN_CORE_ELEMENT_*` resolves on
 * Linux and macOS whether or not the declaration says `extern "C"`; MSVC mangles
 * every variable, and a C++ translation unit including this header without the
 * guard fails to link with LNK2019. */
#ifdef __cplusplus
extern "C" {
#endif

/** The one, immutable descriptor. `core-elements.c`'s table is the only
 * place its position in the attach order is written down. */
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_TABLE;

#ifdef __cplusplus
}
#endif

#endif
