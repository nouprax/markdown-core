#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <stdint.h>
#include <stdio.h>
#include "references.h"
#include "node.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

/* THE NORMALIZED SOURCE AND ITS LINE INDEX, moved out of the parser and into
 * the document.
 *
 * A scope says WHERE an element is, as a pair of (line, column) BOUNDARIES --
 * not as a byte range, and nothing takes a substring with it. What the
 * coordinates are COUNTED AGAINST is this: the normalized
 * source -- the input UTF-8, every NUL replaced by three bytes, every line ending
 * one `\n` and every line having one -- which is NOT the buffer the caller
 * passed, and that difference is the whole reason a document publishes it.
 *
 * The facade moves this value into the returned document on success. */
typedef struct {
    markdown_core_mem *mem;
    markdown_core_strbuf source;
    bufsize_t *line_starts;
    bufsize_t line_starts_size;
} markdown_core_concrete;

/* Where one source line's bytes landed in a block's content buffer.
 *
 * A block's content is the concatenation of the line slices `add_line` copies
 * into it, and the source column a slice starts at is NOT derivable from the
 * block's own `start_column`: the container prefix stripped from a
 * continuation line need not match the one stripped from the first, so
 * `"> foo\nbar"` strips two bytes then none. One mark per `add_line` call
 * records where the slice came from, and `markdown_core_parser_content_place`
 * reads them back.
 *
 * Marks are appended in parse order and only the deepest open block takes
 * lines, so one block's marks are the contiguous run
 * [node->content_mark, node->content_mark + node->content_mark_count). */
typedef struct {
    /* Offset in the owning block's content where this slice begins. */
    bufsize_t content_offset;
    /* The source line the slice was copied from, counted from 1. */
    int line;
    /* The BYTE column on that line the slice begins at, counted from 1. */
    int column;
} markdown_core_line_mark;

struct markdown_core_parser {
    struct markdown_core_mem *mem;
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    /* The labels this document defines footnotes for (see references.h). The
     * block phase fills it as each definition opens; the inline phase reads it
     * to decide whether a `[^label]` is a call at all. */
    struct markdown_core_map *footnote_defs;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* The last open block after a line is fully processed */
    struct markdown_core_node *current;
    /* See the documentation for markdown_core_parser_get_line_number() in markdown_core.h */
    int line_number;
    /* See the documentation for markdown_core_parser_get_offset() in markdown_core.h */
    bufsize_t offset;
    /* See the documentation for markdown_core_parser_get_column() in markdown_core.h */
    bufsize_t column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in markdown_core.h */
    bufsize_t first_nonspace;
    /* See the documentation for markdown_core_parser_get_first_nonspace_column() in markdown_core.h
     */
    bufsize_t first_nonspace_column;
    bufsize_t thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in markdown_core.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in markdown_core.h */
    bool blank;
    /* See the documentation for markdown_core_parser_has_partially_consumed_tab() in
     * markdown_core.h */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* THE NORMALIZED SOURCE: every line exactly as S_process_line normalized
     * it -- UTF-8 validated if the option is on, NUL replaced, the line ending
     * a single '\n' whether the author wrote one, wrote CRLF, or wrote nothing
     * at all -- concatenated in order. The document retains it because a SCOPE
     * is counted against it: `Text scope=2:2..2:4` names lines and columns of
     * HERE, not of the source buffer the caller passed -- an input with a NUL in it
     * has different columns on that line.
     *
     * It is NOT the caller's bytes: two different inputs normalize to the same
     * source. What the tree's positions describe is this normalized value. */
    markdown_core_strbuf source;
    /* Where each line begins in `source`: line N starts at line_starts[N - 1].
     * The line index the same requirement names, and the only thing that can
     * turn a source offset back into a (line, column) after the parse. */
    bufsize_t *line_starts;
    bufsize_t line_starts_size;
    bufsize_t line_starts_alloc;
    /* When set, the parse transaction MOVES the normalized source and its line
     * index here instead of releasing them, and the caller becomes their owner
     * (requirement 12). */
    markdown_core_concrete *concrete_retain;
    /* When set, the parse transaction writes the record set here before
     * releasing it. There is no public reader: requirement 12 is where a
     * document keeps the concrete view, and until then the CLI's `--concrete`
     * and the gate that drives it are the only consumers. */
    FILE *concrete_out;
    /* See the documentation for markdown_core_parser_get_last_line_length() in markdown_core.h */
    bufsize_t last_line_length;
    /* Scratch for a source line containing NUL bytes; curline holds the
     * normalized line currently being parsed. */
    markdown_core_strbuf line_scratch;
    /* Options set by the user, see the Options section in markdown_core.h */
    int options;
    /* Sticky allocation-failure flag: once any parse structure is lost, the
     * one-shot transaction reports the whole parse as failed (NULL) instead of
     * returning a silently truncated document. */
    bool oom;
    markdown_core_llist *syntax_extensions;
    markdown_core_llist *inline_syntax_extensions;
    markdown_core_ispunct_func backslash_ispunct;
    /* Inline special-character tables for this parser: the core defaults plus
     * the special/emphasis-skip characters of the attached inline extensions.
     * Parser-local so concurrent parsers with different extension sets never
     * observe each other's characters. */
    int8_t special_chars[256];
    int8_t skip_chars[256];
    /* The content-to-source map (see markdown_core_line_mark): one run per
     * block that took lines, appended in parse order. It is read while the
     * parse is still running -- the block phase reads it as blocks close and
     * the inline phase reads it before the transaction returns -- and it is
     * released with the rest of the per-parse state. */
    markdown_core_line_mark *line_marks;
    bufsize_t line_marks_size;
    bufsize_t line_marks_alloc;
};

/* The engine has one parse operation. `setup`, when present, configures the
 * fresh parser before any source is read; extension attachment and retained
 * observations belong there. Returning false aborts the transaction. The
 * parser never escapes this call and is destroyed before it returns. */
typedef bool (*markdown_core_parser_setup_func)(markdown_core_parser *parser, void *context);
markdown_core_node *markdown_core_parse_document_with_mem(const char *source, size_t length, int options,
                                                          markdown_core_mem *mem, markdown_core_parser_setup_func setup,
                                                          void *context);

/* Ask the transaction to hand the normalized source and its line index over
 * rather than release them. `out` is zeroed here and filled on success; a
 * parse that fails leaves it empty. */
void markdown_core_parser_retain_concrete(markdown_core_parser *parser, markdown_core_concrete *out);
void markdown_core_concrete_dispose(markdown_core_concrete *concrete);

#ifdef __cplusplus
}
#endif

#endif
