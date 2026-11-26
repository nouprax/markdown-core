#ifndef MARKDOWN_CORE_TOKENIZER_H
#define MARKDOWN_CORE_TOKENIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE TOKENIZER (plan symbols-cleanroom.md 23.3), micromark's shape in C.
 *
 * A construct is a state machine over the codes of the content: each state
 * is handed the code at the reading's place, and either consumes it
 * (markdown_core_consume) and says which state reads the next, or ends the
 * construct, which succeeds (markdown_core_ok) or fails (markdown_core_nok),
 * consuming nothing. A state may hand the same code on to another state by
 * calling it. A code is a byte of the content, or MARKDOWN_CORE_EOF past
 * its end, which no state consumes.
 *
 * Constructs are tried through attempts: an attempt runs a construct, or
 * the first of a list that succeeds, and goes on at `ok` past what it read;
 * when none does it goes back to where it began, every event it made gone,
 * and goes on at `nok` there. A check is an attempt that keeps nothing even
 * when it succeeds. Either way the state named next is handed the code
 * where the reading then is.
 *
 * Effects: a construct makes tokens by entering and exiting them
 * (markdown_core_enter, markdown_core_exit), which nest; that is all it
 * writes. It reads the content's codes and its own tokens' `data`, and
 * keeps what it counts in its attempt's frame (markdown_core_frame_local);
 * it reads no other construct's state. */

#define MARKDOWN_CORE_EOF (-1)

typedef struct markdown_core_tokenizer markdown_core_tokenizer;
typedef struct markdown_core_state markdown_core_state;
struct markdown_core_state {
    markdown_core_state (*step)(markdown_core_tokenizer *, int code);
};

typedef struct markdown_core_construct {
    /* The construct's first state, handed the code it starts at. */
    markdown_core_state (*start)(markdown_core_tokenizer *, int code);
    /* Rewrites the events it made, from `from`, once it succeeded. */
    void (*resolve)(markdown_core_tokenizer *, size_t from);
    /* Rewrites the events up to the end, once it succeeded: a closer that
     * groups what an opener before it began (micromark's resolveTo). */
    void (*resolve_to)(markdown_core_tokenizer *, size_t from);
    /* Rewrites all the reading's events at its end, once for every reading
     * in which the construct succeeded (micromark's resolveAll). */
    void (*resolve_all)(markdown_core_tokenizer *);
    /* What the construct holds open in the events so far that a later
     * construct may close or take: a settle point is a place no construct
     * holds anything open (23.6). NULL holds nothing. */
    size_t (*holds)(const markdown_core_tokenizer *);
    /* The element instance the construct is of, which its states read. */
    const void *context;
} markdown_core_construct;

/* The constructs a byte may start, in the order they are tried. */
typedef struct markdown_core_constructs {
    const markdown_core_construct *const *items;
    /* items[offsets[c] .. offsets[c + 1]) start at byte c. */
    const uint16_t *offsets;
} markdown_core_constructs;

/* A frame's room for the construct's own counts. */
#define MARKDOWN_CORE_FRAME_WORDS 6

typedef struct markdown_core_frame {
    markdown_core_state ok, nok;
    /* The constructs tried, `count` of them at `list`, or the one `one`
     * when `list` is NULL, and which is being tried. */
    const markdown_core_construct *const *list;
    const markdown_core_construct *one;
    size_t count, tried;
    uint32_t position;
    size_t events, tokens, opens;
    bool check;
    uint32_t words[MARKDOWN_CORE_FRAME_WORDS];
} markdown_core_frame;

struct markdown_core_tokenizer {
    const unsigned char *data;
    uint32_t size, position;
    markdown_core_events events;
    /* The tokens entered and not exited, innermost last. */
    uint32_t *opens;
    size_t open_count, open_room;
    markdown_core_frame *frames;
    size_t frame_count, frame_room;
    /* The constructs whose resolve_all the end runs, each once. */
    const markdown_core_construct **alls;
    size_t all_count, all_room;
    markdown_core_constructs text;
    /* Where the text's attempt at a byte began. */
    uint32_t text_from;
    /* Whether the state just handed a code consumed it. */
    bool consumed;
    bool done;
    /* What the reading reads for: the parser, which constructs reach
     * through their context's services. */
    void *reader;
};

void markdown_core_tokenizer_init(markdown_core_tokenizer *tokenizer);
void markdown_core_tokenizer_free(markdown_core_tokenizer *tokenizer);

/* Reads `size` bytes of content at `data` as text: every byte a construct
 * of `text` starts is tried there, and what none reads is data; then every
 * resolver runs. False when storage could not be had. The events stay in
 * `tokenizer->events` until the next reading. */
bool markdown_core_tokenize_text(markdown_core_tokenizer *tokenizer, const unsigned char *data, uint32_t size,
                                 const markdown_core_constructs *text, void *reader);

/* EFFECTS. */
uint32_t markdown_core_enter(markdown_core_tokenizer *tokenizer, uint16_t kind);
uint32_t markdown_core_exit(markdown_core_tokenizer *tokenizer, uint16_t kind);
void markdown_core_consume(markdown_core_tokenizer *tokenizer);
markdown_core_state markdown_core_ok(markdown_core_tokenizer *tokenizer);
markdown_core_state markdown_core_nok(markdown_core_tokenizer *tokenizer);

/* Runs `construct` at `code`, going on at `ok` or `nok`. */
markdown_core_state markdown_core_attempt(markdown_core_tokenizer *tokenizer, const markdown_core_construct *construct,
                                          markdown_core_state ok, markdown_core_state nok, int code);
/* Runs the first of the `count` constructs at `list` that succeeds. */
markdown_core_state markdown_core_attempt_list(markdown_core_tokenizer *tokenizer,
                                               const markdown_core_construct *const *list, size_t count,
                                               markdown_core_state ok, markdown_core_state nok, int code);
/* Runs `construct` and keeps nothing of it either way. */
markdown_core_state markdown_core_check(markdown_core_tokenizer *tokenizer, const markdown_core_construct *construct,
                                        markdown_core_state ok, markdown_core_state nok, int code);

/* The counts of the construct the innermost attempt runs. */
static inline uint32_t *markdown_core_frame_local(markdown_core_tokenizer *tokenizer) {
    return tokenizer->frames[tokenizer->frame_count - 1].words;
}

/* The code `offset` bytes from where the reading is, or MARKDOWN_CORE_EOF:
 * negative offsets look back. */
static inline int markdown_core_code_at(const markdown_core_tokenizer *tokenizer, int64_t offset) {
    const int64_t at = (int64_t)tokenizer->position + offset;
    return at < 0 || at >= (int64_t)tokenizer->size ? MARKDOWN_CORE_EOF : tokenizer->data[at];
}

/* Whether no construct holds anything open in the events so far. */
bool markdown_core_tokenizer_settled(const markdown_core_tokenizer *tokenizer);

static inline markdown_core_state markdown_core_state_of(markdown_core_state (*step)(markdown_core_tokenizer *, int)) {
    markdown_core_state state = {step};
    return state;
}

#ifdef __cplusplus
}
#endif

#endif
