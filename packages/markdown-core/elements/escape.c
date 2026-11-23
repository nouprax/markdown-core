#include "alloc.h"
#include "escape.h"
#include "text.h"
#include "inline_internal.h"
#include "markdown_core_ctype.h"

/* The elements whose state this element names: an escaped space is the
 * text element's contextual space (text.h). */
static const markdown_core_element *const ESCAPE_PEERS[] = {&MARKDOWN_CORE_ELEMENT_TEXT, NULL};
#define advance(inline_state) ((inline_state)->pos += 1)

void markdown_core_escape_strip(const markdown_core_element_instance *escape, markdown_core_strbuf *buf) {
    if (!escape) {
        return;
    }
    bufsize_t r, w;

    markdown_core_strbuf_unsettle(buf, 0);
    for (r = 0, w = 0; r < buf->size; ++r) {
        if (buf->ptr[r] == '\\' && markdown_core_ispunct(buf->ptr[r + 1])) {
            r++;
        }

        buf->ptr[w++] = buf->ptr[r];
    }

    markdown_core_strbuf_truncate(buf, w);
}

/* Whether an element other than this one owns `c`. */
static int any_element_dispatches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                  unsigned char c) {
    const markdown_core_dialect *dialect = parser->dialect;
    for (size_t i = dialect->inline_dispatch_offsets[c]; i < dialect->inline_dispatch_offsets[c + 1]; i++) {
        if (dialect->inline_dispatch[i] != self) {
            return 1;
        }
    }

    return 0;
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    (void)parent;
    if (character != '\\') {
        return NULL;
    }
    markdown_core_escape_work *counts = self->state;
    bufsize_t start = inline_state->pos;
    /* The line frame BEFORE anything is consumed. The hard-break arm below
     * needs it, and reading it after `markdown_core_inline_skip_line_end` would be right only
     * because `markdown_core_inline_skip_line_end` happens not to advance the frame -- an accident,
     * not a contract. `handle_newline` captures its frame first for the same
     * reason, and the two arms must not merely look symmetric. */
    advance(inline_state);
    unsigned char nextchar = markdown_core_inline_peek_char(inline_state);
    if (nextchar == ' ') {
        /* A trailing whitespace run cannot belong to a completed script.
         * Leave it to the inherited text/line-ending scanner, including its
         * trimming and hard-break rules. These lookaheads are disjoint: each
         * begins after its own backslash and ends before the next token. */
        bufsize_t end = inline_state->pos;
        while (end < inline_state->input.len &&
               markdown_core_is_space_or_tab(markdown_core_inline_peek_at(inline_state, end))) {
            end++;
        }
        if (end == inline_state->input.len) {
            markdown_core_inline_saw_end(inline_state);
        }
        counts->whitespace += (size_t)(end - inline_state->pos);
        if ((end == inline_state->input.len && !MARKDOWN_CORE_NODE_TYPE_INLINE_P(inline_state->owner->kind)) ||
            (end < inline_state->input.len &&
             markdown_core_is_line_end(markdown_core_inline_peek_at(inline_state, end)))) {
            return make_str(inline_state, start, start, markdown_core_chunk_dup(&inline_state->input, start, 1));
        }
        advance(inline_state);
        markdown_core_node *escaped = make_str(inline_state, start, inline_state->pos - 1,
                                               markdown_core_chunk_dup(&inline_state->input, start, 2));
        if (escaped) {
            /* Contextual escape token: inline completion decodes it once the
             * delimiter/bracket engine has established its semantic owner. */
            escaped->flags |= MARKDOWN_CORE_NODE__ESCAPED_SPACE;
        }
        return escaped;
    }
    if (markdown_core_ispunct(nextchar)) {
        if (nextchar == '\\' && !any_element_dispatches(self, parser, '\\')) {
            bufsize_t end = start;
            while (end + 1 < inline_state->input.len && inline_state->input.data[end] == '\\' &&
                   inline_state->input.data[end + 1] == '\\') {
                end += 2;
            }
            if (end + 1 >= inline_state->input.len) {
                markdown_core_inline_saw_end(inline_state);
            }
            if (end - start >= 4) {
                bufsize_t output_len = (end - start) / 2;
                unsigned char *output = (unsigned char *)markdown_core_alloc((size_t)output_len + 1, 1);
                if (output) {
                    markdown_core_chunk contents = {output, output_len, 1};
                    markdown_core_node *run;
                    memset(output, '\\', (size_t)output_len);
                    inline_state->pos = end;
                    run = make_str(inline_state, start, end - 1, contents);
                    /* One escape per PAIR: the first backslash of each is the
                     * escape and reaches no literal, the second is the byte the
                     * literal is made of. */
                    return run;
                }
            }
        }
        // only ascii symbols and newline can be escaped
        advance(inline_state);
        {
            markdown_core_node *escaped =
                make_str(inline_state, inline_state->pos - 2, inline_state->pos - 1,
                         markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
            return escaped;
        }
    } else if (!markdown_core_inline_is_eof(inline_state) && markdown_core_inline_skip_line_end(inline_state)) {
        markdown_core_inline_push_boundary(inline_state, inline_state->pos);
        // A backslash hard break CONSUMES a line ending, so the inline state has to
        // be told, exactly as handle_newline tells it. It was not, so every node
        // after such a break kept the break's own line and a column measured
        // from the wrong line's start: `foo\` / `bar` reported Text 1:6..1:8 --
        // three columns that do not exist on a four-character line 1.
        // cmark-gfm reports the same numbers, so upstream cannot be the oracle.
        //
        // The node's extent is the bytes that SPELL it: the backslash and the
        // line ending it escapes. The backslash belonged to no node at all
        // before this, so the break is not taking it from anyone.
        markdown_core_node *hard =
            markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_LINE_BREAK);
        if (hard) {
            /* The break's extent is the backslash and the line ending it
             * escapes; `inline_state->pos` is one past that ending's last byte, CR, LF
             * or CRLF alike. Projected from the two offsets, so the frame
             * captured before the consume is only the fallback's. */
            markdown_core_inline_state_place(inline_state, hard, start, inline_state->pos - 1);
        }
        return hard;
    } else {
        return make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                        markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
    }
}

static const markdown_core_inline_ops ESCAPE_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .match = match,
    .terminates = "\\",
    .dispatch = "\\",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ESCAPE = {
    .peers = ESCAPE_PEERS,
    .state_size = sizeof(markdown_core_escape_work),
    .name = "escape",
    .inlines = &ESCAPE_INLINES,
};
