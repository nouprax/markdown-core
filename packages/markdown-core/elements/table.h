#ifndef MARKDOWN_CORE_TABLE_H
#define MARKDOWN_CORE_TABLE_H

#include "markdown-core-elements.h"
#include "../include/markdown_core.h"
#include "../core/parser.h"

/* Children are the table's rows; the group counts partition them and are
 * counted as children are. The parser appends the head row, then the content
 * rows. */
typedef struct {
    size_t column_count;
    markdown_core_table_column *columns;
    uint32_t head_count, content_count;
    /* The columns and runs are those of the table this one is a view of
     * (opaque_view_func), which it reads and does not free. */
    bool views_columns;
    size_t autocompleted_cells;
} markdown_core_table;

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
