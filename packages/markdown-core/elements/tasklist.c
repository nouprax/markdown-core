#include "tasklist.h"
#include "parser.h"
#include "utf8.h"

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
    const markdown_core_field_to fields = {parser, item, NULL, reader, 0, NULL};
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_NONE, reader->nonspace + 1, reader->nonspace + 1 + width);
    markdown_core_reader_advance(reader, end - reader->offset, false);
}
