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
    if (s[p] < 128) {
        *width = 1;
        return s[p];
    }
    int32_t cp;
    /* Valid UTF-8 is the parser's input precondition. */
    *width = markdown_core_utf8proc_iterate(s + p, n - p, &cp);
    return cp;
}
static int name_rest(int32_t cp) {
    return markdown_core_utf8proc_is_letter(cp) || markdown_core_utf8proc_is_number(cp) || cp == '-' || cp == '_' ||
           cp == ':' || cp == '.';
}

/* A suffix result is -1 until evaluated, zero on grammar failure, or the
 * complete container's exclusive end. A lexical fact records the first
 * unescaped unquoted-value boundary independently of that grammar result.
 * Keys and records live in stable arenas; growing the radix index moves no
 * borrowed key. Its fixed-width offset keys bound every memo lookup. */
typedef struct markdown_core_attribute_fact {
    struct markdown_core_attribute_fact *member_previous, *value_previous;
    bufsize_t at, end, unquoted_end;
} attribute_fact;

typedef struct markdown_core_attribute_arena {
    struct markdown_core_attribute_arena *next;
    size_t size, capacity;
    attribute_fact facts[];
} attribute_arena;

static attribute_fact *fact_at(markdown_core_attribute_parser *p, bufsize_t at) {
    p->work++;
    if (!p->facts.mem) {
        markdown_core_key_index_init(&p->facts, p->mem, 0);
    }
    markdown_core_key_index_slot *slot =
        markdown_core_key_index_entry(&p->facts, (const unsigned char *)&at, sizeof(at));
    if (!slot) {
        p->oom = 1;
        return NULL;
    }
    if (slot->key) {
        return slot->value.pointer;
    }
    attribute_arena *arena = p->arena;
    if (!arena || arena->size == arena->capacity) {
        size_t capacity = arena ? arena->capacity * 2 : 1;
        if ((arena && capacity < arena->capacity) || capacity > (SIZE_MAX - sizeof(*arena)) / sizeof(attribute_fact)) {
            p->oom = 1;
            return NULL;
        }
        arena = p->mem->calloc(1, sizeof(*arena) + capacity * sizeof(attribute_fact));
        if (!arena) {
            p->oom = 1;
            return NULL;
        }
        arena->next = p->arena;
        arena->capacity = capacity;
        p->arena = arena;
    }
    attribute_fact *fact = &arena->facts[arena->size++];
    *fact = (attribute_fact){.at = at, .end = -1, .unquoted_end = -1};
    slot->value.pointer = fact;
    markdown_core_key_index_commit(&p->facts, slot, (const unsigned char *)&fact->at);
    return fact;
}

/* A later query can begin inside a former bare value only through a '{'.
 * Record that lexical continuation while traversing the value. Subsequent
 * queries in either source order reuse it, including escaped braces: a new
 * query starts after the escape pair, with the same lexical state. No table
 * is materialized for ordinary bytes or equals signs inside the value. */
static bufsize_t unquoted_end(markdown_core_attribute_parser *p, attribute_fact *first) {
    if (first->unquoted_end >= 0) {
        return first->unquoted_end;
    }
    attribute_fact *pending = first;
    first->value_previous = NULL;
    bufsize_t at = first->at;
    while (at < p->length) {
        unsigned char c = p->data[at];
        p->work++;
        if (horizontal(c) || newline(c) || c == '}') {
            break;
        }
        bool escape = escaped(p->data, p->length, at);
        bufsize_t brace = escape ? at + 1 : at;
        at += escape ? 2 : 1;
        if (p->data[brace] == '{') {
            attribute_fact *fact = fact_at(p, brace + 1);
            if (!fact) {
                break;
            }
            if (fact->unquoted_end >= 0) {
                at = fact->unquoted_end;
                break;
            }
            fact->value_previous = pending;
            pending = fact;
        }
    }
    while (pending) {
        attribute_fact *previous = pending->value_previous;
        pending->unquoted_end = at;
        pending->value_previous = NULL;
        pending = previous;
    }
    return at;
}

/* An opening quote follows '=' and is therefore never escaped by the value
 * before it. For either quote character, scans from distinct assignments
 * cannot overlap: the next such opening quote closes the earlier scan.
 * Unmatched quotes retain the grammar's unquoted-value fallback. */
static bufsize_t quoted_end(markdown_core_attribute_parser *p, bufsize_t from) {
    unsigned char quote = p->data[from];
    bool line_end = false;
    for (bufsize_t at = from + 1; at < p->length;) {
        unsigned char c = p->data[at];
        p->work++;
        if (c == quote) {
            return at + 1;
        }
        if (newline(c)) {
            if (line_end) {
                break;
            }
            line_end = true;
            at += c == '\r' && at + 1 < p->length && p->data[at + 1] == '\n' ? 2 : 1;
        } else {
            if (!horizontal(c)) {
                line_end = false;
            }
            at += escaped(p->data, p->length, at) ? 2 : 1;
        }
    }
    return 0;
}

static bufsize_t scan_name(markdown_core_attribute_parser *p, bufsize_t n, bufsize_t at);

/* Iterative memoized member recognition. Candidates can join another
 * candidate's grammar only after a value; those joins share one suffix fact.
 * Each unresolved chain advances strictly, then publishes one result to all
 * its members. There is no recursive C stack or per-byte DP allocation. */
static bufsize_t recognize(markdown_core_attribute_parser *p, attribute_fact *first) {
    const unsigned char *s = p->data;
    bufsize_t n = p->length, at = first->at, result = 0;
    attribute_fact *pending = first, *origin = first;
    first->member_previous = NULL;
    while (at < n && !p->oom) {
        unsigned char c = s[at];
        p->work++;
        if (c == '}') {
            result = at + 1;
            break;
        }
        if (horizontal(c) || c == '-') {
            at++;
            continue;
        }
        if (newline(c)) {
            at += c == '\r' && at + 1 < n && s[at + 1] == '\n' ? 2 : 1;
            while (at < n && horizontal(s[at])) {
                p->work++;
                at++;
            }
            if (at < n && newline(s[at])) {
                break;
            }
            continue;
        }
        bufsize_t width;
        if (c == '#' || c == '.') {
            if (++at == n) {
                break;
            }
            int32_t cp = scalar(s, n, at, &width);
            if (!(c == '#' ? name_rest(cp) : markdown_core_utf8proc_is_letter(cp))) {
                break;
            }
            at = scan_name(p, n, at);
            continue;
        }
        if (!markdown_core_utf8proc_is_letter(scalar(s, n, at, &width))) {
            break;
        }
        at = scan_name(p, n, at);
        if (at == n || s[at++] != '=') {
            break;
        }
        bufsize_t end = 0;
        if (at < n && (s[at] == '\'' || s[at] == '"')) {
            end = quoted_end(p, at);
        }
        if (!end) {
            end = unquoted_end(p, origin);
            if (end < at && !p->oom) {
                attribute_fact *value = fact_at(p, at);
                if (value) {
                    end = unquoted_end(p, value);
                }
            }
        }
        if (p->oom) {
            break;
        }
        attribute_fact *join = fact_at(p, end);
        if (!join) {
            break;
        }
        if (join->end >= 0) {
            result = join->end;
            break;
        }
        assert(join->at > origin->at);
        join->member_previous = pending;
        pending = origin = join;
        at = end;
    }
    while (pending) {
        attribute_fact *previous = pending->member_previous;
        pending->end = result;
        pending->member_previous = NULL;
        pending = previous;
    }
    return p->oom ? 0 : result;
}

void markdown_core_attributes_free(markdown_core_mem *mem, markdown_core_attributes *v) {
    markdown_core_chunk_free(mem, &v->anchor);
    for (size_t i = 0; i < v->class_count; i++) {
        markdown_core_chunk_free(mem, &v->classes[i]);
    }
    for (size_t i = 0; i < v->record_count; i++) {
        markdown_core_chunk_free(mem, &v->records[i].name);
        markdown_core_chunk_free(mem, &v->records[i].value);
    }
    mem->free(v->classes);
    mem->free(v->records);
    memset(v, 0, sizeof(*v));
}

void markdown_core_attribute_parser_free(markdown_core_attribute_parser *p) {
    markdown_core_key_index_free(&p->facts);
    while (p->arena) {
        attribute_arena *next = p->arena->next;
        p->mem->free(p->arena);
        p->arena = next;
    }
}

static int copy(markdown_core_mem *mem, markdown_core_chunk *into, const unsigned char *s, bufsize_t n) {
    unsigned char *data = mem->calloc((size_t)n + 1, 1);
    if (!data) {
        return 0;
    }
    memcpy(data, s, (size_t)n);
    markdown_core_chunk_free(mem, into);
    *into = (markdown_core_chunk){data, n, 1};
    return 1;
}

static int reserve(markdown_core_mem *mem, void **items, size_t count, size_t *capacity, size_t size) {
    if (count < *capacity) {
        return 1;
    }
    if (*capacity > SIZE_MAX / 2) {
        return 0;
    }
    size_t grown = *capacity ? *capacity * 2 : 1;
    if (grown > SIZE_MAX / size) {
        return 0;
    }
    void *data = mem->realloc(*items, grown * size);
    if (!data) {
        return 0;
    }
    *items = data;
    *capacity = grown;
    return 1;
}

static int append_class(markdown_core_mem *mem, markdown_core_attributes *v, const unsigned char *s, bufsize_t n) {
    if (!reserve(mem, (void **)&v->classes, v->class_count, &v->class_capacity, sizeof(*v->classes))) {
        return 0;
    }
    markdown_core_chunk item = {0};
    if (!copy(mem, &item, s, n)) {
        return 0;
    }
    v->classes[v->class_count++] = item;
    return 1;
}

static int normalize(markdown_core_mem *mem, markdown_core_attributes *v, const unsigned char *name, bufsize_t length,
                     const unsigned char *value, bufsize_t size) {
    if (length == 2 && memcmp(name, "id", 2) == 0) {
        return copy(mem, &v->anchor, value, size);
    }
    if (length == 5 && memcmp(name, "class", 5) == 0) {
        bufsize_t word = 0, at = 0;
        while (at < size) {
            bufsize_t width;
            int32_t cp = scalar(value, size, at, &width);
            if (markdown_core_utf8proc_is_space(cp) || cp == 11) {
                if (at > word && !append_class(mem, v, value + word, at - word)) {
                    return 0;
                }
                word = at + width;
            }
            at += width;
        }
        return at == word || append_class(mem, v, value + word, at - word);
    }
    if (!reserve(mem, (void **)&v->records, v->record_count, &v->record_capacity, sizeof(*v->records))) {
        return 0;
    }
    markdown_core_record item = {0};
    if (!copy(mem, &item.name, name, length) || !copy(mem, &item.value, value, size)) {
        markdown_core_chunk_free(mem, &item.name);
        markdown_core_chunk_free(mem, &item.value);
        return 0;
    }
    v->records[v->record_count++] = item;
    return 1;
}

static bufsize_t scan_name(markdown_core_attribute_parser *p, bufsize_t n, bufsize_t at) {
    const unsigned char *s = p->data;
    while (at < n) {
        bufsize_t width;
        p->work++;
        if (!name_rest(scalar(s, n, at, &width))) {
            break;
        }
        at += width;
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
    attribute_fact *fact = fact_at(p, start + 1);
    return !fact ? 0 : fact->end >= 0 ? fact->end : recognize(p, fact);
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
    markdown_core_strbuf decoded = MARKDOWN_CORE_BUF_INIT(p->mem);
    bufsize_t at = start + 1;
    while (at < finish - 1) {
        p->work++;
        if (horizontal(s[at]) || newline(s[at])) {
            at++;
            continue;
        }
        if (s[at] == '-') {
            if (!append_class(p->mem, &value, (const unsigned char *)"unnumbered", 10)) {
                goto oom;
            }
            at++;
            continue;
        }
        if (s[at] == '#' || s[at] == '.') {
            unsigned char marker = s[at++];
            bufsize_t from = at;
            at = scan_name(p, finish, at);
            if (marker == '#' ? !copy(p->mem, &value.anchor, s + from, at - from)
                              : !append_class(p->mem, &value, s + from, at - from)) {
                goto oom;
            }
            continue;
        }
        bufsize_t name = at;
        at = scan_name(p, finish, at);
        bufsize_t name_length = at - name;
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
        if (decoded.oom || !normalize(p->mem, &value, s + name, name_length, decoded.ptr, decoded.size)) {
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
    markdown_core_attributes_free(p->mem, &value);
    return 0;
}

#include "inline_internal.h"
#include "block_internal.h"
int markdown_core_inline_state_attributes(markdown_core_inline_state *inline_state, bufsize_t start,
                                          markdown_core_attributes *value, bufsize_t *end) {
    if (start == inline_state->heading_attributes_start) {
        return 0;
    }
    if (!inline_state->attributes.mem) {
        inline_state->attributes.mem = inline_state->mem;
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
    markdown_core_attribute_parser attributes = {.mem = parser->mem, .data = source, .length = length};
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
    if (inline_state->attributes.mem) {
        inline_state->owner_parser->attribute_work += inline_state->attributes.work;
        markdown_core_attribute_parser_free(&inline_state->attributes);
    }
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES = {
    .name = "attributes",
    .dispose_inline = dispose_inline,
};
