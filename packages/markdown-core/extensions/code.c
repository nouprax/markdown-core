#include "code.h"
#include "inline_internal.h"
#include "attributes.h"
#define advance(inline_parser) ((inline_parser)->pos += 1)

#define MAXBACKTICKS 80

static MARKDOWN_CORE_INLINE int isbacktick(int c) { return (c == '`'); }

static MARKDOWN_CORE_INLINE markdown_core_chunk take_while(subject *inline_parser, int (*f)(int)) {
    unsigned char c;
    bufsize_t startpos = inline_parser->pos;
    bufsize_t len = 0;

    while ((c = markdown_core_inline_peek_char(inline_parser)) && (*f)(c)) {
        advance(inline_parser);
        len++;
    }

    return markdown_core_chunk_dup(&inline_parser->input, startpos, len);
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the subject
// after the closing backticks.
bufsize_t markdown_core_inline_scan_to_closing_backticks(subject *inline_parser, bufsize_t openticklength) {

    bool found = false;
    if (openticklength > MAXBACKTICKS) {
        // we limit backtick string length because of the array inline_parser->backticks:
        return 0;
    }
    if (!inline_parser->backticks) {
        inline_parser->backtick_capacity =
            inline_parser->input.len < MAXBACKTICKS ? inline_parser->input.len : MAXBACKTICKS;
        inline_parser->backticks =
            inline_parser->mem->calloc((size_t)inline_parser->backtick_capacity + 1, sizeof(*inline_parser->backticks));
        if (!inline_parser->backticks) {
            inline_parser->oom = 1;
            return 0;
        }
    }
    if (inline_parser->scanned_for_backticks && inline_parser->backticks[openticklength] <= inline_parser->pos) {
        // return if we already know there's no closer
        return 0;
    }
    while (!found) {
        // read non backticks
        unsigned char c;
        while ((c = markdown_core_inline_peek_char(inline_parser)) && c != '`') {
            advance(inline_parser);
        }
        if (markdown_core_inline_is_eof(inline_parser)) {
            break;
        }
        bufsize_t numticks = 0;
        while (markdown_core_inline_peek_char(inline_parser) == '`') {
            advance(inline_parser);
            numticks++;
        }
        // store position of ender
        if (numticks <= inline_parser->backtick_capacity) {
            inline_parser->backticks[numticks] = inline_parser->pos - numticks;
        }
        if (numticks == openticklength) {
            return (inline_parser->pos);
        }
    }
    // got through whole input without finding closer
    inline_parser->scanned_for_backticks = true;
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
// Assumes that the subject has a backtick at the current position.
static markdown_core_node *handle_backticks(subject *inline_parser) {
    markdown_core_chunk openticks = take_while(inline_parser, isbacktick);
    bufsize_t startpos = inline_parser->pos;
    bufsize_t endpos = markdown_core_inline_scan_to_closing_backticks(inline_parser, openticks.len);

    if (endpos == 0) {                 // not found
        inline_parser->pos = startpos; // rewind
        /* The run stands as its own literal, so it covers ITS OWN BYTES:
         * `startpos` is one past the last of them and the run is
         * `openticks.len` long. Both offsets used to be `inline_parser->pos`, one past
         * the run, so the literal was placed one column right -- and
         * consolidation then carried that end onto the whole merged text run:
         * `hi`lo` reported Text 1:5..1:8 inside a seven-byte paragraph. */
        return make_str(inline_parser, inline_parser->pos - openticks.len, inline_parser->pos - 1, openticks);
    } else {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(inline_parser->mem);

        markdown_core_strbuf_set(&buf, inline_parser->input.data + startpos, endpos - startpos - openticks.len);
        S_normalize_code(&buf);
        if (buf.oom) {
            inline_parser->oom = 1;
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
            markdown_core_inline_make_literal(inline_parser, MARKDOWN_CORE_NODE_CODE, startpos - openticks.len,
                                              endpos - 1, markdown_core_chunk_buf_detach(&buf));
        if (!node) {
            return NULL;
        }
        markdown_core_inline_attach_inline_attributes(inline_parser, node, startpos - openticks.len);
        /* The ticks reach no literal and the bytes between them do. */
        return node;
    }
}

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    return character == '`' ? handle_backticks(inline_parser) : NULL;
}
static void dispose_inline(subject *inline_parser) {
    inline_parser->mem->free(inline_parser->backticks);
    inline_parser->backticks = NULL;
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_CODE = {
    .inline_precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .dispose_inline = dispose_inline,

    .name = "code",
    .match_inline = match,
    .terminates_text = "`",
    .dispatch = "`",
};
