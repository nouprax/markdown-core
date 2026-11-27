#ifndef MARKDOWN_CORE_ELEMENT_TOPIC_H
#define MARKDOWN_CORE_ELEMENT_TOPIC_H
#include "element.h"
/* The fields a run of `#` signs from `first` to `end`, which the input's
 * end cuts short, leaves open, to the parse's context sink: the run is the
 * whole line when `alone` says so, and the closing sequence of a heading's
 * line otherwise. */
void markdown_core_topic_report_line(markdown_core_parser *parser, bufsize_t first, bufsize_t end, bool alone);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_TOPIC;
#endif
