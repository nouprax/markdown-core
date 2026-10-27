#include "text.h"

#include <stdlib.h>
#include <string.h>

static bool line_end(uint8_t byte) { return byte == '\n' || byte == '\r'; }

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

void lsp_text_locate(const lsp_text *text, markdown_core_lsp_position position, markdown_core_text_unit unit,
                     size_t *offset, markdown_core_position *cursor) {
    const uint8_t *bytes = text->bytes;
    size_t size = text->size, at = 0;
    uint32_t line = 0;
    for (; line < position.line; line++) {
        size_t i = at;
        while (i < size && !line_end(bytes[i])) {
            i++;
        }
        if (i == size) {
            /* Past the last line: the text's end. */
            *offset = size;
            cursor->line = (int32_t)line + 1;
            cursor->column = (int32_t)(columns(bytes, at, size, unit) + 1);
            return;
        }
        at = i + (bytes[i] == '\r' && i + 1 < size && bytes[i + 1] == '\n' ? 2 : 1);
    }
    size_t end = at;
    while (end < size && !line_end(bytes[end])) {
        end++;
    }
    /* Step over whole scalars, as the parser reads a column, and stop
     * before one the character offset lands inside or the line's end. */
    uint64_t units = 0;
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
    *offset = at;
    cursor->line = (int32_t)line + 1;
    cursor->column = (int32_t)(units + 1);
}

static bool reserve(lsp_text *text, size_t size) {
    if (size <= text->capacity) {
        return true;
    }
    size_t capacity = text->capacity < 256 ? 256 : text->capacity;
    while (capacity < size) {
        capacity = capacity > SIZE_MAX / 2 ? size : capacity * 2;
    }
    uint8_t *bytes = (uint8_t *)realloc(text->bytes, capacity);
    if (!bytes) {
        return false;
    }
    text->bytes = bytes;
    text->capacity = capacity;
    return true;
}

bool lsp_text_assign(lsp_text *text, const uint8_t *bytes, size_t size) {
    if (!reserve(text, size)) {
        return false;
    }
    if (size) {
        memcpy(text->bytes, bytes, size);
    }
    text->size = size;
    return true;
}

bool lsp_text_splice(lsp_text *text, size_t start, size_t end, const uint8_t *bytes, size_t size) {
    size_t kept = text->size - (end - start);
    if (size > SIZE_MAX - kept || !reserve(text, kept + size)) {
        return false;
    }
    if (text->size > end) {
        memmove(text->bytes + start + size, text->bytes + end, text->size - end);
    }
    if (size) {
        memcpy(text->bytes + start, bytes, size);
    }
    text->size = kept + size;
    return true;
}

void lsp_text_release(lsp_text *text) {
    free(text->bytes);
    text->bytes = NULL;
    text->size = text->capacity = 0;
}
