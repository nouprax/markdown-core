#ifndef MARKDOWN_CORE_ELEMENT_CODE_H
#define MARKDOWN_CORE_ELEMENT_CODE_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE;
bufsize_t markdown_core_inline_scan_to_closing_backticks(markdown_core_inline_state *inline_state,
                                                         bufsize_t openticklength);
#endif
