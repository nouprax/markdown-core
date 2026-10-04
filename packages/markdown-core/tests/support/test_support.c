#include "test_support.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* File IO -------------------------------------------------------------- */

uint8_t *ts_read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *bytes;
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (uint8_t *)malloc((size_t)size + 1);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    *length = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (*length != (size_t)size) {
        free(bytes);
        return NULL;
    }
    bytes[*length] = 0;
    return bytes;
}

/* Growable text buffer --------------------------------------------------- */

typedef struct ts_buffer {
    char *data;
    size_t length;
    size_t capacity;
} ts_buffer;

static int ts_buffer_append(ts_buffer *buffer, const char *text, size_t length) {
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        char *grown;
        while (capacity < buffer->length + length + 1) {
            capacity *= 2;
        }
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown) {
            return -1;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = 0;
    return 0;
}

/* Replaces the U+2192 arrow used by spec fixtures with a real tab. */
static char *ts_replace_arrows(const char *text, size_t length, size_t *out_length) {
    ts_buffer buffer = {NULL, 0, 0};
    size_t i = 0;
    while (i < length) {
        if (i + 2 < length && (unsigned char)text[i] == 0xE2 && (unsigned char)text[i + 1] == 0x86 &&
            (unsigned char)text[i + 2] == 0x92) {
            if (ts_buffer_append(&buffer, "\t", 1) != 0) {
                goto fail;
            }
            i += 3;
        } else {
            if (ts_buffer_append(&buffer, text + i, 1) != 0) {
                goto fail;
            }
            i += 1;
        }
    }
    if (!buffer.data && ts_buffer_append(&buffer, "", 0) != 0) {
        goto fail;
    }
    if (out_length) {
        *out_length = buffer.length;
    }
    return buffer.data;
fail:
    free(buffer.data);
    return NULL;
}

/* Spec fixtures ---------------------------------------------------------- */

static const char TS_EXAMPLE_FENCE[] = "````````````````````````````````"; /* 32 backticks */

static int ts_case_push_element(ts_spec_case *test_case, const char *name, size_t length) {
    char *copy;
    if (test_case->tag_count >= TS_MAX_TAGS) {
        return -1;
    }
    copy = (char *)malloc(length + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, name, length);
    copy[length] = 0;
    test_case->tags[test_case->tag_count++] = copy;
    return 0;
}

static void ts_case_free(ts_spec_case *test_case) {
    size_t i;
    free(test_case->markdown);
    free(test_case->expected);
    free(test_case->section);
    for (i = 0; i < test_case->tag_count; i++) {
        free(test_case->tags[i]);
    }
}

int ts_spec_load(const char *path, ts_spec_file *out) {
    size_t length = 0;
    uint8_t *bytes = ts_read_file(path, &length);
    char *text = (char *)bytes;
    size_t line_start = 0;
    int line_number = 0;
    int state = 0; /* 0 prose, 1 markdown, 2 expected output */
    int example_number = 0;
    int start_line = 0;
    int disabled = 0;
    ts_buffer markdown = {NULL, 0, 0};
    ts_buffer expected = {NULL, 0, 0};
    char section[256] = "";
    ts_spec_case pending;
    size_t capacity = 0;

    memset(&pending, 0, sizeof(pending));
    out->cases = NULL;
    out->count = 0;
    if (!bytes) {
        return -1;
    }

    while (line_start <= length) {
        size_t line_end = line_start;
        size_t content_end;
        const char *line = text + line_start;
        size_t line_length;
        while (line_end < length && text[line_end] != '\n') {
            line_end++;
        }
        if (line_start == length && line_end == length && line_start != 0 && text[line_start - 1] == '\n') {
            break;
        }
        line_number++;
        content_end = line_end;
        while (content_end > line_start &&
               (text[content_end - 1] == '\r' || text[content_end - 1] == ' ' || text[content_end - 1] == '\t')) {
            content_end--;
        }
        line_length = content_end - line_start;

        if (line_length >= 40 && strncmp(line, TS_EXAMPLE_FENCE, 32) == 0 && strncmp(line + 32, " example", 8) == 0) {
            const char *cursor = line + 40;
            const char *end = line + line_length;
            state = 1;
            start_line = line_number;
            disabled = 0;
            ts_case_free(&pending);
            memset(&pending, 0, sizeof(pending));
            while (cursor < end) {
                const char *word_start;
                while (cursor < end && *cursor == ' ') {
                    cursor++;
                }
                word_start = cursor;
                while (cursor < end && *cursor != ' ') {
                    cursor++;
                }
                if (cursor > word_start) {
                    if (cursor - word_start == 8 && strncmp(word_start, "disabled", 8) == 0) {
                        disabled = 1;
                    } else if (ts_case_push_element(&pending, word_start, (size_t)(cursor - word_start)) != 0) {
                        goto fail;
                    }
                }
            }
        } else if (line_length == 32 && strncmp(line, TS_EXAMPLE_FENCE, 32) == 0 && state != 0) {
            example_number++;
            if (!disabled) {
                ts_spec_case finished = pending;
                size_t markdown_length = 0;
                size_t expected_length = 0;
                memset(&pending, 0, sizeof(pending));
                finished.markdown =
                    ts_replace_arrows(markdown.data ? markdown.data : "", markdown.length, &markdown_length);
                finished.markdown_length = markdown_length;
                finished.expected =
                    ts_replace_arrows(expected.data ? expected.data : "", expected.length, &expected_length);
                finished.section = (char *)malloc(strlen(section) + 1);
                if (!finished.markdown || !finished.expected || !finished.section) {
                    ts_case_free(&finished);
                    goto fail;
                }
                strcpy(finished.section, section);
                finished.example = example_number;
                finished.start_line = start_line;
                finished.end_line = line_number;
                if (out->count == capacity) {
                    size_t grown = capacity ? capacity * 2 : 64;
                    ts_spec_case *cases = (ts_spec_case *)realloc(out->cases, grown * sizeof(*cases));
                    if (!cases) {
                        ts_case_free(&finished);
                        goto fail;
                    }
                    out->cases = cases;
                    capacity = grown;
                }
                out->cases[out->count++] = finished;
            } else {
                ts_case_free(&pending);
                memset(&pending, 0, sizeof(pending));
            }
            markdown.length = 0;
            if (markdown.data) {
                markdown.data[0] = 0;
            }
            expected.length = 0;
            if (expected.data) {
                expected.data[0] = 0;
            }
            state = 0;
        } else if (state != 0 && line_length == 1 && line[0] == '.') {
            state = 2;
        } else if (state == 1) {
            if (ts_buffer_append(&markdown, line, line_end - line_start) != 0 ||
                ts_buffer_append(&markdown, "\n", 1) != 0) {
                goto fail;
            }
        } else if (state == 2) {
            if (ts_buffer_append(&expected, line, line_end - line_start) != 0 ||
                ts_buffer_append(&expected, "\n", 1) != 0) {
                goto fail;
            }
        } else if (state == 0 && line_length > 0 && line[0] == '#') {
            const char *cursor = line;
            const char *end = line + line_length;
            size_t copy_length;
            while (cursor < end && *cursor == '#') {
                cursor++;
            }
            if (cursor < end && *cursor == ' ') {
                while (cursor < end && *cursor == ' ') {
                    cursor++;
                }
                copy_length = (size_t)(end - cursor);
                if (copy_length >= sizeof(section)) {
                    copy_length = sizeof(section) - 1;
                }
                memcpy(section, cursor, copy_length);
                section[copy_length] = 0;
            }
        }

        if (line_end >= length) {
            break;
        }
        line_start = line_end + 1;
    }

    ts_case_free(&pending);
    free(markdown.data);
    free(expected.data);
    free(bytes);
    return 0;

fail:
    ts_case_free(&pending);
    free(markdown.data);
    free(expected.data);
    free(bytes);
    ts_spec_free(out);
    return -1;
}

void ts_spec_free(ts_spec_file *file) {
    size_t i;
    for (i = 0; i < file->count; i++) {
        ts_case_free(&file->cases[i]);
    }
    free(file->cases);
    file->cases = NULL;
    file->count = 0;
}

/* Traversal ------------------------------------------------------------------ */

markdown_core_document *ts_ast_parse(const uint8_t *bytes, size_t length) {
    markdown_core_document *document;
    markdown_core_status status = markdown_core_document_parse(bytes, length, &document);
    if (status != MARKDOWN_CORE_OK) {
        fprintf(stderr, "facade parse failed with status %d\n", (int)status);
        return NULL;
    }
    return document;
}

void ts_require_ok(markdown_core_status status, const char *call) {
    if (status != MARKDOWN_CORE_OK) {
        fprintf(stderr, "%s answered status %d\n", call, (int)status);
        abort();
    }
}

const markdown_core_node *ts_field(const markdown_core_node *node, ts_node_field accessor) {
    const markdown_core_node *field;
    ts_require_ok(accessor(node, &field), "a node-valued field accessor");
    return field;
}

const markdown_core_nodes *ts_nodes(const markdown_core_node *node, ts_nodes_field accessor) {
    const markdown_core_nodes *field;
    ts_require_ok(accessor(node, &field), "a sequence field accessor");
    return field;
}

const markdown_core_node *ts_at(const markdown_core_nodes *nodes, size_t index) {
    const markdown_core_node *node;
    return markdown_core_nodes_at(nodes, index, &node) == MARKDOWN_CORE_OK ? node : NULL;
}

const markdown_core_node *ts_child(const markdown_core_node *node, size_t index) {
    return ts_at(markdown_core_node_children(node), index);
}

/* One pending step of the walk: the nodes `index` to `end` of one relation --
 * of `nodes`, or the one node `single` -- where `base` is the offset the next
 * one's lead is relative to, and the relation's owner and its place among the
 * owner's relations. */
typedef struct {
    const markdown_core_node *single;
    const markdown_core_nodes *nodes;
    size_t index, end;
    int64_t base;
    const markdown_core_node *owner;
    size_t relation;
} ts_walk_entry;

typedef struct {
    ts_walk_entry *entries;
    size_t count, capacity;
    bool failed;
} ts_walk_stack;

static void ts_walk_push(ts_walk_stack *stack, ts_walk_entry entry) {
    if (entry.index >= entry.end || stack->failed) {
        return;
    }
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 256;
        ts_walk_entry *entries = realloc(stack->entries, capacity * sizeof(*entries));
        if (!entries) {
            stack->failed = true;
            return;
        }
        stack->entries = entries;
        stack->capacity = capacity;
    }
    stack->entries[stack->count++] = entry;
}

/* A node's relations in canonical field order, read through the public
 * facade alone: each relation's first node leads from the owner's start. */
static void ts_walk_relations(ts_walk_stack *stack, const markdown_core_node *node, int64_t start) {
    ts_walk_stack relations = {0};
    size_t relation = 0;
#define TS_RELATION(single_, nodes_, index_, end_)                                                                     \
    ts_walk_push(&relations, (ts_walk_entry){(single_), (nodes_), (index_), (end_), start, node, relation++})
#define TS_NODE(node_)                                                                                                 \
    do {                                                                                                               \
        const markdown_core_node *relation_node = (node_);                                                             \
        TS_RELATION(relation_node, NULL, 0, relation_node ? 1 : 0);                                                    \
    } while (0)
#define TS_SEQUENCE(nodes_)                                                                                            \
    do {                                                                                                               \
        const markdown_core_nodes *relation_nodes = (nodes_);                                                          \
        TS_RELATION(NULL, relation_nodes, 0, markdown_core_nodes_count(relation_nodes));                               \
    } while (0)
    const markdown_core_nodes *children = markdown_core_node_children(node);
    switch (markdown_core_node_get_kind(node)) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        TS_NODE(ts_field(node, markdown_core_node_document_metadata));
        TS_SEQUENCE(children);
        break;
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t columns, head = 0, content = 0, foot = 0;
        TS_OK(markdown_core_node_table_properties(node, &columns, &head, &content, &foot));
        TS_NODE(ts_field(node, markdown_core_node_table_caption));
        TS_RELATION(NULL, children, 0, head);
        TS_RELATION(NULL, children, head, head + content);
        TS_RELATION(NULL, children, head + content, head + content + foot);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE:
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        TS_NODE(ts_field(node, markdown_core_node_directive_label));
        TS_SEQUENCE(children);
        break;
    case MARKDOWN_CORE_KIND_CALLOUT:
        TS_SEQUENCE(ts_nodes(node, markdown_core_node_callout_title));
        TS_SEQUENCE(children);
        break;
    case MARKDOWN_CORE_KIND_CITATION: {
        markdown_core_referent referent;
        TS_OK(markdown_core_citation_referent(node, &referent));
        TS_NODE(referent.note);
        TS_SEQUENCE(ts_nodes(node, markdown_core_citation_prefix));
        TS_SEQUENCE(ts_nodes(node, markdown_core_citation_suffix));
        break;
    }
    case MARKDOWN_CORE_KIND_DEFINITION: {
        size_t count;
        TS_SEQUENCE(ts_nodes(node, markdown_core_node_definition_term));
        TS_OK(markdown_core_node_definition_body_count(node, &count));
        for (size_t index = 0; index < count; index++) {
            const markdown_core_nodes *body;
            TS_OK(markdown_core_node_definition_body_at(node, index, &body));
            TS_SEQUENCE(body);
        }
        break;
    }
    default:
        TS_SEQUENCE(children);
        break;
    }
#undef TS_SEQUENCE
#undef TS_NODE
#undef TS_RELATION
    /* Later relations go on the stack first, so the first comes off first. */
    stack->failed = stack->failed || relations.failed;
    while (relations.count) {
        ts_walk_push(stack, relations.entries[--relations.count]);
    }
    free(relations.entries);
}

int ts_ast_walk_owned(const markdown_core_node *root, ts_ast_owned_visit_fn visit, void *context) {
    ts_walk_stack stack = {0};
    int result = 0;
    ts_walk_push(&stack, (ts_walk_entry){root, NULL, 0, 1, 0, NULL, 0});
    while (stack.count && !stack.failed) {
        ts_walk_entry entry = stack.entries[--stack.count];
        const markdown_core_node *node = entry.nodes ? ts_at(entry.nodes, entry.index) : entry.single;
        markdown_core_extent extent = markdown_core_node_extent(node);
        ts_ast_place place = {
            {entry.base + extent.lead, entry.base + extent.lead + (int64_t)extent.span}, entry.owner, entry.relation};
        result = visit(node, place, context);
        if (result) {
            break;
        }
        entry.index++;
        entry.base = place.range.end;
        ts_walk_push(&stack, entry);
        ts_walk_relations(&stack, node, place.range.start);
    }
    if (stack.failed) {
        result = -1;
    }
    free(stack.entries);
    return result;
}

typedef struct {
    ts_ast_visit_fn visit;
    void *context;
} ts_ranged_visit;

static int ts_visit_range(const markdown_core_node *node, ts_ast_place place, void *context) {
    ts_ranged_visit *ranged = context;
    return ranged->visit(node, place.range, ranged->context);
}

int ts_ast_walk(const markdown_core_node *root, ts_ast_visit_fn visit, void *context) {
    ts_ranged_visit ranged = {visit, context};
    return ts_ast_walk_owned(root, ts_visit_range, &ranged);
}

typedef struct {
    int64_t length;
    const markdown_core_node *outside;
} ts_range_check;

static int ts_range_visit(const markdown_core_node *node, ts_ast_range range, void *context) {
    ts_range_check *check = context;
    if (range.start < 0 || range.end < range.start || range.end > check->length) {
        check->outside = node;
        return 1;
    }
    return 0;
}

const markdown_core_node *ts_ast_range_outside(const markdown_core_node *root, size_t length) {
    ts_range_check check = {(int64_t)length, NULL};
    (void)ts_ast_walk(root, ts_range_visit, &check);
    return check.outside;
}

static int ts_count_visit(const markdown_core_node *node, ts_ast_range range, void *context) {
    (void)range;
    size_t *counts = (size_t *)context;
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    if ((size_t)kind >= TS_KIND_COUNT) {
        fprintf(stderr, "node kind %d exceeds the test counter capacity\n", (int)kind);
        return -1;
    }
    counts[kind]++;
    return 0;
}

int ts_ast_count_kinds(const markdown_core_node *root, size_t *counts) {
    memset(counts, 0, TS_KIND_COUNT * sizeof(*counts));
    return ts_ast_walk(root, ts_count_visit, counts);
}

static int ts_concat_visit(const markdown_core_node *node, ts_ast_range range, void *context) {
    (void)range;
    ts_buffer *buffer = (ts_buffer *)context;
    if (markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_TEXT) {
        markdown_core_string literal;
        TS_OK(markdown_core_node_literal(node, &literal));
        if (literal.length && ts_buffer_append(buffer, (const char *)literal.data, literal.length) != 0) {
            return -1;
        }
    }
    return 0;
}

char *ts_ast_concat_text(const markdown_core_node *root, size_t *length) {
    ts_buffer buffer = {NULL, 0, 0};
    if (ts_buffer_append(&buffer, "", 0) != 0) {
        return NULL;
    }
    if (ts_ast_walk(root, ts_concat_visit, &buffer) != 0) {
        free(buffer.data);
        return NULL;
    }
    if (length) {
        *length = buffer.length;
    }
    return buffer.data;
}

/* Comparison and failure reporting ---------------------------------------- */

static void ts_print_annotated_line(FILE *stream, const char *prefix, const char *line, size_t length) {
    fputs(prefix, stream);
    fwrite(line, 1, length, stream);
    fputc('\n', stream);
}

void ts_print_line_diff(FILE *stream, const char *expected, const char *actual) {
    size_t line = 1;
    const char *expected_cursor = expected;
    const char *actual_cursor = actual;

    while (*expected_cursor || *actual_cursor) {
        const char *expected_end = strchr(expected_cursor, '\n');
        const char *actual_end = strchr(actual_cursor, '\n');
        size_t expected_length = expected_end ? (size_t)(expected_end - expected_cursor) : strlen(expected_cursor);
        size_t actual_length = actual_end ? (size_t)(actual_end - actual_cursor) : strlen(actual_cursor);
        if (expected_length != actual_length || memcmp(expected_cursor, actual_cursor, expected_length) != 0) {
            fprintf(stream, "first difference at output line %zu:\n", line);
            ts_print_annotated_line(stream, "  expected: ", expected_cursor, expected_length);
            ts_print_annotated_line(stream, "  actual:   ", actual_cursor, actual_length);
            return;
        }
        if (!expected_end && !actual_end) {
            break;
        }
        expected_cursor = expected_end ? expected_end + 1 : expected_cursor + expected_length;
        actual_cursor = actual_end ? actual_end + 1 : actual_cursor + actual_length;
        line++;
    }
    fprintf(stream, "outputs share all %zu compared lines but differ in trailing bytes\n", line);
}

/* Deterministic data ------------------------------------------------------ */

void ts_prng_seed(ts_prng *prng, uint64_t seed) { prng->state = seed ? seed : UINT64_C(0x9E3779B97F4A7C15); }

uint64_t ts_prng_next(ts_prng *prng) {
    uint64_t x = prng->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    prng->state = x;
    return x * UINT64_C(0x2545F4914F6CDD1D);
}

char *ts_repeat(const char *unit, size_t count, size_t *length) {
    size_t unit_length = strlen(unit);
    size_t total = unit_length * count;
    char *buffer = (char *)malloc(total + 1);
    size_t i;
    if (!buffer) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memcpy(buffer + i * unit_length, unit, unit_length);
    }
    buffer[total] = 0;
    if (length) {
        *length = total;
    }
    return buffer;
}

uint64_t ts_monotonic_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000.0) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}
