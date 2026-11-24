#ifndef MARKDOWN_CORE_READER_H
#define MARKDOWN_CORE_READER_H

#include <assert.h>
#include <stdbool.h>
#include "chunk.h"
#include "markdown_core_ctype.h"
#include "utf8.h"

/* THE READER (docs/plans no-flush.md, 10.12): the one view a reading has
 * of the text. The engine makes it as a reading begins -- a step of the
 * line in hand, a look at the lines after it, a block's close, a read of a block's content -- and hands it
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
 * bytes. `far` is one past the furthest byte of the line the reading read,
 * which every read of the line notes: a step on a line the text has not
 * finished is taken only when it read none of what more text could change
 * (blocks.c, THE LINE A SESSION HOLDS). */
struct markdown_core_node;
struct markdown_core_parser;
struct markdown_core_element_instance;

/* What the reading read beyond the line in hand, which the engine notes as
 * it closes the reader (blocks.c, THE READER CLOSES): `read`, a block
 * before the line, and whether the reading writes it, `writes`, or the
 * block it makes depends on it, `made`, the first block the step made after
 * it read one, and whether what it read is only how `read` closes, `close`; `past`, one past the furthest byte it read
 * past the line, and `seen`, one past the furthest a lookahead it ran read, 0 for none; and `opener`, the element whose
 * opener the reading read to its closer, `shut`, or to the end without one.
 * `rests`: the reading notes where it stood as it gave its line's text
 * away, as a parse that takes edits does (blocks.c, A REST INSIDE A LINE). */
typedef struct markdown_core_reader {
    markdown_core_chunk line;
    int number, indent;
    bufsize_t offset, column, nonspace, nonspace_column;
    bufsize_t start, end, far;
    bool blank, split, writes, close, rests;
    const struct markdown_core_node *read;
    struct markdown_core_node *made;
    size_t past, seen;
    const struct markdown_core_element_instance *opener;
    bool shut;
} markdown_core_reader;

/* The byte at `at` on the line; its NUL past the line ending. */
static inline unsigned char markdown_core_reader_peek(markdown_core_reader *reader, bufsize_t at) {
    reader->far = at + 1 > reader->far ? at + 1 : reader->far;
    return reader->line.data[at];
}

/* Whether `at` is at the line's end, past its bytes. */
static inline bool markdown_core_reader_ends(markdown_core_reader *reader, bufsize_t at) {
    reader->far = at + 1 > reader->far ? at + 1 : reader->far;
    return at >= reader->line.len;
}

/* The line from `at` to its end, which the reading reads whole, the line's
 * end with it. */
static inline const unsigned char *markdown_core_reader_rest(markdown_core_reader *reader, bufsize_t at) {
    reader->far = reader->line.len + 1;
    return reader->line.data + at;
}

/* The line's bytes from `at` on, for bytes the reading has read already: an
 * open builds from what its scan read. */
static inline const unsigned char *markdown_core_reader_bytes(const markdown_core_reader *reader, bufsize_t at) {
    assert(at <= reader->far);
    return reader->line.data + at;
}

/* The line's text from `at` to its end, given to a block at the line's TEXT
 * step (element.h, `text`): no step of the line reads it. */
static inline const unsigned char *markdown_core_reader_handed(const markdown_core_reader *reader, bufsize_t at,
                                                               bufsize_t *length) {
    *length = reader->line.len - at;
    return reader->line.data + at;
}

/* The line's length, through its line ending: a reading that asks it reads
 * where the line ends. */
static inline bufsize_t markdown_core_reader_length(markdown_core_reader *reader) {
    reader->far = reader->line.len + 1;
    return reader->line.len;
}

/* The end of the scalar at `at`: the first byte from `at` on that is no
 * continuation byte, which the line's end is at the latest. */
static inline bufsize_t markdown_core_reader_scalar_end(markdown_core_reader *reader, bufsize_t at) {
    while ((reader->line.data[at] & 0xC0) == 0x80) {
        at++;
    }
    reader->far = at + 1 > reader->far ? at + 1 : reader->far;
    return at;
}

/* How many bytes `c` the line holds one after another from `at` on: the
 * reader notes it read them and the byte after them. */
static inline bufsize_t markdown_core_reader_run(markdown_core_reader *reader, bufsize_t at, unsigned char c) {
    const unsigned char *const data = reader->line.data;
    bufsize_t end = at;
    while (end < reader->line.len && data[end] == c) {
        end++;
    }
    reader->far = end + 1 > reader->far ? end + 1 : reader->far;
    return end - at;
}

/* Whether the line holds only spaces and tabs from `at` to its ending: the
 * reader notes it read up to the first byte that is neither. */
static inline bool markdown_core_reader_blank_from(markdown_core_reader *reader, bufsize_t at) {
    const unsigned char *const data = reader->line.data;
    while (at < reader->line.len && (data[at] == ' ' || data[at] == '\t')) {
        at++;
    }
    reader->far = at + 1 > reader->far ? at + 1 : reader->far;
    return at >= reader->line.len || data[at] == '\n' || data[at] == '\r';
}

/* WHERE THE READER STANDS on its line: the offset, its column and whether a
 * tab there is split. A reading may move on to read what follows and come
 * back to a place it took (markdown_core_reader_back); what it read stays
 * read. */
typedef struct {
    bufsize_t offset, column;
    bool split;
} markdown_core_reader_mark;

static inline markdown_core_reader_mark markdown_core_reader_tell(const markdown_core_reader *reader) {
    /* No compound literal: C++ includes this header. */
    markdown_core_reader_mark mark;
    mark.offset = reader->offset;
    mark.column = reader->column;
    mark.split = reader->split;
    return mark;
}

static inline void markdown_core_reader_back(markdown_core_reader *reader, markdown_core_reader_mark mark) {
    reader->offset = mark.offset;
    reader->column = mark.column;
    reader->split = mark.split;
}

/* The scalar at `at`, a byte before the line's length, into `*scalar`
 * (MARKDOWN_CORE_NO_SCALAR for a byte that begins none), and its width in
 * bytes: the reader notes it read them. */
static inline int markdown_core_reader_decode(markdown_core_reader *reader, bufsize_t at, int32_t *scalar) {
    const int width = markdown_core_utf8proc_decode(reader->line.data + at, reader->line.len - at, scalar);
    reader->far = at + width > reader->far ? at + width : reader->far;
    return width;
}

/* Finds the first non-space byte at or after the offset, its column, the
 * indentation and whether the line is blank from there. Inline: every step
 * of every line asks it. */
static inline void markdown_core_reader_find(markdown_core_reader *reader) {
    const unsigned char *const data = reader->line.data;
    unsigned char c;
    if (reader->nonspace <= reader->offset) {
        reader->nonspace = reader->offset;
        reader->nonspace_column = reader->column;
        while ((c = data[reader->nonspace])) {
            if (c == ' ') {
                reader->nonspace += 1;
                reader->nonspace_column += 1;
            } else if (c == '\t') {
                reader->nonspace += 1;
                reader->nonspace_column += 4 - (reader->nonspace_column % 4);
            } else {
                break;
            }
        }
    }
    reader->indent = reader->nonspace_column - reader->column;
    reader->blank = markdown_core_is_line_end(data[reader->nonspace]);
    reader->far = reader->nonspace + 1 > reader->far ? reader->nonspace + 1 : reader->far;
}
/* Moves the offset and column on by `count`: columns when `columns`, which
 * may split a tab but finish a whole scalar, and bytes otherwise. */
void markdown_core_reader_advance(markdown_core_reader *reader, bufsize_t count, bool columns);
/* Moves the offset on to the first non-space byte found. */
static inline void markdown_core_reader_skip(markdown_core_reader *reader) {
    markdown_core_reader_advance(reader, reader->nonspace - reader->offset, false);
}
/* Moves the offset on to the line's ending, reading the line to its end. */
static inline void markdown_core_reader_end(markdown_core_reader *reader) {
    reader->far = reader->line.len + 1;
    markdown_core_reader_advance(reader, reader->line.len - 1 - reader->offset, false);
}
/* Whether the line continues a block by `continuation` columns of
 * indentation, or, when `blank` lets it, by being blank; a line that does
 * is advanced past them. */
bool markdown_core_reader_indented(markdown_core_reader *reader, int continuation, bool blank);

/* READS PAST THE LINE. markdown_core_reader_byte (parser.h, inline): the
 * byte at `place` in the active input, or -1 where the text ends there; the
 * reader notes it read through it. */
/* The first place at or after `place` in the active input that holds
 * `byte`, or where the text ends; the reader notes it read through it. */
size_t markdown_core_reader_seek(markdown_core_reader *reader, struct markdown_core_parser *parser, size_t place,
                                 unsigned char byte);
/* markdown_core_reader_after (parser.h, inline): where the line after the
 * one in hand starts in the active input. */

/* READS OF THE TREE. The last block of `parent` before the line, which the
 * line may yet write -- an identifier names it -- or NULL. Looking at it
 * notes nothing: a line that writes it says so (markdown_core_reader_write). */
struct markdown_core_node *markdown_core_reader_last(markdown_core_reader *reader, struct markdown_core_parser *parser,
                                                     struct markdown_core_node *parent);
/* The line writes `node`, a block before it: the reader notes it read the
 * block from its start, and the block depends on the line. */
void markdown_core_reader_write(markdown_core_reader *reader, struct markdown_core_node *node);
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
/* THE FRONT CUT (blocks.c): cuts the items the dialect's elements read at
 * the front of `block`'s text, settled into `parent` before it, and moves
 * the block's front past them; whether anything but blank text is left. */
bool markdown_core_reader_cut_front(markdown_core_reader *reader, struct markdown_core_parser *parser,
                                    struct markdown_core_node *block, struct markdown_core_node *parent);
/* Whether `block`'s text is all items cut whole, each decided by its own
 * bytes and the one after them. */
bool markdown_core_reader_cuts_whole(markdown_core_reader *reader, struct markdown_core_parser *parser,
                                     const struct markdown_core_node *block);
/* `node`, an open block whose facts the block the line makes next depends
 * on. */
static inline const struct markdown_core_node *markdown_core_reader_open(markdown_core_reader *reader,
                                                                         const struct markdown_core_node *node) {
    reader->read = node;
    reader->writes = reader->close = false;
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
