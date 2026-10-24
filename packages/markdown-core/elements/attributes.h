#ifndef MARKDOWN_CORE_ELEMENT_ATTRIBUTES_H
#define MARKDOWN_CORE_ELEMENT_ATTRIBUTES_H
#include "inlines.h"
struct markdown_core_field;
/* The attribute block at content offset `start` of a construct of kind
 * `owner`: whether one closes there, with its value and its end. */
int markdown_core_inline_state_attributes(markdown_core_inline_state *inline_state, bufsize_t start,
                                          markdown_core_node_kind owner, markdown_core_attributes *value,
                                          bufsize_t *end);
void markdown_core_inline_attach_inline_attributes(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                                   bufsize_t from);
bufsize_t markdown_core_attributes_attach_tail(markdown_core_parser *parser, markdown_core_node *node,
                                               const unsigned char *source, bufsize_t length);
/* THE MEMBER AN ATTRIBUTE BLOCK THAT DOES NOT CLOSE IS IN AT ITS END, the
 * block's `{` at `start` of the `n` bytes at `s`: an `#` identifier, a `.`
 * class, a record's name, or its value after the name its prefix holds; a
 * member boundary reads as a name not yet begun. False when the block is no
 * block whatever follows. The field's owner is left to the caller. */
bool markdown_core_attributes_open_member(const unsigned char *s, bufsize_t n, bufsize_t start,
                                          struct markdown_core_field *field);
/* The attribute tails a block of kind `owner` may open on the line being
 * read, `line`, at or after `from`, that the line's end at `end` leaves
 * open, to the parse's context sink: every brace that may be one, since
 * which is the tail only the finished line tells; whether there is one. */
bool markdown_core_attributes_report_tail(markdown_core_parser *parser, markdown_core_node_kind owner,
                                          const unsigned char *line, bufsize_t from, bufsize_t end);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES;

#endif
