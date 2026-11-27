#include <assert.h>

#include "alloc.h"
#include "attributes.h"
#include "ast_internal.h"
#include "attention.h"
#include "label.h"
#include "label.h"
#include "inline_internal.h"
#include "../core/attributes.h"
#include "services.h"
#include "node.h"
#include "markdown_core_ctype.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* THE BYTES THE GRAMMAR TELLS APART, as one table. Every scan below runs
 * until a byte of the classes that end it (markdown_core_scan_to_class), so a
 * byte costs one load whatever the number of bytes that could end the run; a
 * byte not listed is an ordinary byte of whatever is being read. */
enum {
    BYTE_SPACE = 1 << 0,     /* space, tab */
    BYTE_NEWLINE = 1 << 1,   /* LF, CR */
    BYTE_CLOSE = 1 << 2,     /* the closing brace */
    BYTE_NAME_END = 1 << 3,  /* what ends a name (name_terminator) */
    BYTE_QUOTE = 1 << 4,     /* either quote */
    BYTE_ESCAPE = 1 << 5,    /* a backslash, which may escape the next byte */
    BYTE_REFERENCE = 1 << 6, /* `&`, which may open a character reference */
    BYTE_ASSIGN = 1 << 7     /* `=` */
};
static const unsigned char BYTE_CLASS[256] = {
    MARKDOWN_CORE_SPACE_OR_TAB_BYTES(BYTE_SPACE | BYTE_NAME_END),
    MARKDOWN_CORE_LINE_END_BYTES(BYTE_NEWLINE | BYTE_NAME_END),
    ['}'] = BYTE_CLOSE | BYTE_NAME_END,
    ['{'] = BYTE_NAME_END,
    ['"'] = BYTE_QUOTE | BYTE_NAME_END,
    ['\''] = BYTE_QUOTE | BYTE_NAME_END,
    ['='] = BYTE_ASSIGN | BYTE_NAME_END,
    ['\\'] = BYTE_ESCAPE,
    ['&'] = BYTE_REFERENCE,
};
/* What ends an unquoted value, and a bare member. */
#define BYTE_VALUE_END (BYTE_SPACE | BYTE_NEWLINE | BYTE_CLOSE)

static int horizontal(unsigned char c) { return BYTE_CLASS[c] & BYTE_SPACE; }
static int newline(unsigned char c) { return BYTE_CLASS[c] & BYTE_NEWLINE; }
static int escaped(const unsigned char *s, bufsize_t n, bufsize_t p) {
    return s[p] == '\\' && p + 1 < n && markdown_core_ispunct(s[p + 1]);
}
/* A NAME IS TAKEN AS WRITTEN, as in an HTML start tag: any byte that is not a
 * separator, the assignment sign, the closing brace, a quote or an opening
 * brace continues it. Nothing is classified by Unicode category -- a lead
 * byte, a continuation byte, a digit and an underscore are all name bytes --
 * so the run is found by one byte test per byte and no decode. `#`, `.` and
 * a lone `-` mean something else only as a member's FIRST byte; inside a name
 * they are name bytes like any other. */
static int name_terminator(unsigned char c) { return BYTE_CLASS[c] & BYTE_NAME_END; }
/* A member boundary: what may follow a bare name or the `unnumbered` dash. */
static int member_boundary(const unsigned char *s, bufsize_t n, bufsize_t p) {
    return p >= n || (BYTE_CLASS[s[p]] & BYTE_VALUE_END);
}

/* RECOGNITION IS A CONSTRUCT, READ CODE BY CODE, MEMOISED BY POSITION
 * (plan symbols-cleanroom.md 23.16.16).
 *
 * The member grammar is regular, so whether a container closes from a
 * boundary depends on the boundary alone: the rest of the input from there.
 * The walk reads one member at a time -- dispatch on its first byte, take the
 * name run, the `=` and the value, land on the next boundary -- and every
 * byte it takes is consumed by the member that owns it. A quoted value is the
 * one place it looks ahead: an opening quote is a value's opener only if an
 * unescaped closer of its kind comes before a blank line (row 9), which the
 * walk attempts (ATTRIBUTES_QUOTED) and, failing, reads the value unquoted.
 *
 * The memo is what keeps the many candidates a reading asks about linear in
 * its bytes rather than in their number. The answer for a boundary `b` is
 * `.end` of its word, for the value opened by the `=` at `e` its
 * `.assignment_end`: 0 not yet asked, INVALID asked and nothing closes,
 * positive one past the closing brace. A walk that reaches a known position
 * takes its answer and moves there. Every boundary and `=` a walk passes
 * gets the same answer, so while the walk is out they hold a LINK to the
 * position pushed before them, encoded below INVALID, and the answer is
 * written back along that chain when it is found -- a stack in the memo
 * itself, allocation-free, and acyclic because a walk only moves forward.
 * The memo is a window of the reading's bytes, from the earliest place a
 * walk began, to the furthest it read: a block reading's bytes are the whole
 * document, and the walks of its lines must cost their own bytes.
 *
 * Why the tiling holds, per byte class: a name run is entered only at its
 * first byte (its terminators are never inside a run); a whitespace run is
 * taken whole from the boundary before it; lookaheads of one quote kind never
 * overlap past their openers, because an opener follows `=` and so is never
 * escaped, and a lookahead of that kind from an earlier opener stops at it; a
 * failing lookahead is the last opener of its kind before a blank line, so
 * there is at most one per kind per stretch; and an unquoted value's walk
 * memoises every `=` it steps over whose value is not quote-led, since the
 * value from there is the same suffix of the same run. The api test
 * `attribute_recognition_is_memoised` holds these on the shapes that defeat a
 * walk without the memo. */
#define INVALID ((bufsize_t) - 1)
#define LINK_NONE ((bufsize_t) - 2)
#define LINK(previous) ((bufsize_t)(-3 - (previous)))
#define LINKED(value) ((bufsize_t)(-3 - (value)))
#define KNOWN(value) ((value) > 0 || (value) == INVALID)

static markdown_core_attribute_parser *S_recognizer(const markdown_core_tokenizer *tokenizer) {
    return (markdown_core_attribute_parser *)markdown_core_self(tokenizer);
}

/* The memo of the reading `tokenizer` reads, from `at` on, where a walk
 * begins: another reading's is cleared, and one that begins before the
 * window or past its end moves it. False when storage could not be had. */
static bool S_window(markdown_core_attribute_parser *p, const markdown_core_tokenizer *tokenizer, bufsize_t at) {
    if (p->reading != tokenizer || p->serial != tokenizer->serial || p->bytes != tokenizer->data ||
        p->size != tokenizer->size) {
        p->reading = tokenizer;
        p->serial = tokenizer->serial;
        p->bytes = tokenizer->data;
        p->size = tokenizer->size;
        p->count = 0;
        p->ran_out = 0;
    }
    if (!p->count || at >= p->base + p->count) {
        /* Nothing of the window is before `at`'s walk, which reads on. */
        p->base = at;
        p->count = 0;
        return true;
    }
    if (at >= p->base) {
        return true;
    }
    const bufsize_t shift = p->base - at, count = p->count + shift;
    if (count > p->room) {
        bufsize_t room = p->room ? p->room : 64;
        while (room < count) {
            room *= 2;
        }
        struct markdown_core_attribute_suffix *ends = markdown_core_realloc(p->ends, (size_t)room * sizeof(*ends));
        if (!ends) {
            return false;
        }
        p->ends = ends;
        p->room = room;
    }
    memmove(p->ends + shift, p->ends, (size_t)p->count * sizeof(*p->ends));
    memset(p->ends, 0, (size_t)shift * sizeof(*p->ends));
    p->base = at;
    p->count = count;
    return true;
}

/* The memo's words of the byte at `at`, the window grown to hold it; NULL
 * when storage could not be had. */
static struct markdown_core_attribute_suffix *S_slot(markdown_core_attribute_parser *p, bufsize_t at) {
    const bufsize_t index = at - p->base;
    if (index >= p->count) {
        if (index >= p->room) {
            bufsize_t room = p->room ? p->room : 64;
            while (room <= index) {
                room *= 2;
            }
            struct markdown_core_attribute_suffix *ends = markdown_core_realloc(p->ends, (size_t)room * sizeof(*ends));
            if (!ends) {
                return NULL;
            }
            p->ends = ends;
            p->room = room;
        }
        memset(p->ends + p->count, 0, (size_t)(index + 1 - p->count) * sizeof(*p->ends));
        p->count = index + 1;
    }
    return &p->ends[index];
}

/* The memo word for a position: an `=` is asked for its value, anything else
 * for the container from it. */
static bufsize_t *S_word(markdown_core_attribute_parser *p, bufsize_t at) {
    struct markdown_core_attribute_suffix *slot = S_slot(p, at);
    if (!slot) {
        return NULL;
    }
    return p->bytes[at] == '=' ? &slot->assignment_end : &slot->end;
}

/* THE WALK'S WORDS: the chain of positions it passed, whether its boundary
 * read a line ending, and whether it is a line's (ATTRIBUTES_LINE). */
enum { WALK_CHAIN, WALK_LINES, WALK_LINE };

static markdown_core_state S_failed(markdown_core_tokenizer *tokenizer) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    p->oom = 1;
    tokenizer->events.failed = true;
    return markdown_core_nok(tokenizer);
}

/* The walk ends with `answer`: written back along its chain, and the walk
 * moved to past the brace when one closes. */
static markdown_core_state S_answer(markdown_core_tokenizer *tokenizer, bufsize_t answer) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    bufsize_t chain = (bufsize_t)markdown_core_frame_local(tokenizer)[WALK_CHAIN];
    while (chain != LINK_NONE) {
        bufsize_t *word = S_word(p, chain), previous = LINKED(*word);
        *word = answer;
        chain = previous;
    }
    if (p->ran_out) {
        markdown_core_met_end(tokenizer);
    }
    if (p->scratch) {
        p->scratch->work += p->work;
        p->work = 0;
    }
    if (answer == INVALID) {
        p->end = 0;
        return markdown_core_nok(tokenizer);
    }
    /* A known answer moves the walk on to it; one the walk read is where
     * it stands. */
    if ((uint32_t)answer > tokenizer->position) {
        markdown_core_consume_to(tokenizer, (uint32_t)answer);
    }
    p->end = answer;
    return markdown_core_ok(tokenizer);
}

/* Pushes `at` on the walk's chain (PUSHED); or its answer, when it is
 * known (KNOWN, in `*known`); or nothing, when the walk passed it already
 * (PASSED); FAILED when storage could not be had. */
enum { PUSHED, KNOWN_AT, PASSED, FAILED };

static int S_push(markdown_core_tokenizer *tokenizer, bufsize_t at, bufsize_t *known) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    uint32_t *words = markdown_core_frame_local(tokenizer);
    bufsize_t *word = S_word(p, at);
    if (!word) {
        return FAILED;
    }
    if (KNOWN(*word)) {
        *known = *word;
        return KNOWN_AT;
    }
    if (*word) {
        return PASSED;
    }
    *word = LINK((bufsize_t)words[WALK_CHAIN]);
    words[WALK_CHAIN] = (uint32_t)at;
    return PUSHED;
}

static markdown_core_state S_bound(markdown_core_tokenizer *tokenizer, int code);

static inline bool S_horizontal(int code) {
    return code >= 0 && code < 0x100 ? horizontal((unsigned char)code)
                                     : code == MARKDOWN_CORE_HT || code == MARKDOWN_CORE_VS;
}
static inline bool S_newline(int code) { return code == '\n' || code == '\r'; }
/* What may follow a bare name or the `unnumbered` dash. */
static inline bool S_boundary(int code) {
    return code == MARKDOWN_CORE_EOF || S_horizontal(code) || S_newline(code) || code == '}';
}
static inline bool S_name_byte(int code) { return code >= 0 && code < 0x100 && !name_terminator((unsigned char)code); }

/* THE UNQUOTED VALUE: up to its first unescaped terminator. */
static markdown_core_state S_unquoted(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_unquoted_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (code >= 0 && code < 0x100 && markdown_core_ispunct(code)) {
        S_recognizer(tokenizer)->work++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_unquoted);
    }
    return S_unquoted(tokenizer, code);
}

/* After an `=` the value steps over: one whose value is not quote-led opens
 * the same value's suffix, and takes this walk's answer. */
static markdown_core_state S_unquoted_sign(markdown_core_tokenizer *tokenizer, int code) {
    if (code != MARKDOWN_CORE_EOF && code != '"' && code != '\'') {
        bufsize_t known;
        if (S_push(tokenizer, (bufsize_t)tokenizer->position - 1, &known) == FAILED) {
            return S_failed(tokenizer);
        }
    }
    return S_unquoted(tokenizer, code);
}

static markdown_core_state S_unquoted(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    if (S_boundary(code)) {
        return S_bound(tokenizer, code);
    }
    p->work++;
    markdown_core_consume(tokenizer);
    if (code == '\\') {
        return markdown_core_state_of(S_unquoted_escape);
    }
    return markdown_core_state_of(code == '=' ? S_unquoted_sign : S_unquoted);
}

/* THE QUOTED VALUE (ATTRIBUTES_QUOTED): its quote, to the first unescaped
 * quote of its kind, before a blank line; one that closes at the end of the
 * bytes is read unquoted, which the input going on undoes. The frame's
 * words: the quote, and whether a line ending was the last thing read. */
enum { QUOTE_BYTE, QUOTE_LINES };

static markdown_core_state S_quoted(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_quoted_close(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF) {
        S_recognizer(tokenizer)->ran_out = 1;
        markdown_core_met_end(tokenizer);
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_quoted_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (code >= 0 && code < 0x100 && markdown_core_ispunct(code)) {
        S_recognizer(tokenizer)->work++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_quoted);
    }
    return S_quoted(tokenizer, code);
}

/* After a CR, the LF of a CRLF decides. */
static markdown_core_state S_quoted_return(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_quoted);
    }
    return S_quoted(tokenizer, code);
}

static markdown_core_state S_quoted(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == MARKDOWN_CORE_EOF) {
        p->ran_out = 1;
        markdown_core_met_end(tokenizer);
        return markdown_core_nok(tokenizer);
    }
    p->work++;
    if (S_newline(code)) {
        /* A quoted value cannot cross a blank line. */
        if (words[QUOTE_LINES]) {
            return markdown_core_nok(tokenizer);
        }
        words[QUOTE_LINES] = 1;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(code == '\r' ? S_quoted_return : S_quoted);
    }
    if (!S_horizontal(code)) {
        words[QUOTE_LINES] = 0;
    }
    markdown_core_consume(tokenizer);
    if (code == (int)words[QUOTE_BYTE]) {
        return markdown_core_state_of(S_quoted_close);
    }
    return markdown_core_state_of(code == '\\' ? S_quoted_escape : S_quoted);
}

static markdown_core_state S_quoted_start(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    words[QUOTE_BYTE] = (uint32_t)code;
    words[QUOTE_LINES] = 0;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_quoted);
}

static const markdown_core_construct ATTRIBUTES_QUOTED = {.start = S_quoted_start};

static markdown_core_state S_value(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '"' || code == '\'') {
        return markdown_core_attempt(tokenizer, &ATTRIBUTES_QUOTED, markdown_core_state_of(S_bound),
                                     markdown_core_state_of(S_unquoted), code);
    }
    return S_unquoted(tokenizer, code);
}

/* The `=` after a name: its value's answer when it is known. */
static markdown_core_state S_sign(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    bufsize_t known;
    const int pushed = S_push(tokenizer, (bufsize_t)tokenizer->position, &known);
    if (pushed == KNOWN_AT) {
        return S_answer(tokenizer, known);
    }
    if (pushed == FAILED) {
        return S_failed(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_value);
}

/* A name. Followed by `=` it is an assignment; followed by a member
 * boundary it is a bare attribute; followed by a quote or an opening
 * brace it is nothing. */
static markdown_core_state S_name(markdown_core_tokenizer *tokenizer, int code) {
    if (S_name_byte(code)) {
        S_recognizer(tokenizer)->work++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_name);
    }
    if (code == '=') {
        return S_sign(tokenizer, code);
    }
    return S_boundary(code) ? S_bound(tokenizer, code) : S_answer(tokenizer, INVALID);
}

/* `#` or `.` and the name after it. */
static markdown_core_state S_marked(markdown_core_tokenizer *tokenizer, int code) {
    if (S_name_byte(code)) {
        S_recognizer(tokenizer)->work++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_marked);
    }
    return S_bound(tokenizer, code);
}

static markdown_core_state S_mark(markdown_core_tokenizer *tokenizer, int code) {
    if (!S_name_byte(code)) {
        if (code == MARKDOWN_CORE_EOF) {
            S_recognizer(tokenizer)->ran_out = 1;
        }
        return S_answer(tokenizer, INVALID);
    }
    return S_marked(tokenizer, code);
}

/* After `-`: alone it is `unnumbered`, else a name begins with it. */
static markdown_core_state S_dash(markdown_core_tokenizer *tokenizer, int code) {
    return S_boundary(code) ? S_bound(tokenizer, code) : S_name(tokenizer, code);
}

/* A member, from its first byte at a boundary. */
static markdown_core_state S_member(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    if (code == MARKDOWN_CORE_EOF) {
        p->ran_out = 1;
        return S_answer(tokenizer, INVALID);
    }
    bufsize_t known;
    const int pushed = S_push(tokenizer, (bufsize_t)tokenizer->position, &known);
    if (pushed == KNOWN_AT) {
        return S_answer(tokenizer, known);
    }
    if (pushed == FAILED) {
        return S_failed(tokenizer);
    }
    p->work++;
    switch (code) {
    case '}':
        markdown_core_consume(tokenizer);
        return S_answer(tokenizer, (bufsize_t)tokenizer->position);
    case '=':
    case '"':
    case '\'':
    case '{':
        return S_answer(tokenizer, INVALID);
    case '-':
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_dash);
    case '#':
    case '.':
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_mark);
    default:
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_name);
    }
}

/* A BOUNDARY: spaces and tabs, and at most one line ending among them, a
 * line's walk none. */
static markdown_core_state S_space(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_space_return(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space);
    }
    return S_space(tokenizer, code);
}

static markdown_core_state S_space(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (S_horizontal(code)) {
        S_recognizer(tokenizer)->work++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space);
    }
    if (S_newline(code)) {
        if (words[WALK_LINES] || words[WALK_LINE]) {
            return S_answer(tokenizer, INVALID);
        }
        words[WALK_LINES] = 1;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(code == '\r' ? S_space_return : S_space);
    }
    return S_member(tokenizer, code);
}

static markdown_core_state S_bound(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_frame_local(tokenizer)[WALK_LINES] = 0;
    S_recognizer(tokenizer)->work++;
    return S_space(tokenizer, code);
}

static markdown_core_state S_open(markdown_core_tokenizer *tokenizer, int code, bool line) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    uint32_t *words = markdown_core_frame_local(tokenizer);
    p->end = 0;
    p->work++;
    /* A block starts before the cut, whose bytes are another's. */
    if (code != '{' || tokenizer->position >= tokenizer->cut) {
        return markdown_core_nok(tokenizer);
    }
    if (p->oom || !S_window(p, tokenizer, (bufsize_t)tokenizer->position + 1)) {
        return S_failed(tokenizer);
    }
    words[WALK_CHAIN] = (uint32_t)LINK_NONE;
    words[WALK_LINE] = line;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_bound);
}

static markdown_core_state S_open_text(markdown_core_tokenizer *tokenizer, int code) {
    return S_open(tokenizer, code, false);
}

static markdown_core_state S_open_line(markdown_core_tokenizer *tokenizer, int code) {
    return S_open(tokenizer, code, true);
}

/* A RECOGNIZER THAT READS ALONE (attributes.h, markdown_core_attributes_end):
 * its tokenizer and its construct, whose context it is; each query a
 * reading of its bytes from the brace asked. */
struct markdown_core_attribute_own {
    markdown_core_tokenizer tokenizer;
    markdown_core_construct list;
};

static markdown_core_state S_alone_done(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_move_to(tokenizer, tokenizer->size);
    return markdown_core_pause(tokenizer, markdown_core_state_of(S_alone_done));
}

static markdown_core_state S_alone_start(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_attribute_parser *p = tokenizer->reader;
    return markdown_core_attempt(tokenizer, &p->own->list, markdown_core_state_of(S_alone_done),
                                 markdown_core_state_of(S_alone_done), code);
}

bufsize_t markdown_core_attributes_end(markdown_core_attribute_parser *p, bufsize_t start) {
    if (p->oom || start < 0 || start >= p->length || p->data[start] != '{') {
        return 0;
    }
    if (!p->own) {
        p->own = markdown_core_alloc(1, sizeof(*p->own));
        if (!p->own) {
            p->oom = 1;
            return 0;
        }
        markdown_core_tokenizer_init(&p->own->tokenizer);
        p->own->list = (markdown_core_construct){.start = S_open_text, .context = p};
    }
    markdown_core_tokenizer *tokenizer = &p->own->tokenizer;
    /* The same bytes read again: the memo is the last reading's. */
    const bool same = p->reading == tokenizer && p->serial == tokenizer->serial && p->bytes == p->data &&
                      p->size == (uint32_t)p->length;
    const markdown_core_read read = {.cut = UINT32_MAX, .reader = p};
    markdown_core_tokenizer_begin(tokenizer, &read, markdown_core_state_of(S_alone_start));
    if (same) {
        p->serial = tokenizer->serial;
    }
    p->end = 0;
    if (!markdown_core_tokenizer_feed(tokenizer, p->data, (uint32_t)p->length, (uint32_t)start) ||
        !markdown_core_tokenizer_end(tokenizer)) {
        p->oom = 1;
        return 0;
    }
    return p->end;
}

/* RELEASES WHAT THE VALUE OWNS, AND NOTHING FOR A VALUE THAT OWNS NOTHING.
 * Every node carries a value and most carry an empty one -- no Text node has
 * attributes -- and the node's release visits each of them, so the empty value
 * is the common call. A value owns its one block and, when a consumer
 * replaced it, its anchor. */
void markdown_core_attributes_free(markdown_core_attributes *v) {
    if (!markdown_core_attributes_owns(v)) {
        return;
    }
    markdown_core_chunk_free(&v->anchor);
    markdown_core_free(v->storage);
    memset(v, 0, sizeof(*v));
}

/* The memo exists only once recognition ran (see the recogniser above): a
 * parser that never asked owns nothing and releases nothing. The scratch is
 * borrowed and released by its owner, and totals the work of every
 * recogniser released into it. */
void markdown_core_attribute_parser_free(markdown_core_attribute_parser *p) {
    if (p->scratch) {
        p->scratch->work += p->work;
    }
    p->work = 0;
    markdown_core_free(p->ends);
    p->ends = NULL;
    p->base = p->count = p->room = 0;
    p->reading = NULL;
    if (p->own) {
        markdown_core_tokenizer_free(&p->own->tokenizer);
        markdown_core_free(p->own);
        p->own = NULL;
    }
}

/* Likewise the scratch: what was never used was never established. */
void markdown_core_attribute_scratch_free(markdown_core_attribute_scratch *w) {
    if (w->strings.ptr) {
        markdown_core_strbuf_free(&w->strings);
        w->strings.ptr = NULL;
    }
    if (w->members) {
        markdown_core_free(w->members);
        w->members = NULL;
        w->member_count = w->member_capacity = 0;
    }
    markdown_core_free(w->places);
    w->places = NULL;
    w->place_count = w->place_capacity = 0;
}

/* STAGING. A container's strings are staged in the scratch as they are read
 * -- a name or `#`/`.` run as written, a value as decoded -- each followed by
 * a NUL, and its classes and records as offsets into them. Offsets rather
 * than pointers, because the strings move as they grow.
 *
 * A staged string is read only once it is known to be there: `stage` returns
 * -1 when the scratch could not take it, its buffer is then poisoned, and the
 * read fails before anything reads the bytes that were not written. */
static bufsize_t stage(markdown_core_attribute_scratch *w, const unsigned char *bytes, bufsize_t length) {
    bufsize_t offset = w->strings.size;
    markdown_core_strbuf_put(&w->strings, bytes, length);
    markdown_core_strbuf_putc(&w->strings, 0);
    return w->strings.oom ? -1 : offset;
}

static int add_member(markdown_core_attribute_scratch *w, bufsize_t name, bufsize_t name_length, bufsize_t value,
                      bufsize_t value_length) {
    struct markdown_core_attribute_member *members =
        markdown_core_reserve(w->members, &w->member_capacity, w->member_count + 1, sizeof(*members));
    if (!members) {
        return 0;
    }
    w->members = members;
    members[w->member_count++] = (struct markdown_core_attribute_member){name, name_length, value, value_length};
    return 1;
}

/* What separates the classes of a `class=` run: HTML's ASCII white space --
 * tab, line feed, form feed, carriage return and space -- which is what HTML
 * splits a `class` attribute on. Vertical tab and every non-ASCII space stay
 * inside a class, whether written or decoded from a character reference. */
static int class_separator(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

/* `class=` names a run of classes: split the staged value at its separators
 * in place, ending each class with a NUL where the separator was. Every
 * separator is one byte below 0x80 and no byte of a longer character is, so
 * the split reads bytes and decodes nothing. */
static int add_class_run(markdown_core_attribute_scratch *w, bufsize_t value, bufsize_t size) {
    unsigned char *text = w->strings.ptr + value;
    bufsize_t word = 0;
    for (bufsize_t at = 0; at < size; at++) {
        if (class_separator(text[at])) {
            if (at > word && !add_member(w, -1, 0, value + word, at - word)) {
                return 0;
            }
            text[at] = 0;
            word = at + 1;
        }
    }
    return size == word || add_member(w, -1, 0, value + word, size - word);
}

/* A record, or the anchor or class run a record named `id` or `class` is. */
static int add_record(markdown_core_attribute_scratch *w, bufsize_t *anchor, bufsize_t *anchor_length,
                      const unsigned char *name, bufsize_t length, bufsize_t value, bufsize_t size) {
    if (length == 2 && memcmp(name, "id", 2) == 0) {
        *anchor = value;
        *anchor_length = size;
        return 1;
    }
    if (length == 5 && memcmp(name, "class", 5) == 0) {
        return add_class_run(w, value, size);
    }
    bufsize_t staged = stage(w, name, length);
    return staged >= 0 && add_member(w, staged, length, value, size);
}

/* DECODE A VALUE INTO THE SCRATCH, ONE WALK. A value's bytes are its own
 * except at a backslash and, in a quoted value, at `&` and at a line ending;
 * everything between two of those is staged whole.
 *
 * A quote-led value is quoted when its closer comes before the container
 * ends, which is the reading recognition took: its quote lookahead found an
 * unescaped closer before a blank line, and a container holds no blank line.
 * So the quoted walk runs to the closer and returns one past it -- or, when
 * the container ends first, returns 0 having kept nothing, and the value is
 * the unquoted one that starts with the quote. An unquoted value holds no
 * line ending (it ends at one) and keeps its references as written. */
/* Where the name run from `at` ends, before `n`. */
static bufsize_t S_name_end(markdown_core_attribute_scratch *w, const unsigned char *s, bufsize_t n, bufsize_t at) {
    const bufsize_t from = at;
    while (at < n && !name_terminator(s[at])) {
        at++;
    }
    w->work += (size_t)(at - from);
    return at;
}

static bufsize_t decode_quoted(markdown_core_attribute_scratch *w, const unsigned char *s, bufsize_t open,
                               bufsize_t finish) {
    const unsigned char quote = s[open];
    const bufsize_t limit = finish - 1, mark = w->strings.size;
    bufsize_t at = open + 1;
    while (at < limit) {
        bufsize_t run = markdown_core_scan_to_class(
            BYTE_CLASS, BYTE_QUOTE | BYTE_ESCAPE | BYTE_REFERENCE | BYTE_NEWLINE, s, at, limit);
        w->work += (size_t)(run - at);
        markdown_core_strbuf_put(&w->strings, s + at, run - at);
        at = run;
        if (at >= limit) {
            break;
        }
        w->work++;
        unsigned char c = s[at];
        if (c == quote) {
            return at + 1;
        }
        if (escaped(s, finish, at)) {
            markdown_core_strbuf_putc(&w->strings, s[at + 1]);
            at += 2;
        } else if (c == '&') {
            bufsize_t used = markdown_core_decoder_read(w->references, &w->strings, s + at + 1, limit - at - 1);
            if (used) {
                at += used + 1;
            } else {
                markdown_core_strbuf_putc(&w->strings, s[at++]);
            }
        } else if (newline(c)) {
            if (c == '\r' && at + 1 < limit && s[at + 1] == '\n') {
                at++;
            }
            at++;
            markdown_core_strbuf_putc(&w->strings, ' ');
        } else {
            markdown_core_strbuf_putc(&w->strings, s[at++]);
        }
    }
    /* Nothing kept. Through the buffer, which writes a terminator only where
     * it wrote bytes: a value read first into a fresh scratch has staged none,
     * and the scratch is then still the shared empty sentinel. */
    markdown_core_strbuf_truncate(&w->strings, mark);
    return 0;
}

static bufsize_t decode_unquoted(markdown_core_attribute_scratch *w, const unsigned char *s, bufsize_t at,
                                 bufsize_t finish) {
    const bufsize_t limit = finish - 1;
    while (at < limit) {
        bufsize_t run = markdown_core_scan_to_class(BYTE_CLASS, BYTE_VALUE_END | BYTE_ESCAPE, s, at, limit);
        w->work += (size_t)(run - at);
        markdown_core_strbuf_put(&w->strings, s + at, run - at);
        at = run;
        if (at >= limit || (BYTE_CLASS[s[at]] & BYTE_VALUE_END)) {
            break;
        }
        w->work++;
        if (escaped(s, finish, at)) {
            markdown_core_strbuf_putc(&w->strings, s[at + 1]);
            at += 2;
        } else {
            markdown_core_strbuf_putc(&w->strings, s[at++]);
        }
    }
    return at;
}

/* A member's bytes from `from` up to `end`, as a field of slot kind `slot`,
 * where the caller has the parse's fields go: a value, empty or not, as
 * `k=` holds one. */
static inline bool S_field(markdown_core_attribute_scratch *w, markdown_core_slot_kind slot, bufsize_t from,
                           bufsize_t end) {
    markdown_core_field_spot *grown =
        markdown_core_reserve(w->places, &w->place_capacity, w->place_count + 1, sizeof(*w->places));
    if (!grown) {
        return false;
    }
    w->places = grown;
    w->places[w->place_count++] =
        (markdown_core_field_spot){from, end, (int)slot, slot == MARKDOWN_CORE_SLOT_VALUE, MARKDOWN_CORE_FIELD};
    return true;
}

/* The field at `spot` in the scratch, an identifier a later one writes
 * over, is of no slot kind: what is written there is no node's identifier. */
static void S_field_void(markdown_core_attribute_scratch *w, bufsize_t spot) {
    if (spot >= 0) {
        w->places[spot].slot = MARKDOWN_CORE_SLOT_NONE;
        w->places[spot].empty = false;
    }
}

/* READ the recognised container `[start, finish)` into the scratch: its
 * fields, the identifiers a later one writes over left out. Returns 0 only
 * on allocation failure. */
static int read_container(markdown_core_attribute_scratch *w, const unsigned char *s, bufsize_t start, bufsize_t finish,
                          bufsize_t *anchor, bufsize_t *anchor_length) {
    bufsize_t at = start + 1;
    if (!w->strings.ptr) {
        markdown_core_strbuf_init(&w->strings, 0);
    }
    markdown_core_strbuf_clear(&w->strings);
    w->member_count = 0;
    w->place_count = 0;
    /* The field of the identifier the members read so far give, or -1. */
    bufsize_t named = -1;
    while (at < finish - 1) {
        w->work++;
        if (BYTE_CLASS[s[at]] & (BYTE_SPACE | BYTE_NEWLINE)) {
            at++;
            continue;
        }
        if (s[at] == '-' && member_boundary(s, finish, at + 1)) {
            bufsize_t staged = stage(w, (const unsigned char *)"unnumbered", 10);
            if (staged < 0 || !add_member(w, -1, 0, staged, 10)) {
                return 0;
            }
            at++;
            continue;
        }
        if (s[at] == '#' || s[at] == '.') {
            unsigned char marker = s[at++];
            bufsize_t from = at;
            at = S_name_end(w, s, finish, at);
            bufsize_t staged = stage(w, s + from, at - from);
            if (staged < 0 || !S_field(w, marker == '#' ? MARKDOWN_CORE_SLOT_ID : MARKDOWN_CORE_SLOT_CLASS, from, at)) {
                return 0;
            }
            if (marker == '#') {
                S_field_void(w, named);
                named = (bufsize_t)w->place_count - 1;
                *anchor = staged;
                *anchor_length = at - from;
            } else if (!add_member(w, -1, 0, staged, at - from)) {
                return 0;
            }
            continue;
        }
        bufsize_t name = at;
        at = S_name_end(w, s, finish, at);
        if (!S_field(w, MARKDOWN_CORE_SLOT_RECORD, name, at)) {
            return 0;
        }
        bufsize_t name_length = at - name, value = w->strings.size;
        const bool id = name_length == 2 && memcmp(s + name, "id", 2) == 0;
        if (id) {
            S_field_void(w, named);
            named = -1;
        }
        if (at >= finish - 1 || s[at] != '=') {
            /* A bare name: recognition admitted it only at a member boundary. */
            bufsize_t staged = stage(w, (const unsigned char *)"true", 4);
            if (staged < 0 || !add_record(w, anchor, anchor_length, s + name, name_length, staged, 4)) {
                return 0;
            }
            continue;
        }
        at++; /* '=' */
        bufsize_t after = BYTE_CLASS[s[at]] & BYTE_QUOTE ? decode_quoted(w, s, at, finish) : 0;
        const bufsize_t from = after ? at + 1 : at;
        at = after ? after : decode_unquoted(w, s, at, finish);
        if (!S_field(w, MARKDOWN_CORE_SLOT_VALUE, from, after ? at - 1 : at)) {
            return 0;
        }
        named = id ? (bufsize_t)w->place_count - 1 : named;
        bufsize_t size = w->strings.size - value;
        markdown_core_strbuf_putc(&w->strings, 0);
        /* A value that lost bytes to a refused allocation is not read. */
        if (w->strings.oom || !add_record(w, anchor, anchor_length, s + name, name_length, value, size)) {
            return 0;
        }
    }
    return 1;
}

/* LAY OUT the scratch as the value: one block, its records, then its
 * classes, then the staged strings, each chunk pointing into the block. The
 * value is written once, whole, when it is complete; a refused allocation
 * leaves `result` as it was. */
static int lay_out(markdown_core_attribute_scratch *w, bufsize_t anchor, bufsize_t anchor_length,
                   markdown_core_attributes *result) {
    size_t classes = 0, records = 0, strings = (size_t)w->strings.size;
    for (size_t i = 0; i < w->member_count; i++) {
        if (w->members[i].name < 0) {
            classes++;
        } else {
            records++;
        }
    }
    size_t bytes = records * sizeof(markdown_core_record) + classes * sizeof(markdown_core_chunk) + strings;
    if (!bytes) {
        *result = (markdown_core_attributes){0};
        return 1;
    }
    void *storage = markdown_core_realloc(NULL, bytes);
    if (!storage) {
        return 0;
    }
    markdown_core_record *const first_record = storage, *record = first_record;
    markdown_core_chunk *const first_class = (markdown_core_chunk *)(record + records), *class = first_class;
    unsigned char *text = (unsigned char *)(class + classes);
    memcpy(text, w->strings.ptr, strings);
    for (size_t i = 0; i < w->member_count; i++) {
        const struct markdown_core_attribute_member *member = &w->members[i];
        markdown_core_chunk staged = {text + member->value, member->value_length, 0};
        if (member->name < 0) {
            *class ++= staged;
        } else {
            *record++ = (markdown_core_record){{text + member->name, member->name_length, 0}, staged};
        }
    }
    *result = (markdown_core_attributes){
        .anchor = anchor >= 0 ? (markdown_core_chunk){text + anchor, anchor_length, 0} : (markdown_core_chunk){0},
        .classes = classes ? first_class : NULL,
        .records = records ? first_record : NULL,
        .class_count = (uint32_t)classes,
        .record_count = (uint32_t)records,
        .storage = storage,
    };
    return 1;
}

int markdown_core_attributes_single_class(markdown_core_attributes *value, const unsigned char *bytes,
                                          bufsize_t length) {
    markdown_core_chunk *class = markdown_core_realloc(NULL, sizeof(*class) + (size_t)length + 1);
    if (!class) {
        return 0;
    }
    unsigned char *text = (unsigned char *)(class + 1);
    if (length) {
        memcpy(text, bytes, (size_t)length);
    }
    text[length] = 0;
    *class = (markdown_core_chunk){text, length, 0};
    value->storage = class;
    value->classes = class;
    value->class_count = 1;
    return 1;
}

void markdown_core_attributes_fields(const markdown_core_attribute_scratch *w, const markdown_core_field_to *to) {
    if (w) {
        markdown_core_field_many(to, w->places, w->place_count);
    }
}

bool markdown_core_attributes_decode(markdown_core_attribute_scratch *w, const unsigned char *data, bufsize_t start,
                                     bufsize_t end, markdown_core_attributes *result) {
    bufsize_t anchor = -1, anchor_length = 0;
    if (!read_container(w, data, start, end, &anchor, &anchor_length) || !lay_out(w, anchor, anchor_length, result)) {
        return false;
    }
    w->work += (size_t)(end - start);
    return true;
}

int markdown_core_attributes_parse(markdown_core_attribute_parser *p, bufsize_t start, markdown_core_attributes *result,
                                   bufsize_t *end) {
    const bufsize_t finish = markdown_core_attributes_end(p, start);
    if (!finish) {
        return 0;
    }
    assert(p->scratch);
    if (!markdown_core_attributes_decode(p->scratch, p->data, start, finish, result)) {
        p->oom = 1;
        return 0;
    }
    *end = finish;
    return 1;
}

#include "inline_internal.h"
#include "block_internal.h"

static bufsize_t value_end(const unsigned char *data, bufsize_t length) {
    bufsize_t at = 0;
    while ((at = markdown_core_scan_to_class(BYTE_CLASS, BYTE_VALUE_END | BYTE_ESCAPE, data, at, length)) < length &&
           data[at] == '\\') {
        at += escaped(data, length, at) ? 2 : 1;
    }
    return at < length ? at : length;
}
static bufsize_t quoted_end(const unsigned char *data, bufsize_t length, unsigned char quote) {
    bufsize_t at = 0;
    while (at < length && data[at] != quote) {
        at += escaped(data, length, at) ? 2 : 1;
    }
    return at < length ? at : length;
}

/* The fields of the members of a block that does not close, as they are
 * read, in `local` until they outgrow it: storage that could not be had
 * is `failed`. */
#define MEMBERS_LOCAL 16
typedef struct {
    markdown_core_field *items;
    size_t count, room;
    bool failed;
    markdown_core_field local[MEMBERS_LOCAL];
} S_members;

static void S_member_put(S_members *members, markdown_core_slot_kind slot, bufsize_t start, bufsize_t stop) {
    markdown_core_field *items = members->items;
    if (!items) {
        items = members->items = members->local;
        members->room = MEMBERS_LOCAL;
    } else if (members->count == members->room) {
        markdown_core_field *grown = markdown_core_alloc(members->room * 2, sizeof(*grown));
        if (!grown) {
            members->failed = true;
            return;
        }
        memcpy(grown, items, members->count * sizeof(*grown));
        if (items != members->local) {
            markdown_core_free(items);
        }
        items = members->items = grown;
        members->room *= 2;
    }
    items[members->count++] = (markdown_core_field){slot, MARKDOWN_CORE_KIND_NONE, start, stop};
}

enum { MEMBER_FAIL, MEMBER_END, MEMBER_NEXT };

/* The last value of each kind a walk read, unquoted, in double quotes or in
 * single quotes, from `from` to `end`: a value of that kind that begins
 * inside it ends where it did, since nothing between ends one. The walks
 * from each brace of a line share them, as the walks from each block an
 * inline run gives up do, so a line of open blocks reads each value byte at
 * most once for each kind (markdown_core_attributes_report). An end of 0 is
 * none read yet: zeroed, it holds none. */
typedef struct {
    bufsize_t from[3], end[3];
} S_value_seen;

static bufsize_t S_value_end(const unsigned char *s, bufsize_t n, bufsize_t v, unsigned char quote,
                             S_value_seen *seen) {
    const int kind = quote == '"' ? 1 : quote == '\'' ? 2 : 0;
    if (seen && seen->end[kind] > 0 && v >= seen->from[kind] && v <= seen->end[kind]) {
        return seen->end[kind];
    }
    const bufsize_t end = quote ? v + quoted_end(s + v, n - v, quote) : v + value_end(s + v, n - v);
    if (seen) {
        seen->from[kind] = v;
        seen->end[kind] = end;
    }
    return end;
}

/* One member of a block that does not close, read from the boundary `b`:
 * its fields, as the block's would be once it closed, a quoted value's
 * inside its quotes, each to where its reading stopped, and the boundary
 * after it; or, where the bytes end, a name not yet begun there; or neither
 * when the block is no block whatever follows. */
static int member_step(const unsigned char *s, bufsize_t n, bufsize_t b, bufsize_t *next, S_members *members,
                       S_value_seen *seen) {
    bool lines = false;
    while (b < n && (horizontal(s[b]) || newline(s[b]))) {
        if (newline(s[b])) {
            if (lines && !(s[b] == '\n' && s[b - 1] == '\r')) {
                return MEMBER_FAIL;
            }
            lines = true;
        }
        b++;
    }
    if (b >= n) {
        S_member_put(members, MARKDOWN_CORE_SLOT_RECORD, b, b);
        return MEMBER_END;
    }
    const unsigned char c = s[b];
    if (c == '}' || c == '=' || c == '"' || c == '\'' || c == '{') {
        return MEMBER_FAIL;
    }
    if (c == '-' && b + 1 < n && member_boundary(s, n, b + 1)) {
        *next = b + 1;
        return MEMBER_NEXT;
    }
    if (c == '#' || c == '.') {
        if (b + 1 < n && name_terminator(s[b + 1])) {
            return MEMBER_FAIL;
        }
        bufsize_t e = b + 1;
        while (e < n && !name_terminator(s[e])) {
            e++;
        }
        S_member_put(members, c == '#' ? MARKDOWN_CORE_SLOT_ID : MARKDOWN_CORE_SLOT_CLASS, b + 1, e);
        *next = e;
        return e >= n ? MEMBER_END : MEMBER_NEXT;
    }
    bufsize_t e = b + 1;
    while (e < n && !name_terminator(s[e])) {
        e++;
    }
    if (e < n && s[e] != '=' && !member_boundary(s, n, e)) {
        return MEMBER_FAIL;
    }
    S_member_put(members, MARKDOWN_CORE_SLOT_RECORD, b, e);
    if (e >= n || s[e] != '=') {
        *next = e;
        return e >= n ? MEMBER_END : MEMBER_NEXT;
    }
    const bool quoted = e + 1 < n && (s[e + 1] == '"' || s[e + 1] == '\'');
    const bufsize_t v = e + 1 + quoted;
    const bufsize_t close = S_value_end(s, n, v, quoted ? s[v - 1] : 0, seen);
    S_member_put(members, MARKDOWN_CORE_SLOT_VALUE, v, close);
    if (close >= n) {
        return MEMBER_END;
    }
    *next = quoted ? close + 1 : close;
    return MEMBER_NEXT;
}

/* The members of the block at `start` that does not close, of a construct
 * of kind `owner`, read into `members`, none unless the content's end cuts
 * the block short. */
static void S_members_read(S_members *members, markdown_core_node_kind owner, const unsigned char *s, bufsize_t n,
                           bufsize_t start, S_value_seen *seen) {
    members->items = NULL;
    members->count = members->room = 0;
    members->failed = false;
    bufsize_t b = start + 1, next = 0;
    int step;
    while ((step = member_step(s, n, b, &next, members, seen)) == MEMBER_NEXT) {
        b = next;
    }
    if (step != MEMBER_END) {
        members->count = 0;
    }
    for (size_t i = 0; i < members->count; i++) {
        members->items[i].owner = owner;
    }
}

static bool S_members_open(const markdown_core_field_to *to, markdown_core_node_kind owner, bool line,
                           const unsigned char *s, bufsize_t n, bufsize_t start, S_value_seen *seen) {
    S_members members;
    S_members_read(&members, owner, s, n, start, seen);
    if (members.failed && to->parser) {
        markdown_core_parser_fail(to->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    } else if (members.count) {
        markdown_core_field_opener(to, MARKDOWN_CORE_KIND_NONE, line, start, start + 1, members.items, members.count);
    }
    if (members.items != members.local) {
        markdown_core_free(members.items);
    }
    return members.count > 0;
}

bool markdown_core_attributes_open(const markdown_core_field_to *to, markdown_core_node_kind owner, bool line,
                                   const unsigned char *s, bufsize_t n, bufsize_t start) {
    return S_members_open(to, owner, line, s, n, start, NULL);
}

/* WHAT A RUN HOLDS OF THE ATTRIBUTE ELEMENT: one plus the start of the
 * last block given up (services.h, `give`), whose follow then reports it
 * no more, or 0; one plus the start of the block a span's tail read, and
 * where that block closes, 0 when it did not (S_span_tail); and the values
 * the walks of the blocks it gave up read. */
typedef struct {
    bufsize_t asked;
    bufsize_t tail, tail_end;
    S_value_seen seen;
} S_run;

static S_run *S_run_of(const markdown_core_element_instance *self, const markdown_core_inline_state *inline_state) {
    return markdown_core_run_state(inline_state, self);
}

/* WHAT A PARSE HOLDS OF IT: the workspace every value of the parse is read
 * into before it is laid out; the recognizer every block of the parse is
 * read with, and its constructs, the recognizer their context: the block
 * of a text (ATTRIBUTES_TEXT), of a line (ATTRIBUTES_LINE), and a line's
 * tail (S_tail). */
enum { LIST_TEXT, LIST_LINE, LIST_TAIL, LISTS };
typedef struct {
    markdown_core_attribute_scratch scratch;
    markdown_core_attribute_parser recognizer;
    markdown_core_construct lists[LISTS];
    /* Where the brace of the last line's tail read is. */
    bufsize_t brace;
} S_state;

/* The services this element asks, as `self->needs` holds them. */
enum { ATTRIBUTES_REFERENCES };
static const char *const ATTRIBUTES_NEEDS[] = {[ATTRIBUTES_REFERENCES] = MARKDOWN_CORE_SERVICE_REFERENCES, NULL};

markdown_core_attribute_scratch *markdown_core_attributes_scratch(const markdown_core_element_instance *self) {
    markdown_core_attribute_scratch *scratch = &((S_state *)self->state)->scratch;
    scratch->references = self->needs[ATTRIBUTES_REFERENCES];
    return scratch;
}

static markdown_core_state S_tail(markdown_core_tokenizer *tokenizer, int code);

/* The construct `which` of the parse's recognizer. */
static const markdown_core_construct *S_list(const markdown_core_element_instance *self, int which) {
    S_state *state = self->state;
    if (!state->lists[which].start) {
        state->recognizer.scratch = markdown_core_attributes_scratch(self);
        static markdown_core_state (*const starts[LISTS])(markdown_core_tokenizer *, int) = {
            [LIST_TEXT] = S_open_text, [LIST_LINE] = S_open_line, [LIST_TAIL] = S_tail};
        for (int at = 0; at < LISTS; at++) {
            state->lists[at] = (markdown_core_construct){.start = starts[at], .context = &state->recognizer};
        }
    }
    return &state->lists[which];
}

static void dispose_parser(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)parser;
    S_state *state = self->state;
    markdown_core_attribute_parser_free(&state->recognizer);
    markdown_core_attribute_scratch_free(markdown_core_attributes_scratch(self));
}

/* The block at `start` of an inline reading, of a construct of kind
 * `owner`, that does not close: given up with the members it read. */
static void S_give(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer, bufsize_t start,
                   markdown_core_node_kind owner) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    S_run *run = S_run_of(self, inline_state);
    run->asked = start + 1;
    if (inline_state->error) {
        return;
    }
    S_members members;
    S_members_read(&members, owner, tokenizer->data, (bufsize_t)tokenizer->size, start, &run->seen);
    if (members.failed) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    } else if (members.count) {
        markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_NONE, start, start + 1, members.items, members.count);
    }
    if (members.items != members.local) {
        markdown_core_free(members.items);
    }
}

/* The value of the block from `start` to `end` into `node`, and its
 * fields, as compile makes the node it follows or the span it closes. */
static void S_value_into(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                         const unsigned char *data, markdown_core_node *node, bufsize_t start, bufsize_t end) {
    markdown_core_attribute_scratch *scratch = markdown_core_attributes_scratch(self);
    if (!markdown_core_attributes_decode(scratch, data, start, end, &node->attributes)) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    const markdown_core_field_to to = markdown_core_inline_fields(inline_state, node);
    markdown_core_attributes_fields(scratch, &to);
}

/* A block right after a node a later token may follow, of a kind the
 * dialect gives this element to follow (S_follows): a FOLLOW token
 * over it, which compile gives the node before. */
enum { ATTRIBUTES_FOLLOW, ATTRIBUTES_SPAN, ATTRIBUTES_KINDS };

static bool S_follows(const markdown_core_element_instance *self, const markdown_core_tokenizer *tokenizer) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_node_type kind = markdown_core_text_made_at(tokenizer);
    return kind != MARKDOWN_CORE_NODE_NONE &&
           markdown_core_dialect_kind(inline_state->dialect, kind)->follower[MARKDOWN_CORE_FOLLOW_TOKEN] == self;
}

/* The follow's words: where its block starts, and the kind of the node
 * it follows. */
enum { FOLLOW_START, FOLLOW_KIND };

static markdown_core_state S_follow_read(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + ATTRIBUTES_FOLLOW));
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_follow_lost(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t *words = markdown_core_frame_local(tokenizer);
    const bufsize_t start = (bufsize_t)words[FOLLOW_START];
    if (S_run_of(self, tokenizer->reader)->asked != start + 1) {
        S_give(self, tokenizer, start, markdown_core_kind_public((uint16_t)words[FOLLOW_KIND]));
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_follow_start(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    if (!S_follows(self, tokenizer)) {
        return markdown_core_nok(tokenizer);
    }
    uint32_t *words = markdown_core_frame_local(tokenizer);
    words[FOLLOW_START] = tokenizer->position;
    words[FOLLOW_KIND] = (uint32_t)markdown_core_text_made_at(tokenizer);
    markdown_core_enter(tokenizer, (uint16_t)(self->token_base + ATTRIBUTES_FOLLOW));
    return markdown_core_attempt(tokenizer, S_list(self, LIST_TEXT), markdown_core_state_of(S_follow_read),
                                 markdown_core_state_of(S_follow_lost), code);
}

/* No settle point between a node a later token may follow and a `{`
 * after it: the block may yet close as the input goes on (holds). */
static bool S_follow_holds(const markdown_core_tokenizer *tokenizer, const void *context) {
    return tokenizer->position < tokenizer->size && tokenizer->data[tokenizer->position] == '{' &&
           S_follows(context, tokenizer);
}

/* A BRACKETED SPAN (element.h, markdown_core_close_rule): a label no
 * explicit link closes, whose `]` an attribute block follows, is a span of
 * its body, before a shortcut link may take it. */
static markdown_core_state S_span_read(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    S_run_of(self, tokenizer->reader)->tail_end = (bufsize_t)tokenizer->position;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_refuse(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

/* THE SPAN'S TAIL (element.h, close_rule `tail`): the block after the
 * `]`, where it closes kept in the run's record by where it starts. */
static markdown_core_state S_span_tail(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    S_run *run = S_run_of(self, tokenizer->reader);
    /* No block starts there: none is kept. */
    run->tail = code == '{' ? (bufsize_t)tokenizer->position + 1 : 0;
    run->tail_end = 0;
    if (code != '{') {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_attempt(tokenizer, S_list(self, LIST_TEXT), markdown_core_state_of(S_span_read),
                                 markdown_core_state_of(S_refuse), code);
}

static const markdown_core_construct ATTRIBUTES_SPAN_TAIL = {.start = S_span_tail};

static markdown_core_bracket_match S_end_span(const markdown_core_element_instance *self,
                                              markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                              markdown_core_label_made *made) {
    const markdown_core_opener opener = *label;
    const bufsize_t start = (bufsize_t)made->past;
    const S_run *run = S_run_of(self, tokenizer->reader);
    if (run->tail != start + 1) {
        return BRACKET_UNMATCHED;
    }
    const bufsize_t end = run->tail_end;
    if (!end) {
        S_give(self, tokenizer, start, MARKDOWN_CORE_KIND_SPAN);
        return BRACKET_UNMATCHED;
    }
    if (!markdown_core_attention_allows(tokenizer, MARKDOWN_CORE_NODE_NONE, MARKDOWN_CORE_NODE_SPAN)) {
        return BRACKET_REJECTED;
    }
    made->kind = (uint16_t)(self->token_base + ATTRIBUTES_SPAN);
    made->data = (uint32_t)start;
    made->from = opener.start + opener.shape->kept;
    made->past = (uint32_t)end;
    made->holder = MARKDOWN_CORE_NODE_NONE;
    return BRACKET_MATCHED;
}

/* COMPILE: the block a FOLLOW token holds is the node's before it, which
 * reaches to its `}`; a span holds its body. */
static void S_follow_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_node *node = markdown_core_scaffold_last(markdown_core_compile_parent(compiler));
    if (!node) {
        return;
    }
    const uint32_t place = node->place.start;
    markdown_core_inline_state_place(inline_state, node, (int)token->end - 1, (int)token->end - 1);
    node->place.start = place;
    S_value_into(self, inline_state, markdown_core_inline_window(inline_state, 0).data, node, (bufsize_t)token->start,
                 (bufsize_t)token->end);
}

static void S_span_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_node *inl = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_SPAN);
    if (!inl) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    markdown_core_inline_state_place(inline_state, inl, (int)token->start, (int)token->end - 1);
    S_value_into(self, inline_state, markdown_core_inline_window(inline_state, 0).data, inl, (bufsize_t)token->data,
                 (bufsize_t)token->end);
    markdown_core_compile_open(compiler, inl);
}

static void S_span_close(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_compile_pop(compiler);
}

static const markdown_core_text_construct ATTRIBUTES_CONSTRUCTS[] = {
    {"{", {.start = S_follow_start, .holds = S_follow_holds}}};
static const markdown_core_compile_ops ATTRIBUTES_TOKENS[ATTRIBUTES_KINDS] = {
    [ATTRIBUTES_FOLLOW] = {.exit = S_follow_made},
    [ATTRIBUTES_SPAN] = {.enter = S_span_open, .exit = S_span_close, .asked = true}};

/* The blocks of the line's bytes `from` up to `end` that do not close,
 * of the braces before `last`. */
static bool S_report_open(const markdown_core_field_to *to, markdown_core_node_kind owner, const unsigned char *line,
                          bufsize_t from, bufsize_t last, bufsize_t end) {
    bool open = false;
    S_value_seen seen = {{0}, {0}};
    /* Each brace a backslash does not escape: an odd run of them before
     * it, from `from`, ends with one that does. */
    for (const unsigned char *brace = memchr(line + from, '{', (size_t)(last - from)); brace;
         brace = memchr(brace + 1, '{', (size_t)(line + last - brace - 1))) {
        const bufsize_t at = (bufsize_t)(brace - line);
        bufsize_t run = 0;
        while (at - run > from && line[at - run - 1] == '\\') {
            run++;
        }
        if (!(run & 1)) {
            open = S_members_open(to, owner, true, line, end, at, &seen) || open;
        }
    }
    return open;
}

bool markdown_core_attributes_report(const markdown_core_element_instance *self, const markdown_core_field_to *to,
                                     markdown_core_node_kind owner, const unsigned char *line, bufsize_t from,
                                     bufsize_t end) {
    (void)self;
    return S_report_open(to, owner, line, from, end, end);
}

/* A LINE'S TAIL READ (S_follow_tail), with the parse's recognizer: from
 * the bytes' start, each `{` no backslash escapes, until one whose block
 * closes at the bytes' end, where the reading succeeds. */
static S_state *S_state_of(const markdown_core_attribute_parser *p) {
    return (S_state *)(void *)((unsigned char *)(uintptr_t)p - offsetof(S_state, recognizer));
}

static const markdown_core_construct *S_text_of(const markdown_core_attribute_parser *p) {
    return &S_state_of(p)->lists[LIST_TEXT];
}

static markdown_core_state S_tail_at(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    if (p->end == (bufsize_t)tokenizer->size) {
        S_state_of(p)->brace = (bufsize_t)tokenizer->position;
        return markdown_core_ok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    (void)code;
    return markdown_core_state_of(S_tail);
}

static markdown_core_state S_tail_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (code >= 0 && code < 0x100 && markdown_core_ispunct(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_tail);
    }
    return S_tail(tokenizer, code);
}

static markdown_core_state S_tail(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_attribute_parser *p = S_recognizer(tokenizer);
    if (code == MARKDOWN_CORE_EOF) {
        return markdown_core_nok(tokenizer);
    }
    p->work++;
    if (code == '{') {
        return markdown_core_check(tokenizer, S_text_of(p), markdown_core_state_of(S_tail_at),
                                   markdown_core_state_of(S_tail_at), code);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(code == '\\' ? S_tail_escape : S_tail);
}

/* A LINE'S ATTRIBUTE TAIL (element.h, MARKDOWN_CORE_FOLLOW_TAIL): an
 * attribute block that ends the line, after the spaces and tabs before it,
 * is the block's, taken with those spaces. */
static bufsize_t S_follow_tail(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_lead *lead) {
    markdown_core_node *node = lead->node;
    const unsigned char *line = lead->line;
    const bufsize_t from = lead->from, to = lead->to;
    bufsize_t end = to;
    while (end > from && markdown_core_is_whitespace(line[end - 1])) {
        end--;
    }
    const markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    if (end == from || line[end - 1] != '}') {
        S_report_open(&lead->fields, kind, line, from, end, end);
        return 0;
    }
    /* A tail is on the last line of what is offered. */
    bufsize_t first = end;
    while (first > from && !markdown_core_is_line_end(line[first - 1])) {
        first--;
    }
    markdown_core_alone alone;
    const markdown_core_construct *tail = S_list(self, LIST_TAIL);
    bufsize_t start = -1;
    if (markdown_core_alone_read(parser, &alone, tail, line + first, (uint32_t)(end - first)) && alone.found) {
        start = first + ((S_state *)self->state)->brace;
    }
    markdown_core_alone_end(parser, &alone);
    /* The braces of the tail are its own, which closes. */
    S_report_open(&lead->fields, kind, line, from, start >= 0 ? start : end, end);
    if (start < 0) {
        return 0;
    }
    markdown_core_attribute_scratch *scratch = markdown_core_attributes_scratch(self);
    if (!markdown_core_attributes_decode(scratch, line, start, end, &node->attributes)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return 0;
    }
    markdown_core_attributes_fields(scratch, &lead->fields);
    bufsize_t at = start;
    while (at > first && markdown_core_is_space_or_tab(line[at - 1])) {
        at--;
    }
    return to - at;
}

/* A token of the block's inlines crossed into the tail: it is text. */
static void S_untake(const markdown_core_element_instance *self, markdown_core_parser *parser,
                     markdown_core_node *node) {
    (void)self;
    (void)parser;
    markdown_core_attributes_free(&node->attributes);
    memset(&node->attributes, 0, sizeof(node->attributes));
    node->cut_back = 0;
    markdown_core_node_drop_slots(node, 1u << MARKDOWN_CORE_SLOT_ID | 1u << MARKDOWN_CORE_SLOT_CLASS |
                                            1u << MARKDOWN_CORE_SLOT_RECORD | 1u << MARKDOWN_CORE_SLOT_VALUE);
}

/* The nodes an attribute block follows: inline, those of the constructs
 * that say a later token may follow them (markdown_core_text_made), whose
 * follower's construct reads the block (S_follow_start); at a
 * line's end, the blocks that offer it. A heading's inline tokens win over
 * its tail. */
static const markdown_core_node_type ATTRIBUTES_TOKEN_KINDS[] = {MARKDOWN_CORE_NODE_CODE, MARKDOWN_CORE_NODE_LINK,
                                                                 MARKDOWN_CORE_NODE_EMBEDDED,
                                                                 MARKDOWN_CORE_NODE_DIRECTIVE, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type ATTRIBUTES_TAIL_KINDS[] = {MARKDOWN_CORE_NODE_HEADING,
                                                                MARKDOWN_CORE_NODE_CODE_BLOCK, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_follow ATTRIBUTES_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_TOKEN, .kinds = ATTRIBUTES_TOKEN_KINDS},
    {.at = MARKDOWN_CORE_FOLLOW_TAIL,
     .kinds = ATTRIBUTES_TAIL_KINDS,
     .follow = S_follow_tail,
     .yields = true,
     .undo = S_untake},
    {0},
};

/* A span sits between a link whose destination, title or label follows the
 * `]` (precedence -20) and a shortcut link (10). */
static const markdown_core_close_rule ATTRIBUTES_CLOSES[] = {
    {.precedence = -10, .tail = &ATTRIBUTES_SPAN_TAIL, .end = S_end_span},
};

static const markdown_core_inline_ops ATTRIBUTES_INLINES = {
    .constructs = ATTRIBUTES_CONSTRUCTS,
    .construct_count = sizeof(ATTRIBUTES_CONSTRUCTS) / sizeof(ATTRIBUTES_CONSTRUCTS[0]),
    .tokens = ATTRIBUTES_TOKENS,
    .token_count = ATTRIBUTES_KINDS,
    .closes = ATTRIBUTES_CLOSES,
    .close_count = sizeof(ATTRIBUTES_CLOSES) / sizeof(ATTRIBUTES_CLOSES[0]),
    .closer = ']',
};

const markdown_core_facet MARKDOWN_CORE_ATTRIBUTES_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_SPAN, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

/* THE SERVICE (services.h, MARKDOWN_CORE_SERVICE_ATTRIBUTES): the grammar
 * above, for the elements whose constructs take a block. */
static const markdown_core_construct *S_list_of(const markdown_core_element_instance *self, bool line) {
    return S_list(self, line ? LIST_LINE : LIST_TEXT);
}

static bool S_parse(const markdown_core_element_instance *self, const unsigned char *data, bufsize_t start,
                    bufsize_t end, markdown_core_attributes *value) {
    return markdown_core_attributes_decode(markdown_core_attributes_scratch(self), data, start, end, value);
}

static void S_fields(const markdown_core_element_instance *self, const markdown_core_field_to *to) {
    markdown_core_attributes_fields(markdown_core_attributes_scratch(self), to);
}

static bool S_open_block(const markdown_core_element_instance *self, const markdown_core_field_to *to,
                         markdown_core_node_kind owner, const unsigned char *data, bufsize_t size, bufsize_t start) {
    (void)self;
    return markdown_core_attributes_open(to, owner, true, data, size, start);
}

static const markdown_core_attribute_ops ATTRIBUTES_OPS = {
    .list = S_list_of,
    .parse = S_parse,
    .fields = S_fields,
    .give = S_give,
    .report = markdown_core_attributes_report,
    .open = S_open_block,
};
static const markdown_core_service ATTRIBUTES_SERVICES[] = {{MARKDOWN_CORE_SERVICE_ATTRIBUTES, &ATTRIBUTES_OPS}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES = {
    .name = "attributes",
    .provides = ATTRIBUTES_SERVICES,
    .needs = ATTRIBUTES_NEEDS,
    .state_size = sizeof(S_state),
    .run_state_size = sizeof(S_run),
    .dispose_parser = dispose_parser,
    .follows = ATTRIBUTES_FOLLOWS,
    .facets = MARKDOWN_CORE_ATTRIBUTES_FACETS,
    .inlines = &ATTRIBUTES_INLINES,
};
