#include <stdbool.h>
#include <stdint.h>
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

/* A call the case expects to succeed. */
static bool ok(markdown_core_status status, const char *message) {
    check(status == MARKDOWN_CORE_OK, message);
    return status == MARKDOWN_CORE_OK;
}

/* The document parsed from `length` bytes of `source`, or NULL, having
 * reported the failure. */
static markdown_core_document *parse(const void *source, size_t length) {
    markdown_core_document *document = NULL;
    return ok(markdown_core_document_parse((const uint8_t *)source, length, &document), "facade parse succeeds")
               ? document
               : NULL;
}

/* A node-valued field, which the case expects its accessor to answer. */
typedef markdown_core_status (*node_field)(const markdown_core_node *node, const markdown_core_node **field);

static const markdown_core_node *field(const markdown_core_node *node, node_field accessor) {
    const markdown_core_node *value = NULL;
    ok(accessor(node, &value), "a node-valued field of the node's kind answers");
    return value;
}

/* A sequence-valued field, which the case expects its accessor to answer. */
typedef markdown_core_status (*nodes_field)(const markdown_core_node *node, const markdown_core_nodes **field);

static const markdown_core_nodes *sequence(const markdown_core_node *node, nodes_field accessor) {
    const markdown_core_nodes *value = NULL;
    ok(accessor(node, &value), "a sequence-valued field of the node's kind answers");
    return value;
}

/* The member of `nodes` at `index`, or NULL at or past its count, which the
 * sequence must answer as out of bounds. */
static const markdown_core_node *member(const markdown_core_nodes *nodes, size_t index) {
    const markdown_core_node *node = NULL;
    markdown_core_status status = markdown_core_nodes_at(nodes, index, &node);
    check(status == (index < markdown_core_nodes_count(nodes) ? MARKDOWN_CORE_OK : MARKDOWN_CORE_OUT_OF_BOUNDS),
          "a sequence answers an index below its count and refuses one at or past it");
    return status == MARKDOWN_CORE_OK ? node : NULL;
}

static const markdown_core_node *child_at(const markdown_core_node *node, size_t index) {
    return member(markdown_core_node_children(node), index);
}

static size_t children_count(const markdown_core_node *node) {
    return markdown_core_nodes_count(markdown_core_node_children(node));
}

/* Whether `node` is a member of `nodes`. */
static bool holds(const markdown_core_nodes *nodes, const markdown_core_node *node) {
    for (size_t index = 0; index < markdown_core_nodes_count(nodes); index++) {
        if (member(nodes, index) == node) {
            return true;
        }
    }
    return false;
}

/* The scope of `node`, computed from the source its document was parsed from. */
static markdown_core_scope scope_in(const markdown_core_document *document, const markdown_core_node *node,
                                    const void *source, size_t length) {
    markdown_core_scope scope = {{-1, -1}, {-1, -1}};
    ok(markdown_core_document_scope(document, node, (const uint8_t *)source, length, &scope),
       "the scope query answers for a node of the document");
    return scope;
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

    snprintf(markdown_path, sizeof(markdown_path), "%s/%s.md", fixture_dir, name);
    snprintf(ast_path, sizeof(ast_path), "%s/%s.ast", fixture_dir, name);
    markdown = read_file(markdown_path, &markdown_length);
    expected = read_file(ast_path, &expected_length);
    check(markdown != NULL && expected != NULL, "fixture files are readable");
    if (!markdown || !expected) {
        goto done;
    }

    /* The manifest names no option: every case is the one dialect. */
    document = parse(markdown, markdown_length);
    if (!document) {
        goto done;
    }
    ok(markdown_core_document_dump(document, markdown_core_document_root(document), markdown, markdown_length, &actual,
                                   &actual_length),
       "native AST dump succeeds");
    if (actual && (actual_length != expected_length || memcmp(actual, expected, expected_length) != 0)) {
        fprintf(stderr, "FAILED: %s dump differs from reviewed golden\n", name);
        fwrite(actual, 1, actual_length, stderr);
        failures++;
    }
    markdown_core_dump_free(actual);
    markdown_core_document_free(document);

done:
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
    } cases[] = {{"", 1, 0}, {"\n", 1, 0}, {"\r\n", 1, 0}, {"é", 1, 2}, {"🚀", 1, 4}, {"a\r\nb", 2, 1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        markdown_core_document *document = parse(cases[i].source, strlen(cases[i].source));
        if (!document) {
            continue;
        }
        markdown_core_scope scope =
            scope_in(document, markdown_core_document_root(document), cases[i].source, strlen(cases[i].source));
        check(scope.start.line == 1 && scope.start.column == 1 && scope.end.line == cases[i].end_line &&
                  scope.end.column == cases[i].end_column,
              "UTF-8 coordinates follow the byte rule, the empty input included");
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
        {"![a]()\n", MARKDOWN_CORE_KIND_EMBEDDED, "", false, ""},
        {"![a](/s \"\")\n", MARKDOWN_CORE_KIND_EMBEDDED, "/s", true, ""},
        /* M2: a resolved reference answers what its definition stated,
         * through the same accessors, and the definition is not a node. */
        {"[a]: <>\n\n[a]\n", MARKDOWN_CORE_KIND_LINK, "", false, ""},
        {"[a]: <> \"\"\n\n[a][]\n", MARKDOWN_CORE_KIND_LINK, "", true, ""},
        {"[a]: /u \"t\"\n\n[x][a]\n", MARKDOWN_CORE_KIND_LINK, "/u", true, "t"},
        {"![a][r]\n\n[r]: /s \"\"\n", MARKDOWN_CORE_KIND_EMBEDDED, "/s", true, ""},
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
        markdown_core_document *document = parse(CASES[index].source, strlen(CASES[index].source));
        const markdown_core_node *node = NULL;
        markdown_core_string destination = {NULL, 0};
        markdown_core_optional_string title = {false, {NULL, 0}};
        markdown_core_destination tagged;
        if (!document) {
            check(false, "requirement 14 case parses");
            continue;
        }
        node = child_at(child_at(markdown_core_document_root(document), 0), 0);
        check(markdown_core_node_get_kind(node) == CASES[index].kind, "requirement 14 case has the expected kind");
        /* M1: a link or image answers the tagged `Destination`, and every one
         * the inherited grammar produces is the `url` branch, with the other
         * branch's fields zeroed rather than left over. */
        ok(markdown_core_node_destination(node, &tagged), "a link or image answers its destination");
        ok(markdown_core_node_title(node, &title), "a link or image answers its title");
        check(tagged.kind == MARKDOWN_CORE_DESTINATION_URL, "a link or image destination is the url branch");
        check(tagged.path.data == NULL && tagged.path.length == 0 && !tagged.anchor.has_value,
              "the cross branch's fields are zeroed on a url destination");
        destination = tagged.url;
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
        markdown_core_document *document = parse(INFO_CASES[index].source, strlen(INFO_CASES[index].source));
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
        node = child_at(markdown_core_document_root(document), 0);
        ok(markdown_core_node_code_block_properties(node, &info, &language, &literal, &fenced, &closed),
           "a code block answers its properties");
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
    markdown_core_document *document = parse(source, strlen(source));
    if (!document) {
        return;
    }
    const markdown_core_node *paragraph = child_at(markdown_core_document_root(document), 0);
    const markdown_core_resource *shared = NULL;
    int index = 0;
    for (size_t position = 0; position < children_count(paragraph); position++) {
        const markdown_core_node *node = child_at(paragraph, position);
        if (markdown_core_node_get_kind(node) != MARKDOWN_CORE_KIND_EMBEDDED) {
            continue;
        }
        const markdown_core_dimensions *dimensions = NULL;
        const markdown_core_resource *resource = NULL;
        ok(markdown_core_node_dimensions(node, &dimensions), "an image answers its dimensions");
        ok(markdown_core_node_resource(node, &resource), "an image answers its resource");
        check((dimensions != NULL) == (index < 2), "dimension presence is per image");
        if (dimensions) {
            check(dimensions->width == (index == 0 ? INT32_MAX : 3), "parsed width is exact");
            check(dimensions->height.has_value == (index == 0), "height is optional inside dimensions");
        }
        if (index == 0) {
            check(dimensions && dimensions->height.value == 2, "parsed height is exact");
            shared = resource;
        }
        check(shared == resource, "dimensions never split a shared destination");
        if (index == 1) {
            check(children_count(node) == 0, "numeric-only alt has no children");
        }
        index++;
    }
    check(index == 3, "all dimensioned and malformed occurrences remain images");
    markdown_core_document_free(document);
}

/* M2: every occurrence that resolved through one definition shares one
 * resource, and the identity says so; a direct link, a direct image and an
 * autolink each own one. */
static void check_resource_identity(void) {
    static const char source[] = "[a][r] [r][] [r] ![i][r] [d](/r) <https://x.y> [none]\n\n[r]: /r\n";
    markdown_core_document *document = parse(source, strlen(source));
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
    paragraph = child_at(markdown_core_document_root(document), 0);
    for (size_t position = 0; position < children_count(paragraph); position++) {
        child = child_at(paragraph, position);
        markdown_core_node_kind kind = markdown_core_node_get_kind(child);
        if (kind != MARKDOWN_CORE_KIND_LINK && kind != MARKDOWN_CORE_KIND_EMBEDDED) {
            others++;
            continue;
        }
        const markdown_core_resource *resource = NULL;
        ok(markdown_core_node_resource(child, &resource), "a link or image answers its resource");
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
     * facade. */
    static const char source[] = "> quote\n\ntext\n";
    markdown_core_document *document = parse(source, strlen(source));
    const char *name = NULL;
    const markdown_core_node *callout;
    markdown_core_optional_string variant = {true, {(const uint8_t *)"x", 1}};
    markdown_core_optional_bool collapsed = {true, true};
    if (!document) {
        check(false, "callout corpus parses");
        return;
    }
    callout = child_at(markdown_core_document_root(document), 0);
    check(markdown_core_node_get_kind(callout) == MARKDOWN_CORE_KIND_CALLOUT, "a `>` container is a Callout");
    check(markdown_core_node_kind_name(MARKDOWN_CORE_KIND_CALLOUT, &name) == MARKDOWN_CORE_OK &&
              strcmp(name, "Callout") == 0,
          "the kind is named Callout");
    ok(markdown_core_node_callout_properties(callout, &variant, &collapsed), "a callout answers its properties");
    check(!variant.has_value && variant.value.length == 0, "a `>` container has no variant");
    check(!collapsed.has_value && !collapsed.value, "a `>` container has no fold marker");
    check(sequence(callout, markdown_core_node_callout_title) == NULL, "a `>` container has no title");
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
    static const char source[] = "> [!note]- T  \r\n> body";
    markdown_core_document *document = parse(source, sizeof(source) - 1);
    if (!document) {
        return;
    }
    const markdown_core_node *callout = child_at(markdown_core_document_root(document), 0);
    const markdown_core_nodes *title_nodes = sequence(callout, markdown_core_node_callout_title);
    const markdown_core_node *title = member(title_nodes, 0);
    markdown_core_string literal = {NULL, 0};
    ok(markdown_core_node_literal(title, &literal), "the title text answers its literal");
    check(literal.length == 1 && literal.data[0] == 'T', "trailing title spaces never create a break or title text");
    check(title && markdown_core_nodes_count(title_nodes) == 1, "title contains exactly one node");
    markdown_core_scope title_scope = scope_in(document, title, source, sizeof(source) - 1);
    check(title_scope.start.line == 1 && title_scope.start.column == 12 && title_scope.end.column == 12,
          "title scope uses original byte columns after the metadata");
    const markdown_core_node *body = child_at(callout, 0);
    markdown_core_scope body_scope = scope_in(document, body, source, sizeof(source) - 1);
    check(body && body_scope.start.line == 2 && body_scope.start.column == 3 && body_scope.end.column == 6,
          "body scope starts after its quote prefix and reaches EOF");
    markdown_core_document_free(document);
}

static void check_callout_inherited_setext_scope(void) {
    static const char source[] = "> [!note] T\n> head\n> ===\n\nnext\n";
    markdown_core_document *document = parse(source, sizeof(source) - 1);
    if (!document) {
        return;
    }
    const markdown_core_node *callout = child_at(markdown_core_document_root(document), 0);
    const markdown_core_node *heading = child_at(callout, 0);
    markdown_core_scope scope = scope_in(document, heading, source, sizeof(source) - 1);
    check(markdown_core_node_get_kind(heading) == MARKDOWN_CORE_KIND_HEADING && scope.start.line == 2 &&
              scope.start.column == 3 && scope.end.line == 3 && scope.end.column == 5,
          "callout Setext scope ends on the underline before a following blank line");
    markdown_core_document_free(document);
}

static void check_citation_model(void) {
    /* M4: repeated calls name one label, a later definition of the same label
     * stays a footnote where it was written, and the dump nests each value
     * under its owner: items under the cite, definitions in the content. */
    static const char source[] = "[^a] [^a]\n\n[^a]: once\n\n[^a]: twice\n";
    markdown_core_document *document = parse(source, strlen(source));
    uint8_t *dump = NULL;
    size_t length = 0;
    if (!document) {
        return;
    }
    ok(markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)source,
                                   strlen(source), &dump, &length),
       "citation corpus dumps");
    if (dump) {
        const char *text = (const char *)dump;
        check(count_occurrences(text, "Cite scope=") == 2, "every defined call is a Cite");
        check(count_occurrences(text, "referent=footnote(label=\"a\") children=0\n") == 2,
              "every item names the footnote by label");
        check(count_occurrences(text, "CitationPrefix children=0\n") == 2 &&
                  count_occurrences(text, "CitationSuffix children=0\n") == 2,
              "an inherited call has empty affix groups");
        check(count_occurrences(text, "Footnote scope=") == 2, "both definitions are footnotes");
        check(strstr(text, "\n├── Footnote scope=3:1..4:0 anchor=null attributes={} label=\"a\" children=1\n") != NULL,
              "the winning definition is a block where it was written");
        check(strstr(text, "\n└── Footnote scope=5:1..5:11 anchor=null attributes={} label=\"a\" children=1\n") != NULL,
              "the later definition is the block after it, last in the content");
        check(markdown_core_document_footnote_count(document) == 2, "the document lists both definitions");
        markdown_core_string label = {(const uint8_t *)"a", 1};
        const markdown_core_node *first = NULL;
        ok(markdown_core_document_footnote_at(document, 0, &first), "the first footnote is listed");
        check(markdown_core_document_footnote_for(document, label) == first,
              "a label finds the first definition in source order");
        markdown_core_dump_free(dump);
    }
    markdown_core_document_free(document);
}

static void check_directive_label_projection(void) {
    static const uint8_t inline_source[] = ":badge[label]\n";
    static const uint8_t bare_source[] = ":badge{}\n";
    static const uint8_t empty_source[] = ":badge[]\n";
    static const uint8_t block_source[] = ":::note[Title]\nBody\n:::\n";
    markdown_core_document *document;
    const markdown_core_node *root;
    const markdown_core_node *directive;
    const markdown_core_node *label;
    const markdown_core_node *label_child;
    const markdown_core_node *content_child;

    document = parse(inline_source, sizeof(inline_source) - 1);
    if (document) {
        root = markdown_core_document_root(document);
        directive = child_at(child_at(root, 0), 0);
        label = field(directive, markdown_core_node_directive_label);
        label_child = child_at(label, 0);
        check(markdown_core_node_get_kind(label) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL &&
                  markdown_core_node_get_kind(label_child) == MARKDOWN_CORE_KIND_TEXT && children_count(label) == 1,
              "directive label is an optional Markup-valued field");
        check(markdown_core_node_children(directive) == NULL && children_count(directive) == 0 &&
                  !holds(markdown_core_node_children(directive), label) &&
                  !holds(markdown_core_node_children(child_at(root, 0)), label),
              "an inline directive label is not directive content");
        markdown_core_document_free(document);
    }

    document = parse(bare_source, sizeof(bare_source) - 1);
    if (document) {
        root = markdown_core_document_root(document);
        directive = child_at(child_at(root, 0), 0);
        check(markdown_core_node_get_kind(directive) == MARKDOWN_CORE_KIND_DIRECTIVE &&
                  field(directive, markdown_core_node_directive_label) == NULL && children_count(directive) == 0,
              "an absent directive label remains absent and is not content");
        markdown_core_document_free(document);
    }

    document = parse(empty_source, sizeof(empty_source) - 1);
    if (document) {
        root = markdown_core_document_root(document);
        directive = child_at(child_at(root, 0), 0);
        label = field(directive, markdown_core_node_directive_label);
        check(markdown_core_node_get_kind(label) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL && children_count(label) == 0,
              "an empty label remains distinct from an absent label");
        check(children_count(directive) == 0 && !holds(markdown_core_node_children(directive), label),
              "an empty directive label is not directive content");
        markdown_core_document_free(document);
    }

    document = parse(block_source, sizeof(block_source) - 1);
    if (document) {
        root = markdown_core_document_root(document);
        directive = child_at(root, 0);
        label = field(directive, markdown_core_node_directive_label);
        check(markdown_core_node_get_kind(label) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL && children_count(label) == 1,
              "block directive exposes its label through the field accessor");
        content_child = child_at(directive, 0);
        check(markdown_core_node_get_kind(content_child) == MARKDOWN_CORE_KIND_PARAGRAPH,
              "block directive children contain only block content");
        check(!holds(markdown_core_node_children(directive), label) &&
                  !holds(markdown_core_node_children(root), label) && children_count(directive) == 1,
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
        markdown_core_document *document = parse(WITNESSES[index].source, strlen(WITNESSES[index].source));
        uint8_t *dump = NULL;
        size_t length = 0;
        if (!document) {
            continue;
        }
        ok(markdown_core_document_dump(document, markdown_core_document_root(document),
                                       (const uint8_t *)WITNESSES[index].source, strlen(WITNESSES[index].source), &dump,
                                       &length),
           "dialect witness dumps");
        if (dump) {
            check(strstr((const char *)dump, WITNESSES[index].witness) != NULL,
                  "every feature of the dialect is recognized by a parse that was handed nothing but bytes");
        }
        markdown_core_dump_free(dump);
        markdown_core_document_free(document);
    }
}

static void check_api(void) {
    static const uint8_t source[] = "# Heading\n\n- [ ] task\n";
    markdown_core_document *document;
    const markdown_core_node *root;
    const markdown_core_node *heading;
    markdown_core_scope scope;
    int32_t level = 0;

    document = parse(source, sizeof(source) - 1);
    if (document) {
        root = markdown_core_document_root(document);
        heading = child_at(root, 0);
        check(markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT, "document root kind is typed");
        check(markdown_core_node_get_kind(heading) == MARKDOWN_CORE_KIND_HEADING,
              "first child traversal is read-only and typed");
        check(markdown_core_node_heading_level(heading, &level) == MARKDOWN_CORE_OK && level == 1,
              "heading accessor returns its behavior-bearing field");
        scope = scope_in(document, heading, source, sizeof(source) - 1);
        check(scope.start.line == 1 && scope.start.column == 1, "the scope query answers in native coordinates");
        markdown_core_document_free(document);
    }
}

static void check_table_model(void) {
    static const uint8_t input[] = "| h | center | right | plain |\n| :-- | :-: | --: | -- |\n| x\\|y | `\\|` | z |\n";
    markdown_core_document *document = parse(input, sizeof(input) - 1);
    if (!document) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *table = child_at(root, 0);
    size_t columns = 0, head = 0, content = 0, foot = 0;
    ok(markdown_core_node_table_properties(table, &columns, &head, &content, &foot), "a table answers its properties");
    check(columns == 4 && head == 1 && content == 1 && foot == 0, "pipe table group partition");
    check(children_count(table) == head + content + foot, "table rows have one structural owner");
    const markdown_core_flow expected[] = {MARKDOWN_CORE_FLOW_LEFT, MARKDOWN_CORE_FLOW_CENTER, MARKDOWN_CORE_FLOW_RIGHT,
                                           MARKDOWN_CORE_FLOW_NONE};
    for (size_t i = 0; i < columns; i++) {
        markdown_core_table_column column = {MARKDOWN_CORE_FLOW_NONE, {true, 0}};
        ok(markdown_core_node_table_column_at(table, i, &column), "a column below the count answers");
        check(column.flow == expected[i] && !column.relative.has_value, "column authored facts");
    }
    for (size_t row_index = 0; row_index < children_count(table); row_index++) {
        const markdown_core_node *row = child_at(table, row_index);
        check(children_count(row) == 4, "pipe rows have every logical column");
        for (size_t cell_index = 0; cell_index < children_count(row); cell_index++) {
            const markdown_core_node *cell = child_at(row, cell_index);
            int64_t rowspan = 0, colspan = 0;
            ok(markdown_core_node_table_cell_spans(cell, &rowspan, &colspan), "a cell answers its spans");
            check(rowspan == 1 && colspan == 1, "inherited unit spans");
        }
    }
    markdown_core_document_free(document);
}

static void check_definition_model(void) {
    static const uint8_t input[] = "::: box\n*T*\n: one\n~\n\nU\n\n: two\n:::\n\n:::named\n:::\n";
    markdown_core_document *document = parse(input, sizeof(input) - 1);
    if (!document) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *block = child_at(root, 0);
    const markdown_core_node *list = child_at(block, 0);
    const markdown_core_node *definition = child_at(list, 0);
    markdown_core_optional_string name = {true, {NULL, 0}};
    ok(markdown_core_node_directive_properties(block, &name), "a directive block answers its name");
    check(!name.has_value, "nameless block name is absent");
    ok(markdown_core_node_directive_properties(child_at(root, 1), &name), "a directive block answers its name");
    check(name.has_value && name.value.length == 5, "named block retains its name");
    check(markdown_core_node_get_kind(list) == MARKDOWN_CORE_KIND_DEFINITION_LIST && children_count(list) == 2,
          "definition list has typed members");
    check(markdown_core_node_get_kind(definition) == MARKDOWN_CORE_KIND_DEFINITION, "definition kind");
    check(markdown_core_node_children(definition) == NULL && children_count(definition) == 0 &&
              !child_at(definition, 0),
          "body collection roots never enter generic Markup traversal");
    bool compact = false;
    check(markdown_core_node_definition_compact(definition, &compact) == MARKDOWN_CORE_OK && compact,
          "compact term gap");
    check(markdown_core_node_definition_compact(child_at(list, 1), &compact) == MARKDOWN_CORE_OK && !compact,
          "loose term gap");
    const markdown_core_nodes *term = sequence(definition, markdown_core_node_definition_term);
    check(markdown_core_nodes_count(term) == 1 &&
              markdown_core_node_get_kind(member(term, 0)) == MARKDOWN_CORE_KIND_EMPHASIS,
          "term is a separate inline field");
    size_t bodies = 0;
    const markdown_core_nodes *body = NULL;
    ok(markdown_core_node_definition_body_count(definition, &bodies), "a definition answers its body count");
    check(bodies == 2, "a definition holds both of its bodies");
    ok(markdown_core_node_definition_body_at(definition, 0, &body), "a definition answers its first body");
    check(markdown_core_nodes_count(body) > 0 &&
              markdown_core_node_get_kind(member(body, 0)) == MARKDOWN_CORE_KIND_PARAGRAPH,
          "first body exposes ordinary blocks");
    ok(markdown_core_node_definition_body_at(definition, 1, &body), "a definition answers its second body");
    check(markdown_core_nodes_count(body) == 0, "empty second body retains its position");
    body = markdown_core_node_children(root);
    check(markdown_core_node_definition_body_at(definition, bodies, &body) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              body == markdown_core_node_children(root),
          "a body at the count is out of bounds");
    markdown_core_document_free(document);
}

/* THE PUBLIC BOUNDARY. Every check a public call makes where the caller's
 * value enters answers an explicit status instead of reading memory the call
 * does not own, and a failed call writes none of its out-parameters. */
static void check_kind_boundary(void) {
    static const char source[] = "text\n";
    markdown_core_document *document = parse(source, sizeof(source) - 1);
    if (!document) {
        return;
    }
    const markdown_core_node *paragraph = child_at(markdown_core_document_root(document), 0);
    const markdown_core_node *node = paragraph;
    const markdown_core_nodes *const content = markdown_core_node_children(paragraph);
    const markdown_core_nodes *nodes = content;
    const markdown_core_attribute_value *attributes = NULL;
    const markdown_core_dimensions *dimensions = NULL;
    const markdown_core_resource *resource = NULL;
    const markdown_core_metadata_value *value = NULL;
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start = {true, 7};
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    markdown_core_optional_string optional = {true, {NULL, 7}};
    markdown_core_optional_bool optional_bool;
    markdown_core_string string = {NULL, 7};
    markdown_core_placement placement;
    markdown_core_table_column column;
    markdown_core_destination destination;
    markdown_core_referent referent;
    int32_t level = 7;
    int64_t span;
    size_t size = 7;
    bool flag;
    /* A paragraph has no field any kind accessor reads. */
    const markdown_core_status statuses[] = {
        markdown_core_node_heading_level(paragraph, &level),
        markdown_core_node_list_properties(paragraph, &flavor, &start, &variant, &delimiter, &flag),
        markdown_core_node_list_item_marker(paragraph, &optional),
        markdown_core_node_code_block_properties(paragraph, &optional, &optional, &string, &flag, &flag),
        markdown_core_node_literal(paragraph, &string),
        markdown_core_node_formula_properties(paragraph, &placement, &string),
        markdown_core_node_table_properties(paragraph, &size, &size, &size, &size),
        markdown_core_node_table_column_at(paragraph, 0, &column),
        markdown_core_node_table_caption(paragraph, &node),
        markdown_core_node_table_cell_spans(paragraph, &span, &span),
        markdown_core_node_directive_properties(paragraph, &optional),
        markdown_core_node_definition_compact(paragraph, &flag),
        markdown_core_node_definition_term(paragraph, &nodes),
        markdown_core_node_definition_body_count(paragraph, &size),
        markdown_core_node_definition_body_at(paragraph, 0, &nodes),
        markdown_core_node_inherited_attributes(paragraph, &attributes),
        markdown_core_node_dimensions(paragraph, &dimensions),
        markdown_core_node_directive_label(paragraph, &node),
        markdown_core_node_callout_properties(paragraph, &optional, &optional_bool),
        markdown_core_node_callout_title(paragraph, &nodes),
        markdown_core_node_destination(paragraph, &destination),
        markdown_core_node_cross_label(paragraph, &optional),
        markdown_core_node_title(paragraph, &optional),
        markdown_core_node_resource(paragraph, &resource),
        markdown_core_citation_referent(paragraph, &referent),
        markdown_core_citation_prefix(paragraph, &nodes),
        markdown_core_citation_suffix(paragraph, &nodes),
        markdown_core_footnote_label(paragraph, &optional),
        markdown_core_footnote_content(paragraph, &nodes),
        markdown_core_specimen_properties(paragraph, &optional, &start),
        markdown_core_specimen_content(paragraph, &nodes),
        markdown_core_node_document_metadata(paragraph, &node),
        markdown_core_metadata_name(paragraph, &value),
        markdown_core_metadata_title(paragraph, &value),
        markdown_core_metadata_subtitle(paragraph, &value),
        markdown_core_metadata_time(paragraph, &value),
        markdown_core_metadata_date(paragraph, &value),
        markdown_core_metadata_authors(paragraph, &value),
        markdown_core_metadata_keywords(paragraph, &value),
        markdown_core_metadata_abstract(paragraph, &value),
        markdown_core_metadata_state(paragraph, &value),
        markdown_core_metadata_comment(paragraph, &value),
    };
    for (size_t index = 0; index < sizeof(statuses) / sizeof(*statuses); index++) {
        check(statuses[index] == MARKDOWN_CORE_KIND_MISMATCH, "a kind accessor refuses a node of another kind");
    }
    check(content && node == paragraph && nodes == content && size == 7 && !attributes && !dimensions && !resource &&
              !value && level == 7 && start.value == 7 && optional.value.length == 7 && string.length == 7,
          "a refused accessor writes none of its out-parameters");
    /* The kind is the node's own, not its neighbour's: a Text answers the
     * literal its Paragraph does not. */
    check(markdown_core_node_literal(child_at(paragraph, 0), &string) == MARKDOWN_CORE_OK && string.length == 4,
          "a literal kind answers its literal");
    markdown_core_document_free(document);
}

static void check_index_boundary(void) {
    static const char source[] = "x :n{.c k=v}\n\n***\n\n| a | b |\n| - | - |\n";
    static const char metadata_source[] = "---\nname: N\nauthors: [Ada, Lin]\n---\n";
    markdown_core_document *document = parse(source, sizeof(source) - 1);
    markdown_core_document *metadata_document = parse(metadata_source, sizeof(metadata_source) - 1);
    if (!document || !metadata_document) {
        markdown_core_document_free(document);
        markdown_core_document_free(metadata_document);
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *paragraph = child_at(root, 0);
    const markdown_core_node *directive = child_at(paragraph, 1);
    const markdown_core_node *table = child_at(root, 2);
    const markdown_core_node *node = root;
    markdown_core_table_column column = {MARKDOWN_CORE_FLOW_RIGHT, {true, 7}};
    markdown_core_string name = {NULL, 7}, text = {NULL, 7};
    size_t columns = 0, head, content, foot;

    ok(markdown_core_node_table_properties(table, &columns, &head, &content, &foot), "a table answers its columns");
    check(columns == 2 && markdown_core_node_table_column_at(table, 1, &column) == MARKDOWN_CORE_OK,
          "the last column answers");
    column = (markdown_core_table_column){MARKDOWN_CORE_FLOW_RIGHT, {true, 7}};
    check(markdown_core_node_table_column_at(table, columns, &column) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              column.relative.value == 7,
          "a column at the count is out of bounds");

    check(markdown_core_document_footnote_count(document) == 0 &&
              markdown_core_document_footnote_at(document, 0, &node) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              markdown_core_document_specimen_count(document) == 0 &&
              markdown_core_document_specimen_at(document, 0, &node) == MARKDOWN_CORE_OUT_OF_BOUNDS && node == root,
          "a definition at the count is out of bounds");
    check(markdown_core_nodes_at(markdown_core_node_children(paragraph), children_count(paragraph), &node) ==
                  MARKDOWN_CORE_OUT_OF_BOUNDS &&
              markdown_core_nodes_at(NULL, 0, &node) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              markdown_core_nodes_count(NULL) == 0 && node == root,
          "a child at the count is out of bounds, the empty sequence included");

    check(markdown_core_node_get_kind(directive) == MARKDOWN_CORE_KIND_DIRECTIVE &&
              markdown_core_node_attribute_class_count(directive) == 1 &&
              markdown_core_node_attribute_record_count(directive) == 1,
          "the directive holds one class and one record");
    check(markdown_core_node_attribute_class_at(directive, 0, &text) == MARKDOWN_CORE_OK && text.length == 1,
          "the last class answers");
    check(markdown_core_node_attribute_record_at(directive, 0, &name, &text) == MARKDOWN_CORE_OK && name.length == 1,
          "the last record answers");
    name = text = (markdown_core_string){NULL, 7};
    check(markdown_core_node_attribute_class_at(directive, 1, &text) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              markdown_core_node_attribute_record_at(directive, 1, &name, &text) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              markdown_core_node_attribute_class_at(table, 0, &text) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              name.length == 7 && text.length == 7,
          "a class or record at the count is out of bounds, a node with none included");
    const markdown_core_attribute_value *primary = markdown_core_node_primary_attributes(directive);
    check(markdown_core_attribute_value_class_at(primary, 1, &text) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              markdown_core_attribute_value_record_at(primary, 1, &name, &text) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              text.length == 7,
          "a contribution's class or record at the count is out of bounds");

    const markdown_core_node *metadata =
        field(markdown_core_document_root(metadata_document), markdown_core_node_document_metadata);
    const markdown_core_metadata_value *scalar = NULL, *list = NULL;
    markdown_core_metadata_scalar scalar_value = {MARKDOWN_CORE_METADATA_BOOL, {.boolean = true}};
    markdown_core_metadata_list_item item = {MARKDOWN_CORE_METADATA_ITEM_NUMBER, {NULL, 7}};
    size_t count = 7;
    check(metadata && markdown_core_metadata_name(metadata, &scalar) == MARKDOWN_CORE_OK &&
              markdown_core_metadata_authors(metadata, &list) == MARKDOWN_CORE_OK && scalar && list,
          "the metadata holds a scalar and a list");
    if (scalar && list) {
        check(markdown_core_metadata_value_item_count(scalar, &count) == MARKDOWN_CORE_KIND_MISMATCH &&
                  markdown_core_metadata_value_item_at(scalar, 0, &item) == MARKDOWN_CORE_KIND_MISMATCH &&
                  markdown_core_metadata_value_scalar(list, &scalar_value) == MARKDOWN_CORE_KIND_MISMATCH &&
                  count == 7 && item.value.length == 7 && scalar_value.kind == MARKDOWN_CORE_METADATA_BOOL,
              "a metadata value accessor refuses a value of the other kind");
        check(markdown_core_metadata_value_item_count(list, &count) == MARKDOWN_CORE_OK && count == 2 &&
                  markdown_core_metadata_value_item_at(list, 1, &item) == MARKDOWN_CORE_OK,
              "the last list item answers");
        item.value.length = 7;
        check(markdown_core_metadata_value_item_at(list, count, &item) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
                  item.value.length == 7,
              "a list item at the count is out of bounds");
    }

    const char *kind_name = NULL;
    for (int kind = MARKDOWN_CORE_KIND_NONE; kind <= MARKDOWN_CORE_KIND_METADATA; kind++) {
        check(markdown_core_node_kind_name((markdown_core_node_kind)kind, &kind_name) == MARKDOWN_CORE_OK && kind_name,
              "every kind has a name");
    }
    kind_name = NULL;
    check(markdown_core_node_kind_name((markdown_core_node_kind)(MARKDOWN_CORE_KIND_METADATA + 1), &kind_name) ==
                  MARKDOWN_CORE_OUT_OF_BOUNDS &&
              markdown_core_node_kind_name((markdown_core_node_kind)-1, &kind_name) == MARKDOWN_CORE_OUT_OF_BOUNDS &&
              kind_name == NULL,
          "a value outside the kinds has no name");
    markdown_core_document_free(metadata_document);
    markdown_core_document_free(document);
}

/* A scope query reads its source up to the node's end; a source that ends
 * sooner is refused, whatever unit counts the columns. */
static void check_source_boundary(void) {
    static const char source[] = "a\n\nb\xc3\xa9\n";
    const size_t length = sizeof(source) - 1;
    markdown_core_document *document = NULL;
    if (!ok(markdown_core_document_parse_in((const uint8_t *)source, length, MARKDOWN_CORE_TEXT_UNIT_UTF16, &document),
            "a UTF-16 document parses")) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_node *last = child_at(root, 1);
    const markdown_core_node *text = child_at(last, 0);
    const markdown_core_node *found = root;
    markdown_core_scope scope = {{-1, -1}, {-1, -1}};
    uint8_t *dump = NULL;
    size_t dump_length = 7;

    check(markdown_core_document_scope(document, last, (const uint8_t *)source, 3, &scope) ==
                  MARKDOWN_CORE_OUT_OF_BOUNDS &&
              scope.start.line == -1,
          "a source that ends before the node is out of bounds");
    /* The document ends before its last line terminator, and its last
     * scalar ends one byte before that. */
    check(markdown_core_document_scope(document, root, (const uint8_t *)source, length - 2, &scope) ==
              MARKDOWN_CORE_OUT_OF_BOUNDS,
          "one byte short is out of bounds");
    check(markdown_core_document_scope(document, text, (const uint8_t *)source, length - 1, &scope) ==
                  MARKDOWN_CORE_OK &&
              scope.start.line == 3 && scope.start.column == 1 && scope.end.column == 2,
          "a source that reaches the node's end answers");
    check(markdown_core_document_dump(document, root, (const uint8_t *)source, length - 2, &dump, &dump_length) ==
                  MARKDOWN_CORE_OUT_OF_BOUNDS &&
              dump == NULL && dump_length == 7,
          "a dump over a source that ends before the node is out of bounds");
    check(markdown_core_document_dump(document, last, (const uint8_t *)source, length - 1, &dump, &dump_length) ==
              MARKDOWN_CORE_OK,
          "a dump over a source that reaches the node's end answers");
    markdown_core_dump_free(dump);

    const markdown_core_position outside[] = {{0, 1}, {1, 0}, {-1, 1}, {1, -1}, {INT32_MIN, INT32_MIN}};
    for (size_t index = 0; index < sizeof(outside) / sizeof(*outside); index++) {
        check(markdown_core_document_node_at(document, outside[index], (const uint8_t *)source, length, &found) ==
                      MARKDOWN_CORE_OUT_OF_BOUNDS &&
                  found == root,
              "a line or column below 1 is out of bounds");
    }
    check(markdown_core_document_node_at(document, (markdown_core_position){3, 2}, (const uint8_t *)source, length,
                                         &found) == MARKDOWN_CORE_OK &&
              found == text,
          "a position a node holds answers that node");
    check(markdown_core_document_node_at(document, (markdown_core_position){9, 1}, (const uint8_t *)source, length,
                                         &found) == MARKDOWN_CORE_OK &&
              found == NULL,
          "a position past the source is held by no node");
    check(markdown_core_document_node_at(document, (markdown_core_position){3, 9}, (const uint8_t *)source, length,
                                         &found) == MARKDOWN_CORE_OK &&
              found == NULL,
          "a column past its line is held by no node");
    markdown_core_document_free(document);

    /* The empty source may be NULL, and freeing no document does nothing. */
    document = NULL;
    check(markdown_core_document_parse(NULL, 0, &document) == MARKDOWN_CORE_OK && document &&
              markdown_core_node_get_kind(markdown_core_document_root(document)) == MARKDOWN_CORE_KIND_DOCUMENT,
          "a NULL source of length 0 is the empty document");
    markdown_core_document_free(document);
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
    check_kind_boundary();
    check_index_boundary();
    check_source_boundary();
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
