#ifndef MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#define MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#include "inlines.h"
#include "block_internal.h"
void markdown_core_block_prepare_specimens(const markdown_core_element_instance *self, markdown_core_parser *parser);
bool markdown_core_specimen_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_SPECIMEN;
void markdown_core_specimen_dispose(const markdown_core_element_instance *self);

/* THE SPECIMENS OF ONE PARSE (the specimen element's parse record):
 * each definition as it opened, the index of their ids that citations resolve
 * against once the block tree is complete, and the marker bytes the element
 * stepped over, for its complexity gate. */
typedef struct {
    markdown_core_definition_collection definitions;
    markdown_core_key_index ids;
    size_t work;
} markdown_core_specimen_state;

#endif
