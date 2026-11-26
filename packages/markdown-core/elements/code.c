#include "alloc.h"
#include "code.h"
#include "inline_internal.h"
#include "services.h"
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

#define MAXBACKTICKS 80

bufsize_t markdown_core_inline_scan_backticks(markdown_core_backticks *backticks,
                                              markdown_core_inline_state *inline_state, bufsize_t openticklength) {

    bool found = false;
    if (openticklength > MAXBACKTICKS) {
        // we limit backtick string length because of the positions array:
        return 0;
    }
    if (!backticks->positions) {
        backticks->capacity = markdown_core_inline_size(inline_state) < MAXBACKTICKS
                                  ? markdown_core_inline_size(inline_state)
                                  : MAXBACKTICKS;
        backticks->positions = markdown_core_alloc((size_t)backticks->capacity + 1, sizeof(*backticks->positions));
        if (!backticks->positions) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            return 0;
        }
    }
    if (backticks->scanned && backticks->positions[openticklength] <= markdown_core_inline_offset(inline_state)) {
        // return if we already know there's no closer
        return 0;
    }
    while (!found) {
        // read non backticks
        unsigned char c;
        while (!markdown_core_inline_is_eof(inline_state) &&
               (c = markdown_core_inline_peek_char(inline_state)) != '`') {
            advance(inline_state);
        }
        if (markdown_core_inline_is_eof(inline_state)) {
            break;
        }
        bufsize_t numticks = 0;
        while (markdown_core_inline_peek_char(inline_state) == '`') {
            advance(inline_state);
            numticks++;
        }
        // store position of ender
        if (numticks <= backticks->capacity) {
            backticks->positions[numticks] = markdown_core_inline_offset(inline_state) - numticks;
        }
        if (numticks == openticklength) {
            return (markdown_core_inline_offset(inline_state));
        }
    }
    // got through whole input without finding closer
    backticks->scanned = true;
    return 0;
}

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

void markdown_core_backticks_free(markdown_core_backticks *backticks) {
    markdown_core_free(backticks->positions);
    *backticks = (markdown_core_backticks){0};
}

static void dispose_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_backticks_free(markdown_core_run_state(inline_state, self));
}

/* THE CODE SPAN (plan symbols-cleanroom.md 23): a backtick run, the bytes
 * up to the first run as long, and that run, one token whose `data` is the
 * run's length; a run no closer follows, an opening token, which is text
 * and the code span it opens once a closer follows. */
enum { CODE_SPAN, CODE_OPEN };

/* The end of the first backtick run `length` long from `from` in the `size`
 * bytes at `data`, or 0, with the memo of what earlier scans found. */
static uint32_t S_closer(markdown_core_backticks *backticks, const unsigned char *data, uint32_t size, uint32_t from,
                         uint32_t length, bool *error) {
    if (length > MAXBACKTICKS) {
        return 0;
    }
    if (!backticks->positions) {
        backticks->capacity = (bufsize_t)(size < MAXBACKTICKS ? size : MAXBACKTICKS);
        backticks->positions = markdown_core_alloc((size_t)backticks->capacity + 1, sizeof(*backticks->positions));
        if (!backticks->positions) {
            *error = true;
            return 0;
        }
    }
    if (backticks->scanned && (uint32_t)backticks->positions[length] <= from) {
        return 0;
    }
    uint32_t at = from;
    for (;;) {
        while (at < size && data[at] != '`') {
            at++;
        }
        if (at >= size) {
            break;
        }
        uint32_t ticks = 0;
        while (at < size && data[at] == '`') {
            at++;
            ticks++;
        }
        if (ticks <= (uint32_t)backticks->capacity) {
            backticks->positions[ticks] = (bufsize_t)(at - ticks);
        }
        if (ticks == length) {
            return at;
        }
    }
    backticks->scanned = true;
    return 0;
}

static markdown_core_state S_span(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t start = tokenizer->position;
    uint32_t open = start;
    while (open < tokenizer->size && tokenizer->data[open] == '`') {
        open++;
    }
    bool error = false;
    const uint32_t end = S_closer(markdown_core_read_record(tokenizer, self->run_offset), tokenizer->data,
                                  tokenizer->size, open, open - start, &error);
    if (error) {
        tokenizer->events.failed = true;
    }
    /* Reading to the end without a closer reads the end; one found before
     * it reads the byte after it only to know where its run ends. */
    if (!end || end >= tokenizer->size) {
        markdown_core_met_end(tokenizer);
    }
    const uint16_t kind = (uint16_t)(self->token_base + (end ? CODE_SPAN : CODE_OPEN));
    /* A run with no closer opens a code span once one follows. */
    if (!end) {
        markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_CODE, (bufsize_t)start, (bufsize_t)open, NULL, 0);
    }
    const uint32_t token = markdown_core_enter(tokenizer, kind);
    if (token != UINT32_MAX) {
        tokenizer->events.tokens[token].data = open - start;
    }
    markdown_core_consume_to(tokenizer, end ? end : open);
    markdown_core_exit(tokenizer, kind);
    if (end) {
        /* What follows a code span may update it (no-flush.md 10.13). */
        markdown_core_text_made(tokenizer, MARKDOWN_CORE_NODE_CODE);
    }
    return markdown_core_ok(tokenizer);
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

/* (services.h, MARKDOWN_CORE_SERVICE_CODE_SPANS) A code span another
 * element's scan reads ahead of the run's own, from a backtick run the run
 * may not take as an opener, on a backtick index of the scan's own, its
 * memo. */
/* C99 has no _Static_assert: an array of negative size fails the build. */
typedef char S_backticks_fit[sizeof(markdown_core_backticks) <= sizeof(markdown_core_skip_memo) ? 1 : -1];

static bufsize_t S_skip(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                        bufsize_t at, markdown_core_skip_memo *memo, bufsize_t *open) {
    (void)self;
    bufsize_t run = at;
    while (run < markdown_core_inline_size(inline_state) && markdown_core_inline_char_at(inline_state, run) == '`') {
        run++;
    }
    const bufsize_t saved = markdown_core_inline_offset(inline_state);
    markdown_core_inline_seek(inline_state, run);
    const bufsize_t end = markdown_core_inline_scan_backticks((markdown_core_backticks *)memo, inline_state, run - at);
    markdown_core_inline_seek(inline_state, saved);
    *open = end ? at : run;
    return end;
}

static void S_release(const markdown_core_element_instance *self, markdown_core_skip_memo *memo) {
    (void)self;
    markdown_core_backticks_free((markdown_core_backticks *)memo);
}

static const markdown_core_skip_ops CODE_SPANS = {.skip = S_skip, .release = S_release};
static const markdown_core_service CODE_SERVICES[] = {{MARKDOWN_CORE_SERVICE_CODE_SPANS, &CODE_SPANS}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE = {
    .provides = CODE_SERVICES,
    .facets = MARKDOWN_CORE_CODE_FACETS,
    .run_state_size = sizeof(markdown_core_backticks),
    .name = "code",
    .inlines = &CODE_INLINES,
};
