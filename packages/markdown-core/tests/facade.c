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
/* WHAT A SCOPE INDEXES INTO. A scope names a place in the NORMALIZED source --
 * not in the bytes the caller passed -- so a consumer that follows one back to
 * the source needs those bytes and the index that turns a line into an offset
 * into them. This asserts that the two are there, agree with each other, and
 * refuse what is not a line. */
static void check_source_and_lines(void) {
    static const char *const SOURCE = "# heading ##\n"
                                      "\n"
                                      "para *em* text\n"
                                      "\n"
                                      "```js\n"
                                      "code\n"
                                      "```\n"
                                      "\n"
                                      "[a]: /u\n"
                                      "\n"
                                      "see [a] here\n";
    markdown_core_document *document;
    markdown_core_string text;
    size_t at;
    size_t line;
    int lines_agree = 1;

    document = markdown_core_document_parse((const uint8_t *)SOURCE, strlen(SOURCE), NULL, NULL);
    check(document != NULL, "the corpus parses");
    if (!document) {
        return;
    }
    text = markdown_core_document_source(document);
    check(text.length == strlen(SOURCE) && memcmp(text.data, SOURCE, text.length) == 0,
          "the source is the normalized source, byte for byte");
    check(markdown_core_document_line_count(document) == 11, "the line index counts the source's lines");
    for (line = 2; line <= markdown_core_document_line_count(document); line++) {
        size_t start = 0;
        if (!markdown_core_document_line_start(document, line, &start) || start == 0 ||
            text.data[start - 1] != (uint8_t)'\n') {
            lines_agree = 0;
        }
    }
    check(lines_agree, "every line but the first begins after a line ending");
    check(markdown_core_document_line_start(document, 1, &at) && at == 0, "line one begins at offset zero");
    check(!markdown_core_document_line_start(document, 0, &at), "line zero is not a line");
    check(!markdown_core_document_line_start(document, 12, &at), "a line past the end is not a line");
    check(markdown_core_document_source(NULL).data == NULL, "a null document has no source");
    check(markdown_core_document_line_count(NULL) == 0, "a null document has no lines");
    markdown_core_document_free(document);
}

/* REQUIREMENT 14 THROUGH THE ACCESSORS, which is where it has to be checked.
 *
 * The dump renders `null` and `""` differently and the goldens pin that, but a
 * dump can only show what it chooses to print: the accessor answers with
 * `has_value`, and a fold reinstated anywhere between the node and the caller
 * would be invisible to a golden that only ever sees the rendering. Both arms
 * of every optional string are asserted here, and so is the fact that a
 * destination has no absent arm at all (Q26). */
static void check_null_and_empty(void) {
    static const struct {
        const char *source;
        markdown_core_node_kind kind;
        const char *destination;
        bool title_written;
        const char *title;
    } CASES[] = {
        {"[a]()\n", MARKDOWN_CORE_KIND_LINK, "", false, ""},
        {"[a](<>)\n", MARKDOWN_CORE_KIND_LINK, "", false, ""},
        {"[a](/u)\n", MARKDOWN_CORE_KIND_LINK, "/u", false, ""},
        {"[a](/u \"\")\n", MARKDOWN_CORE_KIND_LINK, "/u", true, ""},
        {"[a](/u \"t\")\n", MARKDOWN_CORE_KIND_LINK, "/u", true, "t"},
        {"![a]()\n", MARKDOWN_CORE_KIND_IMAGE, "", false, ""},
        {"![a](/s \"\")\n", MARKDOWN_CORE_KIND_IMAGE, "/s", true, ""},
        {"[a]: <>\n", MARKDOWN_CORE_KIND_REFERENCE_DEFINITION, "", false, ""},
        {"[a]: <> \"\"\n", MARKDOWN_CORE_KIND_REFERENCE_DEFINITION, "", true, ""},
        {"[a]: /u \"t\"\n", MARKDOWN_CORE_KIND_REFERENCE_DEFINITION, "/u", true, "t"},
    };
    static const struct {
        const char *source;
        bool info_written;
        const char *info;
    } INFO_CASES[] = {
        {"```\nx\n```\n", false, ""},
        {"```   \nx\n```\n", false, ""},
        {"    x\n", false, ""},
        {"```js\nx\n```\n", true, "js"},
    };
    size_t index;

    for (index = 0; index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)CASES[index].source, strlen(CASES[index].source), NULL, NULL);
        const markdown_core_node *node = NULL;
        markdown_core_string destination = {NULL, 0};
        markdown_core_optional_string title = {false, {NULL, 0}};
        bool read;
        if (!document) {
            check(false, "requirement 14 case parses");
            continue;
        }
        node = markdown_core_node_get_first_child(markdown_core_document_semantic(document));
        if (CASES[index].kind != MARKDOWN_CORE_KIND_REFERENCE_DEFINITION) {
            node = markdown_core_node_get_first_child(node);
        }
        check(markdown_core_node_get_kind(node) == CASES[index].kind, "requirement 14 case has the expected kind");
        read = CASES[index].kind == MARKDOWN_CORE_KIND_LINK
                   ? markdown_core_node_link_properties(node, &destination, &title)
               : CASES[index].kind == MARKDOWN_CORE_KIND_IMAGE
                   ? markdown_core_node_image_properties(node, &destination, &title)
                   : markdown_core_node_definition_resource(node, &destination, &title);
        check(read, "the resource accessor answers");
        /* A DESTINATION IS NEVER ABSENT. There is no `has_value` to test,
         * because the type does not offer one -- that IS the assertion. */
        check(destination.length == strlen(CASES[index].destination) &&
                  (destination.length == 0 ||
                   memcmp(destination.data, CASES[index].destination, destination.length) == 0),
              "a destination is required and empty means empty");
        check(title.has_value == CASES[index].title_written, "presence is what the source wrote, not what it wrote in");
        if (title.has_value) {
            check(
                title.value.length == strlen(CASES[index].title) &&
                    (title.value.length == 0 || memcmp(title.value.data, CASES[index].title, title.value.length) == 0),
                "a written title keeps its bytes, including none of them");
        }
        markdown_core_document_free(document);
    }

    for (index = 0; index < sizeof(INFO_CASES) / sizeof(INFO_CASES[0]); ++index) {
        markdown_core_document *document = markdown_core_document_parse((const uint8_t *)INFO_CASES[index].source,
                                                                        strlen(INFO_CASES[index].source), NULL, NULL);
        const markdown_core_node *node;
        markdown_core_optional_string info = {false, {NULL, 0}};
        markdown_core_optional_string language = {false, {NULL, 0}};
        markdown_core_string literal = {NULL, 0};
        bool fenced = false;
        bool closed = false;
        if (!document) {
            check(false, "requirement 14 info case parses");
            continue;
        }
        node = markdown_core_node_get_first_child(markdown_core_document_semantic(document));
        check(markdown_core_node_code_block_properties(node, &info, &language, &literal, &fenced, &closed),
              "the code-block accessor answers");
        check(info.has_value == INFO_CASES[index].info_written,
              "a fence with only whitespace after it wrote no info string");
        check(language.has_value == info.has_value, "language is present exactly when the info string is");
        if (info.has_value) {
            check(info.value.length == strlen(INFO_CASES[index].info) &&
                      memcmp(info.value.data, INFO_CASES[index].info, info.value.length) == 0,
                  "a written info string keeps its bytes");
        }
        markdown_core_document_free(document);
    }
}

static void check_directive_label_projection(void) {
    static const uint8_t inline_source[] = ":badge[label]\n";
    static const uint8_t bare_source[] = ":badge\n";
    static const uint8_t empty_source[] = ":badge[]\n";
    static const uint8_t block_source[] = ":::note[Title]\nBody\n:::\n";
    markdown_core_document *document;
    const markdown_core_node *root;
    const markdown_core_node *directive;
    const markdown_core_node *label;
    const markdown_core_node *label_child;
    const markdown_core_node *content_child;

    document = markdown_core_document_parse(inline_source, sizeof(inline_source) - 1, NULL, NULL);
    check(document != NULL, "labelled inline directive parses");
    if (document) {
        root = markdown_core_document_semantic(document);
        directive = markdown_core_node_get_first_child(markdown_core_node_get_first_child(root));
        label = markdown_core_node_directive_label(directive);
        label_child = markdown_core_node_get_first_child(label);
        check(markdown_core_node_get_kind(label) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL &&
                  markdown_core_node_get_kind(label_child) == MARKDOWN_CORE_KIND_TEXT &&
                  markdown_core_node_child_count(label) == 1,
              "directive label is an optional Markup-valued field");
        check(markdown_core_node_get_first_child(directive) == NULL && markdown_core_node_child_count(directive) == 0 &&
                  markdown_core_node_get_next_sibling(label) == NULL,
              "an inline directive label is not directive content");
        markdown_core_document_free(document);
    }

    document = markdown_core_document_parse(bare_source, sizeof(bare_source) - 1, NULL, NULL);
    check(document != NULL, "bare inline directive parses");
    if (document) {
        root = markdown_core_document_semantic(document);
        directive = markdown_core_node_get_first_child(markdown_core_node_get_first_child(root));
        check(markdown_core_node_directive_label(directive) == NULL && markdown_core_node_child_count(directive) == 0,
              "an absent directive label remains absent and is not content");
        markdown_core_document_free(document);
    }

    document = markdown_core_document_parse(empty_source, sizeof(empty_source) - 1, NULL, NULL);
    check(document != NULL, "empty-label directive parses");
    if (document) {
        root = markdown_core_document_semantic(document);
        directive = markdown_core_node_get_first_child(markdown_core_node_get_first_child(root));
        label = markdown_core_node_directive_label(directive);
        check(markdown_core_node_get_kind(label) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL &&
                  markdown_core_node_child_count(label) == 0,
              "an empty label remains distinct from an absent label");
        check(markdown_core_node_child_count(directive) == 0, "an empty directive label is not directive content");
        markdown_core_document_free(document);
    }

    document = markdown_core_document_parse(block_source, sizeof(block_source) - 1, NULL, NULL);
    check(document != NULL, "labelled block directive parses");
    if (document) {
        root = markdown_core_document_semantic(document);
        directive = markdown_core_node_get_first_child(root);
        label = markdown_core_node_directive_label(directive);
        check(markdown_core_node_get_kind(label) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL &&
                  markdown_core_node_child_count(label) == 1,
              "block directive exposes its label through the field accessor");
        content_child = markdown_core_node_get_first_child(directive);
        check(markdown_core_node_get_kind(content_child) == MARKDOWN_CORE_KIND_PARAGRAPH,
              "block directive children contain only block content");
        check(markdown_core_node_get_next_sibling(label) == NULL && markdown_core_node_child_count(directive) == 1,
              "block directive label is not a sibling of its content");
        markdown_core_document_free(document);
    }
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
    check(markdown_core_error_get_message(error).length != 0, "error exposes a UTF-8 message");
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
    check_null_and_empty();
    check_directive_label_projection();
    check_source_and_lines();
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
