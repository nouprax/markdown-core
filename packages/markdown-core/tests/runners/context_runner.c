/* Context query fixture runner (docs/plans/2026-10-23-lsp-completion-engine.md,
 * 3.1).
 *
 * Each example of a spec fixture is a document with one `‸`, the cursor. The
 * runner parses the document without the mark, asks for the context at the
 * cursor and compares its rendering with the expected block, one line per
 * entry: the open blocks, the confirmed inline openers, then the slots, or
 * `none` when the context holds nothing.
 *
 *   block KIND START..END
 *   inline KIND START..END
 *   slot SLOT OWNER open|closed before=START..END after=START..END [prefix="TEXT"]...
 *
 * A scope's end is its last byte, so an empty one ends a column before it
 * starts. `--dump` prints every example's rendering instead of comparing.
 *
 * `--sweep` asks for the context at every position of every example of each
 * spec fixture, in both units, and checks what holds at any cursor: the
 * query answers, and each slot's `before` ends and its `after` starts at
 * the cursor, on the cursor's line.
 *
 *   context_runner --spec FILE [--dump]
 *   context_runner --sweep FILE...
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static const char CURSOR[] = "\xe2\x80\xb8";

typedef struct {
    char *data;
    size_t length, capacity;
} text;

static int append(text *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    int need = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (need < 0) {
        va_end(again);
        return 0;
    }
    if (out->length + (size_t)need + 1 > out->capacity) {
        size_t capacity = (out->capacity ? out->capacity * 2 : 256) + (size_t)need;
        char *data = realloc(out->data, capacity);
        if (!data) {
            va_end(again);
            return 0;
        }
        out->data = data;
        out->capacity = capacity;
    }
    vsnprintf(out->data + out->length, (size_t)need + 1, format, again);
    va_end(again);
    out->length += (size_t)need;
    return 1;
}

static const char *kind_name(markdown_core_node_kind kind) {
    const char *name = "?";
    markdown_core_node_kind_name(kind, &name);
    return name;
}

static int scope(text *out, markdown_core_scope s) {
    return append(out, "%d:%d..%d:%d", s.start.line, s.start.column, s.end.line, s.end.column);
}

/* The context at the cursor of `example`, rendered, or NULL. */
static char *render(const ts_spec_case *example) {
    const char *mark = strstr(example->markdown, CURSOR);
    if (!mark || strstr(mark + 1, CURSOR)) {
        fprintf(stderr, "example %d: needs exactly one cursor mark\n", example->example);
        return NULL;
    }
    const size_t at = (size_t)(mark - example->markdown), size = sizeof(CURSOR) - 1;
    const size_t length = example->markdown_length - size;
    uint8_t *source = malloc(length + 1);
    if (!source) {
        return NULL;
    }
    memcpy(source, example->markdown, at);
    memcpy(source + at, mark + size, example->markdown_length - at - size);
    source[length] = 0;
    markdown_core_position position = {1, 1};
    for (size_t i = 0; i < at; i++) {
        if (source[i] == '\n') {
            position.line++;
            position.column = 1;
        } else {
            position.column++;
        }
    }
    text out = {0};
    markdown_core_context *context = NULL;
    markdown_core_document *document = ts_ast_parse(source, length);
    int ok =
        document && markdown_core_document_context(document, source, length, position, &context) == MARKDOWN_CORE_OK;
    for (size_t i = 0; ok && i < markdown_core_context_block_count(context); i++) {
        markdown_core_opener opener;
        ok = markdown_core_context_block_at(context, i, &opener) == MARKDOWN_CORE_OK &&
             append(&out, "block %s ", kind_name(opener.kind)) && scope(&out, opener.scope) && append(&out, "\n");
    }
    for (size_t i = 0; ok && i < markdown_core_context_inline_count(context); i++) {
        markdown_core_opener opener;
        ok = markdown_core_context_inline_at(context, i, &opener) == MARKDOWN_CORE_OK &&
             append(&out, "inline %s ", kind_name(opener.kind)) && scope(&out, opener.scope) && append(&out, "\n");
    }
    for (size_t i = 0; ok && i < markdown_core_context_slot_count(context); i++) {
        markdown_core_slot slot;
        const char *name = "?";
        ok = markdown_core_context_slot_at(context, i, &slot) == MARKDOWN_CORE_OK &&
             markdown_core_slot_kind_name(slot.kind, &name) == MARKDOWN_CORE_OK &&
             append(&out, "slot %s %s %s before=", name, kind_name(slot.owner), slot.closed ? "closed" : "open") &&
             scope(&out, slot.before) && append(&out, " after=") && scope(&out, slot.after);
        for (size_t j = 0; ok && j < slot.prefix_count; j++) {
            markdown_core_string prefix;
            ok = markdown_core_context_slot_prefix_at(context, i, j, &prefix) == MARKDOWN_CORE_OK &&
                 append(&out, " prefix=\"%.*s\"", (int)prefix.length, (const char *)prefix.data);
        }
        ok = ok && append(&out, "\n");
    }
    if (ok && !out.length) {
        ok = append(&out, "none\n");
    }
    if (!ok) {
        fprintf(stderr, "example %d: the context query failed\n", example->example);
        free(out.data);
        out.data = NULL;
    }
    markdown_core_context_free(context);
    markdown_core_document_free(document);
    free(source);
    return out.data;
}

/* The column after `count` bytes of `source` from a line's start, in `unit`. */
static int32_t column_after(const uint8_t *source, size_t count, markdown_core_text_unit unit) {
    if (unit == MARKDOWN_CORE_TEXT_UNIT_UTF8) {
        return (int32_t)count + 1;
    }
    int32_t column = 1;
    for (size_t i = 0; i < count; i++) {
        if ((source[i] & 0xc0) != 0x80) {
            column += source[i] >= 0xf0 ? 2 : 1;
        }
    }
    return column;
}

/* Whether every position of `example` answers a context whose slots hold
 * the cursor, in `unit`. */
static int sweeps(const ts_spec_case *example, markdown_core_text_unit unit, size_t *queries) {
    const uint8_t *source = (const uint8_t *)example->markdown;
    const size_t length = example->markdown_length;
    markdown_core_document *document = NULL;
    if (markdown_core_document_parse_in(source, length, unit, &document) != MARKDOWN_CORE_OK) {
        fprintf(stderr, "example %d: the parse failed\n", example->example);
        return 0;
    }
    int good = 1;
    size_t line_start = 0;
    markdown_core_position position = {1, 1};
    for (size_t at = 0; good && at <= length; at++) {
        if (at < length && (source[at] & 0xc0) == 0x80) {
            continue;
        }
        position.column = column_after(source + line_start, at - line_start, unit);
        markdown_core_context *context = NULL;
        good = markdown_core_document_context(document, source, length, position, &context) == MARKDOWN_CORE_OK;
        for (size_t i = 0; good && i < markdown_core_context_slot_count(context); i++) {
            markdown_core_slot slot;
            good = markdown_core_context_slot_at(context, i, &slot) == MARKDOWN_CORE_OK &&
                   slot.after.start.line == position.line && slot.after.start.column == position.column &&
                   slot.before.start.line == position.line && slot.before.start.column <= position.column &&
                   slot.before.end.column == position.column - 1;
        }
        if (!good) {
            fprintf(stderr, "example %d (line %d): the context at %d:%d breaks a slot's bounds\n", example->example,
                    example->start_line, position.line, position.column);
        }
        markdown_core_context_free(context);
        ++*queries;
        if (at < length && source[at] == '\n') {
            position.line++;
            line_start = at + 1;
        }
    }
    markdown_core_document_free(document);
    return good;
}

static int sweep(int count, char **paths) {
    int failures = 0;
    size_t queries = 0;
    for (int i = 0; i < count; i++) {
        ts_spec_file spec;
        if (ts_spec_load(paths[i], &spec) != 0) {
            fprintf(stderr, "%s: cannot load\n", paths[i]);
            return 1;
        }
        for (size_t j = 0; j < spec.count; j++) {
            failures += !sweeps(&spec.cases[j], MARKDOWN_CORE_TEXT_UNIT_UTF8, &queries);
            failures += !sweeps(&spec.cases[j], MARKDOWN_CORE_TEXT_UNIT_UTF16, &queries);
        }
        ts_spec_free(&spec);
    }
    printf("%zu queries, %d examples failed\n", queries, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int dump = 0;
    if (argc > 2 && strcmp(argv[1], "--sweep") == 0) {
        return sweep(argc - 2, argv + 2);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--spec") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--dump") == 0) {
            dump = 1;
        } else {
            fputs("usage: context_runner --spec FILE [--dump]\n", stderr);
            return 2;
        }
    }
    ts_spec_file spec;
    if (!path || ts_spec_load(path, &spec) != 0) {
        fputs("usage: context_runner --spec FILE [--dump]\n", stderr);
        return 2;
    }
    int failures = 0;
    for (size_t i = 0; i < spec.count; i++) {
        const ts_spec_case *example = &spec.cases[i];
        char *actual = render(example);
        if (!actual) {
            failures++;
        } else if (dump) {
            printf("example %d (line %d):\n%s\n", example->example, example->start_line, actual);
        } else if (strcmp(actual, example->expected) != 0) {
            fprintf(stderr, "example %d (line %d) differs:\n", example->example, example->start_line);
            ts_print_line_diff(stderr, example->expected, actual);
            failures++;
        }
        free(actual);
    }
    printf("%zu examples, %d failed\n", spec.count, failures);
    ts_spec_free(&spec);
    return failures ? 1 : 0;
}
