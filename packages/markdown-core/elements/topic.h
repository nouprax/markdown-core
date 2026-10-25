#ifndef MARKDOWN_CORE_ELEMENT_TOPIC_H
#define MARKDOWN_CORE_ELEMENT_TOPIC_H
#include "element.h"
/* The fields a line of `#` signs from `first` to `end`, which the input's
 * end cuts short, leaves open, to the parse's context sink. */
void markdown_core_topic_report_line(markdown_core_parser *parser, bufsize_t first, bufsize_t end);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_TOPIC;
#endif
