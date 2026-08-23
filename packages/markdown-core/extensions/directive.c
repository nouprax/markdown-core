#include "directive.h"
#include "syntax_extension.h"

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
#include <houdini.h>
#include <utf8.h>

#include "ext_scanners.h"

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

static int ascii_is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int ascii_is_line_space(unsigned char c) { return c == ' ' || c == '\t'; }

static int is_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    return pos >= len || data[pos] == '\n' || data[pos] == '\r';
}

static int has_only_spaces_until_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    while (pos < len && ascii_is_line_space(data[pos])) {
        pos++;
    }

    return is_line_end(data, len, pos);
}

/* THE GRAMMAR IS APPLIED TO CODE POINTS, not bytes (Q8's oracle says so, and
 * `{中文=1}` is the row that proves it). An attribute name may not BEGIN with
 * punctuation -- a symbol counts, which is why `{a$b}` is malformed and `{:_a}`
 * is too -- and `markdown_core_utf8proc_is_punctuation` is already the
 * engine's answer to "is this code point punctuation", so there is no second
 * table here. From the second code point on, four punctuation marks are
 * ordinary name characters, which is why `{a:b}` is one name and
 * `{data-kind=ref}` is another. */
static int name_cp(int32_t cp) {
    return cp > 0x20 && !markdown_core_utf8proc_is_space(cp) && !markdown_core_utf8proc_is_punctuation(cp);
}

static int attr_name_start_cp(int32_t cp) { return name_cp(cp) || cp == '-' || cp == '_'; }

static int attr_name_cp(int32_t cp) { return attr_name_start_cp(cp) || cp == '.' || cp == ':'; }

/* Reads one code point at `pos`. A byte that is not valid UTF-8 decodes as
 * itself with length 1, which keeps a malformed name malformed rather than
 * ending the scan early on a continuation byte. */
static bufsize_t next_cp(const unsigned char *data, bufsize_t len, bufsize_t pos, int32_t *cp) {
    int consumed = markdown_core_utf8proc_iterate(data + pos, len - pos, cp);
    if (consumed < 1) {
        *cp = data[pos];
        return 1;
    }
    return (bufsize_t)consumed;
}

/* A DIRECTIVE NAME IS CODE POINTS, not bytes -- the same sentence the
 * attribute grammar is written to, and the reason `scan_directive_name` is no
 * longer called. That generated scanner is `[A-Za-z0-9_-]+`, so `:café` was a
 * directive named `caf` followed by the text `é` and `::café` was not a
 * directive at all, while micromark reads both names whole. Its rule is one
 * line: a name character is any code point that is not whitespace and not
 * punctuation, plus `-` and `_` from the second on -- which is `name_cp` and
 * `attr_name_start_cp`, already here for attribute names.
 *
 * A name may CONTAIN a hyphen or underscore but may not begin or end with one.
 * The generated scanner had the trailing half nowhere and never had the
 * leading half: `:-a[]` and `:_a[]` were directives named `-a` and `_a`. */
static int scan_name(unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *name_start, bufsize_t *name_len) {
    bufsize_t start = pos;
    int32_t cp;
    bufsize_t width;

    if (pos >= len) {
        return 0;
    }
    width = next_cp(data, len, pos, &cp);
    if (!name_cp(cp)) {
        return 0;
    }
    pos += width;
    while (pos < len) {
        width = next_cp(data, len, pos, &cp);
        if (!attr_name_start_cp(cp)) {
            break;
        }
        pos += width;
    }
    if (data[pos - 1] == '-' || data[pos - 1] == '_') {
        return 0;
    }

    *name_start = start;
    *name_len = pos - start;
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

        /* micromark's factory-label.js counts the INNER brackets and fails at
         * the 33rd (`if (code === 91 && ++balance > 32)`), so 32 nested labels
         * are a label and 33 are prose. `depth` here starts at 1 for the
         * label's own `[`, which is the off-by-one this bound accounts for. */
        if (data[i] == '[') {
            if (++depth > 33) {
                return 0;
            }
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

/* Scans an attribute name from `pos`. */
static bufsize_t scan_attr_name(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    bufsize_t start = pos;
    int first = 1;
    while (pos < len) {
        int32_t cp;
        bufsize_t width = next_cp(data, len, pos, &cp);
        if (first ? !attr_name_start_cp(cp) : !attr_name_cp(cp)) {
            break;
        }
        first = 0;
        pos += width;
    }
    return pos - start;
}

/* A SHORTHAND VALUE IS NOT A NAME, and reusing the name scanner for it was
 * wrong: `{.a&b}` is one class called `a&b`. The value ENDS at the next marker
 * or the block's end -- which is what makes `{.a.b}` two classes rather than
 * one called `a.b` -- and six characters REJECT it outright, taking the whole
 * block down with them, because a quote or an `=` there means the source meant
 * something else. Everything not in either set is a value character.
 *
 * Returns the length, or -1 for a malformed block. An empty value is malformed
 * too, which is `{#}` and `{.}`. */
static int shorthand_value_rejects(unsigned char c) {
    return c == '"' || c == '\'' || c == '<' || c == '=' || c == '>' || c == '`';
}

static int shorthand_value_ends(unsigned char c) { return c == '#' || c == '.' || c == '}' || ascii_is_space(c); }

static bufsize_t scan_shorthand_value(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    bufsize_t start = pos;
    while (pos < len && !shorthand_value_ends(data[pos])) {
        if (shorthand_value_rejects(data[pos])) {
            return -1;
        }
        pos++;
    }
    return pos == start ? -1 : pos - start;
}

/* The public setter reaches append_attribute with a name nobody scanned, so
 * the scanner's own rule is re-applied here rather than restated: a name is
 * valid exactly when scanning it consumes all of it. */
static int attribute_name_is_valid(const unsigned char *name, bufsize_t name_len) {
    return name_len > 0 && scan_attr_name(name, name_len, 0) == name_len;
}

static int append_attribute(markdown_core_mem *mem, directive_attribute **head, directive_attribute **tail,
                            const unsigned char *name, bufsize_t name_len, const unsigned char *value,
                            bufsize_t value_len, size_t index, int *oom) {
    directive_attribute *attr;
    markdown_core_strbuf decoded;
    int stored;
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
    /* Q20: A VALUE IS DECODED, A NAME IS NOT. mdast-util-directive decodes
     * exactly three things -- an attribute's value and the `#id` and `.class`
     * shorthand values -- and this is where all three arrive. The decoder is
     * the engine's one entity decoder, the same call a link destination, a
     * link title and a code fence's info string already make; the alternative
     * was a second entity rule living in this file (§4.14.7c).
     *
     * It runs AFTER the raw scan, never during it, which is what makes
     * `{a=x&#125;y}` one attribute whose value contains a `}` rather than a
     * block that ended early. */
    markdown_core_strbuf_init(mem, &decoded, value_len + 1);
    houdini_unescape_html_f(&decoded, value, value_len);
    stored = !decoded.oom && replace_chunk_bytes(mem, &attr->name, name, name_len) &&
             replace_chunk_bytes(mem, &attr->value, decoded.ptr, decoded.size);
    markdown_core_strbuf_free(&decoded);
    if (!stored) {
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

/* `class` is the ONE name whose repeats accumulate, in source order, whether
 * they were written as `.x` or as `class=x`. Every other name keeps the last
 * value, which is what normalize_duplicate_attributes does -- so the folding
 * happens first and leaves at most one `class` behind for it to find.
 *
 * The separator goes before every value once something has accumulated, an
 * empty one included: `{.a class="" .b}` is `class="a  b"`. An empty value at
 * the FRONT accumulates nothing, so it contributes no leading separator and
 * `{class="" .b}` is `class="b"`. Both are oracle rows; the rule is "join what
 * is there", not "join what was written". */
static int accumulate_class_attributes(markdown_core_mem *mem, directive_attribute *head, size_t *count) {
    directive_attribute *first = NULL;
    directive_attribute *attr;
    markdown_core_strbuf joined;
    int changed = 0;

    for (attr = head; attr; attr = attr->next) {
        if (attr->name.len != 5 || memcmp(attr->name.data, "class", 5) != 0) {
            continue;
        }
        if (!first) {
            first = attr;
            continue;
        }
        changed = 1;
    }
    if (!changed) {
        return 1;
    }

    markdown_core_strbuf_init(mem, &joined, 32);
    for (attr = first; attr; attr = attr->next) {
        if (attr->name.len != 5 || memcmp(attr->name.data, "class", 5) != 0) {
            continue;
        }
        if (joined.size > 0) {
            markdown_core_strbuf_putc(&joined, ' ');
        }
        markdown_core_strbuf_put(&joined, attr->value.data, attr->value.len);
    }
    if (joined.oom || !replace_chunk_bytes(mem, &first->value, joined.ptr, joined.size)) {
        markdown_core_strbuf_free(&joined);
        return 0;
    }
    markdown_core_strbuf_free(&joined);

    /* The folded ones are UNLINKED, not merely deactivated. The duplicate
     * normalizer that runs next walks the whole list and swaps the first
     * occurrence's value for the last one's; a `class` it can still see would
     * undo the accumulation the moment there were two of them. */
    for (attr = first; attr && attr->next;) {
        directive_attribute *dup = attr->next;
        if (dup->name.len == 5 && memcmp(dup->name.data, "class", 5) == 0) {
            attr->next = dup->next;
            dup->next = NULL;
            free_attribute_list(mem, dup);
            (*count)--;
            continue;
        }
        attr = dup;
    }
    return 1;
}

/* Q19: THE MODEL IS SORTED, and by name. Two orders is how a third order
 * appears in a binding -- after class-accumulation and last-value-wins the
 * list IS a map, and a map has no source order to preserve. remark's own
 * projection is sorted too, which is what the mdast oracle compares against.
 *
 * A LINKED-LIST MERGE SORT, not an array and qsort: the duplicate normalizer
 * above already has a no-allocation fallback for the case where the index
 * cannot be built, and a sort that could fail to allocate would put an
 * unsorted list back on the node with nothing to say so. This one cannot
 * fail. */
static int attribute_name_before(const directive_attribute *a, const directive_attribute *b) {
    bufsize_t common = a->name.len < b->name.len ? a->name.len : b->name.len;
    int cmp = memcmp(a->name.data, b->name.data, (size_t)common);
    if (cmp) {
        return cmp < 0;
    }
    return a->name.len <= b->name.len;
}

static directive_attribute *merge_attribute_runs(directive_attribute *left, directive_attribute *right) {
    directive_attribute *head = NULL;
    directive_attribute **tail = &head;
    while (left && right) {
        directive_attribute **from = attribute_name_before(left, right) ? &left : &right;
        *tail = *from;
        *from = (*from)->next;
        tail = &(*tail)->next;
    }
    *tail = left ? left : right;
    return head;
}

static directive_attribute *sort_attributes_by_name(directive_attribute *head) {
    directive_attribute *rest = head;
    directive_attribute *runs[32];
    size_t i;
    size_t used = 0;

    while (rest) {
        directive_attribute *one = rest;
        rest = rest->next;
        one->next = NULL;
        for (i = 0; i < used && runs[i]; i++) {
            one = merge_attribute_runs(runs[i], one);
            runs[i] = NULL;
        }
        if (i == sizeof(runs) / sizeof(runs[0])) {
            /* 2^32 attributes in one `{...}` is not reachable in a bufsize_t
             * document; merging into the last slot keeps the sort total even
             * if it ever were. */
            i--;
            one = merge_attribute_runs(runs[i], one);
        }
        runs[i] = one;
        if (i == used) {
            used++;
        }
    }
    head = NULL;
    for (i = 0; i < used; i++) {
        head = merge_attribute_runs(runs[i], head);
    }
    return head;
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

int markdown_core_extensions_directive_has_attributes(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    return directive && directive->has_attributes;
}

/* The list is walked rather than indexed. A directive's attribute count is the
 * size of one `{...}`, so the walk is short and a second array to index into
 * would be a third representation of the same list -- which is what the JSON
 * cache was. */
static directive_attribute *attribute_at(node_directive *directive, size_t index) {
    directive_attribute *attr;
    for (attr = directive ? directive->attributes : NULL; attr; attr = attr->next) {
        if (!attr->active) {
            continue;
        }
        if (index == 0) {
            return attr;
        }
        index--;
    }
    return NULL;
}

size_t markdown_core_extensions_directive_attribute_count(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    directive_attribute *attr;
    size_t count = 0;
    for (attr = directive ? directive->attributes : NULL; attr; attr = attr->next) {
        if (attr->active) {
            count++;
        }
    }
    return count;
}

int markdown_core_extensions_directive_attribute_at(markdown_core_node *node, size_t index, const char **name,
                                                    size_t *name_length, const char **value, size_t *value_length) {
    directive_attribute *attr = attribute_at(get_directive(node), index);
    if (!attr || !name || !name_length || !value || !value_length) {
        return 0;
    }
    *name = (const char *)attr->name.data;
    *name_length = (size_t)attr->name.len;
    *value = (const char *)attr->value.data;
    *value_length = (size_t)attr->value.len;
    return 1;
}

static void directive_opaque_alloc(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
                                   markdown_core_node *node) {
    /* A NULL payload is tolerated: every accessor goes through get_directive
     * and treats the node as attribute-less. */
    if (is_directive_node(node)) {
        node->as.opaque = mem->calloc(1, sizeof(node_directive));
    }
}

static void directive_opaque_free(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
                                  markdown_core_node *node) {
    node_directive *directive = (node_directive *)node->as.opaque;
    if (!directive) {
        return;
    }

    markdown_core_chunk_free(mem, &directive->name);
    free_attribute_list(mem, directive->attributes);
    mem->free(directive);
}

/* AN `=` PROMISES A VALUE. With none before the block ends the block is
 * malformed, because an empty value would otherwise be indistinguishable from
 * the valueless `{a}`, which means something else. `{a=}`, `{a= }` and an `=`
 * followed only by a line ending are all the same case.
 *
 * An unquoted value may not contain `<`, `>`, `=` or a backtick: reaching one
 * does not end the value, it makes the block malformed. A quoted value must be
 * separated from whatever follows by whitespace or the closing brace --
 * without that rule `{a="x"b=1}` lets the block run on and swallow the rest of
 * the paragraph. */
static int unquoted_value_cp(unsigned char c) {
    return !ascii_is_space(c) && c != '"' && c != '\'' && c != '<' && c != '>' && c != '=' && c != '`';
}

static int parse_attr_value(const unsigned char *data, bufsize_t len, bufsize_t *pos, const unsigned char **value,
                            bufsize_t *value_len) {
    bufsize_t start;
    unsigned char quote;
    while (*pos < len && ascii_is_space(data[*pos])) {
        (*pos)++;
    }
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
        if (*pos < len && !ascii_is_space(data[*pos])) {
            return 0;
        }
        return 1;
    }
    start = *pos;
    while (*pos < len && unquoted_value_cp(data[*pos])) {
        (*pos)++;
    }
    if (*pos < len && !ascii_is_space(data[*pos])) {
        return 0;
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
        /* SHORTHAND. `#x` and `.x` are the `id` and `class` attributes
         * written short. A marker with nothing after it is not an attribute
         * and takes the whole block down with it, which is `{#}` and `{.}`. */
        if (data[pos] == '#' || data[pos] == '.') {
            const char *shorthand_name = data[pos] == '#' ? "id" : "class";
            bufsize_t shorthand_len;
            pos++;
            shorthand_len = scan_shorthand_value(data, len, pos);
            if (shorthand_len < 0) {
                ok = 0;
                break;
            }
            if (!append_attribute(mem, &attrs, &tail, (const unsigned char *)shorthand_name,
                                  (bufsize_t)strlen(shorthand_name), data + pos, shorthand_len, count++, oom)) {
                ok = 0;
                break;
            }
            pos += shorthand_len;
            continue;
        }
        start = pos;
        name_len = scan_attr_name(data, len, pos);
        pos += name_len;
        if (name_len == 0) {
            ok = 0;
            break;
        }
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

    if (ok && !accumulate_class_attributes(mem, attrs, &count)) {
        if (oom) {
            *oom = 1;
        }
        ok = 0;
    }
    if (ok && !normalize_duplicate_attributes(mem, attrs, count)) {
        if (oom) {
            *oom = 1;
        }
        ok = 0;
    }
    if (ok) {
        *result = sort_attributes_by_name(attrs);
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

static markdown_core_node *make_label_node(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
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
    /* The scope starts ON the `[` and the content starts after it. This is
     * what `internal_offset` is for -- a heading's `#` and a table cell's
     * leading pipe use the same field -- and it is why making the scope
     * bracket-inclusive did not move the label's own children. */
    label_node->internal_offset = 1;
    return label_node;
}

static int attach_label_node(const markdown_core_syntax_extension *extension, markdown_core_node *directive_node,
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

static int apply_parsed_directive(const markdown_core_syntax_extension *extension, markdown_core_node *node,
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
    }

    if (parsed->has_label) {
        /* THE LABEL'S SCOPE SPANS ITS BRACKETS. It used to span the content
         * only, which made `[]` a NEGATIVE range -- end one column before
         * start -- because there was no content to point at. A label always
         * has its two brackets, so the bracket-inclusive range is a place for
         * every label there is, and `:red[]:` reads `1:5..1:6`. */
        int label_start_column = start_column + (int)parsed->label_start;
        int label_end_column = label_start_column + (int)parsed->label_len + 1;

        if (!attach_label_node(extension, node, data + parsed->label_start, parsed->label_len, start_line,
                               label_start_column, label_end_column)) {
            return 0;
        }
    }

    return 1;
}

static markdown_core_node *make_directive_node(const markdown_core_syntax_extension *extension,
                                               markdown_core_parser *parser, const unsigned char *name,
                                               bufsize_t name_len, int start_line, int start_column, int end_line,
                                               int end_column) {
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

/* THE WHOLE CONSTRUCT IS SCANNED AT THE COLON, and that is the difference
 * between this and what was here before.
 *
 * `:name[label]{attrs}` used to be started by PUSHING A DELIMITER for `:name[`
 * and betting that a `]` would turn up later to pair with it. Two things
 * follow from that bet and both are wrong. When no `]` arrives the directive
 * is lost -- `:a[b` was one Text node where micromark gives a directive named
 * `a` and the prose `[b`. And while the bet is open the label has no boundary,
 * so whatever reaches the `]` first takes the directive with it: a GFM
 * autolink literal, an emphasis run, a code span (D36).
 *
 * micromark decides at the colon instead, with
 * `effects.attempt(label, afterLabel, afterLabel)` -- it SCANS the label, and
 * BOTH branches continue. A label that closes is a label; one that does not is
 * prose, and the directive stands either way. That is the same shape 7.1 gave
 * the attribute block, and it is what this does.
 *
 * Scanning it here also means the bytes are CONSUMED here, so no other
 * extension is ever offered them. There is nothing left to protect. */
static markdown_core_node *match_colon_directive(const markdown_core_syntax_extension *extension,
                                                 markdown_core_parser *parser, markdown_core_node *parent,
                                                 markdown_core_inline_parser *inline_parser, markdown_core_chunk *chunk,
                                                 bufsize_t offset) {
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t pos;
    bufsize_t label_start = 0;
    bufsize_t label_len = 0;
    bufsize_t label_open = 0;
    int has_label = 0;
    directive_attribute *attributes = NULL;
    int has_attributes = 0;
    markdown_core_node *node;
    markdown_core_node *label_node = NULL;
    node_directive *directive;
    int start_line = markdown_core_inline_parser_get_line(inline_parser);
    int start_column = markdown_core_inline_parser_get_column(inline_parser);

    /* A TEXT DIRECTIVE'S COLON MAY NOT SIT NEXT TO ANOTHER COLON, on either
     * side. The trailing half keeps `:red:` available to emoji; the leading
     * half keeps a run of colons whole, so `x ::a y` is text rather than
     * `x :` plus a directive named `a`, and `x:::a` is text rather than `x::`
     * plus one. `::name` and `:::name` at the start of a line are leaf and
     * container directives and open through the block path, not this one. */
    if (offset > 0 && chunk->data[offset - 1] == ':') {
        return NULL;
    }

    if (offset + 1 >= chunk->len || chunk->data[offset + 1] == ':') {
        return NULL;
    }

    if (!scan_name(chunk->data, chunk->len, offset + 1, &name_start, &name_len)) {
        return NULL;
    }

    pos = name_start + name_len;
    if (pos < chunk->len && chunk->data[pos] == ':') {
        return NULL;
    }

    if (pos < chunk->len && chunk->data[pos] == '[') {
        bufsize_t label_end;
        if (scan_label(chunk->data, chunk->len, pos, &label_start, &label_len, &label_end)) {
            has_label = 1;
            label_open = pos;
            pos = label_end;
        }
    }

    if (pos < chunk->len && chunk->data[pos] == '{') {
        bufsize_t attr_start;
        bufsize_t attr_len;
        bufsize_t attr_end;
        int attr_oom = 0;
        if (scan_attributes_raw(chunk->data, chunk->len, pos, &attr_start, &attr_len, &attr_end) &&
            parse_attributes(parser->mem, chunk->data + attr_start, attr_len, &attributes, &attr_oom)) {
            has_attributes = 1;
            pos = attr_end;
        } else if (attr_oom) {
            parser->oom = true;
            return NULL;
        }
        /* else the braces are prose; the directive stands (7.1's rule). */
    }

    node = make_directive_node(extension, parser, chunk->data + name_start, name_len, start_line, start_column,
                               start_line, start_column);
    if (!node) {
        free_attribute_list(parser->mem, attributes);
        parser->oom = true;
        return NULL;
    }
    directive = get_directive(node);
    directive->attributes = attributes;
    directive->has_attributes = has_attributes;
    directive->has_label = has_label;

    if (has_label) {
        /* Consume to the `]` first and read the label's end back from the
         * subject, because a label may span a line ending and a column
         * computed from the start plus a length states it in the wrong line's
         * frame -- 0a.10's rule, and the reason D22 was a defect. */
        int label_line = start_line;
        int label_column = start_column + (int)(label_open - offset);
        markdown_core_inline_parser_set_offset(inline_parser, (int)(label_start + label_len + 1));
        label_node = make_label_node(extension, parser->mem, chunk->data + label_start, label_len, label_line,
                                     label_column, markdown_core_inline_parser_get_column(inline_parser) - 1);
        if (!label_node) {
            markdown_core_node_free(node);
            parser->oom = true;
            return NULL;
        }
        label_node->end_line = markdown_core_inline_parser_get_line(inline_parser);
        if (!markdown_core_node_append_child(node, label_node)) {
            markdown_core_node_free(label_node);
            markdown_core_node_free(node);
            parser->oom = true;
            return NULL;
        }
    }

    markdown_core_inline_parser_set_offset(inline_parser, (int)pos);
    node->end_line = markdown_core_inline_parser_get_line(inline_parser);
    node->end_column = markdown_core_inline_parser_get_column(inline_parser) - 1;

    /* The label's content is its own buffer, so its inlines are parsed against
     * it and not against the paragraph that contains it -- the same shape a
     * table cell has, and the same one the BLOCK forms of this directive have
     * always had. `process_inlines`' walk cannot reach a node created during a
     * paragraph's own inline pass, so the parse is driven from here. */
    if (label_node) {
        /* The parent link is set HERE and not left to `parse_inline`'s
         * `append_child`, because requirement 11b's refinement walks a block's
         * ancestors to find the region its content was cut from -- and without
         * this the label's chain stops at a directive that is still a return
         * value, so every node inside a label would own no region at all. The
         * link is the one `append_child` writes a moment later. */
        node->parent = parent;
        markdown_core_parse_inlines(parser, label_node, parser->refmap, parser->options);
        /* The label's scope spans its brackets, so the brackets are the
         * label's markers (requirement 11b). They are claimed from HERE, in
         * the enclosing paragraph's claim run, because they are not part of
         * the label's own content buffer -- the label was made from what is
         * between them. */
        markdown_core_parser_claim_inline(parser, label_node, label_start - 1, label_start,
                                          MARKDOWN_CORE_REGION_ROLE_MARKER);
        markdown_core_parser_claim_inline(parser, label_node, label_start + label_len, label_start + label_len + 1,
                                          MARKDOWN_CORE_REGION_ROLE_MARKER);
    }
    return node;
}

/* ONE BYTE, not two. `]` was claimed because a label's closer had to be
 * recognised as a delimiter to pair with the opener; the label is scanned at
 * the colon now, so the bracket is nobody's business but the core's -- which
 * is what makes `[a](b)` inside a label work like any other link. */
static markdown_core_node *match(const markdown_core_syntax_extension *extension, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    bufsize_t offset = (bufsize_t)markdown_core_inline_parser_get_offset(inline_parser);

    if (character == ':') {
        return match_colon_directive(extension, parser, parent, inline_parser, chunk, offset);
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

static markdown_core_node *open_directive_block(const markdown_core_syntax_extension *extension, int indented,
                                                markdown_core_parser *parser, markdown_core_node *parent_container,
                                                unsigned char *input, int len) {
    bufsize_t first_nonspace = (bufsize_t)markdown_core_parser_get_first_nonspace(parser);
    bufsize_t colon_count;
    parsed_directive parsed;
    markdown_core_node *node;
    node_directive *directive;

    if (indented) {
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

    markdown_core_node_set_syntax_extension(node, extension);
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

static int directive_block_matches(const markdown_core_syntax_extension *extension, markdown_core_parser *parser,
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
        /* Returning 1 here used to leave the container open. The fence was
         * consumed, so nothing else on the line was parsed, but the block
         * stayed open and the next non-blank line arrived as a lazy paragraph
         * continuation -- pulled inside the container and recorded on the
         * fence's line rather than its own. */
        return MARKDOWN_CORE_BLOCK_CLOSED;
    }

    return 1;
}

static const char *get_type_string(const markdown_core_syntax_extension *extension, markdown_core_node *node) {
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

static int can_contain(const markdown_core_syntax_extension *extension, markdown_core_node *node,
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

static int contains_inlines(const markdown_core_syntax_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
}

static int accepts_lines(const markdown_core_syntax_extension *extension, markdown_core_node *node) {
    node_directive *directive = get_directive(node);

    if (!directive) {
        return 0;
    }

    if (node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return 0;
    }

    return directive->fence_length == 2 || directive->consume_line;
}

/* `:` opens a directive. `]` is in the dispatch set for the `]` arbitration
 * `bracket_takes_close_bracket` performs, not because it terminates a text run --
 * `is_core_special_character` refuses it there. */
const markdown_core_syntax_extension MARKDOWN_CORE_EXTENSION_DIRECTIVE = {
    .name = "directive",
    .match_inline = match,
    .last_block_matches = directive_block_matches,
    .try_opening_block = open_directive_block,
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .contains_inlines_func = contains_inlines,
    .accepts_lines_func = accepts_lines,
    .opaque_alloc_func = directive_opaque_alloc,
    .opaque_free_func = directive_opaque_free,
    .terminates_text = ":",
    .dispatch = ":",
};
