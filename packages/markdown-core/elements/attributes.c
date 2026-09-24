#include <assert.h>

#include "alloc.h"
#include "attributes.h"
#include "../core/attributes.h"
#include "houdini.h"
#include "node.h"
#include "markdown_core_ctype.h"
#include "utf8.h"
#include <stdint.h>
#include <string.h>

/* THE BYTES THE GRAMMAR TELLS APART, as one table. Every scan below runs
 * until a byte of the classes that end it, so a byte costs one load whatever
 * the number of bytes that could end the run; a byte not listed is an
 * ordinary byte of whatever is being read. */
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
    [' '] = BYTE_SPACE | BYTE_NAME_END,
    ['\t'] = BYTE_SPACE | BYTE_NAME_END,
    ['\n'] = BYTE_NEWLINE | BYTE_NAME_END,
    ['\r'] = BYTE_NEWLINE | BYTE_NAME_END,
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
static int32_t scalar(const unsigned char *s, bufsize_t n, bufsize_t p, bufsize_t *width) {
    int32_t cp;
    /* Valid UTF-8 is the parser's input precondition, and the advance is total
     * so every caller's `at += width` moves forward whatever the bytes are. */
    *width = markdown_core_utf8proc_step(s + p, n - p, &cp);
    return cp;
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
        bufsize_t run = at;
        while (run < n && !(BYTE_CLASS[s[run]] & (BYTE_QUOTE | BYTE_ESCAPE | BYTE_NEWLINE))) {
            run++;
        }
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
        bufsize_t run = at;
        while (run < n && !(BYTE_CLASS[s[run]] & (BYTE_VALUE_END | BYTE_ESCAPE | BYTE_ASSIGN))) {
            run++;
        }
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
 * is the common call. A value owns its one block, its hold on the resource a
 * computed anchor borrows from and, when a consumer replaced it, its anchor. */
void markdown_core_attributes_free(markdown_core_attributes *v) {
    if (!markdown_core_attributes_owns(v)) {
        return;
    }
    markdown_core_chunk_free(&v->anchor);
    markdown_core_free(v->storage);
    if (v->anchor_owner) {
        markdown_core_resource_release(v->anchor_owner);
    }
    memset(v, 0, sizeof(*v));
}

/* The memo exists only once recognition ran (see the recogniser above): a
 * parser that never asked owns nothing and releases nothing. The scratch is
 * borrowed and released by its owner. */
void markdown_core_attribute_parser_free(markdown_core_attribute_parser *p) {
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

/* `class=` names a run of classes: split the staged value at its white space
 * in place, ending each class with a NUL where the space began. */
static int add_class_run(markdown_core_attribute_scratch *w, bufsize_t value, bufsize_t size) {
    unsigned char *text = w->strings.ptr + value;
    bufsize_t word = 0, at = 0;
    while (at < size) {
        bufsize_t width;
        int32_t cp = scalar(text, size, at, &width);
        if (markdown_core_utf8proc_is_space(cp) || cp == 11) {
            if (at > word && !add_member(w, -1, 0, value + word, at - word)) {
                return 0;
            }
            text[at] = 0;
            word = at + width;
        }
        at += width;
    }
    return at == word || add_member(w, -1, 0, value + word, at - word);
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
        bufsize_t run = at;
        while (run < limit && !(BYTE_CLASS[s[run]] & (BYTE_QUOTE | BYTE_ESCAPE | BYTE_REFERENCE | BYTE_NEWLINE))) {
            run++;
        }
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
            bufsize_t used = houdini_unescape_ent(&w->strings, s + at + 1, limit - at - 1);
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
        bufsize_t run = at;
        while (run < limit && !(BYTE_CLASS[s[run]] & (BYTE_VALUE_END | BYTE_ESCAPE))) {
            run++;
        }
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
int markdown_core_inline_state_attributes(markdown_core_inline_state *inline_state, bufsize_t start,
                                          markdown_core_attributes *value, bufsize_t *end) {
    if (start == inline_state->heading_attributes_start) {
        return 0;
    }
    if (!inline_state->attributes.data) {
        inline_state->attributes = (markdown_core_attribute_parser){
            .data = inline_state->input.data,
            .length = inline_state->input.len,
            .scratch = &inline_state->owner_parser->attribute_scratch,
        };
    }
    int matched = markdown_core_attributes_parse(&inline_state->attributes, start, value, end);
    if (inline_state->attributes.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return matched;
}

void markdown_core_inline_attach_inline_attributes(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                                   bufsize_t from) {
    bufsize_t end;
    if (markdown_core_inline_state_attributes(inline_state, inline_state->pos, &node->attributes, &end)) {
        inline_state->pos = end;
        markdown_core_inline_state_place(inline_state, node, from, end - 1);
    }
}

bufsize_t markdown_core_attributes_attach_tail(markdown_core_parser *parser, markdown_core_node *node,
                                               const unsigned char *source, bufsize_t length) {
    bufsize_t info_end = length, attribute_end;
    while (info_end > 0 && markdown_core_block_is_space_or_tab(source[info_end - 1])) {
        info_end--;
    }
    markdown_core_attribute_parser attributes = {
        .data = source, .length = length, .scratch = &parser->attribute_scratch};
    bufsize_t attribute_start = markdown_core_attributes_tail(&attributes, 0, info_end);
    if (attribute_start >= 0 &&
        markdown_core_attributes_parse(&attributes, attribute_start, &node->attributes, &attribute_end)) {
        info_end = attribute_start;
    }
    if (attributes.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    parser->attribute_work += attributes.work;
    markdown_core_attribute_parser_free(&attributes);
    return info_end;
}

static void dispose_inline(markdown_core_inline_state *inline_state) {
    if (inline_state->attributes.data) {
        inline_state->owner_parser->attribute_work += inline_state->attributes.work;
        markdown_core_attribute_parser_free(&inline_state->attributes);
    }
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES = {
    .name = "attributes",
    .dispose_inline = dispose_inline,
};
