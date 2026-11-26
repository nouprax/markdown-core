#ifndef MARKDOWN_CORE_TOKENIZER_H
#define MARKDOWN_CORE_TOKENIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "events.h"
#include "reader.h"

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
     * in which the construct succeeded (micromark's resolveAll); the
     * constructs that share one run it once (the attention's). A `late`
     * one runs after every other: what it rewrites is what the others
     * decided (the attention pairs what the labels left). */
    void (*resolve_all)(markdown_core_tokenizer *);
    bool late;
    /* What the construct holds open in the events so far that a later
     * construct may close or take: a settle point is a place no construct
     * holds anything open (23.6), asked with the construct's context at
     * every start of the text, whether the construct ran or not. NULL holds
     * nothing. */
    bool (*holds)(const markdown_core_tokenizer *, const void *context);
    /* Where data that meets the construct's byte at `at` runs on to: past
     * `at` when the bytes there are text whatever follows (a run of
     * attention that can neither open nor close), else `at`, where the
     * construct may start. NULL: data stops at the byte. */
    uint32_t (*passes)(markdown_core_tokenizer *, const void *context, uint32_t at);
    /* The element instance the construct is of, which its states read. */
    const void *context;
    /* Whether what it reads is only the start of a construct a later one
     * decides (a label's start): the notes of the alternatives given up
     * before it stay (markdown_core_note_push). */
    bool opens;
} markdown_core_construct;

/* The constructs a byte may start, in the order they are tried. */
typedef struct markdown_core_constructs {
    const markdown_core_construct *const *items;
    /* items[offsets[c] .. offsets[c + 1]) start at byte c. */
    const uint16_t *offsets;
    /* The constructs that may hold something open (`holds`), `holder_count`
     * of them, each asked whether it does whether it ran or not. */
    const markdown_core_construct *const *holders;
    size_t holder_count;
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
    /* The notes when the attempt began, and when the alternative being
     * tried began. */
    size_t given, notes;
    bool check;
    uint32_t words[MARKDOWN_CORE_FRAME_WORDS];
    /* Where the reading's cursor stood, and where its line starts in the
     * data (the reading's `cursor`, `base`). */
    markdown_core_reader_stand stand;
    uint32_t base;
} markdown_core_frame;

/* A MARK: a token a construct keeps on the reading's list of marks, with
 * its construct's `tag`, for a later construct to find without walking
 * the events (a label's start, which its end looks for). A mark lives as
 * long as its token: an attempt that fails drops the marks of the tokens
 * it made. A construct takes a mark off, or changes its token's `data`,
 * only as it ends, so that nothing an attempt did to the marks needs
 * undoing but what it made. */
typedef struct markdown_core_mark {
    uint32_t token;
    uint32_t tag;
} markdown_core_mark;

/* A GROUP: a token whose enter goes before event `from` of the reading
 * and whose exit before event `to`, both counted in the events as they
 * stand when it is added (markdown_core_group_add): what a resolver makes
 * over the tokens a construct read (a pair of attention, a label's
 * construct). The groups go in at the reading's end, after every resolver,
 * those at one place in nesting order: the exits of groups entered before
 * it, inner first; then the enters, outer first; then the exits of the
 * groups that hold nothing. */
typedef struct markdown_core_group {
    uint32_t token, from, to, order;
} markdown_core_group;

/* A NOTE: what a construct began and gave up (node.h, OPENER), kept
 * whatever becomes of its attempt, as the token `token` and the `count`
 * words of the note's own from `first` in the list of words. A note of an
 * alternative given up is void once a later alternative at the same start
 * of the text succeeds, unless that one `opens` (markdown_core_construct):
 * what reads the byte as a construct leaves none of the others' notes. */
typedef struct markdown_core_note {
    markdown_core_token token;
    uint32_t first, count;
    bool gone;
} markdown_core_note;

/* WHAT A READING READS: the content `data`, `size` bytes, read from
 * `from` on (the bytes before it may be looked back at); the constructs
 * of the text; where a cut begins, at which the text stops (UINT32_MAX
 * for none); the reader, which constructs reach through their context;
 * the block of the elements' records for the reading, `room` bytes,
 * zeroed as it begins; and `start`, asked at each start of the text at
 * which nothing is held (markdown_core_tokenizer_settled), which stops
 * the reading there by answering false. A reading that `halts` stops at
 * the first start of the text at which a construct that `opens` holds
 * something (a label open), or at its end when one does: `stopped` says
 * where, and nothing is resolved, as what is held waits on the labels the
 * text declares, which the reading does not know yet. */
typedef struct markdown_core_read {
    const unsigned char *data;
    uint32_t size, from, cut;
    const markdown_core_constructs *text;
    void *reader;
    unsigned char *records;
    size_t room;
    bool (*start)(markdown_core_tokenizer *, void *context);
    void *context;
    bool halts;
    /* THE CURSOR: the line view a block reading's constructs scan the line
     * through (reader.h), or NULL. Its offsets count from the reading's
     * `base`, where its line starts in the data, which the reading sets as
     * it takes a line (markdown_core_cursor_line); every attempt goes back
     * to where the cursor stood as it began when it fails, and what a
     * construct scanned it consumes as it ends its token
     * (markdown_core_cursor_move). */
    markdown_core_reader *cursor;
} markdown_core_read;

struct markdown_core_tokenizer {
    const unsigned char *data;
    uint32_t size, position;
    /* Where the cut begins, or UINT32_MAX; whether a construct read across
     * it, which gives the text its bytes again (UINT32_MAX then). */
    uint32_t cut;
    bool crossed;
    /* Where the earliest construct of the text that read the end began
     * (a state handed MARKDOWN_CORE_EOF, a look past the end), or
     * UINT32_MAX while none has: what a construct decided before it read
     * the end stays decided however the content goes on. */
    uint32_t undecided;
    /* Where `start` stopped the reading, or where it halted, or
     * UINT32_MAX; whether it halts (markdown_core_read), and did. */
    uint32_t stopped;
    bool halts, halted;
    /* Where the data being read began while a construct's `passes` is
     * asked, else UINT32_MAX: an end it reads is the data's. */
    uint32_t passing;
    markdown_core_events events;
    /* The tokens entered and not exited, innermost last. */
    uint32_t *opens;
    size_t open_count, open_room;
    markdown_core_frame *frames;
    size_t frame_count, frame_room;
    /* The constructs whose resolve_all the end runs, each once. */
    const markdown_core_construct **alls;
    size_t all_count, all_room;
    markdown_core_mark *marks;
    size_t mark_count, mark_room;
    markdown_core_group *groups;
    size_t group_count, group_room;
    markdown_core_note *notes;
    size_t note_count, note_room;
    uint32_t *words;
    size_t word_count, word_room;
    /* Room a resolver borrows while it runs, kept for the next. */
    void *room;
    size_t room_size;
    markdown_core_constructs text;
    /* Where the text's attempt at a byte began. */
    uint32_t text_from;
    /* Whether the state just handed a code consumed it. */
    bool consumed;
    /* The bytes failed attempts and checks gave back to be read again,
     * summed until the reader takes them: the engine's work counter. */
    size_t rewound;
    bool done;
    /* Where a reading fed in chunks goes on (markdown_core_pause). */
    markdown_core_state resume;
    /* What the reading reads for: the parser, which constructs reach
     * through their context's services. */
    void *reader;
    unsigned char *records;
    bool (*start)(markdown_core_tokenizer *, void *context);
    void *context;
    markdown_core_reader *cursor;
    uint32_t base;
};

void markdown_core_tokenizer_init(markdown_core_tokenizer *tokenizer);
void markdown_core_tokenizer_free(markdown_core_tokenizer *tokenizer);

/* Reads the content of `read` as text: every byte a construct of the
 * text starts is tried there, and what none reads is data; then every
 * resolve_all runs. Data is not joined: a data token runs from a byte no
 * construct read to the next a construct may start, and the text the
 * reading's nodes make is joined where the tree's texts are (blocks.c,
 * S_consolidate_text). False when storage could
 * not be had. The events stay in `tokenizer->events` until the next
 * reading. */
bool markdown_core_tokenize(markdown_core_tokenizer *tokenizer, const markdown_core_read *read);

/* A READING FED AS ITS TEXT GROWS (plan symbols-cleanroom.md 23.14.12,
 * micromark's `write`): the document's one reading, begun once with its
 * first state, then fed the text read so far, `size` bytes at `data`
 * (the text grows at its end, and may move), to read on at `from`, where
 * its driver stands. A feed runs from where the last one paused: a state
 * that has read what it reads pauses the reading (markdown_core_pause),
 * naming the state the next feed starts in, at no attempt. The end runs
 * every resolver. False, from a feed or the end, when storage could not
 * be had (`events.failed`). */
void markdown_core_tokenizer_begin(markdown_core_tokenizer *tokenizer, const markdown_core_read *read,
                                   markdown_core_state state);
bool markdown_core_tokenizer_feed(markdown_core_tokenizer *tokenizer, const unsigned char *data, uint32_t size,
                                  uint32_t from);
markdown_core_state markdown_core_pause(markdown_core_tokenizer *tokenizer, markdown_core_state next);
bool markdown_core_tokenizer_end(markdown_core_tokenizer *tokenizer);

/* The cursor's line starts at `base` in the data. */
static inline void markdown_core_cursor_line(markdown_core_tokenizer *tokenizer, uint32_t base) {
    tokenizer->base = base;
}

/* The reading consumes what its cursor scanned: its position moves on to
 * where the cursor stands, when that is past it. */
void markdown_core_cursor_move(markdown_core_tokenizer *tokenizer);

/* EFFECTS. */
uint32_t markdown_core_enter(markdown_core_tokenizer *tokenizer, uint16_t kind);
uint32_t markdown_core_exit(markdown_core_tokenizer *tokenizer, uint16_t kind);
void markdown_core_consume(markdown_core_tokenizer *tokenizer);
/* Consumes the code handed and every byte up to `end`, which is past it:
 * a construct that scans a span at once (a scanner's match). */
void markdown_core_consume_to(markdown_core_tokenizer *tokenizer, uint32_t end);
/* The reading moves on to `end`, at or past where it is: what a state read
 * through a reader of its own, the block reading's column cursor, which
 * may read several spans in one step (blocks.c, THE DOCUMENT). */
void markdown_core_move_to(markdown_core_tokenizer *tokenizer, uint32_t end);
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

/* The construct being tried read the end of the content. */
void markdown_core_met_end(markdown_core_tokenizer *tokenizer);

/* The code `offset` bytes from where the reading is, or MARKDOWN_CORE_EOF:
 * negative offsets look back. A look past the end reads the end. */
static inline int markdown_core_code_at(markdown_core_tokenizer *tokenizer, int64_t offset) {
    const int64_t at = (int64_t)tokenizer->position + offset;
    if (at >= (int64_t)tokenizer->size) {
        markdown_core_met_end(tokenizer);
        return MARKDOWN_CORE_EOF;
    }
    return at < 0 ? MARKDOWN_CORE_EOF : tokenizer->data[at];
}

/* The element instance of the construct being tried: the context of the
 * innermost attempt's construct that has one. */
const void *markdown_core_self(const markdown_core_tokenizer *tokenizer);

/* The record `offset` bytes into the block of records of the read. */
static inline void *markdown_core_read_record(const markdown_core_tokenizer *tokenizer, size_t offset) {
    return tokenizer->records + offset;
}

/* Keeps a note of kind `kind` over `start` up to `end` with `data` and the
 * `count` words at `words`. */
bool markdown_core_note_push(markdown_core_tokenizer *tokenizer, uint16_t kind, uint32_t start, uint32_t end,
                             uint32_t data, const uint32_t *words, size_t count);

/* Room of at least `size` bytes for the resolver running, its contents
 * kept from the last asked, or NULL when storage could not be had. */
void *markdown_core_tokenizer_room(markdown_core_tokenizer *tokenizer, size_t size);

/* Lowers where the earliest undecided start is to `at`. */
static inline void markdown_core_undecide(markdown_core_tokenizer *tokenizer, uint32_t at) {
    if (at < tokenizer->undecided) {
        tokenizer->undecided = at;
    }
}

/* MARKS. */
bool markdown_core_mark_push(markdown_core_tokenizer *tokenizer, uint32_t tag, uint32_t token);
/* The innermost mark of `tag`, its index in the list, or SIZE_MAX. */
size_t markdown_core_mark_find(const markdown_core_tokenizer *tokenizer, uint32_t tag);
/* Takes off the mark at `at` and every mark after it. */
static inline void markdown_core_mark_cut(markdown_core_tokenizer *tokenizer, size_t at) {
    if (at < tokenizer->mark_count) {
        tokenizer->mark_count = at;
    }
}

/* GROUPS. Adds a group of `token` over events `from` up to `to`. */
bool markdown_core_group_add(markdown_core_tokenizer *tokenizer, uint32_t token, uint32_t from, uint32_t to);

/* Whether no construct holds anything open in the events so far. */
/* A reading that halts halts where it stands: a construct meets what
 * waits on what the reading does not know (markdown_core_read, `halts`). */
void markdown_core_halt(markdown_core_tokenizer *tokenizer);
bool markdown_core_tokenizer_settled(const markdown_core_tokenizer *tokenizer);

static inline markdown_core_state markdown_core_state_of(markdown_core_state (*step)(markdown_core_tokenizer *, int)) {
    markdown_core_state state = {step};
    return state;
}

#ifdef __cplusplus
}
#endif

#endif
