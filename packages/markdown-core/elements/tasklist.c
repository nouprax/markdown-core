#include "tasklist.h"
#include "parser.h"
#include "parts.h"

/* The status the marker between a check's brackets gives; NONE for a byte
 * that is no marker. */
static markdown_core_task_status task_status(unsigned char c) {
    switch (c) {
    case ' ':
        return MARKDOWN_CORE_TASK_STATUS_OPEN;
    case 'x':
    case 'X':
        return MARKDOWN_CORE_TASK_STATUS_DONE;
    case '?':
        return MARKDOWN_CORE_TASK_STATUS_QUESTION;
    case '-':
        return MARKDOWN_CORE_TASK_STATUS_CANCELLED;
    default:
        return MARKDOWN_CORE_TASK_STATUS_NONE;
    }
}

bufsize_t markdown_core_task_prefix_end(markdown_core_reader *reader) {
    const bufsize_t start = reader->nonspace;
    if (markdown_core_reader_peek(reader, start) != '[') {
        return 0;
    }
    /* The bracket, marker, bracket and separator must all be on the line
     * before any of them is read. */
    if (markdown_core_reader_ends(reader, start + 3) || !task_status(markdown_core_reader_peek(reader, start + 1)) ||
        markdown_core_reader_peek(reader, start + 2) != ']' ||
        !markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, start + 3))) {
        return 0;
    }
    bufsize_t end = start + 4;
    while (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, end))) {
        end++;
    }
    return end;
}

void markdown_core_parse_task_prefix(markdown_core_node *item, markdown_core_reader *reader) {
    const bufsize_t end = markdown_core_task_prefix_end(reader);
    if (!end) {
        return;
    }
    item->as.list->task = task_status(markdown_core_reader_peek(reader, reader->nonspace + 1));
    markdown_core_reader_advance(reader, end - reader->offset, false);
}

/* THE PARTS OF A LIST ITEM (parts.h): a task item's marker, the byte
 * between the brackets markdown_core_parse_task_prefix reads after the
 * item's marker, and the block identifier at its first paragraph's end. */
void markdown_core_list_item_parts(markdown_core_parts *parts) {
    const markdown_core_list *item = parts->node->as.list;
    if (item->task != MARKDOWN_CORE_TASK_STATUS_NONE) {
        const bufsize_t stop = markdown_core_parts_next(parts, 0);
        bufsize_t at = 0;
        while (at < stop && parts->text[at] != '[') {
            at++;
        }
        if (at < stop) {
            markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, at + 1, at + 2);
        }
    }
    markdown_core_block_identifier_parts(parts);
}
