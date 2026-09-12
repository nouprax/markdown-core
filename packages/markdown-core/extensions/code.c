#include "code.h"
#include "inline_internal.h"
#include "attributes.h"
#define advance(inline_state) ((inline_state)->pos += 1)

#define MAXBACKTICKS 80

static MARKDOWN_CORE_INLINE int isbacktick(int c) { return (c == '`'); }

static MARKDOWN_CORE_INLINE markdown_core_chunk take_while(markdown_core_inline_state *inline_state, int (*f)(int)) {
    unsigned char c;
    bufsize_t startpos = inline_state->pos;
    bufsize_t len = 0;

    while ((c = markdown_core_inline_peek_char(inline_state)) && (*f)(c)) {
        advance(inline_state);
        len++;
    }

    return markdown_core_chunk_dup(&inline_state->input, startpos, len);
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the inline state
// after the closing backticks.
bufsize_t markdown_core_inline_scan_to_closing_backticks(markdown_core_inline_state *inline_state,
                                                         bufsize_t openticklength) {

    bool found = false;
    if (openticklength > MAXBACKTICKS) {
        // we limit backtick string length because of the array inline_state->backticks:
        return 0;
    }
    if (!inline_state->backticks) {
        inline_state->backtick_capacity =
            inline_state->input.len < MAXBACKTICKS ? inline_state->input.len : MAXBACKTICKS;
        inline_state->backticks =
            inline_state->mem->calloc((size_t)inline_state->backtick_capacity + 1, sizeof(*inline_state->backticks));
        if (!inline_state->backticks) {
            inline_state->oom = 1;
            return 0;
        }
    }
    if (inline_state->scanned_for_backticks && inline_state->backticks[openticklength] <= inline_state->pos) {
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
        if (numticks <= inline_state->backtick_capacity) {
            inline_state->backticks[numticks] = inline_state->pos - numticks;
        }
        if (numticks == openticklength) {
            return (inline_state->pos);
        }
    }
    // got through whole input without finding closer
    inline_state->scanned_for_backticks = true;
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
// Assumes that the inline state has a backtick at the current position.
static markdown_core_node *handle_backticks(markdown_core_inline_state *inline_state) {
    markdown_core_chunk openticks = take_while(inline_state, isbacktick);
    bufsize_t startpos = inline_state->pos;
    bufsize_t endpos = markdown_core_inline_scan_to_closing_backticks(inline_state, openticks.len);

    if (endpos == 0) {                // not found
        inline_state->pos = startpos; // rewind
        /* The run stands as its own literal, so it covers ITS OWN BYTES:
         * `startpos` is one past the last of them and the run is
         * `openticks.len` long. Both offsets used to be `inline_state->pos`, one past
         * the run, so the literal was placed one column right -- and
         * consolidation then carried that end onto the whole merged text run:
         * `hi`lo` reported Text 1:5..1:8 inside a seven-byte paragraph. */
        return make_str(inline_state, inline_state->pos - openticks.len, inline_state->pos - 1, openticks);
    } else {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(inline_state->mem);

        markdown_core_strbuf_set(&buf, inline_state->input.data + startpos, endpos - startpos - openticks.len);
        S_normalize_code(&buf);
        if (buf.oom) {
            inline_state->oom = 1;
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
        markdown_core_inline_attach_inline_attributes(inline_state, node, startpos - openticks.len);
        /* The ticks reach no literal and the bytes between them do. */
        return node;
    }
}

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    return character == '`' ? handle_backticks(inline_state) : NULL;
}
static void dispose_inline(markdown_core_inline_state *inline_state) {
    inline_state->mem->free(inline_state->backticks);
    inline_state->backticks = NULL;
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_CODE = {
    .inline_precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .dispose_inline = dispose_inline,

    .name = "code",
    .match_inline = match,
    .terminates_text = "`",
    .dispatch = "`",
};
