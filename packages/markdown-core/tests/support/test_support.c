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

/* The children of `node` a cursor reads in `field`, or in any field when it
 * is 0: the `index`th, and how many were read up to it. */
static const markdown_core_node *ts_children(const markdown_core_node *node, markdown_core_field field, size_t index,
                                             size_t *count) {
    markdown_core_cursor *cursor;
    const markdown_core_node *found = NULL;
    bool moved;
    *count = 0;
    TS_OK(markdown_core_cursor_open(node, &cursor));
    TS_OK(markdown_core_cursor_child(cursor, &moved));
    for (; moved; moved = markdown_core_cursor_next(cursor)) {
        if (field && markdown_core_cursor_field(cursor) != field) {
            continue;
        }
        if ((*count)++ == index) {
            found = markdown_core_cursor_node(cursor);
            break;
        }
    }
    markdown_core_cursor_free(cursor);
    return found;
}

const markdown_core_node *ts_child(const markdown_core_node *node, markdown_core_field field, size_t index) {
    size_t count;
    return ts_children(node, field, index, &count);
}

size_t ts_child_count(const markdown_core_node *node, markdown_core_field field) {
    size_t count;
    ts_children(node, field, SIZE_MAX, &count);
    return count;
}

/* A field's place among the relations of the kinds that have it, in
 * canonical traversal order. */
static size_t ts_field_rank(markdown_core_field field) {
    switch (field) {
    case MARKDOWN_CORE_FIELD_METADATA:
    case MARKDOWN_CORE_FIELD_TITLE:
    case MARKDOWN_CORE_FIELD_CAPTION:
    case MARKDOWN_CORE_FIELD_LABEL:
    case MARKDOWN_CORE_FIELD_TERM:
    case MARKDOWN_CORE_FIELD_NOTE:
        return 0;
    case MARKDOWN_CORE_FIELD_HEAD:
    case MARKDOWN_CORE_FIELD_PREFIX:
        return 1;
    case MARKDOWN_CORE_FIELD_FOOT:
        return 3;
    default:
        return 2;
    }
}

/* One node on the walk's path: where its source starts, and the relation of
 * it the walk is in and where that relation's next node is measured from. */
typedef struct {
    const markdown_core_node *node;
    int64_t start, anchor;
    size_t relation;
    bool in_relation;
} ts_walk_frame;

/* The source window of `node`, measured from `anchor`: from where its first
 * run begins to where its last ends. */
static ts_ast_range ts_source_window(const markdown_core_node *node, int64_t anchor) {
    size_t count = 0;
    const markdown_core_run *runs = markdown_core_node_runs(node, &count);
    ts_ast_range range = {count ? anchor + runs[0].lead : anchor, 0};
    int64_t at = anchor;
    for (size_t index = 0; index < count; index++) {
        at += runs[index].lead + (int64_t)runs[index].span;
    }
    range.end = count ? at : range.start;
    return range;
}

/* Each node's relations in canonical field order, read with a cursor: a
 * relation's first node is measured from where its owner's source starts,
 * and each next one from where the source of the one before ends. A
 * relation is numbered from its field and its list, so an absent or empty
 * one keeps its place. */
int ts_ast_walk_owned(const markdown_core_node *root, ts_ast_owned_visit_fn visit, void *context) {
    markdown_core_cursor *cursor;
    ts_walk_frame *frames = NULL;
    size_t depth = 0, capacity = 0;
    int result = 0;
    if (markdown_core_cursor_open(root, &cursor) != MARKDOWN_CORE_OK) {
        return -1;
    }
    ts_ast_place place = {ts_source_window(root, 0), NULL, 0};
    for (;;) {
        const markdown_core_node *node = markdown_core_cursor_node(cursor);
        if ((result = visit(node, place, context))) {
            break;
        }
        if (depth == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            ts_walk_frame *grown = realloc(frames, capacity * sizeof(*frames));
            if (!grown) {
                result = -1;
                break;
            }
            frames = grown;
        }
        frames[depth] = (ts_walk_frame){node, place.range.start, 0, 0, false};
        bool moved;
        if (markdown_core_cursor_child(cursor, &moved) != MARKDOWN_CORE_OK) {
            result = -1;
            break;
        }
        if (moved) {
            depth++;
        } else {
            while (depth && !markdown_core_cursor_next(cursor)) {
                markdown_core_cursor_parent(cursor);
                depth--;
            }
            if (!depth) {
                break;
            }
        }
        ts_walk_frame *owner = &frames[depth - 1];
        markdown_core_field field = markdown_core_cursor_field(cursor);
        size_t relation = ts_field_rank(field) * 1000000 + markdown_core_cursor_list(cursor);
        if (!owner->in_relation || owner->relation != relation) {
            owner->in_relation = true;
            owner->relation = relation;
            owner->anchor = owner->start;
        }
        place =
            (ts_ast_place){ts_source_window(markdown_core_cursor_node(cursor), owner->anchor), owner->node, relation};
        owner->anchor = place.range.end;
    }
    free(frames);
    markdown_core_cursor_free(cursor);
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

/* Whether the source ranges runs lead to from `at`, each from the end of the
 * one before, lie in `[0, length]`. */
static bool ts_runs_inside(const markdown_core_run *runs, size_t count, int64_t at, int64_t length) {
    for (size_t index = 0; index < count; index++) {
        int64_t start = at + runs[index].lead;
        at = start + (int64_t)runs[index].span;
        if (start < 0 || at > length) {
            return false;
        }
    }
    return true;
}

static int ts_range_visit(const markdown_core_node *node, ts_ast_place place, void *context) {
    ts_range_check *check = context;
    bool inside = place.range.start >= 0 && place.range.end >= place.range.start && place.range.end <= check->length;
    size_t runs = 0;
    const markdown_core_run *run = markdown_core_node_runs(node, &runs);
    inside = inside && runs && ts_runs_inside(run, runs, place.range.start - run[0].lead, check->length);
    if (!inside) {
        check->outside = node;
        return 1;
    }
    return 0;
}

const markdown_core_node *ts_ast_range_outside(const markdown_core_node *root, size_t length) {
    ts_range_check check = {(int64_t)length, NULL};
    (void)ts_ast_walk_owned(root, ts_range_visit, &check);
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
