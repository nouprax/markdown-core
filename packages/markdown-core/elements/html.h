#ifndef MARKDOWN_CORE_ELEMENT_HTML_H
#define MARKDOWN_CORE_ELEMENT_HTML_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HTML;
#define FLAG_SKIP_HTML_CDATA (1u << 0)
#define FLAG_SKIP_HTML_DECLARATION (1u << 1)
#define FLAG_SKIP_HTML_PI (1u << 2)
#define FLAG_SKIP_HTML_COMMENT (1u << 3)
#endif
