#ifndef MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#define MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#include "inlines.h"
#include "block_internal.h"
bool markdown_core_specimen_continue(markdown_core_parser *parser, markdown_core_member *container,
                                     markdown_core_chunk *input);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_SPECIMEN;

/* THE SPECIMENS OF ONE PARSE (the specimen element's parse record): the
 * marker bytes the element stepped over, for its complexity gate. */
typedef struct {
    size_t work;
} markdown_core_specimen_state;

#endif
