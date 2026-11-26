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
static inline void *markdown_core_compile_top(const markdown_core_compiler *compiler) {
    return compiler->stack_count ? compiler->stack[compiler->stack_count - 1] : NULL;
}

#ifdef __cplusplus
}
#endif

#endif
