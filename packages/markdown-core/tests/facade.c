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
static void check_native_coordinate_contract(void) {
    static const struct {
        const char *source;
        int end_line, end_column;
    } cases[] = {{"", 0, 0}, {"\n", 1, 0}, {"\r\n", 1, 0}, {"é", 1, 2}, {"🚀", 1, 4}, {"a\r\nb", 2, 1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)cases[i].source, strlen(cases[i].source), NULL);
        check(document != NULL, "native coordinate witness parses");
        markdown_core_scope scope = markdown_core_node_scope(markdown_core_document_root(document));
        check(scope.start.line == 1 && scope.start.column == 1 && scope.end.line == cases[i].end_line &&
                  scope.end.column == cases[i].end_column,
              "UTF-8 coordinates and empty-input sentinels are preserved verbatim");
        markdown_core_document_free(document);
    }
}

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
        {"![a]()\n", MARKDOWN_CORE_KIND_MEDIA, "", false, ""},
        {"![a](/s \"\")\n", MARKDOWN_CORE_KIND_MEDIA, "/s", true, ""},
        /* M2: a resolved reference answers what its definition stated,
         * through the same accessors, and the definition is not a node. */
        {"[a]: <>\n\n[a]\n", MARKDOWN_CORE_KIND_LINK, "", false, ""},
        {"[a]: <> \"\"\n\n[a][]\n", MARKDOWN_CORE_KIND_LINK, "", true, ""},
        {"[a]: /u \"t\"\n\n[x][a]\n", MARKDOWN_CORE_KIND_LINK, "/u", true, "t"},
        {"![a][r]\n\n[r]: /s \"\"\n", MARKDOWN_CORE_KIND_MEDIA, "/s", true, ""},
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

static void check_image_dimensions(void) {
    const char *source = "![*alt*|2147483647x2][r] ![3][r] ![bad|01][r]\n\n[r]: /shared\n";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    check(document != NULL, "dimensioned image references parse");
    if (!document) {
        return;
    }
    const markdown_core_node *paragraph = markdown_core_node_get_first_child(markdown_core_document_root(document));
    const markdown_core_resource *shared = NULL;
    int index = 0;
    for (const markdown_core_node *node = markdown_core_node_get_first_child(paragraph); node;
         node = markdown_core_node_get_next_sibling(node)) {
        if (markdown_core_node_get_kind(node) != MARKDOWN_CORE_KIND_MEDIA) {
            continue;
        }
        const markdown_core_dimensions *dimensions = markdown_core_node_dimensions(node);
        check((dimensions != NULL) == (index < 2), "dimension presence is per image");
        if (dimensions) {
            check(dimensions->width == (index == 0 ? INT32_MAX : 3), "parsed width is exact");
            check(dimensions->height.has_value == (index == 0), "height is optional inside dimensions");
        }
        if (index == 0) {
            check(dimensions && dimensions->height.value == 2, "parsed height is exact");
            shared = markdown_core_node_resource(node);
        }
        check(shared == markdown_core_node_resource(node), "dimensions never split a shared destination");
        if (index == 1) {
            check(markdown_core_node_get_first_child(node) == NULL, "numeric-only alt has no children");
        }
        index++;
    }
    check(index == 3, "all dimensioned and malformed occurrences remain images");
    markdown_core_document_free(document);
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
        if (kind != MARKDOWN_CORE_KIND_LINK && kind != MARKDOWN_CORE_KIND_MEDIA) {
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
     * an absent variant, an absent fold marker, and no title -- through the
     * facade, and the accessors answer nothing for another kind. */
    static const char source[] = "> quote\n\ntext\n";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    const markdown_core_node *callout;
    const markdown_core_node *paragraph;
    markdown_core_optional_string variant = {true, {(const uint8_t *)"x", 1}};
    markdown_core_optional_bool collapsed = {true, true};
    if (!document) {
        check(false, "callout corpus parses");
        return;
    }
    callout = markdown_core_node_get_first_child(markdown_core_document_root(document));
    paragraph = markdown_core_node_get_next_sibling(callout);
    check(markdown_core_node_get_kind(callout) == MARKDOWN_CORE_KIND_CALLOUT, "a `>` container is a Callout");
    check(strcmp(markdown_core_node_kind_name(MARKDOWN_CORE_KIND_CALLOUT), "Callout") == 0,
          "the kind is named Callout");
    check(markdown_core_node_callout_properties(callout, &variant, &collapsed), "a callout answers its properties");
    check(!variant.has_value && variant.value.length == 0, "a `>` container has no variant");
    check(!collapsed.has_value && !collapsed.value, "a `>` container has no fold marker");
    check(markdown_core_node_callout_title(callout) == NULL, "a `>` container has no title");
    check(!markdown_core_node_callout_properties(paragraph, &variant, &collapsed),
          "a paragraph has no callout properties");
    check(!markdown_core_node_callout_properties(callout, NULL, &collapsed), "properties need a variant out-parameter");
    check(!markdown_core_node_callout_properties(callout, &variant, NULL), "properties need a collapsed out-parameter");
    check(markdown_core_node_callout_title(paragraph) == NULL && markdown_core_node_callout_title(NULL) == NULL,
          "only a callout may have a title");
    markdown_core_document_free(document);
}

static size_t count_occurrences(const char *text, const char *needle) {
    size_t count = 0;
    size_t step = strlen(needle);
    for (text = strstr(text, needle); text; text = strstr(text + step, needle)) {
        count++;
    }
    return count;
}

static void check_callout_source_boundaries(void) {
    static const char source[] = "\xEF\xBB\xBF> [!note]- T  \r\n> body";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, sizeof(source) - 1, NULL);
    check(document != NULL, "callout parses BOM, CRLF, trailing spaces and EOF without newline");
    if (!document) {
        return;
    }
    const markdown_core_node *callout = markdown_core_node_get_first_child(markdown_core_document_root(document));
    const markdown_core_node *title = markdown_core_node_callout_title(callout);
    markdown_core_string literal;
    check(title && markdown_core_node_literal(title, &literal) && literal.length == 1 && literal.data[0] == 'T',
          "trailing title spaces never create a break or title text");
    check(title && !markdown_core_node_get_next_sibling(title), "title contains exactly one node");
    markdown_core_scope title_scope = markdown_core_node_scope(title);
    check(title_scope.start.line == 1 && title_scope.start.column == 15 && title_scope.end.column == 15,
          "title scope uses original byte columns after BOM and metadata");
    const markdown_core_node *body = markdown_core_node_get_first_child(callout);
    markdown_core_scope body_scope = markdown_core_node_scope(body);
    check(body && body_scope.start.line == 2 && body_scope.start.column == 3 && body_scope.end.column == 6,
          "body scope starts after its quote prefix and reaches EOF");
    markdown_core_document_free(document);
}

static void check_callout_inherited_setext_scope(void) {
    static const char source[] = "> [!note] T\n> head\n> ===\n\nnext\n";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, sizeof(source) - 1, NULL);
    check(document != NULL, "callout body Setext heading parses");
    if (!document) {
        return;
    }
    const markdown_core_node *callout = markdown_core_node_get_first_child(markdown_core_document_root(document));
    const markdown_core_node *heading = markdown_core_node_get_first_child(callout);
    markdown_core_scope scope = markdown_core_node_scope(heading);
    check(markdown_core_node_get_kind(heading) == MARKDOWN_CORE_KIND_HEADING && scope.start.line == 2 &&
              scope.start.column == 3 && scope.end.line == 3 && scope.end.column == 5,
          "callout Setext scope ends on the underline before a following blank line");
    markdown_core_document_free(document);
}

static void check_citation_model(void) {
    /* M4: repeated calls share one footnote, a later definition of the same
     * id is a footnote after the winner, and the dump nests each value under
     * its owner: items under the cite, footnotes after the content. */
    static const char source[] = "[^a] [^a]\n\n[^a]: once\n\n[^a]: twice\n";
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, strlen(source), &error);
    uint8_t *dump = NULL;
    size_t length = 0;
    check(document != NULL, "citation corpus parses");
    if (!document) {
        return;
    }
    check(markdown_core_document_dump(document, &dump, &length, &error), "citation corpus dumps");
    if (dump) {
        const char *text = (const char *)dump;
        check(count_occurrences(text, "Cite scope=") == 2, "every defined call is a Cite");
        check(count_occurrences(text, "referent=footnote(id=\"a\") children=0\n") == 2,
              "every item names the footnote by id");
        check(count_occurrences(text, "CitationPrefix children=0\n") == 2 &&
                  count_occurrences(text, "CitationSuffix children=0\n") == 2,
              "an inherited call has empty affix groups");
        check(count_occurrences(text, "Footnote scope=") == 2, "both definitions are footnotes");
        check(strstr(text, "\n├── Footnote scope=3:1..4:0 id=\"a\" children=1\n") != NULL,
              "the winning definition is the first footnote");
        check(strstr(text, "\n└── Footnote scope=5:1..5:11 id=\"a\" children=1\n") != NULL,
              "the later definition is the footnote after it, nested last under the document");
        markdown_core_dump_free(dump);
    }
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
        {"- [x] task\n", "marker=\"x\""},
        {"ref[^a]\n\n[^a]: note\n", "Cite scope="},
        {"$x$\n", "Formula scope="},
        {":badge[label]\n", "Directive scope="},
        {"before <!-- kept --> after\n", "Comment scope=1:8..1:20 anchor=null attributes={} literal=\" kept \""},
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

static void check_table_model(void) {
    static const uint8_t input[] = "| h | center | right | plain |\n| :-- | :-: | --: | -- |\n| x\\|y | `\\|` | z |\n";
    markdown_core_document *document = markdown_core_document_parse(input, sizeof(input) - 1, NULL);
    check(document != NULL, "table parses");
    if (!document) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *table = markdown_core_node_get_first_child(root);
    size_t columns = 0, head = 0, content = 0, foot = 0;
    check(markdown_core_node_table_properties(table, &columns, &head, &content, &foot), "table properties");
    check(columns == 4 && head == 1 && content == 1 && foot == 0, "pipe table group partition");
    check(markdown_core_node_child_count(table) == head + content + foot, "table rows have one structural owner");
    const markdown_core_table_alignment expected[] = {
        MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT, MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER, MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT,
        MARKDOWN_CORE_TABLE_ALIGNMENT_NONE};
    for (size_t i = 0; i < columns; i++) {
        markdown_core_table_column column;
        check(markdown_core_node_table_column_at(table, i, &column), "column value");
        check(column.alignment == expected[i] && !column.relative.has_value, "column authored facts");
    }
    markdown_core_table_column column = {0};
    check(!markdown_core_node_table_column_at(table, columns, &column), "column upper bound");
    check(!markdown_core_node_table_column_at(table, 0, NULL), "column null output");
    check(!markdown_core_node_table_properties(root, &columns, &head, &content, &foot), "table kind boundary");
    check(!markdown_core_node_table_properties(table, NULL, &head, &content, &foot), "table null output");
    const markdown_core_node *row = markdown_core_node_get_first_child(table);
    for (; row; row = markdown_core_node_get_next_sibling(row)) {
        check(markdown_core_node_child_count(row) == 4, "pipe rows have every logical column");
        const markdown_core_node *cell = markdown_core_node_get_first_child(row);
        for (; cell; cell = markdown_core_node_get_next_sibling(cell)) {
            int64_t rowspan = 0, colspan = 0;
            check(markdown_core_node_table_cell_spans(cell, &rowspan, &colspan), "cell spans");
            check(rowspan == 1 && colspan == 1, "inherited unit spans");
        }
    }
    int64_t rowspan, colspan;
    check(!markdown_core_node_table_cell_spans(table, &rowspan, &colspan), "span kind boundary");
    markdown_core_document_free(document);
}

static void check_definition_model(void) {
    static const uint8_t input[] = "::: box\n*T*\n: one\n~\n\nU\n\n: two\n:::\n\n:::named\n:::\n";
    markdown_core_document *document = markdown_core_document_parse(input, sizeof(input) - 1, NULL);
    check(document != NULL, "definition model parses");
    if (!document) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *block = markdown_core_node_get_first_child(root);
    const markdown_core_node *list = markdown_core_node_get_first_child(block);
    const markdown_core_node *definition = markdown_core_node_get_first_child(list);
    markdown_core_optional_string name;
    check(markdown_core_node_directive_properties(block, &name) && !name.has_value, "nameless block name is absent");
    check(markdown_core_node_directive_properties(markdown_core_node_get_next_sibling(block), &name) &&
              name.has_value && name.value.length == 5,
          "named block retains its name");
    check(!markdown_core_node_directive_properties(root, &name) &&
              !markdown_core_node_directive_properties(block, NULL),
          "directive property kind and output boundaries");
    check(markdown_core_node_get_kind(list) == MARKDOWN_CORE_KIND_DEFINITION_LIST &&
              markdown_core_node_child_count(list) == 2,
          "definition list has typed members");
    check(markdown_core_node_get_kind(definition) == MARKDOWN_CORE_KIND_DEFINITION, "definition kind");
    check(markdown_core_node_child_count(definition) == 0 && !markdown_core_node_get_first_child(definition),
          "body collection roots never enter generic Markup traversal");
    bool compact = false;
    check(markdown_core_node_definition_compact(definition, &compact) && compact, "compact term gap");
    check(markdown_core_node_definition_compact(markdown_core_node_get_next_sibling(definition), &compact) && !compact,
          "loose term gap");
    check(markdown_core_node_get_kind(markdown_core_node_definition_term(definition)) == MARKDOWN_CORE_KIND_EMPHASIS,
          "term is a separate inline field");
    const markdown_core_definition_body *body = markdown_core_node_definition_bodies(definition);
    check(body &&
              markdown_core_node_get_kind(markdown_core_definition_body_content(body)) == MARKDOWN_CORE_KIND_PARAGRAPH,
          "first body exposes ordinary blocks");
    body = markdown_core_definition_body_next(body);
    check(body && !markdown_core_definition_body_content(body) && !markdown_core_definition_body_next(body),
          "empty second body retains its position");
    check(!markdown_core_node_definition_compact(root, &compact) &&
              !markdown_core_node_definition_compact(definition, NULL),
          "definition property kind and output boundaries");
    check(!markdown_core_node_definition_term(root) && !markdown_core_node_definition_bodies(root),
          "definition collections reject other kinds");
    check(!markdown_core_definition_body_next(NULL) && !markdown_core_definition_body_content(NULL),
          "null body cursor is empty");
    markdown_core_document_free(document);
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
    check_table_model();
    check_definition_model();
    check_dialect_is_whole();
    check_native_coordinate_contract();
    check_null_and_empty();
    check_resource_identity();
    check_image_dimensions();
    check_callout_fields();
    check_callout_source_boundaries();
    check_callout_inherited_setext_scope();
    check_citation_model();
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
