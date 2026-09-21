#include "alloc.h"
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
/* Frontmatter borrows the active input's physical-line index. */
typedef markdown_core_input_line source_line;
typedef struct {
    markdown_core_parser *parser;
    const unsigned char *source;
    markdown_core_metadata_fields *metadata;
    size_t first_line;
    size_t count;
    /* One-token lookahead: the boundary line is the next member's line.
     * Its lexical classification belongs to this decoder, not to the core's
     * physical-line geometry. */
    size_t classified_line, first, key;
    bool classified;
} properties;
typedef struct {
    properties *owner;
    size_t pos, end;
    /* The index of a line at or before `pos`; decoder_line() catches it up. */
    size_t line;
} decoder;

/* Decode a pre-indexed envelope. Return geometry by value so a later parser
 * operation cannot invalidate a decoder's line reference. */
static source_line property_line(const properties *p, size_t line) {
    return p->parser->input_lines[p->first_line + line];
}

static bool space(unsigned char c) { return c == ' ' || c == '\t'; }
static bool newline(unsigned char c) { return c == '\r' || c == '\n'; }
static bool plain_start(const unsigned char *s, size_t start, size_t end) {
    if (start == end || space(s[start]) || strchr(",[]{}#&*!|>'\"%@`", s[start])) {
        return false;
    }
    return !((s[start] == '?' || s[start] == ':') &&
             (start + 1 == end || space(s[start + 1]) || newline(s[start + 1])));
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
/* The line holding the decoder's cursor. The cursor only moves forward, so
 * the line index catches up to it through the index and never reads a byte.
 * Past the last member line it rests on the closing fence. */
static size_t decoder_line(decoder *d) {
    const properties *p = d->owner;
    while (d->line < p->count && property_line(p, d->line + 1).start <= d->pos) {
        d->line++;
    }
    return d->line;
}
static markdown_core_string copy(properties *p, const unsigned char *s, size_t size) {
    unsigned char *data = markdown_core_alloc(size + 1, 1);
    if (!data) {
        markdown_core_parser_fail(p->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return (markdown_core_string){0};
    }
    if (size) {
        memcpy(data, s, size);
    }
    return (markdown_core_string){data, size};
}
static void free_value(markdown_core_metadata_value *v) {
    if (v->kind == MARKDOWN_CORE_METADATA_SCALAR &&
        (v->as.scalar.kind == MARKDOWN_CORE_METADATA_NUMBER || v->as.scalar.kind == MARKDOWN_CORE_METADATA_TEXT)) {
        markdown_core_free((void *)v->as.scalar.value.string.data);
    } else if (v->kind == MARKDOWN_CORE_METADATA_LIST) {
        for (size_t i = 0; i < v->as.list.count; i++) {
            markdown_core_free((void *)v->as.list.items[i].value.data);
        }
        markdown_core_free(v->as.list.items);
    }
    memset(v, 0, sizeof(*v));
}
void markdown_core_metadata_fields_free(markdown_core_metadata_fields *metadata) {
    if (!metadata) {
        return;
    }
    free_value(&metadata->name);
    free_value(&metadata->title);
    free_value(&metadata->subtitle);
    free_value(&metadata->time);
    free_value(&metadata->date);
    free_value(&metadata->authors);
    free_value(&metadata->keywords);
    free_value(&metadata->abstract);
    free_value(&metadata->state);
    free_value(&metadata->comment);
}
static void skip(decoder *d) {
    const unsigned char *s = d->owner->source;
    while (d->pos < d->end) {
        if (space(s[d->pos]) || newline(s[d->pos])) {
            d->pos++;
        } else if (s[d->pos] == '#' && (d->pos == 0 || space(s[d->pos - 1]) || newline(s[d->pos - 1]))) {
            d->pos = property_line(d->owner, decoder_line(d)).end;
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
        int n = markdown_core_utf8proc_step(p->source + i, (bufsize_t)(end - i), &c);
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
        markdown_core_parser_fail(d->owner->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();
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
        markdown_core_parser_fail(d->owner->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_strbuf_free(&buf);
    return valid;
}
static bool plain(decoder *d, bool delimited, markdown_core_string *value) {
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
            return false;
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
static markdown_core_metadata_value *field_slot(markdown_core_metadata_fields *metadata, markdown_core_string name) {
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
    size_t key_line = decoder_line(d);
    size_t key_indent = key_start - property_line(d->owner, key_line).start;
    d->pos++;
    if (d->pos < d->end && !space(s[d->pos]) && !newline(s[d->pos])) {
        return false;
    }
    while (d->pos < d->end && space(s[d->pos])) {
        d->pos++;
    }
    if (d->pos < d->end && s[d->pos] == '#') {
        d->pos = property_line(d->owner, key_line).end;
    }
    if (d->pos < d->end && !newline(s[d->pos])) {
        return false;
    }
    markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT();
    size_t indent = 0, clipped = 0, line = key_line + 1;
    bool valid = true;
    for (; property_line(d->owner, line).start < d->end; line++) {
        size_t start = property_line(d->owner, line).start, end = property_line(d->owner, line).end, content = start;
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
                indent = content - start;
            }
            if (indent <= key_indent || content - start < indent) {
                valid = false;
                break;
            }
            markdown_core_strbuf_put(&text, s + start + indent, (bufsize_t)(end - start - indent));
            markdown_core_strbuf_putc(&text, '\n');
            clipped = (size_t)text.size;
        }
    }
    d->pos = property_line(d->owner, line).start;
    value->kind = MARKDOWN_CORE_METADATA_SCALAR;
    value->as.scalar.kind = MARKDOWN_CORE_METADATA_TEXT;
    markdown_core_strbuf_truncate(&text, (bufsize_t)clipped);
    valid = valid && finish_string(d, &text, &value->as.scalar.value.string);
    if (text.oom) {
        markdown_core_parser_fail(d->owner->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
static bool scalar(decoder *d, scalar_context structure, markdown_core_metadata_value *value) {
    const unsigned char *s = d->owner->source;
    markdown_core_string text = {0};
    bool quoted_style = d->pos < d->end && (s[d->pos] == '"' || s[d->pos] == '\'');
    bool valid = quoted_style ? quoted(d, &text) : plain(d, structure != PROPERTY_SCALAR, &text);
    if (!valid || (!quoted_style && !text.length) || !single_line(text)) {
        markdown_core_free((void *)text.data);
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
        markdown_core_free((void *)text.data);
    }
    return valid && !d->owner->parser->error;
}
static bool list_item(decoder *d, scalar_context structure, markdown_core_metadata_value *list, size_t *capacity) {
    markdown_core_metadata_value value = {0};
    bool valid = scalar(d, structure, &value);
    if (!valid || (value.as.scalar.kind != MARKDOWN_CORE_METADATA_NUMBER &&
                   value.as.scalar.kind != MARKDOWN_CORE_METADATA_TEXT)) {
        free_value(&value);
        return false;
    }
    void *items =
        markdown_core_reserve(list->as.list.items, capacity, list->as.list.count + 1, sizeof(*list->as.list.items));
    if (!items) {
        markdown_core_parser_fail(d->owner->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        free_value(&value);
        return false;
    }
    list->as.list.items = items;
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
        while (d->pos < d->end && !d->owner->parser->error) {
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
    while (d->pos < outer_end && !d->owner->parser->error) {
        size_t start = d->pos;
        if (s[start] != '-' || (start + 1 < outer_end && !space(s[start + 1]) && !newline(s[start + 1]))) {
            return false;
        }
        size_t line = decoder_line(d);
        size_t item_indent = start - property_line(d->owner, line).start;
        d->pos++;
        while (d->pos < outer_end && space(s[d->pos])) {
            d->pos++;
        }
        for (line++; property_line(d->owner, line).start < outer_end; line++) {
            size_t p = property_line(d->owner, line).start, end = property_line(d->owner, line).end;
            while (p < end && space(s[p])) {
                p++;
            }
            if (p < end && s[p] != '#' && p - property_line(d->owner, line).start <= item_indent) {
                break;
            }
        }
        size_t e = property_line(d->owner, line).start;
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
static bool field(decoder *d, size_t key_end) {
    properties *p = d->owner;
    const unsigned char *s = p->source;
    size_t start = d->pos, key_line = decoder_line(d);
    /* The key must close on the line it opened: its source ends at or before
     * that line's end. A quoted key can run across lines; the decoded text is
     * checked separately since escapes can put a newline into it. */
    size_t value_line_end = property_line(p, key_line).end;
    markdown_core_string name = {0};
    markdown_core_metadata_value value = {0};
    bool quoted_key = d->pos < d->end && (s[d->pos] == '\'' || s[d->pos] == '"');
    if (!key_end) {
        return false;
    }
    bool valid = true;
    if (quoted_key) {
        valid = quoted(d, &name);
    } else {
        size_t end = key_end - 1;
        while (end > start && space(s[end - 1])) {
            end--;
        }
        name = (markdown_core_string){s + start, end - start};
        d->pos = key_end - 1;
    }
    valid = valid && name.length && single_line(name) && d->pos <= value_line_end;
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
    if (!printable(p, start, d->end)) {
        goto failed;
    }
    p->parser->metadata_decoded_bytes += d->end - start;
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
    if (quoted_key) {
        markdown_core_free((void *)name.data);
    }
    return true;
failed:
    if (quoted_key) {
        markdown_core_free((void *)name.data);
    }
    free_value(&value);
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

/* A boundary probe leaves its classification for the member it found. All
 * callers progress in line order, so no per-member allocation or rescan is
 * needed, including when a malformed quoted value recovers at a later key. */
static void classify_line(properties *p, size_t line) {
    if (p->classified && p->classified_line == line) {
        return;
    }
    p->classified = true;
    p->classified_line = line;
    size_t first = property_line(p, line).start, end = property_line(p, line).end;
    while (first < end && p->source[first] == ' ') {
        first++;
    }
    p->first = first;
    p->key = block_key_end(p->source, first, end);
    p->parser->metadata_key_work += end - property_line(p, line).start;
}

/* Return the index of the first line after `line` that the member starting
 * there does not own, or `count` when it runs to the closing fence. */
static size_t block_boundary(properties *p, size_t line, size_t indent) {
    const unsigned char *s = p->source;
    enum { VALUE_PREFIX, VALUE_SCALAR, VALUE_QUOTED, VALUE_SEQUENCE, VALUE_BRACKETED } form = VALUE_PREFIX;
    size_t array_depth = 0, object_depth = 0;
    unsigned char quote = 0;
    bool first = true;
    for (; line < p->count; line++) {
        classify_line(p, line);
        size_t cursor = property_line(p, line).start, e = property_line(p, line).end, nonspace = p->first;
        if (!first && nonspace < e && nonspace - cursor <= indent) {
            bool list_line =
                nonspace - cursor == indent && s[nonspace] == '-' && (nonspace + 1 == e || space(s[nonspace + 1]));
            bool recovery_key = p->key != 0;
            size_t content = nonspace;
            while (content < e && space(s[content])) {
                content++;
            }
            bool separation = content == e || s[content] == '#';
            bool continuation = content == e ||
                                ((form == VALUE_PREFIX || form == VALUE_SEQUENCE) && (list_line || separation)) ||
                                array_depth != 0 || object_depth != 0 || (quote && !recovery_key);
            if (!continuation) {
                return line;
            }
        }
        size_t token = first ? p->key : 0;
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
    }
    return line;
}

static void payload(properties *p) {
    const unsigned char *s = p->source;
    for (size_t line = 0; line < p->count && !p->parser->error;) {
        classify_line(p, line);
        size_t first = p->first, key = p->key, e = property_line(p, line).end;
        size_t content = first;
        while (content < e && space(s[content])) {
            content++;
        }
        if (content == e || s[content] == '#' || s[first] == '%' ||
            (e - first >= 3 && memcmp(s + first, "...", 3) == 0 && (e == first + 3 || space(s[first + 3])))) {
            line++;
            continue;
        }
        size_t indent = first - property_line(p, line).start;
        size_t boundary = block_boundary(p, line, indent);
        decoder d = {.owner = p, .pos = first, .end = property_line(p, boundary).start, .line = line};
        field(&d, key);
        line = boundary;
    }
}
size_t markdown_core_properties_parse(markdown_core_parser *parser, const unsigned char *source, size_t length) {
    size_t bom = length >= 3 && memcmp(source, "\xef\xbb\xbf", 3) == 0 ? 3 : 0;
    /* The opener is exactly "---" and a line ending: a peek, not a scan. */
    if (length < bom + 4 || memcmp(source + bom, "---", 3) || !newline(source[bom + 3])) {
        return 0;
    }
    size_t start = next_line(source, bom + 3, length), close = start;
    properties p = {.parser = parser, .source = source};
    /* THE FENCE NEEDS NO LINE GEOMETRY. It is "---" bracketed by line ends,
     * so the search for it is one `memchr` pass over the dashes, checked at
     * each hit for the line start before it and the line end after it, and
     * it allocates nothing: a document that opens with a thematic break and
     * never closes an envelope -- ordinary Markdown -- costs one pass and
     * one byte of state. The shared input index is extended through the
     * closing fence afterwards; the block driver later reuses those entries. */
    bool closed = false;
    size_t fence_work = 0;
    while (close < length) {
        const unsigned char *hit = memchr(source + close, '-', length - close);
        if (!hit) {
            fence_work += length - close;
            break;
        }
        size_t at = (size_t)(hit - source);
        fence_work += at - close + 1;
        if (newline(source[at - 1]) && length - at >= 3 && source[at + 1] == '-' && source[at + 2] == '-' &&
            (at + 3 == length || newline(source[at + 3]))) {
            close = at;
            closed = true;
            break;
        }
        close = at + 1;
    }
    parser->properties_line_work += fence_work;
    if (!closed) {
        return 0;
    }
    int number = 1;
    source_line *line;
    do {
        line = markdown_core_parser_source_line(parser, number++);
    } while (line && line->start < close);
    if (!line) {
        return 0;
    }
    p.count = (size_t)number - 3;
    p.first_line = 1;
    markdown_core_node *node = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_METADATA);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return 0;
    }
    size_t consumed = next_line(source, close + 3, length);
    p.metadata = node->as.metadata;
    node->start_line = 1;
    node->start_column = (int)(bom + 1);
    node->end_line = (int)(p.count + 2);
    node->end_column = 3;
    payload(&p);
    if (parser->error) {
        markdown_core_parser_release_node(parser, node);
        return 0;
    }
    parser->root->as.document->metadata = node;
    parser->line_number = (int)(p.count + 2);
    parser->last_line_length = 3;
    return consumed;
}
