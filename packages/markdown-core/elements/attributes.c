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

/* Reverse dynamic programming over the regular member grammar. ends[p] is
 * the end of a complete member suffix (including its closing brace), or zero.
 * Quotes and unquoted runs need only two preceding byte states; a name run
 * needs one, the position of its next terminator byte. Thus the index costs
 * two offsets per input byte and never decodes a scalar. Assignment
 * continuations are separate from valid member suffixes: a bare '=' can never
 * become a member through a whitespace or shorthand state. */
static bufsize_t suffix(markdown_core_attribute_parser *p, bufsize_t at) { return p->ends[at].end; }

static int index_input(markdown_core_attribute_parser *p) {
    const unsigned char *s = p->data;
    bufsize_t n = p->length;
    bufsize_t quote[2] = {n, n}, quote2[2] = {n, n};
    bufsize_t unquoted = n, unquoted2 = n, name_end = n, spaces = n;
    if ((size_t)n + 1 > SIZE_MAX / sizeof(*p->ends)) {
        return 0;
    }
    p->ends = markdown_core_alloc((size_t)n + 1, sizeof(*p->ends));
    if (!p->ends) {
        return 0;
    }
    for (bufsize_t i = n; i-- > 0;) {
        bufsize_t q[2] = {quote[0], quote[1]}, u = unquoted;
        unsigned char c = s[i];
        p->work++;
        if (c == '}') {
            p->ends[i].end = i + 1;
        } else if (horizontal(c)) {
            p->ends[i].end = suffix(p, i + 1);
        } else if (newline(c)) {
            if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {
                p->ends[i].end = suffix(p, i + 1);
            } else if (spaces == n || !newline(s[spaces])) {
                p->ends[i].end = suffix(p, spaces);
            } else {
                q[0] = q[1] = n; /* A quoted value cannot cross a blank line. */
            }
        } else if (c == '=') {
            bufsize_t end = unquoted;
            if (i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '\'')) {
                bufsize_t close = quote2[s[i + 1] == '\''];
                if (close < n) {
                    end = close + 1;
                }
            }
            p->ends[i].assignment_end = suffix(p, end);
        } else if (c == '-' && member_boundary(s, n, i + 1)) {
            p->ends[i].end = suffix(p, i + 1);
        } else if (c == '#' || c == '.') {
            if (i + 1 < n && !name_terminator(s[i + 1])) {
                p->ends[i].end = suffix(p, name_end);
            }
        } else if (!name_terminator(c)) {
            /* A name runs to `name_end`. Followed by `=` it is an assignment;
             * followed by a member boundary it is a bare attribute; followed
             * by a quote or an opening brace it is nothing. */
            if (name_end < n && s[name_end] == '=') {
                p->ends[i].end = p->ends[name_end].assignment_end;
            } else if (name_end < n && member_boundary(s, n, name_end)) {
                p->ends[i].end = suffix(p, name_end);
            }
        }

        if (escaped(s, n, i)) {
            q[0] = quote2[0];
            q[1] = quote2[1];
            u = unquoted2;
        } else {
            if (c == '"') {
                q[0] = i;
            }
            if (c == '\'') {
                q[1] = i;
            }
            if (horizontal(c) || newline(c) || c == '}') {
                u = i;
            }
        }
        quote2[0] = quote[0];
        quote2[1] = quote[1];
        quote[0] = q[0];
        quote[1] = q[1];
        unquoted2 = unquoted;
        unquoted = u;
        if (!horizontal(c)) {
            spaces = i;
        }
        if (name_terminator(c)) {
            name_end = i;
        }
    }
    return 1;
}

void markdown_core_attributes_free(markdown_core_attributes *v) {
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
    memset(v, 0, sizeof(*v));
}

void markdown_core_attribute_parser_free(markdown_core_attribute_parser *p) {
    markdown_core_free(p->ends);
    p->ends = NULL;
}

static int copy(markdown_core_chunk *into, const unsigned char *s, bufsize_t n) {
    unsigned char *data = markdown_core_alloc((size_t)n + 1, 1);
    if (!data) {
        return 0;
    }
    memcpy(data, s, (size_t)n);
    markdown_core_chunk_free(into);
    *into = (markdown_core_chunk){data, n, 1};
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

static int append_class(markdown_core_attributes *v, const unsigned char *s, bufsize_t n) {
    if (!reserve((void **)&v->classes, v->class_count, &v->class_capacity, sizeof(*v->classes))) {
        return 0;
    }
    markdown_core_chunk item = {0};
    if (!copy(&item, s, n)) {
        return 0;
    }
    v->classes[v->class_count++] = item;
    return 1;
}

static int normalize(markdown_core_attributes *v, const unsigned char *name, bufsize_t length,
                     const unsigned char *value, bufsize_t size) {
    if (length == 2 && memcmp(name, "id", 2) == 0) {
        return copy(&v->anchor, value, size);
    }
    if (length == 5 && memcmp(name, "class", 5) == 0) {
        bufsize_t word = 0, at = 0;
        while (at < size) {
            bufsize_t width;
            int32_t cp = scalar(value, size, at, &width);
            if (markdown_core_utf8proc_is_space(cp) || cp == 11) {
                if (at > word && !append_class(v, value + word, at - word)) {
                    return 0;
                }
                word = at + width;
            }
            at += width;
        }
        return at == word || append_class(v, value + word, at - word);
    }
    if (!reserve((void **)&v->records, v->record_count, &v->record_capacity, sizeof(*v->records))) {
        return 0;
    }
    markdown_core_record item = {0};
    if (!copy(&item.name, name, length) || !copy(&item.value, value, size)) {
        markdown_core_chunk_free(&item.name);
        markdown_core_chunk_free(&item.value);
        return 0;
    }
    v->records[v->record_count++] = item;
    return 1;
}

static bufsize_t scan_name(markdown_core_attribute_parser *p, bufsize_t n, bufsize_t at) {
    const unsigned char *s = p->data;
    while (at < n && !name_terminator(s[at])) {
        p->work++;
        at++;
    }
    return at;
}

bufsize_t markdown_core_attributes_end(markdown_core_attribute_parser *p, bufsize_t start) {
    p->work++;
    if (p->oom) {
        return 0;
    }
    if (start < 0 || start >= p->length || p->data[start] != '{') {
        return 0;
    }
    if (!p->ends && !index_input(p)) {
        p->oom = 1;
        return 0;
    }
    return suffix(p, start + 1);
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
    markdown_core_strbuf decoded = MARKDOWN_CORE_BUF_INIT();
    bufsize_t at = start + 1;
    while (at < finish - 1) {
        p->work++;
        if (horizontal(s[at]) || newline(s[at])) {
            at++;
            continue;
        }
        if (s[at] == '-' && member_boundary(s, finish, at + 1)) {
            if (!append_class(&value, (const unsigned char *)"unnumbered", 10)) {
                goto oom;
            }
            at++;
            continue;
        }
        if (s[at] == '#' || s[at] == '.') {
            unsigned char marker = s[at++];
            bufsize_t from = at;
            at = scan_name(p, finish, at);
            if (marker == '#' ? !copy(&value.anchor, s + from, at - from)
                              : !append_class(&value, s + from, at - from)) {
                goto oom;
            }
            continue;
        }
        bufsize_t name = at;
        at = scan_name(p, finish, at);
        bufsize_t name_length = at - name;
        if (at >= finish - 1 || s[at] != '=') {
            /* A bare name: the index admitted it only at a member boundary. */
            if (!normalize(&value, s + name, name_length, (const unsigned char *)"true", 4)) {
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
        markdown_core_strbuf_clear(&decoded);
        while (at < last) {
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
        if (decoded.oom || !normalize(&value, s + name, name_length, decoded.ptr, decoded.size)) {
            goto oom;
        }
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
