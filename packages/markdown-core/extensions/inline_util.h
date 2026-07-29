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

/** Creates and consumes a text node for an extension delimiter run. */
static inline markdown_core_node *markdown_core_ext_make_delimiter_text(
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    markdown_core_bufsize offset,
    markdown_core_bufsize len
) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_node *node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);

    if (!node) {
        return NULL;
    }

    node->as.literal = markdown_core_chunk_dup(chunk, offset, len);
    node->start_line = node->end_line = markdown_core_inline_parser_get_line(inline_parser);
    node->start_column = markdown_core_inline_parser_get_column(inline_parser);
    node->end_column = node->start_column + (int)len - 1;
    markdown_core_inline_parser_set_offset(inline_parser, (int)(offset + len));
    return node;
}

/** Removes a matched delimiter range, including both endpoints. */
static inline void markdown_core_ext_remove_delimiters(
    markdown_core_inline_parser *inline_parser,
    delimiter *opener,
    delimiter *closer
) {
    delimiter *delim = closer;

    while (delim != NULL && delim != opener) {
        delimiter *previous = delim->previous;
        markdown_core_inline_parser_remove_delimiter(inline_parser, delim);
        delim = previous;
    }

    markdown_core_inline_parser_remove_delimiter(inline_parser, opener);
}

#endif
