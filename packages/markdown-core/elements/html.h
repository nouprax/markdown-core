#ifndef MARKDOWN_CORE_ELEMENT_HTML_H
#define MARKDOWN_CORE_ELEMENT_HTML_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HTML;
bufsize_t markdown_core_inline_scan_inline_html(markdown_core_inline_state *inline_state, bufsize_t pos,
                                                unsigned *flags, bool *is_comment);
#define FLAG_SKIP_HTML_CDATA (1u << 0)
#define FLAG_SKIP_HTML_DECLARATION (1u << 1)
#define FLAG_SKIP_HTML_PI (1u << 2)
#define FLAG_SKIP_HTML_COMMENT (1u << 3)
#endif
