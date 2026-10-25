#include "tasklist.h"
#include "parser.h"
#include "utf8.h"
#include "parts.h"

void markdown_core_parse_task_prefix(markdown_core_parser *parser, markdown_core_node *item, const unsigned char *input,
                                     bufsize_t len) {
    bufsize_t start = parser->at.first_nonspace;
    if (len - start < 4 || input[start] != '[') {
        return;
    }

    /* The marker is one character, whatever it is: only where it ends is
     * asked, so nothing is decoded. The bracket, marker, bracket and
     * separator must all be on the line before any of them is read. */
    bufsize_t width = markdown_core_utf8proc_width(input[start + 1]);
    if (len - start < width + 3 || input[start + width + 1] != ']' ||
        !markdown_core_is_space_or_tab(input[start + width + 2])) {
        return;
    }

    bufsize_t end = start + width + 3;
    while (end < len && markdown_core_is_space_or_tab(input[end])) {
        end++;
    }

    /* Own the marker before consuming any source; allocation failure aborts
     * the parse rather than changing recognition or borrowing the line buffer. */
    markdown_core_chunk marker = {(unsigned char *)input + start + 1, width, 0};
    if (!markdown_core_chunk_to_cstr(&marker)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    item->as.list->task_marker = markdown_core_optional_chunk_present(marker);
    markdown_core_parser_advance_offset(parser, (const char *)input, end - parser->at.offset, false);
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
