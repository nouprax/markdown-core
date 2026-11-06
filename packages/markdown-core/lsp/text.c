#include "text.h"

/* The columns a run of bytes counts in `unit`, by the parser's rule: in
 * UTF-16 a byte that begins a four-byte scalar is two units, a continuation
 * byte none and any other byte one. */
static uint64_t columns(const uint8_t *bytes, size_t from, size_t to, markdown_core_text_unit unit) {
    if (unit == MARKDOWN_CORE_TEXT_UNIT_UTF8) {
        return to - from;
    }
    uint64_t units = 0;
    for (size_t i = from; i < to; i++) {
        units += (bytes[i] & 0xC0) == 0x80 ? 0 : bytes[i] >= 0xF0 ? 2 : 1;
    }
    return units;
}

void lsp_text_locate(const markdown_core_session *session, markdown_core_lsp_position position,
                     markdown_core_text_unit unit, size_t *offset, markdown_core_position *cursor) {
    /* A flushed text has a line, if only the empty one. */
    const size_t lines = markdown_core_session_lines(session);
    const bool past = position.line >= lines;
    const size_t line = past ? lines - 1 : position.line;
    markdown_core_string text = {0};
    (void)markdown_core_session_line(session, line + 1, &text);
    const uint8_t *bytes = text.data;
    const size_t end = text.length;
    const size_t start = (size_t)(bytes - markdown_core_session_source(session).data);
    cursor->line = (int32_t)line + 1;
    if (past) {
        /* Past the last line: the text's end. */
        *offset = markdown_core_session_source(session).length;
        cursor->column = (int32_t)(columns(bytes, 0, end, unit) + 1);
        return;
    }
    /* Step over whole scalars, as the parser reads a column, and stop
     * before one the character offset lands inside or the line's end. */
    uint64_t units = 0;
    size_t at = 0;
    while (at < end && units < position.character) {
        size_t next = at + 1;
        while (next < end && (bytes[next] & 0xC0) == 0x80) {
            next++;
        }
        uint64_t width = columns(bytes, at, next, unit);
        if (units + width > position.character) {
            break;
        }
        units += width;
        at = next;
    }
    *offset = start + at;
    cursor->column = (int32_t)(units + 1);
}
