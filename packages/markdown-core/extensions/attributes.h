#ifndef MARKDOWN_CORE_EXT_ATTRIBUTES_H
#define MARKDOWN_CORE_EXT_ATTRIBUTES_H
#include "inlines.h"
int markdown_core_inline_state_attributes(markdown_core_inline_state *inline_state, bufsize_t start,
                                          markdown_core_attributes *value, bufsize_t *end);
void markdown_core_inline_attach_inline_attributes(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                                   bufsize_t from);
bufsize_t markdown_core_attributes_attach_tail(markdown_core_parser *parser, markdown_core_node *node,
                                               const unsigned char *source, bufsize_t length);
extern const markdown_core_extension MARKDOWN_CORE_EXTENSION_ATTRIBUTES;
#endif
