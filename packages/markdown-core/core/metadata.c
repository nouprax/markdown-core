#include "metadata.h"
#include "parser.h"
#include "node.h"
#include "buffer.h"
#include "utf8.h"
#include <string.h>
#include <limits.h>

/* Properties is an ordered, recovering source decoder. A root member owns its
 * indented continuation; a flow member owns its balanced value. Unsupported
 * members are skipped without changing the envelope or interpreting their
 * interiors. No source range is retried. Public allocations belong to the
 * completed document value. */
typedef struct {
    size_t start, end;
} source_span;
typedef struct {
    size_t start;
} source_line;
/* Syntax contexts differ only where the documented JSON spelling requires
 * quoted keys, JSON scalars/escapes and strict array separators. Lists are
 * flat: their items call the scalar decoder directly, without recursion. */
typedef enum { PROPERTY_SYNTAX, LIST_SYNTAX, JSON_SYNTAX } value_syntax;
typedef struct {
    markdown_core_parser *parser;
    const unsigned char *source;
    source_line *lines;
    size_t line_count, line_capacity;
    markdown_core_metadata *metadata;
    size_t capacity;
    unsigned int fields;
    source_span *flows;
    size_t flow_count, flow_capacity;
} properties;
typedef struct {
    properties *owner;
    size_t pos, end, last;
} decoder;

static bool space(unsigned char c) { return c == ' ' || c == '\t'; }
static bool newline(unsigned char c) { return c == '\r' || c == '\n'; }
static bool plain_start(const unsigned char *s, size_t start, size_t end) {
    if (start == end || space(s[start]) || strchr(",[]{}#&*!|>'\"%@`", s[start])) {
        return false;
    }
    return !((s[start] == '-' || s[start] == '?' || s[start] == ':') &&
             (start + 1 == end || space(s[start + 1]) || newline(s[start + 1])));
}
static size_t line_end(const unsigned char *s, size_t p, size_t end) {
    while (p < end && !newline(s[p])) {
        p++;
    }
    return p;
}
static size_t next_line(const unsigned char *s, size_t p, size_t end) {
    if (p < end && s[p] == '\r') {
        p++;
    }
    if (p < end && s[p] == '\n') {
        p++;
    }
    return p;
}
static bool grow(properties *p, void **array, size_t *capacity, size_t count, size_t width) {
    if (count <= *capacity) {
        return true;
    }
    size_t n = *capacity ? *capacity : 8;
    while (n < count) {
        if (n > SIZE_MAX / 2) {
            p->parser->oom = true;
            return false;
        }
        n *= 2;
    }
    if (n > SIZE_MAX / width) {
        p->parser->oom = true;
        return false;
    }
    void *value = p->parser->mem->realloc(*array, n * width);
    if (!value) {
        p->parser->oom = true;
        return false;
    }
    *array = value;
    *capacity = n;
    return true;
}
static markdown_core_string copy(properties *p, const unsigned char *s, size_t size) {
    unsigned char *data = p->parser->mem->calloc(size + 1, 1);
    if (!data) {
        p->parser->oom = true;
        return (markdown_core_string){0};
    }
    if (size) {
        memcpy(data, s, size);
    }
    return (markdown_core_string){data, size};
}
static void free_value(markdown_core_mem *mem, markdown_core_metadata_value *v) {
    if (v->kind == MARKDOWN_CORE_METADATA_SCALAR &&
        (v->as.scalar.kind == MARKDOWN_CORE_METADATA_NUMBER || v->as.scalar.kind == MARKDOWN_CORE_METADATA_TEXT)) {
        mem->free((void *)v->as.scalar.value.string.data);
    } else if (v->kind == MARKDOWN_CORE_METADATA_LIST) {
        for (size_t i = 0; i < v->as.list.count; i++) {
            mem->free((void *)v->as.list.items[i].value.data);
        }
        mem->free(v->as.list.items);
    }
    memset(v, 0, sizeof(*v));
}
void markdown_core_metadata_free(markdown_core_mem *mem, markdown_core_metadata *metadata) {
    if (!metadata) {
        return;
    }
    for (size_t i = 0; i < metadata->count; i++) {
        markdown_core_metadata_record *record = &metadata->content[i];
        mem->free((void *)record->name.data);
        free_value(mem, &record->value);
    }
    mem->free(metadata->content);
    mem->free(metadata);
}
static size_t line_index(properties *p, size_t offset) {
    size_t lo = 0, hi = p->line_count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        p->parser->metadata_line_lookup_work++;
        if (p->lines[mid].start <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}
static markdown_core_position position(properties *p, size_t offset) {
    size_t line = line_index(p, offset);
    return (markdown_core_position){(int32_t)(line + 1), (int32_t)(offset - p->lines[line].start + 1)};
}
static markdown_core_scope extent(properties *p, size_t start, size_t end) {
    return (markdown_core_scope){position(p, start), position(p, end > start ? end - 1 : start)};
}
static bool append(properties *p, markdown_core_metadata_record value) {
    if (!grow(p, (void **)&p->metadata->content, &p->capacity, p->metadata->count + 1, sizeof(*p->metadata->content))) {
        return false;
    }
    p->metadata->content[p->metadata->count++] = value;
    return true;
}
static void skip(decoder *d) {
    const unsigned char *s = d->owner->source;
    while (d->pos < d->end) {
        if (space(s[d->pos]) || newline(s[d->pos])) {
            d->pos++;
        } else if (s[d->pos] == '#' && (d->pos == 0 || space(s[d->pos - 1]) || newline(s[d->pos - 1]))) {
            size_t end = line_end(s, d->pos, d->end);
            d->pos = end;
        } else {
            break;
        }
    }
}
static bool single_line(markdown_core_string s) {
    return !memchr(s.data, '\n', s.length) && !memchr(s.data, '\r', s.length);
}
static bool printable(properties *p, size_t start, size_t end) {
    for (size_t i = start; i < end;) {
        int32_t c;
        int n = markdown_core_utf8proc_iterate(p->source + i, (bufsize_t)(end - i), &c);
        if (n <= 0) {
            return false; /* Valid UTF-8 remains the public precondition. */
        }
        if (!(c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0x7e) || c == 0x85 || (c >= 0xa0 && c <= 0xd7ff) ||
              (c >= 0xe000 && c <= 0xfffd) || c >= 0x10000)) {
            return false;
        }
        i += (size_t)n;
    }
    return true;
}
static bool finish_string(decoder *d, markdown_core_strbuf *buf, markdown_core_string *value) {
    if (buf->oom) {
        d->owner->parser->oom = true;
        return false;
    }
    *value = copy(d->owner, buf->ptr, (size_t)buf->size);
    return value->data != NULL;
}
static bool hex4(decoder *d, uint32_t *scalar) {
    *scalar = 0;
    for (size_t i = 0; i < 4; i++) {
        if (d->pos == d->end) {
            return false;
        }
        unsigned char c = d->owner->source[d->pos++];
        int digit = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
        if (digit < 0) {
            return false;
        }
        *scalar = (*scalar << 4) | (uint32_t)digit;
    }
    return true;
}
static bool quoted(decoder *d, value_syntax syntax, markdown_core_string *value) {
    const unsigned char *s = d->owner->source;
    unsigned char quote = s[d->pos++];
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(d->owner->parser->mem);
    bool closed = false, valid = true;
    while (d->pos < d->end && valid && !buf.oom) {
        unsigned char c = s[d->pos++];
        if (c == quote) {
            if (quote == '\'' && d->pos < d->end && s[d->pos] == '\'') {
                d->pos++;
                markdown_core_strbuf_putc(&buf, '\'');
            } else {
                closed = true;
                break;
            }
        } else if (quote == '"' && c == '\\') {
            if (d->pos == d->end) {
                valid = false;
                break;
            }
            c = s[d->pos++];
            const char *escapes = "btnfr\"/\\";
            const unsigned char replacements[] = {8, 9, 10, 12, 13, '"', '/', '\\'};
            const char *found = c ? strchr(escapes, c) : NULL;
            if (found) {
                markdown_core_strbuf_putc(&buf, replacements[found - escapes]);
            } else if (c == 'u') {
                uint32_t scalar;
                valid = hex4(d, &scalar);
                if (valid && scalar >= 0xd800 && scalar <= 0xdbff) {
                    uint32_t low;
                    valid = d->end - d->pos >= 2 && s[d->pos] == '\\' && s[d->pos + 1] == 'u';
                    if (valid) {
                        d->pos += 2;
                        valid = hex4(d, &low) && low >= 0xdc00 && low <= 0xdfff;
                        if (valid) {
                            scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
                        }
                    }
                }
                if (valid && !(scalar >= 0xd800 && scalar <= 0xdfff)) {
                    markdown_core_utf8proc_encode_char((int32_t)scalar, &buf);
                } else {
                    valid = false;
                }
            } else {
                valid = false;
            }
        } else if (c < 0x20 && (syntax == JSON_SYNTAX || c != '\t')) {
            valid = false;
        } else {
            markdown_core_strbuf_putc(&buf, c);
        }
    }
    valid = valid && closed && finish_string(d, &buf, value);
    if (buf.oom) {
        d->owner->parser->oom = true;
    }
    markdown_core_strbuf_free(&buf);
    if (valid) {
        d->last = d->pos;
    }
    return valid;
}
static bool plain(decoder *d, bool delimited, bool key, markdown_core_string *value) {
    const unsigned char *s = d->owner->source;
    size_t start = d->pos, last = start;
    if (!plain_start(s, start, d->end) || newline(s[start])) {
        return false;
    }
    while (d->pos < d->end && !newline(s[d->pos])) {
        unsigned char c = s[d->pos];
        bool colon = c == ':' && (d->pos + 1 == d->end || space(s[d->pos + 1]) || newline(s[d->pos + 1]) ||
                                  (delimited && strchr(",[]{}", s[d->pos + 1])));
        if ((delimited && strchr(",[]{}", c)) || (c == '#' && (d->pos == start || space(s[d->pos - 1])))) {
            break;
        }
        if (colon) {
            if (!key) {
                return false;
            }
            break;
        }
        d->pos++;
        if (!space(c)) {
            last = d->pos;
        }
    }
    *value = copy(d->owner, s + start, last - start);
    d->last = last;
    return value->data != NULL;
}
static bool equals(markdown_core_string s, const char *text) {
    size_t n = strlen(text);
    return s.length == n && memcmp(s.data, text, n) == 0;
}
/* The fixed field set bounds both duplicate tracking and committed records. */
static unsigned int field_id(markdown_core_string name) {
    static const char *names[] = {"name",     "time",  "date",    "authors", "keywords",
                                  "abstract", "state", "comment", "title",   "subtitle"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if (equals(name, names[i])) {
            return 1u << i;
        }
    }
    return 0;
}
/* Only the two prose fields accept the literal | form. The first nonblank
 * line fixes its space indentation; inner indentation and blank lines are
 * text. Source line endings normalize to LF, with the default clipped final
 * newline. No folding, escaping, chomping flags or explicit indent indicators. */
static bool literal(decoder *d, size_t key_start, markdown_core_metadata_value *value) {
    const unsigned char *s = d->owner->source;
    size_t key_line = key_start;
    while (key_line && !newline(s[key_line - 1])) {
        key_line--;
    }
    size_t key_indent = key_start - key_line;
    d->last = ++d->pos;
    if (d->pos < d->end && !space(s[d->pos]) && !newline(s[d->pos])) {
        return false;
    }
    while (d->pos < d->end && space(s[d->pos])) {
        d->pos++;
    }
    if (d->pos < d->end && s[d->pos] == '#') {
        d->pos = line_end(s, d->pos, d->end);
    }
    if (d->pos < d->end && !newline(s[d->pos])) {
        return false;
    }
    d->pos = next_line(s, d->pos, d->end);
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT(d->owner->parser->mem);
    size_t indent = 0, clipped = 0;
    bool valid = true;
    while (d->pos < d->end) {
        size_t end = line_end(s, d->pos, d->end), content = d->pos;
        while (content < end && s[content] == ' ') {
            content++;
        }
        size_t nonblank = content;
        while (nonblank < end && space(s[nonblank])) {
            nonblank++;
        }
        if (nonblank == end) {
            markdown_core_strbuf_putc(&text, '\n');
        } else {
            if (!indent) {
                indent = content - d->pos;
            }
            if (indent <= key_indent || content - d->pos < indent) {
                valid = false;
                break;
            }
            markdown_core_strbuf_put(&text, s + d->pos + indent, (bufsize_t)(end - d->pos - indent));
            markdown_core_strbuf_putc(&text, '\n');
            clipped = (size_t)text.size;
            d->last = end;
            while (d->last > content && space(s[d->last - 1])) {
                d->last--;
            }
        }
        d->pos = next_line(s, end, d->end);
    }
    value->kind = MARKDOWN_CORE_METADATA_SCALAR;
    value->as.scalar.kind = MARKDOWN_CORE_METADATA_TEXT;
    markdown_core_strbuf_truncate(&text, (bufsize_t)clipped);
    valid = valid && finish_string(d, &text, &value->as.scalar.value.string);
    if (text.oom) {
        d->owner->parser->oom = true;
    }
    markdown_core_strbuf_free(&text);
    return valid;
}
static bool number(markdown_core_string s, value_syntax syntax) {
    size_t i = 0;
    if (i < s.length && s.data[i] == '-') {
        i++;
    }
    if (i == s.length) {
        return false;
    }
    if (s.data[i] == '0') {
        i++;
    } else {
        if (s.data[i] < '1' || s.data[i] > '9') {
            return false;
        }
        do {
            i++;
        } while (i < s.length && s.data[i] >= '0' && s.data[i] <= '9');
    }
    if (i < s.length && s.data[i] == '.') {
        i++;
        if (syntax == JSON_SYNTAX && (i == s.length || s.data[i] < '0' || s.data[i] > '9')) {
            return false;
        }
        while (i < s.length && s.data[i] >= '0' && s.data[i] <= '9') {
            i++;
        }
    }
    if (i < s.length && (s.data[i] == 'e' || s.data[i] == 'E')) {
        i++;
        if (i < s.length && (s.data[i] == '+' || s.data[i] == '-')) {
            i++;
        }
        size_t digits = i;
        while (i < s.length && s.data[i] >= '0' && s.data[i] <= '9') {
            i++;
        }
        if (digits == i) {
            return false;
        }
    }
    return i == s.length;
}
static bool scalar(decoder *d, value_syntax syntax, markdown_core_metadata_value *value) {
    const unsigned char *s = d->owner->source;
    markdown_core_string text = {0};
    bool quoted_style = d->pos < d->end && (s[d->pos] == '"' || (syntax != JSON_SYNTAX && s[d->pos] == '\''));
    bool valid = quoted_style ? quoted(d, syntax, &text) : plain(d, syntax != PROPERTY_SYNTAX, false, &text);
    if (!valid || (!quoted_style && !text.length) || !single_line(text)) {
        d->owner->parser->mem->free((void *)text.data);
        return false;
    }
    markdown_core_metadata_scalar *result = &value->as.scalar;
    value->kind = MARKDOWN_CORE_METADATA_SCALAR;
    result->kind = quoted_style                                    ? MARKDOWN_CORE_METADATA_TEXT
                   : equals(text, "null")                          ? MARKDOWN_CORE_METADATA_NULL
                   : equals(text, "true") || equals(text, "false") ? MARKDOWN_CORE_METADATA_BOOL
                   : number(text, syntax)                          ? MARKDOWN_CORE_METADATA_NUMBER
                                                                   : MARKDOWN_CORE_METADATA_TEXT;
    if (syntax == JSON_SYNTAX && !quoted_style && result->kind == MARKDOWN_CORE_METADATA_TEXT) {
        valid = false;
    }
    if (valid && (result->kind == MARKDOWN_CORE_METADATA_TEXT || result->kind == MARKDOWN_CORE_METADATA_NUMBER)) {
        result->value.string = text;
    } else {
        if (result->kind == MARKDOWN_CORE_METADATA_BOOL) {
            result->value.boolean = equals(text, "true");
        }
        d->owner->parser->mem->free((void *)text.data);
    }
    return valid && !d->owner->parser->oom;
}
static bool list_item(decoder *d, value_syntax syntax, markdown_core_metadata_value *list, size_t *capacity) {
    markdown_core_metadata_value value = {0};
    bool valid = scalar(d, syntax, &value);
    if (!valid || (value.as.scalar.kind != MARKDOWN_CORE_METADATA_NUMBER &&
                   value.as.scalar.kind != MARKDOWN_CORE_METADATA_TEXT)) {
        free_value(d->owner->parser->mem, &value);
        return false;
    }
    if (!grow(d->owner, (void **)&list->as.list.items, capacity, list->as.list.count + 1,
              sizeof(*list->as.list.items))) {
        free_value(d->owner->parser->mem, &value);
        return false;
    }
    list->as.list.items[list->as.list.count++] = (markdown_core_metadata_list_item){
        value.as.scalar.kind == MARKDOWN_CORE_METADATA_NUMBER ? MARKDOWN_CORE_METADATA_ITEM_NUMBER
                                                              : MARKDOWN_CORE_METADATA_ITEM_TEXT,
        value.as.scalar.value.string};
    return true;
}
static bool sequence(decoder *d, value_syntax syntax, markdown_core_metadata_value *value) {
    const unsigned char *s = d->owner->source;
    value->kind = MARKDOWN_CORE_METADATA_LIST;
    size_t capacity = 0;
    if (s[d->pos] == '[') {
        value_syntax item_syntax = syntax == JSON_SYNTAX ? JSON_SYNTAX : LIST_SYNTAX;
        d->pos++;
        skip(d);
        if (d->pos < d->end && s[d->pos] == ']') {
            d->last = ++d->pos;
            return true;
        }
        while (d->pos < d->end && !d->owner->parser->oom) {
            if (!list_item(d, item_syntax, value, &capacity)) {
                return false;
            }
            skip(d);
            if (d->pos == d->end) {
                return false;
            }
            if (s[d->pos] == ']') {
                d->last = ++d->pos;
                return true;
            }
            if (s[d->pos++] != ',') {
                return false;
            }
            skip(d);
            if (d->pos < d->end && s[d->pos] == ']') {
                if (syntax == JSON_SYNTAX) {
                    return false;
                }
                d->last = ++d->pos;
                return true;
            }
        }
        return false;
    }
    /* Each block member owns exactly its indented continuation. The decoder's
     * range contracts once, then resumes after it; no member is scanned twice. */
    size_t outer_end = d->end;
    while (d->pos < outer_end && !d->owner->parser->oom) {
        size_t start = d->pos;
        if (s[start] != '-' || (start + 1 < outer_end && !space(s[start + 1]) && !newline(s[start + 1]))) {
            return false;
        }
        size_t line_start = start;
        while (line_start && !newline(s[line_start - 1])) {
            line_start--;
        }
        size_t item_indent = start - line_start;
        d->pos++;
        while (d->pos < outer_end && space(s[d->pos])) {
            d->pos++;
        }
        size_t e = next_line(s, line_end(s, start, outer_end), outer_end);
        while (e < outer_end) {
            size_t p = e, end = line_end(s, e, outer_end);
            while (p < end && space(s[p])) {
                p++;
            }
            if (p < end && s[p] != '#' && p - e <= item_indent) {
                break;
            }
            e = next_line(s, end, outer_end);
        }
        d->end = e;
        bool valid = list_item(d, PROPERTY_SYNTAX, value, &capacity);
        skip(d);
        valid = valid && d->pos == d->end;
        d->end = outer_end;
        if (!valid) {
            return false;
        }
        d->pos = e;
        skip(d);
    }
    return true;
}
static bool record(decoder *d, value_syntax syntax) {
    properties *p = d->owner;
    const unsigned char *s = p->source;
    size_t start = d->pos;
    p->parser->metadata_decoded_bytes += d->end - start;
    markdown_core_metadata_record content = {0};
    markdown_core_metadata_record *r = &content;
    bool quoted_key = d->pos < d->end && (s[d->pos] == '\'' || s[d->pos] == '"');
    bool valid = d->pos == d->end || (syntax == JSON_SYNTAX && s[d->pos] != '"') ? false
                 : quoted_key                                                    ? quoted(d, syntax, &r->name)
                                                                                 : plain(d, false, true, &r->name);
    valid = valid && r->name.length && single_line(r->name) && !memchr(s + start, '\n', d->pos - start) &&
            !memchr(s + start, '\r', d->pos - start);
    while (d->pos < d->end && space(s[d->pos])) {
        d->pos++;
    }
    unsigned int field = field_id(r->name);
    if (!valid || !field || (p->fields & field) || d->pos == d->end || s[d->pos++] != ':') {
        goto failed;
    }
    if (syntax == PROPERTY_SYNTAX && d->pos < d->end && !space(s[d->pos]) && !newline(s[d->pos])) {
        goto failed;
    }
    d->last = d->pos;
    size_t value_line_end = line_end(s, d->pos, d->end);
    skip(d);
    if (d->pos == d->end && syntax == PROPERTY_SYNTAX) {
        r->value.kind = MARKDOWN_CORE_METADATA_SCALAR;
        r->value.as.scalar.kind = MARKDOWN_CORE_METADATA_NULL;
    } else if (syntax == PROPERTY_SYNTAX && d->pos < value_line_end && s[d->pos] == '|' &&
               (equals(r->name, "abstract") || equals(r->name, "comment"))) {
        if (!literal(d, start, &r->value)) {
            goto failed;
        }
    } else if (d->pos < d->end && (s[d->pos] == '[' || (syntax == PROPERTY_SYNTAX && s[d->pos] == '-' &&
                                                        d->pos + 1 < d->end && space(s[d->pos + 1])))) {
        if (!sequence(d, syntax, &r->value)) {
            goto failed;
        }
    } else {
        if ((syntax == PROPERTY_SYNTAX && d->pos > value_line_end) || !scalar(d, syntax, &r->value)) {
            goto failed;
        }
    }
    r->scope = extent(p, start, d->last);
    skip(d);
    if (d->pos != d->end) {
        goto failed;
    }
    if (!append(p, content)) {
        goto failed;
    }
    p->fields |= field;
    return true;
failed:
    p->parser->mem->free((void *)r->name.data);
    free_value(p->parser->mem, &r->value);
    return false;
}
/* Locate a source member without interpreting it. Brackets and quoted strings
 * delimit flow members; indentation delimits block members. Recovery resumes
 * only at these boundaries, never at a colon found inside a failed value. */
static size_t flow_boundary(const unsigned char *s, size_t start, size_t end) {
    size_t depth = 0, p = start;
    unsigned char quote = 0;
    while (p < end) {
        unsigned char c = s[p];
        if (newline(c)) {
            quote = 0;
        }
        if (quote) {
            if (quote == '"' && c == '\\' && p + 1 < end) {
                p++;
            } else if (c == quote) {
                if (quote == '\'' && p + 1 < end && s[p + 1] == '\'') {
                    p++;
                } else {
                    quote = 0;
                }
            }
        } else if ((c == '\'' || c == '"') &&
                   (p == start || space(s[p - 1]) || newline(s[p - 1]) || strchr(":,[{?", s[p - 1]))) {
            quote = c;
        } else if (c == '#' && (p == start || space(s[p - 1]) || newline(s[p - 1]))) {
            p = line_end(s, p, end);
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            if (!depth) {
                return p;
            }
            depth--;
        } else if (c == ',' && !depth) {
            return p;
        }
        if (p < end) {
            p++;
        }
    }
    return p;
}
static void json_mapping(properties *p, size_t start, size_t end) {
    decoder d = {.owner = p, .pos = start + 1, .end = end};
    skip(&d);
    while (d.pos < end && p->source[d.pos] != '}' && !p->parser->oom) {
        size_t item = d.pos;
        size_t boundary = flow_boundary(p->source, item, end);
        d.end = boundary;
        if (printable(p, item, boundary)) {
            record(&d, JSON_SYNTAX);
        }
        d.end = end;
        d.pos = boundary;
        if (d.pos < end && p->source[d.pos] == ',') {
            d.pos++;
        } else {
            break;
        }
        skip(&d);
    }
}
/* Return the byte after a directly authored block key's colon. The same
 * lexical rule identifies recovery boundaries. Flow punctuation is
 * ordinary content inside a block plain key; only a separated # is a comment. */
static size_t block_key_end(const unsigned char *s, size_t start, size_t end) {
    if (start == end) {
        return 0;
    }
    if (s[start] == '\'' || s[start] == '"') {
        unsigned char quote = s[start++];
        bool closed = false;
        while (start < end) {
            unsigned char c = s[start++];
            if (quote == '"' && c == '\\' && start < end) {
                start++;
            } else if (c == quote) {
                if (quote == '\'' && start < end && s[start] == '\'') {
                    start++;
                } else {
                    closed = true;
                    break;
                }
            }
        }
        while (start < end && space(s[start])) {
            start++;
        }
        return closed && start < end && s[start] == ':' && (start + 1 == end || space(s[start + 1])) ? start + 1 : 0;
    }
    if (!plain_start(s, start, end)) {
        return 0;
    }
    for (size_t i = start; i < end; i++) {
        if (s[i] == ':' && (i + 1 == end || space(s[i + 1]))) {
            return i + 1;
        }
        if (s[i] == '#' && (i == start || space(s[i - 1]))) {
            return 0;
        }
    }
    return 0;
}

static size_t block_boundary(const unsigned char *s, size_t start, size_t end, size_t indent) {
    enum { VALUE_PREFIX, VALUE_SCALAR, VALUE_QUOTED, VALUE_SEQUENCE, VALUE_FLOW } form = VALUE_PREFIX;
    size_t cursor = start, depth = 0;
    unsigned char quote = 0;
    bool first = true;
    while (cursor < end) {
        size_t e = line_end(s, cursor, end), nonspace = cursor;
        while (nonspace < e && s[nonspace] == ' ') {
            nonspace++;
        }
        if (!first && nonspace < e && nonspace - cursor <= indent) {
            bool list_line =
                nonspace - cursor == indent && s[nonspace] == '-' && (nonspace + 1 == e || space(s[nonspace + 1]));
            bool recovery_key = block_key_end(s, nonspace, e) || s[nonspace] == '{';
            size_t content = nonspace;
            while (content < e && space(s[content])) {
                content++;
            }
            bool separation = content == e || s[content] == '#';
            bool continuation = content == e ||
                                ((form == VALUE_PREFIX || form == VALUE_SEQUENCE) && (list_line || separation)) ||
                                ((depth || quote) && !recovery_key);
            if (!continuation) {
                return cursor;
            }
        }
        size_t token = first ? block_key_end(s, nonspace, e) : 0;
        for (size_t i = token ? token : nonspace; i < e; i++) {
            unsigned char c = s[i];
            if (form == VALUE_PREFIX) {
                if (space(c)) {
                    continue;
                }
                if (c == '#') {
                    break;
                }
                /* Only the value's node token can open a flow collection.
                 * Brackets and quotes within block plain scalars, including
                 * block sequence items, never extend the root member. */
                form = c == '[' || c == '{'                          ? VALUE_FLOW
                       : c == '\'' || c == '"'                       ? VALUE_QUOTED
                       : c == '-' && (i + 1 == e || space(s[i + 1])) ? VALUE_SEQUENCE
                                                                     : VALUE_SCALAR;
            }
            if (form != VALUE_FLOW && form != VALUE_QUOTED) {
                break;
            }
            if (quote) {
                if (quote == '"' && c == '\\' && i + 1 < e) {
                    i++;
                } else if (c == quote) {
                    if (quote == '\'' && i + 1 < e && s[i + 1] == '\'') {
                        i++;
                    } else {
                        quote = 0;
                        if (form == VALUE_QUOTED) {
                            form = VALUE_SCALAR;
                        }
                    }
                }
            } else if ((c == '\'' || c == '"') && (i == nonspace || space(s[i - 1]) || strchr(":,[{?", s[i - 1]))) {
                quote = c;
            } else if (c == '#' && (i == nonspace || space(s[i - 1]))) {
                break;
            } else if (c == '[' || c == '{') {
                depth++;
            } else if ((c == ']' || c == '}') && depth) {
                if (!--depth) {
                    form = VALUE_SCALAR;
                }
            }
        }
        first = false;
        cursor = next_line(s, e, end);
    }
    return cursor;
}

/* Match flow delimiters once for the payload. A root candidate consults this
 * index instead of searching the remaining source, so many unclosed roots
 * followed by recoverable records cannot cause repeated suffix scans. Entries
 * remain in opening-source order; stack indices survive vector growth. */
static void index_flows(properties *p, size_t start, size_t end) {
    const unsigned char *s = p->source;
    size_t *stack = NULL, count = 0, capacity = 0;
    unsigned char quote = 0;
    for (size_t i = start; i < end && !p->parser->oom; i++) {
        unsigned char c = s[i];
        if (newline(c)) {
            if (c == '\r' && i + 1 < end && s[i + 1] == '\n') {
                i++;
            }
            quote = 0;
            continue;
        }
        if (quote) {
            if (quote == '"' && c == '\\' && i + 1 < end && !newline(s[i + 1])) {
                i++;
            } else if (c == quote) {
                if (quote == '\'' && i + 1 < end && s[i + 1] == '\'') {
                    i++;
                } else {
                    quote = 0;
                }
            }
        } else if ((c == '\'' || c == '"') &&
                   (i == start || space(s[i - 1]) || newline(s[i - 1]) || strchr(":,[{?", s[i - 1]))) {
            quote = c;
        } else if (c == '#' && (i == start || space(s[i - 1]) || newline(s[i - 1]))) {
            i = line_end(s, i, end);
            if (i < end) {
                i = next_line(s, i, end) - 1;
            }
        } else if (c == '[' || c == '{') {
            if (!grow(p, (void **)&p->flows, &p->flow_capacity, p->flow_count + 1, sizeof(*p->flows)) ||
                !grow(p, (void **)&stack, &capacity, count + 1, sizeof(*stack))) {
                break;
            }
            stack[count++] = p->flow_count;
            p->flows[p->flow_count++] = (source_span){i, 0};
        } else if ((c == ']' || c == '}') && count) {
            source_span *open = &p->flows[stack[count - 1]];
            if (s[open->start] == (c == ']' ? '[' : '{')) {
                open->end = i + 1;
                count--;
            }
        }
    }
    p->parser->mem->free(stack);
}
static size_t flow_end(properties *p, size_t start) {
    size_t lo = 0, hi = p->flow_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->flows[mid].start < start) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < p->flow_count && p->flows[lo].start == start ? p->flows[lo].end : 0;
}
static void payload(properties *p, size_t start, size_t end) {
    const unsigned char *s = p->source;
    size_t cursor = start;
    while (cursor < end && !p->parser->oom) {
        size_t first = cursor, e = line_end(s, cursor, end);
        while (first < e && s[first] == ' ') {
            first++;
        }
        size_t content = first;
        while (content < e && space(s[content])) {
            content++;
        }
        if (content == e || s[content] == '#' || s[first] == '%' ||
            (e - first >= 3 && memcmp(s + first, "...", 3) == 0 && (e == first + 3 || space(s[first + 3])))) {
            cursor = next_line(s, e, end);
            continue;
        }
        size_t indent = first - cursor;
        if (s[first] == '{') {
            size_t close = flow_end(p, first);
            if (close) {
                json_mapping(p, first, close);
                cursor = close;
            } else {
                size_t boundary = block_boundary(s, cursor, end, indent);
                cursor = boundary;
            }
            continue;
        }
        size_t boundary = block_boundary(s, cursor, end, indent);
        decoder d = {.owner = p, .pos = first, .end = boundary};
        if (printable(p, first, boundary)) {
            record(&d, PROPERTY_SYNTAX);
        }
        cursor = boundary;
    }
}
size_t markdown_core_metadata_parse(markdown_core_parser *parser, const unsigned char *source, size_t length) {
    size_t bom = length >= 3 && memcmp(source, "\xef\xbb\xbf", 3) == 0 ? 3 : 0;
    size_t opening = line_end(source, bom, length);
    if (opening != bom + 3 || opening == length || memcmp(source + bom, "---", 3)) {
        return 0;
    }
    size_t start = next_line(source, opening, length), close = start;
    while (close < length) {
        size_t e = line_end(source, close, length);
        if (e == close + 3 && !memcmp(source + close, "---", 3)) {
            break;
        }
        close = next_line(source, e, length);
    }
    if (close == length) {
        return 0;
    }
    properties p = {.parser = parser, .source = source};
    p.metadata = parser->mem->calloc(1, sizeof(*p.metadata));
    if (!p.metadata) {
        parser->oom = true;
        return 0;
    }
    size_t consumed = next_line(source, close + 3, length);
    for (size_t i = 0; i < consumed && !parser->oom;) {
        if (!grow(&p, (void **)&p.lines, &p.line_capacity, p.line_count + 1, sizeof(*p.lines))) {
            break;
        }
        size_t e = line_end(source, i, consumed);
        p.lines[p.line_count++] = (source_line){i};
        i = next_line(source, e, consumed);
    }
    if (!parser->oom) {
        p.metadata->scope = extent(&p, bom, close + 3);
        index_flows(&p, start, close);
        if (!parser->oom) {
            payload(&p, start, close);
        }
    }
    parser->mem->free(p.flows);
    parser->mem->free(p.lines);
    if (parser->oom) {
        markdown_core_metadata_free(parser->mem, p.metadata);
        return 0;
    }
    parser->root->as.document->metadata = p.metadata;
    parser->line_number = (int)p.line_count;
    parser->last_line_length = 3;
    return consumed;
}
