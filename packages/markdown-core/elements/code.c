#include "alloc.h"
#include "code.h"
#include "inline_internal.h"
#include "services.h"

// Destructively modify string, converting newlines to
// spaces, then removing a single leading + trailing space,
// unless the code span consists entirely of space characters.
static void S_normalize_code(markdown_core_strbuf *s) {
    bufsize_t r, w;
    bool contains_nonspace = false;

    for (r = 0, w = 0; r < s->size; ++r) {
        switch (s->ptr[r]) {
        case '\r':
            if (s->ptr[r + 1] != '\n') {
                s->ptr[w++] = ' ';
            }
            break;
        case '\n':
            s->ptr[w++] = ' ';
            break;
        default:
            s->ptr[w++] = s->ptr[r];
        }
        if (s->ptr[r] != ' ') {
            contains_nonspace = true;
        }
    }

    // begins and ends with space?
    if (contains_nonspace && s->ptr[0] == ' ' && s->ptr[w - 1] == ' ') {
        markdown_core_strbuf_drop(s, 1);
        markdown_core_strbuf_truncate(s, w - 2);
    } else {
        markdown_core_strbuf_truncate(s, w);
    }
}

static void dispose_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_backticks *backticks = markdown_core_run_state(inline_state, self);
    markdown_core_free(backticks->positions);
    *backticks = (markdown_core_backticks){0};
}

/* THE CODE SPAN (plan symbols-cleanroom.md 23, micromark's code text),
 * code by code: a backtick run, then, checked, the bytes up to the first
 * run as long (CODE_BODY), and that run, one token whose `data` is the
 * run's length; a run no closer follows, an opening token, which is text
 * and the code span it opens once a closer follows. A run over 80 long
 * has no closer. The frame's words: where the opener starts, and its token. */
enum { CODE_SPAN, CODE_OPEN };
enum { SPAN_FROM, SPAN_TOKEN };
enum { BODY_RUN };
#define MAXBACKTICKS 80

static inline markdown_core_backticks *S_ticks(markdown_core_tokenizer *tokenizer) {
    return markdown_core_read_record(
        tokenizer, ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->run_offset);
}

/* No closer: the reading read on to the end from where it began. */
static markdown_core_state S_body_end(markdown_core_tokenizer *tokenizer) {
    markdown_core_backticks *backticks = S_ticks(tokenizer);
    const uint32_t from = tokenizer->frames[tokenizer->frame_count - 1].position;
    if (from < backticks->from) {
        backticks->from = from;
    }
    markdown_core_met_end(tokenizer);
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_body(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_body_run(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '`') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_body_run);
    }
    markdown_core_backticks *backticks = S_ticks(tokenizer);
    const uint32_t run = markdown_core_frame_local(tokenizer)[BODY_RUN], ticks = tokenizer->position - run;
    /* The last run of each length: readings may run in any order. */
    if (ticks <= (uint32_t)backticks->capacity && (bufsize_t)run > backticks->positions[ticks]) {
        backticks->positions[ticks] = (bufsize_t)run;
    }
    if (ticks == backticks->want) {
        backticks->close = tokenizer->position;
        return markdown_core_ok(tokenizer);
    }
    return S_body(tokenizer, code);
}

static markdown_core_state S_body(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF) {
        return S_body_end(tokenizer);
    }
    if (code == '`') {
        markdown_core_frame_local(tokenizer)[BODY_RUN] = tokenizer->position;
    }
    markdown_core_consume(tokenizer);
    return code == '`' ? markdown_core_state_of(S_body_run) : markdown_core_state_of(S_body);
}

static markdown_core_state S_body_start(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_backticks *backticks = S_ticks(tokenizer);
    const uint32_t from = tokenizer->position, want = backticks->want;
    if (want > MAXBACKTICKS) {
        return markdown_core_nok(tokenizer);
    }
    if (!backticks->positions) {
        const uint32_t size = tokenizer->size;
        backticks->capacity = (bufsize_t)(size < MAXBACKTICKS ? size : MAXBACKTICKS);
        backticks->positions = markdown_core_alloc((size_t)backticks->capacity + 1, sizeof(*backticks->positions));
        backticks->from = UINT32_MAX;
        if (!backticks->positions) {
            tokenizer->events.failed = true;
            return markdown_core_nok(tokenizer);
        }
    }
    if (backticks->from <= from && (uint32_t)backticks->positions[want] <= from) {
        markdown_core_met_end(tokenizer);
        return markdown_core_nok(tokenizer);
    }
    return S_body(tokenizer, code);
}

static const markdown_core_construct CODE_BODY = {.start = S_body_start};

/* The span's token ends, of `kind`. */
static markdown_core_state S_span_end(markdown_core_tokenizer *tokenizer, unsigned kind) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t token = markdown_core_frame_local(tokenizer)[SPAN_TOKEN];
    const uint16_t made = (uint16_t)(self->token_base + kind);
    if (token != UINT32_MAX) {
        tokenizer->events.tokens[token].kind = made;
    }
    markdown_core_exit(tokenizer, made);
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_closed(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const uint32_t close = S_ticks(tokenizer)->close;
    /* A closer found reads the byte after it only to know where its run
     * ends. */
    if (close >= tokenizer->size) {
        markdown_core_met_end(tokenizer);
    }
    markdown_core_consume_to(tokenizer, close);
    const markdown_core_state next = S_span_end(tokenizer, CODE_SPAN);
    /* What follows a code span may update it (no-flush.md 10.13). */
    markdown_core_text_made(tokenizer, MARKDOWN_CORE_NODE_CODE);
    return next;
}

/* A run with no closer opens a code span once one follows. */
static markdown_core_state S_unclosed(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const uint32_t from = markdown_core_frame_local(tokenizer)[SPAN_FROM];
    markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_CODE, (bufsize_t)from, (bufsize_t)tokenizer->position,
                               NULL, 0);
    return S_span_end(tokenizer, CODE_OPEN);
}

static markdown_core_state S_opener(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '`') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_opener);
    }
    const uint32_t *words = markdown_core_frame_local(tokenizer);
    const uint32_t length = tokenizer->position - words[SPAN_FROM];
    if (words[SPAN_TOKEN] != UINT32_MAX) {
        tokenizer->events.tokens[words[SPAN_TOKEN]].data = length;
    }
    S_ticks(tokenizer)->want = length;
    return markdown_core_check(tokenizer, &CODE_BODY, markdown_core_state_of(S_closed),
                               markdown_core_state_of(S_unclosed), code);
}

static markdown_core_state S_span(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    uint32_t *words = markdown_core_frame_local(tokenizer);
    words[SPAN_FROM] = tokenizer->position;
    words[SPAN_TOKEN] = markdown_core_enter(tokenizer, (uint16_t)(self->token_base + CODE_SPAN));
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_opener);
}
static const markdown_core_text_construct CODE_CONSTRUCTS[] = {{"`", {.start = S_span}}};

static void S_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const bufsize_t length = (bufsize_t)token->data, startpos = (bufsize_t)token->start + length,
                    endpos = (bufsize_t)token->end;
    if (token->kind == self->token_base + CODE_OPEN) {
        markdown_core_compile_append(
            compiler, make_str(inline_state, (bufsize_t)token->start, startpos - 1,
                               markdown_core_inline_take(inline_state, (bufsize_t)token->start, length)));
        return;
    }
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();
    markdown_core_strbuf_set(&buf, markdown_core_inline_window(inline_state, 0).data + startpos,
                             endpos - startpos - length);
    S_normalize_code(&buf);
    if (buf.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    markdown_core_node *node =
        markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_CODE, (bufsize_t)token->start, endpos - 1,
                                          markdown_core_chunk_buf_detach(&buf));
    if (!markdown_core_compile_append(compiler, node)) {
        return;
    }
    markdown_core_inline_seek(inline_state, endpos);
    /* The ticks reach no literal and the bytes between them do: its field
     * of no slot kind, less the one space S_normalize_code takes from each
     * end of a span that holds more than spaces. */
    const unsigned char *s = markdown_core_inline_window(inline_state, 0).data;
    bufsize_t from = startpos, to = endpos - length;
    bool other = false;
    for (bufsize_t i = from; i < to && !other; i++) {
        other = s[i] != ' ' && s[i] != '\n' && s[i] != '\r';
    }
    if (other && to - from >= 2 && (s[from] == ' ' || s[from] == '\n' || s[from] == '\r') &&
        (s[to - 1] == ' ' || s[to - 1] == '\n' || s[to - 1] == '\r')) {
        from += s[from] == '\r' && s[from + 1] == '\n' ? 2 : 1;
        to -= s[to - 1] == '\n' && to - 2 >= from && s[to - 2] == '\r' ? 2 : 1;
    }
    const markdown_core_field_to to_node = markdown_core_inline_fields(inline_state, node);
    markdown_core_field_add(&to_node, MARKDOWN_CORE_SLOT_NONE, from, to);
}
static const markdown_core_compile_ops CODE_TOKENS[] = {[CODE_SPAN] = {.exit = S_made},
                                                        [CODE_OPEN] = {.exit = S_made, .plain = true}};

static const markdown_core_inline_ops CODE_INLINES = {
    .constructs = CODE_CONSTRUCTS,
    .construct_count = 1,
    .tokens = CODE_TOKENS,
    .token_count = 2,
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .dispose = dispose_inline,
};

const markdown_core_facet MARKDOWN_CORE_CODE_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_CODE, .reads = MARKDOWN_CORE_READS_LITERAL},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE = {
    .facets = MARKDOWN_CORE_CODE_FACETS,
    .run_state_size = sizeof(markdown_core_backticks),
    .name = "code",
    .inlines = &CODE_INLINES,
};
