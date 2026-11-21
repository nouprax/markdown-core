#ifndef MARKDOWN_CORE_BRACKET_H
#define MARKDOWN_CORE_BRACKET_H
#include "delimiter.h"
#include "inline_internal.h"

/* THE BRACKETS (docs/plans no-flush.md, 10.12, audit 3): the engine's
 * stack of the brackets a run holds open, beside its delimiter stack. An
 * element opens a bracket for its opener and closes one with what its
 * grammar makes of the body; the engine keeps the stack, the body, the
 * brackets a closer suspends, and the rule that no link opens inside a
 * link's text, and asks the dialect's elements about a bracket through
 * their hooks (element.h, markdown_core_inline_ops: `opened`, `settled`,
 * `freed`), never by name.
 *
 * What a closer reports of a bracket: it made its construct, it refuses
 * every construct (the `]` is text), or it makes none and the next rule is
 * asked. */
typedef enum { BRACKET_UNMATCHED, BRACKET_MATCHED, BRACKET_REJECTED } markdown_core_bracket_match;

/* How the opener is spelled: `[`, `![` or `^[`. A `^[` opens a body of its
 * own: it takes no link or image state from the brackets around it, and the
 * rule about links inside links is restored when it closes. */
typedef enum { BRACKET_LINK, BRACKET_IMAGE, BRACKET_FOOTNOTE } bracket_kind;

/* An open bracket: the bracket around it, the element that opened it, the
 * opener's text node and the position after the opener; whether a bracket
 * opened after it, and whether it is inside a `[` or a `![` body; the rule
 * about links as it opened. A suspended bracket (markdown_core_bracket_wait)
 * also keeps where its `]` was read and the node it made there, the
 * delimiter its body ends at and the rule as it was suspended, in the list
 * of suspended brackets. Then the dialect's elements' records
 * (markdown_core_bracket_record), zeroed as it opens. */
typedef struct bracket {
    struct bracket *previous;
    const markdown_core_element_instance *owner;
    markdown_core_node *inl_text;
    bufsize_t position;
    bracket_kind kind;
    bool outer_no_link_openers;
    bool bracket_after;
    bool in_bracket_image0;
    bool in_bracket_image1;
    markdown_core_node *close_text;
    delimiter *delim_end;
    bufsize_t close_position;
    bool pending_no_link_openers;
    struct bracket *pending_previous, *pending_next;
    unsigned char records[];
} bracket;

/* The run's brackets. */
static inline markdown_core_bracket_scope *markdown_core_brackets(markdown_core_inline_state *inline_state) {
    return &inline_state->brackets;
}

/* The innermost open bracket, or NULL outside every bracket. */
static inline bracket *markdown_core_open_bracket(const markdown_core_inline_state *inline_state) {
    return inline_state->brackets.last;
}

/* The record of `self`'s element in `b` (element.h, `bracket_size`). */
static inline void *markdown_core_bracket_record(const bracket *b, const markdown_core_element_instance *self) {
    return (void *)(b->records + self->bracket_offset);
}

/* `owner` opens a bracket spelled `kind` whose opener is `text`, which
 * ends at the run's position; the dialect's elements are told
 * (`opened`). */
void markdown_core_inline_push_bracket(const markdown_core_element_instance *owner,
                                       markdown_core_inline_state *inline_state, bracket_kind kind,
                                       markdown_core_node *text);
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
/* `replacement` goes where the opener's text was: after the `!` of a `![`,
 * which stays as text, and in place of a `[` or `^[`. */
void markdown_core_inline_replace_bracket_opener(markdown_core_inline_state *inline_state, bracket *opener,
                                                 markdown_core_node *replacement);
/* What a token scanner may ask of the brackets around it: whether it is in
 * a `![` body (`image`) or a `[` body; the byte that closes the innermost
 * bracket, or 0 outside every bracket, so a scanner leaves an unescaped
 * closer to the brackets; and where the innermost body of its own starts. */
int markdown_core_inline_state_in_bracket(markdown_core_inline_state *inline_state, int image);
unsigned char markdown_core_inline_state_closing_bracket(markdown_core_inline_state *inline_state);
int markdown_core_inline_state_context_start(markdown_core_inline_state *inline_state);
/* The run ends: every bracket it holds, open or suspended, is freed. */
void markdown_core_inline_clear_brackets(markdown_core_inline_state *inline_state);

#endif
