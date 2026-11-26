#ifndef MARKDOWN_CORE_ATTENTION_H
#define MARKDOWN_CORE_ATTENTION_H

#include <stdbool.h>
#include <stdint.h>

#include "compile.h"
#include "inline_internal.h"
#include "tokenizer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE ATTENTION (plan symbols-cleanroom.md 23.10, micromark's attention):
 * runs of one byte that pair as an opener and a closer around what they
 * hold, the grammar of each in its element's delimiter rule (delimiter.h,
 * delimiter_rule_spec): its widths, its flank, its body.
 *
 * A run is a SEQUENCE token whose data says whether it opens and whether
 * it closes and how long it was; a run of an exact width read at another
 * is a LITERAL, text. One pass at the reading's end (resolve_all, shared
 * by every element of the attention, late: after the labels) pairs the
 * sequences in CommonMark's order: each closer the nearest opener of its
 * rule below it, the rule of three and a word body's boundaries kept. A
 * pair makes a GROUP token (tokenizer.h, GROUPS) over what it holds and
 * the bytes of the two runs it used, entered right after the opener and
 * exited right before the closer; what is left of a run is text, and a
 * run with nothing left compiles to nothing. A boundary is the end of a
 * token whose kind says so (markdown_core_compile_ops, `boundary`) or the
 * byte after the last white space of a data token. The body of a
 * construct made before (a label's) pairs as it is made
 * (markdown_core_attention_body), and every pass after steps over it.
 *
 * An element takes the attention as its text: its construct is
 * MARKDOWN_CORE_ATTENTION_CONSTRUCT at its byte and its tokens
 * MARKDOWN_CORE_ATTENTION_TOKENS. */

enum {
    MARKDOWN_CORE_ATTENTION_SEQUENCE,
    MARKDOWN_CORE_ATTENTION_LITERAL,
    MARKDOWN_CORE_ATTENTION_GROUP,
    MARKDOWN_CORE_ATTENTION_KINDS
};

/* A sequence's word: whether it opens and closes, its rule and its length
 * as read. An element that reads its own runs (formula) writes it. */
enum { MARKDOWN_CORE_ATTENTION_OPENS = 1u, MARKDOWN_CORE_ATTENTION_CLOSES = 2u };
static inline uint32_t markdown_core_attention_word(bool opens, bool closes, unsigned rule, uint32_t length) {
    return (opens ? MARKDOWN_CORE_ATTENTION_OPENS : 0u) | (closes ? MARKDOWN_CORE_ATTENTION_CLOSES : 0u) |
           (uint32_t)rule << 2 | length << 8;
}
static inline unsigned markdown_core_attention_rule(uint32_t word) { return word >> 2 & 0x3fu; }
static inline uint32_t markdown_core_attention_length(uint32_t word) { return word >> 8; }

/* The runs of `rule` a reading holds on its stack, which a run's reading
 * asks (inline_internal.h, `delim_openers` and `delim_closers`): a run read
 * as a sequence is counted as it opens and as it closes, and taken off as
 * it leaves the stack, paired or not. */
static inline void markdown_core_attention_count(markdown_core_tokenizer *tokenizer, unsigned rule, bool opens,
                                                 bool closes) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    inline_state->delim_openers[rule] += opens;
    inline_state->delim_closers[rule] += closes;
}

markdown_core_state markdown_core_attention_start(markdown_core_tokenizer *tokenizer, int code);
void markdown_core_attention_resolve(markdown_core_tokenizer *tokenizer);
/* THE BODY of a construct a resolver makes over the events from `from`
 * (a label's): its runs pair among themselves alone, as the construct is
 * made, and leave the stack; what they make is held by a node of kind
 * `holder`, or by the reading's owner for NONE. What is made keeps the
 * body's last boundary, which this answers (0 for none) for its end's
 * MARKER to hold (events.h). */
uint32_t markdown_core_attention_body(markdown_core_tokenizer *tokenizer, size_t from, size_t to,
                                      markdown_core_node_type holder);
/* Whether the reading's text, resolved, has a boundary at its own depth
 * (a field's, which an enclosing word body pairs across none of). */
bool markdown_core_attention_spaced(const markdown_core_tokenizer *tokenizer);
/* Whether a node of `kind` may go where a pass of `holder` puts it. */
bool markdown_core_attention_allows(const markdown_core_tokenizer *tokenizer, markdown_core_node_type holder,
                                    markdown_core_node_type kind);
uint32_t markdown_core_attention_passes(markdown_core_tokenizer *tokenizer, const void *context, uint32_t at);

#define MARKDOWN_CORE_ATTENTION_CONSTRUCT                                                                              \
    {.start = markdown_core_attention_start,                                                                           \
     .resolve_all = markdown_core_attention_resolve,                                                                   \
     .late = true,                                                                                                     \
     .passes = markdown_core_attention_passes}

/* How compile makes the attention's tokens: an element with kinds of its
 * own after them starts its list with MARKDOWN_CORE_ATTENTION_OPS. */
void markdown_core_attention_text(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index);
void markdown_core_attention_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index);
void markdown_core_attention_close(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index);
#define MARKDOWN_CORE_ATTENTION_OPS                                                                                    \
    [MARKDOWN_CORE_ATTENTION_SEQUENCE] = {.exit = markdown_core_attention_text,                                        \
                                          .plain = true,                                                               \
                                          .verbatim = true,                                                            \
                                          .sequence = true},                                                           \
    [MARKDOWN_CORE_ATTENTION_LITERAL] = {.exit = markdown_core_attention_text, .plain = true, .verbatim = true},       \
    [MARKDOWN_CORE_ATTENTION_GROUP] = {.enter = markdown_core_attention_open, .exit = markdown_core_attention_close}

extern const markdown_core_compile_ops MARKDOWN_CORE_ATTENTION_TOKENS[MARKDOWN_CORE_ATTENTION_KINDS];

#ifdef __cplusplus
}
#endif

#endif
