#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>
#include "commit_compat.h"

static int failures = 0;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        failures++;
    }
}

static uint8_t *read_file(const char *path, size_t *length) {
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

static int parse_option_mask(const char *mask, markdown_core_parse_options *options) {
    bool *fields[] = {
        &options->smart_punctuation,
        &options->footnotes,
        &options->tables,
        &options->strikethrough,
        &options->autolinks,
        &options->task_lists,
        &options->formulas,
        &options->directives,
        &options->cross_links,
        &options->embeds
    };
    size_t i;
    if (strlen(mask) != sizeof(fields) / sizeof(fields[0])) {
        return 0;
    }
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (mask[i] != '0' && mask[i] != '1') {
            return 0;
        }
        *fields[i] = mask[i] == '1';
    }
    return 1;
}

static void check_fixture(const char *fixture_dir, const char *name, const char *option_mask) {
    char markdown_path[1024];
    char ast_path[1024];
    uint8_t *markdown;
    uint8_t *expected;
    uint8_t *actual = NULL;
    size_t markdown_length = 0, expected_length = 0, actual_length = 0;
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    markdown_core_parse_options options;

    snprintf(markdown_path, sizeof(markdown_path), "%s/%s.md", fixture_dir, name);
    snprintf(ast_path, sizeof(ast_path), "%s/%s.ast", fixture_dir, name);
    markdown = read_file(markdown_path, &markdown_length);
    expected = read_file(ast_path, &expected_length);
    check(markdown != NULL && expected != NULL, "fixture files are readable");
    if (!markdown || !expected) {
        goto done;
    }

    markdown_core_parse_options_init(&options);
    check(parse_option_mask(option_mask, &options), "manifest parse option mask is valid");
    document = markdown_core_document_new(mc_sv(markdown, markdown_length), &options, &error);
    check(document != NULL && error == NULL, "manifest-configured facade parse succeeds");
    if (!document) {
        goto done;
    }
    check(markdown_core_document_dump(document, &actual, &actual_length, &error), "native AST dump succeeds");
    check(error == NULL, "successful dump has no error");
    if (actual && (actual_length != expected_length || memcmp(actual, expected, expected_length) != 0)) {
        fprintf(stderr, "FAILED: %s dump differs from reviewed golden\n", name);
        fwrite(actual, 1, actual_length, stderr);
        failures++;
    }
    markdown_core_dump_free(actual);
    markdown_core_document_free(document);

done:
    markdown_core_error_free(error);
    free(markdown);
    free(expected);
}

typedef enum option_gate {
    GATE_TABLES,
    GATE_STRIKETHROUGH,
    GATE_AUTOLINKS,
    GATE_TASK_LISTS,
    GATE_FORMULAS,
    GATE_DIRECTIVES,
    GATE_CROSS_LINKS,
    GATE_EMBEDS,
    GATE_FOOTNOTES
} option_gate;

static void check_option_gate(option_gate gate, const char *source, const char *forbidden) {
    markdown_core_parse_options options;
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    markdown_core_parse_options_init(&options);
    switch (gate) {
    case GATE_TABLES:
        options.tables = false;
        break;
    case GATE_STRIKETHROUGH:
        options.strikethrough = false;
        break;
    case GATE_AUTOLINKS:
        options.autolinks = false;
        break;
    case GATE_TASK_LISTS:
        options.task_lists = false;
        break;
    case GATE_FORMULAS:
        options.formulas = false;
        break;
    case GATE_DIRECTIVES:
        options.directives = false;
        break;
    case GATE_CROSS_LINKS:
        options.cross_links = false;
        break;
    case GATE_EMBEDS:
        options.embeds = false;
        break;
    case GATE_FOOTNOTES:
        options.footnotes = false;
        break;
    }
    document = markdown_core_document_new(mc_sv((const uint8_t *)source, strlen(source)), &options, &error);
    check(document != NULL && error == NULL, "disabled-option parse succeeds");
    if (!document) {
        goto done;
    }
    check(markdown_core_document_dump(document, &dump, &length, &error), "disabled-option dump succeeds");
    if (dump) {
        check(strstr((const char *)dump, forbidden) == NULL, "disabled parse option falls back to the core AST");
    }
done:
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);
    markdown_core_error_free(error);
}

/* Comments are never deleted: a default-options parse keeps the inline and
 * block comment nodes with their bytes, and the surrounding text is not
 * fused across where a comment sat. */
static void check_html_comments_kept(void) {
    static const char source[] = "x <!-- kept --> y\n"
                                 "\n"
                                 "<!-- block -->\n";
    markdown_core_parse_options options;
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    markdown_core_parse_options_init(&options);
    document = markdown_core_document_new(mc_sv((const uint8_t *)source, sizeof(source) - 1), &options, &error);
    check(document != NULL && error == NULL, "default-options comment parse succeeds");
    if (!document) {
        goto done;
    }
    check(markdown_core_document_dump(document, &dump, &length, &error), "comment dump succeeds");
    if (dump) {
        check(strstr((const char *)dump, "<!-- kept -->") != NULL, "the inline comment node survives by default");
        check(strstr((const char *)dump, "<!-- block -->") != NULL, "the block comment node survives by default");
        check(strstr((const char *)dump, "\"x  y\"") == NULL, "text is not fused across a comment");
    }
    {
        bool comment = true;
        check(
            !markdown_core_node_html_comment(NULL, &comment) &&
                !markdown_core_node_html_comment(markdown_core_document_root(document), &comment),
            "the comment bit answers only for HTML kinds"
        );
    }
    markdown_core_document_free(document);
    document = NULL;
    /* The classification's edges: surrounding whitespace trims (indent,
     * trailing spaces and tab), and an unclosed comment block — no -->
     * anywhere — is not a comment. */
    {
        static const char edges[] = "  <!-- pad -->  \t\n"
                                    "\n"
                                    "<!-- open\n";
        const markdown_core_node *padded;
        const markdown_core_node *unclosed;
        bool comment = false;
        document = markdown_core_document_new(mc_sv((const uint8_t *)edges, sizeof(edges) - 1), &options, &error);
        check(document != NULL && error == NULL, "comment-edge parse succeeds");
        if (!document) {
            goto done;
        }
        padded = markdown_core_node_get_first_child(markdown_core_document_root(document));
        unclosed = padded ? markdown_core_node_get_next_sibling(padded) : NULL;
        check(
            padded && markdown_core_node_html_comment(padded, &comment) && comment,
            "a padded comment block classifies as a comment"
        );
        check(
            unclosed && markdown_core_node_html_comment(unclosed, &comment) && !comment,
            "an unclosed comment block is not a comment"
        );
        check(padded && !markdown_core_node_html_comment(padded, NULL), "a missing out-param answers false");
    }
done:
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);
    markdown_core_error_free(error);
}

static void check_scope_rows(
    const markdown_core_node *node,
    const markdown_core_scope_entry *entries,
    size_t count,
    size_t *index
) {
    const markdown_core_node *child;
    markdown_core_scope expected;
    const markdown_core_scope_entry *entry;

    check(*index < count, "scope table contains every canonical node");
    if (*index >= count) {
        return;
    }
    expected = markdown_core_node_scope(node);
    entry = &entries[(*index)++];
    check(entry->id == markdown_core_node_get_id(node), "scope table is in canonical preorder");
    check(entry->revision == markdown_core_node_get_revision(node), "scope table preserves node revisions");
    check(
        entry->scope.start.line == expected.start.line && entry->scope.start.column == expected.start.column &&
            entry->scope.end.line == expected.end.line && entry->scope.end.column == expected.end.column,
        "scope table preserves absolute scopes"
    );
    for (child = markdown_core_node_get_first_child(node); child; child = markdown_core_node_get_next_sibling(child)) {
        check_scope_rows(child, entries, count, index);
    }
}

static void check_scope_table(void) {
    static const uint8_t source[] = ":badge[first *second*]{k=v}\n\n> alpha\n> beta\n";
    markdown_core_error *error = NULL;
    markdown_core_document *session = NULL;
    const markdown_core_document *document;
    markdown_core_scope_entry *entries = (markdown_core_scope_entry *)(uintptr_t)1;
    markdown_core_scope_entry first = {0};
    size_t count = 99;
    size_t index = 0;

    check(
        !markdown_core_document_scope_table(NULL, &entries, &count, &error) && entries == NULL && count == 0 &&
            markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
        "scope table rejects a null document and clears outputs"
    );
    markdown_core_error_free(error);
    error = NULL;
    markdown_core_scope_table_free(NULL);

    session = markdown_core_document_open(NULL, &error);
    check(session != NULL && error == NULL, "scope table session opens");
    if (!session) {
        goto done;
    }
    {
        markdown_core_commit out;
        memset(&out, 0, sizeof(out));
        markdown_core_document_free(session);
        session = markdown_core_document_new(mc_sv(source, sizeof(source) - 1), NULL, &error);
    }
    if (!session) {
        check(0, "scope table session commits");
        goto done;
    }
    document = session;

    count = 99;
    check(
        !markdown_core_document_scope_table(document, NULL, &count, &error) && count == 0 &&
            markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
        "scope table rejects a null output and clears the count"
    );
    markdown_core_error_free(error);
    error = NULL;

    entries = (markdown_core_scope_entry *)(uintptr_t)1;
    check(
        !markdown_core_document_scope_table(document, &entries, NULL, &error) && entries == NULL &&
            markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
        "scope table rejects a null count and clears the output"
    );
    markdown_core_error_free(error);
    error = NULL;

    entries = NULL;
    count = 0;
    if (!document || !markdown_core_document_scope_table(document, &entries, &count, &error)) {
        check(0, "scope table materializes");
        goto done;
    }
    check(count > 0 && entries != NULL, "scope table returns owned rows");
    if (!count || !entries) {
        goto done;
    }
    check_scope_rows(markdown_core_document_root(document), entries, count, &index);
    check(index == count, "scope table has no hidden or duplicate rows");
    first = entries[0];
    markdown_core_document_release(session);
    session = NULL;
    check(
        entries[0].id == first.id && entries[0].revision == first.revision &&
            entries[0].scope.start.line == first.scope.start.line && entries[0].scope.end.line == first.scope.end.line,
        "scope table outlives its source document"
    );

done:
    markdown_core_scope_table_free(entries);
    markdown_core_document_release(session);
    markdown_core_error_free(error);
}

static long delta_parts(const markdown_core_delta *changes, markdown_core_node_id id) {
    const markdown_core_diff *diffs = NULL;
    size_t count = markdown_core_delta_diffs(changes, &diffs);
    size_t index;

    for (index = 0; index < count; ++index) {
        if (diffs[index].markup == id) {
            return (long)diffs[index].parts;
        }
    }
    return -1;
}
static void check_api(void) {
    static const uint8_t source[] = "# Heading\n\n- [ ] task\n";
    static const uint8_t cross_reference_source[] = "[[folder/note#^block|display]] ![[folder/note#^block|display]]\n";
    markdown_core_parse_options options;
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    const markdown_core_node *root;
    const markdown_core_node *heading;
    markdown_core_scope scope;
    int32_t level = 0;
    markdown_core_string reference_value = {0};

    memset(&options, 0, sizeof(options));
    markdown_core_parse_options_init(&options);
    check(
        options.smart_punctuation && options.footnotes && options.tables && options.strikethrough &&
            options.autolinks && options.task_lists && options.formulas && options.directives && options.cross_links &&
            options.embeds,
        "parse option defaults are explicit and complete"
    );

    document = markdown_core_document_new(mc_sv(source, sizeof(source) - 1), &options, &error);
    check(document != NULL && error == NULL, "typed-options parse succeeds");
    if (document) {
        root = markdown_core_document_root(document);
        heading = markdown_core_node_get_first_child(root);
        check(markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT, "document root kind is typed");
        check(
            markdown_core_node_get_kind(heading) == MARKDOWN_CORE_KIND_HEADING,
            "first child traversal is read-only and typed"
        );
        check(
            markdown_core_node_heading_level(heading, &level) && level == 1,
            "heading accessor returns its behavior-bearing field"
        );
        scope = markdown_core_node_scope(heading);
        check(scope.start.line == 1 && scope.start.column == 1, "scope copies native coordinates");
        markdown_core_document_free(document);
    }

    document =
        markdown_core_document_new(mc_sv(cross_reference_source, sizeof(cross_reference_source) - 1), &options, &error);
    check(document != NULL && error == NULL, "cross-reference parse succeeds");
    if (document) {
        const markdown_core_node *paragraph = markdown_core_node_get_first_child(markdown_core_document_root(document));
        const markdown_core_node *cross_link = markdown_core_node_get_first_child(paragraph);
        const markdown_core_node *embed = markdown_core_node_get_next_sibling(cross_link);
        static const char expected_reference[] = "folder/note#^block|display";
        check(
            markdown_core_node_get_kind(cross_link) == MARKDOWN_CORE_KIND_CROSS_LINK,
            "cross link has a distinct public kind"
        );
        check(
            markdown_core_node_cross_link_reference(cross_link, &reference_value) &&
                reference_value.length == sizeof(expected_reference) - 1 &&
                memcmp(reference_value.data, expected_reference, sizeof(expected_reference) - 1) == 0,
            "cross-link accessor preserves the source reference"
        );
        embed = markdown_core_node_get_next_sibling(embed);
        check(markdown_core_node_get_kind(embed) == MARKDOWN_CORE_KIND_EMBED, "embed has a distinct public kind");
        check(
            markdown_core_node_embed_reference(embed, &reference_value) &&
                reference_value.length == sizeof(expected_reference) - 1 &&
                memcmp(reference_value.data, expected_reference, sizeof(expected_reference) - 1) == 0,
            "embed accessor preserves the source reference"
        );
        markdown_core_document_free(document);
    }

    document = markdown_core_document_new(mc_sv(NULL, 1), NULL, &error);
    check(document == NULL && error != NULL, "invalid input produces an explicit error");
    check(markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "error exposes a stable code");
    check(markdown_core_error_get_message(error).length != 0, "error exposes a UTF-8 diagnostic view");
    markdown_core_error_free(error);
    markdown_core_error_free(NULL);
    markdown_core_document_free(NULL);
    markdown_core_dump_free(NULL);

    check_option_gate(GATE_TABLES, "| a |\n| --- |\n| b |\n", "Table scope=");
    check_option_gate(GATE_STRIKETHROUGH, "~~x~~\n", "Strikethrough scope=");
    check_option_gate(GATE_AUTOLINKS, "www.example.com\n", "Link scope=");
    check_option_gate(GATE_TASK_LISTS, "- [x] task\n", "checked=true");
    check_option_gate(
        GATE_FORMULAS,
        "$x$\n\n$$\ny\n$$\n\n\\\\(z\\\\)\n\n\\\\[q\\\\]\n\n```formula\nw\n```\n",
        "Formula"
    );
    check_option_gate(GATE_DIRECTIVES, ":badge[label]\n", "Directive scope=");
    check_option_gate(GATE_CROSS_LINKS, "[[reference]]\n", "CrossLink scope=");
    check_option_gate(GATE_EMBEDS, "![[reference]]\n", "Embed scope=");
    check_option_gate(GATE_FOOTNOTES, "ref[^a]\n\n[^a]: note\n", "FootnoteReference scope=");
    check_html_comments_kept();
    check_scope_table();
}

/* Diagnostics: the one thing an editor underlines.
 *
 * Markdown's own "mistakes" are all defined outcomes, so this checks the
 * boundary as much as the report — a directive whose attributes DO parse, and
 * one with no attributes at all, must raise nothing. The block and inline
 * forms carry different extents on purpose: the block form loses the whole
 * construct, the inline form loses only its braces.
 */
static void check_diagnostics(void) {
    static const struct {
        const char *markdown;
        size_t count;
        int start_column;
        int end_column;
    } cases[] = {
        {":::note{= bad}\nbody\n:::\n", 1, 8, 14},
        {":inline{= bad} tail\n", 1, 8, 8},
        {":::note{a=1}\nbody\n:::\n", 0, 0, 0},
        {":::note\nbody\n:::\n", 0, 0, 0},
        {"plain paragraph\n", 0, 0, 0}
    };
    size_t c;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        markdown_core_parse_options options;
        markdown_core_document *document;
        const markdown_core_diagnostic *rows = NULL;
        markdown_core_string text;
        size_t count;

        markdown_core_parse_options_init(&options);
        text.data = (const uint8_t *)cases[c].markdown;
        text.length = strlen(cases[c].markdown);
        document = markdown_core_document_new(text, &options, NULL);
        if (!document) {
            fprintf(stderr, "diagnostics: case %zu did not parse\n", c);
            failures++;
            continue;
        }
        count = markdown_core_document_diagnostics(document, &rows);
        if (count != cases[c].count) {
            fprintf(stderr, "diagnostics: case %zu raised %zu, expected %zu\n", c, count, cases[c].count);
            failures++;
        } else if (count == 0) {
            if (rows != NULL) {
                fprintf(stderr, "diagnostics: case %zu reported none but handed back an array\n", c);
                failures++;
            }
        } else if (
            rows[0].code != MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_ATTRIBUTES || rows[0].scope.start.line != 1 ||
            rows[0].scope.start.column != cases[c].start_column || rows[0].scope.end.line != 1 ||
            rows[0].scope.end.column != cases[c].end_column
        ) {
            fprintf(
                stderr,
                "diagnostics: case %zu reported code %d at %d:%d..%d:%d, expected code %d at 1:%d..1:%d\n",
                c,
                (int)rows[0].code,
                rows[0].scope.start.line,
                rows[0].scope.start.column,
                rows[0].scope.end.line,
                rows[0].scope.end.column,
                (int)MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_ATTRIBUTES,
                cases[c].start_column,
                cases[c].end_column
            );
            failures++;
        }
        markdown_core_document_free(document);
    }
}

int main(int argc, char **argv) {
    const char *fixture_dir;
    int i;
    if (argc < 5 || strcmp(argv[1], "--fixtures") != 0 || (argc - 3) % 2 != 0) {
        fputs("usage: facade_test --fixtures DIR NAME OPTION_MASK [NAME OPTION_MASK ...]\n", stderr);
        return 2;
    }
    fixture_dir = argv[2];
    check_api();
    check_diagnostics();
    for (i = 3; i < argc; i += 2) {
        check_fixture(fixture_dir, argv[i], argv[i + 1]);
    }
    if (failures) {
        fprintf(stderr, "%d facade test(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "native facade and canonical AST goldens passed\n");
    return 0;
}
