#ifndef MARKDOWN_CORE_EXT_CODE_H
#define MARKDOWN_CORE_EXT_CODE_H
#include "extension.h"
extern const markdown_core_extension MARKDOWN_CORE_EXTENSION_CODE;
bufsize_t markdown_core_inline_scan_to_closing_backticks(markdown_core_inline_parser *subj, bufsize_t openticklength);
#endif
