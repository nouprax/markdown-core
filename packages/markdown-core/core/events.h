#ifndef MARKDOWN_CORE_EVENTS_H
#define MARKDOWN_CORE_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE EVENTS OF A READING (plan symbols-cleanroom.md 23.2): what the
 * tokenizer makes of an inline root's content, before any node exists.
 *
 * A token is a span of the content of one kind: `start` and `end` are
 * offsets into the content the reading reads, and `data` is the
 * construct's own word, which the construct that made the token reads and
 * writes and nothing else does (micromark's `_balanced`, `_inactive`, a
 * sequence's flanking). Kinds are numbers the dialect gives the elements'
 * token kinds as it is sealed; the core's own are below.
 *
 * An event enters or exits a token. A reading's events are in order of
 * their place, and every token is entered once and exited once, the exit
 * after the enter and nested as the tokens are. Resolvers rewrite the
 * sequence (markdown_core_events_splice); compile reads it once. */

typedef enum {
    MARKDOWN_CORE_TOKEN_NONE = 0,
    /* Bytes no construct read: text. */
    MARKDOWN_CORE_TOKEN_DATA,
    /* Spaces and tabs, and a line's end: what attention reads as a
     * boundary. */
    MARKDOWN_CORE_TOKEN_SPACE,
    MARKDOWN_CORE_TOKEN_LINE_END,
    /* A label's start and its end (label.h), text while no construct is
     * made of them. */
    MARKDOWN_CORE_TOKEN_LABEL,
    MARKDOWN_CORE_TOKEN_LABEL_END,
    /* What is left of a token a resolver made something over: text of its
     * bytes, and its `data` the event past what was resolved from it,
     * which a later pass steps over. */
    MARKDOWN_CORE_TOKEN_MARKER,
    /* The first kind the dialect gives an element's token. */
    MARKDOWN_CORE_TOKEN_FIRST
} markdown_core_token_kind;

typedef struct markdown_core_token {
    uint16_t kind;
    uint32_t start, end;
    uint32_t data;
} markdown_core_token;

typedef enum { MARKDOWN_CORE_ENTER = 0, MARKDOWN_CORE_EXIT = 1 } markdown_core_phase;

typedef struct markdown_core_event {
    uint32_t phase : 1;
    uint32_t token : 31;
} markdown_core_event;

typedef struct markdown_core_events {
    markdown_core_token *tokens;
    size_t token_count, token_room;
    markdown_core_event *items;
    size_t count, room;
    /* What constructs keep for compile beside their tokens' words, each
     * record at an offset a token's `data` names
     * (markdown_core_events_keep). */
    unsigned char *kept;
    size_t kept_size, kept_room;
    /* Set when storage could not be had: every write after it does
     * nothing, and the reading fails. */
    bool failed;
} markdown_core_events;

void markdown_core_events_init(markdown_core_events *events);
void markdown_core_events_free(markdown_core_events *events);
/* Empties the events and tokens, keeping their storage. */
void markdown_core_events_clear(markdown_core_events *events);

/* A new token of `kind` from `start`, its end still `start`: its index,
 * or UINT32_MAX when storage could not be had. */
uint32_t markdown_core_events_token(markdown_core_events *events, uint16_t kind, uint32_t start);

/* Appends an event of `phase` for `token`. */
void markdown_core_events_push(markdown_core_events *events, markdown_core_phase phase, uint32_t token);

/* Replaces the `removed` events from `at` with the `added` at `items`
 * (micromark's splice): the events after move. */
void markdown_core_events_splice(markdown_core_events *events, size_t at, size_t removed,
                                 const markdown_core_event *items, size_t added);

/* Drops the events from `count` on, and the tokens from `tokens` on: what an
 * attempt that failed made. */
void markdown_core_events_cut(markdown_core_events *events, size_t count, size_t tokens);

/* Keeps a copy of the `size` bytes at `record` for compile: its offset,
 * which a token's `data` may hold, or UINT32_MAX when storage could not be
 * had. An attempt that fails leaves what it kept, which nothing reads. */
uint32_t markdown_core_events_keep(markdown_core_events *events, const void *record, size_t size);
static inline const void *markdown_core_events_kept(const markdown_core_events *events, uint32_t offset) {
    return events->kept + offset;
}

static inline markdown_core_token *markdown_core_event_token(const markdown_core_events *events, size_t at) {
    return &events->tokens[events->items[at].token];
}

#ifdef __cplusplus
}
#endif

#endif
