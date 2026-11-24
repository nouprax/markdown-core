#include "alloc.h"
#include "escape.h"
#include "services.h"
#include "inline_internal.h"
#include "markdown_core_ctype.h"

/* The services this element asks, as `self->needs` holds them: an escaped
 * space is a word space (services.h). */
enum { ESCAPE_WORD_SPACES };
static const char *const ESCAPE_NEEDS[] = {[ESCAPE_WORD_SPACES] = MARKDOWN_CORE_SERVICE_WORD_SPACES, NULL};
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

/* The escapes of a slice another element decodes (a link's destination and
 * title, a fence's info string): `buf` loses each backslash before an ASCII
 * punctuation character, in place. */
static void S_strip(const markdown_core_element_instance *self, markdown_core_strbuf *buf) {
    (void)self;
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
    bufsize_t start = markdown_core_inline_offset(inline_state);
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
        bufsize_t end = markdown_core_inline_offset(inline_state);
        while (end < markdown_core_inline_size(inline_state) &&
               markdown_core_is_space_or_tab(markdown_core_inline_peek_at(inline_state, end))) {
            end++;
        }
        if (end == markdown_core_inline_size(inline_state)) {
            markdown_core_inline_saw_end(inline_state);
        }
        counts->whitespace += (size_t)(end - markdown_core_inline_offset(inline_state));
        if ((end == markdown_core_inline_size(inline_state) &&
             !MARKDOWN_CORE_NODE_TYPE_INLINE_P(inline_state->owner->kind)) ||
            (end < markdown_core_inline_size(inline_state) &&
             markdown_core_is_line_end(markdown_core_inline_peek_at(inline_state, end)))) {
            return make_str(inline_state, start, start, markdown_core_inline_take(inline_state, start, 1));
        }
        advance(inline_state);
        markdown_core_node *escaped = make_str(inline_state, start, markdown_core_inline_offset(inline_state) - 1,
                                               markdown_core_inline_take(inline_state, start, 2));
        if (escaped) {
            /* Contextual escape token: inline completion decodes it once the
             * delimiter/bracket engine has established its semantic owner. */
            markdown_core_word_space_note(self->needs[ESCAPE_WORD_SPACES], escaped);
        }
        return escaped;
    }
    if (markdown_core_ispunct(nextchar)) {
        if (nextchar == '\\' && !any_element_dispatches(self, parser, '\\')) {
            bufsize_t end = start;
            while (end + 1 < markdown_core_inline_size(inline_state) &&
                   markdown_core_inline_char_at(inline_state, end) == '\\' &&
                   markdown_core_inline_char_at(inline_state, end + 1) == '\\') {
                end += 2;
            }
            if (end + 1 >= markdown_core_inline_size(inline_state)) {
                markdown_core_inline_saw_end(inline_state);
            }
            if (end - start >= 4) {
                bufsize_t output_len = (end - start) / 2;
                unsigned char *output = (unsigned char *)markdown_core_alloc((size_t)output_len + 1, 1);
                if (output) {
                    markdown_core_chunk contents = {output, output_len, 1};
                    markdown_core_node *run;
                    memset(output, '\\', (size_t)output_len);
                    markdown_core_inline_seek(inline_state, end);
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
                make_str(inline_state, markdown_core_inline_offset(inline_state) - 2,
                         markdown_core_inline_offset(inline_state) - 1,
                         markdown_core_inline_take(inline_state, markdown_core_inline_offset(inline_state) - 1, 1));
            return escaped;
        }
    } else if (!markdown_core_inline_is_eof(inline_state) && markdown_core_inline_skip_line_end(inline_state)) {
        markdown_core_inline_push_boundary(inline_state, markdown_core_inline_offset(inline_state));
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
             * escapes; `markdown_core_inline_offset(inline_state)` is one past that ending's last byte, CR, LF
             * or CRLF alike. Projected from the two offsets, so the frame
             * captured before the consume is only the fallback's. */
            markdown_core_inline_state_place(inline_state, hard, start, markdown_core_inline_offset(inline_state) - 1);
        }
        return hard;
    } else {
        return make_str(inline_state, markdown_core_inline_offset(inline_state) - 1,
                        markdown_core_inline_offset(inline_state) - 1,
                        markdown_core_inline_take(inline_state, markdown_core_inline_offset(inline_state) - 1, 1));
    }
}

static const markdown_core_inline_ops ESCAPE_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .match = match,
    .terminates = "\\",
    .dispatch = "\\",
};

static const markdown_core_decoder ESCAPE_DECODER = {.strip = S_strip};

/* A byte a spelling writes literal: a backslash before it. */
static size_t S_guard(uint8_t *out, uint8_t c) {
    out[0] = '\\';
    out[1] = c;
    return 2;
}

static const markdown_core_escape_writing ESCAPE_WRITING = {.guard = S_guard};
static const markdown_core_service ESCAPE_SERVICES[] = {
    {MARKDOWN_CORE_SERVICE_ESCAPES, &ESCAPE_DECODER}, {MARKDOWN_CORE_SERVICE_ESCAPE_WRITING, &ESCAPE_WRITING}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ESCAPE = {
    .provides = ESCAPE_SERVICES,
    .needs = ESCAPE_NEEDS,
    .state_size = sizeof(markdown_core_escape_work),
    .name = "escape",
    .inlines = &ESCAPE_INLINES,
};
