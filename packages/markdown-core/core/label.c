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

markdown_core_opener *markdown_core_opener_below(markdown_core_tokenizer *tokenizer,
                                                 const markdown_core_opener *label) {
    for (size_t at = tokenizer->mark_count; at--;) {
        if (tokenizer->marks[at].tag == S_TAG && tokenizer->marks[at].token == label->token) {
            while (at--) {
                if (tokenizer->marks[at].tag == S_TAG) {
                    return (markdown_core_opener *)S_label(tokenizer, at);
                }
            }
            return NULL;
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

/* THE RULES FOR `label`, kept at `offset`, whose end, its closer at
 * `tokenizer->position`, is token `end`: what the first that makes a
 * construct made, in `*made`, or false. A body no rule decides itself is
 * decided. */
static bool S_rules(markdown_core_tokenizer *tokenizer, uint32_t offset, markdown_core_label_made *made) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_dialect *dialect = inline_state->dialect;
    const int code = tokenizer->data[tokenizer->position];
    const uint32_t after = tokenizer->position + 1;
    markdown_core_opener *label = markdown_core_opener_at(tokenizer, offset);
    /* A label whose element closes its own by `mine` rules meets those
     * alone (element.h, markdown_core_close_rule); any other, the shared
     * rules (dialect.h, `closers`). */
    const markdown_core_element_instance *owner = label->owner;
    const markdown_core_closer *closer = dialect->closers + (owner->mine_count ? owner->mine_first : 0);
    const markdown_core_closer *const last = closer + (owner->mine_count ? owner->mine_count : dialect->shared);
    bool matched = false;
    for (; closer < last; closer++) {
        if (closer->byte != code || !closer->rule->end) {
            continue;
        }
        *made = (markdown_core_label_made){.from = label->start, .past = after, .before = UINT32_MAX};
        const markdown_core_bracket_match match = closer->rule->end(closer->owner, tokenizer, label, made);
        /* A rule may have kept what it read: the label is where it is
         * kept, wherever that now is. */
        label = markdown_core_opener_at(tokenizer, offset);
        if (match == BRACKET_MATCHED) {
            matched = true;
            break;
        }
        if (match == BRACKET_REJECTED) {
            break;
        }
    }
    if (!matched || !made->keeps) {
        markdown_core_label_decide(tokenizer, label);
    }
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

/* THE END of the innermost label at a closer byte. */
static markdown_core_state S_end(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    const uint32_t after = tokenizer->position + 1;
    const size_t mark = markdown_core_mark_find(tokenizer, S_TAG);
    const uint32_t enter = (uint32_t)tokenizer->events.count;
    const uint32_t end = markdown_core_enter(tokenizer, MARKDOWN_CORE_TOKEN_LABEL_END);
    markdown_core_label_made made = {0};
    bool matched = false;
    uint32_t offset = UINT32_MAX;
    if (mark != SIZE_MAX) {
        offset = tokenizer->events.tokens[tokenizer->marks[mark].token].data;
    }
    markdown_core_opener *label = offset == UINT32_MAX ? NULL : markdown_core_opener_at(tokenizer, offset);
    if (label && label->shape->closer == code) {
        /* The body ends where the end begins. */
        label->close = enter;
        label->ended = inline_state->no_link_openers;
        matched = S_rules(tokenizer, offset, &made);
        label = markdown_core_opener_at(tokenizer, offset);
        markdown_core_mark_cut(tokenizer, mark);
        if (matched && made.links) {
            inline_state->no_link_openers = made.links > 0 || label->outer;
        }
    }
    markdown_core_consume_to(tokenizer, matched && made.past > after ? made.past : after);
    markdown_core_exit(tokenizer, MARKDOWN_CORE_TOKEN_LABEL_END);
    if (matched && end != UINT32_MAX) {
        S_make(tokenizer, markdown_core_opener_at(tokenizer, offset), end, &made);
        if (made.follows != MARKDOWN_CORE_NODE_NONE) {
            markdown_core_text_made(tokenizer, made.follows);
        }
    }
    return markdown_core_ok(tokenizer);
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
