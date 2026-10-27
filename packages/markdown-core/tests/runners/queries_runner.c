/* The query corpus (specs/context): the C queries' answers, kept as a
 * reviewed record so a change to any of them shows in the diff.
 *
 * Each example of the context fixture is a document with one `‸`, the cursor.
 * For each, in both units, the runner renders what the C queries answer, one
 * line per entry (specs/context/README.md):
 *
 *   block KIND START..END
 *   inline KIND START..END
 *   slot SLOT OWNER open|closed before=START..END after=START..END [prefix="TEXT"]...
 *   spell INDEX "VALUE" "TEXT" CURSOR | spell INDEX "VALUE" none
 *   path ID...
 *   node ID [SLOT=SCOPES]... [none=SCOPES] [markers=SCOPES]
 *
 * the context at the cursor, each slot's spelling of every value, the path
 * at the cursor, and every node's fields and markers, a node's SCOPES joined
 * by `,`. A spelling's cursor counts columns in the unit. Strings are written
 * as they are, between quotes; none of them holds a line ending.
 *
 *   queries_runner --emit FIXTURE OUTPUT   write the corpus
 *   queries_runner --check FIXTURE CORPUS  fail unless the corpus is current
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static const char CURSOR[] = "\xe2\x80\xb8";
static const char *const VALUES[] = {"x", "a b", "a|b", "a]b", "\xc3\xa9\xf0\x9f\x9a\x80"};
enum { VALUE_COUNT = sizeof(VALUES) / sizeof(*VALUES) };

typedef struct {
    char *data;
    size_t length, capacity;
} text;

static bool append(text *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    const int need = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (need < 0 || out->length + (size_t)need + 1 > out->capacity) {
        const size_t capacity = (out->length + (size_t)(need < 0 ? 0 : need) + 1) * 2;
        char *grown = need < 0 ? NULL : realloc(out->data, capacity);
        if (!grown) {
            va_end(again);
            return false;
        }
        out->data = grown;
        out->capacity = capacity;
    }
    vsnprintf(out->data + out->length, (size_t)need + 1, format, again);
    va_end(again);
    out->length += (size_t)need;
    return true;
}

static const char *kind_name(markdown_core_node_kind kind) {
    const char *name = "?";
    markdown_core_node_kind_name(kind, &name);
    return name;
}

static bool scope(text *out, markdown_core_scope s) {
    return append(out, "%d:%d..%d:%d", s.start.line, s.start.column, s.end.line, s.end.column);
}

/* The columns of `count` bytes from a line's start, in `unit`. */
static int32_t columns(const uint8_t *bytes, size_t count, markdown_core_text_unit unit) {
    if (unit == MARKDOWN_CORE_TEXT_UNIT_UTF8) {
        return (int32_t)count;
    }
    int32_t units = 0;
    for (size_t i = 0; i < count; i++) {
        if ((bytes[i] & 0xc0) != 0x80) {
            units += bytes[i] >= 0xf0 ? 2 : 1;
        }
    }
    return units;
}

/* The nodes of a document, by identifier: a parse numbers them from 1 in
 * walk order. */
typedef struct {
    const markdown_core_node **nodes;
    size_t count, capacity;
} nodes;

static int collect(const markdown_core_node *node, ts_ast_range range, void *context) {
    (void)range;
    nodes *all = context;
    if (all->count == all->capacity) {
        const size_t capacity = all->capacity ? all->capacity * 2 : 16;
        const markdown_core_node **grown = realloc(all->nodes, capacity * sizeof(*grown));
        if (!grown) {
            return 1;
        }
        all->nodes = grown;
        all->capacity = capacity;
    }
    all->nodes[all->count++] = node;
    return 0;
}

static bool scopes(text *out, const char *label, markdown_core_status status, markdown_core_scope *found,
                   size_t count) {
    bool ok = status == MARKDOWN_CORE_OK && (!count || append(out, " %s=", label));
    for (size_t i = 0; ok && i < count; i++) {
        ok = (!i || append(out, ",")) && scope(out, found[i]);
    }
    markdown_core_scopes_free(found);
    return ok;
}

/* The rendering of every query at `position` of `source` in `unit`, or
 * NULL. */
static char *render(const uint8_t *source, size_t length, markdown_core_position position,
                    markdown_core_text_unit unit) {
    text out = {0};
    /* A finalized session of the source, as the engine holds one per
     * version of a text. */
    markdown_core_session *session = NULL;
    const markdown_core_document *document = NULL;
    markdown_core_context *context = NULL;
    bool ok = markdown_core_session_new(source, length, unit, &session) == MARKDOWN_CORE_OK &&
              markdown_core_session_finalize(session, &document) == MARKDOWN_CORE_OK &&
              markdown_core_document_context(document, source, length, position, &context) == MARKDOWN_CORE_OK;
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
    for (size_t i = 0; ok && i < markdown_core_context_slot_count(context); i++) {
        for (size_t v = 0; ok && v < VALUE_COUNT; v++) {
            markdown_core_insertion *made = NULL;
            ok = markdown_core_slot_spell(context, i, (const uint8_t *)VALUES[v], strlen(VALUES[v]), &made) ==
                     MARKDOWN_CORE_OK &&
                 append(&out, "spell %zu \"%s\" ", i, VALUES[v]) &&
                 (made ? append(&out, "\"%.*s\" %d\n", (int)made->size, (const char *)made->text,
                                columns(made->text, made->cursor, unit))
                       : append(&out, "none\n"));
            markdown_core_insertion_free(made);
        }
    }
    const markdown_core_node **path = NULL;
    size_t count = 0;
    ok = ok && markdown_core_document_path(document, position, source, length, &path, &count) == MARKDOWN_CORE_OK &&
         append(&out, "path");
    for (size_t i = 0; ok && i < count; i++) {
        ok = append(&out, " %llu", (unsigned long long)markdown_core_node_id(path[i]));
    }
    markdown_core_path_free(path);
    ok = ok && append(&out, "\n");
    nodes all = {0};
    ok = ok && ts_ast_walk(markdown_core_document_root(document), collect, &all) == 0;
    for (size_t n = 0; ok && n < all.count; n++) {
        const markdown_core_node *node = all.nodes[n];
        ok = append(&out, "node %llu", (unsigned long long)markdown_core_node_id(node));
        for (int slot = 1; ok && slot <= MARKDOWN_CORE_SLOT_VALUE + 1; slot++) {
            const markdown_core_slot_kind kind =
                slot <= MARKDOWN_CORE_SLOT_VALUE ? (markdown_core_slot_kind)slot : MARKDOWN_CORE_SLOT_NONE;
            const char *name = "?";
            markdown_core_scope *found = NULL;
            size_t found_count = 0;
            const markdown_core_status status =
                markdown_core_document_field(document, node, kind, source, length, &found, &found_count);
            ok = markdown_core_slot_kind_name(kind, &name) == MARKDOWN_CORE_OK &&
                 scopes(&out, name, status, found, found_count);
        }
        markdown_core_scope *found = NULL;
        size_t found_count = 0;
        const markdown_core_status status =
            markdown_core_document_markers(document, node, source, length, &found, &found_count);
        ok = ok && scopes(&out, "markers", status, found, found_count) && append(&out, "\n");
    }
    free(all.nodes);
    markdown_core_context_free(context);
    markdown_core_session_free(session);
    if (!ok) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

/* `bytes` as a JSON string. */
static bool quoted(text *out, const char *bytes, size_t length) {
    bool ok = append(out, "\"");
    for (size_t i = 0; ok && i < length; i++) {
        const unsigned char c = (unsigned char)bytes[i];
        ok = c == '"' || c == '\\' ? append(out, "\\%c", c)
             : c == '\n'           ? append(out, "\\n")
             : c == '\t'           ? append(out, "\\t")
             : c < 0x20            ? append(out, "\\u%04x", c)
                                   : append(out, "%c", c);
    }
    return ok && append(out, "\"");
}

/* The corpus of every example of `fixture`, as JSON, or NULL. */
static char *corpus(const char *fixture) {
    ts_spec_file spec;
    if (ts_spec_load(fixture, &spec) != 0) {
        fprintf(stderr, "%s: cannot load\n", fixture);
        return NULL;
    }
    text out = {0};
    bool ok = append(&out, "{\n    \"schemaVersion\": 1,\n    \"fixture\": \"packages/markdown-core/tests/context/"
                           "context.txt\",\n    \"values\": [");
    for (size_t v = 0; ok && v < VALUE_COUNT; v++) {
        ok = (!v || append(&out, ", ")) && quoted(&out, VALUES[v], strlen(VALUES[v]));
    }
    ok = ok && append(&out, "],\n    \"cases\": [");
    for (size_t i = 0; ok && i < spec.count; i++) {
        const ts_spec_case *example = &spec.cases[i];
        const char *mark = strstr(example->markdown, CURSOR);
        if (!mark) {
            fprintf(stderr, "%s:%d: no cursor mark\n", fixture, example->start_line);
            ok = false;
            break;
        }
        const size_t at = (size_t)(mark - example->markdown), size = sizeof(CURSOR) - 1;
        const size_t length = example->markdown_length - size;
        uint8_t *source = malloc(length + 1);
        if (!source) {
            ok = false;
            break;
        }
        memcpy(source, example->markdown, at);
        memcpy(source + at, mark + size, example->markdown_length - at - size);
        size_t line_start = 0;
        int32_t line = 1;
        for (size_t k = 0; k < at; k++) {
            if (source[k] == '\n') {
                line++;
                line_start = k + 1;
            }
        }
        ok = append(&out, "%s\n        {\n            \"line\": %d,\n            \"source\": ", i ? "," : "",
                    example->start_line) &&
             quoted(&out, (const char *)source, length);
        static const markdown_core_text_unit units[] = {MARKDOWN_CORE_TEXT_UNIT_UTF8, MARKDOWN_CORE_TEXT_UNIT_UTF16};
        static const char *const unit_names[] = {"utf8", "utf16"};
        for (size_t u = 0; ok && u < 2; u++) {
            const markdown_core_position position = {line, columns(source + line_start, at - line_start, units[u]) + 1};
            char *rendered = render(source, length, position, units[u]);
            ok = rendered && append(&out, ",\n            \"%s\": {\"cursor\": [%d, %d], \"expected\": ", unit_names[u],
                                    position.line, position.column);
            ok = ok && quoted(&out, rendered, strlen(rendered)) && append(&out, "}");
            if (!rendered) {
                fprintf(stderr, "%s:%d: a query failed\n", fixture, example->start_line);
            }
            free(rendered);
        }
        ok = ok && append(&out, "\n        }");
        free(source);
    }
    ok = ok && append(&out, "\n    ]\n}\n");
    ts_spec_free(&spec);
    if (!ok) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "--emit") != 0 && strcmp(argv[1], "--check") != 0)) {
        fputs("usage: queries_runner --emit|--check FIXTURE CORPUS\n", stderr);
        return 2;
    }
    char *made = corpus(argv[2]);
    if (!made) {
        return 1;
    }
    int status = 0;
    if (!strcmp(argv[1], "--emit")) {
        FILE *file = fopen(argv[3], "wb");
        status = !file || fputs(made, file) < 0;
        if (file && fclose(file) != 0) {
            status = 1;
        }
    } else {
        size_t length = 0;
        uint8_t *current = ts_read_file(argv[3], &length);
        status = !current || length != strlen(made) || memcmp(current, made, length) != 0;
        if (status) {
            fprintf(stderr, "%s is stale: run queries_runner --emit %s %s\n", argv[3], argv[2], argv[3]);
        }
        free(current);
    }
    free(made);
    if (!status) {
        printf("%s: %s\n", argv[3], argv[1][2] == 'e' ? "written" : "current");
    }
    return status;
}
