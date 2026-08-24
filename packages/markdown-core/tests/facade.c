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
    markdown_core_string_view text;
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

/* REQUIREMENT 13 through the PUBLIC surface, which is the only place the law's
 * two halves meet: a document that carries a diagnostic list, and an error that
 * carries no scope because there is no document at all.
 *
 * The `label-too-long` code is exercised HERE and not in a fixture, and the
 * reason is the label: the cap is 1000 bytes, so a golden that showed it would
 * carry an eleven-hundred-character line that no reviewer can read and every
 * position oracle would then measure. Built in C it is three lines. */
static void check_diagnostics(void) {
    static const char *const SOURCE = ":note[unclosed label\n"
                                      "\n"
                                      ":other{=value}\n"
                                      "\n"
                                      "see [text][undefined] here\n"
                                      "\n"
                                      "and [^undefined] too\n";
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    markdown_core_diagnostic diagnostic;
    size_t count;
    size_t i;
    int severities[3] = {0, 0, 0};

    document = markdown_core_document_parse((const uint8_t *)SOURCE, strlen(SOURCE), NULL, &error);
    check(document != NULL && error == NULL, "the diagnostics corpus parses");
    if (!document) {
        markdown_core_error_free(error);
        return;
    }

    count = markdown_core_document_diagnostic_count(document);
    check(count == 4, "every degradation in the corpus is reported exactly once");

    for (i = 0; i < count; i++) {
        check(markdown_core_document_diagnostic_at(document, i, &diagnostic), "every index in range answers");
        /* THE SCOPE IS RESOLVABLE WITHOUT A NODE HANDLE, which is what
         * requirement 13 depends on 12 for: the line index turns the place
         * back into an offset in the source the concrete view publishes. */
        {
            size_t offset = 0;
            check(markdown_core_document_line_start(document, (size_t)diagnostic.scope.start.line, &offset),
                  "a diagnostic's line is a line of the source");
            check(offset + (size_t)diagnostic.scope.start.column - 1 < markdown_core_document_source(document).length,
                  "a diagnostic's start is a byte of the source");
        }
        check(diagnostic.message.length > 0 && diagnostic.message.data != NULL, "a diagnostic carries a message");
        check(markdown_core_diagnostic_code_name(diagnostic.code) != NULL, "every code has a name");
        if (diagnostic.severity >= MARKDOWN_CORE_DIAGNOSTIC_WARNING &&
            diagnostic.severity <= MARKDOWN_CORE_DIAGNOSTIC_ERROR) {
            severities[diagnostic.severity]++;
        }
    }
    check(severities[MARKDOWN_CORE_DIAGNOSTIC_WARNING] == 2 && severities[MARKDOWN_CORE_DIAGNOSTIC_ERROR] == 2,
          "both severities are reachable, and each says what its rule says");
    check(!markdown_core_document_diagnostic_at(document, count, &diagnostic), "an index past the end is refused");
    check(!markdown_core_document_diagnostic_at(document, 0, NULL), "a null out-parameter is refused");
    markdown_core_document_free(document);

    /* THE ENGINE'S OWN CAP, and the code that separates it from "no such
     * definition": the author's label is well formed and this library refused
     * to look it up. */
    {
        char *source = (char *)malloc(1200);
        markdown_core_document *capped;
        check(source != NULL, "the long-label buffer allocates");
        if (source) {
            memcpy(source, "see [", 5);
            memset(source + 5, 'a', 1100);
            memcpy(source + 1105, "][] here\n", 9);
            capped = markdown_core_document_parse((const uint8_t *)source, 1114, NULL, NULL);
            check(capped != NULL, "an over-long label still parses");
            if (capped) {
                check(markdown_core_document_diagnostic_count(capped) == 1 &&
                          markdown_core_document_diagnostic_at(capped, 0, &diagnostic) &&
                          diagnostic.code == MARKDOWN_CORE_DIAGNOSTIC_LABEL_TOO_LONG,
                      "a label the engine refused as too long says so, and does not say 'undefined'");
                markdown_core_document_free(capped);
            }
            free(source);
        }
    }

    /* THE CONVERSE. A parse failure is not a diagnostic: there is no document,
     * so there is no list to read, and the error carries no scope. */
    {
        markdown_core_scope scope;
        markdown_core_error *refusal = NULL;
        markdown_core_document *none = markdown_core_document_parse(NULL, 8, NULL, &refusal);
        check(none == NULL && refusal != NULL, "an invalid argument produces an error and no document");
        check(!markdown_core_error_get_scope(refusal, &scope), "a parse failure carries no scope");
        markdown_core_error_free(refusal);
    }
    check(markdown_core_document_diagnostic_count(NULL) == 0, "a null document has no diagnostics");
    check(markdown_core_diagnostic_code_name((markdown_core_diagnostic_code)99) == NULL,
          "a code no version defines has no name");
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
    check_source_and_lines();
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
