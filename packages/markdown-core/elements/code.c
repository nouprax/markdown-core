#include "alloc.h"
#include "code.h"
#include "inline_internal.h"
#include "parts.h"
#include "attributes.h"
#include "services.h"
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

#define MAXBACKTICKS 80

static MARKDOWN_CORE_INLINE int isbacktick(int c) { return (c == '`'); }

static MARKDOWN_CORE_INLINE markdown_core_chunk take_while(markdown_core_inline_state *inline_state, int (*f)(int)) {
    unsigned char c;
    bufsize_t startpos = markdown_core_inline_offset(inline_state);
    bufsize_t len = 0;

    while ((c = markdown_core_inline_peek_char(inline_state)) && (*f)(c)) {
        advance(inline_state);
        len++;
    }

    return markdown_core_inline_take(inline_state, startpos, len);
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the inline state
// after the closing backticks.
// Assumes that the inline state has a backtick at the current position.
/* `code` is the code element's instance: the positions the scan indexes are
 * its run record, which another element's scan shares. */
bufsize_t markdown_core_inline_scan_to_closing_backticks(const markdown_core_element_instance *code,
                                                         markdown_core_inline_state *inline_state,
                                                         bufsize_t openticklength) {
    return markdown_core_inline_scan_backticks(markdown_core_run_state(inline_state, code), inline_state,
                                               openticklength);
}

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
        while ((c = markdown_core_inline_peek_char(inline_state)) && c != '`') {
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

// Parse backtick code section or raw backticks, return an inline.
static markdown_core_node *handle_backticks(const markdown_core_element_instance *self,
                                            markdown_core_inline_state *inline_state) {
    markdown_core_chunk openticks = take_while(inline_state, isbacktick);
    bufsize_t startpos = markdown_core_inline_offset(inline_state);
    bufsize_t endpos = markdown_core_inline_scan_to_closing_backticks(self, inline_state, openticks.len);

    if (endpos == 0) {                                     // not found
        markdown_core_inline_seek(inline_state, startpos); // rewind
        /* A run with no closer opens a code span once one follows. */
        markdown_core_inline_open(inline_state, MARKDOWN_CORE_KIND_CODE, startpos - openticks.len, startpos, NULL, 0);
        /* The run stands as its own literal, so it covers ITS OWN BYTES:
         * `startpos` is one past the last of them and the run is
         * `openticks.len` long. Both offsets used to be `markdown_core_inline_offset(inline_state)`, one past
         * the run, so the literal was placed one column right -- and
         * consolidation then carried that end onto the whole merged text run:
         * `hi`lo` reported Text 1:5..1:8 inside a seven-byte paragraph. */
        return make_str(inline_state, markdown_core_inline_offset(inline_state) - openticks.len,
                        markdown_core_inline_offset(inline_state) - 1, openticks);
    } else {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();

        markdown_core_strbuf_set(&buf, markdown_core_inline_window(inline_state, 0).data + startpos,
                                 endpos - startpos - openticks.len);
        S_normalize_code(&buf);
        if (buf.oom) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }

        /* A CODE SPAN COVERS ITS BACKTICKS (Q45, answered 2026-08-23). Every
         * other inline construct covers its own delimiters -- emphasis its
         * asterisks, a link its brackets and parens, strikethrough both tilde
         * pairs -- and this one reported the extent of its CONTENT instead,
         * which is what upstream reports and is a defect inherited from it.
         *
         * A scope exists so a consumer can map a node back to the source it
         * came from, and the source a code span came from includes the ticks
         * that make it one. Reporting the content alone also produced a start
         * that is not a place: `` `` `` alone on a line put the span at column
         * 3 of a two-byte line. */
        markdown_core_node *node =
            markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_CODE, startpos - openticks.len,
                                              endpos - 1, markdown_core_chunk_buf_detach(&buf));
        if (!node) {
            return NULL;
        }
        /* What follows a code span may update it (no-flush.md 10.13). */
        markdown_core_inline_made(inline_state, node);
        /* The ticks reach no literal and the bytes between them do. */
        return node;
    }
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    return character == '`' ? handle_backticks(self, inline_state) : NULL;
}
void markdown_core_backticks_free(markdown_core_backticks *backticks) {
    markdown_core_free(backticks->positions);
    *backticks = (markdown_core_backticks){0};
}

static void dispose_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_backticks_free(markdown_core_run_state(inline_state, self));
}

/* THE PARTS OF A CODE SPAN (parts.h): its literal, the bytes between its
 * backtick runs less the one space S_normalize_code takes from each end,
 * and the attribute container after its closing run. */
void markdown_core_code_parts(markdown_core_parts *parts) {
    const unsigned char *s = parts->text;
    const bufsize_t n = parts->length;
    bufsize_t ticks = 0, at;
    while (ticks < n && s[ticks] == '`') {
        ticks++;
    }
    /* The closer is the first run of as many backticks. */
    bufsize_t close = n;
    for (at = ticks; at < n;) {
        if (s[at] != '`') {
            at++;
            continue;
        }
        const bufsize_t run = at;
        while (at < n && s[at] == '`') {
            at++;
        }
        if (at - run == ticks) {
            close = run;
            break;
        }
    }
    bufsize_t from = ticks, to = close;
    bool space = false;
    for (bufsize_t i = from; i < to && !space; i++) {
        space = s[i] != ' ' && s[i] != '\n' && s[i] != '\r';
    }
    if (space && to - from >= 2 && (s[from] == ' ' || s[from] == '\n' || s[from] == '\r') &&
        (s[to - 1] == ' ' || s[to - 1] == '\n' || s[to - 1] == '\r')) {
        from += s[from] == '\r' && s[from + 1] == '\n' ? 2 : 1;
        to -= s[to - 1] == '\n' && to - 2 >= from && s[to - 2] == '\r' ? 2 : 1;
    }
    markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, from, to);
    if (close + ticks < n && s[close + ticks] == '{') {
        markdown_core_attributes_parts(parts, close + ticks);
    }
}

static const markdown_core_inline_ops CODE_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .dispose = dispose_inline,
    .match = match,
    .terminates = "`",
    .dispatch = "`",
};

const markdown_core_facet MARKDOWN_CORE_CODE_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_CODE, .parts = markdown_core_code_parts, .reads = MARKDOWN_CORE_READS_LITERAL},
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
