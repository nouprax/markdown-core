#include "tasklist.h"
#include "parser.h"
#include "utf8.h"

static bool task_separator(unsigned char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

void markdown_core_parse_task_prefix(markdown_core_parser *parser, markdown_core_node *item, const unsigned char *input,
                                     bufsize_t len) {
    bufsize_t start = parser->first_nonspace;
    if (len - start < 4 || input[start] != '[') {
        return;
    }

    /* Valid UTF-8 is a caller precondition. Decode only the candidate scalar;
     * a malformed prefix never searches for a later closing bracket. */
    int32_t scalar;
    bufsize_t width = markdown_core_utf8proc_iterate(input + start + 1, len - start - 1, &scalar);
    if (width <= 0 || len - start < width + 3 || input[start + width + 1] != ']' ||
        !task_separator(input[start + width + 2])) {
        return;
    }

    bufsize_t end = start + width + 3;
    while (end < len && task_separator(input[end])) {
        end++;
    }

    /* Own the marker before consuming any source; allocation failure aborts
     * the parse rather than changing recognition or borrowing the line buffer. */
    markdown_core_chunk marker = {(unsigned char *)input + start + 1, width, 0};
    if (!markdown_core_chunk_to_cstr(parser->mem, &marker)) {
        parser->oom = true;
        return;
    }
    item->as.list->task_marker = markdown_core_optional_chunk_present(marker);
    markdown_core_parser_advance_offset(parser, (const char *)input, end - parser->offset, false);
}
