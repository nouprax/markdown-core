#ifndef MARKDOWN_CORE_EXTENSIONS_INLINE_UTIL_H
#define MARKDOWN_CORE_EXTENSIONS_INLINE_UTIL_H

#include <chunk.h>
#include <markdown-core-extension-api.h>
#include <node.h>
#include <parser.h>

/** True when `pos` is at or beyond the buffer or points at a line ending. */
static inline int markdown_core_ext_is_line_end(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos
) {
    return pos >= len || data[pos] == '\n' || data[pos] == '\r';
}

/** True when only horizontal line space remains before the next line end. */
static inline int markdown_core_ext_has_only_spaces_until_line_end(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos
) {
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
    }

    return markdown_core_ext_is_line_end(data, len, pos);
}

#endif
