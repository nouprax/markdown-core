#include "directive.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <markdown-core.h>
#include <inlines.h>
#include <map.h>
#include <node.h>
#include <parser.h>
#include <utf8.h>

#include "ext_scanners.h"
#include "extension.h"

#define DIRECTIVE_LABEL_DELIM 8

typedef struct directive_attribute {
    markdown_core_chunk name;
    markdown_core_chunk value;
    size_t index;
    int active;
    struct directive_attribute *next;
} directive_attribute;

typedef struct {
    markdown_core_chunk name;
    directive_attribute *attributes;
    markdown_core_chunk attributes_json;
    int fence_length;
    int closed;
    int consume_line;
    int has_label;
    int has_attributes;
} node_directive;

typedef struct {
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t label_start;
    bufsize_t label_len;
    int has_label;
    int has_attributes;
    directive_attribute *attributes;
    bufsize_t end;
    /* Set when attribute parsing failed from allocation loss rather than
     * invalid syntax; the caller flags the parser instead of silently
     * treating the directive as plain text. */
    int oom;
} parsed_directive;

static int is_directive_node(markdown_core_node *node) {
    return node && (node->type == MARKDOWN_CORE_NODE_DIRECTIVE || node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK);
}

static node_directive *get_directive(markdown_core_node *node) {
    if (!is_directive_node(node)) {
        return NULL;
    }

    return (node_directive *)node->as.opaque;
}

static int directive_enabled(markdown_core_parser *parser) { return parser->options & MARKDOWN_CORE_OPT_DIRECTIVE; }

static int ascii_is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int ascii_is_line_space(unsigned char c) { return c == ' ' || c == '\t'; }

static int is_attr_name_char(unsigned char c);

static int is_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    return pos >= len || data[pos] == '\n' || data[pos] == '\r';
}

static int has_only_spaces_until_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    while (pos < len && ascii_is_line_space(data[pos])) {
        pos++;
    }

    return is_line_end(data, len, pos);
}

static int scan_name(unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *name_start, bufsize_t *name_len) {
    bufsize_t match_len = scan_directive_name(data, len, pos);
    if (match_len == 0) {
        return 0;
    }

    if (data[pos + match_len - 1] == '-' || data[pos + match_len - 1] == '_') {
        return 0;
    }

    *name_start = pos;
    *name_len = match_len;
    return 1;
}

static int scan_label(const unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *label_start,
                      bufsize_t *label_len, bufsize_t *end) {
    int depth = 1;
    bufsize_t i;

    if (pos >= len || data[pos] != '[') {
        return 0;
    }

    i = pos + 1;
    while (i < len) {
        if (data[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }

        if (data[i] == '[') {
            depth++;
            i++;
            continue;
        }

        if (data[i] == ']') {
            depth--;
            if (depth == 0) {
                *label_start = pos + 1;
                *label_len = i - (pos + 1);
                *end = i + 1;
                return 1;
            }
        }

        i++;
    }

    return 0;
}

static int scan_attributes_raw(const unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *attr_start,
                               bufsize_t *attr_len, bufsize_t *end) {
    unsigned char quote = 0;
    bufsize_t i;

    if (pos >= len || data[pos] != '{') {
        return 0;
    }

    i = pos + 1;
    while (i < len) {
        if (quote) {
            if (data[i] == quote) {
                quote = 0;
            }
            i++;
            continue;
        }

        if (data[i] == '"' || data[i] == '\'') {
            quote = data[i++];
            continue;
        }

        if (data[i] == '}') {
            *attr_start = pos + 1;
            *attr_len = i - (pos + 1);
            *end = i + 1;
            return 1;
        }

        i++;
    }
    return 0;
}

static void clear_attribute_caches(markdown_core_node *node, node_directive *directive) {
    markdown_core_chunk_free(markdown_core_node_mem(node), &directive->attributes_json);
}

static int set_chunk_bytes(markdown_core_mem *mem, markdown_core_chunk *chunk, const unsigned char *data,
                           bufsize_t len) {
    markdown_core_chunk_free(mem, chunk);
    chunk->data = (unsigned char *)data;
    chunk->len = len;
    chunk->alloc = 0;
    if (!markdown_core_chunk_to_cstr(mem, chunk)) {
        /* Never keep borrowing the transient line buffer. */
        chunk->data = NULL;
        chunk->len = 0;
        return 0;
    }
    return 1;
}

static int replace_chunk_bytes(markdown_core_mem *mem, markdown_core_chunk *chunk, const unsigned char *data,
                               bufsize_t len) {
    unsigned char *copy = (unsigned char *)mem->calloc((size_t)len + 1, 1);
    if (!copy) {
        return 0;
    }
    if (len > 0) {
        memcpy(copy, data, (size_t)len);
    }
    markdown_core_chunk_free(mem, chunk);
    chunk->data = copy;
    chunk->len = len;
    chunk->alloc = 1;
    return 1;
}

static void free_attribute_list(markdown_core_mem *mem, directive_attribute *attr) {
    while (attr) {
        directive_attribute *next = attr->next;
        markdown_core_chunk_free(mem, &attr->name);
        markdown_core_chunk_free(mem, &attr->value);
        mem->free(attr);
        attr = next;
    }
}

static int attribute_name_is_valid(const unsigned char *name, bufsize_t name_len) {
    bufsize_t i;
    if (name_len == 0) {
        return 0;
    }
    for (i = 0; i < name_len; i++) {
        if (!is_attr_name_char(name[i])) {
            return 0;
        }
    }
    return 1;
}

static int append_attribute(markdown_core_mem *mem, directive_attribute **head, directive_attribute **tail,
                            const unsigned char *name, bufsize_t name_len, const unsigned char *value,
                            bufsize_t value_len, size_t index, int *oom) {
    directive_attribute *attr;
    if (!attribute_name_is_valid(name, name_len)) {
        return 0;
    }
    attr = (directive_attribute *)mem->calloc(1, sizeof(*attr));
    if (!attr) {
        if (oom) {
            *oom = 1;
        }
        return 0;
    }
    if (!replace_chunk_bytes(mem, &attr->name, name, name_len) ||
        !replace_chunk_bytes(mem, &attr->value, value, value_len)) {
        if (oom) {
            *oom = 1;
        }
        free_attribute_list(mem, attr);
        return 0;
    }
    attr->index = index;
    attr->active = 1;
    if (*tail) {
        (*tail)->next = attr;
    } else {
        *head = attr;
    }
    *tail = attr;
    return 1;
}

static int compare_attribute_ptrs(const void *left, const void *right) {
    const directive_attribute *a = *(const directive_attribute *const *)left;
    const directive_attribute *b = *(const directive_attribute *const *)right;
    bufsize_t common = a->name.len < b->name.len ? a->name.len : b->name.len;
    int cmp = memcmp(a->name.data, b->name.data, (size_t)common);
    if (cmp) {
        return cmp;
    }
    if (a->name.len != b->name.len) {
        return a->name.len < b->name.len ? -1 : 1;
    }
    return a->index < b->index ? -1 : a->index > b->index;
}

static int normalize_duplicate_attributes_sorted(markdown_core_mem *mem, directive_attribute *head, size_t count) {
    directive_attribute **sorted;
    directive_attribute *attr;
    size_t i = 0;
    if (count < 2) {
        return 1;
    }
    sorted = (directive_attribute **)mem->calloc(count, sizeof(*sorted));
    if (!sorted) {
        return 0;
    }
    for (attr = head; attr; attr = attr->next) {
        sorted[i++] = attr;
    }
    qsort(sorted, count, sizeof(*sorted), compare_attribute_ptrs);
    for (i = 0; i < count;) {
        size_t end = i + 1;
        while (end < count && sorted[i]->name.len == sorted[end]->name.len &&
               memcmp(sorted[i]->name.data, sorted[end]->name.data, (size_t)sorted[i]->name.len) == 0) {
            end++;
        }
        if (end - i > 1) {
            markdown_core_chunk last_value = sorted[end - 1]->value;
            size_t duplicate;
            sorted[end - 1]->value = sorted[i]->value;
            sorted[i]->value = last_value;
            for (duplicate = i + 1; duplicate < end; duplicate++) {
                sorted[duplicate]->active = 0;
            }
        }
        i = end;
    }
    mem->free(sorted);
    return 1;
}

static size_t attribute_index_expected_size(markdown_core_mem *mem, directive_attribute *head, size_t count) {
    const size_t sample_limit = 1024;
    markdown_core_key_index sample;
    directive_attribute *attr;
    size_t sampled = 0;
    size_t unique;
    if (count <= sample_limit) {
        return count;
    }
    if (!markdown_core_key_index_init(&sample, mem, sample_limit)) {
        return count;
    }
    for (attr = head; attr && sampled < sample_limit; attr = attr->next, sampled++) {
        if (!markdown_core_key_index_insert(&sample, attr->name.data, attr->name.len, attr, 0, NULL)) {
            markdown_core_key_index_free(&sample);
            return count;
        }
    }
    unique = sample.size;
    markdown_core_key_index_free(&sample);
    return unique > sampled / 2 ? count : unique;
}

static int normalize_duplicate_attributes(markdown_core_mem *mem, directive_attribute *head, size_t count) {
    markdown_core_key_index index;
    directive_attribute *attr;
    size_t initial_size;
    if (count < 2) {
        return 1;
    }
    /* Unique-heavy inputs avoid repeated growth, while duplicate-heavy inputs
     * pay for sampled unique keys rather than every source occurrence. */
    initial_size = attribute_index_expected_size(mem, head, count);
    if (!markdown_core_key_index_init(&index, mem, initial_size)) {
        return normalize_duplicate_attributes_sorted(mem, head, count);
    }
    for (attr = head; attr; attr = attr->next) {
        if (!markdown_core_key_index_insert(&index, attr->name.data, attr->name.len, attr, 0, NULL)) {
            markdown_core_key_index_free(&index);
            return normalize_duplicate_attributes_sorted(mem, head, count);
        }
    }
    for (attr = head; attr; attr = attr->next) {
        directive_attribute *first =
            (directive_attribute *)markdown_core_key_index_lookup(&index, attr->name.data, attr->name.len);
        /* Every key was inserted above and the index never deletes, so the
         * lookup cannot miss; keep the release-build guard anyway so a future
         * invariant break degrades to duplicate output instead of a crash. */
        assert(first);
        if (first && first != attr) {
            markdown_core_chunk previous = first->value;
            first->value = attr->value;
            attr->value = previous;
            attr->active = 0;
        }
    }
    markdown_core_key_index_free(&index);
    return 1;
}

static void append_json_escaped(markdown_core_strbuf *buf, const unsigned char *data, bufsize_t len) {
    bufsize_t i;
    char encoded[7];
    for (i = 0; i < len; i++) {
        unsigned char c = data[i];
        switch (c) {
        case '"':
            markdown_core_strbuf_puts(buf, "\\\"");
            break;
        case '\\':
            markdown_core_strbuf_puts(buf, "\\\\");
            break;
        case '\b':
            markdown_core_strbuf_puts(buf, "\\b");
            break;
        case '\f':
            markdown_core_strbuf_puts(buf, "\\f");
            break;
        case '\n':
            markdown_core_strbuf_puts(buf, "\\n");
            break;
        case '\r':
            markdown_core_strbuf_puts(buf, "\\r");
            break;
        case '\t':
            markdown_core_strbuf_puts(buf, "\\t");
            break;
        default:
            if (c < 0x20) {
                snprintf(encoded, sizeof(encoded), "\\u%04x", c);
                markdown_core_strbuf_puts(buf, encoded);
            } else {
                markdown_core_strbuf_putc(buf, c);
            }
        }
    }
}

static const char *render_attributes_json(markdown_core_node *node, node_directive *directive) {
    markdown_core_strbuf json;
    directive_attribute *attr;
    int first = 1;
    if (directive->attributes_json.data) {
        return (const char *)directive->attributes_json.data;
    }
    markdown_core_strbuf_init(markdown_core_node_mem(node), &json, 0);
    markdown_core_strbuf_putc(&json, '{');
    for (attr = directive->attributes; attr; attr = attr->next) {
        if (!attr->active) {
            continue;
        }
        if (!first) {
            markdown_core_strbuf_putc(&json, ',');
        }
        first = 0;
        markdown_core_strbuf_putc(&json, '"');
        append_json_escaped(&json, attr->name.data, attr->name.len);
        markdown_core_strbuf_puts(&json, "\":\"");
        append_json_escaped(&json, attr->value.data, attr->value.len);
        markdown_core_strbuf_putc(&json, '"');
    }
    markdown_core_strbuf_putc(&json, '}');
    directive->attributes_json = markdown_core_chunk_buf_detach(&json);
    return (const char *)directive->attributes_json.data;
}

static void skip_json_space(const unsigned char *data, bufsize_t len, bufsize_t *pos) {
    while (*pos < len && (data[*pos] == ' ' || data[*pos] == '\t' || data[*pos] == '\n' || data[*pos] == '\r')) {
        (*pos)++;
    }
}

static int json_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_json_hex4(const unsigned char *data, bufsize_t len, bufsize_t *pos, int32_t *codepoint) {
    int i;
    int32_t value = 0;
    if (*pos > len - 4) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        int digit = json_hex_value(data[*pos + (bufsize_t)i]);
        if (digit < 0) {
            return 0;
        }
        value = (value << 4) | digit;
    }
    *pos += 4;
    *codepoint = value;
    return 1;
}

static int parse_json_unicode_escape(const unsigned char *data, bufsize_t len, bufsize_t *pos,
                                     markdown_core_strbuf *buf) {
    int32_t codepoint;
    if (!parse_json_hex4(data, len, pos, &codepoint)) {
        return 0;
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
        int32_t low;
        if (*pos > len - 2 || data[*pos] != '\\' || data[*pos + 1] != 'u') {
            return 0;
        }
        *pos += 2;
        if (!parse_json_hex4(data, len, pos, &low) || low < 0xDC00 || low > 0xDFFF) {
            return 0;
        }
        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
        return 0;
    }
    markdown_core_utf8proc_encode_char(codepoint, buf);
    return 1;
}

static int parse_json_string(markdown_core_mem *mem, const unsigned char *data, bufsize_t len, bufsize_t *pos,
                             markdown_core_chunk *result) {
    markdown_core_strbuf value;
    if (*pos >= len || data[*pos] != '"') {
        return 0;
    }
    (*pos)++;
    markdown_core_strbuf_init(mem, &value, 0);
    while (*pos < len) {
        unsigned char c = data[(*pos)++];
        if (c == '"') {
            *result = markdown_core_chunk_buf_detach(&value);
            return 1;
        }
        if (c < 0x20) {
            markdown_core_strbuf_free(&value);
            return 0;
        }
        if (c != '\\') {
            markdown_core_strbuf_putc(&value, c);
            continue;
        }
        if (*pos >= len) {
            markdown_core_strbuf_free(&value);
            return 0;
        }
        c = data[(*pos)++];
        switch (c) {
        case '"':
        case '\\':
        case '/':
            markdown_core_strbuf_putc(&value, c);
            break;
        case 'b':
            markdown_core_strbuf_putc(&value, '\b');
            break;
        case 'f':
            markdown_core_strbuf_putc(&value, '\f');
            break;
        case 'n':
            markdown_core_strbuf_putc(&value, '\n');
            break;
        case 'r':
            markdown_core_strbuf_putc(&value, '\r');
            break;
        case 't':
            markdown_core_strbuf_putc(&value, '\t');
            break;
        case 'u':
            if (!parse_json_unicode_escape(data, len, pos, &value)) {
                markdown_core_strbuf_free(&value);
                return 0;
            }
            break;
        default:
            markdown_core_strbuf_free(&value);
            return 0;
        }
    }
    markdown_core_strbuf_free(&value);
    return 0;
}

static int parse_attributes_json(markdown_core_mem *mem, const unsigned char *data, bufsize_t len,
                                 directive_attribute **result) {
    directive_attribute *head = NULL;
    directive_attribute *tail = NULL;
    size_t count = 0;
    bufsize_t pos = 0;
    int ok = 0;
    *result = NULL;
    skip_json_space(data, len, &pos);
    if (pos >= len || data[pos++] != '{') {
        return 0;
    }
    skip_json_space(data, len, &pos);
    if (pos < len && data[pos] == '}') {
        pos++;
        skip_json_space(data, len, &pos);
        return pos == len;
    }
    while (pos < len) {
        markdown_core_chunk name = MARKDOWN_CORE_CHUNK_EMPTY;
        markdown_core_chunk value = MARKDOWN_CORE_CHUNK_EMPTY;
        if (!parse_json_string(mem, data, len, &pos, &name)) {
            goto done;
        }
        if (!attribute_name_is_valid(name.data, name.len)) {
            goto member_done;
        }
        skip_json_space(data, len, &pos);
        if (pos >= len || data[pos++] != ':') {
            goto member_done;
        }
        skip_json_space(data, len, &pos);
        if (!parse_json_string(mem, data, len, &pos, &value)) {
            goto member_done;
        }
        if (!append_attribute(mem, &head, &tail, name.data, name.len, value.data, value.len, count++, NULL)) {
            goto member_done;
        }
        markdown_core_chunk_free(mem, &name);
        markdown_core_chunk_free(mem, &value);
        skip_json_space(data, len, &pos);
        if (pos < len && data[pos] == ',') {
            pos++;
            skip_json_space(data, len, &pos);
            continue;
        }
        if (pos < len && data[pos] == '}') {
            pos++;
            skip_json_space(data, len, &pos);
            ok = pos == len;
            break;
        }
        goto done;
    member_done:
        markdown_core_chunk_free(mem, &name);
        markdown_core_chunk_free(mem, &value);
        goto done;
    }
done:
    if (ok && normalize_duplicate_attributes(mem, head, count)) {
        *result = head;
        return 1;
    }
    free_attribute_list(mem, head);
    return 0;
}

const char *markdown_core_extensions_get_directive_name(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    if (!directive) {
        return NULL;
    }

    return markdown_core_chunk_to_cstr(markdown_core_node_mem(node), &directive->name);
}

int markdown_core_directive_has_label(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    return directive ? directive->has_label : 0;
}

static int directive_name_is_valid(markdown_core_mem *mem, const char *name) {
    size_t raw_len;
    unsigned char *copy;
    bufsize_t len;
    bufsize_t name_start;
    bufsize_t name_len;
    int valid;

    if (!name) {
        return 0;
    }

    raw_len = strlen(name);
    if (raw_len == 0 || raw_len > INT_MAX) {
        return 0;
    }

    len = (bufsize_t)raw_len;
    copy = (unsigned char *)mem->calloc((size_t)len + 1, 1);
    if (!copy) {
        return 0;
    }

    memcpy(copy, name, (size_t)len);
    valid = scan_name(copy, len, 0, &name_start, &name_len) && name_start == 0 && name_len == len;
    mem->free(copy);
    return valid;
}

int markdown_core_extensions_set_directive_name(markdown_core_node *node, const char *name) {
    node_directive *directive = get_directive(node);

    if (!directive || !directive_name_is_valid(markdown_core_node_mem(node), name)) {
        return 0;
    }

    if (!markdown_core_chunk_set_cstr(markdown_core_node_mem(node), &directive->name, name)) {
        return 0;
    }
    return 1;
}

const char *markdown_core_extensions_get_directive_attributes(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    if (!directive || !directive->has_attributes) {
        return NULL;
    }

    return render_attributes_json(node, directive);
}

int markdown_core_extensions_set_directive_attributes(markdown_core_node *node, const char *attributes) {
    node_directive *directive = get_directive(node);
    directive_attribute *parsed = NULL;
    size_t len;
    if (!directive || !attributes) {
        return 0;
    }
    len = strlen(attributes);
    if (len > INT_MAX) {
        return 0;
    }
    if (!parse_attributes_json(markdown_core_node_mem(node), (const unsigned char *)attributes, (bufsize_t)len,
                               &parsed)) {
        return 0;
    }
    free_attribute_list(markdown_core_node_mem(node), directive->attributes);
    directive->attributes = parsed;
    directive->has_attributes = 1;
    clear_attribute_caches(node, directive);
    return 1;
}

static void directive_opaque_alloc(markdown_core_extension *extension, markdown_core_mem *mem,
                                   markdown_core_node *node) {
    /* A NULL payload is tolerated: every accessor goes through get_directive
     * and treats the node as attribute-less. */
    if (is_directive_node(node)) {
        node->as.opaque = mem->calloc(1, sizeof(node_directive));
    }
}

static void directive_opaque_free(markdown_core_extension *extension, markdown_core_mem *mem,
                                  markdown_core_node *node) {
    node_directive *directive = (node_directive *)node->as.opaque;
    if (!directive) {
        return;
    }

    markdown_core_chunk_free(mem, &directive->name);
    free_attribute_list(mem, directive->attributes);
    markdown_core_chunk_free(mem, &directive->attributes_json);
    mem->free(directive);
}

static int is_attr_name_char(unsigned char c) {
    return c > 0x20 && c != '=' && c != '"' && c != '\'' && c != '<' && c != '>' && c != '/' && c != '{' && c != '}';
}

static int parse_attr_value(const unsigned char *data, bufsize_t len, bufsize_t *pos, const unsigned char **value,
                            bufsize_t *value_len) {
    bufsize_t start;
    unsigned char quote;
    while (*pos < len && ascii_is_space(data[*pos])) {
        (*pos)++;
    }
    if (*pos >= len || ascii_is_space(data[*pos])) {
        *value = data + *pos;
        *value_len = 0;
        return 1;
    }
    if (data[*pos] == '"' || data[*pos] == '\'') {
        quote = data[(*pos)++];
        start = *pos;
        while (*pos < len && data[*pos] != quote) {
            (*pos)++;
        }
        if (*pos >= len) {
            return 0;
        }
        *value = data + start;
        *value_len = *pos - start;
        (*pos)++;
        return 1;
    }
    start = *pos;
    while (*pos < len && !ascii_is_space(data[*pos])) {
        (*pos)++;
    }
    *value = data + start;
    *value_len = *pos - start;
    return 1;
}

static int parse_attributes(markdown_core_mem *mem, const unsigned char *data, bufsize_t len,
                            directive_attribute **result, int *oom) {
    directive_attribute *attrs = NULL;
    directive_attribute *tail = NULL;
    bufsize_t pos = 0;
    size_t count = 0;
    int ok = 1;

    *result = NULL;
    while (pos < len) {
        bufsize_t start;
        bufsize_t name_len;
        const unsigned char *value = (const unsigned char *)"";
        bufsize_t value_len = 0;
        while (pos < len && ascii_is_space(data[pos])) {
            pos++;
        }
        if (pos >= len) {
            break;
        }
        if (data[pos] == '#' || data[pos] == '.') {
            ok = 0;
            break;
        }
        start = pos;
        while (pos < len && is_attr_name_char(data[pos])) {
            pos++;
        }
        if (pos == start) {
            ok = 0;
            break;
        }
        name_len = pos - start;
        while (pos < len && ascii_is_space(data[pos])) {
            pos++;
        }
        if (pos < len && data[pos] == '=') {
            pos++;
            if (!parse_attr_value(data, len, &pos, &value, &value_len)) {
                ok = 0;
                break;
            }
        }
        if (!append_attribute(mem, &attrs, &tail, data + start, name_len, value, value_len, count++, oom)) {
            ok = 0;
            break;
        }
    }

    if (ok && !normalize_duplicate_attributes(mem, attrs, count)) {
        if (oom) {
            *oom = 1;
        }
        ok = 0;
    }
    if (ok) {
        *result = attrs;
        attrs = NULL;
    }
    free_attribute_list(mem, attrs);
    return ok;
}

static void free_parsed_directive(markdown_core_mem *mem, parsed_directive *parsed) {
    free_attribute_list(mem, parsed->attributes);
}

static int parse_directive_suffix(markdown_core_mem *mem, unsigned char *data, bufsize_t len, bufsize_t pos,
                                  parsed_directive *parsed) {
    bufsize_t attr_start;
    bufsize_t attr_len;
    memset(parsed, 0, sizeof(*parsed));

    if (!scan_name(data, len, pos, &parsed->name_start, &parsed->name_len)) {
        return 0;
    }

    pos = parsed->name_start + parsed->name_len;

    if (pos < len && data[pos] == '[') {
        parsed->has_label = 1;
        if (!scan_label(data, len, pos, &parsed->label_start, &parsed->label_len, &pos)) {
            return 0;
        }
    }

    if (pos < len && data[pos] == '{') {
        parsed->has_attributes = 1;
        if (!scan_attributes_raw(data, len, pos, &attr_start, &attr_len, &pos) ||
            !parse_attributes(mem, data + attr_start, attr_len, &parsed->attributes, &parsed->oom)) {
            return 0;
        }
    }

    parsed->end = pos;
    return 1;
}

static markdown_core_node *make_label_node(markdown_core_extension *extension, markdown_core_mem *mem,
                                           const unsigned char *label, bufsize_t label_len, int start_line,
                                           int start_column, int end_column) {
    markdown_core_node *label_node =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, mem, extension);
    if (!label_node) {
        return NULL;
    }

    markdown_core_strbuf_put(&label_node->content, label, label_len);
    if (label_node->content.oom) {
        markdown_core_node_free(label_node);
        return NULL;
    }
    label_node->start_line = label_node->end_line = start_line;
    label_node->start_column = start_column;
    label_node->end_column = end_column;
    return label_node;
}

static int attach_label_node(markdown_core_extension *extension, markdown_core_node *directive_node,
                             const unsigned char *label, bufsize_t label_len, int start_line, int start_column,
                             int end_column) {
    markdown_core_node *label_node;

    label_node = make_label_node(extension, markdown_core_node_mem(directive_node), label, label_len, start_line,
                                 start_column, end_column);
    if (!label_node) {
        return 0;
    }

    if (!markdown_core_node_append_child(directive_node, label_node)) {
        markdown_core_node_free(label_node);
        return 0;
    }

    return 1;
}

static int apply_parsed_directive(markdown_core_extension *extension, markdown_core_node *node,
                                  const unsigned char *data, parsed_directive *parsed, int start_line,
                                  int start_column) {
    node_directive *directive = get_directive(node);
    markdown_core_mem *mem = markdown_core_node_mem(node);

    if (!directive) {
        return 0;
    }

    if (!set_chunk_bytes(mem, &directive->name, data + parsed->name_start, parsed->name_len)) {
        return 0;
    }
    directive->has_label = parsed->has_label;
    directive->has_attributes = parsed->has_attributes;

    if (parsed->has_attributes) {
        directive->attributes = parsed->attributes;
        parsed->attributes = NULL;
        clear_attribute_caches(node, directive);
    }

    if (parsed->has_label) {
        int label_start_column = start_column + (int)parsed->label_start + 1;
        int label_end_column = label_start_column + (int)parsed->label_len - 1;
        if (parsed->label_len == 0) {
            label_end_column = label_start_column - 1;
        }

        if (!attach_label_node(extension, node, data + parsed->label_start, parsed->label_len, start_line,
                               label_start_column, label_end_column)) {
            return 0;
        }
    }

    return 1;
}

static markdown_core_node *make_directive_node(markdown_core_extension *extension, markdown_core_parser *parser,
                                               const unsigned char *name, bufsize_t name_len, int start_line,
                                               int start_column, int end_line, int end_column) {
    markdown_core_node *node =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_DIRECTIVE, parser->mem, extension);
    node_directive *directive;

    if (!node) {
        parser->oom = true;
        return NULL;
    }

    directive = get_directive(node);
    if (!directive) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    if (!set_chunk_bytes(parser->mem, &directive->name, name, name_len)) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    node->start_line = start_line;
    node->end_line = end_line;
    node->start_column = start_column;
    node->end_column = end_column;
    return node;
}

static markdown_core_node *make_name_only_directive(markdown_core_extension *extension, markdown_core_parser *parser,
                                                    markdown_core_inline_parser *inline_parser,
                                                    const unsigned char *name, bufsize_t name_len,
                                                    bufsize_t end_offset) {
    markdown_core_node *node;
    int start_line = markdown_core_inline_parser_get_line(inline_parser);
    int start_column = markdown_core_inline_parser_get_column(inline_parser);
    bufsize_t offset = (bufsize_t)markdown_core_inline_parser_get_offset(inline_parser);

    node = make_directive_node(extension, parser, name, name_len, start_line, start_column, start_line,
                               start_column + (int)(end_offset - offset) - 1);
    if (node) {
        markdown_core_inline_parser_set_offset(inline_parser, (int)end_offset);
    }

    return node;
}

static markdown_core_node *make_delimiter_text(markdown_core_parser *parser, markdown_core_inline_parser *inline_parser,
                                               bufsize_t offset, bufsize_t len) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_node *node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);

    if (!node) {
        return NULL;
    }

    node->as.literal = markdown_core_chunk_dup(chunk, offset, len);
    node->start_line = node->end_line = markdown_core_inline_parser_get_line(inline_parser);
    node->start_column = markdown_core_inline_parser_get_column(inline_parser);
    node->end_column = node->start_column + (int)len - 1;
    markdown_core_inline_parser_set_offset(inline_parser, (int)(offset + len));
    return node;
}

static markdown_core_node *match_directive_delimiter(markdown_core_parser *parser,
                                                     markdown_core_inline_parser *inline_parser,
                                                     unsigned char delim_char, bufsize_t offset, bufsize_t len,
                                                     int can_open, int can_close) {
    markdown_core_node *node = make_delimiter_text(parser, inline_parser, offset, len);

    if (!node) {
        parser->oom = true;
        return NULL;
    }

    markdown_core_inline_parser_push_delimiter(inline_parser, delim_char, can_open, can_close, node);
    return node;
}

static delimiter *find_directive_opener(markdown_core_inline_parser *inline_parser, unsigned char delim_char) {
    delimiter *delim = markdown_core_inline_parser_get_last_delimiter(inline_parser);
    int closer_count = 0;

    while (delim) {
        if (delim->delim_char == delim_char) {
            if (delim->can_close) {
                closer_count++;
            } else if (delim->can_open) {
                if (closer_count > 0) {
                    closer_count--;
                } else {
                    return delim;
                }
            }
        }
        delim = delim->previous;
    }

    return NULL;
}

static int scan_parsed_attributes(markdown_core_mem *mem, const unsigned char *data, bufsize_t len, bufsize_t pos,
                                  bufsize_t *end, int *oom) {
    bufsize_t attr_start;
    bufsize_t attr_len;
    directive_attribute *attributes = NULL;
    int valid = scan_attributes_raw(data, len, pos, &attr_start, &attr_len, end) &&
                parse_attributes(mem, data + attr_start, attr_len, &attributes, oom);
    free_attribute_list(mem, attributes);
    return valid;
}

static markdown_core_node *match_colon_directive(markdown_core_extension *extension, markdown_core_parser *parser,
                                                 markdown_core_inline_parser *inline_parser, markdown_core_chunk *chunk,
                                                 bufsize_t offset) {
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t pos;

    if (offset + 1 >= chunk->len || chunk->data[offset + 1] == ':') {
        return NULL;
    }

    if (!scan_name(chunk->data, chunk->len, offset + 1, &name_start, &name_len)) {
        return NULL;
    }

    pos = name_start + name_len;
    if (pos < chunk->len && chunk->data[pos] == '[') {
        return match_directive_delimiter(parser, inline_parser, DIRECTIVE_LABEL_DELIM, offset, pos - offset + 1, 1, 0);
    }

    if (pos < chunk->len && chunk->data[pos] == '{') {
        bufsize_t attr_start;
        bufsize_t attr_len;
        bufsize_t end;
        markdown_core_node *node;
        directive_attribute *attributes = NULL;
        node_directive *directive;
        int line = markdown_core_inline_parser_get_line(inline_parser);
        int column = markdown_core_inline_parser_get_column(inline_parser);

        int attr_oom = 0;

        if (!scan_attributes_raw(chunk->data, chunk->len, pos, &attr_start, &attr_len, &end) ||
            !parse_attributes(parser->mem, chunk->data + attr_start, attr_len, &attributes, &attr_oom)) {
            if (attr_oom) {
                parser->oom = true;
            }
            return NULL;
        }
        node = make_directive_node(extension, parser, chunk->data + name_start, name_len, line, column, line,
                                   column + (int)(end - offset) - 1);
        if (!node) {
            free_attribute_list(parser->mem, attributes);
            return NULL;
        }
        directive = get_directive(node);
        directive->attributes = attributes;
        directive->has_attributes = 1;
        markdown_core_inline_parser_set_offset(inline_parser, (int)end);
        return node;
    }

    if (pos < chunk->len && chunk->data[pos] == ':') {
        return NULL;
    }

    return make_name_only_directive(extension, parser, inline_parser, chunk->data + name_start, name_len, pos);
}

static markdown_core_node *match_label_closer(markdown_core_parser *parser, markdown_core_inline_parser *inline_parser,
                                              markdown_core_chunk *chunk, bufsize_t offset) {
    delimiter *opener = find_directive_opener(inline_parser, DIRECTIVE_LABEL_DELIM);
    bufsize_t end;
    bufsize_t closer_len = 1;

    if (!opener) {
        return NULL;
    }

    {
        int attr_oom = 0;
        if (offset + 1 < chunk->len && chunk->data[offset + 1] == '{' &&
            scan_parsed_attributes(parser->mem, chunk->data, chunk->len, offset + 1, &end, &attr_oom)) {
            closer_len = end - offset;
        }
        if (attr_oom) {
            parser->oom = true;
        }
    }

    return match_directive_delimiter(parser, inline_parser, DIRECTIVE_LABEL_DELIM, offset, closer_len, 0, 1);
}

static markdown_core_node *match(markdown_core_extension *extension, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    bufsize_t offset = (bufsize_t)markdown_core_inline_parser_get_offset(inline_parser);

    if (!directive_enabled(parser)) {
        return NULL;
    }

    if (character == ':') {
        return match_colon_directive(extension, parser, inline_parser, chunk, offset);
    }

    if (character == ']') {
        return match_label_closer(parser, inline_parser, chunk, offset);
    }

    return NULL;
}

static bufsize_t count_colons(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    bufsize_t count = 0;
    while (pos + count < len && data[pos + count] == ':') {
        count++;
    }
    return count;
}

static markdown_core_node *open_directive_block(markdown_core_extension *extension, int indented,
                                                markdown_core_parser *parser, markdown_core_node *parent_container,
                                                unsigned char *input, int len) {
    bufsize_t first_nonspace = (bufsize_t)markdown_core_parser_get_first_nonspace(parser);
    bufsize_t colon_count;
    parsed_directive parsed;
    markdown_core_node *node;
    node_directive *directive;

    if (!directive_enabled(parser) || indented) {
        return NULL;
    }

    colon_count = count_colons(input, (bufsize_t)len, first_nonspace);
    if (colon_count < 2) {
        return NULL;
    }

    if (!parse_directive_suffix(parser->mem, input, (bufsize_t)len, first_nonspace + colon_count, &parsed)) {
        if (parsed.oom) {
            parser->oom = true;
        }
        return NULL;
    }

    if (!has_only_spaces_until_line_end(input, (bufsize_t)len, parsed.end)) {
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    node = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
                                          (int)first_nonspace + 1);
    if (!node) {
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    markdown_core_node_set_extension(node, extension);
    node->as.opaque = parser->mem->calloc(1, sizeof(node_directive));
    if (!node->as.opaque) {
        parser->oom = true;
        markdown_core_node_free(node);
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    if (!apply_parsed_directive(extension, node, input, &parsed, markdown_core_parser_get_line_number(parser),
                                (int)first_nonspace)) {
        /* The suffix already validated; failure here is allocation loss. */
        parser->oom = true;
        markdown_core_node_free(node);
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    directive = get_directive(node);
    directive->fence_length = (int)colon_count;
    directive->closed = (colon_count == 2);
    directive->consume_line = 1;

    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);

    free_parsed_directive(parser->mem, &parsed);
    return node;
}

static int directive_block_matches(markdown_core_extension *extension, markdown_core_parser *parser,
                                   unsigned char *input, int len, markdown_core_node *container) {
    node_directive *directive = get_directive(container);
    bufsize_t first_nonspace = (bufsize_t)markdown_core_parser_get_first_nonspace(parser);
    bufsize_t colon_count;

    if (!directive) {
        return 0;
    }

    if (directive->closed) {
        return 0;
    }

    directive->consume_line = 0;

    colon_count = count_colons(input, (bufsize_t)len, first_nonspace);
    if (markdown_core_parser_get_indent(parser) <= 3 && colon_count >= (bufsize_t)directive->fence_length &&
        has_only_spaces_until_line_end(input, (bufsize_t)len, first_nonspace + colon_count)) {
        directive->closed = 1;
        directive->consume_line = 1;
        markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser),
                                            false);
    }

    return 1;
}

static void remove_delimiters(markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    delimiter *delim = closer;

    while (delim != NULL && delim != opener) {
        delimiter *previous = delim->previous;
        markdown_core_inline_parser_remove_delimiter(inline_parser, delim);
        delim = previous;
    }

    markdown_core_inline_parser_remove_delimiter(inline_parser, opener);
}

static int set_attributes_from_wrapper(markdown_core_node *node, const unsigned char *data, bufsize_t len,
                                       bufsize_t pos, int *oom) {
    bufsize_t attr_start;
    bufsize_t attr_len;
    bufsize_t end;
    directive_attribute *attributes = NULL;
    node_directive *directive = get_directive(node);

    if (!directive || !scan_attributes_raw(data, len, pos, &attr_start, &attr_len, &end) || end != len ||
        !parse_attributes(markdown_core_node_mem(node), data + attr_start, attr_len, &attributes, oom)) {
        return 0;
    }

    directive->attributes = attributes;
    directive->has_attributes = 1;
    clear_attribute_caches(node, directive);
    return 1;
}

static markdown_core_node *make_empty_label_node(markdown_core_extension *extension, markdown_core_mem *mem,
                                                 int start_line, int start_column, int end_line, int end_column) {
    markdown_core_node *label_node =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, mem, extension);

    if (!label_node) {
        return NULL;
    }

    label_node->start_line = start_line;
    label_node->end_line = end_line;
    label_node->start_column = start_column;
    label_node->end_column = end_column;
    return label_node;
}

static delimiter *insert_label_directive(markdown_core_extension *extension, markdown_core_parser *parser,
                                         markdown_core_inline_parser *inline_parser, delimiter *opener,
                                         delimiter *closer) {
    markdown_core_node *opener_node = opener->inl_text;
    markdown_core_node *closer_node = closer->inl_text;
    markdown_core_chunk *opener_literal = &opener_node->as.literal;
    markdown_core_chunk *closer_literal = &closer_node->as.literal;
    delimiter *res = closer->next;
    markdown_core_node *directive_node;
    markdown_core_node *label_node;
    markdown_core_node *tmp;
    markdown_core_node *tmpnext;
    node_directive *directive;
    bufsize_t name_len;

    if (opener->delim_char != closer->delim_char || opener_literal->len < 3 || opener_literal->data[0] != ':' ||
        opener_literal->data[opener_literal->len - 1] != '[' || closer_literal->len < 1 ||
        closer_literal->data[0] != ']') {
        goto done;
    }

    name_len = opener_literal->len - 2;
    directive_node = make_directive_node(extension, parser, opener_literal->data + 1, name_len, opener_node->start_line,
                                         opener_node->start_column, closer_node->end_line, closer_node->end_column);
    if (!directive_node) {
        goto done;
    }

    directive = get_directive(directive_node);
    directive->has_label = 1;

    {
        int attr_oom = 0;
        if (closer_literal->len > 1 && closer_literal->data[1] == '{' &&
            !set_attributes_from_wrapper(directive_node, closer_literal->data, closer_literal->len, 1, &attr_oom)) {
            if (attr_oom) {
                parser->oom = true;
            }
            markdown_core_node_free(directive_node);
            goto done;
        }
    }

    label_node = make_empty_label_node(extension, parser->mem, opener_node->end_line, opener_node->end_column + 1,
                                       closer_node->start_line, closer_node->start_column - 1);
    if (!label_node) {
        markdown_core_node_free(directive_node);
        goto done;
    }

    tmp = opener_node->next;
    while (tmp && tmp != closer_node) {
        tmpnext = tmp->next;
        markdown_core_node_unlink(tmp);
        markdown_core_node_append_child(label_node, tmp);
        tmp = tmpnext;
    }

    markdown_core_node_append_child(directive_node, label_node);

    if (markdown_core_node_insert_before(opener_node, directive_node)) {
        markdown_core_node_free(opener_node);
        markdown_core_node_free(closer_node);
    } else {
        markdown_core_node_free(directive_node);
    }

done:
    remove_delimiters(inline_parser, opener, closer);
    return res;
}

static delimiter *insert_directive(markdown_core_extension *extension, markdown_core_parser *parser,
                                   markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    if (opener->delim_char == DIRECTIVE_LABEL_DELIM) {
        return insert_label_directive(extension, parser, inline_parser, opener, closer);
    }

    return closer->next;
}

static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE) {
        return "directive";
    }

    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return "directive_block";
    }

    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
        return "directive_label";
    }

    return "<unknown>";
}

static int can_contain(markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE) {
        return child_type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
    }

    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return child_type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL ||
               (MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM &&
                child_type != MARKDOWN_CORE_NODE_DOCUMENT);
    }

    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type) && child_type != MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
    }

    return 0;
}

static int contains_inlines(markdown_core_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
}

static int accepts_lines(markdown_core_extension *extension, markdown_core_node *node) {
    node_directive *directive = get_directive(node);

    if (!directive) {
        return 0;
    }

    if (node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return 0;
    }

    return directive->fence_length == 2 || directive->consume_line;
}

static const unsigned char directive_special_chars[] = {':', ']', '}', DIRECTIVE_LABEL_DELIM};

static const markdown_core_extension directive_extension = {
    .name = "directive",
    .match_inline = match,
    .insert_inline_from_delim = insert_directive,
    .last_block_matches = directive_block_matches,
    .try_opening_block = open_directive_block,
    .get_type_string = get_type_string,
    .can_contain = can_contain,
    .contains_inlines = contains_inlines,
    .accepts_lines = accepts_lines,
    .alloc_opaque = directive_opaque_alloc,
    .free_opaque = directive_opaque_free,
    .special_inline_chars = directive_special_chars,
    .special_inline_char_count = sizeof(directive_special_chars),
    .emphasis = true,
};

markdown_core_extension *markdown_core_directive_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&directive_extension;
}
