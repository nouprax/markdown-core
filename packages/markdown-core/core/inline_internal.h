#ifndef MARKDOWN_CORE_INLINE_INTERNAL_H
#define MARKDOWN_CORE_INLINE_INTERNAL_H
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown_core_ctype.h"
#include "utf8.h"
#include "scanners.h"
#include "houdini.h"
#include "iterator.h"
#include "inlines.h"
#include "extension.h"
#include "../extensions/markdown-core-extensions.h"
#include "../extensions/citation_state.h"
typedef enum { BRACKET_UNMATCHED, BRACKET_MATCHED, BRACKET_REJECTED } markdown_core_bracket_match;

typedef enum { BRACKET_LINK, BRACKET_IMAGE, BRACKET_FOOTNOTE } bracket_kind;

typedef struct bracket {
    struct bracket *previous;
    markdown_core_node *inl_text;
    bufsize_t position;
    /* Last pipe read as ordinary text at this bracket's depth. Opaque tokens,
     * escapes and nested brackets never update the enclosing image. */
    bufsize_t image_pipe;
    bracket_kind kind;
    bool outer_no_link_openers;
    bool active;
    bool bracket_after;
    bool in_bracket_image0;
    bool in_bracket_image1;
    citation_tokens citations;
    citation_token *author;
    /* A tail waits for its key's enclosing owner. All token nodes stay in the
     * AST; this parser-owned continuation borrows the exact bounded range. */
    markdown_core_node *close_text;
    delimiter *delim_end;
    bufsize_t close_position;
    bool pending_no_link_openers;
    struct bracket *pending_previous, *pending_next;
} bracket;

#define FLAG_SKIP_HTML_CDATA (1u << 0)
#define FLAG_SKIP_HTML_DECLARATION (1u << 1)
#define FLAG_SKIP_HTML_PI (1u << 2)
#define FLAG_SKIP_HTML_COMMENT (1u << 3)

/* One maximal parsed delimiter run, classified from immutable source bytes. The text
 * scanner may retain one lookahead run for delimiter dispatch to consume. */
typedef struct {
    bufsize_t start, end;
    markdown_core_delimiter_rule rule;
    bool can_open, can_close;
} delimiter_run;

typedef struct subject {
    markdown_core_mem *mem;
    markdown_core_chunk input;
    markdown_core_attribute_parser attributes;
    bufsize_t heading_attributes_start, heading_content_end, heading_label_end;
    unsigned flags;
    bufsize_t opaque_end;
    /* One plus the start of a suffix proven to contain no closer of a rule. */
    bufsize_t opaque_failed_from[MARKDOWN_CORE_DELIM_RULE_COUNT];
    int line;
    bufsize_t pos;
    /* The block whose content buffer `input` is, and the parser that holds
     * requirement 10's content-to-source map for it. Both are NULL for a
     * subject built straight out of a chunk -- the reference-definition
     * parser -- and the map is then simply not consulted. */
    markdown_core_parser *owner_parser;
    markdown_core_node *owner;
    markdown_core_map *refmap;
    delimiter *last_delim;
    delimiter_run cached_run;
    /* How many delimiters of each rule on the stack can open, and how many
     * can close, kept at every push and removal so a scanner can ask whether
     * a closer will pair without walking the stack. */
    int delim_openers[MARKDOWN_CORE_DELIM_RULE_COUNT];
    int delim_closers[MARKDOWN_CORE_DELIM_RULE_COUNT];
    bracket *last_bracket;
    citation_tokens citations;
    citation_brace_index citation_braces;
    bracket *pending_brackets;
    /* One past the last consumed byte other than SP/TAB. This lets every
     * inline-note closer test its body's non-empty rule in constant time. */
    bufsize_t nonblank_end;
    bufsize_t *backticks;
    bufsize_t backtick_capacity;
    bool scanned_for_backticks;
    bool no_link_openers;
    /* Borrowed from the owning parser (or the immutable core defaults when
     * there is no parser, e.g. reference parsing). */
    const int8_t *special_chars;
    const int8_t *skip_chars;
    /* Sticky allocation-failure flag, copied to the parser after the inline
     * pass so a lossy parse is reported instead of silently truncated. */
    int oom;
} subject;

#define make_str(subj, sc, ec, s) markdown_core_inline_make_literal(subj, MARKDOWN_CORE_NODE_TEXT, sc, ec, s)
#define make_code(subj, sc, ec, s) markdown_core_inline_make_literal(subj, MARKDOWN_CORE_NODE_CODE, sc, ec, s)
#define make_raw_html(subj, sc, ec, s) markdown_core_inline_make_literal(subj, MARKDOWN_CORE_NODE_HTML, sc, ec, s)
#define make_line_break(mem) markdown_core_inline_make_simple(mem, MARKDOWN_CORE_NODE_LINE_BREAK)
#define make_soft_break(mem) markdown_core_inline_make_simple(mem, MARKDOWN_CORE_NODE_SOFT_BREAK)

markdown_core_node *markdown_core_inline_make_literal(subject *subj, markdown_core_node_type t, int start_column,
                                                      int end_column, markdown_core_chunk s);
markdown_core_node *markdown_core_inline_make_simple(markdown_core_mem *mem, markdown_core_node_type t);
markdown_core_node *markdown_core_inline_make_simple_subj(subject *subj, markdown_core_node_type t);
void markdown_core_inline_append_child(markdown_core_node *node, markdown_core_node *child);
void markdown_core_inline_subject_from_buf(markdown_core_parser *parser, markdown_core_mem *mem, int line_number,
                                           subject *e, markdown_core_chunk *chunk, markdown_core_map *refmap);
unsigned char markdown_core_inline_peek_char_n(subject *subj, bufsize_t n);
unsigned char markdown_core_inline_peek_char(subject *subj);
unsigned char markdown_core_inline_peek_at(subject *subj, bufsize_t pos);
int markdown_core_inline_is_eof(subject *subj);
bool markdown_core_inline_skip_spaces(subject *subj);
bool markdown_core_inline_skip_line_end(subject *subj);
bufsize_t markdown_core_inline_scan_to_closing_backticks(subject *subj, bufsize_t openticklength);
void markdown_core_inline_remove_delimiter(subject *subj, delimiter *delim);
void markdown_core_inline_pop_bracket(subject *subj);
delimiter *markdown_core_inline_push_delimiter_entry(subject *subj, delimiter_kind kind, bufsize_t position);
void markdown_core_inline_process_delimiters(markdown_core_parser *parser, subject *subj, bufsize_t stack_bottom,
                                             delimiter *after);
bufsize_t markdown_core_inline_scan_inline_html(subject *subj, bufsize_t pos, unsigned *flags, bool *is_comment);
void markdown_core_inline_take_bracket_content(markdown_core_parser *parser, bracket *opener,
                                               markdown_core_node *owner);
void markdown_core_inline_replace_bracket_opener(subject *subj, bracket *opener, markdown_core_node *replacement);
markdown_core_node *markdown_core_inline_handle_close_bracket(markdown_core_parser *parser, subject *subj);
int markdown_core_inline_parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent);
void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_node *parent,
                                        markdown_core_map *refmap, subject *subj);
void markdown_core_inline_clear_inlines(subject *subj);
bool markdown_core_inline_finish_inlines(markdown_core_parser *parser, subject *subj);
markdown_core_node *markdown_core_inline_match_delimiter(const markdown_core_extension *extension,
                                                         markdown_core_inline_parser *subj);
void markdown_core_inline_push_bracket(subject *subj, bracket_kind kind, markdown_core_node *inl_text);
#endif
