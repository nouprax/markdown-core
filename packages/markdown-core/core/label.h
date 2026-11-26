#ifndef MARKDOWN_CORE_LABEL_H
#define MARKDOWN_CORE_LABEL_H

#include <stdbool.h>
#include <stdint.h>

#include "../include/markdown_core.h"
#include "compile.h"
#include "node_type.h"
#include "tokenizer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE LABELS (plan symbols-cleanroom.md 23.12, micromark's labelStart and
 * labelEnd): an element's construct opens a label at its opener (a link's
 * `[`, an image's `![`, an inline note's `^[`), a LABEL token the label's
 * record names, kept on the reading's marks while it is open; the byte that
 * closes it (`]`) is the core's construct, which every element whose rules
 * close a label at that byte shares (the dialect lists it there, after the
 * elements' constructs). That construct reads the byte as a LABEL_END token
 * and asks the dialect's rules for it in order of precedence (element.h,
 * markdown_core_close_rule `end`): the innermost label's element's own
 * rules when it has any, else the shared ones. A rule that makes its
 * construct says what (markdown_core_label_made); the core then pairs the
 * attention of the body among itself (attention.h,
 * markdown_core_attention_body), leaves of the opener a MARKER and of the
 * end nothing, and adds the construct's GROUP over the opener, the body
 * and the end (tokenizer.h, GROUPS), whose compile makes the node. With
 * no rule making one, the label closes and both its tokens are text.
 *
 * The core keeps the rule that no link opens inside a link's text
 * (inline_state `no_link_openers`): a link made sets it, a label that does
 * not `keep` it clears it as it opens, and an `own` body restores it as it
 * closes (the rule's `links`). */

/* THE SHAPE OF A LABEL: how its opener reads, which the element that opens
 * it declares and the core reads, never the opener's spelling. The opener
 * spans `width` bytes before the body, of which the first `kept` stay text
 * when a construct over the label leaves them (an image's `!` before a
 * call). It sets in-body mark `mark` (0 or 1, -1 none) for the labels
 * inside it. An `own` body takes no marks from the labels around it and is
 * where a body of its own starts. A `keeps` label leaves the rule about
 * links as it was. `closer` is the byte that closes it, and `kind` the kind
 * of construct its element opens it for, which the rules read. */
typedef struct markdown_core_label_shape {
    uint8_t width, kept;
    int8_t mark;
    bool own, keeps;
    unsigned char closer;
    markdown_core_node_kind kind;
} markdown_core_label_shape;

/* AN OPEN LABEL, kept with the reading's events (events.h, `kept`) at the
 * offset its LABEL token's `data` holds: the element that opened it and its
 * shape; where its opener starts and its body; its LABEL token and that
 * token's enter event; the enter of its end once it closed, made or not,
 * else UINT32_MAX; whether a label opened after it, and whether it is in a
 * body that set mark 0 or 1; the rule about links as it opened, and as
 * its end was read. */
typedef struct markdown_core_opener {
    const struct markdown_core_element_instance *owner;
    const markdown_core_label_shape *shape;
    uint32_t start, position;
    uint32_t token, enter, close;
    bool after;
    bool image0, image1;
    bool outer, ended;
} markdown_core_opener;

/* WHAT A RULE MAKES of a label: a group of token kind `kind`, word `data`,
 * over the bytes from `from` (the opener's start, or past the bytes its
 * shape keeps) up to `past`, where the end token ends (the closer and the
 * tail the rule read after it); the attention of the body pairs as held by
 * a node of kind `holder`. `links` is what the rule about links becomes:
 * 1 set, -1 as it was when the label opened, 0 unchanged. A group that
 * begins before the label (`from` before its opener: a citation's author)
 * enters before event `before`, else UINT32_MAX. A rule that `keeps` the
 * body as it is tells no element it is decided (markdown_core_label_decide):
 * it decides it itself, or nothing in it is made. */
typedef struct markdown_core_label_made {
    uint16_t kind;
    uint32_t data;
    uint32_t from, past;
    markdown_core_node_type holder;
    int8_t links;
    uint32_t before;
    bool keeps;
    /* The kind of the node made, when what follows may update it
     * (markdown_core_text_made), else NONE. */
    markdown_core_node_type follows;
} markdown_core_label_made;

/* What a rule answers (element.h, markdown_core_bracket_match). */

/* The label a token's `data` names. */
static inline markdown_core_opener *markdown_core_opener_at(markdown_core_tokenizer *tokenizer, uint32_t data) {
    return (markdown_core_opener *)(void *)(tokenizer->events.kept + data);
}

/* Opens a label of `shape` for the element whose construct is tried: its
 * opener is the `shape->width` bytes from where the reading stands, which
 * it consumes. The construct succeeds with it (markdown_core_ok). */
markdown_core_state markdown_core_label_open(markdown_core_tokenizer *tokenizer,
                                             const markdown_core_label_shape *shape);

/* What a label's construct starts with: its start state. A label's start
 * opens (tokenizer.h, `opens`), holds what it opened, and is resolved at
 * the reading's end by the core. */
void markdown_core_label_resolve(markdown_core_tokenizer *tokenizer);
bool markdown_core_label_holds(const markdown_core_tokenizer *tokenizer, const void *context);
#define MARKDOWN_CORE_LABEL_CONSTRUCT(start_state)                                                                     \
    {.start = (start_state),                                                                                           \
     .resolve_all = markdown_core_label_resolve,                                                                       \
     .holds = markdown_core_label_holds,                                                                               \
     .opens = true}

/* The core's construct at a closer byte, and how compile makes the core's
 * label tokens (LABEL, LABEL_END, MARKER: text of their bytes). */
extern const markdown_core_construct MARKDOWN_CORE_LABEL_END;
extern const markdown_core_compile_ops MARKDOWN_CORE_LABEL_TOKENS[3];

/* `label`'s body is decided: each element of the dialect that asks is told
 * (element.h, markdown_core_inline_ops `decided`). */
void markdown_core_label_decide(markdown_core_tokenizer *tokenizer, markdown_core_opener *label);
/* The label kept at `offset`, which closed earlier and no rule made a
 * construct of, closes again where its end was read, by the rules for it
 * as they read now (a citation's tail, which waited on its author): its
 * construct is made where it was, its end, if one is. */
void markdown_core_label_again(markdown_core_tokenizer *tokenizer, uint32_t offset);

/* Whether a label `owner` opens can close: the dialect has a rule that
 * closes the labels no `mine` rule claims, or `owner` has `mine` rules. An
 * opener none closes is text. */
struct markdown_core_dialect;
bool markdown_core_label_closes(const struct markdown_core_element_instance *owner,
                                const struct markdown_core_dialect *dialect);

/* The innermost open label, or NULL. */
markdown_core_opener *markdown_core_opener_last(markdown_core_tokenizer *tokenizer);
/* The open labels, innermost first, in one walk down the marks: `*at`
 * starts at the tokenizer's mark count and becomes the mark of the label
 * returned; NULL once none is left below it. */
markdown_core_opener *markdown_core_opener_under(markdown_core_tokenizer *tokenizer, size_t *at);

/* The events of a label's body, from past its opener's exit up to `to`,
 * each walked at the label's own depth: the labels inside it, from their
 * opener to their end, are stepped over. The index of the next event at
 * that depth from `at`, or `to`. */
size_t markdown_core_label_step(markdown_core_tokenizer *tokenizer, size_t at, size_t to);

/* What a construct may ask of the labels where the reading stands: whether
 * it is inside the body of a label whose shape sets in-body mark `image`
 * (0 or 1); the byte that closes the innermost label, 0 outside every
 * label; and where the innermost body of its own starts, 0 outside every
 * one. */
bool markdown_core_label_in(const markdown_core_tokenizer *tokenizer, int image);
unsigned char markdown_core_label_closer(const markdown_core_tokenizer *tokenizer);
uint32_t markdown_core_label_body(const markdown_core_tokenizer *tokenizer);

#ifdef __cplusplus
}
#endif

#endif
