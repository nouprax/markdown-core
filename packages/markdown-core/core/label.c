#include "label.h"

#include "attention.h"
#include "dialect.h"
#include "element.h"
#include "inline_internal.h"

/* The tag of an open label's mark (tokenizer.h, MARKS). */
enum { S_TAG = 1 };

static const markdown_core_opener *S_label(const markdown_core_tokenizer *tokenizer, size_t mark) {
    const markdown_core_token *token = &tokenizer->events.tokens[tokenizer->marks[mark].token];
    return (const markdown_core_opener *)(const void *)(tokenizer->events.kept + token->data);
}

static const markdown_core_opener *S_last(const markdown_core_tokenizer *tokenizer) {
    const size_t mark = markdown_core_mark_find(tokenizer, S_TAG);
    return mark == SIZE_MAX ? NULL : S_label(tokenizer, mark);
}

markdown_core_opener *markdown_core_opener_last(markdown_core_tokenizer *tokenizer) {
    return (markdown_core_opener *)S_last(tokenizer);
}

markdown_core_opener *markdown_core_opener_under(markdown_core_tokenizer *tokenizer, size_t *at) {
    while (*at) {
        if (tokenizer->marks[--*at].tag == S_TAG) {
            return (markdown_core_opener *)S_label(tokenizer, *at);
        }
    }
    return NULL;
}

markdown_core_state markdown_core_label_open(markdown_core_tokenizer *tokenizer,
                                             const markdown_core_label_shape *shape) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    const uint32_t start = tokenizer->position;
    markdown_core_opener label = {.owner = markdown_core_self(tokenizer),
                                  .shape = shape,
                                  .start = start,
                                  .position = start + shape->width,
                                  .enter = (uint32_t)tokenizer->events.count,
                                  .close = UINT32_MAX,
                                  .outer = inline_state->no_link_openers};
    markdown_core_opener *below = markdown_core_opener_last(tokenizer);
    if (below) {
        below->after = true;
        if (!shape->own) {
            label.image0 = below->image0;
            label.image1 = below->image1;
        }
    }
    if (shape->mark == 1) {
        label.image1 = true;
    } else if (shape->mark == 0) {
        label.image0 = true;
    }
    label.token = markdown_core_enter(tokenizer, MARKDOWN_CORE_TOKEN_LABEL);
    markdown_core_consume_to(tokenizer, label.position);
    markdown_core_exit(tokenizer, MARKDOWN_CORE_TOKEN_LABEL);
    const uint32_t kept = markdown_core_events_keep(&tokenizer->events, &label, sizeof(label));
    if (label.token != UINT32_MAX && kept != UINT32_MAX) {
        tokenizer->events.tokens[label.token].data = kept;
        markdown_core_mark_push(tokenizer, S_TAG, label.token);
    }
    if (!shape->keeps) {
        inline_state->no_link_openers = false;
    }
    return markdown_core_ok(tokenizer);
}

bool markdown_core_label_holds(const markdown_core_tokenizer *tokenizer, const void *context) {
    (void)context;
    return markdown_core_mark_find(tokenizer, S_TAG) != SIZE_MAX;
}

/* What a rule made of `label`, whose end token `end` the reading just
 * exited: the body's attention pairs, the opener is a MARKER over the bytes
 * it keeps, the end nothing, and the group goes over them all. */
static void S_make(markdown_core_tokenizer *tokenizer, markdown_core_opener *label, uint32_t end,
                   const markdown_core_label_made *made) {
    markdown_core_events *events = &tokenizer->events;
    const uint32_t token = label->token, enter = label->enter, start = label->start;
    /* Past the end's exit. */
    const uint32_t to = label->close + 2;
    if (made->kind == MARKDOWN_CORE_TOKEN_NONE) {
        return;
    }
    const uint32_t word = markdown_core_attention_body(tokenizer, enter + 2, label->close, made->holder);
    if (events->failed) {
        return;
    }
    markdown_core_token *opener = &events->tokens[token];
    opener->kind = MARKDOWN_CORE_TOKEN_MARKER;
    opener->end = made->from > start ? made->from : start;
    opener->data = to;
    markdown_core_token *closer = &events->tokens[end];
    closer->kind = MARKDOWN_CORE_TOKEN_MARKER;
    closer->start = closer->end;
    closer->data = word;
    const uint32_t group = markdown_core_events_token(events, made->kind, made->from);
    if (group == UINT32_MAX) {
        events->failed = true;
        return;
    }
    events->tokens[group].end = made->past;
    events->tokens[group].data = made->data;
    markdown_core_group_add(tokenizer, group,
                            made->before != UINT32_MAX ? made->before
                            : made->from > start       ? enter + 2
                                                       : enter,
                            to);
}

void markdown_core_label_decide(markdown_core_tokenizer *tokenizer, markdown_core_opener *label) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_dialect *dialect = inline_state->dialect;
    /* Each is told with the label where it is kept, wherever an earlier
     * one left that. */
    const uint32_t offset = tokenizer->events.tokens[label->token].data;
    for (size_t i = 0; i < dialect->element_count && !tokenizer->events.failed; i++) {
        const markdown_core_element_instance *self = &dialect->instances[i];
        const markdown_core_inline_ops *inlines = self->element->inlines;
        if (inlines && inlines->decided) {
            inlines->decided(self, tokenizer, markdown_core_opener_at(tokenizer, offset));
        }
    }
}

/* The range of rules `label` meets: a label whose element closes its own
 * by `mine` rules meets those alone (element.h, markdown_core_close_rule);
 * any other, the shared rules (dialect.h, `closers`). */
static size_t S_first(const markdown_core_dialect *dialect, const markdown_core_opener *label, size_t *last) {
    const markdown_core_element_instance *owner = label->owner;
    const size_t first = owner->mine_count ? owner->mine_first : 0;
    *last = first + (owner->mine_count ? owner->mine_count : dialect->shared);
    return first;
}

/* RULE `at` of the dialect asked of `label`, kept at `offset`, whose
 * closer stands before `after`: what it answers, what it made in `*made`. */
static markdown_core_bracket_match S_ask(markdown_core_tokenizer *tokenizer, const markdown_core_closer *closer,
                                         uint32_t offset, uint32_t after, markdown_core_label_made *made) {
    markdown_core_opener *label = markdown_core_opener_at(tokenizer, offset);
    *made = (markdown_core_label_made){.from = label->start, .past = after, .before = UINT32_MAX};
    return closer->rule->end(closer->owner, tokenizer, label, made);
}

/* A label's rules decided, MATCHED or not: a body no rule decides itself
 * is decided. */
static void S_decided(markdown_core_tokenizer *tokenizer, uint32_t offset, bool matched,
                      const markdown_core_label_made *made) {
    if (!matched || !made->keeps) {
        markdown_core_label_decide(tokenizer, markdown_core_opener_at(tokenizer, offset));
    }
}

/* THE RULES FOR `label`, kept at `offset`, as they are asked again, with
 * what their tails kept as they last read (label.h, markdown_core_label_again). */
static bool S_rules(markdown_core_tokenizer *tokenizer, uint32_t offset, markdown_core_label_made *made) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_dialect *dialect = inline_state->dialect;
    const int code = tokenizer->data[tokenizer->position];
    const uint32_t after = tokenizer->position + 1;
    size_t last = 0;
    bool matched = false;
    for (size_t at = S_first(dialect, markdown_core_opener_at(tokenizer, offset), &last); at < last; at++) {
        const markdown_core_closer *closer = &dialect->closers[at];
        if (closer->byte != code || !closer->rule->end) {
            continue;
        }
        const markdown_core_bracket_match match = S_ask(tokenizer, closer, offset, after, made);
        if (match != BRACKET_UNMATCHED) {
            matched = match == BRACKET_MATCHED;
            break;
        }
    }
    S_decided(tokenizer, offset, matched, made);
    return matched;
}

void markdown_core_label_again(markdown_core_tokenizer *tokenizer, uint32_t offset) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    markdown_core_opener *label = markdown_core_opener_at(tokenizer, offset);
    const uint32_t end = tokenizer->events.items[label->close].token;
    /* The rules read where the end was read, with the rule about links
     * as it was there; what they leave of it holds as well as the rule
     * as it is now. */
    const uint32_t position = tokenizer->position;
    const bool links = inline_state->no_link_openers;
    tokenizer->position = tokenizer->events.tokens[end].start;
    inline_state->no_link_openers = label->ended;
    markdown_core_label_made made = {0};
    const bool matched = S_rules(tokenizer, offset, &made);
    tokenizer->position = position;
    label = markdown_core_opener_at(tokenizer, offset);
    if (matched && made.links) {
        inline_state->no_link_openers = made.links > 0 || label->outer;
    }
    inline_state->no_link_openers |= links;
    if (matched && !tokenizer->events.failed) {
        S_make(tokenizer, label, end, &made);
    }
}

/* THE END of the innermost label at a closer byte, code by code
 * (micromark's labelEnd): the closer read, each rule the label meets in
 * turn, its tail checked first when it has one, is asked until one makes
 * its construct or none is left; the end then spans what the rule read
 * past the closer. The frame's words: the label's mark and where it is
 * kept, the end's token, the closer, where it ends, and the rule asked. */
enum { END_MARK, END_OFFSET, END_TOKEN, END_CODE, END_AFTER, END_RULE };

static markdown_core_state S_rule_next(markdown_core_tokenizer *tokenizer, int code);

/* The end, MATCHED by what `made` says or not, spanning what was read. */
static markdown_core_state S_end_done(markdown_core_tokenizer *tokenizer, bool matched,
                                      const markdown_core_label_made *made) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    const uint32_t *words = markdown_core_frame_local(tokenizer);
    const uint32_t offset = words[END_OFFSET], end = words[END_TOKEN], after = words[END_AFTER];
    if (offset != UINT32_MAX) {
        S_decided(tokenizer, offset, matched, made);
        markdown_core_mark_cut(tokenizer, words[END_MARK]);
        if (matched && made->links) {
            inline_state->no_link_openers = made->links > 0 || markdown_core_opener_at(tokenizer, offset)->outer;
        }
    }
    if (matched && made->past > after) {
        markdown_core_consume_to(tokenizer, made->past);
    }
    markdown_core_exit(tokenizer, MARKDOWN_CORE_TOKEN_LABEL_END);
    if (matched && end != UINT32_MAX) {
        S_make(tokenizer, markdown_core_opener_at(tokenizer, offset), end, made);
        if (made->follows != MARKDOWN_CORE_NODE_NONE) {
            markdown_core_text_made(tokenizer, made->follows);
        }
    }
    return markdown_core_ok(tokenizer);
}

/* The rule at END_RULE asked, its tail read. */
static markdown_core_state S_rule_ask(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    uint32_t *words = markdown_core_frame_local(tokenizer);
    const markdown_core_closer *closer = &inline_state->dialect->closers[words[END_RULE]];
    markdown_core_label_made made;
    const markdown_core_bracket_match match = S_ask(tokenizer, closer, words[END_OFFSET], words[END_AFTER], &made);
    if (match != BRACKET_UNMATCHED) {
        return S_end_done(tokenizer, match == BRACKET_MATCHED, &made);
    }
    words[END_RULE]++;
    return S_rule_next(tokenizer, code);
}

static markdown_core_state S_rule_next(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_dialect *dialect = ((const markdown_core_inline_state *)tokenizer->reader)->dialect;
    uint32_t *words = markdown_core_frame_local(tokenizer);
    size_t last = 0;
    (void)S_first(dialect, markdown_core_opener_at(tokenizer, words[END_OFFSET]), &last);
    for (size_t at = words[END_RULE]; at < last; at++) {
        const markdown_core_closer *closer = &dialect->closers[at];
        if (closer->byte != words[END_CODE] || !closer->rule->end) {
            continue;
        }
        words[END_RULE] = (uint32_t)at;
        if (closer->tail.start) {
            return markdown_core_check(tokenizer, &closer->tail, markdown_core_state_of(S_rule_ask),
                                       markdown_core_state_of(S_rule_ask), code);
        }
        return S_rule_ask(tokenizer, code);
    }
    return S_end_done(tokenizer, false, &(markdown_core_label_made){0});
}

static markdown_core_state S_end(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    const size_t mark = markdown_core_mark_find(tokenizer, S_TAG);
    const uint32_t enter = (uint32_t)tokenizer->events.count;
    uint32_t *words = markdown_core_frame_local(tokenizer);
    words[END_TOKEN] = markdown_core_enter(tokenizer, MARKDOWN_CORE_TOKEN_LABEL_END);
    words[END_OFFSET] = UINT32_MAX;
    words[END_CODE] = (uint32_t)code;
    words[END_AFTER] = tokenizer->position + 1;
    markdown_core_opener *label =
        mark == SIZE_MAX
            ? NULL
            : markdown_core_opener_at(tokenizer, tokenizer->events.tokens[tokenizer->marks[mark].token].data);
    markdown_core_consume(tokenizer);
    if (!label || label->shape->closer != code) {
        return S_end_done(tokenizer, false, NULL);
    }
    /* The body ends where the end begins. */
    label->close = enter;
    label->ended = inline_state->no_link_openers;
    words[END_MARK] = (uint32_t)mark;
    words[END_OFFSET] = tokenizer->events.tokens[tokenizer->marks[mark].token].data;
    words[END_RULE] = (uint32_t)S_first(inline_state->dialect, label, &(size_t){0});
    return markdown_core_state_of(S_rule_next);
}

const markdown_core_construct MARKDOWN_CORE_LABEL_END = {.start = S_end};

void markdown_core_label_resolve(markdown_core_tokenizer *tokenizer) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_dialect *dialect = inline_state->dialect;
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element_instance *self = &dialect->instances[i];
        const markdown_core_inline_ops *inlines = self->element->inlines;
        if (!inlines || !inlines->left) {
            continue;
        }
        for (size_t at = tokenizer->mark_count; at--;) {
            if (tokenizer->marks[at].tag == S_TAG) {
                inlines->left(self, tokenizer, (markdown_core_opener *)S_label(tokenizer, at));
            }
        }
    }
}

size_t markdown_core_label_step(markdown_core_tokenizer *tokenizer, size_t at, size_t to) {
    const markdown_core_events *events = &tokenizer->events;
    if (at >= to) {
        return to;
    }
    if (events->items[at].phase == MARKDOWN_CORE_ENTER) {
        const markdown_core_token *token = markdown_core_event_token(events, at);
        size_t past = at + 1;
        if (token->kind == MARKDOWN_CORE_TOKEN_MARKER && token->data > at) {
            past = token->data;
        } else if (token->kind == MARKDOWN_CORE_TOKEN_LABEL) {
            const markdown_core_opener *label = markdown_core_opener_at(tokenizer, token->data);
            past = label->close == UINT32_MAX ? to : (size_t)label->close + 2;
        }
        return past < to ? past : to;
    }
    return at + 1;
}

bool markdown_core_label_in(const markdown_core_tokenizer *tokenizer, int image) {
    const markdown_core_opener *label = S_last(tokenizer);
    return label && (image ? label->image1 : label->image0);
}

unsigned char markdown_core_label_closer(const markdown_core_tokenizer *tokenizer) {
    const markdown_core_opener *label = S_last(tokenizer);
    return label ? label->shape->closer : 0;
}

uint32_t markdown_core_label_body(const markdown_core_tokenizer *tokenizer) {
    const markdown_core_opener *label = S_last(tokenizer);
    return label && label->shape->own ? label->position : 0;
}

/* COMPILE: a label's tokens no construct was made of are text, and so are
 * the bytes a MARKER keeps. */
static void S_text(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    if (token->end > token->start) {
        markdown_core_compile_text(compiler, (bufsize_t)token->start, (bufsize_t)token->end);
    }
}

const markdown_core_compile_ops MARKDOWN_CORE_LABEL_TOKENS[3] = {
    {.exit = S_text, .plain = true},
    {.exit = S_text, .plain = true},
    {.exit = S_text},
};

bool markdown_core_label_closes(const markdown_core_element_instance *owner, const markdown_core_dialect *dialect) {
    return dialect->shared > 0 || owner->mine_count > 0;
}
