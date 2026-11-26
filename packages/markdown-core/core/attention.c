#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "attention.h"
#include "delimiter.h"
#include "element.h"
#include "inline_internal.h"
#include "markdown_core_ctype.h"
#include "node.h"
#include "parser.h"
#include "utf8.h"

static const delimiter_rule_spec *S_spec(const markdown_core_element_instance *self) {
    return &self->element->inlines->delimiter;
}

/* THE RUN at `start` of the rule of `self`: where it ends and what it may
 * do. A run reaches no further than its rule's limit. */
typedef struct S_run {
    uint32_t end;
    bool opens, closes;
} S_run;

static S_run S_scan(markdown_core_tokenizer *tokenizer, const markdown_core_element_instance *self, uint32_t start) {
    const delimiter_rule_spec *spec = S_spec(self);
    const unsigned char *data = tokenizer->data;
    const uint32_t size = tokenizer->size;
    const unsigned char c = data[start];
    S_run run = {start, false, false};
    while (run.end < size && data[run.end] == c && (!spec->run_limit || run.end - start < (uint32_t)spec->run_limit)) {
        run.end++;
    }
    if (run.end == size) {
        markdown_core_met_end(tokenizer);
    }
    if (spec->body == DELIMITER_WORD_BODY) {
        run.opens = run.closes = true;
        return run;
    }
    const uint32_t length = run.end - start;
    if (length < (uint32_t)spec->minimum_width || (spec->exact_run && length != (uint32_t)spec->minimum_width)) {
        return run;
    }
    /* The start and the end of the content read as a newline, and so does
     * a run of skip characters that reaches either. Skip characters are
     * ASCII (the dialect refuses an element that declares any other), so a
     * decoded scalar below 0x80 is its own byte. */
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const int8_t *skip = inline_state->dialect->skip_chars;
    int32_t before_char = 10, after_char = 10;
    if (start > 0) {
        uint32_t at = start - 1;
        /* Back over skip characters and then over the continuation bytes of
         * the character before them; a continuation byte follows only a lead
         * or another, never a skip character, so the walk also stops at a
         * skip character a continuation byte follows. */
        while ((data[at] >> 6 == 2 || (skip[data[at]] && data[at + 1] >> 6 != 2)) && at > 0) {
            at--;
        }
        markdown_core_utf8proc_decode(data + at, (bufsize_t)(start - at), &before_char);
        if (before_char < 0x80 && skip[before_char]) {
            before_char = 10;
        }
    }
    uint32_t after = run.end;
    while (after < size && skip[data[after]]) {
        after++;
    }
    if (after < size) {
        markdown_core_utf8proc_decode(data + after, (bufsize_t)(size - after), &after_char);
    } else {
        markdown_core_met_end(tokenizer);
    }
    const uint8_t b = markdown_core_utf8proc_classes(before_char);
    const uint8_t a = markdown_core_utf8proc_classes(after_char);
    const markdown_core_flank flank = {b & MARKDOWN_CORE_UNICODE_WHITESPACE, a & MARKDOWN_CORE_UNICODE_WHITESPACE,
                                       b & MARKDOWN_CORE_UNICODE_PUNCTUATION_OR_SYMBOL,
                                       a & MARKDOWN_CORE_UNICODE_PUNCTUATION_OR_SYMBOL};
    const unsigned sides = spec->flank ? spec->flank(self, flank) : 0u;
    run.opens = (sides & MARKDOWN_CORE_FLANK_OPENS) != 0;
    run.closes = (sides & MARKDOWN_CORE_FLANK_CLOSES) != 0;
    return run;
}

/* Whether a run may pair: it opens, or it closes and a run of its rule
 * before it opened. */
static bool S_pairs(const markdown_core_tokenizer *tokenizer, const markdown_core_element_instance *self, S_run run) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    return run.opens || (run.closes && inline_state->delim_openers[self->element->inlines->rule] > 0);
}

markdown_core_state markdown_core_attention_start(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const delimiter_rule_spec *spec = S_spec(self);
    const uint32_t start = tokenizer->position;
    const S_run run = S_scan(tokenizer, self, start);
    const uint32_t length = run.end - start;
    if (spec->exact_run && length < (uint32_t)spec->minimum_width) {
        return markdown_core_nok(tokenizer);
    }
    unsigned kind = MARKDOWN_CORE_ATTENTION_SEQUENCE;
    if (!S_pairs(tokenizer, self, run)) {
        /* A run that cannot pair is text; one of an exact width is read
         * whole, whatever its width. */
        if (!spec->exact_run) {
            return markdown_core_nok(tokenizer);
        }
        kind = MARKDOWN_CORE_ATTENTION_LITERAL;
    }
    const uint32_t token = markdown_core_enter(tokenizer, (uint16_t)(self->token_base + kind));
    markdown_core_consume_to(tokenizer, run.end);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + kind));
    if (kind == MARKDOWN_CORE_ATTENTION_SEQUENCE && token != UINT32_MAX) {
        tokenizer->events.tokens[token].data =
            markdown_core_attention_word(run.opens, run.closes, (unsigned)self->element->inlines->rule, length);
        markdown_core_attention_count(tokenizer, self->element->inlines->rule, run.opens, run.closes);
    }
    return markdown_core_ok(tokenizer);
}

uint32_t markdown_core_attention_passes(markdown_core_tokenizer *tokenizer, const void *context, uint32_t at) {
    const markdown_core_element_instance *self = context;
    const markdown_core_inline_ops *ops = self->element->inlines;
    /* Only a rule whose byte is its own lends its runs to the text. */
    if (!ops->character || ops->delimiter.exact_run || tokenizer->data[at] != ops->character) {
        return at;
    }
    const S_run run = S_scan(tokenizer, self, at);
    return S_pairs(tokenizer, self, run) ? at : run.end;
}

/* THE PAIRING. A MARKER is a sequence as the pass holds it: its token and
 * enter event, its rule, what it may do, its length as read, where it ended,
 * and the last boundary before it; the markers still on the stack are a
 * list in their order. */
typedef struct S_marker {
    const markdown_core_element_instance *owner;
    uint32_t token, enter;
    uint32_t position, length, word, wall;
    int32_t prev, next;
    unsigned rule;
    bool opens, closes;
} S_marker;

/* A pass pairs the markers of the events it reads; what it makes is held
 * by a node of kind `holder`, or by the reading's owner for NONE. */
typedef struct S_pass {
    markdown_core_tokenizer *tokenizer;
    markdown_core_inline_state *inline_state;
    markdown_core_node_type holder;
    S_marker *markers;
    size_t count;
    int32_t first;
    /* The last boundary of the events read, 0 for none. */
    uint32_t word;
} S_pass;

bool markdown_core_attention_allows(const markdown_core_tokenizer *tokenizer, markdown_core_node_type holder,
                                    markdown_core_node_type kind) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    if (holder != MARKDOWN_CORE_NODE_NONE) {
        return markdown_core_kind_can_contain(holder, kind);
    }
    return inline_state->owner && markdown_core_node_can_contain_type(inline_state->owner, kind);
}

static const markdown_core_element_instance *S_sequence_of(const markdown_core_dialect *dialect, uint16_t kind) {
    if (kind >= dialect->token_kinds || !dialect->compiles[kind]) {
        return NULL;
    }
    const markdown_core_compile_ops *ops = dialect->compiles[kind];
    const markdown_core_element_instance *self = ops->context;
    if (!ops->sequence || kind != self->token_base + MARKDOWN_CORE_ATTENTION_SEQUENCE) {
        return NULL;
    }
    return self;
}

static void S_unlink(S_pass *pass, int32_t at) {
    S_marker *marker = &pass->markers[at];
    if (marker->prev >= 0) {
        pass->markers[marker->prev].next = marker->next;
    } else {
        pass->first = marker->next;
    }
    if (marker->next >= 0) {
        pass->markers[marker->next].prev = marker->prev;
    }
    marker->prev = marker->next = -2;
}

static uint32_t S_left(const S_pass *pass, int32_t at) {
    const markdown_core_token *token = &pass->tokenizer->events.tokens[pass->markers[at].token];
    return token->end - token->start;
}

/* The pair of `opener` and `closer` uses `used` bytes of each for a node of
 * `kind`: the marker after the closer is the next asked, or the closer
 * while enough of it is left. */
static int32_t S_insert(S_pass *pass, int32_t opener, int32_t closer, uint32_t used, markdown_core_node_type kind) {
    S_marker *o = &pass->markers[opener], *c = &pass->markers[closer];
    const int32_t next = c->next;
    const uint32_t minimum = (uint32_t)S_spec(c->owner)->minimum_width;
    /* A rejected container leaves its authored text intact. */
    if (!markdown_core_attention_allows(pass->tokenizer, pass->holder, kind)) {
        S_unlink(pass, opener);
        S_unlink(pass, closer);
        return next;
    }
    markdown_core_events *events = &pass->tokenizer->events;
    markdown_core_token *open = &events->tokens[o->token];
    const uint32_t start = open->end - used;
    const uint32_t end = events->tokens[c->token].start + used;
    const uint32_t group =
        markdown_core_events_token(events, (uint16_t)(c->owner->token_base + MARKDOWN_CORE_ATTENTION_GROUP), start);
    if (group == UINT32_MAX) {
        events->failed = true;
        return -1;
    }
    events->tokens[group].end = end;
    events->tokens[group].data = (uint32_t)kind;
    events->tokens[o->token].end -= used;
    events->tokens[c->token].start += used;
    if (!markdown_core_group_add(pass->tokenizer, group, o->enter + 2, c->enter)) {
        return -1;
    }
    if (S_left(pass, opener) < minimum) {
        S_unlink(pass, opener);
    }
    if (S_left(pass, closer) < minimum) {
        S_unlink(pass, closer);
        return next;
    }
    return closer;
}

/* The markers from after `opener` up to `closer` are text. An opener of a
 * rule that covers was counted at every later run of its rule (its runs
 * alternate): a reading from past it, without it, need not agree. */
static void S_reduce(S_pass *pass, int32_t opener, int32_t closer) {
    int32_t at = pass->markers[opener].next;
    while (at != closer) {
        const S_marker *marker = &pass->markers[at];
        const int32_t next = marker->next;
        if (marker->opens && marker->owner->element->inlines->covers) {
            markdown_core_undecide(pass->tokenizer, pass->tokenizer->events.tokens[marker->token].start);
        }
        S_unlink(pass, at);
        at = next;
    }
}

/* The pair of `opener` and `closer` of a rule that covers: one token of
 * the element's kind over both and what is between, which compile makes
 * whole. The marker after the closer is the next asked. */
static int32_t S_cover_pair(S_pass *pass, int32_t opener, int32_t closer) {
    const S_marker *o = &pass->markers[opener], *c = &pass->markers[closer];
    const int32_t next = c->next;
    const markdown_core_inline_ops *ops = c->owner->element->inlines;
    markdown_core_events *events = &pass->tokenizer->events;
    uint32_t data = 0;
    const markdown_core_token open = events->tokens[o->token], close = events->tokens[c->token];
    if (ops->covers(c->owner, pass->tokenizer, pass->holder, &open, &close, &data)) {
        const uint32_t token =
            markdown_core_events_token(events, (uint16_t)(c->owner->token_base + ops->cover), open.start);
        if (token == UINT32_MAX || !markdown_core_group_add(pass->tokenizer, token, o->enter, c->enter + 2)) {
            events->failed = true;
            return -1;
        }
        events->tokens[token].end = close.end;
        events->tokens[token].data = data;
    } else {
        /* The pair makes nothing, but the runs between are gone all the
         * same: no reading from between may settle. */
        markdown_core_undecide(pass->tokenizer, open.start);
    }
    S_unlink(pass, opener);
    S_unlink(pass, closer);
    return next;
}

/* CommonMark's process_emphasis, over the markers. */
static void S_process(S_pass *pass) {
    uint32_t bottoms[3][MARKDOWN_CORE_DELIM_RULE_COUNT] = {{0}};
    int32_t closer = pass->first;
    while (closer >= 0) {
        S_marker *c = &pass->markers[closer];
        if (!c->closes) {
            closer = c->next;
            continue;
        }
        const markdown_core_inline_ops *ops = c->owner->element->inlines;
        const delimiter_rule_spec *spec = &ops->delimiter;
        uint32_t bottom = bottoms[c->length % 3][c->rule];
        if (spec->body == DELIMITER_WORD_BODY && bottom < c->word) {
            bottom = c->word;
        }
        if (bottom < c->wall) {
            bottom = c->wall;
        }
        int32_t opener = c->prev;
        bool found = false;
        while (opener >= 0 && pass->markers[opener].position >= bottom) {
            const S_marker *o = &pass->markers[opener];
            /* An interior closer of size 2 matches no opener of size 1, nor
             * one of size 1 an opener of size 2. */
            if (o->opens && o->owner == c->owner && o->rule == c->rule &&
                (!spec->rule_of_three || !(c->opens || o->closes) || c->length % 3 == 0 ||
                 (o->length + c->length) % 3 != 0)) {
                found = true;
                break;
            }
            opener = o->prev;
        }
        const int32_t old = closer;
        if (found) {
            S_reduce(pass, opener, closer);
            if (spec->minimum_width) {
                uint32_t used = (uint32_t)spec->maximum_width;
                if (S_left(pass, opener) < used || S_left(pass, closer) < used) {
                    used = (uint32_t)spec->minimum_width;
                }
                closer = S_insert(pass, opener, closer, used, used == 2 ? spec->double_kind : spec->single_kind);
                if (pass->tokenizer->events.failed) {
                    return;
                }
            } else if (ops->covers) {
                closer = S_cover_pair(pass, opener, closer);
                if (pass->tokenizer->events.failed) {
                    return;
                }
            } else {
                closer = c->next;
            }
        } else {
            closer = c->next;
            bottoms[c->length % 3][c->rule] = c->position;
            if (!c->opens) {
                S_unlink(pass, old);
            }
        }
    }
}

/* The last boundary of the text, `*word`, as event `at`, an enter, leaves
 * it: past whitespace a data token holds, past a token whose kind is a
 * boundary, or the one a region resolved before keeps (its end MARKER's),
 * which is stepped over. The event to go on from. */
static size_t S_word(const markdown_core_tokenizer *tokenizer, const markdown_core_dialect *dialect, size_t at,
                     uint32_t *word) {
    const markdown_core_events *events = &tokenizer->events;
    const markdown_core_token *token = markdown_core_event_token(events, at);
    if (token->kind == MARKDOWN_CORE_TOKEN_MARKER) {
        if (token->data > at + 1) {
            /* What was resolved keeps its last boundary, which its end's
             * MARKER holds: a word body pairs across none. */
            at = token->data - 1;
            const markdown_core_token *last = markdown_core_event_token(events, at);
            if (last->kind == MARKDOWN_CORE_TOKEN_MARKER && last->data) {
                *word = last->data;
            }
        }
    } else if (token->kind == MARKDOWN_CORE_TOKEN_DATA) {
        for (uint32_t end = token->end; end > token->start; end--) {
            if (markdown_core_is_whitespace(tokenizer->data[end - 1])) {
                *word = end;
                break;
            }
        }
    } else if (token->kind < dialect->token_kinds && dialect->compiles[token->kind] &&
               dialect->compiles[token->kind]->boundary) {
        *word = token->end;
    }
    return at;
}

bool markdown_core_attention_spaced(const markdown_core_tokenizer *tokenizer) {
    /* Read after the groups went in (tokenizer.c, S_merge), whose events
     * moved the ones a MARKER names: every event is walked, a resolved
     * body's own, whose last boundary its end's MARKER holds, with it. */
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_events *events = &tokenizer->events;
    for (size_t at = 0; at < events->count; at++) {
        if (events->items[at].phase != MARKDOWN_CORE_ENTER ||
            markdown_core_event_token(events, at)->kind == MARKDOWN_CORE_TOKEN_MARKER) {
            continue;
        }
        uint32_t word = 0;
        S_word(tokenizer, inline_state->dialect, at, &word);
        if (word) {
            return true;
        }
    }
    return false;
}

/* The markers of the events from `from` on, each with the last boundary
 * before it; what a resolver resolved before (markdown_core_label_past)
 * is stepped over. False when storage could not be had. */
static bool S_collect(S_pass *pass, size_t from, size_t to) {
    markdown_core_tokenizer *tokenizer = pass->tokenizer;
    const markdown_core_dialect *dialect = pass->inline_state->dialect;
    markdown_core_events *events = &tokenizer->events;
    size_t count = 0;
    for (size_t at = from; at < to; at++) {
        if (events->items[at].phase != MARKDOWN_CORE_ENTER) {
            continue;
        }
        const markdown_core_token *token = markdown_core_event_token(events, at);
        if (token->kind == MARKDOWN_CORE_TOKEN_MARKER && token->data > at + 1) {
            /* What was resolved holds no marker of this pass, as below
             * (S_word): its events are not counted again, or nested
             * bodies are read once per depth. */
            at = token->data - 1;
        } else if (S_sequence_of(dialect, token->kind)) {
            count++;
        }
    }
    pass->first = count ? 0 : -1;
    pass->markers = markdown_core_alloc(count ? count : 1, sizeof(*pass->markers));
    if (!pass->markers) {
        events->failed = true;
        return false;
    }
    uint32_t word = 0, wall = 0;
    for (size_t at = from; at < to; at++) {
        if (events->items[at].phase != MARKDOWN_CORE_ENTER) {
            continue;
        }
        const markdown_core_token *token = markdown_core_event_token(events, at);
        if (token->kind < dialect->token_kinds && dialect->compiles[token->kind] &&
            dialect->compiles[token->kind]->wall) {
            wall = token->end;
            continue;
        }
        const markdown_core_element_instance *owner = S_sequence_of(dialect, token->kind);
        if (owner) {
            const int32_t index = (int32_t)pass->count++;
            pass->markers[index] = (S_marker){.owner = owner,
                                              .token = events->items[at].token,
                                              .enter = (uint32_t)at,
                                              .position = token->end,
                                              .length = markdown_core_attention_length(token->data),
                                              .rule = markdown_core_attention_rule(token->data),
                                              .word = word,
                                              .wall = wall,
                                              .prev = index - 1,
                                              .next = -1,
                                              .opens = (token->data & MARKDOWN_CORE_ATTENTION_OPENS) != 0,
                                              .closes = (token->data & MARKDOWN_CORE_ATTENTION_CLOSES) != 0};
            if (index > 0) {
                pass->markers[index - 1].next = index;
            }
        } else {
            at = S_word(tokenizer, dialect, at, &word);
        }
    }
    if (!pass->count) {
        pass->first = -1;
    }
    pass->word = word;
    return true;
}

uint32_t markdown_core_attention_body(markdown_core_tokenizer *tokenizer, size_t from, size_t to,
                                      markdown_core_node_type holder) {
    S_pass pass = {.tokenizer = tokenizer, .inline_state = tokenizer->reader, .holder = holder};
    if (S_collect(&pass, from, to)) {
        S_process(&pass);
        /* Every run of the body is gone from the stack, paired or not; an
         * opener of a rule that covers none paired with was counted at
         * every later run of its rule (S_reduce). */
        markdown_core_inline_state *inline_state = pass.inline_state;
        for (size_t at = 0; at < pass.count; at++) {
            const S_marker *marker = &pass.markers[at];
            inline_state->delim_openers[marker->rule] -= marker->opens;
            inline_state->delim_closers[marker->rule] -= marker->closes;
        }
        for (int32_t at = pass.first; at >= 0 && !tokenizer->events.failed; at = pass.markers[at].next) {
            const S_marker *marker = &pass.markers[at];
            if (marker->opens && marker->owner->element->inlines->covers) {
                markdown_core_undecide(tokenizer, tokenizer->events.tokens[marker->token].start);
            }
        }
    }
    markdown_core_free(pass.markers);
    return pass.word;
}

void markdown_core_attention_resolve(markdown_core_tokenizer *tokenizer) {
    S_pass pass = {.tokenizer = tokenizer, .inline_state = tokenizer->reader, .holder = MARKDOWN_CORE_NODE_NONE};
    if (S_collect(&pass, 0, tokenizer->events.count)) {
        S_process(&pass);
    }
    if (!tokenizer->events.failed) {
        /* An opener none paired with may pair with a closer the content
         * goes on with. */
        for (int32_t at = pass.first; at >= 0; at = pass.markers[at].next) {
            if (pass.markers[at].opens) {
                markdown_core_undecide(tokenizer, tokenizer->events.tokens[pass.markers[at].token].start);
            }
        }
    }
    markdown_core_free(pass.markers);
}

/* COMPILE: a run left as it was read, or what is left of one, is its text;
 * a pair is the node of its kind over what the group holds. */
void markdown_core_attention_text(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    if (token->end > token->start) {
        markdown_core_compile_text(compiler, (bufsize_t)token->start, (bufsize_t)token->end);
    }
}

void markdown_core_attention_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_node *node = markdown_core_inline_make_simple(inline_state, (markdown_core_node_type)token->data);
    if (!node) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    node->element = self->element;
    markdown_core_inline_state_place(inline_state, node, (int)token->start, (int)token->start);
    /* The pass asked the reading's owner; what a group holds goes in. */
    markdown_core_scaffold_append(markdown_core_compile_parent(compiler), node);
    if (!markdown_core_compile_push(compiler, node)) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
}

void markdown_core_attention_close(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_node *node = markdown_core_compile_pop(compiler);
    const uint32_t start = node->place.start;
    markdown_core_inline_state_place(inline_state, node, (int)token->end - 1, (int)token->end - 1);
    node->place.start = start;
}

const markdown_core_compile_ops MARKDOWN_CORE_ATTENTION_TOKENS[MARKDOWN_CORE_ATTENTION_KINDS] = {
    MARKDOWN_CORE_ATTENTION_OPS};
