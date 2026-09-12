#ifndef MARKDOWN_CORE_TABLE_H
#define MARKDOWN_CORE_TABLE_H

#include "markdown-core-elements.h"
#include "../include/markdown_core.h"
#include "../core/parser.h"

/* Children are one owned row chain; group counts partition it. The parser
 * appends head, content, then foot rows in that order. */
typedef struct {
    size_t column_count;
    markdown_core_table_column *columns;
    size_t head_count, content_count, foot_count;
    size_t autocompleted_cells;
    markdown_core_node *caption;
} markdown_core_table;

/* Recognize a complete source candidate before claiming any of its lines. */
markdown_core_node *markdown_core_table_try_open(markdown_core_parser *parser, markdown_core_node *parent,
                                                 unsigned char *input, int length);

/* Consumes the active lookahead transaction at its current caption line. */
bool markdown_core_table_caption_probe(markdown_core_block_lookahead *lookahead, markdown_core_chunk *input, int first,
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
