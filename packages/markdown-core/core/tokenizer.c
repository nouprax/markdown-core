#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "tokenizer.h"

static markdown_core_state S_end_step(markdown_core_tokenizer *tokenizer, int code);

/* The two ends a state names instead of a next state, and the end of the
 * reading: never stepped, and told apart by their addresses alone. A
 * linker that folds identical functions (MSVC's /OPT:ICF, on in its
 * release builds) would give two bodies that only assert one address, and
 * a failed attempt would then settle as one that succeeded; so each does
 * its own thing to a reading that steps it by mistake, which ends it. */
static markdown_core_state S_ok_step(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    assert(false);
    tokenizer->events.failed = true;
    return markdown_core_state_of(S_end_step);
}

static markdown_core_state S_nok_step(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    assert(false);
    tokenizer->events.failed = true;
    tokenizer->done = true;
    return markdown_core_state_of(S_end_step);
}

static markdown_core_state S_end_step(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    assert(false);
    tokenizer->done = true;
    return markdown_core_state_of(S_end_step);
}

markdown_core_state markdown_core_ok(markdown_core_tokenizer *tokenizer) {
    (void)tokenizer;
    return markdown_core_state_of(S_ok_step);
}

markdown_core_state markdown_core_nok(markdown_core_tokenizer *tokenizer) {
    (void)tokenizer;
    return markdown_core_state_of(S_nok_step);
}

void markdown_core_tokenizer_init(markdown_core_tokenizer *tokenizer) {
    *tokenizer = (markdown_core_tokenizer){0};
    markdown_core_events_init(&tokenizer->events);
}

void markdown_core_tokenizer_free(markdown_core_tokenizer *tokenizer) {
    markdown_core_events_free(&tokenizer->events);
    markdown_core_free(tokenizer->opens);
    markdown_core_free(tokenizer->frames);
    markdown_core_free(tokenizer->alls);
    markdown_core_free(tokenizer->marks);
    markdown_core_free(tokenizer->groups);
    markdown_core_free(tokenizer->notes);
    markdown_core_free(tokenizer->words);
    markdown_core_free(tokenizer->room);
    *tokenizer = (markdown_core_tokenizer){0};
}

uint32_t markdown_core_enter(markdown_core_tokenizer *tokenizer, uint16_t kind) {
    markdown_core_events *events = &tokenizer->events;
    const uint32_t token = markdown_core_events_token(events, kind, tokenizer->position);
    uint32_t *opens =
        markdown_core_reserve(tokenizer->opens, &tokenizer->open_room, tokenizer->open_count + 1, sizeof(*opens));
    if (opens) {
        tokenizer->opens = opens;
    }
    if (!opens || token == UINT32_MAX) {
        events->failed = true;
        return UINT32_MAX;
    }
    opens[tokenizer->open_count++] = token;
    markdown_core_events_push(events, MARKDOWN_CORE_ENTER, token);
    return token;
}

uint32_t markdown_core_exit(markdown_core_tokenizer *tokenizer, uint16_t kind) {
    markdown_core_events *events = &tokenizer->events;
    if (events->failed || !tokenizer->open_count) {
        events->failed = true;
        return UINT32_MAX;
    }
    const uint32_t token = tokenizer->opens[--tokenizer->open_count];
    assert(events->tokens[token].kind == kind);
    (void)kind;
    events->tokens[token].end = tokenizer->position;
    markdown_core_events_push(events, MARKDOWN_CORE_EXIT, token);
    return token;
}

void markdown_core_give_back(markdown_core_tokenizer *tokenizer, uint32_t at) {
    assert(!tokenizer->frame_count && at >= tokenizer->base && at <= tokenizer->position);
    /* What is read again is the engine's work, as what an attempt gives
     * back is. */
    tokenizer->rewound += tokenizer->position - at;
    tokenizer->position = at;
    tokenizer->counted = tokenizer->split = false;
    markdown_core_reader *cursor = tokenizer->cursor;
    if (cursor) {
        cursor->offset = (bufsize_t)(at - tokenizer->base);
        cursor->column = (bufsize_t)markdown_core_column(tokenizer);
        cursor->split = false;
        /* What the first non-space byte tells is found again. */
        cursor->nonspace = cursor->offset;
    }
}

uint32_t markdown_core_column(markdown_core_tokenizer *tokenizer) {
    if (!tokenizer->counted) {
        /* Counted from the line's start: a tab to the next stop, a scalar
         * at its first byte. The place is at no tab's inside. */
        uint32_t column = 0;
        for (uint32_t at = tokenizer->base; at < tokenizer->position; at++) {
            const unsigned char byte = tokenizer->data[at];
            if (byte == '\t') {
                column += MARKDOWN_CORE_TAB_STOP - column % MARKDOWN_CORE_TAB_STOP;
            } else {
                /* A scalar is counted at its last byte. */
                column += at + 1 >= tokenizer->size || (tokenizer->data[at + 1] & 0xC0) != 0x80;
            }
        }
        tokenizer->column = column;
        tokenizer->counted = true;
    }
    return tokenizer->column;
}

void markdown_core_consume(markdown_core_tokenizer *tokenizer) {
    assert(!tokenizer->consumed && tokenizer->position < tokenizer->size);
    tokenizer->consumed = true;
    if (!tokenizer->columns) {
        tokenizer->position++;
        return;
    }
    const unsigned char byte = tokenizer->data[tokenizer->position];
    if (byte == '\t') {
        /* A column of the tab: the tab is read at its stop. */
        const uint32_t column = markdown_core_column(tokenizer) + 1;
        tokenizer->column = column;
        tokenizer->split = column % MARKDOWN_CORE_TAB_STOP != 0;
        tokenizer->position += !tokenizer->split;
        return;
    }
    /* A code is a scalar: its continuation bytes are read with it, and so
     * are any after a byte that begins none, as the column cursor reads
     * them (reader.h). */
    uint32_t at = tokenizer->position + 1;
    while (at < tokenizer->ending && at < tokenizer->size && (tokenizer->data[at] & 0xC0) == 0x80) {
        at++;
    }
    tokenizer->column++;
    tokenizer->position = at;
}

void markdown_core_consume_to(markdown_core_tokenizer *tokenizer, uint32_t end) {
    assert(!tokenizer->consumed && tokenizer->position < end && end <= tokenizer->size);
    tokenizer->position = end;
    tokenizer->consumed = true;
    tokenizer->counted = tokenizer->split = false;
}

void markdown_core_move_to(markdown_core_tokenizer *tokenizer, uint32_t end) {
    assert(tokenizer->position <= end && end <= tokenizer->size);
    tokenizer->consumed = true;
    if (end != tokenizer->position) {
        tokenizer->position = end;
        tokenizer->counted = tokenizer->split = false;
    }
}

void markdown_core_cursor_move(markdown_core_tokenizer *tokenizer) {
    const markdown_core_reader *cursor = tokenizer->cursor;
    if (cursor) {
        const uint32_t at = tokenizer->base + (uint32_t)cursor->offset;
        if (at > tokenizer->size || at < tokenizer->position) {
            return;
        }
        /* Inside a tab, the cursor is past the reading when it read more of
         * the tab's columns. */
        if (at > tokenizer->position ||
            (cursor->split && (!tokenizer->split || (uint32_t)cursor->column > tokenizer->column))) {
            tokenizer->position = at;
            tokenizer->consumed = true;
            tokenizer->column = (uint32_t)cursor->column;
            tokenizer->split = cursor->split;
            tokenizer->counted = true;
        }
    }
}

void markdown_core_cursor_follow(markdown_core_tokenizer *tokenizer) {
    markdown_core_reader *cursor = tokenizer->cursor;
    if (!cursor || tokenizer->position < tokenizer->base) {
        return;
    }
    const bufsize_t offset = (bufsize_t)(tokenizer->position - tokenizer->base);
    if (offset > cursor->offset || (offset == cursor->offset && tokenizer->split &&
                                    (!cursor->split || markdown_core_column(tokenizer) > (uint32_t)cursor->column))) {
        cursor->offset = offset;
        cursor->column = (bufsize_t)markdown_core_column(tokenizer);
        cursor->split = tokenizer->split;
    }
}

void markdown_core_met_end(markdown_core_tokenizer *tokenizer) {
    /* Data reads no end: only a construct decides by it, or what a
     * construct's `passes` said of the data's bytes. */
    if (tokenizer->frame_count) {
        markdown_core_undecide(tokenizer, tokenizer->frames[0].position);
    } else {
        markdown_core_undecide(tokenizer, tokenizer->passing);
    }
}

void *markdown_core_tokenizer_room(markdown_core_tokenizer *tokenizer, size_t size) {
    if (size > tokenizer->room_size) {
        void *room = markdown_core_realloc(tokenizer->room, size);
        if (!room) {
            tokenizer->events.failed = true;
            return NULL;
        }
        tokenizer->room = room;
        tokenizer->room_size = size;
    }
    return tokenizer->room;
}

const void *markdown_core_self(const markdown_core_tokenizer *tokenizer) {
    for (size_t at = tokenizer->frame_count; at--;) {
        const markdown_core_frame *frame = &tokenizer->frames[at];
        const markdown_core_construct *construct = frame->list ? frame->list[frame->tried] : frame->one;
        if (construct->context) {
            return construct->context;
        }
    }
    return NULL;
}

bool markdown_core_note_push(markdown_core_tokenizer *tokenizer, uint16_t kind, uint32_t start, uint32_t end,
                             uint32_t data, const uint32_t *words, size_t count) {
    /* Each kept as it is had: a reserve that moved one freed where it was. */
    markdown_core_note *notes =
        markdown_core_reserve(tokenizer->notes, &tokenizer->note_room, tokenizer->note_count + 1, sizeof(*notes));
    if (notes) {
        tokenizer->notes = notes;
    }
    uint32_t *room = notes && count ? markdown_core_reserve(tokenizer->words, &tokenizer->word_room,
                                                            tokenizer->word_count + count, sizeof(*room))
                                    : tokenizer->words;
    if (!notes || (count && !room)) {
        tokenizer->events.failed = true;
        return false;
    }
    tokenizer->words = room;
    if (count) {
        memcpy(room + tokenizer->word_count, words, count * sizeof(*words));
    }
    notes[tokenizer->note_count++] =
        (markdown_core_note){{kind, start, end, data}, (uint32_t)tokenizer->word_count, (uint32_t)count, false};
    tokenizer->word_count += count;
    return true;
}

bool markdown_core_mark_push(markdown_core_tokenizer *tokenizer, uint32_t tag, uint32_t token) {
    markdown_core_mark *marks =
        markdown_core_reserve(tokenizer->marks, &tokenizer->mark_room, tokenizer->mark_count + 1, sizeof(*marks));
    if (!marks) {
        tokenizer->events.failed = true;
        return false;
    }
    tokenizer->marks = marks;
    marks[tokenizer->mark_count++] = (markdown_core_mark){token, tag};
    return true;
}

size_t markdown_core_mark_find(const markdown_core_tokenizer *tokenizer, uint32_t tag) {
    for (size_t at = tokenizer->mark_count; at--;) {
        if (tokenizer->marks[at].tag == tag) {
            return at;
        }
    }
    return SIZE_MAX;
}

static const markdown_core_construct *S_tried(const markdown_core_frame *frame) {
    return frame->list ? frame->list[frame->tried] : frame->one;
}

/* Starts an attempt: false when storage could not be had, which ends the
 * reading. */
static bool S_push_frame(markdown_core_tokenizer *tokenizer, markdown_core_state ok, markdown_core_state nok,
                         const markdown_core_construct *const *list, const markdown_core_construct *one, size_t count,
                         bool check) {
    markdown_core_frame *frames =
        markdown_core_reserve(tokenizer->frames, &tokenizer->frame_room, tokenizer->frame_count + 1, sizeof(*frames));
    if (!frames) {
        tokenizer->events.failed = true;
        tokenizer->done = true;
        return false;
    }
    tokenizer->frames = frames;
    frames[tokenizer->frame_count++] = (markdown_core_frame){.ok = ok,
                                                             .nok = nok,
                                                             .list = list,
                                                             .one = one,
                                                             .count = count,
                                                             .position = tokenizer->position,
                                                             .events = tokenizer->events.count,
                                                             .tokens = tokenizer->events.token_count,
                                                             .opens = tokenizer->open_count,
                                                             .given = tokenizer->note_count,
                                                             .notes = tokenizer->note_count,
                                                             .check = check,
                                                             .cursor = tokenizer->cursor,
                                                             .base = tokenizer->base,
                                                             .column = tokenizer->column,
                                                             .ending = tokenizer->ending,
                                                             .counted = tokenizer->counted,
                                                             .split = tokenizer->split};
    if (tokenizer->cursor) {
        frames[tokenizer->frame_count - 1].stand = markdown_core_reader_stand_of(tokenizer->cursor);
    }
    return true;
}

static markdown_core_state S_begin(markdown_core_tokenizer *tokenizer, markdown_core_state ok, markdown_core_state nok,
                                   const markdown_core_construct *const *list, const markdown_core_construct *one,
                                   size_t count, bool check, int code) {
    assert(count);
    if (!S_push_frame(tokenizer, ok, nok, list, one, count, check)) {
        return markdown_core_state_of(S_end_step);
    }
    /* The construct is handed the code where the reading stands, which it
     * has not consumed whatever the state that tried it moved past. */
    tokenizer->consumed = false;
    if (tokenizer->columns) {
        markdown_core_note_read(tokenizer, tokenizer->position);
    }
    return S_tried(&tokenizer->frames[tokenizer->frame_count - 1])->start(tokenizer, code);
}

markdown_core_state markdown_core_attempt_list(markdown_core_tokenizer *tokenizer,
                                               const markdown_core_construct *const *list, size_t count,
                                               markdown_core_state ok, markdown_core_state nok, int code) {
    return S_begin(tokenizer, ok, nok, list, NULL, count, false, code);
}

markdown_core_state markdown_core_attempt(markdown_core_tokenizer *tokenizer, const markdown_core_construct *construct,
                                          markdown_core_state ok, markdown_core_state nok, int code) {
    return S_begin(tokenizer, ok, nok, NULL, construct, 1, false, code);
}

markdown_core_state markdown_core_check(markdown_core_tokenizer *tokenizer, const markdown_core_construct *construct,
                                        markdown_core_state ok, markdown_core_state nok, int code) {
    return S_begin(tokenizer, ok, nok, NULL, construct, 1, true, code);
}

/* Goes back to where the innermost attempt began, nothing it made kept. */
static void S_restore(markdown_core_tokenizer *tokenizer, const markdown_core_frame *frame) {
    tokenizer->rewound += tokenizer->position - frame->position;
    tokenizer->position = frame->position;
    tokenizer->base = frame->base;
    tokenizer->column = frame->column;
    tokenizer->ending = frame->ending;
    tokenizer->counted = frame->counted;
    tokenizer->split = frame->split;
    tokenizer->cursor = frame->cursor;
    if (tokenizer->cursor) {
        markdown_core_reader_stand_back(tokenizer->cursor, &frame->stand);
    }
    tokenizer->open_count = frame->opens;
    markdown_core_events_cut(&tokenizer->events, frame->events, frame->tokens);
    while (tokenizer->mark_count && tokenizer->marks[tokenizer->mark_count - 1].token >= frame->tokens) {
        tokenizer->mark_count--;
    }
    while (tokenizer->group_count && tokenizer->groups[tokenizer->group_count - 1].token >= frame->tokens) {
        tokenizer->group_count--;
    }
}

/* The construct succeeded: its resolvers run, and its resolve_all is kept
 * for the end, once. */
static void S_resolved(markdown_core_tokenizer *tokenizer, const markdown_core_construct *construct, size_t from) {
    if (construct->resolve) {
        construct->resolve(tokenizer, from);
    }
    if (construct->resolve_to) {
        construct->resolve_to(tokenizer, from);
    }
    if (construct->resolve_all) {
        for (size_t i = 0; i < tokenizer->all_count; i++) {
            if (tokenizer->alls[i] == construct) {
                return;
            }
        }
        const markdown_core_construct **alls =
            markdown_core_reserve(tokenizer->alls, &tokenizer->all_room, tokenizer->all_count + 1, sizeof(*alls));
        if (!alls) {
            tokenizer->events.failed = true;
            return;
        }
        tokenizer->alls = alls;
        alls[tokenizer->all_count++] = construct;
    }
}

/* What a state named next, once the ends it named are taken: an end
 * pops its attempt and names the attempt's own next state, which the
 * code where the reading then is is handed to. */
static markdown_core_state S_settle(markdown_core_tokenizer *tokenizer, markdown_core_state next, int *code) {
    while (next.step == S_ok_step || next.step == S_nok_step) {
        if (!tokenizer->frame_count) {
            /* An end with no attempt: the text's own initial state never
             * names one. */
            tokenizer->done = true;
            return markdown_core_state_of(S_end_step);
        }
        markdown_core_frame *frame = &tokenizer->frames[tokenizer->frame_count - 1];
        if (next.step == S_ok_step) {
            const markdown_core_frame done = *frame;
            tokenizer->frame_count--;
            /* The text's alternative read its byte: the notes of those it
             * gave up before it are void. */
            if (!tokenizer->frame_count && done.tried && !S_tried(&done)->opens) {
                for (size_t at = done.given; at < done.notes; at++) {
                    tokenizer->notes[at].gone = true;
                }
            }
            if (done.check) {
                S_restore(tokenizer, &done);
            } else {
                S_resolved(tokenizer, S_tried(&done), done.events);
            }
            next = done.ok;
        } else {
            S_restore(tokenizer, frame);
            if (frame->tried + 1 < frame->count) {
                frame->tried++;
                frame->notes = tokenizer->note_count;
                memset(frame->words, 0, sizeof(frame->words));
                next = S_tried(frame)->start(tokenizer, markdown_core_code_at(tokenizer, 0));
                continue;
            }
            const markdown_core_state nok = frame->nok;
            tokenizer->frame_count--;
            next = nok;
        }
    }
    *code = markdown_core_code_at(tokenizer, 0);
    return next;
}

/* THE TEXT (micromark's `text` content type): every byte is offered to
 * the constructs it may start, in order; a byte none reads is data, and
 * data runs to the next byte a construct may start. The text stops where
 * the cut begins. */
static markdown_core_state S_text(markdown_core_tokenizer *tokenizer, int code);

static bool S_starts(const markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_constructs *text = &tokenizer->text;
    return code != MARKDOWN_CORE_EOF && text->offsets[code] != text->offsets[code + 1];
}

/* Where data meeting the constructs of the byte at `at` runs on to
 * (markdown_core_construct, `passes`). */
static uint32_t S_passes(markdown_core_tokenizer *tokenizer, uint32_t at) {
    const markdown_core_constructs *text = &tokenizer->text;
    const unsigned char byte = tokenizer->data[at];
    for (uint16_t i = text->offsets[byte]; i < text->offsets[byte + 1]; i++) {
        if (text->items[i]->passes) {
            const uint32_t end = text->items[i]->passes(tokenizer, text->items[i]->context, at);
            if (end > at) {
                return end;
            }
        }
    }
    return at;
}

/* A byte no construct read is data, and so is every byte after it up to
 * the next a construct may start, or the cut: the bytes a construct says
 * are text whatever follows are data too, the first among them. */
static markdown_core_state S_data(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const uint16_t *offsets = tokenizer->text.offsets;
    const uint32_t end = tokenizer->cut < tokenizer->size ? tokenizer->cut : tokenizer->size;
    const uint32_t from = tokenizer->position;
    tokenizer->passing = from;
    uint32_t at = from;
    while (at < end) {
        if (offsets[tokenizer->data[at]] == offsets[tokenizer->data[at] + 1]) {
            at++;
            continue;
        }
        const uint32_t past = S_passes(tokenizer, at);
        if (past > at) {
            at = past < end ? past : end;
        } else if (at == from) {
            at++;
        } else {
            break;
        }
    }
    tokenizer->passing = UINT32_MAX;
    /* Data right after data is the same data (micromark merges them as
     * the text resolves): a byte a construct failed at runs on in the
     * data before it. */
    markdown_core_events *events = &tokenizer->events;
    if (events->count && events->items[events->count - 1].phase == MARKDOWN_CORE_EXIT) {
        markdown_core_token *last = markdown_core_event_token(events, events->count - 1);
        if (last->kind == MARKDOWN_CORE_TOKEN_DATA && last->end == from) {
            markdown_core_consume_to(tokenizer, at);
            last->end = at;
            return markdown_core_state_of(S_text);
        }
    }
    markdown_core_enter(tokenizer, MARKDOWN_CORE_TOKEN_DATA);
    markdown_core_consume_to(tokenizer, at);
    markdown_core_exit(tokenizer, MARKDOWN_CORE_TOKEN_DATA);
    return markdown_core_state_of(S_text);
}

/* After a construct: one that read nothing is no construct, and its byte
 * is data. One that read across the cut took it. */
static markdown_core_state S_after(markdown_core_tokenizer *tokenizer, int code) {
    if (tokenizer->position == tokenizer->text_from) {
        return S_data(tokenizer, code);
    }
    if (tokenizer->position > tokenizer->cut) {
        /* What was cut is the text's again: the reading goes on to the
         * end. */
        tokenizer->crossed = true;
        tokenizer->cut = UINT32_MAX;
    }
    return S_text(tokenizer, code);
}

static markdown_core_state S_end(markdown_core_tokenizer *tokenizer) {
    tokenizer->done = true;
    return markdown_core_state_of(S_end_step);
}

/* Whether what the reading holds waits on what it does not know yet: a
 * construct that opens (a label) holds something open, whose end the
 * labels the text declares decide (markdown_core_read, `halts`). */
static bool S_waits(const markdown_core_tokenizer *tokenizer) {
    const markdown_core_constructs *text = &tokenizer->text;
    for (size_t i = 0; i < text->holder_count; i++) {
        if (text->holders[i]->opens && text->holders[i]->holds(tokenizer, text->holders[i]->context)) {
            return true;
        }
    }
    return false;
}

static markdown_core_state S_text(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF || tokenizer->position >= tokenizer->cut) {
        return S_end(tokenizer);
    }
    if (tokenizer->halted || (tokenizer->halts && S_waits(tokenizer))) {
        markdown_core_halt(tokenizer);
        return S_end(tokenizer);
    }
    if (tokenizer->start && tokenizer->undecided == UINT32_MAX && markdown_core_tokenizer_settled(tokenizer) &&
        !tokenizer->start(tokenizer, tokenizer->context)) {
        tokenizer->stopped = tokenizer->position;
        return S_end(tokenizer);
    }
    if (!S_starts(tokenizer, code)) {
        return S_data(tokenizer, code);
    }
    const markdown_core_constructs *text = &tokenizer->text;
    tokenizer->text_from = tokenizer->position;
    return markdown_core_attempt_list(tokenizer, text->items + text->offsets[code],
                                      (size_t)(text->offsets[code + 1] - text->offsets[code]),
                                      markdown_core_state_of(S_after), markdown_core_state_of(S_data), code);
}

bool markdown_core_group_add(markdown_core_tokenizer *tokenizer, uint32_t token, uint32_t from, uint32_t to) {
    markdown_core_group *groups =
        markdown_core_reserve(tokenizer->groups, &tokenizer->group_room, tokenizer->group_count + 1, sizeof(*groups));
    if (!groups) {
        tokenizer->events.failed = true;
        return false;
    }
    tokenizer->groups = groups;
    groups[tokenizer->group_count] = (markdown_core_group){token, from, to, (uint32_t)tokenizer->group_count};
    tokenizer->group_count++;
    return true;
}

/* An event a group adds, before event `slot`. */
typedef struct S_added {
    uint32_t slot, from, to, order, token;
    uint8_t phase;
} S_added;

/* Where at its place an added event goes: the exits of groups entered
 * before it, then the enters, then the exits of groups that hold
 * nothing. */
static int S_rank(const S_added *added) {
    return added->phase == MARKDOWN_CORE_ENTER ? 1 : added->from < added->slot ? 0 : 2;
}

static int S_order(const void *a, const void *b) {
    const S_added *x = a, *y = b;
    if (x->slot != y->slot) {
        return x->slot < y->slot ? -1 : 1;
    }
    if (S_rank(x) != S_rank(y)) {
        return S_rank(x) < S_rank(y) ? -1 : 1;
    }
    if (x->phase == MARKDOWN_CORE_EXIT) {
        /* The inner exits first: the one entered later, or of two entered
         * at one place the one added first. */
        if (x->from != y->from) {
            return x->from > y->from ? -1 : 1;
        }
        return x->order < y->order ? -1 : 1;
    }
    /* The outer enters first: the one that exits later, or of two that
     * exit at one place the one added later. */
    if (x->to != y->to) {
        return x->to > y->to ? -1 : 1;
    }
    return x->order > y->order ? -1 : 1;
}

/* The groups' events go in. */
static void S_merge(markdown_core_tokenizer *tokenizer) {
    markdown_core_events *events = &tokenizer->events;
    const size_t count = events->count, added = 2 * tokenizer->group_count;
    S_added *adds =
        markdown_core_tokenizer_room(tokenizer, added * sizeof(*adds) + (count + added) * sizeof(markdown_core_event));
    if (!adds) {
        return;
    }
    for (size_t i = 0; i < tokenizer->group_count; i++) {
        const markdown_core_group *group = &tokenizer->groups[i];
        adds[2 * i] = (S_added){group->from, group->from, group->to, group->order, group->token, MARKDOWN_CORE_ENTER};
        adds[2 * i + 1] = (S_added){group->to, group->from, group->to, group->order, group->token, MARKDOWN_CORE_EXIT};
    }
    qsort(adds, added, sizeof(*adds), S_order);
    markdown_core_event *items = (markdown_core_event *)(adds + added);
    size_t write = 0, add = 0;
    for (size_t at = 0; at <= count; at++) {
        while (add < added && adds[add].slot == at) {
            items[write++] = (markdown_core_event){adds[add].phase, adds[add].token};
            add++;
        }
        if (at < count) {
            items[write++] = events->items[at];
        }
    }
    markdown_core_event *wider = markdown_core_reserve(events->items, &events->room, write, sizeof(*wider));
    if (!wider) {
        events->failed = true;
        return;
    }
    events->items = wider;
    memcpy(events->items, items, write * sizeof(*items));
    events->count = write;
}

/* A reading begins: nothing of the last one is kept. */
static void S_reset(markdown_core_tokenizer *tokenizer, const markdown_core_read *read) {
    tokenizer->data = read->data;
    tokenizer->size = read->size;
    tokenizer->position = read->from;
    tokenizer->cut = read->cut;
    tokenizer->crossed = false;
    tokenizer->undecided = UINT32_MAX;
    tokenizer->stopped = UINT32_MAX;
    tokenizer->passing = UINT32_MAX;
    tokenizer->open_count = 0;
    tokenizer->frame_count = 0;
    tokenizer->all_count = 0;
    tokenizer->mark_count = 0;
    tokenizer->group_count = 0;
    tokenizer->note_count = 0;
    tokenizer->word_count = 0;
    if (read->text) {
        tokenizer->text = *read->text;
    } else {
        tokenizer->text = (markdown_core_constructs){0};
    }
    tokenizer->reader = read->reader;
    tokenizer->records = read->records;
    tokenizer->start = read->start;
    tokenizer->context = read->context;
    tokenizer->cursor = read->cursor;
    tokenizer->base = 0;
    tokenizer->columns = read->columns;
    tokenizer->column = 0;
    tokenizer->counted = read->from == 0;
    tokenizer->split = false;
    tokenizer->ending = UINT32_MAX;
    tokenizer->halts = read->halts;
    tokenizer->halted = false;
    tokenizer->done = false;
    if (read->room) {
        memset(read->records, 0, read->room);
    }
    markdown_core_events_clear(&tokenizer->events);
}

/* Runs the reading from `state` until it is done. */
static void S_run(markdown_core_tokenizer *tokenizer, markdown_core_state state) {
    int code = markdown_core_code_at(tokenizer, 0);
    while (!tokenizer->done && !tokenizer->events.failed) {
        tokenizer->consumed = false;
        const uint32_t at = tokenizer->position;
        const uint32_t column = tokenizer->split ? tokenizer->column : 0;
        const size_t rewound = tokenizer->rewound;
        if (code == MARKDOWN_CORE_EOF) {
            markdown_core_met_end(tokenizer);
        }
        state = state.step(tokenizer, code);
        if (tokenizer->done) {
            break;
        }
        if (state.step == S_ok_step || state.step == S_nok_step) {
            state = S_settle(tokenizer, state, &code);
            continue;
        }
        /* Every state consumes the code it is handed or names an end. */
        /* A column of a tab is read when the place moves into the tab or
         * on in it. */
        assert(tokenizer->consumed && (tokenizer->position > at || (tokenizer->split && tokenizer->column > column) ||
                                       tokenizer->rewound > rewound));
        (void)at;
        (void)column;
        (void)rewound;
        code = markdown_core_code_at(tokenizer, 0);
    }
}

/* Every resolve_all of the reading runs, and the groups go in. */
static bool S_finish(markdown_core_tokenizer *tokenizer) {
    if (tokenizer->events.failed) {
        return false;
    }
    assert(!tokenizer->frame_count && !tokenizer->open_count);
    if (tokenizer->halts && S_waits(tokenizer)) {
        markdown_core_halt(tokenizer);
    }
    if (tokenizer->halted) {
        return true;
    }
    for (int late = 0; late < 2; late++) {
        for (size_t i = 0; i < tokenizer->all_count && !tokenizer->events.failed; i++) {
            /* Once by what it runs: the elements whose constructs share one
             * resolver are rewritten by one pass. */
            const markdown_core_construct *construct = tokenizer->alls[i];
            bool ran = !construct->resolve_all || construct->late != (late != 0);
            for (size_t j = 0; j < i && !ran; j++) {
                ran = tokenizer->alls[j]->resolve_all == construct->resolve_all;
            }
            if (!ran) {
                construct->resolve_all(tokenizer);
            }
        }
    }
    if (tokenizer->group_count && !tokenizer->events.failed) {
        S_merge(tokenizer);
    }
    return !tokenizer->events.failed;
}

bool markdown_core_tokenize(markdown_core_tokenizer *tokenizer, const markdown_core_read *read) {
    S_reset(tokenizer, read);
    S_run(tokenizer, markdown_core_state_of(S_text));
    return S_finish(tokenizer);
}

void markdown_core_tokenizer_begin(markdown_core_tokenizer *tokenizer, const markdown_core_read *read,
                                   markdown_core_state state) {
    S_reset(tokenizer, read);
    tokenizer->resume = state;
}

bool markdown_core_tokenizer_feed(markdown_core_tokenizer *tokenizer, const unsigned char *data, uint32_t size,
                                  uint32_t from) {
    assert(!tokenizer->frame_count && !tokenizer->open_count);
    tokenizer->data = data;
    tokenizer->size = size;
    if (tokenizer->position != from) {
        tokenizer->position = from;
        tokenizer->counted = tokenizer->split = false;
    }
    tokenizer->done = false;
    S_run(tokenizer, tokenizer->resume);
    return !tokenizer->events.failed;
}

markdown_core_state markdown_core_pause(markdown_core_tokenizer *tokenizer, markdown_core_state next) {
    assert(!tokenizer->frame_count);
    tokenizer->resume = next;
    return S_end(tokenizer);
}

bool markdown_core_tokenizer_end(markdown_core_tokenizer *tokenizer) { return S_finish(tokenizer); }

void markdown_core_halt(markdown_core_tokenizer *tokenizer) {
    if (!tokenizer->halted) {
        tokenizer->stopped = tokenizer->position;
        tokenizer->halted = true;
    }
}

bool markdown_core_tokenizer_settled(const markdown_core_tokenizer *tokenizer) {
    if (tokenizer->frame_count || tokenizer->open_count) {
        return false;
    }
    const markdown_core_constructs *text = &tokenizer->text;
    for (size_t i = 0; i < text->holder_count; i++) {
        if (text->holders[i]->holds(tokenizer, text->holders[i]->context)) {
            return false;
        }
    }
    return true;
}

/* THE SPACE: the frame's words are the most columns, the token's kind, the
 * columns read and whether the token is entered. */
enum { SPACE_MOST, SPACE_KIND, SPACE_READ, SPACE_ENTERED };

static markdown_core_state S_space(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (markdown_core_code_space(code) && words[SPACE_READ] < words[SPACE_MOST]) {
        if (words[SPACE_KIND] && !words[SPACE_ENTERED]) {
            markdown_core_enter(tokenizer, (uint16_t)words[SPACE_KIND]);
            words[SPACE_ENTERED] = 1;
        }
        markdown_core_consume(tokenizer);
        words[SPACE_READ]++;
        return markdown_core_state_of(S_space);
    }
    if (words[SPACE_ENTERED]) {
        markdown_core_exit(tokenizer, (uint16_t)words[SPACE_KIND]);
    }
    return markdown_core_ok(tokenizer);
}

static const markdown_core_construct S_SPACE = {.start = S_space};

markdown_core_state markdown_core_spaces(markdown_core_tokenizer *tokenizer, markdown_core_state ok, uint16_t kind,
                                         uint32_t most, int code) {
    if (!S_push_frame(tokenizer, ok, ok, NULL, &S_SPACE, 1, false)) {
        return markdown_core_state_of(S_end_step);
    }
    uint32_t *words = markdown_core_frame_local(tokenizer);
    words[SPACE_MOST] = most;
    words[SPACE_KIND] = kind;
    return S_space(tokenizer, code);
}

static markdown_core_state S_blank_after(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_code_ending(code) ? markdown_core_ok(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_blank_line(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_blank_after), 0, UINT32_MAX, code);
}

const markdown_core_construct markdown_core_blank_line = {.start = S_blank_line};
