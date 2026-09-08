#include "metadata.h"
#include "parser.h"
#include "node.h"
#include "map.h"
#include "buffer.h"
#include "utf8.h"
#include <string.h>
#include <limits.h>

/* Properties is an ordered, recovering source decoder. A root member owns its
 * indented continuation; a flow member owns its balanced value. A failed member
 * becomes source, never Markdown. No source range is retried as another YAML
 * shape. The only public allocations belong to the completed document value. */
typedef struct {
    size_t start, end;
} source_span;
typedef struct {
    size_t start, content, indent;
} source_line;
typedef struct {
    size_t start, end;
    bool root_context;
} flow_span;
typedef struct binding {
    struct binding *previous, *next;
    markdown_core_string name;
    markdown_core_metadata_value value;
    size_t source_size;
} binding;
typedef struct {
    markdown_core_parser *parser;
    const unsigned char *source;
    source_line *lines;
    size_t line_count, line_capacity;
    markdown_core_metadata *metadata;
    size_t capacity, records, alias_bytes;
    flow_span *flows;
    size_t flow_count, flow_capacity;
    markdown_core_key_index names, anchors;
    binding *bindings;
} properties;
typedef struct {
    properties *owner;
    size_t pos, end, last;
    source_span *comments;
    size_t comment_count, comment_capacity;
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
        markdown_core_metadata_content *item = &metadata->content[i];
        if (item->kind == MARKDOWN_CORE_METADATA_COMMENT) {
            mem->free((void *)item->as.comment.data);
        } else if (item->kind == MARKDOWN_CORE_METADATA_DATA) {
            mem->free((void *)item->as.data.name.data);
            free_value(mem, &item->as.data.value);
        }
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
static bool append(properties *p, markdown_core_metadata_content value) {
    if (!grow(p, (void **)&p->metadata->content, &p->capacity, p->metadata->count + 1, sizeof(*p->metadata->content))) {
        return false;
    }
    p->metadata->content[p->metadata->count++] = value;
    return true;
}
static void comment(properties *p, size_t start, size_t end) {
    /* Line endings separate members. Interior bytes, including indentation and
     * line endings between lines of one failed member, remain exactly authored. */
    while (end > start && newline(p->source[end - 1])) {
        end--;
    }
    bool meaningful = false;
    for (size_t i = start; i < end; i++) {
        if (!space(p->source[i]) && !newline(p->source[i])) {
            meaningful = true;
        }
    }
    if (!meaningful || p->parser->oom) {
        return;
    }
    markdown_core_metadata_content value = {.kind = MARKDOWN_CORE_METADATA_COMMENT};
    value.as.comment = copy(p, p->source + start, end - start);
    if (!value.as.comment.data || !append(p, value)) {
        p->parser->mem->free((void *)value.as.comment.data);
    }
}
static bool remember_comment(decoder *d, size_t start, size_t end) {
    source_line line = d->owner->lines[line_index(d->owner, start)];
    if (line.content == start) {
        start = line.start;
    }
    if (!grow(d->owner, (void **)&d->comments, &d->comment_capacity, d->comment_count + 1, sizeof(*d->comments))) {
        return false;
    }
    d->comments[d->comment_count++] = (source_span){start, end};
    return true;
}
static void skip(decoder *d) {
    const unsigned char *s = d->owner->source;
    while (d->pos < d->end) {
        if (space(s[d->pos]) || newline(s[d->pos])) {
            d->pos++;
        } else if (s[d->pos] == '#') {
            size_t end = line_end(s, d->pos, d->end);
            remember_comment(d, d->pos, end);
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
/* YAML continuation content must be indented beyond its parent. Root flow
 * collections have parent indentation -1. Closing delimiters and comments are
 * separation, not continuation content. */
static bool continuation_indent(properties *p, size_t pos, int indent) {
    source_line line = p->lines[line_index(p, pos)];
    /* Index both the line start and first content byte once. Scanning backward
     * from every flow item would make long single-line collections quadratic.
     * Only spaces count as YAML indentation; tabs can follow that indentation. */
    return line.content < pos || (int)line.indent > indent;
}
static bool quoted(decoder *d, int indent, markdown_core_string *value) {
    const unsigned char *s = d->owner->source;
    unsigned char quote = s[d->pos++];
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(d->owner->parser->mem);
    bool closed = false, valid = true;
    while (d->pos < d->end && valid) {
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
            if (newline(c)) {
                if (c == '\r' && d->pos < d->end && s[d->pos] == '\n') {
                    d->pos++;
                }
                while (d->pos < d->end && space(s[d->pos])) {
                    d->pos++;
                }
                if (d->pos < d->end && !newline(s[d->pos]) && !continuation_indent(d->owner, d->pos, indent)) {
                    valid = false;
                }
                continue;
            }
            const char *escapes = "0abtnvfre \"/\\";
            const unsigned char replacements[] = {0, 7, 8, 9, 10, 11, 12, 13, 27, 32, '"', '/', '\\'};
            const char *found = strchr(escapes, c);
            if (found) {
                markdown_core_strbuf_putc(&buf, replacements[found - escapes]);
            } else if (c == 'N' || c == '_' || c == 'L' || c == 'P') {
                markdown_core_utf8proc_encode_char(c == 'N'   ? 0x85
                                                   : c == '_' ? 0xa0
                                                   : c == 'L' ? 0x2028
                                                              : 0x2029,
                                                   &buf);
            } else if (c == 'x' || c == 'u' || c == 'U') {
                size_t count = c == 'x' ? 2 : c == 'u' ? 4 : 8;
                uint32_t scalar = 0;
                if (count > d->end - d->pos) {
                    valid = false;
                    break;
                }
                for (size_t i = 0; i < count; i++) {
                    unsigned char h = s[d->pos++];
                    int digit = h >= '0' && h <= '9'   ? h - '0'
                                : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                       : -1;
                    if (digit < 0) {
                        valid = false;
                        break;
                    }
                    scalar = (scalar << 4) | (uint32_t)digit;
                }
                if (!valid || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) {
                    valid = false;
                    break;
                }
                markdown_core_utf8proc_encode_char((int32_t)scalar, &buf);
            } else {
                valid = false;
            }
        } else if (newline(c)) {
            if (c == '\r' && d->pos < d->end && s[d->pos] == '\n') {
                d->pos++;
            }
            while (buf.size && space(buf.ptr[buf.size - 1])) {
                markdown_core_strbuf_truncate(&buf, buf.size - 1);
            }
            size_t breaks = 0;
            while (d->pos < d->end) {
                if (space(s[d->pos])) {
                    d->pos++;
                } else if (newline(s[d->pos])) {
                    d->pos = next_line(s, d->pos, d->end);
                    breaks++;
                } else {
                    break;
                }
            }
            if (d->pos < d->end && !continuation_indent(d->owner, d->pos, indent)) {
                valid = false;
                break;
            }
            if (breaks) {
                for (size_t i = 0; i < breaks; i++) {
                    markdown_core_strbuf_putc(&buf, '\n');
                }
            } else {
                markdown_core_strbuf_putc(&buf, ' ');
            }
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
static bool plain(decoder *d, bool flow, bool key, int indent, markdown_core_string *value) {
    const unsigned char *s = d->owner->source;
    size_t start = d->pos;
    if (start == d->end) {
        *value = copy(d->owner, s + start, 0);
        return value->data != NULL;
    }
    if (!plain_start(s, start, d->end)) {
        return false;
    }
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(d->owner->parser->mem);
    size_t last = start;
    bool valid = true;
    while (d->pos < d->end) {
        unsigned char c = s[d->pos];
        bool colon = c == ':' && (d->pos + 1 == d->end || space(s[d->pos + 1]) || newline(s[d->pos + 1]) ||
                                  (flow && strchr(",[]{}", s[d->pos + 1])));
        if ((flow && strchr(",[]{}", c)) || (c == '#' && (d->pos == start || space(s[d->pos - 1]))) || colon) {
            if (colon && !key) {
                valid = false;
            }
            break;
        }
        if (newline(c)) {
            if (key) {
                break;
            }
            size_t p = next_line(s, d->pos, d->end), breaks = 0;
            while (p < d->end) {
                if (space(s[p])) {
                    p++;
                } else if (newline(s[p])) {
                    p = next_line(s, p, d->end);
                    breaks++;
                } else {
                    break;
                }
            }
            if (p == d->end || s[p] == '#' || (flow && strchr(",]}", s[p]))) {
                break;
            }
            if (!continuation_indent(d->owner, p, indent)) {
                valid = false;
                break;
            }
            while (buf.size && space(buf.ptr[buf.size - 1])) {
                markdown_core_strbuf_truncate(&buf, buf.size - 1);
            }
            if (breaks) {
                for (size_t i = 0; i < breaks; i++) {
                    markdown_core_strbuf_putc(&buf, '\n');
                }
            } else {
                markdown_core_strbuf_putc(&buf, ' ');
            }
            d->pos = p;
            continue;
        }
        markdown_core_strbuf_putc(&buf, c);
        d->pos++;
        if (!space(c)) {
            last = d->pos;
        }
    }
    while (buf.size && space(buf.ptr[buf.size - 1])) {
        markdown_core_strbuf_truncate(&buf, buf.size - 1);
    }
    valid = valid && finish_string(d, &buf, value);
    if (buf.oom) {
        d->owner->parser->oom = true;
    }
    markdown_core_strbuf_free(&buf);
    d->last = last;
    return valid;
}
static bool equals(markdown_core_string s, const char *text) {
    size_t n = strlen(text);
    return s.length == n && memcmp(s.data, text, n) == 0;
}
static bool number(markdown_core_string s, bool integer) {
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
    if (!integer && i < s.length && s.data[i] == '.') {
        i++;
        while (i < s.length && s.data[i] >= '0' && s.data[i] <= '9') {
            i++;
        }
    }
    if (!integer && i < s.length && (s.data[i] == 'e' || s.data[i] == 'E')) {
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
static bool clone(properties *p, const markdown_core_metadata_value *source, markdown_core_metadata_value *target) {
    target->kind = source->kind;
    if (source->kind == MARKDOWN_CORE_METADATA_SCALAR) {
        target->as.scalar = source->as.scalar;
        if (source->as.scalar.kind == MARKDOWN_CORE_METADATA_TEXT ||
            source->as.scalar.kind == MARKDOWN_CORE_METADATA_NUMBER) {
            target->as.scalar.value.string =
                copy(p, source->as.scalar.value.string.data, source->as.scalar.value.string.length);
        }
    } else if (source->kind == MARKDOWN_CORE_METADATA_LIST) {
        size_t count = source->as.list.count;
        if (count) {
            target->as.list.items = p->parser->mem->calloc(count, sizeof(*target->as.list.items));
            if (!target->as.list.items) {
                p->parser->oom = true;
                return false;
            }
        }
        for (size_t i = 0; i < count && !p->parser->oom; i++) {
            target->as.list.items[i].kind = source->as.list.items[i].kind;
            target->as.list.items[i].value =
                copy(p, source->as.list.items[i].value.data, source->as.list.items[i].value.length);
            target->as.list.count++;
        }
    } else {
        return false;
    }
    return !p->parser->oom;
}
static bool identifier(decoder *d, markdown_core_string *name) {
    const unsigned char *s = d->owner->source;
    size_t start = ++d->pos;
    while (d->pos < d->end && !space(s[d->pos]) && !newline(s[d->pos]) && !strchr(",[]{}", s[d->pos])) {
        d->pos++;
    }
    if (start == d->pos) {
        return false;
    }
    *name = copy(d->owner, s + start, d->pos - start);
    d->last = d->pos;
    return name->data != NULL;
}
static bool block_scalar(decoder *d, size_t indent, markdown_core_string *value) {
    const unsigned char *s = d->owner->source;
    bool folded = s[d->pos++] == '>';
    int chomp = 0;
    size_t explicit_indent = 0;
    for (int i = 0; i < 2 && d->pos < d->end; i++) {
        unsigned char c = s[d->pos];
        if ((c == '-' || c == '+') && !chomp) {
            chomp = c;
            d->pos++;
        } else if (c >= '1' && c <= '9' && !explicit_indent) {
            explicit_indent = c - '0';
            d->pos++;
        } else {
            break;
        }
    }
    d->last = d->pos;
    while (d->pos < d->end && space(s[d->pos])) {
        d->pos++;
    }
    if (d->pos < d->end && s[d->pos] == '#') {
        size_t e = line_end(s, d->pos, d->end);
        remember_comment(d, d->pos, e);
        d->pos = e;
    }
    if (d->pos < d->end && !newline(s[d->pos])) {
        return false;
    }
    d->pos = next_line(s, d->pos, d->end);
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(d->owner->parser->mem);
    size_t content_indent = explicit_indent ? indent + explicit_indent : 0;
    bool previous_nonempty = false, previous_more = false;
    size_t pending_breaks = 0;
    bool valid = true;
    while (d->pos < d->end) {
        size_t start = d->pos, e = line_end(s, start, d->end), p = start;
        while (p < e && s[p] == ' ') {
            p++;
        }
        bool empty = p == e;
        if (!empty && !content_indent) {
            content_indent = p - start;
        }
        if (!empty && (content_indent <= indent || p - start < content_indent)) {
            valid = false;
            break;
        }
        if (!empty) {
            bool more = p - start > content_indent;
            if (pending_breaks) {
                if (folded && previous_nonempty && !previous_more && !more) {
                    if (pending_breaks == 1) {
                        markdown_core_strbuf_putc(&buf, ' ');
                    } else {
                        for (size_t i = 1; i < pending_breaks; i++) {
                            markdown_core_strbuf_putc(&buf, '\n');
                        }
                    }
                } else {
                    for (size_t i = 0; i < pending_breaks; i++) {
                        markdown_core_strbuf_putc(&buf, '\n');
                    }
                }
            }
            markdown_core_strbuf_put(&buf, s + start + content_indent, (bufsize_t)(e - start - content_indent));
            previous_nonempty = true;
            previous_more = more;
            pending_breaks = 0;
            d->last = e;
            while (d->last > start + content_indent && space(s[d->last - 1])) {
                d->last--;
            }
        }
        if (e < d->end) {
            pending_breaks++;
        }
        d->pos = next_line(s, e, d->end);
    }
    if (chomp != '-' && pending_breaks) {
        size_t count = chomp == '+' ? pending_breaks : 1;
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_putc(&buf, '\n');
        }
    }
    valid = valid && finish_string(d, &buf, value);
    if (buf.oom) {
        d->owner->parser->oom = true;
    }
    markdown_core_strbuf_free(&buf);
    return valid;
}
static bool decode_value(decoder *d, bool flow, bool member, int indent, markdown_core_metadata_value *value);
static bool list_item(decoder *d, bool flow, int indent, markdown_core_metadata_value *list, size_t *capacity) {
    markdown_core_metadata_value value = {0};
    bool valid = decode_value(d, flow, true, indent, &value);
    if (!valid || value.kind != MARKDOWN_CORE_METADATA_SCALAR ||
        (value.as.scalar.kind != MARKDOWN_CORE_METADATA_NUMBER &&
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
static bool sequence(decoder *d, bool flow, int indent, markdown_core_metadata_value *value) {
    const unsigned char *s = d->owner->source;
    value->kind = MARKDOWN_CORE_METADATA_LIST;
    size_t capacity = 0;
    if (flow) {
        d->pos++;
        skip(d);
        if (d->pos < d->end && s[d->pos] == ']') {
            d->last = ++d->pos;
            return true;
        }
        while (d->pos < d->end && !d->owner->parser->oom) {
            if (!list_item(d, true, indent, value, &capacity)) {
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
        bool valid = list_item(d, false, (int)item_indent, value, &capacity);
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
static bool decode_value(decoder *d, bool flow, bool member, int indent, markdown_core_metadata_value *value) {
    properties *p = d->owner;
    const unsigned char *s = p->source;
    skip(d);
    size_t start = d->pos;
    if (flow && start < d->end && !strchr(",]}", s[start]) && !continuation_indent(p, start, indent)) {
        return false;
    }
    markdown_core_string tag = {0};
    binding *anchor = NULL;
    bool valid = true;
    for (int i = 0; i < 2 && d->pos < d->end; i++) {
        if (s[d->pos] == '&' && !anchor) {
            anchor = p->parser->mem->calloc(1, sizeof(*anchor));
            if (!anchor) {
                p->parser->oom = true;
                valid = false;
                goto done;
            }
            anchor->next = p->bindings;
            p->bindings = anchor;
            if (!identifier(d, &anchor->name)) {
                valid = false;
                goto done;
            }
            void *previous = NULL;
            if (!markdown_core_key_index_insert(&p->anchors, anchor->name.data, (bufsize_t)anchor->name.length, anchor,
                                                1, &previous)) {
                p->parser->oom = true;
                valid = false;
                goto done;
            }
            anchor->previous = previous;
            skip(d);
        } else if (s[d->pos] == '!' && !tag.data) {
            if (d->pos + 1 < d->end && s[d->pos + 1] == '<') {
                size_t begin = ++d->pos;
                while (d->pos < d->end && s[d->pos] != '>' && !space(s[d->pos]) && !newline(s[d->pos])) {
                    d->pos++;
                }
                if (d->pos == d->end || s[d->pos] != '>') {
                    valid = false;
                    goto done;
                }
                d->pos++;
                tag = copy(p, s + begin, d->pos - begin);
            } else if (!identifier(d, &tag)) {
                valid = false;
                goto done;
            }
            skip(d);
        } else {
            break;
        }
    }
    if (d->pos < d->end && s[d->pos] == '*') {
        if (anchor || tag.data) {
            valid = false;
            goto done;
        }
        markdown_core_string name = {0};
        if (!identifier(d, &name)) {
            valid = false;
            goto done;
        }
        binding *resolved = markdown_core_key_index_lookup(&p->anchors, name.data, (bufsize_t)name.length);
        p->parser->mem->free((void *)name.data);
        if (!resolved || !resolved->value.kind || resolved->source_size > 1048576 - p->alias_bytes) {
            valid = false;
            goto done;
        }
        p->alias_bytes += resolved->source_size;
        valid = clone(p, &resolved->value, value);
        goto done;
    }
    if (d->pos < d->end &&
        (s[d->pos] == '[' ||
         (!flow && s[d->pos] == '-' && (d->pos + 1 == d->end || space(s[d->pos + 1]) || newline(s[d->pos + 1]))))) {
        if (member || (tag.data && !equals(tag, "!seq") && !equals(tag, "<tag:yaml.org,2002:seq>"))) {
            valid = false;
            goto done;
        }
        valid = sequence(d, s[d->pos] == '[', indent, value);
    } else {
        markdown_core_string text = {0};
        bool quoted_style = d->pos < d->end && (s[d->pos] == '\'' || s[d->pos] == '"');
        bool block_style = d->pos < d->end && (s[d->pos] == '|' || s[d->pos] == '>');
        if (quoted_style) {
            valid = quoted(d, indent, &text);
        } else if (block_style && !flow) {
            valid = block_scalar(d, (size_t)indent, &text);
        } else if (d->pos == d->end || (flow && strchr(",]}", s[d->pos]))) {
            text = copy(p, s + d->pos, 0);
            valid = text.data != NULL;
        } else {
            valid = plain(d, flow, false, indent, &text);
        }
        if (!valid || !single_line(text)) {
            p->parser->mem->free((void *)text.data);
            valid = false;
            goto done;
        }
        value->kind = MARKDOWN_CORE_METADATA_SCALAR;
        markdown_core_metadata_scalar *scalar = &value->as.scalar;
        bool null_value = text.length == 0 || equals(text, "null");
        bool bool_value = equals(text, "true") || equals(text, "false");
        bool number_value = number(text, false);
        if (tag.data) {
            if (equals(tag, "!str") || equals(tag, "<tag:yaml.org,2002:str>")) {
                scalar->kind = MARKDOWN_CORE_METADATA_TEXT;
            } else if (equals(tag, "!null") || equals(tag, "<tag:yaml.org,2002:null>")) {
                scalar->kind = MARKDOWN_CORE_METADATA_NULL;
                valid = null_value;
            } else if (equals(tag, "!bool") || equals(tag, "<tag:yaml.org,2002:bool>")) {
                scalar->kind = MARKDOWN_CORE_METADATA_BOOL;
                valid = bool_value;
            } else if (equals(tag, "!int") || equals(tag, "<tag:yaml.org,2002:int>")) {
                scalar->kind = MARKDOWN_CORE_METADATA_NUMBER;
                valid = number(text, true);
            } else if (equals(tag, "!float") || equals(tag, "<tag:yaml.org,2002:float>")) {
                scalar->kind = MARKDOWN_CORE_METADATA_NUMBER;
                valid = number_value;
            } else {
                valid = false;
            }
        } else if (quoted_style || block_style) {
            scalar->kind = MARKDOWN_CORE_METADATA_TEXT;
        } else {
            scalar->kind = null_value     ? MARKDOWN_CORE_METADATA_NULL
                           : bool_value   ? MARKDOWN_CORE_METADATA_BOOL
                           : number_value ? MARKDOWN_CORE_METADATA_NUMBER
                                          : MARKDOWN_CORE_METADATA_TEXT;
        }
        if (valid && (scalar->kind == MARKDOWN_CORE_METADATA_TEXT || scalar->kind == MARKDOWN_CORE_METADATA_NUMBER)) {
            scalar->value.string = text;
        } else {
            if (valid && scalar->kind == MARKDOWN_CORE_METADATA_BOOL) {
                scalar->value.boolean = equals(text, "true");
            }
            p->parser->mem->free((void *)text.data);
        }
    }
    if (valid && anchor) {
        anchor->source_size = d->last > start ? d->last - start : 0;
        valid = clone(p, value, &anchor->value);
    }
done:
    p->parser->mem->free((void *)tag.data);
    return valid && !p->parser->oom;
}
static void rollback(properties *p, binding *before) {
    for (binding *b = p->bindings; b != before; b = b->next) {
        if (b->name.data && markdown_core_key_index_lookup(&p->anchors, b->name.data, (bufsize_t)b->name.length) == b) {
            if (!markdown_core_key_index_insert(&p->anchors, b->name.data, (bufsize_t)b->name.length, b->previous, 1,
                                                NULL)) {
                p->parser->oom = true;
            }
        }
    }
}
static bool record(decoder *d, bool flow, int indent) {
    properties *p = d->owner;
    const unsigned char *s = p->source;
    size_t start = d->pos, alias_bytes = p->alias_bytes;
    p->parser->metadata_decoded_bytes += d->end - start;
    binding *before = p->bindings;
    markdown_core_metadata_content content = {.kind = MARKDOWN_CORE_METADATA_DATA};
    markdown_core_metadata_record *r = &content.as.data;
    bool quoted_key = d->pos < d->end && (s[d->pos] == '\'' || s[d->pos] == '"');
    bool valid = quoted_key ? quoted(d, indent, &r->name) : plain(d, flow, true, indent, &r->name);
    valid = valid && r->name.length && single_line(r->name) && !memchr(s + start, '\n', d->pos - start) &&
            !memchr(s + start, '\r', d->pos - start);
    while (d->pos < d->end && space(s[d->pos])) {
        d->pos++;
    }
    if (!valid || d->pos == d->end || s[d->pos++] != ':') {
        goto failed;
    }
    if (!flow && d->pos < d->end && !space(s[d->pos]) && !newline(s[d->pos])) {
        goto failed;
    }
    d->last = d->pos;
    if (!decode_value(d, flow, false, indent, &r->value)) {
        goto failed;
    }
    r->scope = extent(p, start, d->last);
    skip(d);
    if (d->pos != d->end && !(flow && (s[d->pos] == ',' || s[d->pos] == '}'))) {
        goto failed;
    }
    if (p->records == 65536 || markdown_core_key_index_lookup(&p->names, r->name.data, (bufsize_t)r->name.length)) {
        goto failed;
    }
    if (!append(p, content)) {
        goto failed;
    }
    if (!markdown_core_key_index_insert(&p->names, r->name.data, (bufsize_t)r->name.length, (void *)r->name.data, 0,
                                        NULL)) {
        p->parser->oom = true;
        return false;
    }
    p->records++;
    for (size_t i = 0; i < d->comment_count; i++) {
        comment(p, d->comments[i].start, d->comments[i].end);
    }
    d->comment_count = 0;
    return true;
failed:
    rollback(p, before);
    p->alias_bytes = alias_bytes;
    p->parser->mem->free((void *)r->name.data);
    free_value(p->parser->mem, &r->value);
    d->comment_count = 0;
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
static void flow_mapping(properties *p, size_t start, size_t end) {
    decoder d = {.owner = p, .pos = start + 1, .end = end};
    skip(&d);
    for (size_t i = 0; i < d.comment_count; i++) {
        comment(p, d.comments[i].start, d.comments[i].end);
    }
    d.comment_count = 0;
    while (d.pos < end && p->source[d.pos] != '}' && !p->parser->oom) {
        size_t item = d.pos;
        size_t boundary = flow_boundary(p->source, item, end);
        d.end = boundary;
        bool valid = printable(p, item, boundary) && record(&d, true, -1);
        if (!valid) {
            comment(p, item, boundary);
        }
        d.end = end;
        d.pos = boundary;
        if (d.pos < end && p->source[d.pos] == ',') {
            d.pos++;
        } else {
            break;
        }
        skip(&d);
        for (size_t i = 0; i < d.comment_count; i++) {
            comment(p, d.comments[i].start, d.comments[i].end);
        }
        d.comment_count = 0;
    }
    if (d.pos < end && p->source[d.pos] == '}') {
        d.pos++;
    }
    if (d.pos < end) {
        comment(p, d.pos, end);
    }
    p->parser->mem->free(d.comments);
}
/* Return the byte after a directly authored block key's colon. The same
 * lexical rule serves recovery and block-scalar headers. Flow punctuation is
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

/* A block scalar's indentation owns its body, including bytes that resemble
 * quotes, comments, or flow delimiters. Recognizing its header here prevents
 * source-boundary scanning from assigning those bytes another lexical role. */
static bool block_scalar_header(const unsigned char *s, size_t start, size_t end) {
    size_t cursor = start;
    while (cursor < end && s[cursor] == ' ') {
        cursor++;
    }
    if (cursor == end) {
        return false;
    }
    if (s[cursor] == '-' && cursor + 1 < end && space(s[cursor + 1])) {
        cursor++;
    } else {
        cursor = block_key_end(s, cursor, end);
        if (!cursor) {
            return false;
        }
    }
    while (cursor < end && space(s[cursor])) {
        cursor++;
    }
    for (int i = 0; i < 2 && cursor < end && (s[cursor] == '&' || s[cursor] == '!'); i++) {
        while (cursor < end && !space(s[cursor])) {
            cursor++;
        }
        while (cursor < end && space(s[cursor])) {
            cursor++;
        }
    }
    if (cursor == end || (s[cursor] != '|' && s[cursor] != '>')) {
        return false;
    }
    cursor++;
    while (cursor < end && (s[cursor] == '+' || s[cursor] == '-' || (s[cursor] >= '1' && s[cursor] <= '9'))) {
        cursor++;
    }
    while (cursor < end && space(s[cursor])) {
        cursor++;
    }
    return cursor == end || s[cursor] == '#';
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
            bool continuation = ((form == VALUE_PREFIX || form == VALUE_SEQUENCE) && list_line) ||
                                (form == VALUE_PREFIX && s[nonspace] == '#') || ((depth || quote) && !recovery_key);
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
                if (c == '&' || c == '!') {
                    while (i + 1 < e && !space(s[i + 1])) {
                        i++;
                    }
                    continue;
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
    size_t line_start = start, root_opening = start;
    for (size_t i = start; i < end && !p->parser->oom; i++) {
        if (i == line_start) {
            size_t e = line_end(s, i, end);
            root_opening = i;
            while (root_opening < e && space(s[root_opening])) {
                root_opening++;
            }
            if (e - root_opening >= 5 && !memcmp(s + root_opening, "!!map", 5)) {
                root_opening += 5;
            } else if (e - root_opening >= 24 && !memcmp(s + root_opening, "!<tag:yaml.org,2002:map>", 24)) {
                root_opening += 24;
            }
            while (root_opening < e && space(s[root_opening])) {
                root_opening++;
            }
        }
        if (i == line_start && !quote) {
            size_t e = line_end(s, i, end);
            if (block_scalar_header(s, i, e)) {
                size_t indentation = i;
                while (indentation < e && s[indentation] == ' ') {
                    indentation++;
                }
                indentation -= i;
                size_t next = next_line(s, e, end);
                while (next < end) {
                    size_t last = line_end(s, next, end), first = next;
                    while (first < last && s[first] == ' ') {
                        first++;
                    }
                    if (first < last && first - next <= indentation) {
                        break;
                    }
                    next = next_line(s, last, end);
                }
                line_start = next;
                i = next - 1;
                continue;
            }
        }
        if (i == line_start && quote) {
            size_t e = line_end(s, i, end);
            if (!(count && p->flows[stack[count - 1]].root_context) && (block_key_end(s, i, e) || s[i] == '{')) {
                quote = 0;
            }
        }
        unsigned char c = s[i];
        if (newline(c)) {
            if (c == '\r' && i + 1 < end && s[i + 1] == '\n') {
                i++;
            }
            line_start = i + 1;
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
                line_start = next_line(s, i, end);
                i = line_start - 1;
            }
        } else if (c == '[' || c == '{') {
            if (!grow(p, (void **)&p->flows, &p->flow_capacity, p->flow_count + 1, sizeof(*p->flows)) ||
                !grow(p, (void **)&stack, &capacity, count + 1, sizeof(*stack))) {
                break;
            }
            bool root_context = (count && p->flows[stack[count - 1]].root_context) || (c == '{' && i == root_opening);
            stack[count++] = p->flow_count;
            p->flows[p->flow_count++] = (flow_span){i, 0, root_context};
        } else if ((c == ']' || c == '}') && count) {
            flow_span *open = &p->flows[stack[count - 1]];
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
            comment(p, cursor, e);
            cursor = next_line(s, e, end);
            continue;
        }
        size_t indent = first - cursor;
        if ((e - first >= 5 && !memcmp(s + first, "!!map", 5) && (e == first + 5 || space(s[first + 5]))) ||
            (e - first >= 24 && !memcmp(s + first, "!<tag:yaml.org,2002:map>", 24) &&
             (e == first + 24 || space(s[first + 24])))) {
            first += s[first + 1] == '!' ? 5 : 24;
            while (first < e && space(s[first])) {
                first++;
            }
            if (first == e) {
                cursor = next_line(s, e, end);
                continue;
            }
        }
        if (s[first] == '{') {
            size_t close = flow_end(p, first);
            if (close) {
                flow_mapping(p, first, close);
                cursor = close;
            } else {
                size_t boundary = block_boundary(s, cursor, end, indent);
                comment(p, cursor, boundary);
                cursor = boundary;
            }
            continue;
        }
        size_t boundary = block_boundary(s, cursor, end, indent);
        decoder d = {.owner = p, .pos = first, .end = boundary};
        if (!printable(p, first, boundary) || !record(&d, false, (int)indent)) {
            comment(p, cursor, boundary);
        }
        p->parser->mem->free(d.comments);
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
        size_t e = line_end(source, i, consumed), content = i;
        while (content < e && source[content] == ' ') {
            content++;
        }
        size_t indent = content - i;
        while (content < e && space(source[content])) {
            content++;
        }
        p.lines[p.line_count++] = (source_line){i, content, indent};
        i = next_line(source, e, consumed);
    }
    if (!markdown_core_key_index_init(&p.names, parser->mem, 0) ||
        !markdown_core_key_index_init(&p.anchors, parser->mem, 0)) {
        parser->oom = true;
    }
    if (!parser->oom) {
        p.metadata->scope = extent(&p, bom, close + 3);
        index_flows(&p, start, close);
        if (!parser->oom) {
            payload(&p, start, close);
        }
    }
    markdown_core_key_index_free(&p.names);
    markdown_core_key_index_free(&p.anchors);
    while (p.bindings) {
        binding *b = p.bindings;
        p.bindings = b->next;
        parser->mem->free((void *)b->name.data);
        free_value(parser->mem, &b->value);
        parser->mem->free(b);
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
