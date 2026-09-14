#include "attributes.h"
#include "../core/arena.h"
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

/* Scan work accumulates locally and is published on exit. Do not write
 * p->work in byte loops: input bytes may alias the parser, preventing the
 * compiler from hoisting shared counter updates. Nested helpers add their
 * own completed work; the final += preserves those contributions. */

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

/* One block holds every fact an ordinary container asks about. Facts are the
 * parser's scratch: they answer queries about one input extent while it is
 * being read and mean nothing once it has been, so a transaction's blocks are
 * taken from its arena's recycling pools and handed straight back when the
 * parser ends -- the storage is the transaction's, the lifetime the parser's.
 * Only a committed value is carried by the document. */
#define MARKDOWN_CORE_ATTRIBUTE_FACT_BLOCK 4

static size_t fact_block_bytes(size_t capacity) { return sizeof(attribute_arena) + capacity * sizeof(attribute_fact); }

static attribute_fact *fact_at(markdown_core_attribute_parser *p, bufsize_t at) {
    MARKDOWN_CORE_DIAGNOSTIC(p->work++;)
    if (!p->facts.mem && !markdown_core_key_index_init(&p->facts, p->mem, 0)) {
        p->oom = 1;
        return NULL;
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
        size_t capacity = arena ? arena->capacity * 2 : MARKDOWN_CORE_ATTRIBUTE_FACT_BLOCK;
        if ((arena && capacity < arena->capacity) || capacity > (SIZE_MAX - sizeof(*arena)) / sizeof(attribute_fact)) {
            p->oom = 1;
            return NULL;
        }
        arena = p->store ? markdown_core_arena_take(p->store, fact_block_bytes(capacity))
                         : p->mem->calloc(1, fact_block_bytes(capacity));
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
    p->fact_count++;
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
    MARKDOWN_CORE_DIAGNOSTIC(size_t work = 0;)
    if (first->unquoted_end >= 0) {
        return first->unquoted_end;
    }
    attribute_fact *pending = first;
    first->value_previous = NULL;
    bufsize_t at = first->at;
    while (at < p->length) {
        unsigned char c = p->data[at];
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
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
    MARKDOWN_CORE_DIAGNOSTIC(p->work += work;)
    return at;
}

/* An opening quote follows '=' and is therefore never escaped by the value
 * before it. For either quote character, scans from distinct assignments
 * cannot overlap: the next such opening quote closes the earlier scan.
 * Unmatched quotes retain the grammar's unquoted-value fallback. */
static bufsize_t quoted_end(markdown_core_attribute_parser *p, bufsize_t from) {
    MARKDOWN_CORE_DIAGNOSTIC(size_t work = 0;)
    unsigned char quote = p->data[from];
    bool line_end = false;
    bufsize_t result = 0;
    for (bufsize_t at = from + 1; at < p->length;) {
        unsigned char c = p->data[at];
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
        if (c == quote) {
            result = at + 1;
            break;
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
    MARKDOWN_CORE_DIAGNOSTIC(p->work += work;)
    return result;
}

static bufsize_t scan_name(markdown_core_attribute_parser *p, bufsize_t n, bufsize_t at);

/* Iterative memoized member recognition. Candidates can join another
 * candidate's grammar only after a value; those joins share one suffix fact.
 * Each unresolved chain advances strictly, then publishes one result to all
 * its members. There is no recursive C stack or per-byte DP allocation. */
static bufsize_t recognize(markdown_core_attribute_parser *p, attribute_fact *first) {
    MARKDOWN_CORE_DIAGNOSTIC(size_t work = 0;)
    const unsigned char *s = p->data;
    bufsize_t n = p->length, at = first->at, result = 0;
    attribute_fact *pending = first, *origin = first;
    first->member_previous = NULL;
    while (at < n && !p->oom) {
        unsigned char c = s[at];
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
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
                MARKDOWN_CORE_DIAGNOSTIC(work++;)
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
    MARKDOWN_CORE_DIAGNOSTIC(p->work += work;)
    return p->oom ? 0 : result;
}

void markdown_core_attributes_free(markdown_core_mem *mem, markdown_core_attributes *v) {
    markdown_core_chunk_free(mem, &v->anchor);
    for (uint32_t i = 0; i < v->class_count; i++) {
        markdown_core_chunk_free(mem, &v->classes[i]);
    }
    for (uint32_t i = 0; i < v->record_count; i++) {
        markdown_core_chunk_free(mem, &v->records[i].name);
        markdown_core_chunk_free(mem, &v->records[i].value);
    }
    if (!v->borrowed) {
        if (v->classes) {
            mem->free(v->classes);
        }
        if (v->records) {
            mem->free(v->records);
        }
    }
    memset(v, 0, sizeof(*v));
}

void markdown_core_attribute_parser_free(markdown_core_attribute_parser *p) {
    /* A parser that asked about no fact never built an index, and an
     * uninitialized one has nothing to clear. */
    if (p->facts.mem) {
        markdown_core_key_index_free(&p->facts);
    }
    /* A parser that decoded nothing never took a buffer, and an
     * uninitialized one has no allocator to release it through. */
    if (p->decoded.mem) {
        markdown_core_strbuf_free(&p->decoded);
    }
    /* The index borrowed a key out of every fact, and it has just been
     * released, so the blocks are free to go back. */
    while (p->arena) {
        attribute_arena *next = p->arena->next;
        if (p->store) {
            markdown_core_arena_recycle(p->store, p->arena, fact_block_bytes(p->arena->capacity));
        } else {
            p->mem->free(p->arena);
        }
        p->arena = next;
    }
    p->fact_count = 0;
}

/* A value's normalized bytes. The arena's are borrowed by the chunk and go
 * with the document; without an arena the chunk owns the allocator's, which
 * is what a node built outside a parse gets. The terminator is written
 * either way: every value reads back as a C string. */
static int copy(markdown_core_attribute_parser *p, markdown_core_chunk *into, const unsigned char *s, bufsize_t n) {
    unsigned char *data =
        p->store ? markdown_core_arena_text(p->store, (size_t)n + 1) : p->mem->calloc((size_t)n + 1, 1);
    if (!data) {
        return 0;
    }
    memcpy(data, s, (size_t)n);
    data[n] = '\0';
    markdown_core_chunk_free(p->mem, into);
    *into = (markdown_core_chunk){data, n, p->store ? 0 : 1};
    return 1;
}

/* The vector a value's occurrences land in. A transaction takes it from the
 * arena's recycling pools and copies the occurrences forward, so growth costs
 * bytes the document already owns rather than an allocation, and the vector it
 * supersedes goes straight back to the pool it came from rather than sitting
 * dead in the arena for the document's life. The value releases neither. */
static int reserve(markdown_core_attribute_parser *p, markdown_core_attributes *v, void **items, uint32_t count,
                   uint32_t *capacity, size_t size) {
    if (count < *capacity) {
        return 1;
    }
    if (*capacity > UINT32_MAX / 2) {
        return 0;
    }
    uint32_t grown = *capacity ? *capacity * 2 : 1;
    if ((size_t)grown > SIZE_MAX / size) {
        return 0;
    }
    void *data;
    if (p->store) {
        data = markdown_core_arena_take(p->store, (size_t)grown * size);
        if (!data) {
            return 0;
        }
        if (count) {
            memcpy(data, *items, (size_t)count * size);
        }
        if (*capacity) {
            markdown_core_arena_recycle(p->store, *items, (size_t)*capacity * size);
        }
    } else {
        data = p->mem->realloc(*items, (size_t)grown * size);
    }
    if (!data) {
        return 0;
    }
    v->borrowed = p->store != NULL;
    *items = data;
    *capacity = grown;
    return 1;
}

static int append_class(markdown_core_attribute_parser *p, markdown_core_attributes *v, const unsigned char *s,
                        bufsize_t n) {
    if (!reserve(p, v, (void **)&v->classes, v->class_count, &v->class_capacity, sizeof(*v->classes))) {
        return 0;
    }
    markdown_core_chunk item = {0};
    if (!copy(p, &item, s, n)) {
        return 0;
    }
    v->classes[v->class_count++] = item;
    return 1;
}

static int normalize(markdown_core_attribute_parser *p, markdown_core_attributes *v, const unsigned char *name,
                     bufsize_t length, const unsigned char *value, bufsize_t size) {
    if (length == 2 && memcmp(name, "id", 2) == 0) {
        return copy(p, &v->anchor, value, size);
    }
    if (length == 5 && memcmp(name, "class", 5) == 0) {
        bufsize_t word = 0, at = 0;
        while (at < size) {
            bufsize_t width;
            int32_t cp = scalar(value, size, at, &width);
            if (markdown_core_utf8proc_is_space(cp) || cp == 11) {
                if (at > word && !append_class(p, v, value + word, at - word)) {
                    return 0;
                }
                word = at + width;
            }
            at += width;
        }
        return at == word || append_class(p, v, value + word, at - word);
    }
    if (!reserve(p, v, (void **)&v->records, v->record_count, &v->record_capacity, sizeof(*v->records))) {
        return 0;
    }
    markdown_core_record item = {0};
    if (!copy(p, &item.name, name, length) || !copy(p, &item.value, value, size)) {
        markdown_core_chunk_free(p->mem, &item.name);
        markdown_core_chunk_free(p->mem, &item.value);
        return 0;
    }
    v->records[v->record_count++] = item;
    return 1;
}

static bufsize_t scan_name(markdown_core_attribute_parser *p, bufsize_t n, bufsize_t at) {
    MARKDOWN_CORE_DIAGNOSTIC(size_t work = 0;)
    const unsigned char *s = p->data;
    while (at < n) {
        bufsize_t width;
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
        if (!name_rest(scalar(s, n, at, &width))) {
            break;
        }
        at += width;
    }
    MARKDOWN_CORE_DIAGNOSTIC(p->work += work;)
    return at;
}

/* The byte after `{` that can begin a member list: blank space, a line end,
 * the `-` shorthand, an anchor or class marker, the closing brace, or the
 * first byte of a letter (every non-ASCII lead byte may start one). Any
 * other byte is a grammar failure that needs no memoized fact. */
static int member_list_can_begin(unsigned char c) {
    return horizontal(c) || newline(c) || c == '-' || c == '#' || c == '.' || c == '}' || markdown_core_isalpha(c) ||
           c >= 0x80;
}

bufsize_t markdown_core_attributes_end(markdown_core_attribute_parser *p, bufsize_t start) {
    MARKDOWN_CORE_DIAGNOSTIC(p->work++;)
    if (p->oom) {
        return 0;
    }
    if (start < 0 || start + 1 >= p->length || p->data[start] != '{' || !member_list_can_begin(p->data[start + 1])) {
        return 0;
    }
    attribute_fact *fact = fact_at(p, start + 1);
    return !fact ? 0 : fact->end >= 0 ? fact->end : recognize(p, fact);
}

bufsize_t markdown_core_attributes_tail(markdown_core_attribute_parser *p, bufsize_t start, bufsize_t end) {
    MARKDOWN_CORE_DIAGNOSTIC(size_t work = 0;)
    if (end <= start || p->data[end - 1] != '}') {
        return -1;
    }
    bufsize_t result = -1;
    for (bufsize_t at = start; at < end; at++) {
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
        if (escaped(p->data, end, at)) {
            at++;
        } else if (p->data[at] == '{' && markdown_core_attributes_end(p, at) == end) {
            result = at;
            break;
        }
    }
    MARKDOWN_CORE_DIAGNOSTIC(p->work += work;)
    return result;
}

int markdown_core_attributes_parse(markdown_core_attribute_parser *p, bufsize_t start, markdown_core_attributes *result,
                                   bufsize_t *end) {
    MARKDOWN_CORE_DIAGNOSTIC(size_t work = 0;)
    const unsigned char *s = p->data;
    bufsize_t finish = markdown_core_attributes_end(p, start);
    if (!finish) {
        return 0;
    }
    markdown_core_attributes value = {0};
    markdown_core_strbuf *decoded = &p->decoded;
    bufsize_t at = start + 1;
    while (at < finish - 1) {
        MARKDOWN_CORE_DIAGNOSTIC(work++;)
        if (horizontal(s[at]) || newline(s[at])) {
            at++;
            continue;
        }
        if (s[at] == '-') {
            if (!append_class(p, &value, (const unsigned char *)"unnumbered", 10)) {
                goto oom;
            }
            at++;
            continue;
        }
        if (s[at] == '#' || s[at] == '.') {
            unsigned char marker = s[at++];
            bufsize_t from = at;
            at = scan_name(p, finish, at);
            if (marker == '#' ? !copy(p, &value.anchor, s + from, at - from)
                              : !append_class(p, &value, s + from, at - from)) {
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
                MARKDOWN_CORE_DIAGNOSTIC(work++;)
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
                MARKDOWN_CORE_DIAGNOSTIC(work++;)
                last += escaped(s, finish, last) ? 2 : 1;
            }
        }
        /* A value whose bytes stand for themselves is read where it lies: no
         * escape to unfold, no entity to resolve, no line ending to fold into
         * a space. Only the remainder is decoded, and only such a parse ever
         * takes the buffer it is decoded through. */
        const unsigned char *bytes = s + at;
        bufsize_t size = last - at;
        bufsize_t plain = at;
        while (plain < last && s[plain] != '\\' && !(quoted && (s[plain] == '&' || newline(s[plain])))) {
            MARKDOWN_CORE_DIAGNOSTIC(work++;)
            plain++;
        }
        if (plain < last) {
            if (!decoded->mem) {
                markdown_core_strbuf_init(p->mem, decoded, 0);
            }
            markdown_core_strbuf_clear(decoded);
            while (at < last) {
                MARKDOWN_CORE_DIAGNOSTIC(work++;)
                if (escaped(s, last, at)) {
                    markdown_core_strbuf_putc(decoded, s[at + 1]);
                    at += 2;
                } else if (quoted && s[at] == '&') {
                    bufsize_t used = houdini_unescape_ent(decoded, s + at + 1, last - at - 1);
                    if (used) {
                        at += used + 1;
                    } else {
                        markdown_core_strbuf_putc(decoded, s[at++]);
                    }
                } else if (quoted && newline(s[at])) {
                    if (s[at] == '\r' && at + 1 < last && s[at + 1] == '\n') {
                        at++;
                    }
                    at++;
                    markdown_core_strbuf_putc(decoded, ' ');
                } else {
                    markdown_core_strbuf_putc(decoded, s[at++]);
                }
            }
            if (decoded->oom) {
                goto oom;
            }
            bytes = decoded->ptr;
            size = decoded->size;
        }
        at = last;
        if (quoted) {
            at++;
        }
        if (!normalize(p, &value, s + name, name_length, bytes, size)) {
            goto oom;
        }
    }
    MARKDOWN_CORE_DIAGNOSTIC(p->work += work + (size_t)(finish - start);)
    markdown_core_strbuf_clear(decoded);
    *result = value;
    *end = finish;
    return 1;
oom:
    MARKDOWN_CORE_DIAGNOSTIC(p->work += work;)
    p->oom = 1;
    markdown_core_strbuf_clear(decoded);
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
        inline_state->attributes.store = inline_state->arena;
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
    markdown_core_attributes value = {0};
    if (markdown_core_inline_state_attributes(inline_state, inline_state->pos, &value, &end)) {
        markdown_core_attributes *owned = markdown_core_node_attributes_mut(node, inline_state->arena);
        if (!owned) {
            markdown_core_attributes_free(inline_state->mem, &value);
            inline_state->oom = 1;
            return;
        }
        markdown_core_attributes_free(inline_state->mem, owned);
        *owned = value;
        if (owned->anchor.len) {
            markdown_core_inline_request_completion(inline_state);
        }
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
        .mem = parser->mem, .store = parser->arena, .data = source, .length = length};
    bufsize_t attribute_start = markdown_core_attributes_tail(&attributes, 0, info_end);
    markdown_core_attributes value = {0};
    if (attribute_start >= 0 && markdown_core_attributes_parse(&attributes, attribute_start, &value, &attribute_end)) {
        markdown_core_attributes *owned = markdown_core_node_attributes_mut(node, parser->arena);
        if (owned) {
            markdown_core_attributes_free(parser->mem, owned);
            *owned = value;
            info_end = attribute_start;
        } else {
            markdown_core_attributes_free(parser->mem, &value);
            attributes.oom = 1;
        }
    }
    if (attributes.oom) {
        parser->oom = true;
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->attribute_work += attributes.work;)
    markdown_core_attribute_parser_free(&attributes);
    return info_end;
}

static void dispose_inline(markdown_core_inline_state *inline_state) {
    if (inline_state->attributes.mem) {
        MARKDOWN_CORE_DIAGNOSTIC(inline_state->owner_parser->attribute_work += inline_state->attributes.work;)
        markdown_core_attribute_parser_free(&inline_state->attributes);
    }
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES = {
    .name = "attributes",
    .dispose_inline = dispose_inline,
};
