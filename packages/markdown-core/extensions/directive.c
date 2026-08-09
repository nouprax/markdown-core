#include "directive.h"

#include <assert.h>
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
#include "inline_util.h"

enum {
    DIRECTIVE_LABEL_RULE = 0,
};

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
    int has_attributes;
} node_directive;

typedef struct {
    markdown_core_bufsize name_start;
    markdown_core_bufsize name_len;
    markdown_core_bufsize label_start;
    markdown_core_bufsize label_len;
    int has_label;
    int has_attributes;
    directive_attribute *attributes;
    /* The raw interior of the `{...}` block when has_attributes — the one
     * source spelling of the normalized attributes, kept for the concrete
     * DIRECTIVE_ATTRIBUTES record (braces sit at attr_start-1 and
     * attr_start+attr_len). */
    markdown_core_bufsize attr_start;
    markdown_core_bufsize attr_len;
    markdown_core_bufsize end;
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

static int is_attr_name_start_char(int32_t c);
static int is_attr_name_char(int32_t c);
/* Length of the name at `pos`, or 0 if none starts there. */
static markdown_core_bufsize scan_attr_name(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos
);

static int scan_name(
    unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos,
    markdown_core_bufsize *name_start,
    markdown_core_bufsize *name_len
) {
    markdown_core_bufsize match_len = scan_directive_name(data, len, pos);
    if (match_len == 0) {
        return 0;
    }

    // A name may contain `-` and `_` but may not begin or end with one. The
    // reference implementation (micromark-extension-directive) rejects both
    // ends; its prose documents only the trailing rule, so the leading one was
    // verified against the implementation itself: `:-a[]` and `:_a[]` are not
    // directives there, while `:1a[]` is.
    if (data[pos] == '-' || data[pos] == '_') {
        return 0;
    }

    if (data[pos + match_len - 1] == '-' || data[pos + match_len - 1] == '_') {
        return 0;
    }

    *name_start = pos;
    *name_len = match_len;
    return 1;
}

static int scan_label(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos,
    markdown_core_bufsize *label_start,
    markdown_core_bufsize *label_len,
    markdown_core_bufsize *end
) {
    int depth = 1;
    markdown_core_bufsize i;

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

static int scan_attributes_raw(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos,
    markdown_core_bufsize *attr_start,
    markdown_core_bufsize *attr_len,
    markdown_core_bufsize *end
) {
    unsigned char quote = 0;
    markdown_core_bufsize i;

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

static int set_chunk_bytes(
    markdown_core_mem *mem,
    markdown_core_chunk *chunk,
    const unsigned char *data,
    markdown_core_bufsize len
) {
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

static int replace_chunk_bytes(
    markdown_core_mem *mem,
    markdown_core_chunk *chunk,
    const unsigned char *data,
    markdown_core_bufsize len
) {
    unsigned char *copy = (unsigned char *)mem->calloc(mem, (size_t)len + 1, 1);
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
        mem->free(mem, attr);
        attr = next;
    }
}

static int attribute_name_is_valid(const unsigned char *name, markdown_core_bufsize name_len) {
    if (name_len == 0) {
        return 0;
    }
    /* One scan of the whole name: it is valid only if the grammar consumes
     * every byte of it. */
    return scan_attr_name(name, name_len, 0) == name_len;
}

static int append_attribute(
    markdown_core_mem *mem,
    directive_attribute **head,
    directive_attribute **tail,
    const unsigned char *name,
    markdown_core_bufsize name_len,
    const unsigned char *value,
    markdown_core_bufsize value_len,
    size_t index,
    int *oom
) {
    directive_attribute *attr;
    if (!attribute_name_is_valid(name, name_len)) {
        return 0;
    }
    attr = (directive_attribute *)mem->calloc(mem, 1, sizeof(*attr));
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
    markdown_core_bufsize common = a->name.len < b->name.len ? a->name.len : b->name.len;
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
    sorted = (directive_attribute **)mem->calloc(mem, count, sizeof(*sorted));
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
    mem->free(mem, sorted);
    return 1;
}

/* `class` is the one attribute whose repeats accumulate instead of replacing:
 * `{.a .b}`, `{.a.b}`, and `{class=a .b}` all mean `class="a b"`, in source
 * order. Every other name keeps the last value, `id` included.
 *
 * This runs before duplicate normalization and collapses the repeats out of
 * the list entirely, so the two dedup paths below never see more than one
 * `class` and neither needs to know the rule. `*count` is updated because both
 * of them size allocations from it.
 */
static int merge_class_attributes(markdown_core_mem *mem, directive_attribute *head, size_t *count) {
    static const char CLASS_NAME[] = "class";
    const markdown_core_bufsize class_len = (markdown_core_bufsize)(sizeof(CLASS_NAME) - 1);
    directive_attribute *first = NULL;
    directive_attribute *attr;
    markdown_core_strbuf joined;
    int merged = 0;

    for (attr = head; attr; attr = attr->next) {
        if (attr->name.len != class_len || memcmp(attr->name.data, CLASS_NAME, (size_t)class_len) != 0) {
            continue;
        }
        if (!first) {
            first = attr;
            continue;
        }
        merged = 1;
    }
    if (!merged) {
        return 1;
    }

    // Collect the joined value first, then unlink the repeats. Removing them
    // outright rather than deactivating them is what lets the dedup passes stay
    // unaware of this rule: they walk the list without consulting `active`.
    markdown_core_strbuf_init(mem, &joined, 32);
    {
        directive_attribute *previous = NULL;
        attr = head;
        while (attr) {
            directive_attribute *next = attr->next;
            int is_class = attr->name.len == class_len && memcmp(attr->name.data, CLASS_NAME, (size_t)class_len) == 0;
            if (is_class) {
                // The separator goes before every value once something has
                // accumulated, an empty value included: `{.a class="" .b}` is
                // `class="a  b"`. An empty value at the front accumulates
                // nothing, so `{class="" .b}` is `"b"` rather than `" b"` —
                // micromark-extension-directive tests the accumulated string,
                // not the count of values seen.
                if (joined.size) {
                    markdown_core_strbuf_putc(&joined, ' ');
                }
                markdown_core_strbuf_put(&joined, attr->value.data, attr->value.len);
                if (attr != first) {
                    // `first` precedes every repeat, so a repeat is never the
                    // head and `previous` is always set here.
                    previous->next = next;
                    attr->next = NULL;
                    free_attribute_list(mem, attr);
                    (*count)--;
                    attr = next;
                    continue;
                }
            }
            previous = attr;
            attr = next;
        }
    }
    // strbuf keeps `ptr` pointing at a static sentinel and reports failure
    // through `oom`, so testing the pointer would never catch a lost
    // allocation and the join would silently truncate.
    if (joined.oom || !replace_chunk_bytes(mem, &first->value, joined.ptr, (markdown_core_bufsize)joined.size)) {
        markdown_core_strbuf_free(&joined);
        return 0;
    }
    markdown_core_strbuf_free(&joined);
    return 1;
}

static int normalize_duplicate_attributes(markdown_core_mem *mem, directive_attribute *head, size_t count) {
    markdown_core_key_index index;
    directive_attribute *attr;
    if (count < 2) {
        return 1;
    }
    if (!markdown_core_key_index_init(&index, mem)) {
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

static void append_json_escaped(markdown_core_strbuf *buf, const unsigned char *data, markdown_core_bufsize len) {
    markdown_core_bufsize i;
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

static void skip_json_space(const unsigned char *data, markdown_core_bufsize len, markdown_core_bufsize *pos) {
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

static int parse_json_hex4(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize *pos,
    int32_t *codepoint
) {
    int i;
    int32_t value = 0;
    if (*pos > len - 4) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        int digit = json_hex_value(data[*pos + (markdown_core_bufsize)i]);
        if (digit < 0) {
            return 0;
        }
        value = (value << 4) | digit;
    }
    *pos += 4;
    *codepoint = value;
    return 1;
}

static int parse_json_unicode_escape(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize *pos,
    markdown_core_strbuf *buf
) {
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

static int parse_json_string(
    markdown_core_mem *mem,
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize *pos,
    markdown_core_chunk *result
) {
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

static int parse_attributes_json(
    markdown_core_mem *mem,
    const unsigned char *data,
    markdown_core_bufsize len,
    directive_attribute **result
) {
    directive_attribute *head = NULL;
    directive_attribute *tail = NULL;
    size_t count = 0;
    markdown_core_bufsize pos = 0;
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

markdown_core_node *markdown_core_directive_label(markdown_core_node *node) {
    if (!is_directive_node(node) || !node->first_child ||
        node->first_child->type != MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
        return NULL;
    }
    return node->first_child;
}

static int directive_name_is_valid(markdown_core_mem *mem, const char *name) {
    size_t raw_len;
    unsigned char *copy;
    markdown_core_bufsize len;
    markdown_core_bufsize name_start;
    markdown_core_bufsize name_len;
    int valid;

    if (!name) {
        return 0;
    }

    raw_len = strlen(name);
    if (raw_len == 0 || raw_len > INT_MAX) {
        return 0;
    }

    len = (markdown_core_bufsize)raw_len;
    copy = (unsigned char *)mem->calloc(mem, (size_t)len + 1, 1);
    if (!copy) {
        return 0;
    }

    memcpy(copy, name, (size_t)len);
    valid = scan_name(copy, len, 0, &name_start, &name_len) && name_start == 0 && name_len == len;
    mem->free(mem, copy);
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
    if (!parse_attributes_json(
            markdown_core_node_mem(node),
            (const unsigned char *)attributes,
            (markdown_core_bufsize)len,
            &parsed
        )) {
        return 0;
    }
    free_attribute_list(markdown_core_node_mem(node), directive->attributes);
    directive->attributes = parsed;
    directive->has_attributes = 1;
    clear_attribute_caches(node, directive);
    return 1;
}

static void directive_opaque_alloc(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *node
) {
    /* A NULL payload is tolerated: every accessor goes through get_directive
     * and treats the node as attribute-less. */
    if (is_directive_node(node)) {
        node->as.opaque = mem->calloc(mem, 1, sizeof(node_directive));
    }
}

static void directive_opaque_free(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *node
) {
    node_directive *directive = (node_directive *)node->as.opaque;
    if (!directive) {
        return;
    }

    markdown_core_chunk_free(mem, &directive->name);
    free_attribute_list(mem, directive->attributes);
    markdown_core_chunk_free(mem, &directive->attributes_json);
    mem->free(mem, directive);
}

/* Attribute names follow micromark-extension-directive: a name may not begin
 * with whitespace or punctuation, except that `-` and `_` are allowed there,
 * and from the second character `.` and `:` are allowed as well. `:` and `.`
 * being punctuation is why `{a:b}` is a name and `{:_a}` is not a valid
 * attribute block at all.
 *
 * The reference tests `\p{P}|\p{S}`. Below U+0080 the two agree exactly,
 * because C's ispunct spans both categories; above it this uses the parser's
 * own punctuation table, which carries the P categories but not the non-ASCII
 * S ones. A non-ASCII symbol in an attribute name is therefore the one place
 * the grammars can still differ. */
static int is_attr_name_start_char(int32_t c) {
    if (c == '-' || c == '_') {
        return 1;
    }
    return !markdown_core_utf8proc_is_space(c) && !markdown_core_utf8proc_is_punctuation(c);
}

static int is_attr_name_char(int32_t c) {
    if (c == '-' || c == '_' || c == '.' || c == ':') {
        return 1;
    }
    return !markdown_core_utf8proc_is_space(c) && !markdown_core_utf8proc_is_punctuation(c);
}

/* The name is scanned by code point, not by byte: the rules above are Unicode
 * predicates, and a multi-byte character tested one byte at a time would be
 * classified by bytes that mean nothing on their own. */
static markdown_core_bufsize scan_attr_name(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos
) {
    markdown_core_bufsize start = pos;
    int first = 1;

    while (pos < len) {
        int32_t code = 0;
        markdown_core_bufsize width = markdown_core_utf8proc_iterate(data + pos, len - pos, &code);
        /* Bounds, not validity. `iterate` reports a non-positive width when
         * the buffer stops inside a character — a truncated final code point,
         * which 8.2 makes a legal final document — and advancing by it would
         * not terminate. This is not a guard against input that is not UTF-8;
         * input is UTF-8 (7.1) and nothing here checks. */
        if (width <= 0) {
            break;
        }
        if (first ? !is_attr_name_start_char(code) : !is_attr_name_char(code)) {
            break;
        }
        first = 0;
        pos += width;
    }
    return pos - start;
}

static int parse_attr_value(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize *pos,
    const unsigned char **value,
    markdown_core_bufsize *value_len
) {
    markdown_core_bufsize start;
    unsigned char quote;
    while (*pos < len && ascii_is_space(data[*pos])) {
        (*pos)++;
    }
    // An `=` promises a value. Reaching the end of the block without one — the
    // block interior arrives with its `}` already removed, so this covers both
    // `{a=}` and `{a=` followed by the line ending — makes the whole attribute
    // block malformed rather than yielding an attribute with an empty value.
    // micromark-extension-directive rejects it at `valueBefore`, and the
    // difference is visible: an empty value would otherwise be
    // indistinguishable from the valueless `{a}`, which is a different thing.
    if (*pos >= len) {
        return 0;
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
        // A quoted value has to be separated from whatever follows it. Without
        // this, `{a="x"b=1}` reads as two attributes here and as an invalid
        // block in micromark-extension-directive — and, worse, a quote that
        // only closes much later lets the block swallow everything between,
        // which is how an unterminated quote used to absorb the rest of a
        // paragraph.
        if (*pos < len && !ascii_is_space(data[*pos]) && data[*pos] != '}') {
            return 0;
        }
        return 1;
    }
    // An unquoted value ends at whitespace or at the block's closer, and may
    // not contain `"`, `'`, `<`, `=`, `>`, or a backtick anywhere — reaching
    // one of those does not end the value, it makes the block malformed
    // (micromark-extension-directive, `valueUnquoted`). Quotes are already
    // refused earlier by the closer probe, which will not accept a block whose
    // quote never closes; they are listed here because this function is where
    // the grammar lives, not because the earlier check is in doubt.
    start = *pos;
    while (*pos < len && !ascii_is_space(data[*pos])) {
        switch (data[*pos]) {
        case '"':
        case '\'':
        case '<':
        case '=':
        case '>':
        case '`':
            return 0;
        default:
            break;
        }
        (*pos)++;
    }
    *value = data + start;
    *value_len = *pos - start;
    return 1;
}

typedef int (*attribute_sink)(
    void *context,
    const unsigned char *name,
    markdown_core_bufsize name_len,
    const unsigned char *value,
    markdown_core_bufsize value_len,
    size_t index
);

/*
 * One allocation-free grammar scanner serves both the phase-one closer probe
 * and the phase-two payload builder. The former validates only; the latter
 * supplies a sink and therefore parses each attribute exactly once into its
 * final representation.
 */
static int scan_attribute_sequence(
    const unsigned char *data,
    markdown_core_bufsize len,
    attribute_sink sink,
    void *context,
    size_t *attribute_count
) {
    markdown_core_bufsize pos = 0;
    size_t count = 0;
    while (pos < len) {
        markdown_core_bufsize start;
        markdown_core_bufsize name_len;
        const unsigned char *value = (const unsigned char *)"";
        markdown_core_bufsize value_len = 0;
        while (pos < len && ascii_is_space(data[pos])) {
            pos++;
        }
        if (pos >= len) {
            break;
        }
        // `#name` and `.name` are shorthand for the `id` and `class`
        // attributes, as in micromark-extension-directive. A marker with no
        // name after it is skipped rather than failing the block: `{#}` and
        // `{.}` produce a directive with no attributes there.
        if (data[pos] == '#' || data[pos] == '.') {
            const unsigned char *shorthand =
                data[pos] == '#' ? (const unsigned char *)"id" : (const unsigned char *)"class";
            markdown_core_bufsize shorthand_len = data[pos] == '#' ? 2 : 5;
            pos++;
            start = pos;
            // A shorthand value ends at the next marker as well as at the
            // characters an ordinary name ends at, so `{.a.b}` is two classes
            // rather than one named `a.b`.
            while (pos < len && is_attr_name_char(data[pos]) && data[pos] != '.' && data[pos] != '#') {
                pos++;
            }
            // A marker with no name is not a valid attribute, and it
            // invalidates the block rather than being skipped: remark parses
            // `:n{#}` as a directive named `n` followed by the literal text
            // `{#}`, which is the malformed-block fallback above.
            if (pos == start) {
                return 0;
            }
            if (sink && !sink(context, shorthand, shorthand_len, data + start, pos - start, count)) {
                return 0;
            }
            count++;
            continue;
        }
        start = pos;
        name_len = scan_attr_name(data, len, pos);
        if (name_len == 0) {
            return 0;
        }
        pos += name_len;
        while (pos < len && ascii_is_space(data[pos])) {
            pos++;
        }
        if (pos < len && data[pos] == '=') {
            pos++;
            if (!parse_attr_value(data, len, &pos, &value, &value_len)) {
                return 0;
            }
        }
        if (sink && !sink(context, data + start, name_len, value, value_len, count)) {
            return 0;
        }
        count++;
    }
    if (attribute_count) {
        *attribute_count = count;
    }
    return 1;
}

typedef struct {
    markdown_core_mem *mem;
    directive_attribute *head;
    directive_attribute *tail;
    int *oom;
} attribute_builder;

static int build_attribute(
    void *context,
    const unsigned char *name,
    markdown_core_bufsize name_len,
    const unsigned char *value,
    markdown_core_bufsize value_len,
    size_t index
) {
    attribute_builder *builder = (attribute_builder *)context;
    return append_attribute(
        builder->mem,
        &builder->head,
        &builder->tail,
        name,
        name_len,
        value,
        value_len,
        index,
        builder->oom
    );
}

static int parse_attributes(
    markdown_core_mem *mem,
    const unsigned char *data,
    markdown_core_bufsize len,
    directive_attribute **result,
    int *oom
) {
    attribute_builder builder = {mem, NULL, NULL, oom};
    size_t count = 0;
    int ok;

    *result = NULL;
    ok = scan_attribute_sequence(data, len, build_attribute, &builder, &count);
    if (ok && !merge_class_attributes(mem, builder.head, &count)) {
        if (oom) {
            *oom = 1;
        }
        ok = 0;
    }
    if (ok && !normalize_duplicate_attributes(mem, builder.head, count)) {
        if (oom) {
            *oom = 1;
        }
        ok = 0;
    }
    if (ok) {
        *result = builder.head;
        builder.head = NULL;
    }
    free_attribute_list(mem, builder.head);
    return ok;
}

static void free_parsed_directive(markdown_core_mem *mem, parsed_directive *parsed) {
    free_attribute_list(mem, parsed->attributes);
}

static int parse_directive_suffix(
    markdown_core_mem *mem,
    unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos,
    parsed_directive *parsed
) {
    markdown_core_bufsize attr_start;
    markdown_core_bufsize attr_len;
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

    // An attribute block that does not parse does not invalidate the
    // directive: the name still stands and the braces stay as literal text,
    // which is what micromark-extension-directive does — `:invalid{=value}`
    // is a directive named `invalid` followed by the text `{=value}`. Ending
    // the directive at `pos` leaves the braces unconsumed, so the inline
    // parser emits them as text.
    //
    // Allocation loss is not a syntax verdict, so it still fails the parse
    // rather than silently producing an attribute-less directive.
    if (pos < len && data[pos] == '{') {
        markdown_core_bufsize after_attributes = pos;
        if (scan_attributes_raw(data, len, pos, &attr_start, &attr_len, &after_attributes) &&
            parse_attributes(mem, data + attr_start, attr_len, &parsed->attributes, &parsed->oom)) {
            parsed->has_attributes = 1;
            parsed->attr_start = attr_start;
            parsed->attr_len = attr_len;
            pos = after_attributes;
        } else if (parsed->oom) {
            return 0;
        }
    }

    parsed->end = pos;
    return 1;
}

static markdown_core_node *make_label_node(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    const unsigned char *label,
    markdown_core_bufsize label_len,
    int start_line,
    int start_column,
    int end_column
) {
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
    label_node->internal_offset = 1;
    label_node->flags |= MARKDOWN_CORE_NODE__OWNS_INLINE_SOURCE;
    return label_node;
}

static int attach_label_node(
    markdown_core_extension *extension,
    markdown_core_node *directive_node,
    const unsigned char *label,
    markdown_core_bufsize label_len,
    int start_line,
    int start_column,
    int end_column
) {
    markdown_core_node *label_node;

    label_node = make_label_node(
        extension,
        markdown_core_node_mem(directive_node),
        label,
        label_len,
        start_line,
        start_column,
        end_column
    );
    if (!label_node) {
        return 0;
    }

    if (!markdown_core_node_append_child(directive_node, label_node)) {
        markdown_core_node_free(label_node);
        return 0;
    }

    return 1;
}

static int apply_parsed_directive(
    markdown_core_extension *extension,
    markdown_core_node *node,
    const unsigned char *data,
    parsed_directive *parsed,
    int start_line,
    int start_column
) {
    node_directive *directive = get_directive(node);
    markdown_core_mem *mem = markdown_core_node_mem(node);

    if (!directive) {
        return 0;
    }

    if (!set_chunk_bytes(mem, &directive->name, data + parsed->name_start, parsed->name_len)) {
        return 0;
    }
    directive->has_attributes = parsed->has_attributes;

    if (parsed->has_attributes) {
        directive->attributes = parsed->attributes;
        parsed->attributes = NULL;
        clear_attribute_caches(node, directive);
    }

    if (parsed->has_label) {
        /* DirectiveLabel is a real source construct, so its scope includes
         * both brackets. internal_offset=1 keeps inline parsing anchored at
         * the first byte inside the opening bracket. */
        int label_start_column = start_column + (int)parsed->label_start;
        int label_end_column = label_start_column + (int)parsed->label_len + 1;
        if (!attach_label_node(
                extension,
                node,
                data + parsed->label_start,
                parsed->label_len,
                start_line,
                label_start_column,
                label_end_column
            )) {
            return 0;
        }
    }

    return 1;
}

static markdown_core_node *make_directive_node(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    const unsigned char *name,
    markdown_core_bufsize name_len,
    int start_line,
    int start_column,
    int end_line,
    int end_column
) {
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

static markdown_core_node *make_name_only_directive(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const unsigned char *name,
    markdown_core_bufsize name_len,
    markdown_core_bufsize end_offset
) {
    markdown_core_node *node;
    markdown_core_inline_source_span span;
    markdown_core_bufsize spelling_start = (markdown_core_bufsize)markdown_core_inline_parser_get_offset(inline_parser);

    node = make_directive_node(extension, parser, name, name_len, 0, 0, 0, 0);
    if (!node) {
        return NULL;
    }
    if (!markdown_core_inline_parser_consume_source(inline_parser, end_offset, &span)) {
        markdown_core_node_free(node);
        return NULL;
    }
    /* The `:name` spelling this consume just advanced past — the one
     * sanctioned capture outside the engine funnel; the colon that
     * triggered the match is an unconditional special char and therefore
     * already a seam barrier. */
    markdown_core_inline_parser_concrete_capture_spelling(
        inline_parser,
        MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME,
        spelling_start,
        end_offset
    );

    node->start_line = span.start_line;
    node->start_column = span.start_column;
    node->end_line = span.end_line;
    node->end_column = span.end_column;
    return node;
}

static markdown_core_node *match_directive_delimiter(
    markdown_core_inline_parser *inline_parser,
    markdown_core_bufsize end_offset
) {
    return markdown_core_inline_parser_consume_delimiter(inline_parser, DIRECTIVE_LABEL_RULE, 1, 0, end_offset);
}

static int scan_valid_attributes(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos,
    markdown_core_bufsize *end
) {
    markdown_core_bufsize attr_start;
    markdown_core_bufsize attr_len;
    return scan_attributes_raw(data, len, pos, &attr_start, &attr_len, end) &&
           scan_attribute_sequence(data + attr_start, attr_len, NULL, NULL, NULL);
}

static markdown_core_bufsize probe_label_close(
    uint16_t kind,
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize offset
) {
    markdown_core_bufsize end;

    if (kind != DIRECTIVE_LABEL_RULE || offset < 0 || offset >= len || data[offset] != ']') {
        return 0;
    }
    if (offset + 1 < len && data[offset + 1] == '{' && scan_valid_attributes(data, len, offset + 1, &end)) {
        return end - offset;
    }
    return 1;
}

static markdown_core_node *match_colon_directive(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_inline_parser *inline_parser,
    markdown_core_chunk *chunk,
    markdown_core_bufsize offset
) {
    markdown_core_bufsize name_start;
    markdown_core_bufsize name_len;
    markdown_core_bufsize pos;

    // A text directive's colon may not sit next to another colon on either
    // side. The trailing rule keeps `:red:` available to emoji; the leading one
    // keeps a run of colons whole, so `x ::a` is text rather than `x :` plus a
    // directive. `::name` and `:::name` at the start of a line are leaf and
    // container directives and are opened by the block path instead.
    if (offset + 1 >= chunk->len || chunk->data[offset + 1] == ':') {
        return NULL;
    }
    if (offset > 0 && chunk->data[offset - 1] == ':') {
        return NULL;
    }

    if (!scan_name(chunk->data, chunk->len, offset + 1, &name_start, &name_len)) {
        return NULL;
    }

    pos = name_start + name_len;
    if (pos < chunk->len && chunk->data[pos] == '[') {
        // The directive is emitted before the label opens, and the delimiter
        // marker is the bracket alone. A label that never closes then leaves
        // exactly what the reference implementation produces — the directive
        // standing on its name, followed by the literal `[` — with no
        // lookahead: an opener the engine never pairs discards only the
        // bracket.
        //
        // Scanning ahead for the closer instead would be quadratic on
        // `:x[:x[:x[...]]]`, where every opener's match sits at the far end.
        // The engine already pairs delimiters in linear time, so the fallback
        // has to be expressed in what it leaves behind rather than recomputed.
        markdown_core_node *directive =
            make_name_only_directive(extension, parser, inline_parser, chunk->data + name_start, name_len, pos);
        if (!directive) {
            return NULL;
        }
        markdown_core_node_append_child_unchecked(parent, directive);
        return match_directive_delimiter(inline_parser, pos + 1);
    }

    if (pos < chunk->len && chunk->data[pos] == '{') {
        markdown_core_bufsize attr_start;
        markdown_core_bufsize attr_len;
        markdown_core_bufsize end;
        markdown_core_node *node;
        markdown_core_inline_source_span span;
        directive_attribute *attributes = NULL;
        node_directive *directive;
        int attr_oom = 0;

        // An attribute block that does not parse leaves the directive standing
        // on its name alone and the braces as literal text, matching
        // micromark-extension-directive: `:invalid{=value}` is a directive
        // named `invalid` followed by the text `{=value}`. Allocation loss is
        // not a syntax verdict and still fails.
        if (!scan_attributes_raw(chunk->data, chunk->len, pos, &attr_start, &attr_len, &end) ||
            !parse_attributes(parser->mem, chunk->data + attr_start, attr_len, &attributes, &attr_oom)) {
            if (attr_oom) {
                parser->oom = true;
                return NULL;
            }
            return make_name_only_directive(extension, parser, inline_parser, chunk->data + name_start, name_len, pos);
        }
        node = make_directive_node(extension, parser, chunk->data + name_start, name_len, 0, 0, 0, 0);
        if (!node) {
            free_attribute_list(parser->mem, attributes);
            return NULL;
        }
        directive = get_directive(node);
        directive->attributes = attributes;
        directive->has_attributes = 1;
        if (!markdown_core_inline_parser_consume_source(inline_parser, end, &span)) {
            markdown_core_node_free(node);
            return NULL;
        }
        /* One consume, two spellings: the `:name` through the byte before
         * `{`, then the whole attribute block braces included. */
        markdown_core_inline_parser_concrete_capture_spelling(
            inline_parser,
            MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME,
            offset,
            pos
        );
        markdown_core_inline_parser_concrete_capture_spelling(
            inline_parser,
            MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_ATTRIBUTES,
            pos,
            end
        );
        node->start_line = span.start_line;
        node->start_column = span.start_column;
        node->end_line = span.end_line;
        node->end_column = span.end_column;
        return node;
    }

    if (pos < chunk->len && chunk->data[pos] == ':') {
        return NULL;
    }

    return make_name_only_directive(extension, parser, inline_parser, chunk->data + name_start, name_len, pos);
}

static markdown_core_node *match(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
) {
    if (!directive_enabled(parser) || character != ':') {
        return NULL;
    }

    return match_colon_directive(
        extension,
        parser,
        parent,
        inline_parser,
        markdown_core_inline_parser_get_chunk(inline_parser),
        (markdown_core_bufsize)markdown_core_inline_parser_get_offset(inline_parser)
    );
}

static markdown_core_bufsize count_colons(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos
) {
    markdown_core_bufsize count = 0;
    while (pos + count < len && data[pos + count] == ':') {
        count++;
    }
    return count;
}

static markdown_core_node *open_directive_block(
    markdown_core_extension *extension,
    int indented,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
) {
    markdown_core_bufsize first_nonspace = (markdown_core_bufsize)markdown_core_parser_get_first_nonspace(parser);
    markdown_core_bufsize colon_count;
    parsed_directive parsed;
    markdown_core_node *node;
    node_directive *directive;

    if (!directive_enabled(parser) || indented) {
        return NULL;
    }

    colon_count = count_colons(input, (markdown_core_bufsize)len, first_nonspace);
    if (colon_count < 2) {
        return NULL;
    }

    if (!parse_directive_suffix(
            parser->mem,
            input,
            (markdown_core_bufsize)len,
            first_nonspace + colon_count,
            &parsed
        )) {
        if (parsed.oom) {
            parser->oom = true;
        }
        return NULL;
    }

    if (!markdown_core_ext_has_only_spaces_until_line_end(input, (markdown_core_bufsize)len, parsed.end)) {
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    node = markdown_core_parser_add_child(
        parser,
        parent_container,
        MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
        (int)first_nonspace + 1
    );
    if (!node) {
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    markdown_core_node_set_extension(node, extension);
    node->as.opaque = parser->mem->calloc(parser->mem, 1, sizeof(node_directive));
    if (!node->as.opaque) {
        parser->oom = true;
        markdown_core_node_free(node);
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    if (!apply_parsed_directive(
            extension,
            node,
            input,
            &parsed,
            markdown_core_parser_get_line_number(parser),
            (int)first_nonspace
        )) {
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

    /* Concrete capture, in the open line's spelled order: the colon run,
     * the name, the label's two brackets (the label's interior is the
     * DirectiveLabel child's own region), and the attribute block braces
     * included. Attributes that failed to parse never get here — the
     * leftover braces fail the blank-rest-of-line check above. */
    markdown_core_parser_capture_marker(parser, node, MARKDOWN_CORE_CONCRETE_FENCE_OPEN, first_nonspace, colon_count);
    markdown_core_parser_capture_marker(
        parser,
        node,
        MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME,
        parsed.name_start,
        parsed.name_len
    );
    if (parsed.has_label) {
        markdown_core_parser_capture_marker(
            parser,
            node,
            MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_OPEN,
            parsed.label_start - 1,
            1
        );
        markdown_core_parser_capture_marker(
            parser,
            node,
            MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_CLOSE,
            parsed.label_start + parsed.label_len,
            1
        );
    }
    if (parsed.has_attributes) {
        markdown_core_parser_capture_marker(
            parser,
            node,
            MARKDOWN_CORE_CONCRETE_DIRECTIVE_ATTRIBUTES,
            parsed.attr_start - 1,
            parsed.attr_len + 2
        );
    }

    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);

    free_parsed_directive(parser->mem, &parsed);
    return node;
}

static int directive_block_matches(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    unsigned char *input,
    int len,
    markdown_core_node *container
) {
    node_directive *directive = get_directive(container);
    markdown_core_bufsize first_nonspace = (markdown_core_bufsize)markdown_core_parser_get_first_nonspace(parser);
    markdown_core_bufsize colon_count;

    if (!directive) {
        return 0;
    }

    if (directive->closed) {
        return 0;
    }

    directive->consume_line = 0;

    colon_count = count_colons(input, (markdown_core_bufsize)len, first_nonspace);
    if (markdown_core_parser_get_indent(parser) <= 3 && colon_count >= (markdown_core_bufsize)directive->fence_length &&
        markdown_core_ext_has_only_spaces_until_line_end(
            input,
            (markdown_core_bufsize)len,
            first_nonspace + colon_count
        )) {
        directive->closed = 1;
        directive->consume_line = 1;
        markdown_core_parser_capture_marker(
            parser,
            container,
            MARKDOWN_CORE_CONCRETE_FENCE_CLOSE,
            first_nonspace,
            colon_count
        );
        markdown_core_parser_advance_offset(
            parser,
            (char *)input,
            len - markdown_core_parser_get_offset(parser),
            false
        );
        /* The container ends here, on its own terminator. Reporting that
         * rather than "matched" is what closes the content inside it now: a
         * paragraph left open until the next non-matching line would take that
         * line as a lazy continuation, from outside the container. */
        return -1;
    }

    return 1;
}

static int set_attributes_from_wrapper(
    markdown_core_node *node,
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos,
    int *oom
) {
    markdown_core_bufsize attr_start;
    markdown_core_bufsize attr_len;
    markdown_core_bufsize end;
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

static markdown_core_node *make_empty_label_node(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    int start_line,
    int start_column,
    int end_line,
    int end_column
) {
    markdown_core_node *label =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, mem, extension);
    if (!label) {
        return NULL;
    }
    label->start_line = start_line;
    label->start_column = start_column;
    label->end_line = end_line;
    label->end_column = end_column;
    label->internal_offset = 1;
    return label;
}

static markdown_core_delimiter_result insert_directive(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    markdown_core_node *opener_node;
    markdown_core_node *closer_node;
    markdown_core_chunk *opener_literal;
    markdown_core_chunk *closer_literal;
    markdown_core_node *directive_node;
    markdown_core_node *label_node;
    markdown_core_node *tmp;
    markdown_core_node *tmpnext;
    markdown_core_bufsize name_len;

    if (match->kind != DIRECTIVE_LABEL_RULE) {
        assert(0 && "directive reducer received an unsupported delimiter rule");
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    opener_node = match->opener_node;
    closer_node = match->closer_node;
    if (!opener_node || !closer_node || opener_node->type != MARKDOWN_CORE_NODE_TEXT ||
        closer_node->type != MARKDOWN_CORE_NODE_TEXT) {
        assert(0 && "directive reducer received invalid delimiter endpoints");
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    opener_literal = &opener_node->as.literal;
    closer_literal = &closer_node->as.literal;

    /* The opener marker is the bracket alone; the directive it belongs to was
     * emitted just before it, so an unpaired opener leaves that directive
     * standing with a literal `[` after it. */
    directive_node = opener_node->prev;
    if (opener_literal->len != 1 || opener_literal->data[0] != '[' || closer_literal->len < 1 ||
        closer_literal->data[0] != ']' || !directive_node || directive_node->type != MARKDOWN_CORE_NODE_DIRECTIVE) {
        assert(0 && "directive reducer received malformed delimiter literals");
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    (void)name_len;
    markdown_core_node_unlink(directive_node);
    directive_node->end_line = closer_node->end_line;
    directive_node->end_column = closer_node->end_column;

    {
        int attr_oom = 0;
        if (closer_literal->len > 1 && closer_literal->data[1] == '{' &&
            !set_attributes_from_wrapper(directive_node, closer_literal->data, closer_literal->len, 1, &attr_oom)) {
            if (attr_oom) {
                markdown_core_node_free(directive_node);
                return MARKDOWN_CORE_DELIMITER_OOM;
            }
            markdown_core_node_free(directive_node);
            assert(0 && "directive close probe and reducer attribute grammars diverged");
            return MARKDOWN_CORE_DELIMITER_INVALID;
        }
    }

    label_node = make_empty_label_node(
        extension,
        parser->mem,
        opener_node->end_line,
        opener_node->end_column,
        closer_node->start_line,
        closer_node->start_column
    );
    if (!label_node) {
        markdown_core_node_free(directive_node);
        return MARKDOWN_CORE_DELIMITER_OOM;
    }

    /* Inline parsing has already materialized the label's Markup. Preserve
     * that exact child run under the canonical DirectiveLabel owner. */
    tmp = opener_node->next;
    while (tmp && tmp != closer_node) {
        tmpnext = tmp->next;
        markdown_core_node_append_child_unchecked(label_node, tmp);
        tmp = tmpnext;
    }
    markdown_core_node_append_child_unchecked(directive_node, label_node);
    markdown_core_node_insert_before_unchecked(opener_node, directive_node);
    markdown_core_node_free(opener_node);
    markdown_core_node_free(closer_node);
    /* A RANGE reduce reports its own consumption: the `[` opener and the
     * `]{...}` closer run are markup now. The label children and their
     * records stay — nothing to retract. */
    markdown_core_inline_parser_concrete_use_endpoints(inline_parser, match);
    return MARKDOWN_CORE_DELIMITER_OK;
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

static int can_contain(
    markdown_core_extension *extension,
    markdown_core_node *node,
    markdown_core_node_type child_type
) {
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

static markdown_core_node *prepare_inline_domain(
    markdown_core_extension *extension,
    const markdown_core_node *committed_owner
) {
    markdown_core_node *root;
    markdown_core_mem *mem;

    if (!committed_owner || committed_owner->type != MARKDOWN_CORE_NODE_DIRECTIVE_LABEL ||
        committed_owner->extension != extension) {
        assert(0 && "directive reparse requested for an unsupported owner");
        return NULL;
    }
    mem = committed_owner->content.mem;
    if (!mem) {
        assert(0 && "directive label has no content allocator");
        return NULL;
    }

    root = make_label_node(
        extension,
        mem,
        committed_owner->content.ptr,
        committed_owner->content.size,
        1,
        committed_owner->start_column,
        committed_owner->end_column
    );
    if (!root) {
        return NULL;
    }
    return root;
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

static const markdown_core_delimiter_rule directive_delimiter_rules[] = {
    {
        .pairing = MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        .reduction = MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        .close_trigger = ']',
        .close_probe = probe_label_close,
    },
};

// The close trigger is dispatched by the delimiter engine; match() only scans
// directive openers and standalone directives.
static const unsigned char directive_special_chars[] = {':'};

static const markdown_core_extension directive_extension = {
    .name = "directive",
    .match_inline = match,
    .insert_inline_from_delim = insert_directive,
    .delimiter_rules = directive_delimiter_rules,
    .delimiter_rule_count = sizeof(directive_delimiter_rules) / sizeof(directive_delimiter_rules[0]),
    .last_block_matches = directive_block_matches,
    .try_opening_block = open_directive_block,
    .get_type_string = get_type_string,
    .can_contain = can_contain,
    .prepare_inline_domain = prepare_inline_domain,
    .accepts_lines = accepts_lines,
    .alloc_opaque = directive_opaque_alloc,
    .free_opaque = directive_opaque_free,
    .special_inline_chars = directive_special_chars,
    .special_inline_char_count = sizeof(directive_special_chars),
};

markdown_core_extension *markdown_core_directive_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&directive_extension;
}
