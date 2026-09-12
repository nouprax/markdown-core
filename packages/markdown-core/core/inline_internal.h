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
#include "houdini.h"
#include "iterator.h"
#include "inlines.h"
#include "element.h"
#include "../elements/markdown-core-elements.h"
#include "../elements/citation_state.h"
#include "../elements/bracket_state.h"

/* One maximal parsed delimiter run, classified from immutable source bytes. The text
 * scanner may retain one lookahead run for delimiter dispatch to consume. */
typedef struct {
    bufsize_t start, end;
    markdown_core_delimiter_rule rule;
    bool can_open, can_close;
} delimiter_run;

struct markdown_core_inline_state {
    markdown_core_mem *mem;
    markdown_core_chunk input;
    markdown_core_attribute_parser attributes;
    bufsize_t heading_attributes_start, text_end, heading_label_end;
    unsigned flags;
    bufsize_t opaque_end;
    /* One plus the start of a suffix proven to contain no closer of a rule. */
    bufsize_t opaque_failed_from[MARKDOWN_CORE_DELIM_RULE_COUNT];
    int line;
    bufsize_t pos;
    /* The block whose content buffer `input` is, and the parser that holds
     * requirement 10's content-to-source map for it. Both are NULL for a
     * inline state built straight out of a chunk -- the reference-definition
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
};

#define make_str(inline_state, sc, ec, s)                                                                              \
    markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_TEXT, sc, ec, s)

markdown_core_node *markdown_core_inline_make_literal(markdown_core_inline_state *inline_state,
                                                      markdown_core_node_type t, int start_column, int end_column,
                                                      markdown_core_chunk s);
markdown_core_node *markdown_core_inline_make_simple(markdown_core_mem *mem, markdown_core_node_type t);
markdown_core_node *markdown_core_inline_make_simple_with_state(markdown_core_inline_state *inline_state,
                                                                markdown_core_node_type t);
void markdown_core_inline_append_child(markdown_core_node *node, markdown_core_node *child);
void markdown_core_inline_state_from_buf(markdown_core_parser *parser, markdown_core_mem *mem, int line_number,
                                         markdown_core_inline_state *inline_state, markdown_core_chunk *chunk,
                                         markdown_core_map *refmap);
unsigned char markdown_core_inline_peek_char_n(markdown_core_inline_state *inline_state, bufsize_t n);
unsigned char markdown_core_inline_peek_char(markdown_core_inline_state *inline_state);
unsigned char markdown_core_inline_peek_at(markdown_core_inline_state *inline_state, bufsize_t pos);
int markdown_core_inline_is_eof(markdown_core_inline_state *inline_state);
bool markdown_core_inline_skip_spaces(markdown_core_inline_state *inline_state);
bool markdown_core_inline_skip_line_end(markdown_core_inline_state *inline_state);
void markdown_core_inline_remove_delimiter(markdown_core_inline_state *inline_state, delimiter *delim);
delimiter *markdown_core_inline_push_delimiter_entry(markdown_core_inline_state *inline_state, delimiter_kind kind,
                                                     bufsize_t position);
void markdown_core_inline_process_delimiters(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                             bufsize_t stack_bottom, delimiter *after);
int markdown_core_inline_parse_inline(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                      markdown_core_node *parent);
void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_node *parent,
                                        markdown_core_map *refmap, markdown_core_inline_state *inline_state);
void markdown_core_inline_clear_inlines(markdown_core_inline_state *inline_state);
bool markdown_core_inline_finish_inlines(markdown_core_parser *parser, markdown_core_inline_state *inline_state);
markdown_core_node *markdown_core_inline_match_delimiter(const markdown_core_element *element,
                                                         markdown_core_inline_state *inline_state);
int markdown_core_byte_set_has(const char *set, unsigned char character);
void markdown_core_inline_push_boundary(markdown_core_inline_state *inline_state, bufsize_t position);
#endif
