#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

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
        &options->smart_punctuation, &options->footnotes, &options->strip_html_comments, &options->tables,
        &options->strikethrough,     &options->autolinks, &options->task_lists,          &options->formulas,
        &options->directives};
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
    document = markdown_core_document_parse(markdown, markdown_length, &options, &error);
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
    GATE_FOOTNOTES,
    GATE_STRIP_HTML_COMMENTS
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
    case GATE_FOOTNOTES:
        options.footnotes = false;
        break;
    case GATE_STRIP_HTML_COMMENTS:
        options.strip_html_comments = false;
        break;
    }
    document = markdown_core_document_parse((const uint8_t *)source, strlen(source), &options, &error);
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

/* Is `node` in `root`'s tree? A region's owner must be, or the two views are
 * not one parse. */
static int reachable(const markdown_core_node *root, const markdown_core_node *node) {
    const markdown_core_node *child;
    if (root == node) {
        return 1;
    }
    for (child = markdown_core_node_get_first_child(root); child; child = markdown_core_node_get_next_sibling(child)) {
        if (reachable(child, node)) {
            return 1;
        }
    }
    return 0;
}

/* REQUIREMENT 12'S LAW, through the public surface and nothing else.
 *
 * The tree may omit bytes; the concrete view may not. Stated as three checks
 * that a consumer could make for itself:
 *
 *   the regions TILE the source -- no gap, no overlap, nothing past the end;
 *   every region's OWNER is a node of this document's semantic view;
 *   the line index agrees with the source, line for line.
 *
 * The corpus is deliberately one of everything the tree DOES omit: a fence's
 * backticks, an ATX heading's closing hashes, a link's brackets and
 * destination, an emphasis delimiter, a definition, an entity. If the pair were
 * incomplete this is where it would show.
 */
static void check_two_views(void) {
    static const uint8_t source[] = "# Heading ##\n"
                                    "\n"
                                    "> quoted *em* and `code`\n"
                                    "\n"
                                    "```c\n"
                                    "x\n"
                                    "```\n"
                                    "\n"
                                    "[a]: /u \"t\"\n"
                                    "\n"
                                    "see [a] and &amp; and [b](/v).\n";
    markdown_core_document *document = markdown_core_document_parse(source, sizeof(source) - 1, NULL, NULL);
    markdown_core_string_view text;
    const markdown_core_node *root;
    size_t count;
    size_t at = 0;
    size_t index;
    size_t line;
    int tiled = 1;
    int owned = 1;
    int lines_agree = 1;

    check(document != NULL, "two-views corpus parses");
    if (!document) {
        return;
    }
    root = markdown_core_document_semantic(document);
    text = markdown_core_document_source(document);
    count = markdown_core_document_region_count(document);
    check(text.length > 0 && count > 0, "the concrete view is populated");

    for (index = 0; index < count; index++) {
        markdown_core_region region;
        if (!markdown_core_document_region_at(document, index, &region)) {
            tiled = 0;
            break;
        }
        if (region.start != at || region.length == 0) {
            tiled = 0;
        }
        at = region.start + region.length;
        if (!region.owner || !reachable(root, region.owner)) {
            owned = 0;
        }
    }
    check(tiled && at == text.length, "every byte of the source is in exactly one region");
    check(owned, "every region has exactly one owner in the semantic view");

    /* The tree omits bytes and the pair does not: the closing `##`, the fence
     * and the definition's punctuation are in no literal anywhere, and every
     * one of them is in a region above. */
    check(markdown_core_document_line_count(document) == 11, "the line index counts the source's lines");
    for (line = 1; line <= markdown_core_document_line_count(document); line++) {
        size_t offset = 0;
        if (!markdown_core_document_line_start(document, line, &offset) || offset > text.length) {
            lines_agree = 0;
            break;
        }
        if (line > 1 && text.data[offset - 1] != '\n') {
            lines_agree = 0;
        }
    }
    check(lines_agree, "every line but the first begins after a line ending");
    check(!markdown_core_document_line_start(document, 0, &at), "line zero is not a line");
    check(!markdown_core_document_line_start(document, 12, &at), "a line past the end is not a line");
    {
        markdown_core_region region;
        check(!markdown_core_document_region_at(document, count, &region), "a region past the end is not a region");
    }
    markdown_core_document_free(document);

    check(markdown_core_document_source(NULL).data == NULL, "a null document has no source");
    check(markdown_core_document_region_count(NULL) == 0, "a null document has no regions");
    check(markdown_core_document_line_count(NULL) == 0, "a null document has no lines");
}

static void check_api(void) {
    static const uint8_t source[] = "# Heading\n\n- [ ] task\n";
    markdown_core_parse_options options;
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    const markdown_core_node *root;
    const markdown_core_node *heading;
    markdown_core_scope scope;
    int32_t level = 0;

    memset(&options, 0, sizeof(options));
    markdown_core_parse_options_init(&options);
    check(options.smart_punctuation && options.footnotes && options.strip_html_comments && options.tables &&
              options.strikethrough && options.autolinks && options.task_lists && options.formulas &&
              options.directives,
          "parse option defaults are explicit and complete");

    document = markdown_core_document_parse(source, sizeof(source) - 1, &options, &error);
    check(document != NULL && error == NULL, "typed-options parse succeeds");
    if (document) {
        root = markdown_core_document_semantic(document);
        heading = markdown_core_node_get_first_child(root);
        check(markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT, "document root kind is typed");
        check(markdown_core_node_get_kind(heading) == MARKDOWN_CORE_KIND_HEADING,
              "first child traversal is read-only and typed");
        check(markdown_core_node_heading_level(heading, &level) && level == 1,
              "heading accessor returns its behavior-bearing field");
        scope = markdown_core_node_scope(heading);
        check(scope.start.line == 1 && scope.start.column == 1, "scope copies native coordinates");
        markdown_core_document_free(document);
    }

    document = markdown_core_document_parse(NULL, 1, NULL, &error);
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
    check_option_gate(GATE_FORMULAS, "$x$\n", "Formula scope=");
    check_option_gate(GATE_DIRECTIVES, ":badge[label]\n", "Directive scope=");
    check_option_gate(GATE_FOOTNOTES, "ref[^a]\n\n[^a]: note\n", "FootnoteReference scope=");
    check_option_gate(GATE_STRIP_HTML_COMMENTS, "before <!-- kept --> after\n", "literal=\"before  after\"");
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
    check_two_views();
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
