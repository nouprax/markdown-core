#include "code.h"
#include "inline_internal.h"
#include "attributes.h"
#define advance(subj) ((subj)->pos += 1)

#define MAXBACKTICKS 80

static MARKDOWN_CORE_INLINE int isbacktick(int c) { return (c == '`'); }

static MARKDOWN_CORE_INLINE markdown_core_chunk take_while(subject *subj, int (*f)(int)) {
    unsigned char c;
    bufsize_t startpos = subj->pos;
    bufsize_t len = 0;

    while ((c = markdown_core_inline_peek_char(subj)) && (*f)(c)) {
        advance(subj);
        len++;
    }

    return markdown_core_chunk_dup(&subj->input, startpos, len);
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the subject
// after the closing backticks.
bufsize_t markdown_core_inline_scan_to_closing_backticks(subject *subj, bufsize_t openticklength) {

    bool found = false;
    if (openticklength > MAXBACKTICKS) {
        // we limit backtick string length because of the array subj->backticks:
        return 0;
    }
    if (!subj->backticks) {
        subj->backtick_capacity = subj->input.len < MAXBACKTICKS ? subj->input.len : MAXBACKTICKS;
        subj->backticks = subj->mem->calloc((size_t)subj->backtick_capacity + 1, sizeof(*subj->backticks));
        if (!subj->backticks) {
            subj->oom = 1;
            return 0;
        }
    }
    if (subj->scanned_for_backticks && subj->backticks[openticklength] <= subj->pos) {
        // return if we already know there's no closer
        return 0;
    }
    while (!found) {
        // read non backticks
        unsigned char c;
        while ((c = markdown_core_inline_peek_char(subj)) && c != '`') {
            advance(subj);
        }
        if (markdown_core_inline_is_eof(subj)) {
            break;
        }
        bufsize_t numticks = 0;
        while (markdown_core_inline_peek_char(subj) == '`') {
            advance(subj);
            numticks++;
        }
        // store position of ender
        if (numticks <= subj->backtick_capacity) {
            subj->backticks[numticks] = subj->pos - numticks;
        }
        if (numticks == openticklength) {
            return (subj->pos);
        }
    }
    // got through whole input without finding closer
    subj->scanned_for_backticks = true;
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
static markdown_core_node *handle_backticks(subject *subj) {
    markdown_core_chunk openticks = take_while(subj, isbacktick);
    bufsize_t startpos = subj->pos;
    bufsize_t endpos = markdown_core_inline_scan_to_closing_backticks(subj, openticks.len);

    if (endpos == 0) {        // not found
        subj->pos = startpos; // rewind
        /* The run stands as its own literal, so it covers ITS OWN BYTES:
         * `startpos` is one past the last of them and the run is
         * `openticks.len` long. Both offsets used to be `subj->pos`, one past
         * the run, so the literal was placed one column right -- and
         * consolidation then carried that end onto the whole merged text run:
         * `hi`lo` reported Text 1:5..1:8 inside a seven-byte paragraph. */
        return make_str(subj, subj->pos - openticks.len, subj->pos - 1, openticks);
    } else {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(subj->mem);

        markdown_core_strbuf_set(&buf, subj->input.data + startpos, endpos - startpos - openticks.len);
        S_normalize_code(&buf);
        if (buf.oom) {
            subj->oom = 1;
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
        markdown_core_node *node = markdown_core_inline_make_literal(
            subj, MARKDOWN_CORE_NODE_CODE, startpos - openticks.len, endpos - 1, markdown_core_chunk_buf_detach(&buf));
        if (!node) {
            return NULL;
        }
        markdown_core_inline_attach_inline_attributes(subj, node, startpos - openticks.len);
        /* The ticks reach no literal and the bytes between them do. */
        return node;
    }
}

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *subj) {
    return character == '`' ? handle_backticks(subj) : NULL;
}
static void dispose_inline(subject *subj) {
    subj->mem->free(subj->backticks);
    subj->backticks = NULL;
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_CODE = {
    .inline_precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .dispose_inline = dispose_inline,

    .name = "code",
    .match_inline = match,
    .terminates_text = "`",
    .dispatch = "`",
};
