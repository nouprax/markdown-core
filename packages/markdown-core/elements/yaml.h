#ifndef MARKDOWN_CORE_ELEMENT_YAML_H
#define MARKDOWN_CORE_ELEMENT_YAML_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE YAML PARSER. It reads one language, YAML as the properties grammar
 * takes it (docs/specs/dialect/properties.md): block and flow collections
 * of any depth, names that are scalars or flow collections on one line,
 * plain and quoted scalars on one line, a double-quoted scalar's JSON
 * escapes, a bare `|` literal, comments, and the document markers. A
 * folded scalar, a scalar that goes on to another line, an explicit name,
 * an anchor, an alias, a tag and a directive are errors. A NUL reads as
 * U+FFFD, as the Markdown parser reads it, and a line ends at CR, LF or
 * CRLF only. One read takes a text in one pass, by recursive descent, and
 * writes its events in order into the parser's room. */

/* How a scalar is written. */
typedef enum {
    MARKDOWN_CORE_YAML_PLAIN,
    /* Between apostrophes. */
    MARKDOWN_CORE_YAML_APOSTROPHE,
    /* Between double quotes. */
    MARKDOWN_CORE_YAML_QUOTE,
    MARKDOWN_CORE_YAML_LITERAL,
} markdown_core_yaml_style;

typedef enum {
    MARKDOWN_CORE_YAML_STREAM_START,
    MARKDOWN_CORE_YAML_STREAM_END,
    MARKDOWN_CORE_YAML_DOCUMENT_START,
    MARKDOWN_CORE_YAML_DOCUMENT_END,
    MARKDOWN_CORE_YAML_SCALAR,
    MARKDOWN_CORE_YAML_LIST_START,
    MARKDOWN_CORE_YAML_LIST_END,
    MARKDOWN_CORE_YAML_MAP_START,
    MARKDOWN_CORE_YAML_MAP_END,
} markdown_core_yaml_kind;

/* AN EVENT. A collection's start says whether it is written in flow,
 * between brackets or braces; a document's start or end whether a marker
 * writes it. A scalar holds its value, `size` bytes at `text` (NULL when
 * empty): the bytes it is written in, read in place, or, when reading
 * changes them, a buffer the event owns, `owned`. `from` to `to` are its
 * bytes as written, without the quotes around them, and for a literal from
 * its first line's text to its last line's end; `line` is the line it
 * starts on, counted from 0 at the text's start. */
typedef struct {
    markdown_core_yaml_kind kind;
    bool flow, explicit;
    markdown_core_yaml_style style;
    const unsigned char *text;
    unsigned char *owned;
    size_t size, from, to, line;
} markdown_core_yaml_event;

/* A span of the text, the comments a read skips. */
typedef struct {
    size_t from, to;
} markdown_core_yaml_span;

/* THE PARSER'S STATE. One parser reads one text at a time; reading another
 * keeps the room it has grown. */
typedef struct {
    const unsigned char *text;
    size_t at, end;
    /* Where the line `at` is on starts, and its number. */
    size_t head, line;
    /* Whether a block item or a name may start at `at`; the flow depth,
     * and the block collections open, for the nesting limit. */
    bool allowed;
    size_t depth, open;
    /* Why the text is no YAML of the language, and where; `failed` when an
     * allocation failed. */
    const char *problem;
    size_t place;
    bool failed;
    /* The events read, in order. */
    markdown_core_yaml_event *events;
    size_t count, room;
    /* The comments the read skipped, in order. */
    markdown_core_yaml_span *comments;
    size_t comment_count, comment_room;
} markdown_core_yaml_parser;

/* Reads the text from `start` to `end`, which starts a line, into the
 * parser's events, which the next read or the close frees; false when the
 * text is no YAML of the language or an allocation failed (`problem`). */
bool markdown_core_yaml_read(markdown_core_yaml_parser *parser, const unsigned char *text, size_t start, size_t end);
/* Releases the parser's room. */
void markdown_core_yaml_close(markdown_core_yaml_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
