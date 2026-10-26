#ifndef MARKDOWN_CORE_ELEMENT_YAML_H
#define MARKDOWN_CORE_ELEMENT_YAML_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE YAML PARSER, reworked from libyaml's scanner and parser (yaml.c says
 * from which commit, under which licence). It reads one language, YAML as
 * the properties grammar takes it (docs/specs/dialect/properties.md): block
 * and flow collections of any depth, names that are scalars on one line,
 * plain and quoted scalars on one line, a double-quoted scalar's JSON
 * escapes, a bare `|` literal, comments, and the document markers. A
 * folded scalar, a scalar that goes on to another line, an explicit name,
 * an anchor, an alias, a tag and a directive are errors. A NUL reads as
 * U+FFFD, as the Markdown parser reads it, and a line ends at CR, LF or
 * CRLF only. */

/* A place in the text: its byte offset, and its line and column counted
 * from 0 at the start the parser was opened at, in characters. */
typedef struct {
    size_t at, line, column;
} markdown_core_yaml_mark;

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

/* AN EVENT, from `start` to `end`. A collection's start says whether it is
 * written in flow, between brackets or braces; a document's start or end
 * whether a marker writes it. A scalar holds its value, `size` bytes at
 * `text` (NULL when empty): the bytes it is written in, read in place, or,
 * when reading changes them, a buffer the event owns, `owned`. `from` to
 * `to` are its bytes as written, without the quotes around them, and for a
 * literal from its first line's text to its last line's end. */
typedef struct {
    markdown_core_yaml_kind kind;
    markdown_core_yaml_mark start, end;
    bool flow, explicit;
    markdown_core_yaml_style style;
    const unsigned char *text;
    unsigned char *owned;
    size_t size, from, to;
} markdown_core_yaml_event;

/* A span of the text, the comments a parse skips. */
typedef struct {
    size_t from, to;
} markdown_core_yaml_span;

typedef struct markdown_core_yaml_token markdown_core_yaml_token;
typedef struct markdown_core_yaml_candidate markdown_core_yaml_candidate;

/* THE PARSER'S STATE. One parser reads one text at a time; opening it on
 * another keeps the room it has grown. */
typedef struct {
    const unsigned char *text;
    size_t end;
    markdown_core_yaml_mark mark;
    /* Why the text is no YAML of the language, and where; `failed` when an
     * allocation failed. */
    const char *problem;
    markdown_core_yaml_mark place;
    bool failed;
    /* The scanner: its queue of tokens, the candidates for a simple name
     * (one per flow depth), the block indentations and the flow depth. */
    bool started, ended, available, allowed;
    markdown_core_yaml_token *tokens;
    size_t head, tail, room, parsed;
    markdown_core_yaml_candidate *candidates;
    size_t candidate_count, candidate_room;
    int indent;
    int *indents;
    size_t indent_count, indent_room;
    int depth;
    /* The parser: its state and the states and marks it returns to. */
    int state;
    int *states;
    size_t state_count, state_room;
    markdown_core_yaml_mark *marks;
    size_t mark_count, mark_room;
    /* The comments the scanner skipped, in order. */
    markdown_core_yaml_span *comments;
    size_t comment_count, comment_room;
} markdown_core_yaml_parser;

/* Opens `parser` on the text from `start` to `end`, which starts a line. */
void markdown_core_yaml_open(markdown_core_yaml_parser *parser, const unsigned char *text, size_t start, size_t end);
/* The next event into `event`, which the caller frees, or false when the
 * text is no YAML of the language or an allocation failed (`problem`). */
bool markdown_core_yaml_next(markdown_core_yaml_parser *parser, markdown_core_yaml_event *event);
void markdown_core_yaml_event_free(markdown_core_yaml_event *event);
/* Releases the parser's room. */
void markdown_core_yaml_close(markdown_core_yaml_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
