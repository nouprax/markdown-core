#ifndef MARKDOWN_CORE_BRACKET_H
#define MARKDOWN_CORE_BRACKET_H
#include "delimiter.h"
#include "inline_internal.h"
#include "element.h"

/* THE BRACKETS (docs/plans no-flush.md, 10.12, audit 3): the engine's
 * stack of the brackets a run holds open, beside its delimiter stack. An
 * element opens a bracket for its opener and closes one with what its
 * grammar makes of the body; the engine keeps the stack, the body, the
 * brackets a closer suspends, and the rule that no link opens inside a
 * link's text, and asks the dialect's elements about a bracket through
 * their hooks (element.h, markdown_core_inline_ops: `opened`, `settled`,
 * `freed`), never by name. A `]` closes the innermost bracket by the dialect's rules (element.h,
 * markdown_core_close_rule), which the engine asks in order. */

/* THE SHAPE OF A BRACKET: how its opener reads, which the element that
 * opens it declares and the engine reads -- never the opener's spelling.
 * The opener spans `width` bytes before the body, of which the first
 * `kept` stay as text when a construct replaces it (an image's `!`). It
 * sets in-body mark `mark` (0 or 1, -1 none) for the brackets inside it,
 * which markdown_core_inline_state_in_bracket answers. An `own` body takes
 * no marks from the brackets around it, is where a body of its own starts
 * (markdown_core_inline_state_context_start), and leaves the rule about
 * links inside links to its element, which restores it as it closes. A
 * `keeps` bracket leaves that rule as it was around it. `closer` is the
 * byte that closes it. */
typedef struct markdown_core_bracket_shape {
    uint8_t width, kept;
    int8_t mark;
    bool own, keeps;
    unsigned char closer;
} markdown_core_bracket_shape;

/* An open bracket, an item of the run's stack of open brackets (stack.h),
 * the bracket around it before it: the element that opened it, the
 * opener's text node and the position after the opener; whether a bracket
 * opened after it, and whether it is inside a `[` or a `![` body; the rule
 * about links as it opened. A suspended bracket (markdown_core_inline_suspend_bracket)
 * is an item of the stack of suspended ones instead, and keeps where its
 * `]` was read and the node it made there, the delimiter its body ends at
 * and the rule as it was suspended. Then the dialect's elements' records
 * (markdown_core_bracket_record), zeroed as it opens. */
typedef struct bracket {
    markdown_core_stack_item item;
    const markdown_core_element_instance *owner;
    markdown_core_node *inl_text;
    bufsize_t position;
    const markdown_core_bracket_shape *shape;
    bool outer_no_link_openers;
    bool bracket_after;
    bool in_bracket_image0;
    bool in_bracket_image1;
    markdown_core_node *close_text;
    delimiter *delim_end;
    bufsize_t close_position;
    bool pending_no_link_openers;
    /* Aligned as every element record begins (dialect.h). */
    markdown_core_state_alignment records[];
} bracket;
MARKDOWN_CORE_STACK_OF(bracket, bracket)

/* The run's brackets. */
static inline markdown_core_bracket_scope *markdown_core_brackets(markdown_core_inline_state *inline_state) {
    return &inline_state->brackets;
}

/* The innermost open bracket, or NULL outside every bracket. */
static inline bracket *markdown_core_open_bracket(const markdown_core_inline_state *inline_state) {
    return bracket_last(&inline_state->brackets.open);
}

/* The record of `self`'s element in `b` (element.h, `bracket_size`). */
static inline void *markdown_core_bracket_record(const bracket *b, const markdown_core_element_instance *self) {
    return (void *)((unsigned char *)b->records + self->bracket_offset);
}

/* `owner` opens a bracket of `shape` whose opener is `text`, which ends at
 * the run's position; the dialect's elements are told (`opened`). */
void markdown_core_inline_push_bracket(const markdown_core_element_instance *owner,
                                       markdown_core_inline_state *inline_state,
                                       const markdown_core_bracket_shape *shape, markdown_core_node *text);
/* The innermost bracket closes, its construct made or not: the dialect's
 * elements are told (`freed`) and it is freed. */
void markdown_core_inline_pop_bracket(markdown_core_inline_state *inline_state);
/* The body of `opener` is decided: the dialect's elements are told
 * (`settled`) before its delimiters are processed. */
void markdown_core_inline_settle_bracket(markdown_core_inline_state *inline_state, bracket *opener);
/* `owner` takes the body of `opener`: the nodes after its opener text, up
 * to the node a suspended `]` made. */
void markdown_core_inline_take_bracket_content(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                               bracket *opener, markdown_core_node *owner);
/* `replacement` goes where the opener's text was: after the bytes of it
 * its shape keeps as text, or in its place when it keeps none. */
void markdown_core_inline_replace_bracket_opener(markdown_core_inline_state *inline_state, bracket *opener,
                                                 markdown_core_node *replacement);
/* What a token scanner may ask of the brackets around it: whether it is in
 * a `![` body (`image`) or a `[` body; the byte that closes the innermost
 * bracket, or 0 outside every bracket, so a scanner leaves an unescaped
 * closer to the brackets; and where the innermost body of its own starts. */
int markdown_core_inline_state_in_bracket(markdown_core_inline_state *inline_state, int image);
unsigned char markdown_core_inline_state_closing_bracket(markdown_core_inline_state *inline_state);
int markdown_core_inline_state_context_start(markdown_core_inline_state *inline_state);
/* THE SUSPENDED BRACKETS. The innermost bracket, `opener`, waits at its
 * `]`, which was read as `close`, its text, with its body's delimiters
 * ended at `end`: it leaves the open brackets for the suspended ones, until
 * a closer resumes it -- reads its `]` again where it was read, with the
 * brackets as they were around it then -- and the `]` leaves either the
 * construct made, its text dropped, or its text. */
void markdown_core_inline_suspend_bracket(markdown_core_inline_state *inline_state, bracket *opener,
                                          markdown_core_node *close, delimiter *end);
void markdown_core_inline_resume_bracket(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                         bracket *opener);
/* The run ends: every bracket it holds, open or suspended, is freed. */
void markdown_core_inline_clear_brackets(markdown_core_inline_state *inline_state);
/* THE CLOSER at the run's position, a byte an element's close rules close
 * at (element.h, markdown_core_inline_ops `closer`): it closes the
 * innermost bracket by the dialect's rules for that byte when the bracket's
 * shape closes at it, or is text. What it leaves to append, or NULL. */
markdown_core_node *markdown_core_inline_close_bracket(markdown_core_parser *parser,
                                                       markdown_core_inline_state *inline_state);

#endif
