#include <assert.h>

#include "alloc.h"
#include "attributes.h"
#include "parts.h"
#include "bracket.h"
#include "inline_internal.h"
#include "../core/attributes.h"
#include "entities.h"
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

/* Where the block must reach to hold `chunk`'s bytes and the NUL past them,
 * as an offset from its start: a chunk the value does not own, and not
 * empty, points into the block (lay_out). */
static size_t block_end(const markdown_core_chunk *chunk, const unsigned char *start, size_t end) {
    if (chunk->alloc || !chunk->data) {
        return end;
    }
    const size_t past = (size_t)(chunk->data - start) + (size_t)chunk->len + 1;
    return past > end ? past : end;
}

/* The place in the copy that lies where `data` lies in the block. */
static void *in_copy(unsigned char *copy, const unsigned char *start, const void *data) {
    return copy + ((const unsigned char *)data - start);
}

/* THE VALUE'S BLOCK IS ITS TABLES AND THEN ITS STRINGS (lay_out), each
 * string terminated, so the block ends past the last string's NUL, and past
 * its tables when it holds no string. Copying it and pointing every member
 * the view kept in it at the same offset in the copy keeps its layout. A view
 * keeps a member of the value's until it lets the whole value go, or, the
 * anchor, until it writes one where the value has none. */
bool markdown_core_attributes_own(markdown_core_attributes *view, const markdown_core_attributes *value) {
    const unsigned char *const start = value->storage;
    const bool records = view->records && view->records == value->records;
    const bool classes = view->classes && view->classes == value->classes;
    const bool anchor = view->anchor.data && view->anchor.data == value->anchor.data && !view->anchor.alloc;
    if (start && !view->storage && (records || classes || (anchor && !value->anchor.alloc))) {
        size_t end = 0;
        if (value->records) {
            end = (size_t)((const unsigned char *)(value->records + value->record_count) - start);
        }
        if (value->classes) {
            const size_t past = (size_t)((const unsigned char *)(value->classes + value->class_count) - start);
            end = past > end ? past : end;
        }
        end = block_end(&value->anchor, start, end);
        for (uint32_t i = 0; i < value->class_count; i++) {
            end = block_end(&value->classes[i], start, end);
        }
        for (uint32_t i = 0; i < value->record_count; i++) {
            end = block_end(&value->records[i].name, start, end);
            end = block_end(&value->records[i].value, start, end);
        }
        unsigned char *block = markdown_core_realloc(NULL, end);
        if (!block) {
            return false;
        }
        memcpy(block, start, end);
        view->storage = block;
        if (records) {
            view->records = in_copy(block, start, value->records);
            for (uint32_t i = 0; i < view->record_count; i++) {
                view->records[i].name.data = in_copy(block, start, value->records[i].name.data);
                view->records[i].value.data = in_copy(block, start, value->records[i].value.data);
            }
        }
        if (classes) {
            view->classes = in_copy(block, start, value->classes);
            for (uint32_t i = 0; i < view->class_count; i++) {
                view->classes[i].data = in_copy(block, start, value->classes[i].data);
            }
        }
        if (anchor && !value->anchor.alloc) {
            view->anchor.data = in_copy(block, start, value->anchor.data);
        }
    }
    return !(anchor && value->anchor.alloc) || markdown_core_chunk_to_cstr(&view->anchor);
}

void markdown_core_attributes_adopt(markdown_core_attributes *view, markdown_core_attributes *value) {
    const bool records = view->records && view->records == value->records;
    const bool classes = view->classes && view->classes == value->classes;
    const bool anchor = view->anchor.data && view->anchor.data == value->anchor.data && !view->anchor.alloc;
    if (value->storage && !view->storage && (records || classes || (anchor && !value->anchor.alloc))) {
        view->storage = value->storage;
        value->storage = NULL;
    }
    if (anchor && value->anchor.alloc) {
        view->anchor.alloc = value->anchor.alloc;
        value->anchor.alloc = 0;
    }
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
            bufsize_t used = markdown_core_entities_read(w->entities, &w->strings, s + at + 1, limit - at - 1);
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

/* READ the recognised container `[start, finish)` into the scratch. Returns
 * 0 only on allocation failure. */
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
            if (staged < 0) {
                return 0;
            }
            if (marker == '#') {
                *anchor = staged;
                *anchor_length = at - from;
            } else if (!add_member(w, -1, 0, staged, at - from)) {
                return 0;
            }
            continue;
        }
        bufsize_t name = at;
        at = scan_name(p, finish, at);
        bufsize_t name_length = at - name, value = w->strings.size;
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
        at = after ? after : decode_unquoted(p, at, finish);
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

/* Where a member's field goes on past a cursor: a name, an unquoted value,
 * and a value in either quote. A backslash that escapes nothing is a value
 * byte, as in unquoted_end. */
static bufsize_t name_end(const unsigned char *data, bufsize_t length) {
    return markdown_core_scan_to_class(BYTE_CLASS, BYTE_NAME_END, data, 0, length);
}
bufsize_t markdown_core_attributes_name_end(const unsigned char *data, bufsize_t length) {
    return name_end(data, length);
}
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
/* A quote-led value's field starts at its quote. Closed on its line, the
 * field takes the closing quote too; with no closing quote the value falls
 * back to an unquoted one, as in recognise, once a brace on the line closes
 * the block, and reads to the line's end while the block is still open. */
static bufsize_t quote_led_end(const unsigned char *data, bufsize_t length) {
    const bufsize_t close = 1 + quoted_end(data + 1, length - 1, data[0]);
    if (close < length) {
        return close + 1;
    }
    for (bufsize_t at = 1; at < length; at += escaped(data, length, at) ? 2 : 1) {
        if (data[at] == '}') {
            return value_end(data, length);
        }
    }
    return length;
}
/* A value's field, read from its first byte, which may follow the cursor. */
static bufsize_t member_value_end(const unsigned char *data, bufsize_t length) {
    return length && (data[0] == '"' || data[0] == '\'') ? quote_led_end(data, length) : value_end(data, length);
}

/* THE PARTS OF A CONTAINER (parts.h): its members as read_container reads
 * them, with the scanners its fields are read with past a cursor. An `#`
 * identifier's name is an id and a `.` class's a class; a record's name is
 * a record and its value, inside its quotes, a value. */
bufsize_t markdown_core_attributes_parts(markdown_core_parts *parts, bufsize_t brace) {
    markdown_core_attribute_parser p = {.data = parts->text, .length = parts->length};
    const bufsize_t finish = markdown_core_attributes_end(&p, brace);
    const bool oom = p.oom;
    markdown_core_attribute_parser_free(&p);
    if (!finish) {
        parts->failed = parts->failed || oom;
        return brace;
    }
    const unsigned char *s = parts->text;
    const bufsize_t limit = finish - 1;
    bufsize_t at = brace + 1;
    while (at < limit) {
        if (BYTE_CLASS[s[at]] & (BYTE_SPACE | BYTE_NEWLINE)) {
            at++;
            continue;
        }
        if (s[at] == '-' && member_boundary(s, finish, at + 1)) {
            at++;
            continue;
        }
        if (s[at] == '#' || s[at] == '.') {
            const markdown_core_slot_kind slot = s[at] == '#' ? MARKDOWN_CORE_SLOT_ID : MARKDOWN_CORE_SLOT_CLASS;
            const bufsize_t from = ++at;
            at += name_end(s + at, limit - at);
            markdown_core_parts_add(parts, slot, from, at);
            continue;
        }
        const bufsize_t name = at;
        at += name_end(s + at, limit - at);
        markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_RECORD, name, at);
        if (at >= limit || s[at] != '=') {
            continue;
        }
        at++;
        if (at < limit && (s[at] == '"' || s[at] == '\'')) {
            const bufsize_t close = at + 1 + quoted_end(s + at + 1, limit - at - 1, s[at]);
            if (close < limit) {
                markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_VALUE, at + 1, close);
                at = close + 1;
                continue;
            }
        }
        const bufsize_t value = at;
        at += value_end(s + at, limit - at);
        markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_VALUE, value, at);
    }
    return finish;
}

bufsize_t markdown_core_attributes_tail_parts(markdown_core_parts *parts, bufsize_t from, bufsize_t end) {
    while (end > from && markdown_core_is_space_or_tab(parts->text[end - 1])) {
        end--;
    }
    markdown_core_attribute_parser p = {.data = parts->text, .length = end};
    const bufsize_t brace = markdown_core_attributes_tail(&p, from, end);
    const bool oom = p.oom;
    markdown_core_attribute_parser_free(&p);
    /* A brace a node of the block's content holds is that node's. */
    if (brace < 0 || markdown_core_parts_within(parts, brace)) {
        parts->failed = parts->failed || oom;
        return end;
    }
    markdown_core_attributes_parts(parts, brace);
    return brace;
}

/* AN UNQUOTED VALUE'S END, MEMOISED BY THE POSITION IT IS READ FROM. A value
 * opens after an `=`, and a scan that steps over an `=`, escaped or not,
 * lands on the byte after it, so the value from there ends where the scan
 * does: the scan notes its end at each such byte, and a later value read
 * from one of them takes the end there. */
static bufsize_t memo_value_end(markdown_core_member_memo *memo, const unsigned char *s, bufsize_t n, bufsize_t v) {
    if (!memo) {
        return v + value_end(s + v, n - v);
    }
    if (memo->values[v]) {
        return memo->values[v] - 1;
    }
    const unsigned char stops = BYTE_VALUE_END | BYTE_ESCAPE | BYTE_ASSIGN;
    bufsize_t at = v, end = n;
    while ((at = markdown_core_scan_to_class(BYTE_CLASS, stops, s, at, n)) < n) {
        if (s[at] != '\\' && s[at] != '=') {
            end = at;
            break;
        }
        at += escaped(s, n, at) ? 2 : 1;
        if (s[at - 1] == '=' && memo->values[at]) {
            end = memo->values[at] - 1;
            break;
        }
    }
    memo->values[v] = end + 1;
    for (at = v; (at = markdown_core_scan_to_class(BYTE_CLASS, stops, s, at, end)) < end;) {
        at += escaped(s, n, at) ? 2 : 1;
        if (s[at - 1] == '=') {
            if (memo->values[at]) {
                break;
            }
            memo->values[at] = end + 1;
        }
    }
    return end;
}

enum { MEMBER_FAIL, MEMBER_FIELD, MEMBER_NEXT };

/* One member of a block that does not close, read from the boundary `b`:
 * the field the input's end cuts it short in, the boundary after it, or
 * neither when the block is no block whatever follows. */
static int member_step(markdown_core_member_memo *memo, const unsigned char *s, bufsize_t n, bufsize_t b,
                       bufsize_t *next, markdown_core_field *field) {
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
        *field = (markdown_core_field){MARKDOWN_CORE_SLOT_RECORD, 0, b, name_end, {{0}}, 0, false, 0};
        return MEMBER_FIELD;
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
        const markdown_core_slot_kind slot = c == '#' ? MARKDOWN_CORE_SLOT_ID : MARKDOWN_CORE_SLOT_CLASS;
        if (b + 1 < n && name_terminator(s[b + 1])) {
            return MEMBER_FAIL;
        }
        bufsize_t e = b + 1;
        while (e < n && !name_terminator(s[e])) {
            e++;
        }
        if (e >= n) {
            *field = (markdown_core_field){slot, 0, b + 1, name_end, {{0}}, 0, false, 0};
            return MEMBER_FIELD;
        }
        *next = e;
        return MEMBER_NEXT;
    }
    bufsize_t e = b + 1;
    while (e < n && !name_terminator(s[e])) {
        e++;
    }
    if (e >= n) {
        *field = (markdown_core_field){MARKDOWN_CORE_SLOT_RECORD, 0, b, name_end, {{0}}, 0, false, 0};
        return MEMBER_FIELD;
    }
    if (s[e] != '=') {
        if (!member_boundary(s, n, e)) {
            return MEMBER_FAIL;
        }
        *next = e;
        return MEMBER_NEXT;
    }
    const bool quoted = e + 1 < n && (s[e + 1] == '"' || s[e + 1] == '\'');
    bufsize_t v = e + 1;
    bufsize_t close = quoted ? v + 1 + quoted_end(s + v + 1, n - v - 1, s[v]) : memo_value_end(memo, s, n, v);
    if (close >= n) {
        *field = (markdown_core_field){MARKDOWN_CORE_SLOT_VALUE, 0, v, member_value_end, {{b, e}}, 1, false, 0};
        return MEMBER_FIELD;
    }
    *next = quoted ? close + 1 : close;
    return MEMBER_NEXT;
}

/* The memo's tables, made on the second brace it is asked about: one brace
 * needs none. A refusal leaves the walks unmemoised. */
static bool member_memo_made(markdown_core_member_memo *memo, bufsize_t n) {
    if (memo->walks) {
        return true;
    }
    if (memo->refused || !memo->asked++) {
        return false;
    }
    memo->walks = markdown_core_alloc((size_t)n + 1, sizeof(*memo->walks));
    memo->values = markdown_core_alloc((size_t)n + 1, sizeof(*memo->values));
    if (!memo->walks || !memo->values) {
        markdown_core_member_memo_free(memo);
        memo->refused = true;
        return false;
    }
    return true;
}

void markdown_core_member_memo_free(markdown_core_member_memo *memo) {
    markdown_core_free(memo->walks);
    markdown_core_free(memo->values);
    markdown_core_free(memo->found);
    *memo = (markdown_core_member_memo){0};
}

/* (attributes.h) A member boundary reads as a name not yet begun. Whether a
 * block from a boundary is cut short, and in which field, depends on the
 * boundary alone, so with a memo every boundary a walk passes takes the
 * walk's answer, `walks[b]`: 0 not yet asked, -1 no block, else one past the
 * field's index in `found`. A walk that reaches a known boundary stops
 * there; the answer is written back by walking again to it. */
bool markdown_core_attributes_open_member(const unsigned char *s, bufsize_t n, bufsize_t start,
                                          markdown_core_field *field, markdown_core_member_memo *memo) {
    if (memo && !member_memo_made(memo, n)) {
        memo = NULL;
    }
    bufsize_t b = start + 1, next = 0;
    int step = MEMBER_FAIL;
    while (!(memo && memo->walks[b]) && (step = member_step(memo, s, n, b, &next, field)) == MEMBER_NEXT) {
        b = next;
    }
    if (!memo) {
        return step == MEMBER_FIELD;
    }
    bufsize_t answer = memo->walks[b];
    if (!answer) {
        answer = -1;
        if (step == MEMBER_FIELD) {
            markdown_core_field *found =
                markdown_core_reserve(memo->found, &memo->room, memo->count + 1, sizeof(*memo->found));
            if (!found) {
                return true;
            }
            memo->found = found;
            memo->found[memo->count++] = *field;
            answer = (bufsize_t)memo->count;
        }
    }
    markdown_core_field unused;
    for (bufsize_t at = start + 1; !memo->walks[at]; at = next) {
        memo->walks[at] = answer;
        if (member_step(memo, s, n, at, &next, &unused) != MEMBER_NEXT) {
            break;
        }
    }
    if (answer < 0) {
        return false;
    }
    *field = memo->found[answer - 1];
    return true;
}

/* The block at `start` that the input's end leaves open: the input may go
 * on, and a context query reads the member it cuts short. Kept out of line,
 * off the path of every attribute block that closes. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_saw_end(markdown_core_inline_state *inline_state, bufsize_t start,
                                                          markdown_core_node_kind owner) {
    markdown_core_inline_saw_end(inline_state);
    markdown_core_field field;
    if (inline_state->context && start < markdown_core_inline_size(inline_state) &&
        markdown_core_attributes_open_member(markdown_core_inline_window(inline_state, 0).data,
                                             markdown_core_inline_window(inline_state, 0).len, start, &field, NULL)) {
        field.owner = owner;
        markdown_core_inline_open(inline_state, MARKDOWN_CORE_KIND_NONE, start, start + 1, &field, 1);
    }
}

/* The attribute block at `start`, read as markdown_core_inline_state_attributes
 * reads it, with `value` NULL for recognition alone. */
/* WHAT A RUN HOLDS OF THE ATTRIBUTE ELEMENT: the recognizer of its input,
 * made at the first block read, and one plus the start of the last block a
 * peer asked about (markdown_core_attributes_closes), whose end reads that
 * question already reported, or 0. */
typedef struct {
    markdown_core_attribute_parser parser;
    bufsize_t asked;
} S_run;

static S_run *S_run_of(const markdown_core_element_instance *self, const markdown_core_inline_state *inline_state) {
    return markdown_core_run_state(inline_state, self);
}

/* WHAT A PARSE HOLDS OF IT: the workspace every value of the parse is read
 * into before it is laid out, which every recognizer of the parse borrows. */
typedef struct {
    markdown_core_attribute_scratch scratch;
} S_state;

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { ATTRIBUTES_ENTITIES };
static const markdown_core_element *const ATTRIBUTES_PEERS[] = {[ATTRIBUTES_ENTITIES] = &MARKDOWN_CORE_ELEMENT_ENTITIES,
                                                                NULL};

markdown_core_attribute_scratch *markdown_core_attributes_scratch(const markdown_core_element_instance *self) {
    markdown_core_attribute_scratch *scratch = &((S_state *)self->state)->scratch;
    scratch->entities = self->peers[ATTRIBUTES_ENTITIES];
    return scratch;
}

static void dispose_parser(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)parser;
    markdown_core_attribute_scratch_free(markdown_core_attributes_scratch(self));
}

static int S_read(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state, bufsize_t start,
                  markdown_core_node_kind owner, markdown_core_attributes *value, bufsize_t *end, bool report) {
    markdown_core_attribute_parser *recognizer = &S_run_of(self, inline_state)->parser;
    /* A cut is the take's: no inline construct takes an attribute block that
     * begins in it (core/element.h, markdown_core_follow). One the input's
     * end leaves open is the tail only once the line ends after it: until
     * then it may be the construct's. */
    if (inline_state->cut >= 0 && start >= inline_state->cut) {
        if (report && inline_state->context && start < markdown_core_inline_size(inline_state) &&
            markdown_core_inline_char_at(inline_state, start) == '{' && markdown_core_inline_tracks_end(inline_state)) {
            S_saw_end(inline_state, start, owner);
        }
        return 0;
    }
    /* Most constructs have no attribute block after them: only a `{` is
     * read further. At the input's end a block may yet begin, once the
     * input goes on. */
    if (start >= markdown_core_inline_size(inline_state)) {
        if (markdown_core_inline_tracks_end(inline_state)) {
            S_saw_end(inline_state, start, owner);
        }
        return 0;
    }
    if (markdown_core_inline_char_at(inline_state, start) != '{') {
        return 0;
    }
    if (!recognizer->data) {
        *recognizer = (markdown_core_attribute_parser){
            .data = markdown_core_inline_window(inline_state, 0).data,
            .length = markdown_core_inline_size(inline_state),
            .scratch = markdown_core_attributes_scratch(self),
        };
    }
    int matched;
    if (value) {
        matched = markdown_core_attributes_parse(recognizer, start, value, end);
    } else {
        *end = markdown_core_attributes_end(recognizer, start);
        matched = *end != 0;
    }
    if (recognizer->oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    /* A block that does not close may close once the input goes on, and
     * one that closes inside a quote the input's end leaves open, which
     * reads it as text of a bare value, may close past it. */
    if (report && (!matched || inline_state->context || recognizer->ran_out) &&
        markdown_core_inline_tracks_end(inline_state)) {
        S_saw_end(inline_state, start, owner);
    }
    return matched;
}

bufsize_t markdown_core_attributes_closes(const markdown_core_element_instance *self,
                                          markdown_core_inline_state *inline_state, bufsize_t start,
                                          markdown_core_node_kind owner) {
    bufsize_t end = 0;
    S_run_of(self, inline_state)->asked = start + 1;
    return S_read(self, inline_state, start, owner, NULL, &end, true) ? end : 0;
}

/* AN ATTRIBUTE BLOCK FOLLOWS THE NODE BEFORE IT (element.h,
 * MARKDOWN_CORE_FOLLOW_TOKEN): a `{` where the token before made a node a
 * later token may follow is that node's attribute block when one closes,
 * and the node then reaches to its `}`. Its start stays where its own
 * syntax put it. */
static bufsize_t S_follow_token(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                markdown_core_lead *lead) {
    (void)parser;
    markdown_core_inline_state *inline_state = lead->inline_state;
    markdown_core_node *node = lead->node;
    if (markdown_core_inline_offset(inline_state) >= markdown_core_inline_size(inline_state) ||
        markdown_core_inline_char_at(inline_state, markdown_core_inline_offset(inline_state)) != '{') {
        return 0;
    }
    bufsize_t end;
    /* Only a context query reads the owner's kind. */
    const markdown_core_node_kind owner =
        inline_state->context ? markdown_core_node_get_kind(node) : MARKDOWN_CORE_KIND_NONE;
    const bool asked = S_run_of(self, inline_state)->asked == markdown_core_inline_offset(inline_state) + 1;
    const bufsize_t start = markdown_core_inline_offset(inline_state);
    if (!S_read(self, inline_state, start, owner, &node->attributes, &end, !asked)) {
        return 0;
    }
    const uint32_t place = node->place.start;
    markdown_core_inline_seek(inline_state, end);
    markdown_core_inline_state_place(inline_state, node, end - 1, end - 1);
    node->place.start = place;
    return end - start;
}

/* A node that may be updated where the run stands is open when a `{`
 * follows it: the block may yet close as the input goes on. */
static bool holds(const markdown_core_element_instance *self, const markdown_core_inline_state *inline_state) {
    (void)self;
    return markdown_core_inline_made_node(inline_state) &&
           markdown_core_inline_offset(inline_state) < markdown_core_inline_size(inline_state) &&
           markdown_core_inline_window(inline_state, markdown_core_inline_offset(inline_state)).data[0] == '{';
}

/* A run whose last token made a node that may be updated at the input's end:
 * an attribute block may yet begin there, once the input goes on. */
static void finish(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    (void)self;
    if (inline_state->made && inline_state->made_end >= markdown_core_inline_size(inline_state) &&
        inline_state->cut < 0 && markdown_core_inline_tracks_end(inline_state)) {
        markdown_core_inline_saw_end(inline_state);
    }
}

/* A BRACKETED SPAN (element.h, markdown_core_close_rule): a bracket no
 * explicit link closes, whose `]` an attribute block follows, is a span of
 * its body, before a shortcut link may take it. */
static markdown_core_bracket_match S_close_span(const markdown_core_element_instance *self,
                                                markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                                bracket *opener, markdown_core_node **made) {
    (void)made;
    markdown_core_attributes attributes = {0};
    bufsize_t end;
    if (!S_read(self, inline_state, markdown_core_inline_offset(inline_state), MARKDOWN_CORE_KIND_SPAN, &attributes,
                &end, true)) {
        return BRACKET_UNMATCHED;
    }
    if (!markdown_core_node_can_contain_type(markdown_core_scaffold_parent(opener->inl_text),
                                             MARKDOWN_CORE_NODE_SPAN)) {
        markdown_core_attributes_free(&attributes);
        return BRACKET_REJECTED;
    }
    markdown_core_node *inl = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_SPAN);
    if (!inl) {
        markdown_core_attributes_free(&attributes);
        markdown_core_inline_pop_bracket(inline_state);
        return BRACKET_MATCHED;
    }
    inl->attributes = attributes;
    markdown_core_inline_seek(inline_state, end);
    markdown_core_inline_state_place(inline_state, inl, opener->position - 1, end - 1);
    markdown_core_inline_settle_bracket(inline_state, opener);
    markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
    markdown_core_inline_take_bracket_content(parser, inline_state, opener, inl);
    markdown_core_inline_replace_bracket_opener(inline_state, opener, inl);
    markdown_core_inline_pop_bracket(inline_state);
    return BRACKET_MATCHED;
}

/* THE PARTS OF A BRACKETED SPAN (parts.h): the attribute container after
 * the `]` that closes its text. */
static void S_span_parts(markdown_core_parts *parts) {
    const unsigned char *s = parts->text;
    const bufsize_t n = parts->length;
    bufsize_t close = markdown_core_parts_last(parts, 1);
    while (close < n && s[close] != ']') {
        close += s[close] == '\\' && close + 1 < n ? 2 : 1;
    }
    if (close + 1 < n && s[close + 1] == '{') {
        markdown_core_attributes_parts(parts, close + 1);
    }
}

bufsize_t markdown_core_attributes_close_at(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                            markdown_core_attribute_parser *held, const unsigned char *data,
                                            bufsize_t length, bufsize_t start) {
    (void)self;
    held->data = data;
    held->length = length;
    held->scratch = markdown_core_attributes_scratch(self);
    const bufsize_t end = markdown_core_attributes_end(held, start);
    if (held->oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return end;
}

bool markdown_core_attributes_write(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_attribute_parser *held, bufsize_t start, markdown_core_node *node) {
    (void)self;
    bufsize_t end;
    const bool written = markdown_core_attributes_parse(held, start, &node->attributes, &end);
    if (held->oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return written;
}

void markdown_core_attributes_release(const markdown_core_element_instance *self,
                                      markdown_core_attribute_parser *held) {
    (void)self;
    markdown_core_attribute_parser_free(held);
}

bool markdown_core_attributes_report(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node_kind owner, const unsigned char *line, bufsize_t from,
                                     bufsize_t end) {
    (void)self;
    if (!parser->sink) {
        return false;
    }
    markdown_core_field field;
    markdown_core_member_memo memo = {0};
    bool open = false;
    for (bufsize_t at = from; at < end; at++) {
        if (escaped(line, end, at)) {
            at++;
        } else if (line[at] == '{' && markdown_core_attributes_open_member(line, end, at, &field, &memo)) {
            field.owner = owner;
            markdown_core_parser_open(parser, MARKDOWN_CORE_KIND_NONE, at, at + 1, &field, 1);
            open = true;
        }
    }
    markdown_core_member_memo_free(&memo);
    return open;
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
    if (lead->live) {
        markdown_core_attributes_report(self, parser, markdown_core_node_get_kind(node), line, from, end);
    }
    if (end == from || line[end - 1] != '}') {
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
    bufsize_t taken = 0, stop;
    if (start >= 0 && markdown_core_attributes_parse(&attributes, start, &node->attributes, &stop)) {
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
}

/* The nodes an attribute block follows: inline, those of the tokens that
 * say a later token may follow them (markdown_core_inline_made); at a
 * line's end, the blocks that offer it. A heading's inline tokens win over
 * its tail. */
static const markdown_core_node_type ATTRIBUTES_TOKEN_KINDS[] = {MARKDOWN_CORE_NODE_CODE, MARKDOWN_CORE_NODE_LINK,
                                                                 MARKDOWN_CORE_NODE_EMBEDDED,
                                                                 MARKDOWN_CORE_NODE_DIRECTIVE, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type ATTRIBUTES_TAIL_KINDS[] = {MARKDOWN_CORE_NODE_HEADING,
                                                                MARKDOWN_CORE_NODE_CODE_BLOCK, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_follow ATTRIBUTES_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_TOKEN, .kinds = ATTRIBUTES_TOKEN_KINDS, .follow = S_follow_token},
    {.at = MARKDOWN_CORE_FOLLOW_TAIL,
     .kinds = ATTRIBUTES_TAIL_KINDS,
     .follow = S_follow_tail,
     .yields = true,
     .undo = S_untake},
    {0},
};

/* The attribute tails the run's end may leave open on the last line of a
 * heading's text, or of a paragraph's that a setext underline may yet make a
 * heading's, where the dialect holds headings, to the run's context sink:
 * every brace that may be one, since which is the tail only the finished
 * line tells. A heading whose tail closed has it taken from its text. */
static void report(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    (void)self;
    const markdown_core_node *owner = inline_state->owner;
    const markdown_core_parser *parser = inline_state->owner_parser;
    bool declares;
    if (owner->kind == MARKDOWN_CORE_NODE_HEADING) {
        if (owner->cut_back) {
            return;
        }
        declares = false;
    } else if (owner->kind == MARKDOWN_CORE_NODE_PARAGRAPH &&
               markdown_core_dialect_block_kind(parser->dialect, MARKDOWN_CORE_NODE_HEADING)->structure) {
        declares = true;
    } else {
        return;
    }
    const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
    const bufsize_t len = markdown_core_inline_size(inline_state);
    bufsize_t at = len;
    while (at > 0 && !markdown_core_is_line_end(data[at - 1])) {
        at--;
    }
    markdown_core_field field;
    markdown_core_member_memo memo = {0};
    for (; at < len; at++) {
        if (data[at] == '\\' && at + 1 < len && markdown_core_ispunct(data[at + 1])) {
            at++;
        } else if (data[at] == '{' && markdown_core_attributes_open_member(data, len, at, &field, &memo)) {
            field.owner = MARKDOWN_CORE_KIND_HEADING;
            field.declares = declares;
            if (declares) {
                markdown_core_inline_open_line(inline_state, MARKDOWN_CORE_KIND_NONE, at, at + 1, &field, 1);
            } else {
                markdown_core_inline_open(inline_state, MARKDOWN_CORE_KIND_NONE, at, at + 1, &field, 1);
            }
        }
    }
    markdown_core_member_memo_free(&memo);
}

/* A span sits between a link whose destination, title or label follows the
 * `]` (precedence -20) and a shortcut link (10). */
static const markdown_core_close_rule ATTRIBUTES_CLOSES[] = {
    {.precedence = -10, .close = S_close_span},
};

static const markdown_core_inline_ops ATTRIBUTES_INLINES = {
    .holds = holds,
    .report = report,
    .finish = finish,
    .dispose = dispose_inline,
    .closes = ATTRIBUTES_CLOSES,
    .close_count = sizeof(ATTRIBUTES_CLOSES) / sizeof(ATTRIBUTES_CLOSES[0]),
    .closer = ']',
};

const markdown_core_facet MARKDOWN_CORE_ATTRIBUTES_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_SPAN, .parts = S_span_parts},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES = {
    .name = "attributes",
    .peers = ATTRIBUTES_PEERS,
    .state_size = sizeof(S_state),
    .run_state_size = sizeof(S_run),
    .dispose_parser = dispose_parser,
    .follows = ATTRIBUTES_FOLLOWS,
    .facets = MARKDOWN_CORE_ATTRIBUTES_FACETS,
    .inlines = &ATTRIBUTES_INLINES,
};
