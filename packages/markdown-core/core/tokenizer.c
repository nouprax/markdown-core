#include <assert.h>
#include <string.h>

#include "alloc.h"
#include "tokenizer.h"

/* The two ends a state names instead of a next state, and the end of the
 * reading: never stepped. */
static markdown_core_state S_ok_step(markdown_core_tokenizer *tokenizer, int code) {
    (void)tokenizer;
    (void)code;
    assert(false);
    return markdown_core_state_of(S_ok_step);
}

static markdown_core_state S_nok_step(markdown_core_tokenizer *tokenizer, int code) {
    (void)tokenizer;
    (void)code;
    assert(false);
    return markdown_core_state_of(S_nok_step);
}

static markdown_core_state S_end_step(markdown_core_tokenizer *tokenizer, int code) {
    (void)tokenizer;
    (void)code;
    assert(false);
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
    *tokenizer = (markdown_core_tokenizer){0};
}

uint32_t markdown_core_enter(markdown_core_tokenizer *tokenizer, uint16_t kind) {
    markdown_core_events *events = &tokenizer->events;
    const uint32_t token = markdown_core_events_token(events, kind, tokenizer->position);
    uint32_t *opens =
        markdown_core_reserve(tokenizer->opens, &tokenizer->open_room, tokenizer->open_count + 1, sizeof(*opens));
    if (!opens || token == UINT32_MAX) {
        events->failed = true;
        return UINT32_MAX;
    }
    tokenizer->opens = opens;
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

void markdown_core_consume(markdown_core_tokenizer *tokenizer) {
    assert(!tokenizer->consumed && tokenizer->position < tokenizer->size);
    tokenizer->position++;
    tokenizer->consumed = true;
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
                                                             .check = check};
    return true;
}

static markdown_core_state S_begin(markdown_core_tokenizer *tokenizer, markdown_core_state ok, markdown_core_state nok,
                                   const markdown_core_construct *const *list, const markdown_core_construct *one,
                                   size_t count, bool check, int code) {
    assert(count);
    if (!S_push_frame(tokenizer, ok, nok, list, one, count, check)) {
        return markdown_core_state_of(S_end_step);
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
    tokenizer->position = frame->position;
    tokenizer->open_count = frame->opens;
    markdown_core_events_cut(&tokenizer->events, frame->events, frame->tokens);
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
    if (construct->resolve_all || construct->holds) {
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
 * data runs to the next byte a construct may start. */
static markdown_core_state S_text(markdown_core_tokenizer *tokenizer, int code);
static markdown_core_state S_data_more(markdown_core_tokenizer *tokenizer, int code);

static bool S_starts(const markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_constructs *text = &tokenizer->text;
    return code != MARKDOWN_CORE_EOF && text->offsets[code] != text->offsets[code + 1];
}

/* A byte no construct read is data. */
static markdown_core_state S_data(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_enter(tokenizer, MARKDOWN_CORE_TOKEN_DATA);
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_data_more);
}

static markdown_core_state S_data_more(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF || S_starts(tokenizer, code)) {
        markdown_core_exit(tokenizer, MARKDOWN_CORE_TOKEN_DATA);
        return S_text(tokenizer, code);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_data_more);
}

/* After a construct: one that read nothing is no construct, and its byte
 * is data. */
static markdown_core_state S_after(markdown_core_tokenizer *tokenizer, int code) {
    if (tokenizer->position == tokenizer->text_from) {
        return S_data(tokenizer, code);
    }
    return S_text(tokenizer, code);
}

static markdown_core_state S_text(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF) {
        tokenizer->done = true;
        return markdown_core_state_of(S_end_step);
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

/* THE DATA OF THE TEXT (micromark's resolveAllText): touching data tokens
 * are one. */
static void S_join_data(markdown_core_tokenizer *tokenizer) {
    markdown_core_events *events = &tokenizer->events;
    size_t out = 0;
    for (size_t i = 0; i < events->count; i++) {
        const markdown_core_event event = events->items[i];
        if (event.phase == MARKDOWN_CORE_ENTER && out >= 1 &&
            events->tokens[event.token].kind == MARKDOWN_CORE_TOKEN_DATA) {
            const markdown_core_event last = events->items[out - 1];
            markdown_core_token *before = &events->tokens[last.token];
            if (last.phase == MARKDOWN_CORE_EXIT && before->kind == MARKDOWN_CORE_TOKEN_DATA &&
                before->end == events->tokens[event.token].start) {
                /* The data before goes on: its exit is the later one's. */
                const size_t exit = i + 1;
                assert(exit < events->count && events->items[exit].token == event.token);
                before->end = events->tokens[event.token].end;
                i = exit;
                continue;
            }
        }
        events->items[out++] = event;
    }
    events->count = out;
}

bool markdown_core_tokenize_text(markdown_core_tokenizer *tokenizer, const unsigned char *data, uint32_t size,
                                 const markdown_core_constructs *text, void *reader) {
    tokenizer->data = data;
    tokenizer->size = size;
    tokenizer->position = 0;
    tokenizer->open_count = 0;
    tokenizer->frame_count = 0;
    tokenizer->all_count = 0;
    tokenizer->text = *text;
    tokenizer->reader = reader;
    tokenizer->done = false;
    markdown_core_events_clear(&tokenizer->events);
    markdown_core_state state = markdown_core_state_of(S_text);
    int code = markdown_core_code_at(tokenizer, 0);
    while (!tokenizer->done && !tokenizer->events.failed) {
        tokenizer->consumed = false;
        const uint32_t at = tokenizer->position;
        state = state.step(tokenizer, code);
        if (tokenizer->done) {
            break;
        }
        if (state.step == S_ok_step || state.step == S_nok_step) {
            state = S_settle(tokenizer, state, &code);
            continue;
        }
        /* Every state consumes the code it is handed or names an end. */
        assert(tokenizer->consumed && tokenizer->position == at + 1);
        (void)at;
        code = markdown_core_code_at(tokenizer, 0);
    }
    if (tokenizer->events.failed) {
        return false;
    }
    assert(!tokenizer->frame_count && !tokenizer->open_count);
    for (size_t i = 0; i < tokenizer->all_count; i++) {
        if (tokenizer->alls[i]->resolve_all) {
            tokenizer->alls[i]->resolve_all(tokenizer);
        }
    }
    S_join_data(tokenizer);
    return !tokenizer->events.failed;
}

bool markdown_core_tokenizer_settled(const markdown_core_tokenizer *tokenizer) {
    if (tokenizer->frame_count || tokenizer->open_count) {
        return false;
    }
    for (size_t i = 0; i < tokenizer->all_count; i++) {
        if (tokenizer->alls[i]->holds && tokenizer->alls[i]->holds(tokenizer)) {
            return false;
        }
    }
    return true;
}
