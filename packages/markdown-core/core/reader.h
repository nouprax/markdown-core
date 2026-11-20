#ifndef MARKDOWN_CORE_READER_H
#define MARKDOWN_CORE_READER_H

#include <stdbool.h>
#include "chunk.h"

/* THE READER (docs/plans no-flush.md, 10.12): the one view a reading has
 * of the text. The engine makes it as a reading begins -- a step of the
 * line in hand, a look at the lines after it, a candidate's feed and
 * decision, a block's close, a read of a block's content -- and hands it
 * to every hook that reads; a hook reads the text only through it, and
 * the engine records what the reading read as it ends the reader.
 *
 * The line in hand: its bytes, through its line ending, with a NUL after
 * them; its input line number, where it starts in the active input and
 * where it ends, before its line ending; and where on it the reading
 * stands: the byte offset and the column, the first non-space byte and its
 * column, the indentation between them, whether the line is blank, and
 * whether a tab the offset is inside is split, partly consumed. Columns
 * count each Unicode scalar once and expand tabs to tab stops; offsets are
 * bytes. */
typedef struct markdown_core_reader {
    markdown_core_chunk line;
    int number, indent;
    bufsize_t offset, column, nonspace, nonspace_column;
    bufsize_t start, end;
    bool blank, split;
} markdown_core_reader;

/* The byte at `at` on the line; its NUL past the line ending. */
static inline unsigned char markdown_core_reader_peek(const markdown_core_reader *reader, bufsize_t at) {
    return reader->line.data[at];
}

/* The end of the scalar at `at`: the first byte from `at` on that is no
 * continuation byte, which the line's end is at the latest. */
static inline bufsize_t markdown_core_reader_scalar_end(const markdown_core_reader *reader, bufsize_t at) {
    while ((reader->line.data[at] & 0xC0) == 0x80) {
        at++;
    }
    return at;
}

/* Finds the first non-space byte at or after the offset, its column, the
 * indentation and whether the line is blank from there. */
void markdown_core_reader_find(markdown_core_reader *reader);
/* Moves the offset and column on by `count`: columns when `columns`, which
 * may split a tab but finish a whole scalar, and bytes otherwise. */
void markdown_core_reader_advance(markdown_core_reader *reader, bufsize_t count, bool columns);
/* Moves the offset on to the first non-space byte found. */
static inline void markdown_core_reader_skip(markdown_core_reader *reader) {
    markdown_core_reader_advance(reader, reader->nonspace - reader->offset, false);
}
/* Whether the line continues a block by `continuation` columns of
 * indentation, or, when `blank` lets it, by being blank; a line that does
 * is advanced past them. */
bool markdown_core_reader_indented(markdown_core_reader *reader, int continuation, bool blank);

#endif
