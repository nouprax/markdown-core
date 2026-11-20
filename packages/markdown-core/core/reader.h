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
struct markdown_core_node;
struct markdown_core_parser;
struct markdown_core_element_instance;

/* What the reading read beyond the line in hand, which the engine notes as
 * it closes the reader (blocks.c, THE READER CLOSES): `read`, a block
 * before the line, and whether the reading writes it, `writes`, or the
 * block it makes depends on it, `made`, the first block the step made after
 * it read one; `past`, one past the furthest byte it read past the line,
 * and `seen`, one past the furthest a lookahead it ran read, 0 for none;
 * and `opener`, the element whose opener the reading read to its closer,
 * `shut`, or to the end without one. */
typedef struct markdown_core_reader {
    markdown_core_chunk line;
    int number, indent;
    bufsize_t offset, column, nonspace, nonspace_column;
    bufsize_t start, end;
    bool blank, split, writes;
    const struct markdown_core_node *read;
    struct markdown_core_node *made;
    size_t past, seen;
    const struct markdown_core_element_instance *opener;
    bool shut;
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

/* READS PAST THE LINE. The byte at `place` in the active input, or -1 where
 * the text ends there; the reader notes it read through it. */
int markdown_core_reader_byte(markdown_core_reader *reader, struct markdown_core_parser *parser, size_t place);
/* The first place at or after `place` in the active input that holds
 * `byte`, or where the text ends; the reader notes it read through it. */
size_t markdown_core_reader_seek(markdown_core_reader *reader, struct markdown_core_parser *parser, size_t place,
                                 unsigned char byte);
/* Where the line after the one in hand starts in the active input. */
size_t markdown_core_reader_after(const markdown_core_reader *reader, const struct markdown_core_parser *parser);

/* READS OF THE TREE. The last block of `parent` before the line, which the
 * line may yet write -- an identifier names it -- or NULL. */
struct markdown_core_node *markdown_core_reader_last(markdown_core_reader *reader, struct markdown_core_parser *parser,
                                                     struct markdown_core_node *parent);
/* The block before `node`, a block the line just made, among its parent's
 * children, or NULL: `node` depends on what it is. */
const struct markdown_core_node *markdown_core_reader_before(markdown_core_reader *reader,
                                                             struct markdown_core_parser *parser,
                                                             struct markdown_core_node *node);
/* Whether `node`, an open block the line in hand goes on in, begins on
 * input line `line`, the line in hand or one before it the reading reached:
 * the line's reading reads that block again whenever it reads the line. */
bool markdown_core_reader_starts(const markdown_core_reader *reader, struct markdown_core_parser *parser,
                                 const struct markdown_core_node *node, int line);
/* `node`, an open block whose facts the block the line makes next depends
 * on. */
static inline const struct markdown_core_node *markdown_core_reader_open(markdown_core_reader *reader,
                                                                         const struct markdown_core_node *node) {
    reader->read = node;
    reader->writes = false;
    return node;
}

/* The reading is an opener of `owner`'s element, an element with stop
 * lines, from the line's start: `shut` when it read on to its closer, and
 * otherwise to the end, which it notes in place of what it read past the
 * line (blocks.c, AN OPENER READ TO THE END). */
static inline void markdown_core_reader_opens(markdown_core_reader *reader,
                                              const struct markdown_core_element_instance *owner, bool shut) {
    reader->opener = owner;
    reader->shut = shut;
}

#endif
