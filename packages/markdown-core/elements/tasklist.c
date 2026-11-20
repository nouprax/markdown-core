#include "tasklist.h"
#include "parser.h"
#include "utf8.h"
#include "parts.h"

bufsize_t markdown_core_task_prefix_end(markdown_core_reader *reader, bufsize_t *width) {
    const bufsize_t start = reader->nonspace;
    if (markdown_core_reader_peek(reader, start) != '[') {
        return 0;
    }
    /* The marker is one character, whatever it is: only where it ends is
     * asked, so nothing is decoded. The bracket, marker, bracket and
     * separator must all be on the line before any of them is read. */
    *width = markdown_core_utf8proc_width(markdown_core_reader_peek(reader, start + 1));
    if (markdown_core_reader_ends(reader, start + *width + 2) ||
        markdown_core_reader_peek(reader, start + *width + 1) != ']' ||
        !markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, start + *width + 2))) {
        return 0;
    }
    bufsize_t end = start + *width + 3;
    while (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, end))) {
        end++;
    }
    return end;
}

void markdown_core_parse_task_prefix(markdown_core_parser *parser, markdown_core_node *item,
                                     markdown_core_reader *reader) {
    bufsize_t width;
    const bufsize_t end = markdown_core_task_prefix_end(reader, &width);
    if (!end) {
        return;
    }
    /* Own the marker before consuming any source; allocation failure aborts
     * the parse rather than changing recognition or borrowing the line buffer. */
    markdown_core_chunk marker = {(unsigned char *)markdown_core_reader_bytes(reader, reader->nonspace + 1), width, 0};
    if (!markdown_core_chunk_to_cstr(&marker)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    item->as.list->task_marker = markdown_core_optional_chunk_present(marker);
    markdown_core_reader_advance(reader, end - reader->offset, false);
}

/* THE PARTS OF A LIST ITEM (parts.h): a task item's marker, the character
 * between the brackets markdown_core_parse_task_prefix reads after the
 * item's marker, and the block identifier at its first paragraph's end. */
void markdown_core_list_item_parts(markdown_core_parts *parts) {
    const markdown_core_list *item = parts->node->as.list;
    if (item->task_marker.has_value) {
        const bufsize_t stop = markdown_core_parts_next(parts, 0);
        bufsize_t at = 0;
        while (at < stop && parts->text[at] != '[') {
            at++;
        }
        if (at < stop) {
            markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, at + 1,
                                    at + 1 + markdown_core_utf8proc_width(parts->text[at + 1]));
        }
    }
    markdown_core_block_identifier_parts(parts);
}
