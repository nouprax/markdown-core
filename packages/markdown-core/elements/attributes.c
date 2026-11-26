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

/* RECOGNITION IS A FORWARD WALK FROM THE QUERIED BRACE, MEMOISED BY POSITION.
 *
 * The member grammar is regular, so whether a container closes from a
 * boundary depends on the boundary alone: the rest of the input from there.
 * The walk reads one member at a time -- dispatch on its first byte, take the
 * name run, the `=` and the value, land on the next boundary -- and every
 * byte it takes is consumed by the member that owns it. A quoted value is the
 * one place it looks ahead: an opening quote is a value's opener only if an
 * unescaped closer of its kind comes before a blank line (row 9), and a
 * forward scan cannot know that at the `=`.
 *
 * The memo is what keeps the many candidates an extent is asked about linear
 * in the extent rather than in their number. `ends[b].end` is the answer for
 * a boundary `b`, `ends[e].assignment_end` the answer for the value opened by
 * the `=` at `e`: 0 not yet asked, INVALID asked and nothing closes, positive
 * one past the closing brace. A walk that reaches a known position takes its
 * answer and stops. Every boundary and `=` a walk passes gets the same answer,
 * so while the walk is out they hold a LINK to the position pushed before
 * them, encoded below INVALID, and the answer is written back along that
 * chain when it is found -- a stack in the memo itself, allocation-free, and
 * acyclic because a walk only moves forward.
 *
 * Why the tiling holds, per byte class: a name run is entered only at its
 * first byte (its terminators are never inside a run); a whitespace run is
 * taken whole from the boundary before it; lookaheads of one quote kind never
 * overlap past their openers, because an opener follows `=` and so is never
 * escaped, and a lookahead of that kind from an earlier opener stops at it; a
 * failing lookahead is the last opener of its kind before a blank line, so
 * there is at most one per kind per stretch; and an unquoted value's scan
 * memoises every `=` it steps over whose value is not quote-led, since the
 * value from there is the same suffix of the same run. The api test
 * `attribute_recognition_is_memoised` holds these on the shapes that defeat a
 * scan without the memo. */
#define INVALID ((bufsize_t) - 1)
#define LINK_NONE ((bufsize_t) - 2)
#define LINK(previous) ((bufsize_t)(-3 - (previous)))
#define LINKED(value) ((bufsize_t)(-3 - (value)))
#define KNOWN(value) ((value) > 0 || (value) == INVALID)

static int memo_ready(markdown_core_attribute_parser *p) {
    if (p->ends) {
        return 1;
    }
    if ((size_t)p->length + 1 > SIZE_MAX / sizeof(*p->ends)) {
        return 0;
    }
    p->ends = markdown_core_alloc((size_t)p->length + 1, sizeof(*p->ends));
    return p->ends != NULL;
}

/* The memo word for a position: an `=` is asked for its value, anything else
 * for the container from it. */
static bufsize_t *memo(markdown_core_attribute_parser *p, bufsize_t at) {
    return p->data[at] == '=' ? &p->ends[at].assignment_end : &p->ends[at].end;
}

static void push(markdown_core_attribute_parser *p, bufsize_t *chain, bufsize_t at) {
    *memo(p, at) = LINK(*chain);
    *chain = at;
}

static bufsize_t scan_name(markdown_core_attribute_parser *p, bufsize_t n, bufsize_t at) {
    const unsigned char *s = p->data;
    const bufsize_t from = at;
    while (at < n && !name_terminator(s[at])) {
        at++;
    }
    p->work += (size_t)(at - from);
    return at;
}

/* The closer of the quote at `open`: the first unescaped quote of its kind
 * after it, or n when a blank line or the end comes first. */
static bufsize_t quote_close(markdown_core_attribute_parser *p, bufsize_t open) {
    const unsigned char *s = p->data;
    bufsize_t n = p->length, at = open + 1;
    unsigned char quote = s[open];
    while (at < n) {
        bufsize_t run = markdown_core_scan_to_class(BYTE_CLASS, BYTE_QUOTE | BYTE_ESCAPE | BYTE_NEWLINE, s, at, n);
        p->work += (size_t)(run - at);
        at = run;
        if (at >= n) {
            break;
        }
        unsigned char c = s[at];
        p->work++;
        if (c == quote) {
            return at;
        }
        if (escaped(s, n, at)) {
            at += 2;
        } else if (newline(c)) {
            if (c == '\r' && at + 1 < n && s[at + 1] == '\n') {
                at++; /* The LF of a CRLF decides. */
            }
            for (at++; at < n && horizontal(s[at]); at++) {
                p->work++;
            }
            if (at < n && newline(s[at])) {
                return n; /* A quoted value cannot cross a blank line. */
            }
        } else {
            at++;
        }
    }
    p->ran_out = 1;
    return n;
}

/* The end of the unquoted value starting at `at`: its first unescaped
 * terminator, or n. Every `=` stepped over whose value is not quote-led opens
 * the same value's suffix, so it joins the chain and takes this walk's
 * answer. */
static bufsize_t unquoted_end(markdown_core_attribute_parser *p, bufsize_t at, bufsize_t *chain) {
    const unsigned char *s = p->data;
    bufsize_t n = p->length;
    while (at < n) {
        bufsize_t run = markdown_core_scan_to_class(BYTE_CLASS, BYTE_VALUE_END | BYTE_ESCAPE | BYTE_ASSIGN, s, at, n);
        p->work += (size_t)(run - at);
        at = run;
        if (at >= n) {
            break;
        }
        unsigned char c = s[at];
        p->work++;
        if (BYTE_CLASS[c] & BYTE_VALUE_END) {
            return at;
        }
        if (escaped(s, n, at)) {
            at += 2;
            continue;
        }
        if (c == '=' && at + 1 < n && s[at + 1] != '"' && s[at + 1] != '\'' && !p->ends[at].assignment_end) {
            push(p, chain, at);
        }
        at++;
    }
    return n;
}

/* The answer for the boundary `b`: one past the brace that closes the
 * container continuing there, or INVALID. */
static bufsize_t recognise(markdown_core_attribute_parser *p, bufsize_t b) {
    const unsigned char *s = p->data;
    bufsize_t n = p->length, chain = LINK_NONE, answer;
    for (;;) {
        bufsize_t e;
        p->work++;
        if (b >= n) {
            answer = INVALID;
            break;
        }
        if (horizontal(s[b]) || newline(s[b])) {
            /* Spaces and tabs, and at most one line ending among them. */
            for (; b < n && horizontal(s[b]); b++) {
                p->work++;
            }
            if (b < n && newline(s[b])) {
                if (s[b] == '\r' && b + 1 < n && s[b + 1] == '\n') {
                    b++;
                }
                for (b++; b < n && horizontal(s[b]); b++) {
                    p->work++;
                }
                if (b < n && newline(s[b])) {
                    answer = INVALID;
                    break;
                }
            }
            continue;
        }
        if (KNOWN(p->ends[b].end)) {
            answer = p->ends[b].end;
            break;
        }
        push(p, &chain, b);
        switch (s[b]) {
        case '}':
            answer = b + 1;
            goto done;
        case '=':
        case '"':
        case '\'':
        case '{':
            answer = INVALID;
            goto done;
        case '-':
            if (member_boundary(s, n, b + 1)) {
                b++;
                continue;
            }
            break;
        case '#':
        case '.':
            if (b + 1 < n && !name_terminator(s[b + 1])) {
                b = scan_name(p, n, b + 1);
                continue;
            }
            answer = INVALID;
            goto done;
        default:
            break;
        }
        /* A name. Followed by `=` it is an assignment; followed by a member
         * boundary it is a bare attribute; followed by a quote or an opening
         * brace it is nothing. */
        e = scan_name(p, n, b + 1);
        if (e >= n || s[e] != '=') {
            if (member_boundary(s, n, e)) {
                b = e;
                continue;
            }
            answer = INVALID;
            goto done;
        }
        if (KNOWN(p->ends[e].assignment_end)) {
            answer = p->ends[e].assignment_end;
            goto done;
        }
        push(p, &chain, e);
        b = n;
        if (e + 1 < n && (s[e + 1] == '"' || s[e + 1] == '\'')) {
            bufsize_t close = quote_close(p, e + 1);
            if (close < n) {
                b = close + 1;
                /* A closer at the end is read as an unquoted value's byte
                 * below, which the input going on undoes. */
                p->ran_out |= b == n;
            }
        }
        if (b == n) {
            b = unquoted_end(p, e + 1, &chain);
        }
    }
done:
    while (chain != LINK_NONE) {
        bufsize_t *word = memo(p, chain), previous = LINKED(*word);
        *word = answer;
        chain = previous;
    }
    return answer;
}

bufsize_t markdown_core_attributes_end(markdown_core_attribute_parser *p, bufsize_t start) {
    bufsize_t answer;
    p->work++;
    if (p->oom) {
        return 0;
    }
    if (start < 0 || start >= p->length || p->data[start] != '{') {
        return 0;
    }
    if (!memo_ready(p)) {
        p->oom = 1;
        return 0;
    }
    answer = recognise(p, start + 1);
    return answer == INVALID ? 0 : answer;
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
    if (p->ends) {
        markdown_core_free(p->ends);
        p->ends = NULL;
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
static bufsize_t decode_quoted(markdown_core_attribute_parser *p, bufsize_t open, bufsize_t finish) {
    markdown_core_attribute_scratch *const w = p->scratch;
    const unsigned char *s = p->data;
    const unsigned char quote = s[open];
    const bufsize_t limit = finish - 1, mark = w->strings.size;
    bufsize_t at = open + 1;
    while (at < limit) {
        bufsize_t run = markdown_core_scan_to_class(
            BYTE_CLASS, BYTE_QUOTE | BYTE_ESCAPE | BYTE_REFERENCE | BYTE_NEWLINE, s, at, limit);
        p->work += (size_t)(run - at);
        markdown_core_strbuf_put(&w->strings, s + at, run - at);
        at = run;
        if (at >= limit) {
            break;
        }
        p->work++;
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

static bufsize_t decode_unquoted(markdown_core_attribute_parser *p, bufsize_t at, bufsize_t finish) {
    markdown_core_attribute_scratch *const w = p->scratch;
    const unsigned char *s = p->data;
    const bufsize_t limit = finish - 1;
    while (at < limit) {
        bufsize_t run = markdown_core_scan_to_class(BYTE_CLASS, BYTE_VALUE_END | BYTE_ESCAPE, s, at, limit);
        p->work += (size_t)(run - at);
        markdown_core_strbuf_put(&w->strings, s + at, run - at);
        at = run;
        if (at >= limit || (BYTE_CLASS[s[at]] & BYTE_VALUE_END)) {
            break;
        }
        p->work++;
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
static inline bool S_field(markdown_core_attribute_parser *p, markdown_core_slot_kind slot, bufsize_t from,
                           bufsize_t end) {
    markdown_core_attribute_scratch *const w = p->scratch;
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
static int read_container(markdown_core_attribute_parser *p, bufsize_t start, bufsize_t finish, bufsize_t *anchor,
                          bufsize_t *anchor_length) {
    markdown_core_attribute_scratch *const w = p->scratch;
    const unsigned char *s = p->data;
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
        p->work++;
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
            at = scan_name(p, finish, at);
            bufsize_t staged = stage(w, s + from, at - from);
            if (staged < 0 || !S_field(p, marker == '#' ? MARKDOWN_CORE_SLOT_ID : MARKDOWN_CORE_SLOT_CLASS, from, at)) {
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
        at = scan_name(p, finish, at);
        if (!S_field(p, MARKDOWN_CORE_SLOT_RECORD, name, at)) {
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
        bufsize_t after = BYTE_CLASS[s[at]] & BYTE_QUOTE ? decode_quoted(p, at, finish) : 0;
        const bufsize_t from = after ? at + 1 : at;
        at = after ? after : decode_unquoted(p, at, finish);
        if (!S_field(p, MARKDOWN_CORE_SLOT_VALUE, from, after ? at - 1 : at)) {
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
static int lay_out(markdown_core_attribute_parser *p, bufsize_t anchor, bufsize_t anchor_length,
                   markdown_core_attributes *result) {
    markdown_core_attribute_scratch *const w = p->scratch;
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

bufsize_t markdown_core_attributes_tail(markdown_core_attribute_parser *p, bufsize_t start, bufsize_t end) {
    if (end <= start || p->data[end - 1] != '}') {
        return -1;
    }
    for (bufsize_t at = start; at < end; at++) {
        p->work++;
        if (escaped(p->data, end, at)) {
            at++;
        } else if (p->data[at] == '{' && markdown_core_attributes_end(p, at) == end) {
            return at;
        }
    }
    return -1;
}

void markdown_core_attributes_fields(const markdown_core_attribute_parser *p, const markdown_core_field_to *to) {
    const markdown_core_attribute_scratch *const w = p->scratch;
    if (w) {
        markdown_core_field_many(to, w->places, w->place_count);
    }
}

int markdown_core_attributes_parse(markdown_core_attribute_parser *p, bufsize_t start, markdown_core_attributes *result,
                                   bufsize_t *end) {
    bufsize_t finish = markdown_core_attributes_end(p, start), anchor = -1, anchor_length = 0;
    if (!finish) {
        return 0;
    }
    assert(p->scratch);
    if (!read_container(p, start, finish, &anchor, &anchor_length) || !lay_out(p, anchor, anchor_length, result)) {
        p->oom = 1;
        return 0;
    }
    p->work += (size_t)(finish - start);
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

/* WHAT A RUN HOLDS OF THE ATTRIBUTE ELEMENT: the recognizer of its input,
 * made at the first block read, one plus the start of the last block a
 * service user asked about (services.h, `block`), whose end reads
 * that question already reported, or 0, and the values the walks of the
 * blocks it gave up read. */
typedef struct {
    markdown_core_attribute_parser parser;
    bufsize_t asked;
    S_value_seen seen;
} S_run;

static S_run *S_run_of(const markdown_core_element_instance *self, const markdown_core_inline_state *inline_state) {
    return markdown_core_run_state(inline_state, self);
}

/* WHAT A PARSE HOLDS OF IT: the workspace every value of the parse is read
 * into before it is laid out, which every recognizer of the parse borrows. */
typedef struct {
    markdown_core_attribute_scratch scratch;
} S_state;

/* The services this element asks, as `self->needs` holds them. */
enum { ATTRIBUTES_REFERENCES };
static const char *const ATTRIBUTES_NEEDS[] = {[ATTRIBUTES_REFERENCES] = MARKDOWN_CORE_SERVICE_REFERENCES, NULL};

markdown_core_attribute_scratch *markdown_core_attributes_scratch(const markdown_core_element_instance *self) {
    markdown_core_attribute_scratch *scratch = &((S_state *)self->state)->scratch;
    scratch->references = self->needs[ATTRIBUTES_REFERENCES];
    return scratch;
}

static void dispose_parser(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)parser;
    markdown_core_attribute_scratch_free(markdown_core_attributes_scratch(self));
}

/* THE ATTRIBUTES' TEXT (plan symbols-cleanroom.md 23.12): the block at
 * `start` of a construct of kind `owner` as the reading of the text reads
 * it: where it ends, or 0. One that does not close is given up
 * (`report`) with the members it read. Its value is read again where
 * compile takes it (S_value). */
static bufsize_t S_block(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                         bufsize_t start, markdown_core_node_kind owner, bool report) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    S_run *run = S_run_of(self, inline_state);
    markdown_core_attribute_parser *recognizer = &run->parser;
    const bufsize_t size = (bufsize_t)tokenizer->size;
    if ((uint32_t)start >= tokenizer->cut) {
        return 0;
    }
    if (start >= size) {
        markdown_core_met_end(tokenizer);
        return 0;
    }
    if (tokenizer->data[start] != '{') {
        return 0;
    }
    if (!recognizer->data) {
        *recognizer = (markdown_core_attribute_parser){
            .data = tokenizer->data,
            .length = size,
            .scratch = markdown_core_attributes_scratch(self),
        };
    }
    const bufsize_t end = markdown_core_attributes_end(recognizer, start);
    if (recognizer->oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    if (report && (!end || recognizer->ran_out)) {
        markdown_core_met_end(tokenizer);
    }
    if (report && !end && !inline_state->error) {
        S_members members;
        S_members_read(&members, owner, tokenizer->data, size, start, &run->seen);
        if (members.failed) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        } else if (members.count) {
            markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_NONE, start, start + 1, members.items,
                                       members.count);
        }
        if (members.items != members.local) {
            markdown_core_free(members.items);
        }
    }
    return end;
}

/* (services.h, `block`) A block a construct of kind `owner` asks for, which
 * the block after it then reads as asked. */
static bufsize_t S_asked_block(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                               bufsize_t start, markdown_core_node_kind owner) {
    S_run_of(self, tokenizer->reader)->asked = start + 1;
    return S_block(self, tokenizer, start, owner, true);
}

/* The value of the block at `start` into `node`, and its fields, as
 * compile makes the node it follows or the span it closes. */
static void S_value(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                    markdown_core_node *node, bufsize_t start) {
    markdown_core_attribute_parser *recognizer = &S_run_of(self, inline_state)->parser;
    bufsize_t end = 0;
    if (!markdown_core_attributes_parse(recognizer, start, &node->attributes, &end) || recognizer->oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    const markdown_core_field_to to = markdown_core_inline_fields(inline_state, node);
    markdown_core_attributes_fields(recognizer, &to);
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

static markdown_core_state S_follow_start(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_inline_state *inline_state = tokenizer->reader;
    if (!S_follows(self, tokenizer)) {
        return markdown_core_nok(tokenizer);
    }
    const bufsize_t start = (bufsize_t)tokenizer->position;
    const markdown_core_node_type kind = markdown_core_text_made_at(tokenizer);
    const bool asked = S_run_of(self, inline_state)->asked == start + 1;
    const bufsize_t end = S_block(self, tokenizer, start, markdown_core_kind_public((uint16_t)kind), !asked);
    if (!end) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_enter(tokenizer, (uint16_t)(self->token_base + ATTRIBUTES_FOLLOW));
    markdown_core_consume_to(tokenizer, (uint32_t)end);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + ATTRIBUTES_FOLLOW));
    return markdown_core_ok(tokenizer);
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
static markdown_core_bracket_match S_end_span(const markdown_core_element_instance *self,
                                              markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                              markdown_core_label_made *made) {
    const markdown_core_opener opener = *label;
    const bufsize_t start = (bufsize_t)made->past;
    const bufsize_t end = S_block(self, tokenizer, start, MARKDOWN_CORE_KIND_SPAN, true);
    if (!end) {
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
    S_value(self, inline_state, node, (bufsize_t)token->start);
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
    S_value(self, inline_state, inl, (bufsize_t)token->data);
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

static void dispose_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_attribute_parser *recognizer = &S_run_of(self, inline_state)->parser;
    if (recognizer->data) {
        markdown_core_attribute_parser_free(recognizer);
    }
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
    markdown_core_attribute_parser attributes = {
        .data = line, .length = end, .scratch = markdown_core_attributes_scratch(self)};
    const bufsize_t start = markdown_core_attributes_tail(&attributes, first, end);
    /* The braces of the tail are its own, which closes. */
    S_report_open(&lead->fields, kind, line, from, start >= 0 ? start : end, end);
    bufsize_t taken = 0, stop;
    if (start >= 0 && markdown_core_attributes_parse(&attributes, start, &node->attributes, &stop)) {
        markdown_core_attributes_fields(&attributes, &lead->fields);
        bufsize_t at = start;
        while (at > first && markdown_core_is_space_or_tab(line[at - 1])) {
            at--;
        }
        taken = to - at;
    }
    if (attributes.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_attribute_parser_free(&attributes);
    return taken;
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
    {.precedence = -10, .end = S_end_span},
};

static const markdown_core_inline_ops ATTRIBUTES_INLINES = {
    .constructs = ATTRIBUTES_CONSTRUCTS,
    .construct_count = sizeof(ATTRIBUTES_CONSTRUCTS) / sizeof(ATTRIBUTES_CONSTRUCTS[0]),
    .tokens = ATTRIBUTES_TOKENS,
    .token_count = ATTRIBUTES_KINDS,
    .dispose = dispose_inline,
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
static void S_begin(const markdown_core_element_instance *self, markdown_core_attribute_parser *p,
                    const unsigned char *data, bufsize_t size) {
    *p = (markdown_core_attribute_parser){
        .data = data, .length = size, .scratch = markdown_core_attributes_scratch(self)};
}

static void S_release(const markdown_core_element_instance *self, markdown_core_attribute_parser *p) {
    (void)self;
    markdown_core_attribute_parser_free(p);
}

static bufsize_t S_end(const markdown_core_element_instance *self, markdown_core_attribute_parser *p, bufsize_t start) {
    (void)self;
    return markdown_core_attributes_end(p, start);
}

static bool S_parse(const markdown_core_element_instance *self, markdown_core_attribute_parser *p, bufsize_t start,
                    markdown_core_attributes *value, bufsize_t *end) {
    (void)self;
    return markdown_core_attributes_parse(p, start, value, end) != 0;
}

static bool S_open(const markdown_core_element_instance *self, const markdown_core_field_to *to,
                   markdown_core_node_kind owner, const unsigned char *data, bufsize_t size, bufsize_t start) {
    (void)self;
    return markdown_core_attributes_open(to, owner, true, data, size, start);
}

static void S_fields(const markdown_core_element_instance *self, const markdown_core_attribute_parser *p,
                     const markdown_core_field_to *to) {
    (void)self;
    markdown_core_attributes_fields(p, to);
}

static const markdown_core_attribute_ops ATTRIBUTES_OPS = {
    .begin = S_begin,
    .release = S_release,
    .end = S_end,
    .parse = S_parse,
    .block = S_asked_block,
    .report = markdown_core_attributes_report,
    .open = S_open,
    .fields = S_fields,
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
