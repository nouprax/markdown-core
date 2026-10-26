/* The YAML parser (yaml.h), reworked from libyaml's scanner (src/scanner.c)
 * and parser (src/parser.c), https://github.com/yaml/libyaml at commit
 * 90a56d4500aa1a1798514c5cb55c3ad4cb095f94 (2026-08-21). Its structure,
 * the token queue, the simple-key candidates, the indentation stack and the
 * parser's states, is libyaml's, so a fix upstream can be ported by hand.
 * The rework reads UTF-8 text in place with the repository's allocator and
 * buffers, and reads the properties language (yaml.h): no encodings, BOM,
 * directives, tags, anchors, aliases, explicit names or folded scalars; a
 * scalar ends with its line; double-quoted escapes are JSON's; a literal is
 * a bare `|` whose first nonblank line sets its indentation; a `- ` where no
 * block item may begin starts plain text; NEL, LS and PS are text, not line
 * breaks; a simple name's 1024 limit counts characters; and the scanner
 * keeps the comments it skips.
 *
 * libyaml's licence:
 *
 * Copyright (c) 2017-2020 Ingy döt Net
 * Copyright (c) 2006-2016 Kirill Simonov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "yaml.h"
#include "alloc.h"
#include "buffer.h"
#include "utf8.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

/* The deepest nesting of collections, libyaml's default. */
#define NESTING 1000
/* The most characters a simple name may hold. */
#define NAME_LIMIT 1024

typedef enum {
    STREAM_START,
    STREAM_END,
    DOCUMENT_START,
    DOCUMENT_END,
    BLOCK_LIST_START,
    BLOCK_MAP_START,
    BLOCK_END,
    FLOW_LIST_START,
    FLOW_LIST_END,
    FLOW_MAP_START,
    FLOW_MAP_END,
    /* A block item's `-`. */
    ITEM,
    /* A flow collection's `,`. */
    COMMA,
    NAME,
    VALUE,
    SCALAR,
} token_kind;

struct markdown_core_yaml_token {
    token_kind kind;
    markdown_core_yaml_mark start, end;
    markdown_core_yaml_style style;
    unsigned char *text;
    size_t size, from, to;
};

/* A place a simple name may start, at one flow depth: libyaml's simple
 * key. */
struct markdown_core_yaml_candidate {
    bool possible, required;
    size_t number;
    markdown_core_yaml_mark mark;
};

typedef markdown_core_yaml_parser parser;
typedef markdown_core_yaml_token token;
typedef markdown_core_yaml_candidate candidate;
typedef markdown_core_yaml_mark mark;
typedef markdown_core_yaml_event event;

/* THE TEXT, read in place. A byte past the end reads as -1. */
static int peek(const parser *p, size_t k) {
    const size_t at = p->mark.at + k;
    return at < p->end ? p->text[at] : -1;
}
static bool blank(int c) { return c == ' ' || c == '\t'; }
static bool newline(int c) { return c == '\r' || c == '\n'; }
static bool blankz(int c) { return c < 0 || blank(c) || newline(c); }
static bool breakz(int c) { return c < 0 || newline(c); }
static bool among(int c, const char *set) { return c > 0 && strchr(set, c); }
static int hex(int c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
static bool marker(const parser *p, int c) {
    return p->mark.column == 0 && peek(p, 0) == c && peek(p, 1) == c && peek(p, 2) == c && blankz(peek(p, 3));
}

/* The width of the character at the mark, as the validation walked it. */
static size_t width(const parser *p) {
    int32_t scalar;
    return (size_t)markdown_core_utf8proc_decode(p->text + p->mark.at, (bufsize_t)(p->end - p->mark.at), &scalar);
}
static void skip(parser *p) {
    p->mark.at += width(p);
    p->mark.column++;
}
static void skip_line(parser *p) {
    p->mark.at += peek(p, 0) == '\r' && peek(p, 1) == '\n' ? 2 : 1;
    p->mark.line++;
    p->mark.column = 0;
}
/* Appends the character at the mark, a NUL as U+FFFD, and skips it. */
static void take(parser *p, markdown_core_strbuf *text) {
    if (!p->text[p->mark.at]) {
        markdown_core_strbuf_put(text, (const unsigned char *)"\xEF\xBF\xBD", 3);
        skip(p);
        return;
    }
    const size_t at = p->mark.at;
    skip(p);
    markdown_core_strbuf_put(text, p->text + at, (bufsize_t)(p->mark.at - at));
}

static bool fail(parser *p, const char *problem) {
    if (!p->problem) {
        p->problem = problem;
        p->place = p->mark;
    }
    return false;
}
static bool exhausted(parser *p) {
    p->failed = true;
    return fail(p, "an allocation failed");
}

/* A scalar's text given up on a problem: an allocation that failed while
 * it was read is the problem, whatever the text goes on to be. */
static bool abandon(parser *p, markdown_core_strbuf *text, const char *problem) {
    const bool oom = text->oom != 0;
    markdown_core_strbuf_free(text);
    return oom ? exhausted(p) : fail(p, problem);
}

/* THE QUEUE of tokens the scanner has read and the parser has not taken. */
static void token_free(token *t) {
    markdown_core_free(t->text);
    t->text = NULL;
}
static bool insert(parser *p, size_t index, token t) {
    token *grown = markdown_core_reserve(p->tokens, &p->room, p->tail + 1, sizeof(*p->tokens));
    if (!grown) {
        token_free(&t);
        return exhausted(p);
    }
    p->tokens = grown;
    memmove(p->tokens + p->head + index + 1, p->tokens + p->head + index,
            (p->tail - p->head - index) * sizeof(*p->tokens));
    p->tokens[p->head + index] = t;
    p->tail++;
    return true;
}
static bool enqueue(parser *p, token t) { return insert(p, p->tail - p->head, t); }
static bool simple(parser *p, token_kind kind, mark start, mark end) {
    return enqueue(p, (token){.kind = kind, .start = start, .end = end});
}

static bool push_int(parser *p, int **values, size_t *count, size_t *room, int value) {
    int *grown = markdown_core_reserve(*values, room, *count + 1, sizeof(**values));
    if (!grown) {
        return exhausted(p);
    }
    *values = grown;
    grown[(*count)++] = value;
    return true;
}

/* THE SIMPLE NAMES. A scalar or a flow collection that a `:` follows on its
 * line within 1024 characters is a name; the scanner keeps where each one
 * that may be starts, and a `:` turns it into a NAME token before it. */
static bool stale(parser *p) {
    for (size_t i = 0; i < p->candidate_count; i++) {
        candidate *c = &p->candidates[i];
        if (c->possible && (c->mark.line < p->mark.line || c->mark.column + NAME_LIMIT < p->mark.column)) {
            if (c->required) {
                p->mark = c->mark;
                return fail(p, "could not find expected ':'");
            }
            c->possible = false;
        }
    }
    return true;
}
static bool remove_candidate(parser *p) {
    candidate *c = &p->candidates[p->candidate_count - 1];
    if (c->possible && c->required) {
        return fail(p, "could not find expected ':'");
    }
    c->possible = false;
    return true;
}
static bool save_candidate(parser *p) {
    /* A name is required where a block collection's indentation puts it. */
    const bool required = !p->depth && p->indent == (int)p->mark.column;
    if (p->allowed) {
        const candidate c = {true, required, p->parsed + (p->tail - p->head), p->mark};
        if (!remove_candidate(p)) {
            return false;
        }
        p->candidates[p->candidate_count - 1] = c;
    }
    return true;
}
static bool push_candidate(parser *p) {
    candidate *grown =
        markdown_core_reserve(p->candidates, &p->candidate_room, p->candidate_count + 1, sizeof(*p->candidates));
    if (!grown) {
        return exhausted(p);
    }
    p->candidates = grown;
    p->candidates[p->candidate_count++] = (candidate){0};
    return true;
}

static bool deeper(parser *p) {
    if (!push_candidate(p)) {
        return false;
    }
    if (p->indent_count >= (size_t)(NESTING - p->depth)) {
        return fail(p, "exceeded maximum nesting depth");
    }
    p->depth++;
    return true;
}
static void shallower(parser *p) {
    if (p->depth) {
        p->depth--;
        p->candidate_count--;
    }
}

/* THE INDENTATION of block collections. A token at a deeper column opens a
 * collection, its start token queued at `number`, or last when -1; a token
 * at a shallower one closes each collection deeper than it. */
static bool roll(parser *p, int column, ptrdiff_t number, token_kind kind, mark at) {
    if (p->depth || p->indent >= column) {
        return true;
    }
    if (!push_int(p, &p->indents, &p->indent_count, &p->indent_room, p->indent)) {
        return false;
    }
    if (p->indent_count > (size_t)(NESTING - p->depth)) {
        return fail(p, "exceeded maximum nesting depth");
    }
    p->indent = column;
    const token t = {.kind = kind, .start = at, .end = at};
    return number < 0 ? enqueue(p, t) : insert(p, (size_t)number - p->parsed, t);
}
static bool unroll(parser *p, int column) {
    if (p->depth) {
        return true;
    }
    while (p->indent > column) {
        if (!simple(p, BLOCK_END, p->mark, p->mark)) {
            return false;
        }
        p->indent = p->indents[--p->indent_count];
    }
    return true;
}

/* THE TOKENS. */
static bool fetch_stream_start(parser *p) {
    p->indent = -1;
    if (!push_candidate(p)) {
        return false;
    }
    p->allowed = true;
    p->started = true;
    return simple(p, STREAM_START, p->mark, p->mark);
}
static bool fetch_stream_end(parser *p) {
    if (p->mark.column != 0) {
        p->mark.column = 0;
        p->mark.line++;
    }
    if (!unroll(p, -1) || !remove_candidate(p)) {
        return false;
    }
    p->allowed = false;
    return simple(p, STREAM_END, p->mark, p->mark);
}
static bool fetch_document_marker(parser *p, token_kind kind) {
    if (!unroll(p, -1) || !remove_candidate(p)) {
        return false;
    }
    p->allowed = false;
    const mark start = p->mark;
    skip(p);
    skip(p);
    skip(p);
    return simple(p, kind, start, p->mark);
}
static bool fetch_flow_start(parser *p, token_kind kind) {
    if (!save_candidate(p) || !deeper(p)) {
        return false;
    }
    p->allowed = true;
    const mark start = p->mark;
    skip(p);
    return simple(p, kind, start, p->mark);
}
static bool fetch_flow_end(parser *p, token_kind kind) {
    if (!remove_candidate(p)) {
        return false;
    }
    shallower(p);
    p->allowed = false;
    const mark start = p->mark;
    skip(p);
    return simple(p, kind, start, p->mark);
}
static bool fetch_comma(parser *p) {
    if (!remove_candidate(p)) {
        return false;
    }
    p->allowed = true;
    const mark start = p->mark;
    skip(p);
    return simple(p, COMMA, start, p->mark);
}
static bool fetch_item(parser *p) {
    if (!p->depth) {
        if (!p->allowed) {
            return fail(p, "block sequence entries are not allowed in this context");
        }
        if (!roll(p, (int)p->mark.column, -1, BLOCK_LIST_START, p->mark)) {
            return false;
        }
    }
    if (!remove_candidate(p)) {
        return false;
    }
    p->allowed = true;
    const mark start = p->mark;
    skip(p);
    return simple(p, ITEM, start, p->mark);
}
static bool fetch_value(parser *p) {
    candidate *c = &p->candidates[p->candidate_count - 1];
    if (c->possible) {
        /* The candidate was a name: its NAME token, and the block mapping it
         * opens, go before it. */
        if (!insert(p, c->number - p->parsed, (token){.kind = NAME, .start = c->mark, .end = c->mark}) ||
            !roll(p, (int)c->mark.column, (ptrdiff_t)c->number, BLOCK_MAP_START, c->mark)) {
            return false;
        }
        c->possible = false;
        p->allowed = false;
    } else {
        if (!p->depth) {
            if (!p->allowed) {
                return fail(p, "mapping values are not allowed in this context");
            }
            if (!roll(p, (int)p->mark.column, -1, BLOCK_MAP_START, p->mark)) {
                return false;
            }
        }
        p->allowed = !p->depth;
    }
    const mark start = p->mark;
    skip(p);
    return simple(p, VALUE, start, p->mark);
}

static bool comment(parser *p) {
    markdown_core_yaml_span *grown =
        markdown_core_reserve(p->comments, &p->comment_room, p->comment_count + 1, sizeof(*p->comments));
    if (!grown) {
        return exhausted(p);
    }
    p->comments = grown;
    const size_t from = p->mark.at;
    while (!breakz(peek(p, 0))) {
        skip(p);
    }
    p->comments[p->comment_count++] = (markdown_core_yaml_span){from, p->mark.at};
    return true;
}

/* A scalar between apostrophes or double quotes, on one line. */
static bool scan_quoted(parser *p, token *t, bool apostrophe) {
    const mark start = p->mark;
    const int quote = apostrophe ? '\'' : '"';
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    skip(p);
    const size_t from = p->mark.at;
    for (;;) {
        const int c = peek(p, 0);
        if (c < 0) {
            return abandon(p, &text, "found unexpected end of stream");
        }
        if (newline(c)) {
            return abandon(p, &text, "found a line break in a quoted scalar");
        }
        if (apostrophe && c == '\'' && peek(p, 1) == '\'') {
            markdown_core_strbuf_putc(&text, '\'');
            skip(p);
            skip(p);
        } else if (c == quote) {
            break;
        } else if (!apostrophe && c == '\\') {
            /* JSON's escapes, with a surrogate pair written as two. */
            const char *found = among(peek(p, 1), "btnfr\"/\\") ? strchr("btnfr\"/\\", peek(p, 1)) : NULL;
            if (found) {
                markdown_core_strbuf_putc(&text, (unsigned char)"\b\t\n\f\r\"/\\"[found - "btnfr\"/\\"]);
                skip(p);
                skip(p);
                continue;
            }
            uint32_t scalar = 0;
            for (int half = 0; half < 2; half++) {
                uint32_t code = 0;
                if (peek(p, 0) != '\\' || peek(p, 1) != 'u') {
                    return abandon(p, &text,
                                   half ? "found an unpaired surrogate escape" : "found unknown escape character");
                }
                for (size_t k = 2; k < 6; k++) {
                    const int digit = hex(peek(p, k));
                    if (digit < 0) {
                        return abandon(p, &text, "did not find expected hexdecimal number");
                    }
                    code = (code << 4) | (uint32_t)digit;
                }
                for (size_t k = 0; k < 6; k++) {
                    skip(p);
                }
                if (half) {
                    if (code < 0xDC00 || code > 0xDFFF) {
                        return abandon(p, &text, "found an unpaired surrogate escape");
                    }
                    scalar = 0x10000 + ((scalar - 0xD800) << 10) + (code - 0xDC00);
                    break;
                }
                scalar = code;
                if (code >= 0xDC00 && code <= 0xDFFF) {
                    return abandon(p, &text, "found an unpaired surrogate escape");
                }
                if (code < 0xD800 || code > 0xDBFF) {
                    break;
                }
            }
            markdown_core_utf8proc_encode_char((int32_t)scalar, &text);
        } else {
            take(p, &text);
        }
    }
    const size_t to = p->mark.at;
    skip(p);
    if (text.oom) {
        markdown_core_strbuf_free(&text);
        return exhausted(p);
    }
    const size_t size = (size_t)text.size;
    *t = (token){SCALAR,
                 start,
                 p->mark,
                 apostrophe ? MARKDOWN_CORE_YAML_APOSTROPHE : MARKDOWN_CORE_YAML_QUOTE,
                 size ? markdown_core_strbuf_detach(&text) : NULL,
                 size,
                 from,
                 to};
    markdown_core_strbuf_free(&text);
    return !size || t->text || exhausted(p);
}

/* A plain scalar, which its line ends: a scalar that would go on to the
 * next line is no scalar of the language, and the next line reads as the
 * tokens it holds. */
static bool scan_plain(parser *p, token *t) {
    const mark start = p->mark;
    mark end = p->mark;
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    size_t spaces = 0;
    for (;;) {
        if (marker(p, '-') || marker(p, '.') || peek(p, 0) == '#') {
            break;
        }
        while (!blankz(peek(p, 0))) {
            const int c = peek(p, 0);
            if (p->depth && c == ':' && among(peek(p, 1), ",?[]{}")) {
                return abandon(p, &text, "found unexpected ':'");
            }
            if ((c == ':' && blankz(peek(p, 1))) || (p->depth && among(c, ",[]{}"))) {
                break;
            }
            if (spaces) {
                markdown_core_strbuf_put(&text, p->text + end.at, (bufsize_t)spaces);
                spaces = 0;
            }
            take(p, &text);
            end = p->mark;
        }
        if (!blank(peek(p, 0))) {
            break;
        }
        while (blank(peek(p, 0))) {
            skip(p);
        }
        spaces = p->mark.at - end.at;
    }
    if (text.oom) {
        markdown_core_strbuf_free(&text);
        return exhausted(p);
    }
    const size_t size = (size_t)text.size;
    *t = (token){SCALAR, start,    end,   MARKDOWN_CORE_YAML_PLAIN, size ? markdown_core_strbuf_detach(&text) : NULL,
                 size,   start.at, end.at};
    markdown_core_strbuf_free(&text);
    return !size || t->text || exhausted(p);
}

/* A literal: a bare `|`, then the lines its first nonblank line's
 * indentation holds, which must be deeper than the collection's. Each
 * line keeps what follows that indentation; a blank line is an empty one;
 * the text ends with one line ending after its last nonblank line. A
 * nonblank line indented less ends it. */
static bool scan_literal(parser *p, token *t) {
    const mark start = p->mark;
    skip(p);
    if (!blankz(peek(p, 0))) {
        return fail(p, "only a bare '|' is supported");
    }
    while (blank(peek(p, 0))) {
        skip(p);
    }
    if (peek(p, 0) == '#' && !comment(p)) {
        return false;
    }
    if (!breakz(peek(p, 0))) {
        return fail(p, "did not find expected comment or line break");
    }
    if (newline(peek(p, 0))) {
        skip_line(p);
    }
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    size_t indent = 0, clipped = 0, from = p->mark.at, to = p->mark.at;
    bool first = true;
    while (peek(p, 0) >= 0) {
        size_t spaces = 0, content = 0;
        while (peek(p, spaces) == ' ') {
            spaces++;
        }
        for (content = spaces; blank(peek(p, content)); content++) {
        }
        if (breakz(peek(p, content))) {
            for (size_t k = 0; k < content; k++) {
                skip(p);
            }
            markdown_core_strbuf_putc(&text, '\n');
            if (newline(peek(p, 0))) {
                skip_line(p);
            }
            continue;
        }
        if (!indent) {
            /* Deeper than the collection, and at least one space deep. */
            if ((int)spaces <= p->indent || !spaces) {
                break;
            }
            indent = spaces;
        }
        if (spaces < indent) {
            break;
        }
        for (size_t k = 0; k < indent; k++) {
            skip(p);
        }
        if (first) {
            from = p->mark.at;
            first = false;
        }
        while (!breakz(peek(p, 0))) {
            take(p, &text);
        }
        to = p->mark.at;
        markdown_core_strbuf_putc(&text, '\n');
        clipped = (size_t)text.size;
        if (newline(peek(p, 0))) {
            skip_line(p);
        }
    }
    if (first) {
        from = to = start.at;
    }
    markdown_core_strbuf_truncate(&text, (bufsize_t)clipped);
    if (text.oom) {
        markdown_core_strbuf_free(&text);
        return exhausted(p);
    }
    *t = (token){
        SCALAR,  start, p->mark, MARKDOWN_CORE_YAML_LITERAL, clipped ? markdown_core_strbuf_detach(&text) : NULL,
        clipped, from,  to};
    markdown_core_strbuf_free(&text);
    return !clipped || t->text || exhausted(p);
}

static bool fetch_scalar(parser *p, int c) {
    token t;
    if (c == '|') {
        if (!remove_candidate(p)) {
            return false;
        }
        p->allowed = true;
        if (!scan_literal(p, &t)) {
            return false;
        }
    } else {
        if (!save_candidate(p)) {
            return false;
        }
        p->allowed = false;
        if (!(c == '\'' || c == '"' ? scan_quoted(p, &t, c == '\'') : scan_plain(p, &t))) {
            return false;
        }
    }
    return enqueue(p, t);
}

/* Whether only blanks and a comment follow the mark on its line. */
static bool idle(const parser *p) {
    size_t k = 0;
    while (blank(peek(p, k))) {
        k++;
    }
    return breakz(peek(p, k)) || peek(p, k) == '#';
}
/* Whether a `#` at the mark starts a comment: a blank or a line start
 * comes before it. */
static bool separated(const parser *p) { return p->mark.column == 0 || blank(p->text[p->mark.at - 1]); }

/* Skips spaces, comments and line breaks to the next token. In a block
 * collection a tab may only follow its line's first token, or lead to a
 * comment or the line's end. */
static bool scan_to_next(parser *p) {
    for (;;) {
        while (peek(p, 0) == ' ' || (peek(p, 0) == '\t' && (p->depth || !p->allowed || idle(p)))) {
            skip(p);
        }
        if (peek(p, 0) == '#' && separated(p) && !comment(p)) {
            return false;
        }
        if (!newline(peek(p, 0))) {
            return true;
        }
        skip_line(p);
        if (!p->depth) {
            p->allowed = true;
        }
    }
}

static bool fetch_next(parser *p) {
    if (!p->started) {
        return fetch_stream_start(p);
    }
    if (!scan_to_next(p) || !stale(p) || !unroll(p, (int)p->mark.column)) {
        return false;
    }
    const int c = peek(p, 0);
    if (c < 0) {
        return fetch_stream_end(p);
    }
    if (p->mark.column == 0 && c == '%') {
        return fail(p, "directives are unsupported");
    }
    if (marker(p, '-')) {
        return fetch_document_marker(p, DOCUMENT_START);
    }
    if (marker(p, '.')) {
        return fetch_document_marker(p, DOCUMENT_END);
    }
    switch (c) {
    case '[':
        return fetch_flow_start(p, FLOW_LIST_START);
    case '{':
        return fetch_flow_start(p, FLOW_MAP_START);
    case ']':
        return fetch_flow_end(p, FLOW_LIST_END);
    case '}':
        return fetch_flow_end(p, FLOW_MAP_END);
    case ',':
        return fetch_comma(p);
    case '*':
        return fail(p, "aliases are unsupported");
    case '&':
        return fail(p, "anchors are unsupported");
    case '!':
        return fail(p, "tags are unsupported");
    default:
        break;
    }
    const int next = peek(p, 1);
    /* A `- ` where no block item may begin, as after a name's `:` on its
     * line, is plain text: `authors: - Ada` is the text "- Ada". */
    if (c == '-' && blankz(next) && (p->depth || p->allowed)) {
        return fetch_item(p);
    }
    if (c == '?' && (p->depth || blankz(next))) {
        return fail(p, "explicit names are unsupported");
    }
    if (c == ':' && (p->depth || blankz(next))) {
        return fetch_value(p);
    }
    if (c == '|' && !p->depth) {
        return fetch_scalar(p, c);
    }
    if (c == '>' && !p->depth) {
        return fail(p, "folded scalars are unsupported");
    }
    if (c == '\'' || c == '"') {
        return fetch_scalar(p, c);
    }
    /* A dash left here starts plain text: one no blank follows, or one
     * where no block item may begin. */
    if (!(blankz(c) || among(c, "-?:,[]{}#&*!|>'\"%@`")) || c == '-' ||
        (!p->depth && (c == '?' || c == ':') && !blankz(next))) {
        return fetch_scalar(p, c);
    }
    return fail(p, "found character that cannot start any token");
}

static bool fetch_more(parser *p) {
    for (;;) {
        bool need = p->head == p->tail;
        if (!need) {
            if (!stale(p)) {
                return false;
            }
            for (size_t i = 0; i < p->candidate_count && !need; i++) {
                need = p->candidates[i].possible && p->candidates[i].number == p->parsed;
            }
        }
        if (!need) {
            break;
        }
        if (!fetch_next(p)) {
            return false;
        }
    }
    p->available = true;
    return true;
}

static token *peek_token(parser *p) {
    if (p->problem || (!p->available && !fetch_more(p))) {
        return NULL;
    }
    return &p->tokens[p->head];
}
static void skip_token(parser *p) {
    p->available = false;
    p->parsed++;
    token_free(&p->tokens[p->head]);
    if (++p->head == p->tail) {
        p->head = p->tail = 0;
    }
}

/* THE PARSER, libyaml's state machine. */
enum {
    PARSE_STREAM_START,
    PARSE_IMPLICIT_DOCUMENT_START,
    PARSE_DOCUMENT_START,
    PARSE_DOCUMENT_CONTENT,
    PARSE_DOCUMENT_END,
    PARSE_BLOCK_NODE,
    PARSE_BLOCK_NODE_OR_INDENTLESS_LIST,
    PARSE_FLOW_NODE,
    PARSE_BLOCK_LIST_FIRST_ITEM,
    PARSE_BLOCK_LIST_ITEM,
    PARSE_INDENTLESS_LIST_ITEM,
    PARSE_BLOCK_MAP_FIRST_NAME,
    PARSE_BLOCK_MAP_NAME,
    PARSE_BLOCK_MAP_VALUE,
    PARSE_FLOW_LIST_FIRST_ITEM,
    PARSE_FLOW_LIST_ITEM,
    PARSE_FLOW_LIST_ITEM_MAP_NAME,
    PARSE_FLOW_LIST_ITEM_MAP_VALUE,
    PARSE_FLOW_LIST_ITEM_MAP_END,
    PARSE_FLOW_MAP_FIRST_NAME,
    PARSE_FLOW_MAP_NAME,
    PARSE_FLOW_MAP_VALUE,
    PARSE_FLOW_MAP_ABSENT_VALUE,
    PARSE_END,
};

static bool parse_error(parser *p, const char *problem, mark at) {
    p->mark = at;
    return fail(p, problem);
}
static bool push_state(parser *p, int state) { return push_int(p, &p->states, &p->state_count, &p->state_room, state); }
static int pop_state(parser *p) { return p->states[--p->state_count]; }
static bool push_mark(parser *p, mark at) {
    mark *grown = markdown_core_reserve(p->marks, &p->mark_room, p->mark_count + 1, sizeof(*p->marks));
    if (!grown) {
        return exhausted(p);
    }
    p->marks = grown;
    p->marks[p->mark_count++] = at;
    return true;
}
static mark pop_mark(parser *p) { return p->marks[--p->mark_count]; }

static void emit(event *e, markdown_core_yaml_kind kind, mark start, mark end) {
    *e = (event){.kind = kind, .start = start, .end = end};
}
/* An empty plain scalar where a node is left out. */
static bool absent(event *e, mark at) {
    emit(e, MARKDOWN_CORE_YAML_SCALAR, at, at);
    e->from = e->to = at.at;
    return true;
}

static bool parse_node(parser *p, event *e, bool block, bool indentless) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (indentless && t->kind == ITEM) {
        p->state = PARSE_INDENTLESS_LIST_ITEM;
        emit(e, MARKDOWN_CORE_YAML_LIST_START, t->start, t->end);
        return true;
    }
    switch (t->kind) {
    case SCALAR:
        p->state = pop_state(p);
        emit(e, MARKDOWN_CORE_YAML_SCALAR, t->start, t->end);
        e->style = t->style;
        e->text = t->text;
        e->size = t->size;
        e->from = t->from;
        e->to = t->to;
        t->text = NULL;
        skip_token(p);
        return true;
    case FLOW_LIST_START:
        p->state = PARSE_FLOW_LIST_FIRST_ITEM;
        emit(e, MARKDOWN_CORE_YAML_LIST_START, t->start, t->end);
        e->flow = true;
        return true;
    case FLOW_MAP_START:
        p->state = PARSE_FLOW_MAP_FIRST_NAME;
        emit(e, MARKDOWN_CORE_YAML_MAP_START, t->start, t->end);
        e->flow = true;
        return true;
    case BLOCK_LIST_START:
        if (block) {
            p->state = PARSE_BLOCK_LIST_FIRST_ITEM;
            emit(e, MARKDOWN_CORE_YAML_LIST_START, t->start, t->end);
            return true;
        }
        break;
    case BLOCK_MAP_START:
        if (block) {
            p->state = PARSE_BLOCK_MAP_FIRST_NAME;
            emit(e, MARKDOWN_CORE_YAML_MAP_START, t->start, t->end);
            return true;
        }
        break;
    default:
        break;
    }
    return parse_error(p, "did not find expected node content", t->start);
}

static bool parse_document_start(parser *p, event *e, bool implicit) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (!implicit) {
        while (t->kind == DOCUMENT_END) {
            skip_token(p);
            if (!(t = peek_token(p))) {
                return false;
            }
        }
    }
    if (implicit && t->kind != DOCUMENT_START && t->kind != STREAM_END) {
        if (!push_state(p, PARSE_DOCUMENT_END)) {
            return false;
        }
        p->state = PARSE_BLOCK_NODE;
        emit(e, MARKDOWN_CORE_YAML_DOCUMENT_START, t->start, t->start);
        return true;
    }
    if (t->kind != STREAM_END) {
        if (t->kind != DOCUMENT_START) {
            return parse_error(p, "did not find expected <document start>", t->start);
        }
        if (!push_state(p, PARSE_DOCUMENT_END)) {
            return false;
        }
        p->state = PARSE_DOCUMENT_CONTENT;
        emit(e, MARKDOWN_CORE_YAML_DOCUMENT_START, t->start, t->end);
        e->explicit = true;
        skip_token(p);
        return true;
    }
    p->state = PARSE_END;
    emit(e, MARKDOWN_CORE_YAML_STREAM_END, t->start, t->end);
    skip_token(p);
    return true;
}

static bool parse_document_content(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (t->kind == DOCUMENT_START || t->kind == DOCUMENT_END || t->kind == STREAM_END) {
        p->state = pop_state(p);
        return absent(e, t->start);
    }
    return parse_node(p, e, true, false);
}

static bool parse_document_end(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    const mark start = t->start;
    mark end = t->start;
    bool explicit = false;
    if (t->kind == DOCUMENT_END) {
        end = t->end;
        skip_token(p);
        explicit = true;
    }
    p->state = PARSE_DOCUMENT_START;
    emit(e, MARKDOWN_CORE_YAML_DOCUMENT_END, start, end);
    e->explicit = explicit;
    return true;
}

static bool parse_block_list_item(parser *p, event *e, bool first) {
    token *t;
    if (first) {
        t = peek_token(p);
        if (!push_mark(p, t->start)) {
            return false;
        }
        skip_token(p);
    }
    if (!(t = peek_token(p))) {
        return false;
    }
    if (t->kind == ITEM) {
        const mark at = t->end;
        skip_token(p);
        if (!(t = peek_token(p))) {
            return false;
        }
        if (t->kind != ITEM && t->kind != BLOCK_END) {
            return push_state(p, PARSE_BLOCK_LIST_ITEM) && parse_node(p, e, true, false);
        }
        p->state = PARSE_BLOCK_LIST_ITEM;
        return absent(e, at);
    }
    if (t->kind == BLOCK_END) {
        p->state = pop_state(p);
        pop_mark(p);
        emit(e, MARKDOWN_CORE_YAML_LIST_END, t->start, t->end);
        skip_token(p);
        return true;
    }
    pop_mark(p);
    return parse_error(p, "did not find expected '-' indicator", t->start);
}

static bool parse_indentless_list_item(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (t->kind == ITEM) {
        const mark at = t->end;
        skip_token(p);
        if (!(t = peek_token(p))) {
            return false;
        }
        if (t->kind != ITEM && t->kind != NAME && t->kind != VALUE && t->kind != BLOCK_END) {
            return push_state(p, PARSE_INDENTLESS_LIST_ITEM) && parse_node(p, e, true, false);
        }
        p->state = PARSE_INDENTLESS_LIST_ITEM;
        return absent(e, at);
    }
    p->state = pop_state(p);
    emit(e, MARKDOWN_CORE_YAML_LIST_END, t->start, t->start);
    return true;
}

static bool parse_block_map_name(parser *p, event *e, bool first) {
    token *t;
    if (first) {
        t = peek_token(p);
        if (!push_mark(p, t->start)) {
            return false;
        }
        skip_token(p);
    }
    if (!(t = peek_token(p))) {
        return false;
    }
    if (t->kind == NAME) {
        const mark at = t->end;
        skip_token(p);
        if (!(t = peek_token(p))) {
            return false;
        }
        if (t->kind != NAME && t->kind != VALUE && t->kind != BLOCK_END) {
            return push_state(p, PARSE_BLOCK_MAP_VALUE) && parse_node(p, e, true, true);
        }
        p->state = PARSE_BLOCK_MAP_VALUE;
        return absent(e, at);
    }
    if (t->kind == BLOCK_END) {
        p->state = pop_state(p);
        pop_mark(p);
        emit(e, MARKDOWN_CORE_YAML_MAP_END, t->start, t->end);
        skip_token(p);
        return true;
    }
    pop_mark(p);
    return parse_error(p, "did not find expected key", t->start);
}

static bool parse_block_map_value(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (t->kind == VALUE) {
        const mark at = t->end;
        skip_token(p);
        if (!(t = peek_token(p))) {
            return false;
        }
        if (t->kind != NAME && t->kind != VALUE && t->kind != BLOCK_END) {
            return push_state(p, PARSE_BLOCK_MAP_NAME) && parse_node(p, e, true, true);
        }
        p->state = PARSE_BLOCK_MAP_NAME;
        return absent(e, at);
    }
    p->state = PARSE_BLOCK_MAP_NAME;
    return absent(e, t->start);
}

static bool parse_flow_list_item(parser *p, event *e, bool first) {
    token *t;
    if (first) {
        t = peek_token(p);
        if (!push_mark(p, t->start)) {
            return false;
        }
        skip_token(p);
    }
    if (!(t = peek_token(p))) {
        return false;
    }
    if (t->kind != FLOW_LIST_END) {
        if (!first) {
            if (t->kind != COMMA) {
                pop_mark(p);
                return parse_error(p, "did not find expected ',' or ']'", t->start);
            }
            skip_token(p);
            if (!(t = peek_token(p))) {
                return false;
            }
        }
        if (t->kind == NAME) {
            p->state = PARSE_FLOW_LIST_ITEM_MAP_NAME;
            emit(e, MARKDOWN_CORE_YAML_MAP_START, t->start, t->end);
            e->flow = true;
            skip_token(p);
            return true;
        }
        if (t->kind != FLOW_LIST_END) {
            return push_state(p, PARSE_FLOW_LIST_ITEM) && parse_node(p, e, false, false);
        }
    }
    p->state = pop_state(p);
    pop_mark(p);
    emit(e, MARKDOWN_CORE_YAML_LIST_END, t->start, t->end);
    skip_token(p);
    return true;
}

static bool parse_flow_list_item_map_name(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (t->kind != VALUE && t->kind != COMMA && t->kind != FLOW_LIST_END) {
        return push_state(p, PARSE_FLOW_LIST_ITEM_MAP_VALUE) && parse_node(p, e, false, false);
    }
    p->state = PARSE_FLOW_LIST_ITEM_MAP_VALUE;
    if (t->kind == FLOW_LIST_END) {
        return absent(e, t->start);
    }
    const mark at = t->end;
    skip_token(p);
    return absent(e, at);
}

static bool parse_flow_list_item_map_value(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (t->kind == VALUE) {
        skip_token(p);
        if (!(t = peek_token(p))) {
            return false;
        }
        if (t->kind != COMMA && t->kind != FLOW_LIST_END) {
            return push_state(p, PARSE_FLOW_LIST_ITEM_MAP_END) && parse_node(p, e, false, false);
        }
    }
    p->state = PARSE_FLOW_LIST_ITEM_MAP_END;
    return absent(e, t->start);
}

static bool parse_flow_list_item_map_end(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    p->state = PARSE_FLOW_LIST_ITEM;
    emit(e, MARKDOWN_CORE_YAML_MAP_END, t->start, t->start);
    return true;
}

static bool parse_flow_map_name(parser *p, event *e, bool first) {
    token *t;
    if (first) {
        t = peek_token(p);
        if (!push_mark(p, t->start)) {
            return false;
        }
        skip_token(p);
    }
    if (!(t = peek_token(p))) {
        return false;
    }
    if (t->kind != FLOW_MAP_END) {
        if (!first) {
            if (t->kind != COMMA) {
                pop_mark(p);
                return parse_error(p, "did not find expected ',' or '}'", t->start);
            }
            skip_token(p);
            if (!(t = peek_token(p))) {
                return false;
            }
        }
        if (t->kind == NAME) {
            skip_token(p);
            if (!(t = peek_token(p))) {
                return false;
            }
            if (t->kind != VALUE && t->kind != COMMA && t->kind != FLOW_MAP_END) {
                return push_state(p, PARSE_FLOW_MAP_VALUE) && parse_node(p, e, false, false);
            }
            p->state = PARSE_FLOW_MAP_VALUE;
            return absent(e, t->start);
        }
        if (t->kind != FLOW_MAP_END) {
            return push_state(p, PARSE_FLOW_MAP_ABSENT_VALUE) && parse_node(p, e, false, false);
        }
    }
    p->state = pop_state(p);
    pop_mark(p);
    emit(e, MARKDOWN_CORE_YAML_MAP_END, t->start, t->end);
    skip_token(p);
    return true;
}

static bool parse_flow_map_value(parser *p, event *e, bool missing) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    if (missing) {
        p->state = PARSE_FLOW_MAP_NAME;
        return absent(e, t->start);
    }
    if (t->kind == VALUE) {
        skip_token(p);
        if (!(t = peek_token(p))) {
            return false;
        }
        if (t->kind != COMMA && t->kind != FLOW_MAP_END) {
            return push_state(p, PARSE_FLOW_MAP_NAME) && parse_node(p, e, false, false);
        }
    }
    p->state = PARSE_FLOW_MAP_NAME;
    return absent(e, t->start);
}

static bool parse_stream_start(parser *p, event *e) {
    token *t = peek_token(p);
    if (!t) {
        return false;
    }
    p->state = PARSE_IMPLICIT_DOCUMENT_START;
    emit(e, MARKDOWN_CORE_YAML_STREAM_START, t->start, t->start);
    skip_token(p);
    return true;
}

static bool state_machine(parser *p, event *e) {
    switch (p->state) {
    case PARSE_STREAM_START:
        return parse_stream_start(p, e);
    case PARSE_IMPLICIT_DOCUMENT_START:
        return parse_document_start(p, e, true);
    case PARSE_DOCUMENT_START:
        return parse_document_start(p, e, false);
    case PARSE_DOCUMENT_CONTENT:
        return parse_document_content(p, e);
    case PARSE_DOCUMENT_END:
        return parse_document_end(p, e);
    case PARSE_BLOCK_NODE:
        return parse_node(p, e, true, false);
    case PARSE_BLOCK_NODE_OR_INDENTLESS_LIST:
        return parse_node(p, e, true, true);
    case PARSE_FLOW_NODE:
        return parse_node(p, e, false, false);
    case PARSE_BLOCK_LIST_FIRST_ITEM:
        return parse_block_list_item(p, e, true);
    case PARSE_BLOCK_LIST_ITEM:
        return parse_block_list_item(p, e, false);
    case PARSE_INDENTLESS_LIST_ITEM:
        return parse_indentless_list_item(p, e);
    case PARSE_BLOCK_MAP_FIRST_NAME:
        return parse_block_map_name(p, e, true);
    case PARSE_BLOCK_MAP_NAME:
        return parse_block_map_name(p, e, false);
    case PARSE_BLOCK_MAP_VALUE:
        return parse_block_map_value(p, e);
    case PARSE_FLOW_LIST_FIRST_ITEM:
        return parse_flow_list_item(p, e, true);
    case PARSE_FLOW_LIST_ITEM:
        return parse_flow_list_item(p, e, false);
    case PARSE_FLOW_LIST_ITEM_MAP_NAME:
        return parse_flow_list_item_map_name(p, e);
    case PARSE_FLOW_LIST_ITEM_MAP_VALUE:
        return parse_flow_list_item_map_value(p, e);
    case PARSE_FLOW_LIST_ITEM_MAP_END:
        return parse_flow_list_item_map_end(p, e);
    case PARSE_FLOW_MAP_FIRST_NAME:
        return parse_flow_map_name(p, e, true);
    case PARSE_FLOW_MAP_NAME:
        return parse_flow_map_name(p, e, false);
    case PARSE_FLOW_MAP_VALUE:
        return parse_flow_map_value(p, e, false);
    case PARSE_FLOW_MAP_ABSENT_VALUE:
        return parse_flow_map_value(p, e, true);
    default:
        return fail(p, "the stream has ended");
    }
}

/* Whether each character from `start` to `end` is one YAML prints: a tab,
 * a line ending, or a printable scalar, with a NUL read as U+FFFD. */
static bool printable(const unsigned char *text, size_t start, size_t end, size_t *bad) {
    for (size_t i = start; i < end;) {
        int32_t c;
        const int n = markdown_core_utf8proc_decode(text + i, (bufsize_t)(end - i), &c);
        if (!(c == 0 || c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0x7e) || c == 0x85 ||
              (c >= 0xa0 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff))) {
            *bad = i;
            return false;
        }
        i += (size_t)n;
    }
    return true;
}

void markdown_core_yaml_open(parser *p, const unsigned char *text, size_t start, size_t end) {
    for (size_t i = p->head; i < p->tail; i++) {
        token_free(&p->tokens[i]);
    }
    p->text = text;
    p->end = end;
    p->mark = (mark){start, 0, 0};
    p->problem = NULL;
    p->failed = false;
    p->started = p->ended = p->available = p->allowed = false;
    p->head = p->tail = p->parsed = 0;
    p->candidate_count = p->indent_count = p->state_count = p->mark_count = p->comment_count = 0;
    p->indent = -1;
    p->depth = 0;
    p->state = PARSE_STREAM_START;
    size_t bad;
    if (!printable(text, start, end, &bad)) {
        p->mark.at = bad;
        fail(p, "found a character that is not printable");
    }
}

bool markdown_core_yaml_next(parser *p, event *e) {
    *e = (event){0};
    if (p->problem) {
        return false;
    }
    if (p->ended || p->state == PARSE_END) {
        return fail(p, "the stream has ended");
    }
    if (!state_machine(p, e)) {
        markdown_core_yaml_event_free(e);
        return false;
    }
    p->ended = e->kind == MARKDOWN_CORE_YAML_STREAM_END;
    return true;
}

void markdown_core_yaml_event_free(event *e) {
    markdown_core_free(e->text);
    e->text = NULL;
}

void markdown_core_yaml_close(parser *p) {
    for (size_t i = p->head; i < p->tail; i++) {
        token_free(&p->tokens[i]);
    }
    markdown_core_free(p->tokens);
    markdown_core_free(p->candidates);
    markdown_core_free(p->indents);
    markdown_core_free(p->states);
    markdown_core_free(p->marks);
    markdown_core_free(p->comments);
    *p = (parser){0};
}
