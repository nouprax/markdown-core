/* The YAML parser (yaml.h). It reads a text in one pass by recursive
 * descent: each block collection is a call that knows its column, each
 * flow collection a call that knows its depth, and a node that a `:`
 * follows on its line becomes a name once it is read, its mapping's start
 * written before it. The readings are libyaml's (https://github.com/yaml/libyaml)
 * as the properties language takes them: a name may start only where a
 * block item may, or after a flow collection's `[`, `{` or `,`; a name at
 * a block collection's column must be one; an item or a name deeper than
 * its collection opens a nested one, and a token shallower closes it; a
 * value left out reads as an empty plain scalar. */
#include "yaml.h"
#include "alloc.h"
#include "buffer.h"
#include "utf8.h"

#include <stdint.h>
#include <string.h>

/* The deepest nesting of collections, block and flow together. */
#define NESTING 1000
/* The most characters a simple name and the spaces after it may hold. */
#define NAME_LIMIT 1024

typedef markdown_core_yaml_parser parser;
typedef markdown_core_yaml_event event;

/* THE TEXT, read in place. A byte past the end reads as -1. */
static int peek(const parser *p, size_t k) {
    const size_t at = p->at + k;
    return at < p->end ? p->text[at] : -1;
}
static bool blank(int c) { return c == ' ' || c == '\t'; }
static bool newline(int c) { return c == '\r' || c == '\n'; }
static bool blankz(int c) { return c < 0 || blank(c) || newline(c); }
static bool breakz(int c) { return c < 0 || newline(c); }
/* A flow collection's punctuation. */
static bool punctuation(int c) { return c == ',' || c == '[' || c == ']' || c == '{' || c == '}'; }
/* A character that starts a token other than plain text. */
static bool indicator(int c) {
    switch (c) {
    case '-':
    case '?':
    case ':':
    case ',':
    case '[':
    case ']':
    case '{':
    case '}':
    case '#':
    case '&':
    case '*':
    case '!':
    case '|':
    case '>':
    case '\'':
    case '"':
    case '%':
    case '@':
    case '`':
        return true;
    default:
        return false;
    }
}
static int hex(int c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
/* The column of `at`. A block token's column counts the spaces, dashes
 * and indicators before it on its line, which are ASCII, so bytes count
 * characters. */
static size_t column(const parser *p) { return p->at - p->head; }
static bool marker(const parser *p, int c) {
    return p->at == p->head && peek(p, 0) == c && peek(p, 1) == c && peek(p, 2) == c && blankz(peek(p, 3));
}
static void feed(parser *p) {
    p->at += peek(p, 0) == '\r' && peek(p, 1) == '\n' ? 2 : 1;
    p->line++;
    p->head = p->at;
}

static bool fail(parser *p, const char *problem) {
    if (!p->problem) {
        p->problem = problem;
        p->place = p->at;
    }
    return false;
}
static bool exhausted(parser *p) {
    p->failed = true;
    return fail(p, "an allocation failed");
}

/* THE EVENTS, written in order; a mapping's start goes back before the
 * name that opens it. */
static event *insert(parser *p, size_t index, markdown_core_yaml_kind kind) {
    event *wider = markdown_core_reserve(p->events, &p->room, p->count + 1, sizeof(*p->events));
    if (!wider) {
        exhausted(p);
        return NULL;
    }
    p->events = wider;
    memmove(wider + index + 1, wider + index, (p->count - index) * sizeof(*wider));
    p->count++;
    wider[index] = (event){.kind = kind, .line = p->line};
    return wider + index;
}
static event *emit(parser *p, markdown_core_yaml_kind kind) {
    if (p->count == p->room) {
        return insert(p, p->count, kind);
    }
    event *e = p->events + p->count++;
    *e = (event){.kind = kind, .line = p->line};
    return e;
}
/* An empty plain scalar where a node is left out, at `at` on `line`. */
static bool absent(parser *p, size_t index, size_t at, size_t line) {
    event *e = insert(p, index, MARKDOWN_CORE_YAML_SCALAR);
    if (!e) {
        return false;
    }
    e->from = e->to = at;
    e->line = line;
    return true;
}
/* Opens a collection, at most NESTING deep. */
static event *open(parser *p, markdown_core_yaml_kind kind, size_t index, bool flow) {
    if (p->open + p->depth >= NESTING) {
        fail(p, "exceeded maximum nesting depth");
        return NULL;
    }
    event *e = insert(p, index, kind);
    if (e) {
        e->flow = flow;
        if (flow) {
            p->depth++;
        } else {
            p->open++;
        }
    }
    return e;
}
static bool shut(parser *p, markdown_core_yaml_kind kind, bool flow) {
    event *e = emit(p, kind);
    if (e) {
        e->flow = flow;
    }
    return e != NULL;
}

/* Appends the text from `from` to `to`, each NUL as U+FFFD. */
static void put(const parser *p, markdown_core_strbuf *text, size_t from, size_t to) {
    for (;;) {
        const unsigned char *nul = memchr(p->text + from, 0, to - from);
        const size_t stop = nul ? (size_t)(nul - p->text) : to;
        markdown_core_strbuf_put(text, p->text + from, (bufsize_t)(stop - from));
        if (!nul) {
            return;
        }
        markdown_core_strbuf_put(text, (const unsigned char *)"\xEF\xBF\xBD", 3);
        from = stop + 1;
    }
}
/* A scalar's text given up on a problem: an allocation that failed while
 * it was read is the problem, whatever the text goes on to be. */
static bool abandon(parser *p, markdown_core_strbuf *text, const char *problem) {
    const bool oom = text->oom != 0;
    markdown_core_strbuf_free(text);
    return oom ? exhausted(p) : fail(p, problem);
}

/* A SCALAR'S TEXT is the bytes it is written in, read in place, unless
 * reading it changes them: an escape, a doubled apostrophe, a NUL, which
 * reads as U+FFFD, or a literal's indentation and line endings. Then the
 * parser writes the text into a buffer the event owns. */
static event *scalar(parser *p, markdown_core_yaml_style style, size_t from, size_t to, size_t line) {
    event *e = emit(p, MARKDOWN_CORE_YAML_SCALAR);
    if (e) {
        e->style = style;
        e->from = from;
        e->to = to;
        e->line = line;
    }
    return e;
}
/* Moves `text` into the event, which then owns it; an empty text is none. */
static bool own(parser *p, event *e, markdown_core_strbuf *text) {
    if (!e || text->oom) {
        markdown_core_strbuf_free(text);
        return e ? exhausted(p) : false;
    }
    e->size = (size_t)text->size;
    e->owned = e->size ? markdown_core_strbuf_detach(text) : NULL;
    e->text = e->owned;
    markdown_core_strbuf_free(text);
    return !e->size || e->owned || exhausted(p);
}
static bool view(parser *p, event *e) {
    if (!e) {
        return false;
    }
    if (!memchr(p->text + e->from, 0, e->to - e->from)) {
        e->text = e->to > e->from ? p->text + e->from : NULL;
        e->size = e->to - e->from;
        return true;
    }
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    put(p, &text, e->from, e->to);
    return own(p, e, &text);
}

/* Skips to the end of the line a comment starts on, keeping its span. */
static bool comment(parser *p) {
    markdown_core_yaml_span *wider =
        markdown_core_reserve(p->comments, &p->comment_room, p->comment_count + 1, sizeof(*p->comments));
    if (!wider) {
        return exhausted(p);
    }
    p->comments = wider;
    const size_t from = p->at;
    while (!breakz(peek(p, 0))) {
        p->at++;
    }
    p->comments[p->comment_count++] = (markdown_core_yaml_span){from, p->at};
    return true;
}
/* Whether only blanks and a comment follow `at` on its line. */
static bool idle(const parser *p) {
    size_t k = 0;
    while (blank(peek(p, k))) {
        k++;
    }
    return breakz(peek(p, k)) || peek(p, k) == '#';
}
/* Skips spaces, comments and line breaks to the next token. A `#` starts a
 * comment where a blank or the line's start comes before it. In a block
 * collection a tab may only follow its line's first token, or lead to a
 * comment or the line's end. */
static bool pass(parser *p) {
    for (;;) {
        while (peek(p, 0) == ' ' || (peek(p, 0) == '\t' && (p->depth || !p->allowed))) {
            p->at++;
        }
        /* A tab that leads to a comment or the line's end: so does each
         * blank after it. */
        if (peek(p, 0) == '\t' && idle(p)) {
            while (blank(peek(p, 0))) {
                p->at++;
            }
        }
        if (peek(p, 0) == '#' && (p->at == p->head || blank(p->text[p->at - 1])) && !comment(p)) {
            return false;
        }
        if (!newline(peek(p, 0))) {
            return true;
        }
        feed(p);
        if (!p->depth) {
            p->allowed = true;
        }
    }
}

/* THE TOKEN at `at`, which a pass has reached: what starts there. */
typedef enum {
    END,
    BAD,
    DOCUMENT_START,
    DOCUMENT_END,
    LIST_OPEN,
    MAP_OPEN,
    LIST_CLOSE,
    MAP_CLOSE,
    COMMA,
    /* A block item's `-`. */
    ITEM,
    /* A name's `:`. */
    VALUE,
    LITERAL,
    QUOTED,
    PLAIN,
} token;

static token look(parser *p) {
    const int c = peek(p, 0);
    if (c < 0) {
        return END;
    }
    if (p->at == p->head) {
        if (c == '%') {
            fail(p, "directives are unsupported");
            return BAD;
        }
        if (c == '-' && marker(p, '-')) {
            return DOCUMENT_START;
        }
        if (c == '.' && marker(p, '.')) {
            return DOCUMENT_END;
        }
    }
    switch (c) {
    case '[':
        return LIST_OPEN;
    case '{':
        return MAP_OPEN;
    case ']':
        return LIST_CLOSE;
    case '}':
        return MAP_CLOSE;
    case ',':
        return COMMA;
    case '*':
        fail(p, "aliases are unsupported");
        return BAD;
    case '&':
        fail(p, "anchors are unsupported");
        return BAD;
    case '!':
        fail(p, "tags are unsupported");
        return BAD;
    default:
        break;
    }
    const int next = peek(p, 1);
    /* A `- ` where no block item may begin, as after a name's `:` on its
     * line, is plain text: `authors: - Ada` is the text "- Ada". */
    if (c == '-' && blankz(next) && (p->depth || p->allowed)) {
        return ITEM;
    }
    if (c == '?' && (p->depth || blankz(next))) {
        fail(p, "explicit names are unsupported");
        return BAD;
    }
    if (c == ':' && (p->depth || blankz(next))) {
        return VALUE;
    }
    if (c == '|' && !p->depth) {
        return LITERAL;
    }
    if (c == '>' && !p->depth) {
        fail(p, "folded scalars are unsupported");
        return BAD;
    }
    if (c == '\'' || c == '"') {
        return QUOTED;
    }
    /* A dash left here starts plain text: one no blank follows, or one
     * where no block item may begin. */
    if (!(blankz(c) || indicator(c)) || c == '-' || (!p->depth && (c == '?' || c == ':') && !blankz(next))) {
        return PLAIN;
    }
    fail(p, "found character that cannot start any token");
    return BAD;
}
/* Whether a token ends what holds a block node: the text's end, a
 * document marker, or a token left of `column`. */
static bool closes(const parser *p, token t, size_t inset) {
    return t == END || t == DOCUMENT_START || t == DOCUMENT_END || column(p) < inset;
}

/* THE SCALARS. */

/* A scalar between apostrophes or double quotes, on one line. */
static bool quoted(parser *p) {
    const size_t line = p->line;
    const bool apostrophe = peek(p, 0) == '\'';
    const int quote = apostrophe ? '\'' : '"';
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    /* Whether the text is written into `text`: from the first character
     * that reading changes on. */
    bool copied = false;
    const size_t from = ++p->at;
    for (;;) {
        const int c = peek(p, 0);
        if (c < 0) {
            return abandon(p, &text, "found unexpected end of stream");
        }
        if (newline(c)) {
            return abandon(p, &text, "found a line break in a quoted scalar");
        }
        if (c == quote && !(apostrophe && peek(p, 1) == '\'')) {
            break;
        }
        if (!copied && (c == 0 || c == (apostrophe ? '\'' : '\\'))) {
            put(p, &text, from, p->at);
            copied = true;
        }
        if (apostrophe && c == '\'') {
            markdown_core_strbuf_putc(&text, '\'');
            p->at += 2;
        } else if (!apostrophe && c == '\\') {
            /* JSON's escapes, with a surrogate pair written as two. */
            const char *hit = peek(p, 1) > 0 ? strchr("btnfr\"/\\", peek(p, 1)) : NULL;
            if (hit) {
                markdown_core_strbuf_putc(&text, (unsigned char)"\b\t\n\f\r\"/\\"[hit - "btnfr\"/\\"]);
                p->at += 2;
                continue;
            }
            uint32_t code = 0;
            for (int half = 0; half < 2; half++) {
                uint32_t unit = 0;
                if (peek(p, 0) != '\\' || peek(p, 1) != 'u') {
                    return abandon(p, &text,
                                   half ? "found an unpaired surrogate escape" : "found unknown escape character");
                }
                for (size_t k = 2; k < 6; k++) {
                    const int nibble = hex(peek(p, k));
                    if (nibble < 0) {
                        return abandon(p, &text, "did not find expected hexdecimal number");
                    }
                    unit = (unit << 4) | (uint32_t)nibble;
                }
                p->at += 6;
                if (half) {
                    if (unit < 0xDC00 || unit > 0xDFFF) {
                        return abandon(p, &text, "found an unpaired surrogate escape");
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (unit - 0xDC00);
                    break;
                }
                code = unit;
                if (unit >= 0xDC00 && unit <= 0xDFFF) {
                    return abandon(p, &text, "found an unpaired surrogate escape");
                }
                if (unit < 0xD800 || unit > 0xDBFF) {
                    break;
                }
            }
            markdown_core_utf8proc_encode_char((int32_t)code, &text);
        } else if (copied) {
            const size_t at = p->at++;
            put(p, &text, at, p->at);
        } else {
            p->at++;
        }
    }
    event *e = scalar(p, apostrophe ? MARKDOWN_CORE_YAML_APOSTROPHE : MARKDOWN_CORE_YAML_QUOTE, from, p->at, line);
    p->at++;
    p->allowed = false;
    if (!copied) {
        markdown_core_strbuf_free(&text);
        return view(p, e);
    }
    return own(p, e, &text);
}

/* A plain scalar, which its line ends: a scalar that would go on to the
 * next line is no scalar of the language, and the next line reads as the
 * tokens it holds. It reads the blanks after it, as libyaml does. */
static bool plain(parser *p) {
    const size_t from = p->at;
    size_t to = from;
    while (peek(p, 0) != '#') {
        int c;
        while (!blankz(c = peek(p, 0))) {
            if (p->depth && c == ':' && (punctuation(peek(p, 1)) || peek(p, 1) == '?')) {
                return fail(p, "found unexpected ':'");
            }
            if ((c == ':' && blankz(peek(p, 1))) || (p->depth && punctuation(c))) {
                break;
            }
            to = ++p->at;
        }
        if (!blank(peek(p, 0))) {
            break;
        }
        while (blank(peek(p, 0))) {
            p->at++;
        }
    }
    p->allowed = false;
    return view(p, scalar(p, MARKDOWN_CORE_YAML_PLAIN, from, to, p->line));
}

/* A literal: a bare `|`, then the lines its first nonblank line's
 * indentation holds, which must be deeper than the block collection's
 * column `indent` and at least one space deep. Each line keeps what
 * follows that indentation; a blank line is an empty one; the text ends
 * with one line ending after its last nonblank line. A nonblank line
 * indented less ends it. */
static bool literal(parser *p, ptrdiff_t indent) {
    const size_t start = p->at, line = p->line;
    p->at++;
    if (!blankz(peek(p, 0))) {
        return fail(p, "only a bare '|' is supported");
    }
    while (blank(peek(p, 0))) {
        p->at++;
    }
    if (peek(p, 0) == '#' && !comment(p)) {
        return false;
    }
    if (!breakz(peek(p, 0))) {
        return fail(p, "did not find expected comment or line break");
    }
    if (newline(peek(p, 0))) {
        feed(p);
    }
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    size_t depth = 0, clipped = 0, from = start, to = start;
    bool first = true;
    while (peek(p, 0) >= 0) {
        size_t spaces = 0, content;
        while (peek(p, spaces) == ' ') {
            spaces++;
        }
        for (content = spaces; blank(peek(p, content)); content++) {
        }
        if (breakz(peek(p, content))) {
            p->at += content;
            markdown_core_strbuf_putc(&text, '\n');
            if (newline(peek(p, 0))) {
                feed(p);
            }
            continue;
        }
        if (!depth) {
            if ((ptrdiff_t)spaces <= indent || !spaces) {
                break;
            }
            depth = spaces;
        }
        if (spaces < depth) {
            break;
        }
        p->at += depth;
        if (first) {
            from = p->at;
            first = false;
        }
        const size_t at = p->at;
        while (!breakz(peek(p, 0))) {
            p->at++;
        }
        put(p, &text, at, p->at);
        to = p->at;
        markdown_core_strbuf_putc(&text, '\n');
        clipped = (size_t)text.size;
        if (newline(peek(p, 0))) {
            feed(p);
        }
    }
    markdown_core_strbuf_truncate(&text, (bufsize_t)clipped);
    p->allowed = true;
    return own(p, scalar(p, MARKDOWN_CORE_YAML_LITERAL, from, to, line), &text);
}

/* THE NODES. */

/* What a block node's read found: the node; a name at the column of the
 * collection that holds it, so that the node is left out (NAME); or the
 * node, and after it a name at that column (AFTER). Either name is read,
 * its events from `index` on, and `at` is at its `:`. */
typedef enum { FAILED, NODE, NAME, AFTER } found;

static bool flow(parser *p);
static found block(parser *p, ptrdiff_t indent, bool indentless, size_t *index);

/* A node of a flow collection: a scalar or a flow collection. */
static bool element(parser *p, token t) {
    return t == QUOTED                       ? quoted(p)
           : t == PLAIN                      ? plain(p)
           : t == LIST_OPEN || t == MAP_OPEN ? flow(p)
                                             : t != BAD && fail(p, "did not find expected node content");
}

/* Whether the node read from `start`, on line `line`, is a name: a `:`
 * follows it on that line, within NAME_LIMIT characters of its start.
 * Blanks before the `:` are read. */
static bool named(parser *p, size_t start, size_t line) {
    while (blank(peek(p, 0))) {
        p->at++;
    }
    if (p->line != line || peek(p, 0) != ':' || !(p->depth || blankz(peek(p, 1)))) {
        return false;
    }
    /* No more characters than bytes. */
    size_t characters = 0;
    for (size_t i = start; p->at - start > NAME_LIMIT && i < p->at && characters <= NAME_LIMIT; i++) {
        characters += (p->text[i] & 0xC0) != 0x80;
    }
    return characters <= NAME_LIMIT || fail(p, "could not find expected ':'");
}

/* A flow collection, from its `[` or `{`. A node a `:` follows is a name:
 * in a list it opens a mapping of one pair; in a mapping a node no `:`
 * follows is a name whose value is left out. */
static bool flow(parser *p) {
    const bool map = peek(p, 0) == '{';
    const token close = map ? MAP_CLOSE : LIST_CLOSE;
    if (!open(p, map ? MARKDOWN_CORE_YAML_MAP_START : MARKDOWN_CORE_YAML_LIST_START, p->count, true)) {
        return false;
    }
    p->at++;
    p->allowed = true;
    for (bool first = true;; first = false) {
        if (!pass(p)) {
            return false;
        }
        token t = look(p);
        if (t != close) {
            if (!first) {
                if (t != COMMA) {
                    return t != BAD &&
                           fail(p, map ? "did not find expected ',' or '}'" : "did not find expected ',' or ']'");
                }
                p->at++;
                p->allowed = true;
                if (!pass(p)) {
                    return false;
                }
                t = look(p);
            }
        }
        if (t == close) {
            break;
        }
        const size_t index = p->count, start = p->at, line = p->line;
        if (!element(p, t)) {
            return false;
        }
        if (named(p, start, line)) {
            /* A list's pair is a mapping of the parser's, not the text's
             * flow depth. */
            event *pair = map ? NULL : insert(p, index, MARKDOWN_CORE_YAML_MAP_START);
            if (!map && !pair) {
                return false;
            }
            if (pair) {
                pair->flow = true;
            }
            p->at++;
            p->allowed = false;
            if (!pass(p)) {
                return false;
            }
            t = look(p);
            if (t == COMMA || t == close) {
                if (!absent(p, p->count, p->at, p->line)) {
                    return false;
                }
            } else if (!element(p, t)) {
                return false;
            }
            if (!map && !shut(p, MARKDOWN_CORE_YAML_MAP_END, true)) {
                return false;
            }
        } else if (p->problem) {
            return false;
        } else if (map) {
            if (!pass(p) || !absent(p, p->count, p->at, p->line)) {
                return false;
            }
        }
    }
    p->at++;
    p->depth--;
    p->allowed = false;
    return shut(p, map ? MARKDOWN_CORE_YAML_MAP_END : MARKDOWN_CORE_YAML_LIST_END, true);
}

/* A block mapping at `inset`, whose first name is read and whose `:`
 * is at `at`. Each name is at that column; a token left of it ends the
 * mapping. */
static bool pairs(parser *p, size_t inset) {
    for (;;) {
        const size_t value = p->at + 1, line = p->line;
        p->at++;
        p->allowed = false;
        if (!pass(p)) {
            return false;
        }
        token t = look(p);
        if (t == BAD) {
            return false;
        }
        if (closes(p, t, inset)) {
            if (!absent(p, p->count, value, line)) {
                return false;
            }
            break;
        }
        size_t index;
        const found f = block(p, (ptrdiff_t)inset, true, &index);
        if (f == FAILED) {
            return false;
        }
        if (f == NAME) {
            if (!absent(p, index, value, line)) {
                return false;
            }
            continue;
        }
        if (f == AFTER) {
            continue;
        }
        if (!pass(p)) {
            return false;
        }
        t = look(p);
        if (t == BAD) {
            return false;
        }
        if (closes(p, t, inset)) {
            break;
        }
        if (column(p) > inset || !(t == QUOTED || t == PLAIN || t == LIST_OPEN || t == MAP_OPEN)) {
            return fail(p, "did not find expected key");
        }
        const size_t start = p->at, at = p->line;
        if (!(t == QUOTED ? quoted(p) : t == PLAIN ? plain(p) : flow(p))) {
            return false;
        }
        if (!named(p, start, at)) {
            return fail(p, "could not find expected ':'");
        }
    }
    p->open--;
    return shut(p, MARKDOWN_CORE_YAML_MAP_END, false);
}

/* A block list whose items' `-` is at `inset`, from its first. An
 * indentless list is a mapping's value whose items are at the mapping's
 * column: it opens no block collection, and a name at that column ends it
 * (AFTER). */
static found items(parser *p, size_t inset, bool indentless, size_t *index) {
    if (indentless ? !emit(p, MARKDOWN_CORE_YAML_LIST_START)
                   : !open(p, MARKDOWN_CORE_YAML_LIST_START, p->count, false)) {
        return FAILED;
    }
    for (;;) {
        const size_t item = p->at + 1, line = p->line;
        p->at++;
        p->allowed = true;
        if (!pass(p)) {
            return FAILED;
        }
        token t = look(p);
        if (t == BAD) {
            return FAILED;
        }
        if (closes(p, t, inset)) {
            if (!absent(p, p->count, item, line)) {
                return FAILED;
            }
            break;
        }
        if (column(p) == inset && t == ITEM) {
            if (!absent(p, p->count, item, line)) {
                return FAILED;
            }
            continue;
        }
        const found g = block(p, (ptrdiff_t)inset, false, index);
        if (g == FAILED) {
            return FAILED;
        }
        if (g != NODE) {
            if (!indentless) {
                fail(p, "did not find expected '-' indicator");
                return FAILED;
            }
            /* The item is left out and the list ends before the name. */
            if (!absent(p, *index, item, line) || !insert(p, *index + 1, MARKDOWN_CORE_YAML_LIST_END)) {
                return FAILED;
            }
            *index += 2;
            return AFTER;
        }
        if (!pass(p)) {
            return FAILED;
        }
        t = look(p);
        if (t == BAD) {
            return FAILED;
        }
        if (t == ITEM && column(p) == inset) {
            continue;
        }
        if (indentless || closes(p, t, inset)) {
            break;
        }
        fail(p, "did not find expected '-' indicator");
        return FAILED;
    }
    if (!indentless) {
        p->open--;
    }
    return shut(p, MARKDOWN_CORE_YAML_LIST_END, false) ? NODE : FAILED;
}

/* A block node at `at`, a token a pass reached, inside a block collection
 * at column `indent` (-1 for a document's root). */
static found block(parser *p, ptrdiff_t indent, bool indentless, size_t *index) {
    const token t = look(p);
    const size_t at = column(p);
    *index = p->count;
    if (t == ITEM) {
        if ((ptrdiff_t)at > indent) {
            return items(p, at, false, index);
        }
        if (indentless) {
            return items(p, at, true, index);
        }
        fail(p, "did not find expected node content");
        return FAILED;
    }
    if (t == LITERAL) {
        return literal(p, indent) ? NODE : FAILED;
    }
    if (t == QUOTED || t == PLAIN || t == LIST_OPEN || t == MAP_OPEN) {
        const bool candidate = p->allowed, required = candidate && (ptrdiff_t)at == indent;
        const size_t start = p->at, line = p->line;
        if (!(t == QUOTED ? quoted(p) : t == PLAIN ? plain(p) : flow(p))) {
            return FAILED;
        }
        if (candidate && named(p, start, line)) {
            if ((ptrdiff_t)at == indent) {
                return NAME;
            }
            return open(p, MARKDOWN_CORE_YAML_MAP_START, *index, false) && pairs(p, at) ? NODE : FAILED;
        }
        if (p->problem || required) {
            fail(p, "could not find expected ':'");
            return FAILED;
        }
        return NODE;
    }
    if (t != BAD) {
        fail(p, "did not find expected node content");
    }
    return FAILED;
}

/* THE STREAM: documents, each implicit or opened by `---`, closed by `...`
 * or by the next document or the stream's end. */
static bool stream(parser *p) {
    if (!emit(p, MARKDOWN_CORE_YAML_STREAM_START) || !pass(p)) {
        return false;
    }
    token t = look(p);
    for (bool implicit = true;; implicit = false) {
        if (t == BAD) {
            return false;
        }
        if (t == END) {
            break;
        }
        if (!implicit && t != DOCUMENT_START) {
            return fail(p, "did not find expected <document start>");
        }
        event *start = emit(p, MARKDOWN_CORE_YAML_DOCUMENT_START);
        if (!start) {
            return false;
        }
        size_t index;
        if (t == DOCUMENT_START) {
            start->explicit = true;
            p->at += 3;
            p->allowed = false;
            if (!pass(p)) {
                return false;
            }
            t = look(p);
            if (t == DOCUMENT_START || t == DOCUMENT_END || t == END) {
                /* The stream's end is on a line of its own, as libyaml
                 * marks it. */
                if (!absent(p, p->count, p->at, p->line + (t == END && column(p) != 0))) {
                    return false;
                }
            } else if (block(p, -1, false, &index) == FAILED) {
                return false;
            }
        } else if (block(p, -1, false, &index) == FAILED) {
            return false;
        }
        if (!pass(p)) {
            return false;
        }
        t = look(p);
        event *end = emit(p, MARKDOWN_CORE_YAML_DOCUMENT_END);
        if (!end) {
            return false;
        }
        while (t == DOCUMENT_END) {
            end->explicit = true;
            p->at += 3;
            p->allowed = false;
            if (!pass(p)) {
                return false;
            }
            t = look(p);
        }
    }
    return emit(p, MARKDOWN_CORE_YAML_STREAM_END) != NULL;
}

/* Whether each character from `start` to `end` is one YAML prints: a tab,
 * a line ending, or a printable scalar, with a NUL read as U+FFFD. */
static bool printable(const unsigned char *text, size_t start, size_t end, size_t *bad) {
    for (size_t i = start; i < end;) {
        while (i < end && text[i] >= 0x20 && text[i] < 0x7f) {
            i++;
        }
        if (i == end) {
            break;
        }
        if (text[i] < 0x80) {
            const unsigned char c = text[i];
            if (c < 0x20 ? c && c != 9 && c != 10 && c != 13 : c == 0x7f) {
                *bad = i;
                return false;
            }
            i++;
            continue;
        }
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

static void release(parser *p) {
    for (size_t i = 0; i < p->count; i++) {
        if (p->events[i].owned) {
            markdown_core_free(p->events[i].owned);
        }
    }
    p->count = 0;
}

bool markdown_core_yaml_read(parser *p, const unsigned char *text, size_t start, size_t end) {
    release(p);
    p->text = text;
    p->at = p->head = start;
    p->end = end;
    p->line = p->depth = p->open = p->comment_count = 0;
    p->allowed = true;
    p->problem = NULL;
    p->failed = false;
    size_t bad;
    if (!printable(text, start, end, &bad)) {
        p->at = bad;
        return fail(p, "found a character that is not printable");
    }
    return stream(p);
}

void markdown_core_yaml_close(parser *p) {
    release(p);
    markdown_core_free(p->events);
    markdown_core_free(p->comments);
    *p = (parser){0};
}
