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

static int ts_case_push_extension(ts_spec_case *test_case, const char *name, size_t length) {
    char *copy;
    if (test_case->extension_count >= TS_MAX_EXTENSIONS) {
        return -1;
    }
    copy = (char *)malloc(length + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, name, length);
    copy[length] = 0;
    test_case->extensions[test_case->extension_count++] = copy;
    return 0;
}

static void ts_case_free(ts_spec_case *test_case) {
    size_t i;
    free(test_case->markdown);
    free(test_case->expected);
    free(test_case->section);
    for (i = 0; i < test_case->extension_count; i++) {
        free(test_case->extensions[i]);
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
                    } else if (ts_case_push_extension(&pending, word_start, (size_t)(cursor - word_start)) != 0) {
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

/* Facade parsing ----------------------------------------------------------- */

void ts_ast_options_none(markdown_core_parse_options *options) { memset(options, 0, sizeof(*options)); }

int ts_ast_enable(markdown_core_parse_options *options, const char *name) {
    if (strcmp(name, "smart") == 0) {
        options->smart_punctuation = true;
    } else if (strcmp(name, "footnotes") == 0) {
        options->footnotes = true;
    } else if (strcmp(name, "strip-html-comments") == 0) {
        options->strip_html_comments = true;
    } else if (strcmp(name, "table") == 0 || strcmp(name, "tables") == 0) {
        options->tables = true;
    } else if (strcmp(name, "strikethrough") == 0) {
        options->strikethrough = true;
    } else if (strcmp(name, "autolink") == 0 || strcmp(name, "autolinks") == 0) {
        options->autolinks = true;
    } else if (strcmp(name, "tasklist") == 0 || strcmp(name, "task-lists") == 0) {
        options->task_lists = true;
    } else if (strcmp(name, "formula") == 0 || strcmp(name, "formulas") == 0) {
        options->formulas = true;
    } else if (strcmp(name, "dollar-formula-delimiters") == 0) {
        options->dollar_formula_delimiters = true;
    } else if (strcmp(name, "latex-formula-delimiters") == 0) {
        options->latex_formula_delimiters = true;
    } else if (strcmp(name, "directive") == 0 || strcmp(name, "directives") == 0) {
        options->directives = true;
    } else {
        return -1;
    }
    return 0;
}

markdown_core_document *ts_ast_parse(const uint8_t *bytes, size_t length, const markdown_core_parse_options *options) {
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_parse(bytes, length, options, &error);
    if (!document) {
        markdown_core_string_view message =
            error ? markdown_core_error_get_message(error) : (markdown_core_string_view){NULL, 0};
        fprintf(stderr, "facade parse failed: ");
        if (message.data) {
            fwrite(message.data, 1, message.length, stderr);
        }
        fputc('\n', stderr);
        markdown_core_error_free(error);
        return NULL;
    }
    return document;
}

/* Traversal ------------------------------------------------------------------ */

int ts_ast_walk(const markdown_core_node *root, ts_ast_visit_fn visit, void *context) {
    const markdown_core_node **stack;
    size_t depth = 0;
    size_t capacity = 256;
    int result = 0;

    if (!root) {
        return 0;
    }
    stack = (const markdown_core_node **)malloc(capacity * sizeof(*stack));
    if (!stack) {
        return -1;
    }
    stack[depth++] = root;
    while (depth > 0) {
        const markdown_core_node *node = stack[--depth];
        const markdown_core_node *sibling = markdown_core_node_get_next_sibling(node);
        const markdown_core_node *child = markdown_core_node_get_first_child(node);
        result = visit(node, context);
        if (result != 0) {
            break;
        }
        if (depth + 2 > capacity) {
            const markdown_core_node **grown;
            capacity *= 2;
            grown = (const markdown_core_node **)realloc((void *)stack, capacity * sizeof(*stack));
            if (!grown) {
                result = -1;
                break;
            }
            stack = grown;
        }
        /* Push the sibling first so the child is visited before it. */
        if (sibling) {
            stack[depth++] = sibling;
        }
        if (child) {
            stack[depth++] = child;
        }
    }
    free((void *)stack);
    return result;
}

static int ts_count_visit(const markdown_core_node *node, void *context) {
    size_t *counts = (size_t *)context;
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    if ((size_t)kind < TS_KIND_COUNT) {
        counts[kind]++;
    }
    return 0;
}

int ts_ast_count_kinds(const markdown_core_node *root, size_t *counts) {
    memset(counts, 0, TS_KIND_COUNT * sizeof(*counts));
    return ts_ast_walk(root, ts_count_visit, counts);
}

static int ts_concat_visit(const markdown_core_node *node, void *context) {
    ts_buffer *buffer = (ts_buffer *)context;
    if (markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_TEXT) {
        markdown_core_string_view literal;
        if (markdown_core_node_literal(node, &literal) && literal.data) {
            if (ts_buffer_append(buffer, (const char *)literal.data, literal.length) != 0) {
                return -1;
            }
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

/* Comparison and diagnostics ---------------------------------------------- */

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
