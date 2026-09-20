#include "alloc.h"
#include "attributes.h"
#include "../core/attributes.h"
#include "houdini.h"
#include "markdown_core_ctype.h"
#include "utf8.h"
#include <stdint.h>
#include <string.h>

static int horizontal(unsigned char c) { return c == ' ' || c == '\t'; }
static int newline(unsigned char c) { return c == '\n' || c == '\r'; }
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
static int name_terminator(unsigned char c) {
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '=':
    case '}':
    case '"':
    case '\'':
    case '{':
        return 1;
    default:
        return 0;
    }
}
/* A member boundary: what may follow a bare name or the `unnumbered` dash. */
static int member_boundary(const unsigned char *s, bufsize_t n, bufsize_t p) {
    return p >= n || horizontal(s[p]) || newline(s[p]) || s[p] == '}';
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
    while (at < n && !name_terminator(s[at])) {
        p->work++;
        at++;
    }
    return at;
}

/* The closer of the quote at `open`: the first unescaped quote of its kind
 * after it, or n when a blank line or the end comes first. */
static bufsize_t quote_close(markdown_core_attribute_parser *p, bufsize_t open) {
    const unsigned char *s = p->data;
    bufsize_t n = p->length, at = open + 1;
    unsigned char quote = s[open];
    while (at < n) {
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
        unsigned char c = s[at];
        p->work++;
        if (horizontal(c) || newline(c) || c == '}') {
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
 * is the common call. It owns no vector, no arena and no anchor bytes, and a
 * release that walked it anyway made three round trips into the allocation
 * module to free nothing, then cleared a struct that was already zero. */
void markdown_core_attributes_free(markdown_core_attributes *v) {
    if (!v->classes && !v->records && !v->arena && !v->anchor.alloc) {
        return;
    }
    markdown_core_chunk_free(&v->anchor);
    for (size_t i = 0; i < v->class_count; i++) {
        markdown_core_chunk_free(&v->classes[i]);
    }
    for (size_t i = 0; i < v->record_count; i++) {
        markdown_core_chunk_free(&v->records[i].name);
        markdown_core_chunk_free(&v->records[i].value);
    }
    markdown_core_free(v->classes);
    markdown_core_free(v->records);
    markdown_core_free(v->arena);
    memset(v, 0, sizeof(*v));
}

void markdown_core_attribute_parser_free(markdown_core_attribute_parser *p) {
    markdown_core_free(p->ends);
    p->ends = NULL;
}

/* THE STRINGS OF ONE VALUE ARE INTERNED IN ONE ARENA, built while the members
 * are read and sized to what they produced. Until the arena is final its
 * chunks hold OFFSETS, one more than the string's offset so that the first
 * string is not a null pointer, and `finish_arena` turns them into pointers
 * once the arena has its final address; nothing reads a chunk before then,
 * and a value abandoned on the way is freed by `markdown_core_attributes_free`
 * without dereferencing one. The arena is first sized to the container's own
 * length plus a little, which the decoded strings rarely exceed (a bare member
 * adds `true`, the `-` member `unnumbered`, and each string a NUL), doubles
 * when they do, and is trimmed at the end only when more than half of it is
 * unused, so the common container costs one allocation and no copy. */
typedef struct {
    markdown_core_attributes *value;
    size_t used, capacity, first;
} arena;

static int intern(arena *a, const unsigned char *s, bufsize_t n, markdown_core_chunk *into) {
    size_t needed = a->used + (size_t)n + 1;
    if (needed > a->capacity) {
        size_t grown = a->capacity ? a->capacity : a->first;
        while (grown < needed) {
            if (grown > SIZE_MAX / 2) {
                return 0;
            }
            grown *= 2;
        }
        unsigned char *moved = markdown_core_realloc(a->value->arena, grown);
        if (!moved) {
            return 0;
        }
        a->value->arena = moved;
        a->capacity = grown;
    }
    memcpy(a->value->arena + a->used, s, (size_t)n);
    a->value->arena[a->used + n] = 0;
    *into = (markdown_core_chunk){(unsigned char *)(uintptr_t)(a->used + 1), n, 0};
    a->used = needed;
    return 1;
}

static void finish_chunk(markdown_core_chunk *chunk, unsigned char *base) {
    if (chunk->data) {
        chunk->data = base + ((uintptr_t)chunk->data - 1);
    }
}

/* Give the arena its final size, then every chunk its address in it. */
static int finish_arena(arena *a) {
    markdown_core_attributes *v = a->value;
    if (a->used < a->capacity / 2) {
        unsigned char *moved = markdown_core_realloc(v->arena, a->used);
        if (!moved) {
            return 0;
        }
        v->arena = moved;
    }
    finish_chunk(&v->anchor, v->arena);
    for (size_t i = 0; i < v->class_count; i++) {
        finish_chunk(&v->classes[i], v->arena);
    }
    for (size_t i = 0; i < v->record_count; i++) {
        finish_chunk(&v->records[i].name, v->arena);
        finish_chunk(&v->records[i].value, v->arena);
    }
    return 1;
}

static int reserve(void **items, size_t count, size_t *capacity, size_t size) {
    if (count < *capacity) {
        return 1;
    }
    if (*capacity > SIZE_MAX / 2) {
        return 0;
    }
    size_t grown = *capacity ? *capacity * 2 : 8;
    if (grown > SIZE_MAX / size) {
        return 0;
    }
    void *data = markdown_core_realloc(*items, grown * size);
    if (!data) {
        return 0;
    }
    *items = data;
    *capacity = grown;
    return 1;
}

static int append_class(arena *a, const unsigned char *s, bufsize_t n) {
    markdown_core_attributes *v = a->value;
    if (!reserve((void **)&v->classes, v->class_count, &v->class_capacity, sizeof(*v->classes)) ||
        !intern(a, s, n, &v->classes[v->class_count])) {
        return 0; /* The slot is counted only once it holds a string. */
    }
    v->class_count++;
    return 1;
}

static int normalize(arena *a, const unsigned char *name, bufsize_t length, const unsigned char *value,
                     bufsize_t size) {
    markdown_core_attributes *v = a->value;
    if (length == 2 && memcmp(name, "id", 2) == 0) {
        return intern(a, value, size, &v->anchor);
    }
    if (length == 5 && memcmp(name, "class", 5) == 0) {
        bufsize_t word = 0, at = 0;
        while (at < size) {
            bufsize_t width;
            int32_t cp = scalar(value, size, at, &width);
            if (markdown_core_utf8proc_is_space(cp) || cp == 11) {
                if (at > word && !append_class(a, value + word, at - word)) {
                    return 0;
                }
                word = at + width;
            }
            at += width;
        }
        return at == word || append_class(a, value + word, at - word);
    }
    if (!reserve((void **)&v->records, v->record_count, &v->record_capacity, sizeof(*v->records))) {
        return 0;
    }
    markdown_core_record *item = &v->records[v->record_count];
    if (!intern(a, name, length, &item->name) || !intern(a, value, size, &item->value)) {
        return 0;
    }
    v->record_count++;
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
    const unsigned char *s = p->data;
    bufsize_t finish = markdown_core_attributes_end(p, start);
    if (!finish) {
        return 0;
    }
    markdown_core_attributes value = {0};
    arena strings = {&value, 0, 0, (size_t)(finish - start) + 16};
    markdown_core_strbuf decoded = MARKDOWN_CORE_BUF_INIT();
    bufsize_t at = start + 1;
    while (at < finish - 1) {
        p->work++;
        if (horizontal(s[at]) || newline(s[at])) {
            at++;
            continue;
        }
        if (s[at] == '-' && member_boundary(s, finish, at + 1)) {
            if (!append_class(&strings, (const unsigned char *)"unnumbered", 10)) {
                goto oom;
            }
            at++;
            continue;
        }
        if (s[at] == '#' || s[at] == '.') {
            unsigned char marker = s[at++];
            bufsize_t from = at;
            at = scan_name(p, finish, at);
            if (marker == '#' ? !intern(&strings, s + from, at - from, &value.anchor)
                              : !append_class(&strings, s + from, at - from)) {
                goto oom;
            }
            continue;
        }
        bufsize_t name = at;
        at = scan_name(p, finish, at);
        bufsize_t name_length = at - name;
        if (at >= finish - 1 || s[at] != '=') {
            /* A bare name: recognition admitted it only at a member boundary. */
            if (!normalize(&strings, s + name, name_length, (const unsigned char *)"true", 4)) {
                goto oom;
            }
            continue;
        }
        at++; /* '=' */
        bufsize_t last = at;
        int quoted = 0;
        if (s[at] == '"' || s[at] == '\'') {
            unsigned char quote = s[at];
            for (bufsize_t i = at + 1; i < finish - 1; i++) {
                p->work++;
                if (escaped(s, finish, i)) {
                    i++;
                    continue;
                }
                if (s[i] == quote) {
                    quoted = 1;
                    last = i;
                    break;
                }
            }
        }
        if (quoted) {
            at++;
        } else {
            while (last < finish - 1 && !horizontal(s[last]) && !newline(s[last]) && s[last] != '}') {
                p->work++;
                last += escaped(s, finish, last) ? 2 : 1;
            }
        }
        /* DECODE BY RUNS. A value's bytes are its own except at a backslash
         * and, in a quoted value, at `&` and at a line ending; everything
         * between two of those is copied whole. An unquoted value holds no
         * line ending (it ends at one) and keeps its references as written. */
        markdown_core_strbuf_clear(&decoded);
        while (at < last) {
            bufsize_t run = at;
            while (run < last && s[run] != '\\' && (!quoted || (s[run] != '&' && !newline(s[run])))) {
                run++;
            }
            p->work += (size_t)(run - at);
            markdown_core_strbuf_put(&decoded, s + at, run - at);
            at = run;
            if (at >= last) {
                break;
            }
            p->work++;
            if (escaped(s, last, at)) {
                markdown_core_strbuf_putc(&decoded, s[at + 1]);
                at += 2;
            } else if (quoted && s[at] == '&') {
                bufsize_t used = houdini_unescape_ent(&decoded, s + at + 1, last - at - 1);
                if (used) {
                    at += used + 1;
                } else {
                    markdown_core_strbuf_putc(&decoded, s[at++]);
                }
            } else if (quoted && newline(s[at])) {
                if (s[at] == '\r' && at + 1 < last && s[at + 1] == '\n') {
                    at++;
                }
                at++;
                markdown_core_strbuf_putc(&decoded, ' ');
            } else {
                markdown_core_strbuf_putc(&decoded, s[at++]);
            }
        }
        if (quoted) {
            at++;
        }
        if (decoded.oom || !normalize(&strings, s + name, name_length, decoded.ptr, decoded.size)) {
            goto oom;
        }
    }
    if (!finish_arena(&strings)) {
        goto oom;
    }
    p->work += (size_t)(finish - start);
    markdown_core_strbuf_free(&decoded);
    *result = value;
    *end = finish;
    return 1;
oom:
    p->oom = 1;
    markdown_core_strbuf_free(&decoded);
    markdown_core_attributes_free(&value);
    return 0;
}

#include "inline_internal.h"
#include "block_internal.h"
int markdown_core_inline_state_attributes(markdown_core_inline_state *inline_state, bufsize_t start,
                                          markdown_core_attributes *value, bufsize_t *end) {
    if (start == inline_state->heading_attributes_start) {
        return 0;
    }
    if (!inline_state->attributes.data) {
        inline_state->attributes.data = inline_state->input.data;
        inline_state->attributes.length = inline_state->input.len;
    }
    int matched = markdown_core_attributes_parse(&inline_state->attributes, start, value, end);
    if (inline_state->attributes.oom) {
        inline_state->oom = 1;
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
    markdown_core_attribute_parser attributes = {.data = source, .length = length};
    bufsize_t attribute_start = markdown_core_attributes_tail(&attributes, 0, info_end);
    if (attribute_start >= 0 &&
        markdown_core_attributes_parse(&attributes, attribute_start, &node->attributes, &attribute_end)) {
        info_end = attribute_start;
    }
    if (attributes.oom) {
        parser->oom = true;
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
