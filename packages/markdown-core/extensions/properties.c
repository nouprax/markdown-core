#include "properties.h"
#include "metadata.h"
#include "parser.h"
#include "node.h"
#include "buffer.h"
#include "utf8.h"
#include <string.h>
#include <limits.h>

/* Properties is an ordered, recovering source decoder. A field owns its
 * indented continuation and bracketed value. Unsupported
 * members are skipped without changing the envelope or interpreting their
 * interiors. No source range is retried. Public allocations belong to the
 * completed document value. */
/* Plain scalars in bracketed arrays stop at array separators; field-line
 * scalars keep that punctuation as text. Quoted strings share one decoder. */
typedef enum { PROPERTY_SCALAR, ARRAY_SCALAR } scalar_context;
typedef struct {
    markdown_core_parser *parser;
    const unsigned char *source;
    markdown_core_metadata *metadata;
} properties;
typedef struct {
    properties *owner;
    size_t pos, end;
} decoder;

static bool space(unsigned char c) { return c == ' ' || c == '\t'; }
static bool newline(unsigned char c) { return c == '\r' || c == '\n'; }
static bool plain_start(const unsigned char *s, size_t start, size_t end) {
    if (start == end || space(s[start]) || strchr(",[]{}#&*!|>'\"%@`", s[start])) {
        return false;
    }
    return !((s[start] == '?' || s[start] == ':') &&
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
    free_value(mem, &metadata->name);
    free_value(mem, &metadata->title);
    free_value(mem, &metadata->subtitle);
    free_value(mem, &metadata->time);
    free_value(mem, &metadata->date);
    free_value(mem, &metadata->authors);
    free_value(mem, &metadata->keywords);
    free_value(mem, &metadata->abstract);
    free_value(mem, &metadata->state);
    free_value(mem, &metadata->comment);
    mem->free(metadata);
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
static bool quoted(decoder *d, markdown_core_string *value) {
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
        } else if (c < 0x20 && c != '\t') {
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
    return value->data != NULL;
}
static bool equals(markdown_core_string s, const char *text) {
    size_t n = strlen(text);
    return s.length == n && memcmp(s.data, text, n) == 0;
}
/* Resolve the source name directly to its optional destination field. */
static markdown_core_metadata_value *field_slot(markdown_core_metadata *metadata, markdown_core_string name) {
    if (equals(name, "name")) {
        return &metadata->name;
    }
    if (equals(name, "title")) {
        return &metadata->title;
    }
    if (equals(name, "subtitle")) {
        return &metadata->subtitle;
    }
    if (equals(name, "time")) {
        return &metadata->time;
    }
    if (equals(name, "date")) {
        return &metadata->date;
    }
    if (equals(name, "authors")) {
        return &metadata->authors;
    }
    if (equals(name, "keywords")) {
        return &metadata->keywords;
    }
    if (equals(name, "abstract")) {
        return &metadata->abstract;
    }
    if (equals(name, "state")) {
        return &metadata->state;
    }
    if (equals(name, "comment")) {
        return &metadata->comment;
    }
    return NULL;
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
    d->pos++;
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
static bool number(markdown_core_string s) {
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
static bool scalar(decoder *d, scalar_context syntax, markdown_core_metadata_value *value) {
    const unsigned char *s = d->owner->source;
    markdown_core_string text = {0};
    bool quoted_style = d->pos < d->end && (s[d->pos] == '"' || s[d->pos] == '\'');
    bool valid = quoted_style ? quoted(d, &text) : plain(d, syntax != PROPERTY_SCALAR, false, &text);
    if (!valid || (!quoted_style && !text.length) || !single_line(text)) {
        d->owner->parser->mem->free((void *)text.data);
        return false;
    }
    markdown_core_metadata_scalar *result = &value->as.scalar;
    value->kind = MARKDOWN_CORE_METADATA_SCALAR;
    result->kind = quoted_style                                    ? MARKDOWN_CORE_METADATA_TEXT
                   : equals(text, "null")                          ? MARKDOWN_CORE_METADATA_NULL
                   : equals(text, "true") || equals(text, "false") ? MARKDOWN_CORE_METADATA_BOOL
                   : number(text)                                  ? MARKDOWN_CORE_METADATA_NUMBER
                                                                   : MARKDOWN_CORE_METADATA_TEXT;
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
static bool list_item(decoder *d, scalar_context syntax, markdown_core_metadata_value *list, size_t *capacity) {
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
static bool sequence(decoder *d, markdown_core_metadata_value *value) {
    const unsigned char *s = d->owner->source;
    value->kind = MARKDOWN_CORE_METADATA_LIST;
    size_t capacity = 0;
    if (s[d->pos] == '[') {
        d->pos++;
        skip(d);
        if (d->pos < d->end && s[d->pos] == ']') {
            d->pos++;
            return true;
        }
        while (d->pos < d->end && !d->owner->parser->oom) {
            if (!list_item(d, ARRAY_SCALAR, value, &capacity)) {
                return false;
            }
            skip(d);
            if (d->pos == d->end) {
                return false;
            }
            if (s[d->pos] == ']') {
                d->pos++;
                return true;
            }
            if (s[d->pos++] != ',') {
                return false;
            }
            skip(d);
            if (d->pos < d->end && s[d->pos] == ']') {
                d->pos++;
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
        bool valid = list_item(d, PROPERTY_SCALAR, value, &capacity);
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
static bool field(decoder *d) {
    properties *p = d->owner;
    const unsigned char *s = p->source;
    size_t start = d->pos;
    p->parser->metadata_decoded_bytes += d->end - start;
    markdown_core_string name = {0};
    markdown_core_metadata_value value = {0};
    bool quoted_key = d->pos < d->end && (s[d->pos] == '\'' || s[d->pos] == '"');
    bool valid = quoted_key ? quoted(d, &name) : plain(d, false, true, &name);
    valid = valid && name.length && single_line(name) && !memchr(s + start, '\n', d->pos - start) &&
            !memchr(s + start, '\r', d->pos - start);
    while (d->pos < d->end && space(s[d->pos])) {
        d->pos++;
    }
    markdown_core_metadata_value *slot = field_slot(p->metadata, name);
    if (!valid || !slot || slot->kind || d->pos == d->end || s[d->pos++] != ':') {
        goto failed;
    }
    if (d->pos < d->end && !space(s[d->pos]) && !newline(s[d->pos])) {
        goto failed;
    }
    size_t value_line_end = line_end(s, d->pos, d->end);
    skip(d);
    if (d->pos == d->end) {
        value.kind = MARKDOWN_CORE_METADATA_SCALAR;
        value.as.scalar.kind = MARKDOWN_CORE_METADATA_NULL;
    } else if (d->pos < value_line_end && s[d->pos] == '|' && (equals(name, "abstract") || equals(name, "comment"))) {
        if (!literal(d, start, &value)) {
            goto failed;
        }
    } else if (d->pos < d->end && (s[d->pos] == '[' || (d->pos > value_line_end && s[d->pos] == '-' &&
                                                        d->pos + 1 < d->end && space(s[d->pos + 1])))) {
        if (!sequence(d, &value)) {
            goto failed;
        }
    } else {
        if (d->pos > value_line_end || !scalar(d, PROPERTY_SCALAR, &value)) {
            goto failed;
        }
    }
    skip(d);
    if (d->pos != d->end) {
        goto failed;
    }
    *slot = value;
    p->parser->mem->free((void *)name.data);
    return true;
failed:
    p->parser->mem->free((void *)name.data);
    free_value(p->parser->mem, &value);
    return false;
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
    enum { VALUE_PREFIX, VALUE_SCALAR, VALUE_QUOTED, VALUE_SEQUENCE, VALUE_BRACKETED } form = VALUE_PREFIX;
    size_t cursor = start, array_depth = 0, object_depth = 0;
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
            bool recovery_key = block_key_end(s, nonspace, e) != 0;
            size_t content = nonspace;
            while (content < e && space(s[content])) {
                content++;
            }
            bool separation = content == e || s[content] == '#';
            bool continuation = content == e ||
                                ((form == VALUE_PREFIX || form == VALUE_SEQUENCE) && (list_line || separation)) ||
                                array_depth != 0 || object_depth != 0 || (quote && !recovery_key);
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
                /* Only the value's first token can open a bracketed collection.
                 * Brackets and quotes within block plain scalars, including
                 * block sequence items, never extend the root member. */
                form = c == '[' || c == '{'                                    ? VALUE_BRACKETED
                       : c == '\'' || c == '"'                                 ? VALUE_QUOTED
                       : !first && c == '-' && (i + 1 == e || space(s[i + 1])) ? VALUE_SEQUENCE
                                                                               : VALUE_SCALAR;
            }
            if (form != VALUE_BRACKETED && form != VALUE_QUOTED) {
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
            } else if (c == '[') {
                array_depth++;
            } else if (c == '{') {
                object_depth++;
            } else if (c == ']' || c == '}') {
                /* A mismatched closer cannot release an unfinished member.
                 * These counters only bound ownership; nested values are not decoded. */
                size_t *depth = c == ']' ? &array_depth : &object_depth;
                if (*depth) {
                    --*depth;
                }
                if (!array_depth && !object_depth) {
                    form = VALUE_SCALAR;
                }
            }
        }
        first = false;
        cursor = next_line(s, e, end);
    }
    return cursor;
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
        size_t boundary = block_boundary(s, cursor, end, indent);
        decoder d = {.owner = p, .pos = first, .end = boundary};
        if (printable(p, first, boundary)) {
            field(&d);
        }
        cursor = boundary;
    }
}
size_t markdown_core_properties_parse(markdown_core_parser *parser, const unsigned char *source, size_t length) {
    size_t bom = length >= 3 && memcmp(source, "\xef\xbb\xbf", 3) == 0 ? 3 : 0;
    size_t opening = line_end(source, bom, length);
    if (opening != bom + 3 || opening == length || memcmp(source + bom, "---", 3)) {
        return 0;
    }
    size_t start = next_line(source, opening, length), close = start;
    size_t closing_line = 2;
    while (close < length) {
        size_t e = line_end(source, close, length);
        if (e == close + 3 && !memcmp(source + close, "---", 3)) {
            break;
        }
        close = next_line(source, e, length);
        closing_line++;
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
    p.metadata->scope = (markdown_core_scope){{1, (int32_t)(bom + 1)}, {(int32_t)closing_line, 3}};
    payload(&p, start, close);
    if (parser->oom) {
        markdown_core_metadata_free(parser->mem, p.metadata);
        return 0;
    }
    parser->root->as.document->metadata = p.metadata;
    parser->line_number = (int)closing_line;
    parser->last_line_length = 3;
    return consumed;
}
