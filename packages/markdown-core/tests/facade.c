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

static void check_fixture(const char *fixture_dir, const char *name) {
    char markdown_path[1024];
    char ast_path[1024];
    uint8_t *markdown;
    uint8_t *expected;
    uint8_t *actual = NULL;
    size_t markdown_length = 0, expected_length = 0, actual_length = 0;
    markdown_core_document *document;
    markdown_core_error *error = NULL;

    snprintf(markdown_path, sizeof(markdown_path), "%s/%s.md", fixture_dir, name);
    snprintf(ast_path, sizeof(ast_path), "%s/%s.ast", fixture_dir, name);
    markdown = read_file(markdown_path, &markdown_length);
    expected = read_file(ast_path, &expected_length);
    check(markdown != NULL && expected != NULL, "fixture files are readable");
    if (!markdown || !expected) {
        goto done;
    }

    /* The manifest names no option: every case is the one dialect. */
    document = markdown_core_document_parse(markdown, markdown_length, &error);
    check(document != NULL && error == NULL, "facade parse succeeds");
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
        /* M2: a resolved reference answers what its definition stated,
         * through the same accessors, and the definition is not a node. */
        {"[a]: <>\n\n[a]\n", MARKDOWN_CORE_KIND_LINK, "", false, ""},
        {"[a]: <> \"\"\n\n[a][]\n", MARKDOWN_CORE_KIND_LINK, "", true, ""},
        {"[a]: /u \"t\"\n\n[x][a]\n", MARKDOWN_CORE_KIND_LINK, "/u", true, "t"},
        {"![a][r]\n\n[r]: /s \"\"\n", MARKDOWN_CORE_KIND_IMAGE, "/s", true, ""},
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
            markdown_core_document_parse((const uint8_t *)CASES[index].source, strlen(CASES[index].source), NULL);
        const markdown_core_node *node = NULL;
        markdown_core_string destination = {NULL, 0};
        markdown_core_optional_string title = {false, {NULL, 0}};
        markdown_core_destination tagged;
        bool read;
        if (!document) {
            check(false, "requirement 14 case parses");
            continue;
        }
        node = markdown_core_node_get_first_child(markdown_core_document_root(document));
        node = markdown_core_node_get_first_child(node);
        check(markdown_core_node_get_kind(node) == CASES[index].kind, "requirement 14 case has the expected kind");
        /* M1: a link or image answers the tagged `Destination`, and every one
         * the inherited grammar produces is the `url` branch, with the other
         * branch's fields zeroed rather than left over. */
        read = markdown_core_node_destination(node, &tagged) && markdown_core_node_title(node, &title);
        check(read && tagged.kind == MARKDOWN_CORE_DESTINATION_URL, "a link or image destination is the url branch");
        check(tagged.path.data == NULL && tagged.path.length == 0 && !tagged.anchor.has_value,
              "the cross branch's fields are zeroed on a url destination");
        destination = tagged.url;
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
                                                                        strlen(INFO_CASES[index].source), NULL);
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
        node = markdown_core_node_get_first_child(markdown_core_document_root(document));
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

/* M2: every occurrence that resolved through one definition shares one
 * resource, and the identity says so; a direct link, a direct image and an
 * autolink each own one, and every other kind has none. */
static void check_resource_identity(void) {
    static const char source[] = "[a][r] [r][] [r] ![i][r] [d](/r) <https://x.y> [none]\n\n[r]: /r\n";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    const markdown_core_node *paragraph;
    const markdown_core_node *child;
    const markdown_core_resource *shared = NULL;
    const markdown_core_resource *direct = NULL;
    const markdown_core_resource *autolink = NULL;
    int occurrences = 0;
    int others = 0;
    if (!document) {
        check(false, "resource identity corpus parses");
        return;
    }
    paragraph = markdown_core_node_get_first_child(markdown_core_document_root(document));
    check(markdown_core_node_resource(markdown_core_document_root(document)) == NULL &&
              markdown_core_node_resource(paragraph) == NULL && markdown_core_node_resource(NULL) == NULL,
          "a kind with no destination has no resource");
    for (child = markdown_core_node_get_first_child(paragraph); child;
         child = markdown_core_node_get_next_sibling(child)) {
        const markdown_core_resource *resource = markdown_core_node_resource(child);
        markdown_core_node_kind kind = markdown_core_node_get_kind(child);
        if (kind != MARKDOWN_CORE_KIND_LINK && kind != MARKDOWN_CORE_KIND_IMAGE) {
            check(resource == NULL, "a text node has no resource");
            others++;
            continue;
        }
        check(resource != NULL, "every link and image answers a resource");
        if (occurrences < 4) {
            /* The three link forms and the image reference name one
             * definition and share one resource. */
            if (occurrences == 0) {
                shared = resource;
            }
            check(resource == shared, "every occurrence of one definition shares its resource");
        } else if (occurrences == 4) {
            direct = resource;
            check(direct != shared, "a direct link owns a resource of its own");
        } else {
            autolink = resource;
            check(autolink != shared && autolink != direct, "an autolink owns a resource of its own");
        }
        occurrences++;
    }
    check(occurrences == 6 && others > 0, "the corpus holds four occurrences, a direct link and an autolink");
    markdown_core_document_free(document);
}

static void check_callout_fields(void) {
    /* M3: every `>` container is a `Callout` that reads as metadata-free --
     * an absent variant, `none`, and no title -- through the facade, and the
     * accessors answer nothing for another kind. */
    static const char source[] = "> quote\n\ntext\n";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    const markdown_core_node *callout;
    const markdown_core_node *paragraph;
    markdown_core_optional_string variant = {true, {(const uint8_t *)"x", 1}};
    markdown_core_callout_fold fold = MARKDOWN_CORE_CALLOUT_FOLD_COLLAPSED;
    if (!document) {
        check(false, "callout corpus parses");
        return;
    }
    callout = markdown_core_node_get_first_child(markdown_core_document_root(document));
    paragraph = markdown_core_node_get_next_sibling(callout);
    check(markdown_core_node_get_kind(callout) == MARKDOWN_CORE_KIND_CALLOUT, "a `>` container is a Callout");
    check(strcmp(markdown_core_node_kind_name(MARKDOWN_CORE_KIND_CALLOUT), "Callout") == 0,
          "the kind is named Callout");
    check(markdown_core_node_callout_properties(callout, &variant, &fold), "a callout answers its properties");
    check(!variant.has_value && variant.value.length == 0, "a `>` container has no variant");
    check(fold == MARKDOWN_CORE_CALLOUT_FOLD_NONE, "a `>` container has no fold marker");
    check(markdown_core_node_callout_title(callout) == NULL, "a `>` container has no title");
    check(!markdown_core_node_callout_properties(paragraph, &variant, &fold), "a paragraph has no callout properties");
    check(!markdown_core_node_callout_properties(callout, NULL, &fold), "properties need a variant out-parameter");
    check(!markdown_core_node_callout_properties(callout, &variant, NULL), "properties need a fold out-parameter");
    check(markdown_core_node_callout_title(paragraph) == NULL && markdown_core_node_callout_title(NULL) == NULL,
          "only a callout may have a title");
    markdown_core_document_free(document);
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

    document = markdown_core_document_parse(inline_source, sizeof(inline_source) - 1, NULL);
    check(document != NULL, "labelled inline directive parses");
    if (document) {
        root = markdown_core_document_root(document);
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

    document = markdown_core_document_parse(bare_source, sizeof(bare_source) - 1, NULL);
    check(document != NULL, "bare inline directive parses");
    if (document) {
        root = markdown_core_document_root(document);
        directive = markdown_core_node_get_first_child(markdown_core_node_get_first_child(root));
        check(markdown_core_node_directive_label(directive) == NULL && markdown_core_node_child_count(directive) == 0,
              "an absent directive label remains absent and is not content");
        markdown_core_document_free(document);
    }

    document = markdown_core_document_parse(empty_source, sizeof(empty_source) - 1, NULL);
    check(document != NULL, "empty-label directive parses");
    if (document) {
        root = markdown_core_document_root(document);
        directive = markdown_core_node_get_first_child(markdown_core_node_get_first_child(root));
        label = markdown_core_node_directive_label(directive);
        check(markdown_core_node_get_kind(label) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL &&
                  markdown_core_node_child_count(label) == 0,
              "an empty label remains distinct from an absent label");
        check(markdown_core_node_child_count(directive) == 0, "an empty directive label is not directive content");
        markdown_core_document_free(document);
    }

    document = markdown_core_document_parse(block_source, sizeof(block_source) - 1, NULL);
    check(document != NULL, "labelled block directive parses");
    if (document) {
        root = markdown_core_document_root(document);
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

/* EVERY FEATURE, ALWAYS, through the one public entry: the dialect has no
 * switch, so the proof that a feature is on is that its node comes out of a
 * parse that was handed nothing but bytes. One witness per registered
 * feature, so that a feature which stopped being attached would fail here
 * and not only in a fixture. */
static void check_dialect_is_whole(void) {
    static const struct {
        const char *source;
        const char *witness;
    } WITNESSES[] = {
        {"| a |\n| --- |\n| b |\n", "Table scope="},
        {"~~x~~\n", "Strikethrough scope="},
        {"www.example.com\n", "Link scope="},
        {"- [x] task\n", "checked=true"},
        {"ref[^a]\n\n[^a]: note\n", "FootnoteReference scope="},
        {"$x$\n", "Formula scope="},
        {":badge[label]\n", "Directive scope="},
        {"before <!-- kept --> after\n", "Comment scope=1:8..1:20 literal=\" kept \""},
        /* No smart punctuation: quotation marks, hyphen runs, and periods are
         * stored as written. */
        {"\"quotes\" 'single' -- --- ... a\n", "literal=\"\\\"quotes\\\" 'single' -- --- ... a\""},
    };
    size_t index;
    for (index = 0; index < sizeof(WITNESSES) / sizeof(WITNESSES[0]); ++index) {
        markdown_core_error *error = NULL;
        markdown_core_document *document = markdown_core_document_parse((const uint8_t *)WITNESSES[index].source,
                                                                        strlen(WITNESSES[index].source), &error);
        uint8_t *dump = NULL;
        size_t length = 0;
        check(document != NULL && error == NULL, "dialect witness parses");
        if (!document) {
            markdown_core_error_free(error);
            continue;
        }
        check(markdown_core_document_dump(document, &dump, &length, &error), "dialect witness dumps");
        if (dump) {
            check(strstr((const char *)dump, WITNESSES[index].witness) != NULL,
                  "every feature of the dialect is recognized by a parse that was handed nothing but bytes");
        }
        markdown_core_dump_free(dump);
        markdown_core_document_free(document);
        markdown_core_error_free(error);
    }
}

static void check_api(void) {
    static const uint8_t source[] = "# Heading\n\n- [ ] task\n";
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    const markdown_core_node *root;
    const markdown_core_node *heading;
    markdown_core_scope scope;
    int32_t level = 0;

    document = markdown_core_document_parse(source, sizeof(source) - 1, &error);
    check(document != NULL && error == NULL, "parse succeeds");
    if (document) {
        root = markdown_core_document_root(document);
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

    document = markdown_core_document_parse(NULL, 1, &error);
    check(document == NULL && error != NULL, "invalid input produces an explicit error");
    check(markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "error exposes a stable code");
    check(markdown_core_error_get_message(error).length != 0, "error exposes a UTF-8 message");
    markdown_core_error_free(error);
    markdown_core_error_free(NULL);
    markdown_core_document_free(NULL);
    markdown_core_dump_free(NULL);
}

int main(int argc, char **argv) {
    const char *fixture_dir;
    int i;
    if (argc < 4 || strcmp(argv[1], "--fixtures") != 0) {
        fputs("usage: facade_test --fixtures DIR NAME [NAME ...]\n", stderr);
        return 2;
    }
    fixture_dir = argv[2];
    check_api();
    check_dialect_is_whole();
    check_null_and_empty();
    check_resource_identity();
    check_callout_fields();
    check_directive_label_projection();
    for (i = 3; i < argc; i++) {
        check_fixture(fixture_dir, argv[i]);
    }
    if (failures) {
        fprintf(stderr, "%d facade test(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "native facade and canonical AST goldens passed\n");
    return 0;
}
