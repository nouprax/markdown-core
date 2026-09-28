#ifndef MARKDOWN_CORE_ELEMENT_CODE_H
#define MARKDOWN_CORE_ELEMENT_CODE_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE;
/* The closing backtick run for an opening run of `openticklength`, with
 * `code` the code element's instance; 0 when there is none. */
bufsize_t markdown_core_inline_scan_to_closing_backticks(const markdown_core_element_instance *code,
                                                         markdown_core_inline_state *inline_state,
                                                         bufsize_t openticklength);
#endif
