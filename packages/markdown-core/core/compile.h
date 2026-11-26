#ifndef MARKDOWN_CORE_COMPILE_H
#define MARKDOWN_CORE_COMPILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE COMPILE (plan symbols-cleanroom.md 23.5): one walk over a reading's
 * events, in order, which hands each event to its token kind's handler.
 * The handlers make the nodes, place them and record what the reading
 * read (fields, uses, openers); a node exists only from here. The walk
 * keeps a stack the handlers push what they open on and pop as it closes
 * (mdast-util-from-markdown's `this.stack`). */

typedef struct markdown_core_compiler markdown_core_compiler;

typedef struct markdown_core_compile_ops {
    void (*enter)(markdown_core_compiler *, const markdown_core_token *, uint32_t token);
    void (*exit)(markdown_core_compiler *, const markdown_core_token *, uint32_t token);
    /* The element instance whose tokens these are. */
    const void *context;
    /* Whether a token of the kind no resolver made anything of is text
     * (an attention sequence that paired with none): a seam does not stop
     * after one (inlines.c, THE SEAMS OF A READING). */
    bool plain;
    /* Whether the token makes the text of exactly the bytes it covers, so
     * a resolver may move its ends (a cite's parentheses taken from the
     * text beside it). */
    bool verbatim;
    /* Whether the token's node was asked of its holder as the token was
     * resolved, so compile appends it without asking again: one decision
     * authorizes one commit (a link's, a cite's). */
    bool asked;
    /* Whether the token's end bounds a word (a line's end): a run of a
     * word body pairs with no opener before it (attention.h). */
    bool boundary;
    /* Whether no pair of attention crosses the token (a bibliography's
     * item key, its separator). */
    bool wall;
    /* Whether what the token holds is its own: compile hands it the enter
     * and the exit and none of the events between (a construct made of
     * the bytes it covers, as a footnote call of its label). */
    bool opaque;
    /* Whether the token is a paired run the core's sequence construct read
     * (attention.h), its context the element instance. A flag and not the
     * handler's address: a linker that folds identical functions (MSVC's
     * /OPT:ICF) gives two elements' handlers one address. */
    bool sequence;
} markdown_core_compile_ops;

struct markdown_core_compiler {
    const markdown_core_events *events;
    const unsigned char *data;
    /* The handlers by token kind, `kind_count` of them; a kind with none
     * makes nothing. */
    const markdown_core_compile_ops *const *ops;
    size_t kind_count;
    /* The event being handed. */
    size_t at;
    void **stack;
    size_t stack_count, stack_room;
    /* The ops of the event being handed. */
    const markdown_core_compile_ops *current;
    /* What the reading compiles for: the parser. */
    void *reader;
    bool failed;
};

void markdown_core_compiler_init(markdown_core_compiler *compiler);
void markdown_core_compiler_free(markdown_core_compiler *compiler);

/* Hands every event of `events` to its handler. False when a handler
 * failed or storage could not be had. */
bool markdown_core_compile(markdown_core_compiler *compiler, const markdown_core_events *events,
                           const unsigned char *data, const markdown_core_compile_ops *const *ops, size_t kind_count,
                           void *reader);

bool markdown_core_compile_push(markdown_core_compiler *compiler, void *item);
void *markdown_core_compile_pop(markdown_core_compiler *compiler);
/* The element instance of the token being handed. */
static inline const void *markdown_core_compile_self(const markdown_core_compiler *compiler) {
    return compiler->current->context;
}
static inline void *markdown_core_compile_top(const markdown_core_compiler *compiler) {
    return compiler->stack_count ? compiler->stack[compiler->stack_count - 1] : NULL;
}

#ifdef __cplusplus
}
#endif

#endif
