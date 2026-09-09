#include <stdio.h>
#include "table.h"
#include "autolink.h"
#include "formula.h"
#include "directive.h"
#include <stdlib.h>
#include <string.h>

#include "markdown-core.h"
#include "node.h"
#include "buffer.h"
#include "parser.h"
#include "extension.h"
#include "markdown-core-extensions.h"

#include <markdown_core.h>

#include "ast_internal.h"

#include "harness.h"
#include "cplusplus.h"

#define UTF8_REPL "\xEF\xBF\xBD"

typedef struct probe_setup {
    const markdown_core_extension *const *extensions;
    size_t count;
} probe_setup;

static bool attach_probes(markdown_core_parser *parser, void *context) {
    const probe_setup *setup = (const probe_setup *)context;
    size_t i;

    for (i = 0; i < setup->count; i++) {
        if (!markdown_core_parser_attach_extension(parser, setup->extensions[i])) {
            return false;
        }
    }
    return true;
}

static markdown_core_node *parse_with_probes(const char *source, size_t length,
                                             const markdown_core_extension *const *extensions, size_t extension_count) {
    probe_setup setup = {extensions, extension_count};
    return markdown_core_parse_document_with_mem(source, length, markdown_core_get_default_mem_allocator(),
                                                 attach_probes, &setup);
}

static const markdown_core_node_type node_types[] = {
    MARKDOWN_CORE_NODE_DOCUMENT,       MARKDOWN_CORE_NODE_CALLOUT,    MARKDOWN_CORE_NODE_LIST,
    MARKDOWN_CORE_NODE_LIST_ITEM,      MARKDOWN_CORE_NODE_CODE_BLOCK, MARKDOWN_CORE_NODE_HTML_BLOCK,
    MARKDOWN_CORE_NODE_COMMENT_BLOCK,  MARKDOWN_CORE_NODE_PARAGRAPH,  MARKDOWN_CORE_NODE_HEADING,
    MARKDOWN_CORE_NODE_THEMATIC_BREAK, MARKDOWN_CORE_NODE_TEXT,       MARKDOWN_CORE_NODE_SOFT_BREAK,
    MARKDOWN_CORE_NODE_LINE_BREAK,     MARKDOWN_CORE_NODE_CODE,       MARKDOWN_CORE_NODE_HTML,
    MARKDOWN_CORE_NODE_COMMENT,        MARKDOWN_CORE_NODE_EMPHASIS,   MARKDOWN_CORE_NODE_STRONG,
    MARKDOWN_CORE_NODE_LINK,           MARKDOWN_CORE_NODE_MEDIA};
static const char *const node_type_names[] = {
    "document",  "callout", "list",           "list_item", "code_block", "html_block", "comment_block",
    "paragraph", "heading", "thematic_break", "text",      "soft_break", "line_break", "code",
    "html",      "comment", "emphasis",       "strong",    "link",       "media"};
static const int num_node_types = sizeof(node_types) / sizeof(*node_types);

static void test_md_paragraph_text(test_batch_runner *runner, const char *markdown, const char *expected_text,
                                   const char *msg);

static void test_md_paragraph_bytes(test_batch_runner *runner, const char *markdown, size_t markdown_length,
                                    const char *expected_text, const char *msg);

static markdown_core_node *parse(const char *source);

static void test_content(test_batch_runner *runner, markdown_core_node_type type, unsigned int *allowed_content);

static void test_valid_char(test_batch_runner *runner, const char *utf8, const char *msg);

static void version(test_batch_runner *runner) {
    INT_EQ(runner, markdown_core_version(), MARKDOWN_CORE_VERSION, "markdown_core_version");
    STR_EQ(runner, markdown_core_version_string(), MARKDOWN_CORE_VERSION_STRING, "markdown_core_version_string");
}

/* The extension types continue these two sequences, so listing them here means
 * the existing contiguity assertions pin every one of the nine values AND make
 * a collision or a gap impossible. Until Step 3.1 they were globals filled in
 * by runtime registration in whatever order `core_extensions_registration`
 * called the `create_*` functions, and nothing in the repository asserted a
 * single one of them. */
static void node_type_values(test_batch_runner *runner) {
    static const markdown_core_node_type block_types[] = {
        MARKDOWN_CORE_NODE_DOCUMENT,     MARKDOWN_CORE_NODE_CALLOUT,       MARKDOWN_CORE_NODE_LIST,
        MARKDOWN_CORE_NODE_LIST_ITEM,    MARKDOWN_CORE_NODE_CODE_BLOCK,    MARKDOWN_CORE_NODE_HTML_BLOCK,
        MARKDOWN_CORE_NODE_PARAGRAPH,    MARKDOWN_CORE_NODE_HEADING,       MARKDOWN_CORE_NODE_THEMATIC_BREAK,
        MARKDOWN_CORE_NODE_FOOTNOTE,     MARKDOWN_CORE_NODE_TABLE,         MARKDOWN_CORE_NODE_TABLE_ROW,
        MARKDOWN_CORE_NODE_TABLE_CELL,   MARKDOWN_CORE_NODE_FORMULA_BLOCK, MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
        MARKDOWN_CORE_NODE_COMMENT_BLOCK};
    static const markdown_core_node_type inline_types[] = {
        MARKDOWN_CORE_NODE_TEXT,           MARKDOWN_CORE_NODE_SOFT_BREAK,
        MARKDOWN_CORE_NODE_LINE_BREAK,     MARKDOWN_CORE_NODE_CODE,
        MARKDOWN_CORE_NODE_HTML,           MARKDOWN_CORE_NODE_EMPHASIS,
        MARKDOWN_CORE_NODE_STRONG,         MARKDOWN_CORE_NODE_LINK,
        MARKDOWN_CORE_NODE_MEDIA,          MARKDOWN_CORE_NODE_CITE,
        MARKDOWN_CORE_NODE_STRIKETHROUGH,  MARKDOWN_CORE_NODE_FORMULA,
        MARKDOWN_CORE_NODE_DIRECTIVE,      MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
        MARKDOWN_CORE_NODE_COMMENT,        MARKDOWN_CORE_NODE_CITATION,
        MARKDOWN_CORE_NODE_CROSS_LINK,     MARKDOWN_CORE_NODE_MARK,
        MARKDOWN_CORE_NODE_CROSS_EMBEDDED, MARKDOWN_CORE_NODE_INSERTION};

    for (size_t i = 0; i < sizeof(block_types) / sizeof(*block_types); ++i) {
        INT_EQ(runner, block_types[i] & MARKDOWN_CORE_NODE_TYPE_MASK, MARKDOWN_CORE_NODE_TYPE_BLOCK,
               "block node type class %zu", i);
        INT_EQ(runner, block_types[i] & MARKDOWN_CORE_NODE_VALUE_MASK, i + 1, "block node type value %zu", i);
    }

    for (size_t i = 0; i < sizeof(inline_types) / sizeof(*inline_types); ++i) {
        INT_EQ(runner, inline_types[i] & MARKDOWN_CORE_NODE_TYPE_MASK, MARKDOWN_CORE_NODE_TYPE_INLINE,
               "inline node type class %zu", i);
        INT_EQ(runner, inline_types[i] & MARKDOWN_CORE_NODE_VALUE_MASK, i + 1, "inline node type value %zu", i);
    }
}

static void constructor(test_batch_runner *runner) {
    for (int i = 0; i < num_node_types; ++i) {
        markdown_core_node_type type = node_types[i];
        markdown_core_node *node = markdown_core_node_new(type);
        OK(runner, node != NULL, "new type %d", type);
        INT_EQ(runner, markdown_core_node_get_type(node), type, "get_type %d", type);
        STR_EQ(runner, markdown_core_node_get_type_string(node), node_type_names[i], "get_type_string %d", type);

        switch (node->kind) {
        case MARKDOWN_CORE_NODE_HEADING:
            INT_EQ(runner, markdown_core_node_get_heading_level(node), 1, "default heading level is 1");
            node->as.heading->level = 1;
            break;

        case MARKDOWN_CORE_NODE_LIST:
            INT_EQ(runner, markdown_core_node_get_list_type(node), MARKDOWN_CORE_BULLET_LIST,
                   "default is list type is bullet");
            INT_EQ(runner, markdown_core_node_get_list_delim(node), MARKDOWN_CORE_NO_DELIM,
                   "default is list delim is NO_DELIM");
            INT_EQ(runner, markdown_core_node_get_list_start(node), 0, "default is list start is 0");
            INT_EQ(runner, markdown_core_node_get_list_tight(node), 0, "default is list is loose");
            break;

        default:
            break;
        }

        markdown_core_node_free(node);
    }
}

static void accessors(test_batch_runner *runner) {
    static const char markdown[] = "## Header\n"
                                   "\n"
                                   "* Item 1\n"
                                   "* Item 2\n"
                                   "\n"
                                   "2. Item 1\n"
                                   "\n"
                                   "3. Item 2\n"
                                   "\n"
                                   "``` lang\n"
                                   "fenced\n"
                                   "```\n"
                                   "    code\n"
                                   "\n"
                                   "<div>html</div>\n"
                                   "\n"
                                   "[link](url 'title')\n";

    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1);

    // Getters

    markdown_core_node *heading = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_heading_level(heading), 2, "get_heading_level");

    markdown_core_node *bullet_list = markdown_core_node_next(heading);
    INT_EQ(runner, markdown_core_node_get_list_type(bullet_list), MARKDOWN_CORE_BULLET_LIST, "get_list_type bullet");
    INT_EQ(runner, markdown_core_node_get_list_tight(bullet_list), 1, "get_list_tight tight");

    markdown_core_node *ordered_list = markdown_core_node_next(bullet_list);
    INT_EQ(runner, markdown_core_node_get_list_type(ordered_list), MARKDOWN_CORE_ORDERED_LIST, "get_list_type ordered");
    INT_EQ(runner, markdown_core_node_get_list_delim(ordered_list), MARKDOWN_CORE_PERIOD_DELIM,
           "get_list_delim ordered");
    INT_EQ(runner, markdown_core_node_get_list_start(ordered_list), 2, "get_list_start");
    INT_EQ(runner, markdown_core_node_get_list_tight(ordered_list), 0, "get_list_tight loose");

    markdown_core_node *fenced = markdown_core_node_next(ordered_list);
    STR_EQ(runner, markdown_core_node_get_literal(fenced), "fenced\n", "get_literal fenced code");
    STR_EQ(runner, markdown_core_node_get_fence_info(fenced), "lang", "get_fence_info");
    INT_EQ(runner, markdown_core_node_get_fence_closed(fenced), 1, "get_fence_closed closed fenced code");

    markdown_core_node *code = markdown_core_node_next(fenced);
    STR_EQ(runner, markdown_core_node_get_literal(code), "code\n", "get_literal indented code");
    INT_EQ(runner, markdown_core_node_get_fence_closed(code), 0, "get_fence_closed indented code");

    static const char unclosed_markdown[] = "``` lang\n"
                                            "unclosed\n";
    markdown_core_node *unclosed_doc = markdown_core_parse_document(unclosed_markdown, sizeof(unclosed_markdown) - 1);
    markdown_core_node *unclosed = markdown_core_node_first_child(unclosed_doc);
    INT_EQ(runner, markdown_core_node_get_fence_closed(unclosed), 0, "get_fence_closed unclosed fenced code");
    markdown_core_node_free(unclosed_doc);

    markdown_core_node *html = markdown_core_node_next(code);
    STR_EQ(runner, markdown_core_node_get_literal(html), "<div>html</div>\n", "get_literal html");

    markdown_core_node *paragraph = markdown_core_node_next(html);
    INT_EQ(runner, markdown_core_node_get_start_line(paragraph), 17, "get_start_line");
    INT_EQ(runner, markdown_core_node_get_start_column(paragraph), 1, "get_start_column");
    INT_EQ(runner, markdown_core_node_get_end_line(paragraph), 17, "get_end_line");

    markdown_core_node *link = markdown_core_node_first_child(paragraph);
    markdown_core_destination destination;
    markdown_core_optional_string title;
    OK(runner,
       markdown_core_node_destination(link, &destination) && destination.kind == MARKDOWN_CORE_DESTINATION_URL &&
           destination.url.length == 3 && memcmp(destination.url.data, "url", 3) == 0,
       "a parsed link's destination is read through the facade");
    OK(runner,
       markdown_core_node_title(link, &title) && title.has_value && title.value.length == 5 &&
           memcmp(title.value.data, "title", 5) == 0,
       "a parsed link's title is read through the facade");

    markdown_core_node *string = markdown_core_node_first_child(link);
    STR_EQ(runner, markdown_core_node_get_literal(string), "link", "get_literal string");

    // Setters

    OK(runner, markdown_core_node_set_heading_level(heading, 3), "set_heading_level");

    OK(runner, markdown_core_node_set_list_type(bullet_list, MARKDOWN_CORE_ORDERED_LIST), "set_list_type ordered");
    OK(runner, markdown_core_node_set_list_delim(bullet_list, MARKDOWN_CORE_PAREN_DELIM), "set_list_delim paren");
    OK(runner, markdown_core_node_set_list_start(bullet_list, 3), "set_list_start");
    OK(runner, markdown_core_node_set_list_tight(bullet_list, 0), "set_list_tight loose");

    OK(runner, markdown_core_node_set_list_type(ordered_list, MARKDOWN_CORE_BULLET_LIST), "set_list_type bullet");
    OK(runner, markdown_core_node_set_list_tight(ordered_list, 1), "set_list_tight tight");

    OK(runner, markdown_core_node_set_literal(code, "CODE\n"), "set_literal indented code");

    OK(runner, markdown_core_node_set_literal(fenced, "FENCED\n"), "set_literal fenced code");
    OK(runner, markdown_core_node_set_fence_info(fenced, "LANG"), "set_fence_info");

    OK(runner, markdown_core_node_set_literal(html, "<div>HTML</div>\n"), "set_literal html");

    OK(runner, markdown_core_node_set_literal(string, "prefix-LINK"), "set_literal string");

    // Set literal to suffix of itself (issue #139).
    const char *literal = markdown_core_node_get_literal(string);
    OK(runner, markdown_core_node_set_literal(string, literal + sizeof("prefix")), "set_literal suffix");

    // Every setter must be observable through the AST accessors.
    INT_EQ(runner, markdown_core_node_get_heading_level(heading), 3, "set_heading_level applied");
    INT_EQ(runner, markdown_core_node_get_list_type(bullet_list), MARKDOWN_CORE_ORDERED_LIST, "set_list_type applied");
    INT_EQ(runner, markdown_core_node_get_list_delim(bullet_list), MARKDOWN_CORE_PAREN_DELIM, "set_list_delim applied");
    INT_EQ(runner, markdown_core_node_get_list_start(bullet_list), 3, "set_list_start applied");
    INT_EQ(runner, markdown_core_node_get_list_tight(bullet_list), 0, "set_list_tight applied");
    INT_EQ(runner, markdown_core_node_get_list_type(ordered_list), MARKDOWN_CORE_BULLET_LIST,
           "set_list_type bullet applied");
    INT_EQ(runner, markdown_core_node_get_list_tight(ordered_list), 1, "set_list_tight tight applied");
    STR_EQ(runner, markdown_core_node_get_literal(code), "CODE\n", "set_literal code applied");
    STR_EQ(runner, markdown_core_node_get_literal(fenced), "FENCED\n", "set_literal fenced applied");
    STR_EQ(runner, markdown_core_node_get_fence_info(fenced), "LANG", "set_fence_info applied");
    STR_EQ(runner, markdown_core_node_get_literal(html), "<div>HTML</div>\n", "set_literal html applied");
    STR_EQ(runner, markdown_core_node_get_literal(string), "LINK", "set_literal suffix applied");

    // Getter errors

    INT_EQ(runner, markdown_core_node_get_heading_level(bullet_list), 0, "get_heading_level error");
    INT_EQ(runner, markdown_core_node_get_list_type(heading), MARKDOWN_CORE_NO_LIST, "get_list_type error");
    INT_EQ(runner, markdown_core_node_get_list_start(code), 0, "get_list_start error");
    INT_EQ(runner, markdown_core_node_get_list_tight(fenced), 0, "get_list_tight error");
    OK(runner, markdown_core_node_get_literal(ordered_list) == NULL, "get_literal error");
    OK(runner, markdown_core_node_get_fence_info(paragraph) == NULL, "get_fence_info error");
    INT_EQ(runner, markdown_core_node_get_fence_closed(paragraph), 0, "get_fence_closed error");

    // Setter errors

    OK(runner, !markdown_core_node_set_heading_level(bullet_list, 3), "set_heading_level error");
    OK(runner, !markdown_core_node_set_list_type(heading, MARKDOWN_CORE_ORDERED_LIST), "set_list_type error");
    OK(runner, !markdown_core_node_set_list_start(code, 3), "set_list_start error");
    OK(runner, !markdown_core_node_set_list_tight(fenced, 0), "set_list_tight error");
    OK(runner, !markdown_core_node_set_literal(ordered_list, "content\n"), "set_literal error");
    OK(runner, !markdown_core_node_set_fence_info(paragraph, "lang"), "set_fence_info error");

    OK(runner, !markdown_core_node_set_heading_level(heading, 0), "set_heading_level too small");
    OK(runner, !markdown_core_node_set_heading_level(heading, 7), "set_heading_level too large");
    OK(runner, !markdown_core_node_set_list_type(bullet_list, MARKDOWN_CORE_NO_LIST), "set_list_type invalid");
    OK(runner, !markdown_core_node_set_list_start(bullet_list, -1), "set_list_start negative");

    markdown_core_node_free(doc);
}

static markdown_core_node *parse(const char *source) { return markdown_core_parse_document(source, strlen(source)); }

static void formula_extension_accessors(test_batch_runner *runner) {
    markdown_core_node *doc = parse("Inline $x+y$ end.\n");
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));

    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "formula type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "formula inline literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
           "formula inline mode is embedded");
    INT_EQ(runner, markdown_core_extensions_set_formula_literal(formula, "z"), 1, "set formula literal succeeds");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "z",
           "formula literal setter updates payload");
    INT_EQ(runner, markdown_core_extensions_set_formula_mode(formula, MARKDOWN_CORE_FORMULA_MODE_STANDALONE), 1,
           "set formula mode succeeds");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula mode setter updates mode");
    INT_EQ(runner, markdown_core_extensions_set_formula_literal(paragraph, "nope"), 0,
           "set formula literal rejects non-formula nodes");
    INT_EQ(runner, markdown_core_extensions_set_formula_mode(paragraph, MARKDOWN_CORE_FORMULA_MODE_EMBEDDED), 0,
           "set formula mode rejects non-formula nodes");
    OK(runner, markdown_core_extensions_get_formula_literal(paragraph) == NULL,
       "get formula literal rejects non-formula nodes");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(paragraph), MARKDOWN_CORE_FORMULA_MODE_NONE,
           "get formula mode rejects non-formula nodes");
    markdown_core_node_free(doc);

    doc = parse("$$x+y$$\n");
    formula = markdown_core_node_first_child(doc);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "standalone formula block type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "standalone formula block literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula block mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("Display $$a+b$$ end.\n");
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "standalone formula inline type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "a+b", "standalone formula inline literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula inline mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("Inline \\\\(x+y\\\\) end.\n");
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "LaTeX embedded formula inline type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y",
           "LaTeX embedded formula inline literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
           "LaTeX formula inline mode is embedded");
    markdown_core_node_free(doc);

    doc = parse("Display \\\\[x+y\\\\] end.\n");
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula",
           "LaTeX standalone formula inline type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y",
           "LaTeX standalone formula inline literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "LaTeX formula inline mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("\\\\[x+y\\\\]\n");
    formula = markdown_core_node_first_child(doc);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "LaTeX standalone formula block type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y",
           "LaTeX standalone formula block literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "LaTeX formula block mode is standalone");
    markdown_core_node_free(doc);

    doc = parse("```formula\nx+y\n```\n");
    formula = markdown_core_node_first_child(doc);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "formula fence becomes standalone block");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "formula fence literal is trimmed");
    markdown_core_node_free(doc);
}

/* Reads one attribute and compares it, so a sequence can be asserted without a
 * serialization to compare against -- which is what the deleted JSON was
 * doing for these tests. */
static void attribute_eq(test_batch_runner *runner, markdown_core_node *node, size_t index, const char *name,
                         const char *value, const char *message) {
    markdown_core_string actual_name, actual_value;
    int ok = markdown_core_node_attribute_record_at(node, index, &actual_name, &actual_value);
    OK(runner,
       ok && actual_name.length == strlen(name) && memcmp(actual_name.data, name, actual_name.length) == 0 &&
           actual_value.length == strlen(value) && memcmp(actual_value.data, value, actual_value.length) == 0,
       message);
}

static void directive_extension_accessors(test_batch_runner *runner) {
    /* `:-a[]` was the input here until Step 7, and it is not a directive: a
     * name may not BEGIN with a hyphen or underscore any more than it may end
     * with one. `class` is also the one name whose repeats accumulate now, so
     * the three of them are one value rather than the last one. The sequence
     * keeps the first occurrence of each name in source order. */
    markdown_core_node *doc = parse(":a[]{id=first muted=true title=\"My Video\" bare= dup=first dup=last "
                                    "class=red class=green class=blue id=123}\n");
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *directive = markdown_core_node_first_child(paragraph);
    markdown_core_node *label = markdown_core_directive_label(directive);

    STR_EQ(runner, markdown_core_node_get_type_string(directive), "directive", "directive inline type string");
    OK(runner, label != NULL && markdown_core_node_get_type(label) == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
       "an explicit directive label is a typed node field");
    OK(runner, markdown_core_node_first_child(directive) == NULL, "an inline directive label is not a content child");
    OK(runner,
       markdown_core_node_parent(label) == NULL && markdown_core_node_previous(label) == NULL &&
           markdown_core_node_next(label) == NULL,
       "a directive label is a detached field root, not a child or sibling");
    STR_EQ(runner, markdown_core_extensions_get_directive_name(directive), "a", "directive name getter");
    markdown_core_optional_string anchor = markdown_core_node_anchor(directive);
    OK(runner, anchor.has_value && anchor.value.length == 3 && memcmp(anchor.value.data, "123", 3) == 0,
       "last ID wins");
    INT_EQ(runner, (int)markdown_core_node_attribute_record_count(directive), 5, "every record occurrence survives");
    INT_EQ(runner, (int)markdown_core_node_attribute_class_count(directive), 3, "class assignments append words");
    attribute_eq(runner, directive, 0, "muted", "true", "first record");
    attribute_eq(runner, directive, 1, "title", "My Video", "quoted value");
    attribute_eq(runner, directive, 2, "bare", "", "empty assignment");
    attribute_eq(runner, directive, 3, "dup", "first", "first duplicate");
    attribute_eq(runner, directive, 4, "dup", "last", "last duplicate");
    OK(runner, !markdown_core_node_attribute_record_at(directive, 5, NULL, NULL), "out of range refused");

    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "next_name-2"), 1,
           "set directive name succeeds");
    STR_EQ(runner, markdown_core_extensions_get_directive_name(directive), "next_name-2",
           "directive name setter updates payload");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "bad-"), 0,
           "set directive name rejects trailing hyphen");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "-bad"), 0,
           "set directive name rejects leading hyphen");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "_bad"), 0,
           "set directive name rejects leading underscore");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "bad_"), 0,
           "set directive name rejects trailing underscore");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, ""), 0,
           "set directive name rejects empty name");
    STR_EQ(runner, markdown_core_extensions_get_directive_name(directive), "next_name-2",
           "rejected directive name leaves payload unchanged");

    INT_EQ(runner, markdown_core_extensions_set_directive_name(paragraph, "ok"), 0,
           "set directive name rejects non-directive nodes");
    OK(runner, markdown_core_extensions_get_directive_name(paragraph) == NULL,
       "get directive name rejects non-directive nodes");
    INT_EQ(runner, markdown_core_node_anchor(paragraph).has_value, 0,
           "a non-directive node has no attribute container");
    INT_EQ(runner, (int)markdown_core_node_attribute_record_count(paragraph), 0,
           "a non-directive node has no attributes to count");
    markdown_core_node_free(doc);

    /* A block directive has two independent node-valued relations: `label`
     * and block content. The ordinary cmark iterator follows only the content
     * child tree; callers can start a separate walk at the label field root. */
    doc = parse(":::note[Title]\nBody\n:::\n");
    directive = markdown_core_node_first_child(doc);
    label = markdown_core_directive_label(directive);
    paragraph = markdown_core_node_first_child(directive);
    OK(runner,
       label != NULL && paragraph != NULL && markdown_core_node_get_type(paragraph) == MARKDOWN_CORE_NODE_PARAGRAPH,
       "a block directive exposes label and content as distinct relations");
    OK(runner,
       markdown_core_node_next(label) == NULL && markdown_core_node_previous(label) == NULL &&
           markdown_core_node_first_child(directive) == paragraph,
       "the label is not mixed into the block content list");
    OK(runner, markdown_core_node_parent(label) == NULL, "the directive label is a detached field root");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "the document child tree is structurally valid");
    INT_EQ(runner, markdown_core_node_check(label, NULL), 0, "the label child tree is structurally valid");
    {
        markdown_core_node *expected[] = {directive, paragraph, markdown_core_node_first_child(paragraph)};
        size_t entered = 0;
        markdown_core_iter *iter = markdown_core_iter_new(directive);
        markdown_core_event_type event;
        while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            if (event == MARKDOWN_CORE_EVENT_ENTER) {
                OK(runner,
                   entered < sizeof(expected) / sizeof(expected[0]) &&
                       markdown_core_iter_get_node(iter) == expected[entered],
                   "iterator preserves structural child order");
                entered++;
            }
        }
        INT_EQ(runner, (int)entered, (int)(sizeof(expected) / sizeof(expected[0])),
               "iterator visits only directive content nodes");
        markdown_core_iter_free(iter);
    }
    {
        markdown_core_node *expected[] = {label, markdown_core_node_first_child(label)};
        size_t entered = 0;
        markdown_core_iter *iter = markdown_core_iter_new(label);
        markdown_core_event_type event;
        while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            if (event == MARKDOWN_CORE_EVENT_ENTER) {
                OK(runner,
                   entered < sizeof(expected) / sizeof(expected[0]) &&
                       markdown_core_iter_get_node(iter) == expected[entered],
                   "a walk rooted at the label follows its own child tree");
                entered++;
            }
        }
        INT_EQ(runner, (int)entered, (int)(sizeof(expected) / sizeof(expected[0])),
               "the label tree is independently iterable");
        markdown_core_iter_free(iter);
    }
    markdown_core_node_free(doc);

    /* Missing and authored-empty containers have the same public value. */
    doc = parse(":plain[] :empty{}\n");
    paragraph = markdown_core_node_first_child(doc);
    directive = markdown_core_node_first_child(paragraph);
    for (int i = 0; i < 2; i++) {
        OK(runner, !markdown_core_node_anchor(directive).has_value, "empty value has no anchor");
        INT_EQ(runner, (int)markdown_core_node_attribute_class_count(directive), 0, "empty classes");
        INT_EQ(runner, (int)markdown_core_node_attribute_record_count(directive), 0, "empty records");
        if (i == 0) {
            directive = markdown_core_node_next(markdown_core_node_next(directive));
        }
    }
    markdown_core_node_free(doc);
}

static void node_check(test_batch_runner *runner) {
    // Construct an incomplete tree.
    markdown_core_node *doc = markdown_core_node_new(MARKDOWN_CORE_NODE_DOCUMENT);
    markdown_core_node *p1 = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    markdown_core_node *p2 = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    doc->first_child = p1;
    p1->next = p2;

    INT_EQ(runner, markdown_core_node_check(doc, NULL), 4, "node_check works");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "node_check fixes tree");

    markdown_core_node_free(doc);
}

static void iterator(test_batch_runner *runner) {
    markdown_core_node *doc = markdown_core_parse_document("> a *b*\n\nc", 10);
    int parnodes = 0;
    markdown_core_event_type ev_type;
    markdown_core_iter *iter = markdown_core_iter_new(doc);
    markdown_core_node *cur;

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (cur->kind == MARKDOWN_CORE_NODE_PARAGRAPH && ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            parnodes += 1;
        }
    }
    INT_EQ(runner, parnodes, 2, "iterate correctly counts paragraphs");

    markdown_core_iter_free(iter);
    markdown_core_node_free(doc);
}

static void iterator_delete(test_batch_runner *runner) {
    static const char md[] = "a *b* c\n"
                             "\n"
                             "* item1\n"
                             "* item2\n"
                             "\n"
                             "a `b` c\n"
                             "\n"
                             "* item1\n"
                             "* item2\n";
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1);
    markdown_core_iter *iter = markdown_core_iter_new(doc);
    markdown_core_event_type ev_type;

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        // Delete list, emph, and code nodes -- all at EXIT, which is Step 5's
        // mutation rule. `CODE` was deleted at ENTER here until the event
        // contract became total, and that was legal only because `S_is_leaf`
        // suppressed `CODE`'s EXIT. A test that frees at ENTER is a test
        // asserting the suppression list.
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT &&
            (node->kind == MARKDOWN_CORE_NODE_LIST || node->kind == MARKDOWN_CORE_NODE_EMPHASIS ||
             node->kind == MARKDOWN_CORE_NODE_CODE)) {
            markdown_core_node_free(node);
        }
    }

    // Both lists are gone and each paragraph keeps only its text pieces.
    markdown_core_node *first = markdown_core_node_first_child(doc);
    markdown_core_node *second = markdown_core_node_next(first);
    INT_EQ(runner, markdown_core_node_get_type(first), MARKDOWN_CORE_NODE_PARAGRAPH,
           "first surviving node is a paragraph");
    INT_EQ(runner, markdown_core_node_get_type(second), MARKDOWN_CORE_NODE_PARAGRAPH,
           "second surviving node is a paragraph");
    OK(runner, markdown_core_node_next(second) == NULL, "deleted lists are unlinked");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(first)), "a ",
           "first paragraph keeps leading text");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_next(markdown_core_node_first_child(first))), " c",
           "first paragraph keeps trailing text after deleted emph");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_next(markdown_core_node_first_child(second))),
           " c", "second paragraph keeps trailing text after deleted code");

    markdown_core_iter_free(iter);
    markdown_core_node_free(doc);
}

static void create_tree(test_batch_runner *runner) {
    markdown_core_node *doc = markdown_core_node_new(MARKDOWN_CORE_NODE_DOCUMENT);

    markdown_core_node *p = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    OK(runner, !markdown_core_node_insert_before(doc, p), "insert before root fails");
    OK(runner, !markdown_core_node_insert_after(doc, p), "insert after root fails");
    OK(runner, markdown_core_node_append_child(doc, p), "append1");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "append1 consistent");
    OK(runner, markdown_core_node_parent(p) == doc, "node_parent");

    markdown_core_node *emph = markdown_core_node_new(MARKDOWN_CORE_NODE_EMPHASIS);
    OK(runner, markdown_core_node_prepend_child(p, emph), "prepend1");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "prepend1 consistent");

    markdown_core_node *str1 = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    markdown_core_node_set_literal(str1, "Hello, ");
    OK(runner, markdown_core_node_prepend_child(p, str1), "prepend2");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "prepend2 consistent");

    markdown_core_node *str3 = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    markdown_core_node_set_literal(str3, "!");
    OK(runner, markdown_core_node_append_child(p, str3), "append2");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "append2 consistent");

    markdown_core_node *str2 = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    markdown_core_node_set_literal(str2, "world");
    OK(runner, markdown_core_node_append_child(emph, str2), "append3");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "append3 consistent");

    // Built tree: p -> [str1 "Hello, ", emph(str2 "world"), str3 "!"]
    OK(runner, markdown_core_node_first_child(p) == str1, "built tree starts with str1");
    OK(runner, markdown_core_node_next(str1) == emph, "emph follows str1");
    OK(runner, markdown_core_node_first_child(emph) == str2, "emph contains str2");
    OK(runner, markdown_core_node_next(emph) == str3, "str3 follows emph");
    STR_EQ(runner, markdown_core_node_get_literal(str1), "Hello, ", "str1 literal");
    STR_EQ(runner, markdown_core_node_get_literal(str2), "world", "str2 literal");
    STR_EQ(runner, markdown_core_node_get_literal(str3), "!", "str3 literal");

    OK(runner, markdown_core_node_insert_before(str1, str3), "ins before1");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "ins before1 consistent");
    // 31e
    OK(runner, markdown_core_node_first_child(p) == str3, "ins before1 works");

    OK(runner, markdown_core_node_insert_before(str1, emph), "ins before2");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "ins before2 consistent");
    // 3e1
    OK(runner, markdown_core_node_last_child(p) == str1, "ins before2 works");

    OK(runner, markdown_core_node_insert_after(str1, str3), "ins after1");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "ins after1 consistent");
    // e13
    OK(runner, markdown_core_node_next(str1) == str3, "ins after1 works");

    OK(runner, markdown_core_node_insert_after(str1, emph), "ins after2");
    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "ins after2 consistent");
    // 1e3
    OK(runner, markdown_core_node_previous(emph) == str1, "ins after2 works");

    markdown_core_node *str4 = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    markdown_core_node_set_literal(str4, "brzz");
    OK(runner, markdown_core_node_replace(str1, str4), "replace");
    // The replaced node is not freed
    markdown_core_node_free(str1);

    INT_EQ(runner, markdown_core_node_check(doc, NULL), 0, "replace consistent");
    OK(runner, markdown_core_node_previous(emph) == str4, "replace works");
    INT_EQ(runner, markdown_core_node_replace(p, str4), 0, "replace str for p fails");

    markdown_core_node_unlink(emph);

    // After shuffling: p -> [str4 "brzz", str3 "!"]
    OK(runner, markdown_core_node_first_child(p) == str4, "shuffled tree starts with str4");
    OK(runner, markdown_core_node_next(str4) == str3, "str3 follows str4");
    OK(runner, markdown_core_node_next(str3) == NULL, "unlinked emph is gone");
    STR_EQ(runner, markdown_core_node_get_literal(str4), "brzz", "str4 literal");

    markdown_core_node_free(doc);

    // The inherited mutable engine API guarantees the unlinked node itself,
    // but not descendants formerly owned through the destroyed parent. The
    // immutable public facade does not expose this ownership state.
    markdown_core_node_free(emph);
}

void hierarchy(test_batch_runner *runner) {
    markdown_core_node *bquote1 = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);
    markdown_core_node *bquote2 = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);
    markdown_core_node *bquote3 = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);

    OK(runner, markdown_core_node_append_child(bquote1, bquote2), "append bquote2");
    OK(runner, markdown_core_node_append_child(bquote2, bquote3), "append bquote3");
    OK(runner, !markdown_core_node_append_child(bquote3, bquote3), "adding a node as child of itself fails");
    OK(runner, !markdown_core_node_append_child(bquote3, bquote1), "adding a parent as child fails");

    markdown_core_node_free(bquote1);

    unsigned int list_item_flag[] = {MARKDOWN_CORE_NODE_LIST_ITEM, 0};
    unsigned int top_level_blocks[] = {
        MARKDOWN_CORE_NODE_CALLOUT,    MARKDOWN_CORE_NODE_LIST,           MARKDOWN_CORE_NODE_CODE_BLOCK,
        MARKDOWN_CORE_NODE_HTML_BLOCK, MARKDOWN_CORE_NODE_COMMENT_BLOCK,  MARKDOWN_CORE_NODE_PARAGRAPH,
        MARKDOWN_CORE_NODE_HEADING,    MARKDOWN_CORE_NODE_THEMATIC_BREAK, 0};
    unsigned int all_inlines[] = {MARKDOWN_CORE_NODE_TEXT,
                                  MARKDOWN_CORE_NODE_SOFT_BREAK,
                                  MARKDOWN_CORE_NODE_LINE_BREAK,
                                  MARKDOWN_CORE_NODE_CODE,
                                  MARKDOWN_CORE_NODE_HTML,
                                  MARKDOWN_CORE_NODE_COMMENT,
                                  MARKDOWN_CORE_NODE_EMPHASIS,
                                  MARKDOWN_CORE_NODE_STRONG,
                                  MARKDOWN_CORE_NODE_LINK,
                                  MARKDOWN_CORE_NODE_MEDIA,
                                  0};

    test_content(runner, MARKDOWN_CORE_NODE_DOCUMENT, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_CALLOUT, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_LIST, list_item_flag);
    test_content(runner, MARKDOWN_CORE_NODE_LIST_ITEM, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_CODE_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_COMMENT_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_PARAGRAPH, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_HEADING, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_THEMATIC_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_TEXT, 0);
    test_content(runner, MARKDOWN_CORE_NODE_SOFT_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_LINE_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_CODE, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML, 0);
    test_content(runner, MARKDOWN_CORE_NODE_COMMENT, 0);
    test_content(runner, MARKDOWN_CORE_NODE_EMPHASIS, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_STRONG, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_LINK, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_MEDIA, all_inlines);
}

static void test_content(test_batch_runner *runner, markdown_core_node_type type, unsigned int *allowed_content) {
    markdown_core_node *node = markdown_core_node_new(type);

    for (int i = 0; i < num_node_types; ++i) {
        markdown_core_node_type child_type = node_types[i];
        markdown_core_node *child = markdown_core_node_new(child_type);

        int got = markdown_core_node_append_child(node, child);
        int expected = 0;
        if (allowed_content) {
            for (unsigned int *p = allowed_content; *p; ++p) {
                expected |= *p == (unsigned int)child_type;
            }
        }

        INT_EQ(runner, got, expected, "add %d as child of %d", child_type, type);

        markdown_core_node_free(child);
    }

    markdown_core_node_free(node);
}

static void parser(test_batch_runner *runner) {
    test_md_paragraph_text(runner, "No newline", "No newline", "document without trailing newline");
}

static void utf8(test_batch_runner *runner) {
    test_valid_char(runner, "\x01", "valid utf8 01");
    test_valid_char(runner, "\x7F", "valid utf8 7F");
    test_valid_char(runner, "\xC2\x80", "valid utf8 C280");
    test_valid_char(runner, "\xDF\xBF", "valid utf8 DFBF");
    test_valid_char(runner, "\xE0\xA0\x80", "valid utf8 E0A080");
    test_valid_char(runner, "\xED\x9F\xBF", "valid utf8 ED9FBF");
    test_valid_char(runner, "\xEF\xBF\xBE", "valid utf8 U+FFFE noncharacter");
    test_valid_char(runner, "\xEF\xBF\xBF", "valid utf8 U+FFFF noncharacter");
    test_valid_char(runner, "\xF0\x90\x80\x80", "valid utf8 F0908080");
    test_valid_char(runner, "\xF4\x8F\xBF\xBF", "valid utf8 F48FBFBF");

    // Test string containing null character
    static const char string_with_null[] = "((((\0))))";
    test_md_paragraph_bytes(runner, string_with_null, sizeof(string_with_null) - 1, "((((" UTF8_REPL "))))",
                            "utf8 with U+0000");

    // Test NUL followed by newline
    static const char string_with_nul_lf[] = "```\n\0\n```\n";
    markdown_core_node *doc = markdown_core_parse_document(string_with_nul_lf, sizeof(string_with_nul_lf) - 1);
    markdown_core_node *code_block = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_type(code_block), MARKDOWN_CORE_NODE_CODE_BLOCK,
           "utf8 with \\0\\n parses a code block");
    STR_EQ(runner, markdown_core_node_get_literal(code_block), UTF8_REPL "\n", "utf8 with \\0\\n");
    markdown_core_node_free(doc);

    // Test byte-order marker
    static const char string_with_bom[] = "\xef\xbb\xbf# Hello\n";
    doc = markdown_core_parse_document(string_with_bom, sizeof(string_with_bom) - 1);
    markdown_core_node *heading = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_type(heading), MARKDOWN_CORE_NODE_HEADING, "utf8 with BOM parses a heading");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(heading)), "Hello", "utf8 with BOM");
    markdown_core_node_free(doc);
}

static void test_valid_char(test_batch_runner *runner, const char *utf8, const char *msg) {
    char buf[20];
    char expected[30];

    snprintf(buf, sizeof(buf), "((((%s))))", utf8);
    snprintf(expected, sizeof(expected), "((((%s))))", utf8);
    test_md_paragraph_text(runner, buf, expected, msg);
}

static void line_endings(test_batch_runner *runner) {
    // Test list with different line endings
    static const char list_with_endings[] = "- a\n- b\r\n- c\r- d";
    static const char *const expected_items[] = {"a", "b", "c", "d"};
    markdown_core_node *doc = markdown_core_parse_document(list_with_endings, sizeof(list_with_endings) - 1);
    markdown_core_node *list = markdown_core_node_first_child(doc);
    markdown_core_node *item = markdown_core_node_first_child(list);
    INT_EQ(runner, markdown_core_node_get_type(list), MARKDOWN_CORE_NODE_LIST,
           "list with different line endings parses one list");
    for (size_t i = 0; i < 4; i++) {
        OK(runner, item != NULL, "list item %zu exists", i);
        if (item) {
            markdown_core_node *paragraph = markdown_core_node_first_child(item);
            STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(paragraph)), expected_items[i],
                   "list item %zu text", i);
            item = markdown_core_node_next(item);
        }
    }
    OK(runner, item == NULL, "list has exactly four items");
    markdown_core_node_free(doc);

    // A CRLF line ending is a SoftBreak between the two texts.
    static const char crlf_lines[] = "line\r\nline\r\n";
    doc = markdown_core_parse_document(crlf_lines, sizeof(crlf_lines) - 1);
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *middle = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(paragraph)), "line",
           "crlf line splits into text");
    INT_EQ(runner, markdown_core_node_get_type(middle), MARKDOWN_CORE_NODE_SOFT_BREAK,
           "crlf endings produce a softbreak");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_next(middle)), "line",
           "crlf trailing text follows the softbreak");
    markdown_core_node_free(doc);

    static const char no_line_ending[] = "```\nline\n```";
    doc = markdown_core_parse_document(no_line_ending, sizeof(no_line_ending) - 1);
    markdown_core_node *code_block = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_type(code_block), MARKDOWN_CORE_NODE_CODE_BLOCK,
           "fenced code block with no final newline parses");
    STR_EQ(runner, markdown_core_node_get_literal(code_block), "line\n", "fenced code block with no final newline");
    markdown_core_node_free(doc);
}

static void numeric_entities(test_batch_runner *runner) {
    test_md_paragraph_text(runner, "&#0;", UTF8_REPL, "Invalid numeric entity 0");
    test_md_paragraph_text(runner, "&#55295;", "\xED\x9F\xBF", "Valid numeric entity 0xD7FF");
    test_md_paragraph_text(runner, "&#xD800;", UTF8_REPL, "Invalid numeric entity 0xD800");
    test_md_paragraph_text(runner, "&#xDFFF;", UTF8_REPL, "Invalid numeric entity 0xDFFF");
    test_md_paragraph_text(runner, "&#57344;", "\xEE\x80\x80", "Valid numeric entity 0xE000");
    test_md_paragraph_text(runner, "&#x10FFFF;", "\xF4\x8F\xBF\xBF", "Valid numeric entity 0x10FFFF");
    test_md_paragraph_text(runner, "&#x110000;", UTF8_REPL, "Invalid numeric entity 0x110000");
    test_md_paragraph_text(runner, "&#9999999;", UTF8_REPL, "Seven-digit decimal entity is recognized");
    test_md_paragraph_text(runner, "&#99999999;", "&#99999999;", "Eight-digit decimal entity stays literal");
    test_md_paragraph_text(runner, "&#87654321;", "&#87654321;", "CommonMark long decimal entity stays literal");
    test_md_paragraph_text(runner, "&#x80000000;", "&#x80000000;", "Eight-digit hexadecimal entity stays literal");
    test_md_paragraph_text(runner, "&#xFFFFFFFF;", "&#xFFFFFFFF;", "Long hexadecimal entity stays literal");

    test_md_paragraph_text(runner, "&#;", "&#;", "Min decimal entity length");
    test_md_paragraph_text(runner, "&#x;", "&#x;", "Min hexadecimal entity length");
    test_md_paragraph_text(runner, "&#999999999;", "&#999999999;", "Overlong decimal entity stays literal");
    test_md_paragraph_text(runner, "&#x000000041;", "&#x000000041;", "Overlong hexadecimal entity stays literal");
    test_md_paragraph_text(runner, "a*\xC2\xA3*b", "a*\xC2\xA3*b",
                           "Unicode symbols are punctuation for emphasis flanking");
}

/* M0: an HTML comment is a `Comment` node of the dialect -- inline for the
 * token, block for an HTML block that opened with `<!--` and whose end line
 * held only whitespace after the first `-->` -- and its literal is the bytes
 * between the delimiters. Nothing strips it; the text around an inline
 * comment stays split where the comment sits. */
static void comment_nodes(test_batch_runner *runner) {
    static const char markdown[] = "before <!-- hidden --> after <br>\n"
                                   "\n"
                                   "<!-- block\n"
                                   "hidden -->\n"
                                   "\n"
                                   "<div>raw</div>\n"
                                   "\n"
                                   "<!-- a --> b\n"
                                   "\n"
                                   "<!-->\n";

    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1);
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *text = markdown_core_node_first_child(paragraph);
    markdown_core_node *comment = markdown_core_node_next(text);
    markdown_core_node *after = markdown_core_node_next(comment);
    markdown_core_node *inline_html = markdown_core_node_next(after);
    markdown_core_node *block_comment = markdown_core_node_next(paragraph);
    markdown_core_node *block_html = markdown_core_node_next(block_comment);
    markdown_core_node *trailing = markdown_core_node_next(block_html);
    markdown_core_node *empties = markdown_core_node_next(trailing);
    markdown_core_node *empty;

    STR_EQ(runner, markdown_core_node_get_literal(text), "before ", "text before an inline comment");
    INT_EQ(runner, markdown_core_node_get_type(comment), MARKDOWN_CORE_NODE_COMMENT, "inline comment type");
    STR_EQ(runner, markdown_core_node_get_literal(comment), " hidden ", "inline comment literal excludes delimiters");
    STR_EQ(runner, markdown_core_node_get_type_string(comment), "comment", "inline comment type string");
    STR_EQ(runner, markdown_core_node_get_literal(after), " after ", "text after an inline comment");
    INT_EQ(runner, markdown_core_node_get_type(inline_html), MARKDOWN_CORE_NODE_HTML, "a tag stays inline HTML");
    STR_EQ(runner, markdown_core_node_get_literal(inline_html), "<br>", "inline HTML literal");

    INT_EQ(runner, markdown_core_node_get_type(block_comment), MARKDOWN_CORE_NODE_COMMENT_BLOCK, "block comment type");
    STR_EQ(runner, markdown_core_node_get_literal(block_comment), " block\nhidden ",
           "block comment literal keeps its line ending and excludes delimiters");
    STR_EQ(runner, markdown_core_node_get_type_string(block_comment), "comment_block", "block comment type string");
    INT_EQ(runner, markdown_core_node_get_start_line(block_comment), 3, "block comment starts on its opener line");
    INT_EQ(runner, markdown_core_node_get_end_line(block_comment), 4, "block comment ends on its closer line");
    INT_EQ(runner, markdown_core_node_get_end_column(block_comment), 10, "block comment ends at its closer");

    INT_EQ(runner, markdown_core_node_get_type(block_html), MARKDOWN_CORE_NODE_HTML_BLOCK, "a div stays an HTML block");
    STR_EQ(runner, markdown_core_node_get_literal(block_html), "<div>raw</div>\n", "HTML block literal");
    INT_EQ(runner, markdown_core_node_get_type(trailing), MARKDOWN_CORE_NODE_HTML_BLOCK,
           "non-whitespace after the closer keeps the HTML block");
    STR_EQ(runner, markdown_core_node_get_literal(trailing), "<!-- a --> b\n", "HTML block literal as written");

    INT_EQ(runner, markdown_core_node_get_type(empties), MARKDOWN_CORE_NODE_COMMENT_BLOCK,
           "`<!-->` opens a block comment");
    STR_EQ(runner, markdown_core_node_get_literal(empties), "", "`<!-->` as a block has an empty literal");
    OK(runner, markdown_core_node_first_child(empties) == NULL, "a block comment is a leaf");

    markdown_core_node_free(doc);

    doc = markdown_core_parse_document("a <!--> b <!---> c <!----> d\n", 29);
    paragraph = markdown_core_node_first_child(doc);
    empty = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    INT_EQ(runner, markdown_core_node_get_type(empty), MARKDOWN_CORE_NODE_COMMENT, "`<!-->` is an inline comment");
    STR_EQ(runner, markdown_core_node_get_literal(empty), "", "`<!-->` has an empty literal");
    INT_EQ(runner, markdown_core_node_get_start_column(empty), 3, "`<!-->` starts at its `<`");
    INT_EQ(runner, markdown_core_node_get_end_column(empty), 7, "`<!-->` ends at its `>`");
    empty = markdown_core_node_next(markdown_core_node_next(empty));
    INT_EQ(runner, markdown_core_node_get_type(empty), MARKDOWN_CORE_NODE_COMMENT, "`<!--->` is an inline comment");
    STR_EQ(runner, markdown_core_node_get_literal(empty), "", "`<!--->` has an empty literal");
    empty = markdown_core_node_next(markdown_core_node_next(empty));
    INT_EQ(runner, markdown_core_node_get_type(empty), MARKDOWN_CORE_NODE_COMMENT, "`<!---->` is an inline comment");
    STR_EQ(runner, markdown_core_node_get_literal(empty), "", "`<!---->` has an empty literal");
    markdown_core_node_free(doc);

    /* The line splitter is the inherited one: a CR, an LF, or a CRLF ends a
     * line, and every block receives its lines LF-terminated, so every
     * literal in the tree -- a code block's, an HTML block's, a comment's --
     * holds LF where the source held CR or CRLF. cmark 0.31.2 stores the same
     * bytes for the same input. A comment that kept the CR bytes alone would
     * be the one literal out of step with its neighbours. */
    {
        static const char crlf[] = "a <!--x\r\ny--> b\r\n\r\n<!--\r\nx\r\n-->\r\n\r\n```\r\nx\r\n```\r\n";
        static const char cr[] = "a <!--x\ry--> b\r\r<!--\rx\r-->\r";
        markdown_core_node *code;
        doc = markdown_core_parse_document(crlf, sizeof(crlf) - 1);
        paragraph = markdown_core_node_first_child(doc);
        comment = markdown_core_node_next(markdown_core_node_first_child(paragraph));
        block_comment = markdown_core_node_next(paragraph);
        code = markdown_core_node_next(block_comment);
        INT_EQ(runner, markdown_core_node_get_type(comment), MARKDOWN_CORE_NODE_COMMENT, "CRLF: inline comment");
        STR_EQ(runner, markdown_core_node_get_literal(comment), "x\ny",
               "CRLF inside an inline comment is stored as LF");
        INT_EQ(runner, markdown_core_node_get_end_line(comment), 2, "CRLF: the inline comment ends on line 2");
        INT_EQ(runner, markdown_core_node_get_type(block_comment), MARKDOWN_CORE_NODE_COMMENT_BLOCK,
               "CRLF: block comment");
        STR_EQ(runner, markdown_core_node_get_literal(block_comment), "\nx\n",
               "CRLF inside a block comment is stored as LF");
        INT_EQ(runner, markdown_core_node_get_end_line(block_comment), 6,
               "CRLF: the block comment ends on its closer line");
        INT_EQ(runner, markdown_core_node_get_type(code), MARKDOWN_CORE_NODE_CODE_BLOCK, "CRLF: code block");
        STR_EQ(runner, markdown_core_node_get_literal(code), "x\n", "CRLF inside a code block is stored as LF too");
        markdown_core_node_free(doc);

        doc = markdown_core_parse_document(cr, sizeof(cr) - 1);
        paragraph = markdown_core_node_first_child(doc);
        comment = markdown_core_node_next(markdown_core_node_first_child(paragraph));
        block_comment = markdown_core_node_next(paragraph);
        STR_EQ(runner, markdown_core_node_get_literal(comment), "x\ny",
               "a lone CR inside an inline comment is stored as LF");
        STR_EQ(runner, markdown_core_node_get_literal(block_comment), "\nx\n",
               "a lone CR inside a block comment is stored as LF");
        INT_EQ(runner, markdown_core_node_get_end_line(block_comment), 6,
               "CR: the block comment ends on its closer line");
        markdown_core_node_free(doc);
    }
}

/* Parses and asserts the document is a single paragraph whose concatenated
 * Text literals equal `expected_text`.  This replaces the retired
 * markdown_to_html comparisons: AST literals carry raw bytes, without HTML
 * escaping. */
static void test_md_paragraph_bytes(test_batch_runner *runner, const char *markdown, size_t markdown_length,
                                    const char *expected_text, const char *msg) {
    markdown_core_node *doc = markdown_core_parse_document(markdown, markdown_length);
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    char text[4096] = "";
    size_t length = 0;
    markdown_core_node *child;

    if (markdown_core_node_get_type(paragraph) != MARKDOWN_CORE_NODE_PARAGRAPH ||
        markdown_core_node_next(paragraph) != NULL) {
        OK(runner, 0, "%s (document is a single paragraph)", msg);
        markdown_core_node_free(doc);
        return;
    }
    for (child = markdown_core_node_first_child(paragraph); child; child = markdown_core_node_next(child)) {
        const char *literal = markdown_core_node_get_literal(child);
        size_t literal_length;
        if (markdown_core_node_get_type(child) != MARKDOWN_CORE_NODE_TEXT || !literal) {
            OK(runner, 0, "%s (paragraph contains only text)", msg);
            markdown_core_node_free(doc);
            return;
        }
        literal_length = strlen(literal);
        if (length + literal_length + 1 > sizeof(text)) {
            OK(runner, 0, "%s (text fits the harness buffer)", msg);
            markdown_core_node_free(doc);
            return;
        }
        memcpy(text + length, literal, literal_length + 1);
        length += literal_length;
    }
    STR_EQ(runner, text, expected_text, "%s", msg);
    markdown_core_node_free(doc);
}

static void test_md_paragraph_text(test_batch_runner *runner, const char *markdown, const char *expected_text,
                                   const char *msg) {
    test_md_paragraph_bytes(runner, markdown, strlen(markdown), expected_text, msg);
}

static void test_crlf_line_ending(test_batch_runner *runner) {
    const char *source = "line1\r\nline2\r\n";
    markdown_core_node *document = markdown_core_parse_document(source, strlen(source));
    OK(runner, document->first_child->next == NULL, "document has one paragraph");
    markdown_core_node_free(document);
}

static void test_pathological_parse_completion(test_batch_runner *runner, const char *pattern, size_t repetitions,
                                               const char *msg) {
    size_t pattern_length = strlen(pattern);
    size_t input_length = pattern_length * repetitions;
    char *input = (char *)malloc(input_length);
    markdown_core_node *document;

    OK(runner, input != NULL, "%s (input allocation succeeds)", msg);
    if (!input) {
        return;
    }
    for (size_t i = 0; i < repetitions; ++i) {
        memcpy(input + i * pattern_length, pattern, pattern_length);
    }

    document = markdown_core_parse_document(input, input_length);
    OK(runner, document != NULL, "%s (parse succeeds)", msg);
    markdown_core_node_free(document);
    free(input);
}

static void test_pathological_regressions(test_batch_runner *runner) {
    test_pathological_parse_completion(runner, "[a](b", 50000, "unclosed inline destination");
    test_pathological_parse_completion(runner, "[a](<b", 50000, "unclosed pointy inline destination");
}

/* Parses through the read-only facade and compares the canonical AST dump,
 * which carries every node's scope, byte-for-byte.  This replaces the
 * retired sourcepos XML renderer assertions. */
static void test_facade_dump(test_batch_runner *runner, const char *markdown, const char *expected_dump,
                             const char *msg) {
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    uint8_t *dump = NULL;
    size_t dump_length = 0;

    document = markdown_core_document_parse((const uint8_t *)markdown, strlen(markdown), &error);
    if (!document) {
        OK(runner, 0, "%s (facade parse succeeds)", msg);
        markdown_core_error_free(error);
        return;
    }
    if (!markdown_core_document_dump(document, &dump, &dump_length, &error)) {
        OK(runner, 0, "%s (facade dump succeeds)", msg);
        markdown_core_error_free(error);
        markdown_core_document_free(document);
        return;
    }
    STR_EQ(runner, (const char *)dump, expected_dump, "%s", msg);
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);
}

// An extension that declines to open a block must answer NULL. The parser
// offers each attached extension a turn in attach order and stops at the first
// non-NULL answer, so an extension that returns the parent container on a
// decline takes away the turn of every extension attached after it -- and
// `table` used to do exactly that, on every path including "there is no table
// here". Enabling tables then stopped a directive block from interrupting a
// paragraph.
//
// The parse always uses the dialect's fixed order.
static void extension_decline_yields_turn(test_batch_runner *runner) {
    /* Test the decline contract directly, so the fixed attach order cannot
     * hide a table matcher that wrongly returns its unchanged parent. */
    markdown_core_parser parser = {0};
    parser.mem = markdown_core_get_default_mem_allocator();
    markdown_core_node *candidate = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    markdown_core_strbuf_puts(&candidate->content, "text\n");
    unsigned char line[] = ":::note\n";
    OK(runner,
       MARKDOWN_CORE_EXTENSION_TABLE.try_opening_block(&MARKDOWN_CORE_EXTENSION_TABLE, 0, &parser, candidate, line,
                                                       sizeof(line) - 1) == NULL,
       "a non-table line yields no block, independently of attach order");
    markdown_core_node_free(candidate);

    static const char *const markdown = "text\n:::note\nbody\n:::\n";
    markdown_core_node *doc = parse(markdown);
    OK(runner, doc != NULL, "the full dialect parses the extension conflict");
    if (!doc) {
        return;
    }

    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *block = paragraph ? markdown_core_node_next(paragraph) : NULL;
    INT_EQ(runner, markdown_core_node_get_type(paragraph), MARKDOWN_CORE_NODE_PARAGRAPH,
           "the lead paragraph survives table declining");
    OK(runner, block != NULL, "the directive interrupts the ordinary paragraph");
    STR_EQ(runner, block ? markdown_core_node_get_type_string(block) : "", "directive_block",
           "a declining table does not swallow the directive block");
    markdown_core_node_free(doc);
}

/* Step 5. The event contract is TOTAL: every node yields exactly one ENTER and
 * exactly one EXIT, in that order, with its descendants' events between them.
 *
 * Until Step 5 an internal `S_is_leaf` list of eight node types suppressed the
 * EXIT of a node that "cannot have children" -- a list, not a property, so a
 * `FOOTNOTE_REFERENCE` with no children got an EXIT and a `TEXT` with no
 * children did not, and every walk in the engine had to know which. Three did:
 * `consolidate_text_nodes`, the HTML-comment stripper `M0` deleted, and
 * `autolink`'s `postprocess`, and all three freed or spliced at ENTER because
 * the suppression made it safe. The input below contains one of every suppressed
 * kind. */
static size_t total_nodes(markdown_core_node *node) {
    size_t n = 1;
    for (markdown_core_node *c = markdown_core_node_first_child(node); c; c = markdown_core_node_next(c)) {
        n += total_nodes(c);
    }
    return n;
}

static void iterator_contract_is_total(test_batch_runner *runner) {
    static const char md[] = "---\n"
                             "\n"
                             "<div>html block</div>\n"
                             "\n"
                             "    code block\n"
                             "\n"
                             "a `code` b <span>html</span> c\\\n"
                             "d\n"
                             "e\n";
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1);
    markdown_core_iter *iter = markdown_core_iter_new(doc);
    markdown_core_node *stack[64];
    size_t depth = 0, enters = 0, exits = 0, mismatched = 0, overflow = 0;
    markdown_core_event_type ev;

    while ((ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev == MARKDOWN_CORE_EVENT_ENTER) {
            enters += 1;
            if (depth == sizeof(stack) / sizeof(stack[0])) {
                overflow += 1;
            } else {
                stack[depth++] = node;
            }
        } else if (ev == MARKDOWN_CORE_EVENT_EXIT) {
            exits += 1;
            if (depth == 0 || stack[--depth] != node) {
                mismatched += 1;
            }
        }
    }

    INT_EQ(runner, (int)enters, (int)total_nodes(doc), "every node is entered exactly once");
    INT_EQ(runner, (int)exits, (int)enters, "every node is exited exactly once");
    INT_EQ(runner, (int)mismatched, 0, "every EXIT closes the ENTER it belongs to");
    INT_EQ(runner, (int)depth, 0, "the walk ends with nothing left open");
    INT_EQ(runner, (int)overflow, 0, "the test's stack was deep enough");

    markdown_core_iter_free(iter);
    markdown_core_node_free(doc);
}

/* 3b. The ancestor check is unconditional, so the shipped library answers the
 * same as the test suite. It used to sit behind
 * `markdown_core_enable_safety_checks`, which defaulted to OFF and which only
 * `main()` here ever turned on -- so what shipped made `q->parent == q` on
 * request and returned success, while the tests that denied it flipped a flag
 * nothing else flipped. */
static void no_node_is_its_own_ancestor(test_batch_runner *runner) {
    markdown_core_node *q = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);
    markdown_core_node *r = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);
    markdown_core_node *a = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);
    markdown_core_node *b = markdown_core_node_new(MARKDOWN_CORE_NODE_CALLOUT);

    INT_EQ(runner, markdown_core_node_append_child(q, q), 0, "a node cannot be appended to itself");
    OK(runner, q->parent != q, "and it is not left as its own parent");
    INT_EQ(runner, markdown_core_node_prepend_child(r, r), 0, "a node cannot be prepended to itself");
    OK(runner, r->parent != r, "and it is not left as its own parent");

    INT_EQ(runner, markdown_core_node_append_child(a, b), 1, "b becomes a child of a");
    INT_EQ(runner, markdown_core_node_append_child(b, a), 0, "and a cannot then become a child of b");
    OK(runner, a->parent == NULL, "so there is no two-node cycle");

    INT_EQ(runner, markdown_core_node_insert_before(b, b), 0, "a node cannot be inserted before itself");
    INT_EQ(runner, markdown_core_node_insert_after(b, b), 0, "a node cannot be inserted after itself");

    markdown_core_node_free(a);
    markdown_core_node_free(q);
    markdown_core_node_free(r);
}

/* D33. `process_emphasis` used to choose its arm by the delimiter's BYTE:
 *
 *     if (extension)                       ... else
 *     if (delim_char == '*' || '_')        ... else
 *     if (delim_char == '\'' || '"')        ...
 *
 * where `extension` was "the first attached extension whose dispatch set
 * contains this byte". A delimiter matching none of the three left `closer`
 * exactly where it was, fell into the removal below, freed it, and read it
 * again on the next turn -- ASan `heap-use-after-free`, READ of size 8 in
 * `process_emphasis`. With `can_open` set nothing freed it and the loop never
 * ended at all.
 *
 * No in-tree extension reaches it, because each pushes a tag it also declares.
 * The public push does not care: it is one call from any extension, and the two
 * descriptors below are the two ways to make it. The first pushes a real rule
 * with a NULL owner, so nothing can handle it; the second pushes a rule outside
 * the enum, which would also index `openers_bottom` out of bounds.
 *
 * They are `static const` descriptors, like every extension since 3.4. A test
 * may still build one -- what 3.4 removed is the ability to REGISTER one, look
 * one up by name, or mutate one after the fact. */
static markdown_core_node *stray_delimiter_push(markdown_core_parser *parser,
                                                markdown_core_inline_parser *inline_parser, unsigned char character,
                                                markdown_core_delimiter_rule rule) {
    markdown_core_node *node;

    (void)parser;
    if (character != '@') {
        return NULL;
    }
    markdown_core_inline_parser_advance_offset(inline_parser);
    node = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    if (!node) {
        return NULL;
    }
    markdown_core_node_set_literal(node, "@");
    int offset = markdown_core_inline_parser_get_offset(inline_parser);
    markdown_core_inline_parser_place(inline_parser, node, offset - 1, offset - 1);
    markdown_core_inline_parser_push_delimiter(inline_parser, NULL, rule, 0, 1, node);
    return node;
}

static markdown_core_node *stray_unowned_match(const markdown_core_extension *self, markdown_core_parser *parser,
                                               markdown_core_node *parent, unsigned char character,
                                               markdown_core_inline_parser *inline_parser) {
    (void)self;
    (void)parent;
    return stray_delimiter_push(parser, inline_parser, character, MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH);
}

static markdown_core_node *stray_unnamed_match(const markdown_core_extension *self, markdown_core_parser *parser,
                                               markdown_core_node *parent, unsigned char character,
                                               markdown_core_inline_parser *inline_parser) {
    (void)self;
    (void)parent;
    return stray_delimiter_push(parser, inline_parser, character, (markdown_core_delimiter_rule)200);
}

static const markdown_core_extension STRAY_UNOWNED = {
    .name = "stray-unowned", .match_inline = stray_unowned_match, .terminates_text = "@", .dispatch = "@"};
static const markdown_core_extension STRAY_UNNAMED = {
    .name = "stray-unnamed", .match_inline = stray_unnamed_match, .terminates_text = "@", .dispatch = "@"};

static void stray_delimiter_parse(test_batch_runner *runner, const markdown_core_extension *extension,
                                  const char *what) {
    const char *input = "a @ b @ c\n";
    markdown_core_node *document = parse_with_probes(input, strlen(input), &extension, 1);

    OK(runner, document != NULL, "a delimiter with %s still finishes the parse", what);
    markdown_core_node_free(document);
}

static void stray_delimiter(test_batch_runner *runner) {
    stray_delimiter_parse(runner, &STRAY_UNOWNED, "a rule and no owner");
    stray_delimiter_parse(runner, &STRAY_UNNAMED, "a rule outside the enum");
}

/* A1. An allocation failure is a fact about the write that failed, not a
 * property the buffer keeps. `markdown_core_strbuf_clear` used not to lift
 * `oom`, and `markdown_core_strbuf_detach` was the only operation in the engine
 * that did -- so a buffer cleared and reused went on silently dropping every
 * later write WITH THE ALLOCATOR WORKING AGAIN.
 *
 * This is a property test rather than a parse test on purpose. Measured at
 * 3a.3: reverting the lift alone leaves `correctness` at 69/69 and both
 * allocation-failure sweeps green, because the two engine buffers that are
 * cleared and reused -- `parser->curline` and `parser->line_scratch` -- now both
 * report at the transaction and abandon the parse before the reuse. The lift
 * removes the class by construction, and nothing else can see it. */
static int strbuf_refuse_next;
static void *strbuf_test_calloc(size_t n, size_t size) {
    if (strbuf_refuse_next) {
        strbuf_refuse_next = 0;
        return NULL;
    }
    return calloc(n, size);
}
static void *strbuf_test_realloc(void *pointer, size_t size) {
    if (strbuf_refuse_next) {
        strbuf_refuse_next = 0;
        return NULL;
    }
    return realloc(pointer, size);
}
static void strbuf_test_free(void *pointer) { free(pointer); }
static markdown_core_mem strbuf_test_mem = {strbuf_test_calloc, strbuf_test_realloc, strbuf_test_free};

static void strbuf_failure_is_a_transaction(test_batch_runner *runner) {
    markdown_core_strbuf buf;

    markdown_core_strbuf_init(&strbuf_test_mem, &buf, 0);
    strbuf_refuse_next = 1;
    markdown_core_strbuf_put(&buf, (const unsigned char *)"hello", 5);
    INT_EQ(runner, buf.oom, 1, "a refused growth poisons the buffer");
    INT_EQ(runner, buf.size, 0, "a refused growth writes nothing");

    markdown_core_strbuf_clear(&buf);
    INT_EQ(runner, buf.oom, 0, "clearing the buffer lifts the failure with the content it described");

    markdown_core_strbuf_put(&buf, (const unsigned char *)"world", 5);
    INT_EQ(runner, buf.oom, 0, "the next write succeeds with the allocator working again");
    INT_EQ(runner, buf.size, 5, "the next write lands");
    STR_EQ(runner, markdown_core_strbuf_cstr(&buf), "world", "and it lands intact");

    markdown_core_strbuf_free(&buf);
}

/* A4. `bufsize_t` is int32_t, and every append went through
 * `markdown_core_strbuf_grow(buf, buf->size + add)`. Two things were wrong and
 * either alone is enough:
 *
 *   the sum is undefined behaviour past INT32_MAX, and wraps NEGATIVE;
 *   `grow` answered a negative target with "already big enough" -- silently,
 *   because its `assert(target_size > 0)` compiles out under NDEBUG.
 *
 * `put` then memmoved `add` bytes into a buffer that had not grown. Measured
 * before the fix, by direct call: SIGSEGV, status 139, writing 1,073,741,833
 * bytes into an eight-byte allocation.
 *
 * The forged `size` below is the largest a legitimate buffer may hold -- the
 * cap is INT32_MAX/2 -- so this is the state the normalized-line scratch can
 * reach after an embedded NUL in a 1.07 GiB line, and the put is the remaining
 * segment. It is forged because constructing that source costs 2 GiB. */
static void strbuf_overflow(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    markdown_core_strbuf buf;
    unsigned char data[16] = {0};

    markdown_core_strbuf_init(mem, &buf, 0);
    markdown_core_strbuf_grow(&buf, -1);
    INT_EQ(runner, buf.oom, 1, "a negative grow target poisons the buffer");
    INT_EQ(runner, buf.asize, 0, "a negative grow target allocates nothing");
    markdown_core_strbuf_free(&buf);

    markdown_core_strbuf_init(mem, &buf, 0);
    markdown_core_strbuf_grow(&buf, 0);
    INT_EQ(runner, buf.oom, 1, "a zero grow target poisons the buffer");
    markdown_core_strbuf_free(&buf);

    /* The overflow itself. Without the fix this line does not return. */
    markdown_core_strbuf_init(mem, &buf, 8);
    buf.size = (bufsize_t)(INT32_MAX / 2);
    markdown_core_strbuf_put(&buf, data, (bufsize_t)(INT32_MAX / 2) + 10);
    INT_EQ(runner, buf.oom, 1, "an append whose length overflows the size sum poisons instead of writing");
    INT_EQ(runner, buf.size, (bufsize_t)(INT32_MAX / 2), "the refused append moves nothing");
    buf.size = 0;
    markdown_core_strbuf_free(&buf);

    /* And the guard is not over-tight: an ordinary large append still works. */
    markdown_core_strbuf_init(mem, &buf, 0);
    for (int i = 0; i < 4096; i++) {
        markdown_core_strbuf_put(&buf, data, (bufsize_t)sizeof(data));
    }
    INT_EQ(runner, buf.oom, 0, "4096 ordinary appends do not poison");
    INT_EQ(runner, buf.size, 4096 * (bufsize_t)sizeof(data), "4096 ordinary appends all landed");
    markdown_core_strbuf_free(&buf);
}

static void source_pos(test_batch_runner *runner) {
    static const char markdown[] = "# Hi *there*.\n"
                                   "\n"
                                   "Hello &ldquo; <http://www.google.com>\n"
                                   "there `hi` -- [okay](www.google.com (ok)).\n"
                                   "\n"
                                   "> 1. Okay.\n"
                                   ">    Sure.\n"
                                   ">\n"
                                   "> 2. Yes, okay.\n"
                                   ">    ![ok](hi \"yes\")\n";

    test_facade_dump(
        runner, markdown,
        "Document scope=1:1..10:20 anchor=null attributes={} children=3\n"
        "├── Heading scope=1:1..1:13 anchor=null attributes={} level=1 children=3\n"
        "│   ├── Text scope=1:3..1:5 anchor=null attributes={} literal=\"Hi \" children=0\n"
        "│   ├── Emphasis scope=1:6..1:12 anchor=null attributes={} children=1\n"
        "│   │   └── Text scope=1:7..1:11 anchor=null attributes={} literal=\"there\" children=0\n"
        "│   └── Text scope=1:13..1:13 anchor=null attributes={} literal=\".\" children=0\n"
        "├── Paragraph scope=3:1..4:42 anchor=null attributes={} children=8\n"
        "│   ├── Text scope=3:1..3:14 anchor=null attributes={} literal=\"Hello “ \" children=0\n"
        "│   ├── Link scope=3:15..3:37 anchor=null attributes={} dest=url(\"http://www.google.com\") "
        "title=null children=1\n"
        "│   │   └── Text scope=3:16..3:36 anchor=null attributes={} literal=\"http://www.google.com\" "
        "children=0\n"
        "│   ├── SoftBreak scope=3:38..3:38 anchor=null attributes={} children=0\n"
        "│   ├── Text scope=4:1..4:6 anchor=null attributes={} literal=\"there \" children=0\n"
        "│   ├── Code scope=4:7..4:10 anchor=null attributes={} literal=\"hi\" children=0\n"
        "│   ├── Text scope=4:11..4:14 anchor=null attributes={} literal=\" -- \" children=0\n"
        "│   ├── Link scope=4:15..4:41 anchor=null attributes={} dest=url(\"www.google.com\") title=\"ok\" "
        "children=1\n"
        "│   │   └── Text scope=4:16..4:19 anchor=null attributes={} literal=\"okay\" children=0\n"
        "│   └── Text scope=4:42..4:42 anchor=null attributes={} literal=\".\" children=0\n"
        "└── Callout scope=6:1..10:20 anchor=null attributes={} variant=null collapsed=null children=1\n"
        "    └── List scope=6:3..10:20 anchor=null attributes={} flavor=ordered start=1 variant=decimal "
        "delimiter=period tight=false children=2\n"
        "        ├── ListItem scope=6:3..8:1 anchor=null attributes={} marker=null children=1\n"
        "        │   └── Paragraph scope=6:6..7:10 anchor=null attributes={} children=3\n"
        "        │       ├── Text scope=6:6..6:10 anchor=null attributes={} literal=\"Okay.\" children=0\n"
        "        │       ├── SoftBreak scope=6:11..6:11 anchor=null attributes={} children=0\n"
        "        │       └── Text scope=7:6..7:10 anchor=null attributes={} literal=\"Sure.\" children=0\n"
        "        └── ListItem scope=9:3..10:20 anchor=null attributes={} marker=null children=1\n"
        "            └── Paragraph scope=9:6..10:20 anchor=null attributes={} children=3\n"
        "                ├── Text scope=9:6..9:15 anchor=null attributes={} literal=\"Yes, okay.\" children=0\n"
        "                ├── SoftBreak scope=9:16..9:16 anchor=null attributes={} children=0\n"
        "                └── Media scope=10:6..10:20 anchor=null attributes={} dest=url(\"hi\") title=\"yes\" "
        "dimensions=null "
        "children=1\n"
        "                    └── Text scope=10:8..10:9 anchor=null attributes={} literal=\"ok\" children=0\n",
        "scopes are as expected");
}

static void source_pos_inlines(test_batch_runner *runner) {
    test_facade_dump(runner,
                     "*first*\n"
                     "second\n",
                     "Document scope=1:1..2:6 anchor=null attributes={} children=1\n"
                     "└── Paragraph scope=1:1..2:6 anchor=null attributes={} children=3\n"
                     "    ├── Emphasis scope=1:1..1:7 anchor=null attributes={} children=1\n"
                     "    │   └── Text scope=1:2..1:6 anchor=null attributes={} literal=\"first\" children=0\n"
                     "    ├── SoftBreak scope=1:8..1:8 anchor=null attributes={} children=0\n"
                     "    └── Text scope=2:1..2:6 anchor=null attributes={} literal=\"second\" children=0\n",
                     "closed emphasis scopes are as expected");
    test_facade_dump(runner,
                     "*first\n"
                     "second*\n",
                     "Document scope=1:1..2:7 anchor=null attributes={} children=1\n"
                     "└── Paragraph scope=1:1..2:7 anchor=null attributes={} children=1\n"
                     "    └── Emphasis scope=1:1..2:7 anchor=null attributes={} children=3\n"
                     "        ├── Text scope=1:2..1:6 anchor=null attributes={} literal=\"first\" children=0\n"
                     "        ├── SoftBreak scope=1:7..1:7 anchor=null attributes={} children=0\n"
                     "        └── Text scope=2:1..2:6 anchor=null attributes={} literal=\"second\" children=0\n",
                     "multiline emphasis scopes are as expected");
}

/* THE CITATION MODEL's values (M4). A defined call is a one-item `Cite`
 * whose `Citation` names the footnote by the normalized label without its
 * caret and carries empty affixes; the document owns every winning or
 * unreferenced definition as a `Footnote` in scope order and no definition
 * remains a child; a losing duplicate is ordinary content in which the
 * leading `[^label]` is a call to the winner; and every value accessor
 * refuses a node that is not its owner and a value that is missing. */
static void citation_and_footnote_values(test_batch_runner *runner) {
    static const char markdown[] = "[^Note] [^b] [c](/inline)\n"
                                   "\n"
                                   "[^note]: first\n"
                                   "\n"
                                   "[^note]: second\n"
                                   "\n"
                                   "[^b]: kept\n"
                                   "\n"
                                   "[^unused]: still here\n";
    static const char *const ids[] = {"note", "note", "b", "unused"};
    markdown_core_document *document;
    const markdown_core_node *root;
    const markdown_core_node *paragraph;
    const markdown_core_node *cite;
    const markdown_core_citation *item;
    const markdown_core_footnote *footnote;
    markdown_core_referent referent;
    markdown_core_scope scope;
    markdown_core_string id;
    size_t count = 0;

    document = markdown_core_document_parse((const uint8_t *)markdown, strlen(markdown), NULL);
    if (!document) {
        OK(runner, 0, "citation corpus parses");
        return;
    }
    root = markdown_core_document_root(document);
    /* Every definition left the tree, the losing duplicate included: one
     * paragraph remains. */
    INT_EQ(runner, (int)markdown_core_node_child_count(root), 1, "no definition remains a child of the document");
    paragraph = markdown_core_node_get_first_child(root);
    cite = markdown_core_node_get_first_child(paragraph);
    INT_EQ(runner, markdown_core_node_get_kind(cite), MARKDOWN_CORE_KIND_CITE, "a defined call is a Cite");
    INT_EQ(runner, (int)markdown_core_node_child_count(cite), 0, "a cite has no children");
    item = markdown_core_node_cite_citations(cite);
    OK(runner, item != NULL, "a cite holds an item");
    OK(runner, item != NULL && markdown_core_citation_next(item) == NULL, "an inherited call holds exactly one item");
    OK(runner, markdown_core_citation_referent(item, &referent), "an item answers its referent");
    INT_EQ(runner, referent.kind, MARKDOWN_CORE_REFERENT_FOOTNOTE, "an inherited call names a footnote");
    OK(runner, referent.id.length == 4 && memcmp(referent.id.data, "note", 4) == 0,
       "the id is the normalized label without the caret");
    OK(runner, referent.key.length == 0 && referent.key.data == NULL && referent.mode == 0,
       "the footnote branch zeroes the bib fields");
    OK(runner, markdown_core_citation_prefix(item) == NULL && markdown_core_citation_suffix(item) == NULL,
       "an inherited call has empty affixes");
    scope = markdown_core_node_scope(cite);
    OK(runner, scope.start.line == 1 && scope.start.column == 1 && scope.end.line == 1 && scope.end.column == 7,
       "the cite covers the brackets");
    scope = markdown_core_citation_scope(item);
    OK(runner, scope.start.line == 1 && scope.start.column == 2 && scope.end.line == 1 && scope.end.column == 6,
       "the item covers the caret and the label");

    OK(runner, markdown_core_node_get_next_sibling(paragraph) == NULL, "the paragraph is the only content");

    for (footnote = markdown_core_node_document_footnotes(root); footnote;
         footnote = markdown_core_footnote_next(footnote)) {
        OK(runner, markdown_core_footnote_id(footnote, &id), "a footnote answers its id");
        OK(runner, count < 4 && id.length == strlen(ids[count]) && memcmp(id.data, ids[count], id.length) == 0,
           "footnote %zu carries the expected id", count);
        count++;
    }
    INT_EQ(runner, (int)count, 4,
           "the winner, its duplicate, the referenced, and the unreferenced definitions are footnotes");
    footnote = markdown_core_node_document_footnotes(root);
    scope = markdown_core_footnote_scope(footnote);
    OK(runner, scope.start.line == 3 && scope.start.column == 1, "the first footnote is the winning definition");
    scope = markdown_core_footnote_scope(markdown_core_footnote_next(footnote));
    OK(runner, scope.start.line == 5 && scope.start.column == 1,
       "a later definition of the same id is the footnote after the winner");
    OK(runner,
       markdown_core_footnote_content(footnote) != NULL &&
           markdown_core_node_get_kind(markdown_core_footnote_content(footnote)) == MARKDOWN_CORE_KIND_PARAGRAPH,
       "a footnote's content is its block content");

    OK(runner,
       markdown_core_node_cite_citations(paragraph) == NULL && markdown_core_node_document_footnotes(paragraph) == NULL,
       "the owner accessors refuse other kinds");
    OK(runner,
       markdown_core_citation_next(NULL) == NULL && markdown_core_citation_prefix(NULL) == NULL &&
           markdown_core_citation_suffix(NULL) == NULL && !markdown_core_citation_referent(NULL, &referent) &&
           !markdown_core_citation_referent(item, NULL),
       "the citation accessors refuse a missing value");
    OK(runner,
       markdown_core_footnote_next(NULL) == NULL && markdown_core_footnote_content(NULL) == NULL &&
           !markdown_core_footnote_id(NULL, &id) && !markdown_core_footnote_id(footnote, NULL),
       "the footnote accessors refuse a missing value");
    markdown_core_document_free(document);
}

static void link_resource_lifecycle(test_batch_runner *runner) {
    /* M2: a link or image reads its destination and title through a resource
     * the parser creates -- one per direct link, image, or autolink, and one
     * per definition, shared by every occurrence that resolves to it. Nothing
     * else writes one: the engine's url and title setters left with the
     * model. A node built by hand, or converted into a link, has no resource
     * and is the link `[a]()` is -- the empty url and no title -- rather than
     * reading another arm's bytes as a resource pointer. */
    markdown_core_node *paragraph = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
    markdown_core_node *link = markdown_core_node_new(MARKDOWN_CORE_NODE_LINK);
    markdown_core_node *image = markdown_core_node_new(MARKDOWN_CORE_NODE_MEDIA);
    markdown_core_node *converted = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
    markdown_core_destination destination;
    markdown_core_optional_string title;

    OK(runner, markdown_core_node_append_child(paragraph, link), "hand-built link joins a paragraph");
    OK(runner, markdown_core_node_append_child(paragraph, image), "hand-built image joins a paragraph");
    OK(runner, markdown_core_node_append_child(paragraph, converted), "text joins a paragraph");

    OK(runner, markdown_core_node_resource(link) == NULL, "a hand-built link reads through no resource");
    OK(runner, markdown_core_node_destination(link, &destination), "the facade answers a hand-built link");
    INT_EQ(runner, destination.kind, MARKDOWN_CORE_DESTINATION_URL, "a hand-built link is the url branch");
    INT_EQ(runner, (int)destination.url.length, 0, "a hand-built link's url is empty");
    OK(runner, markdown_core_node_title(link, &title) && !title.has_value, "a hand-built link's title is absent");
    OK(runner, markdown_core_node_resource(image) == NULL, "a hand-built image reads through no resource");
    OK(runner, markdown_core_node_title(image, &title) && !title.has_value, "a hand-built image's title is absent");

    OK(runner, markdown_core_node_set_literal(converted, "~~"), "the text to convert has a literal");
    INT_EQ(runner, markdown_core_node_set_kind(converted, MARKDOWN_CORE_NODE_LINK), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "set_kind converts text into a link");
    OK(runner, markdown_core_node_resource(converted) == NULL, "a converted link starts without a resource");
    OK(runner, markdown_core_node_destination(converted, &destination) && destination.url.length == 0,
       "a converted link starts with the empty url");
    INT_EQ(runner, markdown_core_node_set_kind(converted, MARKDOWN_CORE_NODE_TEXT), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "set_kind converts the link back");
    OK(runner, !markdown_core_node_destination(converted, &destination), "a text node has no destination");
    STR_EQ(runner, markdown_core_node_get_literal(converted), "", "converting back starts the literal empty");

    markdown_core_node_free(paragraph);

    /* Every occurrence of one definition reads one resource; a direct link
     * with the same bytes owns its own. */
    static const char markdown[] = "[a]: /shared \"t\"\n\n[a] [a] [d](/shared \"t\")\n";
    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1);
    markdown_core_node *first = markdown_core_node_first_child(markdown_core_node_first_child(doc));
    markdown_core_node *second = markdown_core_node_next(markdown_core_node_next(first));
    markdown_core_node *direct = markdown_core_node_next(markdown_core_node_next(second));
    OK(runner,
       markdown_core_node_resource(first) != NULL &&
           markdown_core_node_resource(first) == markdown_core_node_resource(second),
       "two occurrences of one definition read one resource");
    OK(runner,
       markdown_core_node_resource(direct) != NULL &&
           markdown_core_node_resource(direct) != markdown_core_node_resource(first),
       "a direct link with the same bytes owns its own resource");
    markdown_core_node_free(doc);
}

static size_t payload_allocations, payload_fail_at, payload_live;
static void *payload_test_calloc(size_t count, size_t size) {
    if (++payload_allocations == payload_fail_at) {
        return NULL;
    }
    void *pointer = calloc(count, size);
    payload_live += pointer != NULL;
    return pointer;
}
static void *payload_test_realloc(void *pointer, size_t size) {
    if (++payload_allocations == payload_fail_at) {
        return NULL;
    }
    bool new_allocation = pointer == NULL;
    void *result = realloc(pointer, size);
    payload_live += result != NULL && new_allocation;
    return result;
}
static void payload_test_free(void *pointer) {
    payload_live -= pointer != NULL;
    free(pointer);
}
static markdown_core_mem payload_test_mem = {payload_test_calloc, payload_test_realloc, payload_test_free};

typedef struct {
    char prefix;
    long double value;
} payload_float_alignment;
typedef struct {
    char prefix;
    int64_t value;
} payload_integer_alignment;

static void node_payload_lifecycle(test_batch_runner *runner) {
    INT_EQ(runner, sizeof(markdown_core_node_data), sizeof(void *),
           "all payload arms share one pointer-sized node slot");
    static const markdown_core_node_type extra_types[] = {
        MARKDOWN_CORE_NODE_INSERTION,
        MARKDOWN_CORE_NODE_CROSS_EMBEDDED,
        MARKDOWN_CORE_NODE_MARK,
        MARKDOWN_CORE_NODE_CROSS_LINK,
        MARKDOWN_CORE_NODE_CITE,
        MARKDOWN_CORE_NODE_CITATION,
        MARKDOWN_CORE_NODE_FOOTNOTE,
        MARKDOWN_CORE_NODE_SPECIMEN,
        MARKDOWN_CORE_NODE_TABLE,
        MARKDOWN_CORE_NODE_TABLE_ROW,
        MARKDOWN_CORE_NODE_TABLE_CELL,
        MARKDOWN_CORE_NODE_STRIKETHROUGH,
        MARKDOWN_CORE_NODE_FORMULA,
        MARKDOWN_CORE_NODE_FORMULA_BLOCK,
        MARKDOWN_CORE_NODE_DIRECTIVE,
        MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
        MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
    };
    size_t extra_count = sizeof(extra_types) / sizeof(*extra_types);
    for (size_t i = 0; i < (size_t)num_node_types + extra_count; i++) {
        markdown_core_node_type type = i < (size_t)num_node_types ? node_types[i] : extra_types[i - num_node_types];
        payload_allocations = payload_fail_at = 0;
        markdown_core_node *node = markdown_core_node_new_with_mem(type, &payload_test_mem);
        OK(runner, node != NULL, "type %u constructs with its default fields", (unsigned)type);
        size_t total = payload_allocations;
        INT_EQ(runner, total, 1, "node and initial typed record have one allocation for every kind");
        if (node->as.data) {
            OK(runner,
               (uintptr_t)node->as.data % offsetof(payload_float_alignment, value) == 0 &&
                   (uintptr_t)node->as.data % offsetof(payload_integer_alignment, value) == 0,
               "initial typed record has scalar alignment");
        }
        markdown_core_node_free(node);
        INT_EQ(runner, payload_live, 0, "type %u releases every constructor allocation", (unsigned)type);
        for (size_t fail = 1; fail <= total; fail++) {
            payload_allocations = 0;
            payload_fail_at = fail;
            node = markdown_core_node_new_with_mem(type, &payload_test_mem);
            OK(runner, node == NULL, "type %u never publishes a partial payload", (unsigned)type);
            if (node) {
                markdown_core_node_free(node);
            }
            INT_EQ(runner, payload_live, 0, "failed constructor releases all acquired allocations");
        }
    }
    payload_fail_at = 0;
    markdown_core_node *parent = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, &payload_test_mem);
    markdown_core_node *text = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, &payload_test_mem);
    OK(runner, markdown_core_node_append_child(parent, text), "text joins its parent");
    OK(runner, markdown_core_node_set_literal(text, "retained"), "text owns a literal");
    markdown_core_chunk *original_payload = text->as.literal;
    size_t before = payload_live;
    payload_fail_at = payload_allocations + 1;
    INT_EQ(runner, markdown_core_node_set_kind(text, MARKDOWN_CORE_NODE_TEXT), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "setting the current kind preserves its data without allocation");
    INT_EQ(runner, payload_allocations + 1, payload_fail_at, "setting the current kind allocates nothing");
    INT_EQ(runner, markdown_core_node_set_kind(text, MARKDOWN_CORE_NODE_LINK),
           MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED, "conversion reports replacement allocation failure");
    OK(runner,
       text->kind == MARKDOWN_CORE_NODE_TEXT && text->as.literal == original_payload && text->parent == parent &&
           parent->first_child == text,
       "failed kind conversion preserves data, identity, and tree links");
    STR_EQ(runner, markdown_core_node_get_literal(text), "retained", "failed retyping retains owned bytes");
    INT_EQ(runner, payload_live, before, "failed retyping neither frees nor leaks an allocation");
    payload_fail_at = 0;
    INT_EQ(runner, markdown_core_node_set_kind(text, MARKDOWN_CORE_NODE_LINK), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "successful kind conversion installs new defaults");
    OK(runner, text->as.link && !text->as.link->resource, "converted link has a payload and no resource");
    size_t attempts = payload_allocations;
    markdown_core_link *original_link = text->as.link;
    payload_fail_at = attempts + 1;
    INT_EQ(runner, markdown_core_node_set_kind(text, MARKDOWN_CORE_NODE_CODE_BLOCK),
           MARKDOWN_CORE_NODE_SET_KIND_REJECTED, "containment rejects a block in a paragraph");
    INT_EQ(runner, payload_allocations, attempts, "invalid containment allocates nothing");
    OK(runner,
       text->kind == MARKDOWN_CORE_NODE_LINK && text->as.link == original_link && text->parent == parent &&
           parent->first_child == text,
       "containment rejection preserves kind, data, identity, and tree links");
    payload_fail_at = 0;
    OK(runner,
       markdown_core_node_set_kind(text, MARKDOWN_CORE_NODE_STRONG) == MARKDOWN_CORE_NODE_SET_KIND_OK && !text->as.data,
       "a fieldless kind releases the old payload without creating an empty record");

    markdown_core_node *empty = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_EMPHASIS, &payload_test_mem);
    OK(runner, markdown_core_node_append_child(parent, empty), "a fieldless node joins the parent");
    INT_EQ(runner, markdown_core_node_set_kind(empty, MARKDOWN_CORE_NODE_CROSS_LINK), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "a node constructed without fields acquires an owned replacement record");
    markdown_core_destination destination;
    markdown_core_optional_string label;
    label = markdown_core_node_cross_label(empty);
    OK(runner,
       markdown_core_node_destination(empty, &destination) && destination.path.length == 0 &&
           !destination.anchor.has_value && !label.has_value && markdown_core_node_dimensions(empty) == NULL,
       "converted cross link establishes ordinary empty and absent defaults");

    markdown_core_node *cite = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_CITE, &payload_test_mem);
    markdown_core_node *item = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_CITATION, &payload_test_mem);
    markdown_core_node *prefix = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, &payload_test_mem);
    OK(runner, markdown_core_node_append_child(parent, cite), "cite joins its parent");
    cite->as.cite->citations = item;
    item->as.citation->prefix = prefix;
    OK(runner, markdown_core_node_set_literal(prefix, "prefix"), "citation owns an affix subtree");
    payload_fail_at = payload_allocations + 1;
    before = payload_live;
    OK(runner,
       markdown_core_node_set_kind(cite, MARKDOWN_CORE_NODE_TEXT) == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED &&
           cite->as.cite->citations == item && item->as.citation->prefix == prefix,
       "failed retyping preserves node-valued fields");
    INT_EQ(runner, payload_live, before, "failed retyping leaves the owned subtree alive");
    payload_fail_at = 0;
    INT_EQ(runner, markdown_core_node_set_kind(cite, MARKDOWN_CORE_NODE_TEXT), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "kind conversion releases the old owned subtrees");
    markdown_core_node_free(parent);
    INT_EQ(runner, payload_live, 0, "conversion and destruction release payloads, fields, and affixes exactly once");
}

typedef struct {
    markdown_core_node_type rejected_kind;
    size_t rejections;
} conversion_policy;

static int conversion_can_contain(const markdown_core_extension *extension, markdown_core_node *node,
                                  markdown_core_node_type child_kind) {
    conversion_policy *policy = node->user_data;
    (void)extension;
    if (child_kind == policy->rejected_kind) {
        policy->rejections++;
        return false;
    }
    return node->kind == MARKDOWN_CORE_NODE_DOCUMENT
               ? MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_kind) && child_kind != MARKDOWN_CORE_NODE_LIST_ITEM
               : node->kind == MARKDOWN_CORE_NODE_PARAGRAPH && MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_kind);
}

/* A literal ! lets this probe set the parent policy before the following
 * delimiter, with every production extension still in its fixed order. */
static markdown_core_node *conversion_match_inline(const markdown_core_extension *extension,
                                                   markdown_core_parser *parser, markdown_core_node *parent,
                                                   unsigned char character,
                                                   markdown_core_inline_parser *inline_parser) {
    (void)character;
    (void)inline_parser;
    parent->extension = extension;
    parent->user_data = parser->root->user_data;
    return NULL;
}

static const markdown_core_extension CONVERSION_POLICY = {
    .name = "conversion-policy",
    .can_contain_func = conversion_can_contain,
    .match_inline = conversion_match_inline,
    .dispatch = "!",
};

static bool configure_conversion_policy(markdown_core_parser *parser, void *context) {
    parser->root->extension = &CONVERSION_POLICY;
    parser->root->user_data = context;
    return markdown_core_parser_attach_extension(parser, &CONVERSION_POLICY);
}

static void kind_conversion_containment(test_batch_runner *runner) {
    static const struct {
        markdown_core_node_type rejected_kind, retained_kind;
        const char *source, *retained_literal;
    } cases[] = {
        {MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_PARAGRAPH, "| h |\n| - |\n", "| h |\n| - |"},
        {MARKDOWN_CORE_NODE_HEADING, MARKDOWN_CORE_NODE_PARAGRAPH, "heading\n===\n", "heading\n==="},
        {MARKDOWN_CORE_NODE_COMMENT_BLOCK, MARKDOWN_CORE_NODE_HTML_BLOCK, "<!-- body -->\n", "<!-- body -->\n"},
        {MARKDOWN_CORE_NODE_STRIKETHROUGH, MARKDOWN_CORE_NODE_PARAGRAPH, "!~~text~~\n", "!~~text~~"},
    };
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        conversion_policy policy = {cases[i].rejected_kind, 0};
        markdown_core_node *root = markdown_core_parse_document_with_mem(cases[i].source, strlen(cases[i].source), mem,
                                                                         configure_conversion_policy, &policy);
        OK(runner, policy.rejections > 0, "case %zu exercises parent containment rejection", i);
        OK(runner, root != NULL, "case %zu declines conversion without failing the parse", i);
        if (!root) {
            continue;
        }
        markdown_core_node *block = root->first_child;
        OK(runner, block && block->kind == cases[i].retained_kind && !block->next,
           "case %zu retains the original block", i);
        markdown_core_strbuf literal = MARKDOWN_CORE_BUF_INIT(mem);
        if (block && block->kind == MARKDOWN_CORE_NODE_HTML_BLOCK) {
            markdown_core_strbuf_puts(&literal, markdown_core_node_get_literal(block));
        } else if (block) {
            for (markdown_core_node *child = block->first_child; child; child = child->next) {
                OK(runner, child->kind == MARKDOWN_CORE_NODE_TEXT || child->kind == MARKDOWN_CORE_NODE_SOFT_BREAK,
                   "case %zu retains text and line breaks", i);
                if (child->kind == MARKDOWN_CORE_NODE_TEXT) {
                    markdown_core_strbuf_puts(&literal, markdown_core_node_get_literal(child));
                } else if (child->kind == MARKDOWN_CORE_NODE_SOFT_BREAK) {
                    markdown_core_strbuf_putc(&literal, '\n');
                }
            }
        }
        STR_EQ(runner, (char *)literal.ptr, cases[i].retained_literal, "case %zu preserves authored content", i);
        markdown_core_strbuf_free(&literal);
        markdown_core_node_free(root);
    }
}

static void *marker_to_free;
static int marker_free_count;
static void marker_test_free(void *pointer) {
    if (pointer == marker_to_free && pointer != NULL) {
        marker_free_count++;
        marker_to_free = NULL;
    }
    free(pointer);
}
static markdown_core_mem marker_test_mem = {calloc, realloc, marker_test_free};

/* A byte offset and an indentation column are different coordinates. Splitting
 * byte advances anywhere, including inside a scalar, must compose identically;
 * a column advance can split a tab but must finish a whole scalar. */
static void block_cursor_coordinates(test_batch_runner *runner) {
    const char *markers[] = {"?", "é", "✓", "🚀", "́"};
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        char input[32];
        int length = snprintf(input, sizeof(input), "%s\t%s\t%sX", markers[i], markers[i], markers[i]);
        int width = (int)strlen(markers[i]);
        for (int split = 0; split <= length; split++) {
            markdown_core_parser cursor = {0};
            markdown_core_parser_advance_offset(&cursor, input, split, false);
            markdown_core_parser_advance_offset(&cursor, input, length - split, false);
            OK(runner, cursor.offset == length && cursor.column == 10 && !cursor.partially_consumed_tab,
               "byte advances compose across scalar and tab boundaries (%s, %d)", markers[i], split);
        }
        markdown_core_parser cursor = {0};
        markdown_core_parser_advance_offset(&cursor, input, 1, true);
        OK(runner, cursor.offset == width && cursor.column == 1, "one column consumes one whole scalar");
        markdown_core_parser_advance_offset(&cursor, input, 2, true);
        OK(runner, cursor.offset == width && cursor.column == 3 && cursor.partially_consumed_tab,
           "a tab after any scalar expands from its virtual column");
        markdown_core_parser_advance_offset(&cursor, input, 7, true);
        OK(runner, cursor.offset == length && cursor.column == 10 && !cursor.partially_consumed_tab,
           "resuming a partial tab preserves later scalar and tab coordinates");
    }
}

/* All block facts except the authored marker and byte coordinates must be
 * invariant under replacing a single-scalar marker with another UTF-8 width.
 * The generated trees have bounded depth and contain only lists and leaves. */
static bool task_block_facts_equal(markdown_core_node *a, markdown_core_node *b) {
    if (!a || !b) {
        return a == b;
    }
    if (a->kind != b->kind) {
        return false;
    }
    const char *a_literal = markdown_core_node_get_literal(a);
    const char *b_literal = markdown_core_node_get_literal(b);
    if ((a_literal == NULL) != (b_literal == NULL) || (a_literal && strcmp(a_literal, b_literal) != 0)) {
        return false;
    }
    if (a->kind == MARKDOWN_CORE_NODE_LIST &&
        (a->as.list->list_type != b->as.list->list_type || a->as.list->start != b->as.list->start ||
         a->as.list->delimiter != b->as.list->delimiter || a->as.list->tight != b->as.list->tight)) {
        return false;
    }
    return task_block_facts_equal(a->first_child, b->first_child) && task_block_facts_equal(a->next, b->next);
}

static void task_marker_tab_structure(test_batch_runner *runner) {
    const char *markers[] = {"é", "✓", "🚀", "́"};
    const char *separators[] = {" ", "\t", " \t", "\v", "\f", " \t\v\f"};
    const char *padding[] = {" ", "\t", " \t", "\t ", " \t ", "\t\t"};
    const char *lists[] = {"-", "1.", "1)"};
    for (int indent = 0; indent < 4; indent++) {
        for (size_t sep = 0; sep < sizeof(separators) / sizeof(separators[0]); sep++) {
            for (size_t pad = 0; pad < sizeof(padding) / sizeof(padding[0]); pad++) {
                for (size_t list = 0; list < sizeof(lists) / sizeof(lists[0]); list++) {
                    char source[128];
                    int length = snprintf(source, sizeof(source), "%*s- [?]%s%s%schild\n", indent, "", separators[sep],
                                          lists[list], padding[pad]);
                    markdown_core_node *baseline = markdown_core_parse_document(source, length);
                    OK(runner, baseline != NULL, "ASCII task baseline parses");
                    for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); m++) {
                        length = snprintf(source, sizeof(source), "%*s- [%s]%s%s%schild\n", indent, "", markers[m],
                                          separators[sep], lists[list], padding[pad]);
                        markdown_core_node *actual = markdown_core_parse_document(source, length);
                        OK(runner, baseline && actual && task_block_facts_equal(baseline, actual),
                           "task marker width cannot change block facts (indent=%d sep=%zu pad=%zu list=%zu marker=%s)",
                           indent, sep, pad, list, markers[m]);
                        markdown_core_node_free(actual);
                    }
                    markdown_core_node_free(baseline);
                }
            }
        }
    }
}

static void task_marker_ownership(test_batch_runner *runner) {
    char source[] = "- [ ] open\n- [X] done\n- ordinary\n- [🚀] custom\n";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    OK(runner, document != NULL, "task marker ownership document parses");
    if (!document) {
        return;
    }
    memset(source, '?', sizeof(source) - 1);
    markdown_core_node *item = markdown_core_node_first_child(markdown_core_node_first_child(document->root));
    markdown_core_optional_string marker;
    markdown_core_node_list_item_marker(item, &marker);
    OK(runner, marker.has_value && marker.value.length == 1 && marker.value.data[0] == ' ',
       "incomplete marker survives input reuse");
    OK(runner, item->as.list->task_marker.value.alloc, "parsed task owns its marker bytes");
    markdown_core_node_list_item_marker(item->next, &marker);
    OK(runner, marker.has_value && marker.value.length == 1 && marker.value.data[0] == 'X',
       "completed marker retains authored case");
    markdown_core_node_list_item_marker(item->next->next, &marker);
    OK(runner, !marker.has_value && marker.value.data == NULL && marker.value.length == 0,
       "ordinary item has an absent marker");

    markdown_core_node_list_item_marker(item->next->next->next, &marker);
    OK(runner, marker.has_value && marker.value.length == 4 && memcmp(marker.value.data, "🚀", 4) == 0,
       "parsed custom marker survives input reuse with its complete UTF-8 spelling");
    uint8_t *dump = NULL;
    size_t length = 0;
    OK(runner, markdown_core_document_dump(document, &dump, &length, NULL), "UTF-8 marker document dumps");
    OK(runner, dump && strstr((const char *)dump, "marker=\"🚀\""), "dump preserves UTF-8 marker spelling");
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);

    item = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_LIST_ITEM, &marker_test_mem);
    markdown_core_chunk bytes = markdown_core_chunk_literal("🚀");
    OK(runner, markdown_core_chunk_to_cstr(&marker_test_mem, &bytes) != NULL, "custom marker allocates");
    item->as.list->task_marker = markdown_core_optional_chunk_present(bytes);
    marker_to_free = bytes.data;
    marker_free_count = 0;
    markdown_core_node_free(item);
    INT_EQ(runner, marker_free_count, 1, "destroying an item frees its owned marker exactly once");
}

/* Specimen syntax lands with P9b. Build its reserved native values directly
 * to test the shared document ownership and failure boundary independently. */
static void specimen_values(test_batch_runner *runner) {
    markdown_core_node *root = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_DOCUMENT, &marker_test_mem);
    markdown_core_node *first = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_SPECIMEN, &marker_test_mem);
    markdown_core_node *anonymous = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_SPECIMEN, &marker_test_mem);
    markdown_core_node *body = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, &marker_test_mem);
    markdown_core_node *footnote = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_FOOTNOTE, &marker_test_mem);
    markdown_core_chunk bytes = markdown_core_chunk_literal("étude");
    markdown_core_optional_string id;
    markdown_core_optional_i64 start;
    OK(runner, markdown_core_chunk_to_cstr(&marker_test_mem, &bytes) != NULL, "specimen id allocates");
    first->as.specimen->id = markdown_core_optional_chunk_present(bytes);
    first->as.specimen->start = 5;
    first->as.specimen->has_start = true;
    first->next = anonymous;
    anonymous->prev = first;
    root->as.document->specimens = first;
    root->as.document->footnotes = footnote;
    OK(runner, markdown_core_node_append_child(first, body), "specimen owns block content");
    const markdown_core_specimen *value = markdown_core_node_document_specimens(root);
    OK(runner, markdown_core_specimen_properties(value, &id, &start), "specimen answers properties");
    OK(runner, id.has_value && id.value.length == 6 && memcmp(id.value.data, "étude", 6) == 0,
       "specimen label retains owned UTF-8 bytes");
    OK(runner, start.has_value && start.value == 5, "specimen retains effective reset");
    OK(runner, markdown_core_specimen_content(value) == body, "specimen retains its content relation");
    value = markdown_core_specimen_next(value);
    OK(runner, value && markdown_core_specimen_properties(value, &id, &start), "anonymous definition remains present");
    OK(runner, !id.has_value && !start.has_value, "anonymous label and absent reset remain absent");
    INT_EQ(runner, markdown_core_node_child_count(root), 0, "definitions are not document children");
    OK(runner,
       !markdown_core_specimen_properties(NULL, &id, &start) &&
           !markdown_core_specimen_properties(value, NULL, &start) &&
           !markdown_core_specimen_properties(value, &id, NULL) && markdown_core_node_document_specimens(body) == NULL,
       "typed accessors reject missing values and incorrect owners");
    markdown_core_node *citation = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_CITATION, &marker_test_mem);
    citation->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_SPECIMEN;
    OK(runner, markdown_core_chunk_set_cstr(&marker_test_mem, &citation->as.citation->value, "étude"),
       "specimen reference owns its label");
    markdown_core_referent referent;
    OK(runner,
       markdown_core_citation_referent((const markdown_core_citation *)citation, &referent) &&
           referent.kind == MARKDOWN_CORE_REFERENT_SPECIMEN && referent.id.length == 6 && referent.key.data == NULL &&
           referent.mode == 0,
       "specimen reference uses only its own branch fields");
    citation->as.citation->referent = 99;
    OK(runner, !markdown_core_citation_referent((const markdown_core_citation *)citation, &referent),
       "unknown referent cannot masquerade as a specimen");
    markdown_core_node_free(citation);
    markdown_core_document document = {0};
    document.root = root;
    uint8_t *dump = NULL;
    size_t length = 0;
    OK(runner, markdown_core_document_dump(&document, &dump, &length, NULL), "specimen document dumps");
    OK(runner,
       dump && strstr((const char *)dump, "id=\"étude\" start=5 children=1") &&
           strstr((const char *)dump, "id=null start=null children=0"),
       "dump carries only authored definition facts");
    markdown_core_dump_free(dump);
    marker_to_free = bytes.data;
    marker_free_count = 0;
    markdown_core_node_free(root);
    INT_EQ(runner, marker_free_count, 1, "document frees its owned specimen label exactly once");
}

static void set_kind_keeps_extension_data_beside_the_arm(test_batch_runner *runner) {
    /* Converting a formula to a link installs default link data and preserves
     * the extension's opaque data. Destruction releases both exactly once. */
    static const char markdown[] = "$x$ tail\n";
    markdown_core_error *error = NULL;
    markdown_core_document *document =
        markdown_core_document_parse((const uint8_t *)markdown, sizeof(markdown) - 1, &error);
    markdown_core_node *formula;
    markdown_core_destination destination;

    OK(runner, document != NULL && error == NULL, "the formula document parses");
    formula = markdown_core_node_first_child(markdown_core_node_first_child(document->root));
    INT_EQ(runner, markdown_core_node_get_kind(formula), MARKDOWN_CORE_KIND_FORMULA,
           "the paragraph opens with a formula");
    OK(runner, formula->opaque != NULL, "the formula's extension owns per-node data");
    INT_EQ(runner, markdown_core_node_set_kind(formula, MARKDOWN_CORE_NODE_LINK), MARKDOWN_CORE_NODE_SET_KIND_OK,
           "set_kind converts the formula into a link");
    OK(runner, formula->opaque != NULL, "the extension's data stays with the node");
    OK(runner, markdown_core_node_resource(formula) == NULL, "the converted link starts without a resource");
    OK(runner, markdown_core_node_destination(formula, &destination) && destination.url.length == 0,
       "the converted link answers the empty url");
    markdown_core_document_free(document);
}

static void ref_source_pos(test_batch_runner *runner) {
    static const char markdown[] = "Let's try [reference] links.\n"
                                   "\n"
                                   "[reference]: https://github.com (GitHub)\n";

    /* M2: the occurrence is the Link it names, with its own scope and the
     * definition's destination and title; the definition produces no node. */
    test_facade_dump(runner, markdown,
                     "Document scope=1:1..3:40 anchor=null attributes={} children=1\n"
                     "└── Paragraph scope=1:1..1:28 anchor=null attributes={} children=3\n"
                     "    ├── Text scope=1:1..1:10 anchor=null attributes={} literal=\"Let's try \" children=0\n"
                     "    ├── Link scope=1:11..1:21 anchor=null attributes={} dest=url(\"https://github.com\") "
                     "title=\"GitHub\" children=1\n"
                     "    │   └── Text scope=1:12..1:20 anchor=null attributes={} literal=\"reference\" children=0\n"
                     "    └── Text scope=1:22..1:28 anchor=null attributes={} literal=\" links.\" children=0\n",
                     "reference link scopes are as expected");
}

static void autolink_source_pos(test_batch_runner *runner) {
    test_facade_dump(
        runner, "See www.example.com.\n",
        "Document scope=1:1..1:20 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:20 anchor=null attributes={} children=3\n"
        "    ├── Text scope=1:1..1:4 anchor=null attributes={} literal=\"See \" children=0\n"
        "    ├── Link scope=1:5..1:19 anchor=null attributes={} dest=url(\"http://www.example.com\") "
        "title=null children=1\n"
        "    │   └── Text scope=1:5..1:19 anchor=null attributes={} literal=\"www.example.com\" children=0\n"
        "    └── Text scope=1:20..1:20 anchor=null attributes={} literal=\".\" children=0\n",
        "www autolink scopes are as expected");
    test_facade_dump(
        runner, "See http://example.com.\n",
        "Document scope=1:1..1:23 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:23 anchor=null attributes={} children=3\n"
        "    ├── Text scope=1:1..1:4 anchor=null attributes={} literal=\"See \" children=0\n"
        "    ├── Link scope=1:5..1:22 anchor=null attributes={} dest=url(\"http://example.com\") title=null "
        "children=1\n"
        "    │   └── Text scope=1:5..1:22 anchor=null attributes={} literal=\"http://example.com\" children=0\n"
        "    └── Text scope=1:23..1:23 anchor=null attributes={} literal=\".\" children=0\n",
        "scheme autolink scopes are as expected");
    /* An autolink at column one leaves NO prefix. This assertion used to pin the
     * defect -- it asserted a `Text scope=0:0..0:0 anchor=null attributes={} literal=""` as expected
     * output, a child with no bytes and no position, and a paragraph that said
     * it had two children when it had one thing in it. 0a.14 removes the node;
     * unpinning the assertion is the fix, the same shape as D10's
     * `regression.txt` example 24 at 0a.2. */
    test_facade_dump(
        runner, "http://example.com\n",
        "Document scope=1:1..1:18 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:18 anchor=null attributes={} children=1\n"
        "    └── Link scope=1:1..1:18 anchor=null attributes={} dest=url(\"http://example.com\") title=null "
        "children=1\n"
        "        └── Text scope=1:1..1:18 anchor=null attributes={} literal=\"http://example.com\" children=0\n",
        "scheme autolink at column one scopes are as expected");
    test_facade_dump(
        runner, "Mail user@example.com now.\n",
        "Document scope=1:1..1:26 anchor=null attributes={} children=1\n"
        "└── Paragraph scope=1:1..1:26 anchor=null attributes={} children=3\n"
        "    ├── Text scope=1:1..1:5 anchor=null attributes={} literal=\"Mail \" children=0\n"
        "    ├── Link scope=1:6..1:21 anchor=null attributes={} dest=url(\"mailto:user@example.com\") "
        "title=null children=1\n"
        "    │   └── Text scope=1:6..1:21 anchor=null attributes={} literal=\"user@example.com\" children=0\n"
        "    └── Text scope=1:22..1:26 anchor=null attributes={} literal=\" now.\" children=0\n",
        "email autolink scopes are as expected");
}

static void table_values(test_batch_runner *runner) {
    const char source[] = "| a | b |\n| - | - |\n| c | d |\n| e | f |\n";
    markdown_core_node *root = markdown_core_parse_document(source, sizeof(source) - 1);
    OK(runner, root != NULL, "table value fixture parses");
    if (!root) {
        return;
    }
    markdown_core_node *table = root->first_child;
    markdown_core_table *properties = (markdown_core_table *)table->opaque;
    properties->content_count = 1;
    properties->foot_count = 1;
    for (markdown_core_node *row = table->first_child; row; row = row->next) {
        markdown_core_node *cell = row->first_child;
        markdown_core_node_free(cell->next);
        markdown_core_node_free(cell->first_child);
        cell->as.table_cell->colspan = 2;
        markdown_core_node *block = markdown_core_node_new(MARKDOWN_CORE_NODE_PARAGRAPH);
        OK(runner, markdown_core_node_append_child(cell, block), "cell owns block content directly");
        int64_t rowspan, colspan;
        OK(runner, markdown_core_node_table_cell_spans(cell, &rowspan, &colspan) && rowspan == 1 && colspan == 2,
           "facade retains non-unit spans");
    }
    size_t columns, head, content, foot;
    OK(runner,
       markdown_core_node_table_properties(table, &columns, &head, &content, &foot) && columns == 2 && head == 1 &&
           content == 1 && foot == 1,
       "all three row groups retain independent counts");
    static const struct {
        double width;
        const char *dump;
    } widths[] = {{0.1, "0.1"},
                  {1e-6, "0.000001"},
                  {1e-7, "1e-7"},
                  {1e20, "100000000000000000000"},
                  {1e21, "1e+21"},
                  {5e-324, "5e-324"},
                  {1.2345678901234567, "1.2345678901234567"}};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        properties->columns[0].relative = (markdown_core_optional_double){true, widths[i].width};
        markdown_core_table_column column;
        OK(runner,
           markdown_core_node_table_column_at(table, 0, &column) && column.relative.has_value &&
               column.relative.value == widths[i].width,
           "column width is an authored double");
        markdown_core_document document = {0};
        document.root = root;
        uint8_t *dump = NULL;
        size_t length = 0;
        char expected[128];
        snprintf(expected, sizeof(expected), "columns=[none:%s,none:null] children=3", widths[i].dump);
        OK(runner, markdown_core_document_dump(&document, &dump, &length, NULL), "table values dump");
        OK(runner,
           dump && strstr((const char *)dump, expected) && strstr((const char *)dump, "TableFoot children=1") &&
               strstr((const char *)dump, "rowspan=1 colspan=2"),
           "dump retains groups, spans, and canonical relative-width digits");
        markdown_core_dump_free(dump);
    }
    markdown_core_node_free(root);
}

/* Many contractions followed by many address splits must retain one linear
 * source map, rather than copying every unconsumed suffix for each link. */
static int observed_source_marks;
static markdown_core_node *observe_source_marks(const markdown_core_extension *extension, markdown_core_parser *parser,
                                                markdown_core_node *root) {
    (void)extension;
    observed_source_marks = parser->line_marks_size;
    return root;
}

static void table_source_map_growth(test_batch_runner *runner) {
    static const markdown_core_extension observer = {.postprocess_func = observe_source_marks};
    const markdown_core_extension *extensions[] = {&observer};
    const char *unit = "\\| &amp; user@example.com ";
    size_t unit_length = strlen(unit);
    for (size_t count = 256; count <= 4096; count *= 4) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(markdown_core_get_default_mem_allocator());
        markdown_core_strbuf_puts(&source, "| h |\n| - |\n| ");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, unit);
        }
        markdown_core_strbuf_puts(&source, "|\n");
        observed_source_marks = 0;
        markdown_core_node *root = parse_with_probes((const char *)source.ptr, source.size, extensions, 1);
        OK(runner, root != NULL, "mapped table with %zu address splits parses", count);
        if (root) {
            // Every source byte may contribute only a bounded number of runs,
            // even as the number of links and remaining runs both increase.
            OK(runner, observed_source_marks > 0 && (size_t)observed_source_marks <= 16 * count + 16,
               "source map storage is linear at %zu repeats (%d runs)", count, observed_source_marks);
            markdown_core_node *table = root->first_child;
            markdown_core_node *cell = table->first_child->next->first_child;
            size_t links = 0;
            for (markdown_core_node *node = cell->first_child; node; node = node->next) {
                if (node->kind != MARKDOWN_CORE_NODE_LINK) {
                    continue;
                }
                INT_EQ(runner, node->start_column, 12 + links * unit_length, "address begins at its authored byte");
                INT_EQ(runner, node->end_column, 27 + links * unit_length, "address ends at its authored byte");
                links++;
            }
            INT_EQ(runner, links, count, "every address is retained");
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

static size_t metadata_populated_count(const markdown_core_metadata *metadata) {
    return (markdown_core_metadata_name(metadata) != NULL) + (markdown_core_metadata_title(metadata) != NULL) +
           (markdown_core_metadata_subtitle(metadata) != NULL) + (markdown_core_metadata_time(metadata) != NULL) +
           (markdown_core_metadata_date(metadata) != NULL) + (markdown_core_metadata_authors(metadata) != NULL) +
           (markdown_core_metadata_keywords(metadata) != NULL) + (markdown_core_metadata_abstract(metadata) != NULL) +
           (markdown_core_metadata_state(metadata) != NULL) + (markdown_core_metadata_comment(metadata) != NULL);
}

static void properties_values(test_batch_runner *runner) {
    const char *source = "---\n# ignored\nname: \"Note\"\nunknown: x\nnot YAML\n...\n"
                         "time: 9007199254740993\ndate: 2026-09-08\nauthors: [Ada, 2]\n"
                         "keywords: []\nabstract: |\n  one\n\n  two\nstate: true\ncomment:\n"
                         "name: duplicate\ntitle: A title\nsubtitle: A subtitle\n---\nbody\n";
    char *input = malloc(strlen(source) + 1);
    strcpy(input, source);
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)input, strlen(input), NULL);
    memset(input, 0, strlen(input));
    free(input);
    OK(runner, document != NULL, "fixed metadata fields parse independently of source storage");
    if (!document) {
        return;
    }
    const markdown_core_node *root = markdown_core_document_root(document);
    const markdown_core_metadata *metadata = markdown_core_node_document_metadata(root);
    INT_EQ(runner, metadata_populated_count(metadata), 10, "all ten named fields are present");
    markdown_core_metadata_scalar scalar;
    const markdown_core_metadata_value *record = markdown_core_metadata_time(metadata);
    OK(runner,
       markdown_core_metadata_value_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_NUMBER &&
           scalar.value.string.length == 16 && !memcmp(scalar.value.string.data, "9007199254740993", 16),
       "exact numbers own their spelling without alias expansion");
    record = markdown_core_metadata_abstract(metadata);
    OK(runner,
       markdown_core_metadata_value_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_TEXT &&
           scalar.value.string.length == 9 && !memcmp(scalar.value.string.data, "one\n\ntwo\n", 9),
       "literal prose keeps internal newlines and a clipped final newline");
    INT_EQ(runner, markdown_core_metadata_scope(metadata).end.line, 20, "metadata ends at the closing fence");
    INT_EQ(runner, markdown_core_node_scope(markdown_core_node_get_first_child(root)).start.line, 21,
           "body stays outside metadata");
    OK(runner, !markdown_core_metadata_name(NULL), "absent metadata has no field");
    markdown_core_document_free(document);
}

static void properties_source_boundaries(test_batch_runner *runner) {
    const char *prefixes[] = {"", "\t", "  ", " \t"};
    const char *endings[] = {"\n", "\r", "\r\n"};
    for (size_t ending = 0; ending < 3; ending++) {
        for (size_t prefix = 0; prefix < 4; prefix++) {
            for (size_t indent = 0; indent <= 2; indent += 2) {
                char original[256];
                snprintf(original, sizeof(original),
                         "---\nauthors:\n%*s- Ada\n%s# note\n%*s- Lin\n"
                         "abstract: | # header\n  # prose\n\n    name: inside\n\ncomment: |\n  done\nstate: "
                         "ready\n---\nbody\n",
                         (int)indent, "", prefixes[prefix], (int)indent, "");
                markdown_core_strbuf input = MARKDOWN_CORE_BUF_INIT(markdown_core_get_default_mem_allocator());
                for (const char *c = original; *c; c++) {
                    if (*c == '\n') {
                        markdown_core_strbuf_puts(&input, endings[ending]);
                    } else {
                        markdown_core_strbuf_putc(&input, *c);
                    }
                }
                markdown_core_document *doc = markdown_core_document_parse(input.ptr, input.size, NULL);
                const markdown_core_metadata *metadata =
                    markdown_core_node_document_metadata(markdown_core_document_root(doc));
                INT_EQ(runner, metadata_populated_count(metadata), 4,
                       "comments never split a flat list or become records");
                const markdown_core_metadata_value *record = markdown_core_metadata_authors(metadata);
                INT_EQ(runner, markdown_core_metadata_value_item_count(record), 2,
                       "both list entries survive every separation form");
                record = markdown_core_metadata_abstract(metadata);
                markdown_core_metadata_scalar scalar;
                const char *expected = "# prose\n\n  name: inside\n";
                OK(runner,
                   markdown_core_metadata_value_scalar(record, &scalar) && scalar.kind == MARKDOWN_CORE_METADATA_TEXT &&
                       scalar.value.string.length == strlen(expected) &&
                       !memcmp(scalar.value.string.data, expected, strlen(expected)),
                   "literal indentation, hashes and line endings are text, not nested fields");
                markdown_core_document_free(doc);
                markdown_core_strbuf_free(&input);
            }
        }
    }
    const char *invalid[] = {"name: &a value",        "name: *a",
                             "name: !!str 1",         "name: |\n  text",
                             "abstract: >\n  text",   "abstract: |-\n  text",
                             "abstract: |2\n  text",  "abstract: |\n    a\n  b",
                             "name: 'one\n  two'",    "name: one\n  two",
                             "authors: [true]",       "authors: [[one]]",
                             "name: {nested: value}", "Name: uppercase",
                             "unknown: ignored",      "\"name\\n\": bad",
                             "name: \"bad\\x41\""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        char source[256];
        snprintf(source, sizeof(source), "---\n%s\nname: kept\n---\n", invalid[i]);
        markdown_core_document *doc = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
        const markdown_core_metadata *metadata = markdown_core_node_document_metadata(markdown_core_document_root(doc));
        const markdown_core_metadata_value *record = markdown_core_metadata_name(metadata);
        markdown_core_metadata_scalar value;
        OK(runner,
           metadata_populated_count(metadata) == 1 && markdown_core_metadata_value_scalar(record, &value) &&
               value.kind == MARKDOWN_CORE_METADATA_TEXT && value.value.string.length == 4 &&
               !memcmp(value.value.string.data, "kept", 4),
           "unsupported member %zu is ignored without consuming the next valid field or reserving its name", i);
        markdown_core_document_free(doc);
    }
    const char *dash_text = "---\nauthors: - Ada\n- ignored\nauthors: [Lin]\nkeywords: - language # note\n"
                            "title: -\n---\nbody\n";
    markdown_core_document *dash_doc =
        markdown_core_document_parse((const uint8_t *)dash_text, strlen(dash_text), NULL);
    const markdown_core_metadata *dash_metadata =
        markdown_core_node_document_metadata(markdown_core_document_root(dash_doc));
    const markdown_core_metadata_value *dash_values[] = {markdown_core_metadata_authors(dash_metadata),
                                                         markdown_core_metadata_keywords(dash_metadata),
                                                         markdown_core_metadata_title(dash_metadata)};
    const char *dash_expected[] = {"- Ada", "- language", "-"};
    INT_EQ(runner, metadata_populated_count(dash_metadata), 3, "field-line dashes assign text fields once");
    for (size_t i = 0; i < sizeof(dash_values) / sizeof(*dash_values); i++) {
        markdown_core_metadata_scalar value;
        OK(runner,
           markdown_core_metadata_value_scalar(dash_values[i], &value) && value.kind == MARKDOWN_CORE_METADATA_TEXT &&
               value.value.string.length == strlen(dash_expected[i]) &&
               !memcmp(value.value.string.data, dash_expected[i], strlen(dash_expected[i])),
           "a field-line dash stays text, distinct from a following-line list marker");
    }
    markdown_core_document_free(dash_doc);
    const struct {
        const char *source;
        bool closed;
    } bracketed[] = {{"authors: [\nstate: true\n]\n", true},
                     {"authors: [\n# ]\nstate: true\n]\n", true},
                     {"authors: [\n\"state: true ]\",\n[one]\n]\n", true},
                     {"authors: [\n{\nstate: true\n}\n]\n", true},
                     {"unknown: [\nstate: true\n]\n", true},
                     {"{\nstate: true\n}\n", true},
                     {"authors: [\nstate: true\n", false},
                     {"authors: [\n}\nstate: true\n", false},
                     {"{\n]\nstate: true\n", false},
                     {"authors: [\n\"unterminated\n]\nstate: false\n", false},
                     {"unknown: [\n...\nstate: true\n", false},
                     {"{\nstate: true\n", false}};
    for (size_t i = 0; i < sizeof(bracketed) / sizeof(*bracketed); i++) {
        char source[256];
        snprintf(source, sizeof(source), "---\nname: kept\n%sstate: ready\n---\nbody\n", bracketed[i].source);
        markdown_core_document *doc = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
        const markdown_core_node *root = markdown_core_document_root(doc);
        const markdown_core_metadata *metadata = markdown_core_node_document_metadata(root);
        const markdown_core_metadata_value *state = markdown_core_metadata_state(metadata);
        markdown_core_metadata_scalar value;
        INT_EQ(runner, metadata_populated_count(metadata), bracketed[i].closed ? 2 : 1,
               "bracketed member %zu keeps its interior opaque", i);
        OK(runner,
           bracketed[i].closed
               ? markdown_core_metadata_value_scalar(state, &value) && value.kind == MARKDOWN_CORE_METADATA_TEXT &&
                     value.value.string.length == 5 && !memcmp(value.value.string.data, "ready", 5)
               : state == NULL,
           "only a closed bracketed member allows the next independent field");
        INT_EQ(runner, markdown_core_node_scope(markdown_core_node_get_first_child(root)).start.line,
               markdown_core_metadata_scope(metadata).end.line + 1, "the closing fence always separates the body");
        markdown_core_document_free(doc);
    }
    const char *source =
        "---\n{\"name\":\"ignored\"}\nname: \"\\uD83D\\uDE80\"\nauthors: [Ada, 2]\nstate: false\n---\n";
    markdown_core_document *doc = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    const markdown_core_metadata *metadata = markdown_core_node_document_metadata(markdown_core_document_root(doc));
    INT_EQ(runner, metadata_populated_count(metadata), 3, "only field lines produce metadata records");
    markdown_core_metadata_scalar value;
    OK(runner,
       markdown_core_metadata_value_scalar(markdown_core_metadata_name(metadata), &value) &&
           value.kind == MARKDOWN_CORE_METADATA_TEXT && value.value.string.length == 4 &&
           !memcmp(value.value.string.data, "\xf0\x9f\x9a\x80", 4),
       "quoted Unicode surrogate pair decodes to a scalar");
    markdown_core_document_free(doc);
    const char *ordered[] = {"---\nname: one\nstate: ready\n---\n", "---\nstate: ready\nname: one\n---\n"};
    uint8_t *dumps[2] = {0};
    size_t lengths[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        doc = markdown_core_document_parse((const uint8_t *)ordered[i], strlen(ordered[i]), NULL);
        OK(runner, markdown_core_document_dump(doc, &dumps[i], &lengths[i], NULL), "named fields dump");
        markdown_core_document_free(doc);
    }
    OK(runner, dumps[0] && dumps[1] && lengths[0] == lengths[1] && !memcmp(dumps[0], dumps[1], lengths[0]),
       "field order does not change metadata or its envelope scope");
    markdown_core_dump_free(dumps[0]);
    markdown_core_dump_free(dumps[1]);
}

static size_t properties_decoded_bytes;
static markdown_core_node *observe_properties(const markdown_core_extension *extension, markdown_core_parser *parser,
                                              markdown_core_node *root) {
    (void)extension;
    properties_decoded_bytes = parser->metadata_decoded_bytes;
    return root;
}
static void properties_member_work(test_batch_runner *runner) {
    static const markdown_core_extension observer = {.postprocess_func = observe_properties};
    const markdown_core_extension *extensions[] = {&observer};
    const struct {
        const char *source;
        bool final_field;
    } units[] = {{"authors: [\\\"\nunknown: 1\n", false},
                 {"# comment\nnot YAML\n...\n", true},
                 {"abstract: &a [true]\ncomment: *a\n", true},
                 {"{bad: [true], unknown: 1}\n", true},
                 {"{\nunknown: 1\n", false},
                 {"unknown: x[\nb[c: 2\nnot YAML\n", true},
                 {"name: duplicate\n", true},
                 {"authors: [\nstate: true\n]\n", true},
                 {"{\nstate: true\n}\n", true}};
    for (size_t shape = 0; shape < sizeof(units) / sizeof(*units); shape++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(markdown_core_get_default_mem_allocator());
            markdown_core_strbuf_puts(&source, "---\n");
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, units[shape].source);
            }
            markdown_core_strbuf_puts(&source, "state: kept\n---\nbody\n");
            markdown_core_node *root = parse_with_probes((const char *)source.ptr, source.size, extensions, 1);
            OK(runner, root != NULL, "adversarial Properties shape %zu at %zu members parses", shape, count);
            OK(runner, properties_decoded_bytes <= (size_t)source.size, "disjoint members never retry a failed suffix");
            if (root) {
                markdown_core_metadata *metadata = root->as.document->metadata;
                const markdown_core_metadata_value *state = markdown_core_metadata_state(metadata);
                markdown_core_metadata_scalar value;
                OK(runner,
                   units[shape].final_field
                       ? markdown_core_metadata_value_scalar(state, &value) &&
                             value.kind == MARKDOWN_CORE_METADATA_TEXT && value.value.string.length == 4 &&
                             !memcmp(value.value.string.data, "kept", 4)
                       : state == NULL,
                   "member ownership governs whether the final field is independent");
                OK(runner, metadata_populated_count(metadata) <= 10, "only named fields can be assigned");
                markdown_core_node_free(root);
            }
            markdown_core_strbuf_free(&source);
        }
    }
    for (size_t count = 128; count <= 65536; count *= 2) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(markdown_core_get_default_mem_allocator());
        markdown_core_strbuf_puts(&source, "---\nauthors: [");
        for (size_t i = 0; i < count; i++) {
            markdown_core_strbuf_puts(&source, i ? ",x" : "x");
        }
        markdown_core_strbuf_puts(&source, "]\n---\n");
        markdown_core_node *root = parse_with_probes((const char *)source.ptr, source.size, extensions, 1);
        OK(runner, root != NULL, "long flat list parses");
        if (root) {
            markdown_core_metadata *metadata = root->as.document->metadata;
            const markdown_core_metadata_value *record = markdown_core_metadata_authors(metadata);
            OK(runner, record && record->kind == MARKDOWN_CORE_METADATA_LIST && record->as.list.count == count,
               "all items survive one general flat-list decoder");
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

/* Track live bytes, not RSS or allocation timing. The header preserves C99
 * fundamental alignment and lets realloc account for released capacity. */
typedef union {
    size_t size;
    long double alignment;
    void *pointer;
} properties_allocation;
static size_t properties_live_bytes, properties_peak_bytes;
static void properties_account(size_t old_size, size_t new_size) {
    properties_live_bytes = properties_live_bytes - old_size + new_size;
    if (properties_live_bytes > properties_peak_bytes) {
        properties_peak_bytes = properties_live_bytes;
    }
}
static void *properties_calloc(size_t count, size_t size) {
    if (count && size > (SIZE_MAX - sizeof(properties_allocation)) / count) {
        return NULL;
    }
    size_t bytes = count * size;
    properties_allocation *allocation = calloc(1, sizeof(*allocation) + bytes);
    if (!allocation) {
        return NULL;
    }
    allocation->size = bytes;
    properties_account(0, bytes);
    return allocation + 1;
}
static void properties_free(void *pointer) {
    if (pointer) {
        properties_allocation *allocation = (properties_allocation *)pointer - 1;
        properties_account(allocation->size, 0);
        free(allocation);
    }
}
static void *properties_realloc(void *pointer, size_t size) {
    if (!size) {
        properties_free(pointer);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(properties_allocation)) {
        return NULL;
    }
    properties_allocation *allocation = pointer ? (properties_allocation *)pointer - 1 : NULL;
    size_t old_size = allocation ? allocation->size : 0;
    allocation = realloc(allocation, sizeof(*allocation) + size);
    if (!allocation) {
        return NULL;
    }
    allocation->size = size;
    properties_account(old_size, size);
    return allocation + 1;
}
static void properties_text_memory(test_batch_runner *runner) {
    markdown_core_mem mem = {properties_calloc, properties_realloc, properties_free};
    const char *prefixes[] = {"---\nname: x", "---\nname: \"x", "---\nabstract: |\n  ", "---\nauthors:\n- x"};
    const char *suffixes[] = {"\n", "\"\n", "\n", "\n- second\n"};
    for (size_t shape = 0; shape < sizeof(prefixes) / sizeof(*prefixes); shape++) {
        for (size_t count = 65536; count <= 1048576; count *= 4) {
            size_t baseline = 0;
            const char *characters = "x{}[]";
            for (size_t character = 0; characters[character]; character++) {
                markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(markdown_core_get_default_mem_allocator());
                markdown_core_strbuf_puts(&source, prefixes[shape]);
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, characters[character]);
                }
                markdown_core_strbuf_puts(&source, suffixes[shape]);
                markdown_core_strbuf_puts(&source, "state: ready\n---\n");
                properties_live_bytes = properties_peak_bytes = 0;
                markdown_core_node *root =
                    markdown_core_parse_document_with_mem((const char *)source.ptr, source.size, &mem, NULL, NULL);
                OK(runner, root != NULL, "long scalar shape %zu with %c parses", shape, characters[character]);
                if (root) {
                    markdown_core_metadata *metadata = root->as.document->metadata;
                    OK(runner, metadata_populated_count(metadata) == 2, "text and following field survive");
                    const markdown_core_metadata_value *record = shape == 2 ? markdown_core_metadata_abstract(metadata)
                                                                 : shape == 3 ? markdown_core_metadata_authors(metadata)
                                                                              : markdown_core_metadata_name(metadata);
                    markdown_core_string text = {0};
                    if (shape == 3) {
                        markdown_core_metadata_list_item item;
                        if (markdown_core_metadata_value_item_at(record, 0, &item)) {
                            text = item.value;
                        }
                    } else {
                        markdown_core_metadata_scalar scalar;
                        if (markdown_core_metadata_value_scalar(record, &scalar) &&
                            scalar.kind == MARKDOWN_CORE_METADATA_TEXT) {
                            text = scalar.value.string;
                        }
                    }
                    size_t offset = shape == 2 ? 0 : 1;
                    bool intact = text.length == count + 1;
                    for (size_t i = 0; intact && i < count; i++) {
                        intact = text.data[offset + i] == characters[character];
                    }
                    OK(runner, intact, "brackets remain complete scalar text");
                    markdown_core_node_free(root);
                }
                if (!character) {
                    baseline = properties_peak_bytes;
                }
                OK(runner, properties_peak_bytes == baseline,
                   "text brackets allocate no index storage: shape %zu, %zu bytes, %c peak %zu baseline %zu", shape,
                   count, characters[character], properties_peak_bytes, baseline);
                INT_EQ(runner, properties_live_bytes, 0, "all tracked parse allocations are released");
                markdown_core_strbuf_free(&source);
            }
        }
    }
}

static markdown_core_string owned_metadata_string(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    memcpy(copy, text, length + 1);
    return (markdown_core_string){(const uint8_t *)copy, length};
}

static void universal_values(test_batch_runner *runner) {
    const char *source = "![x](/u) ![y](/u)";
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    markdown_core_node *root = document->root;
    markdown_core_metadata *metadata = calloc(1, sizeof(*metadata));
    metadata->scope = (markdown_core_scope){{1, 1}, {1, 4}};
    root->as.document->metadata = metadata;
    markdown_core_metadata_value *values[] = {&metadata->name,  &metadata->time,    &metadata->date,
                                              &metadata->title, &metadata->authors, &metadata->keywords};
    for (size_t i = 0; i < 6; i++) {
        markdown_core_metadata_value *value = values[i];
        /* Only active union members are initialized. Access and cleanup must
         * not read inactive storage, including absent field payloads. */
        memset(&value->as, 0xa5, sizeof(value->as));
        value->kind = i < 4 ? MARKDOWN_CORE_METADATA_SCALAR : MARKDOWN_CORE_METADATA_LIST;
        if (i < 4) {
            value->as.scalar.kind = (markdown_core_metadata_scalar_kind)i;
        } else {
            value->as.list.items = NULL;
            value->as.list.count = 0;
        }
    }
    memset(&metadata->subtitle.as, 0xa5, sizeof(metadata->subtitle.as));
    metadata->time.as.scalar.value.boolean = true;
    metadata->date.as.scalar.value.string = owned_metadata_string("9007199254740993");
    metadata->title.as.scalar.value.string = owned_metadata_string("中文\nquoted");
    markdown_core_metadata_value *list = &metadata->keywords;
    list->as.list.count = 2;
    list->as.list.items = calloc(2, sizeof(*list->as.list.items));
    list->as.list.items[0] =
        (markdown_core_metadata_list_item){MARKDOWN_CORE_METADATA_ITEM_NUMBER, owned_metadata_string("1.25")};
    list->as.list.items[1] =
        (markdown_core_metadata_list_item){MARKDOWN_CORE_METADATA_ITEM_TEXT, owned_metadata_string("")};
    OK(runner, markdown_core_node_document_metadata(root) == metadata, "document owns metadata");
    INT_EQ(runner, metadata_populated_count(metadata), 6, "all metadata records retained");
    for (size_t i = 0; i < 6; i++) {
        const markdown_core_metadata_value *record = values[i];
        markdown_core_metadata_scalar scalar = {.kind = MARKDOWN_CORE_METADATA_BOOL, .value.boolean = false};
        markdown_core_metadata_list_item item = {.kind = MARKDOWN_CORE_METADATA_ITEM_TEXT, .value = {0}};
        INT_EQ(runner, markdown_core_metadata_value_get_kind(record),
               i < 4 ? MARKDOWN_CORE_METADATA_SCALAR : MARKDOWN_CORE_METADATA_LIST, "metadata value tag retained");
        INT_EQ(runner, markdown_core_metadata_value_scalar(record, &scalar), i < 4,
               "scalar accessor checks value branch");
        if (i < 4) {
            INT_EQ(runner, scalar.kind, i, "scalar tag retained");
            if (i == 1) {
                OK(runner, scalar.value.boolean, "boolean payload retained");
            } else if (i >= 2) {
                const char *expected = i == 2 ? "9007199254740993" : "中文\nquoted";
                OK(runner,
                   scalar.value.string.length == strlen(expected) &&
                       memcmp(scalar.value.string.data, expected, strlen(expected)) == 0,
                   "scalar string payload retained");
                OK(runner, scalar.value.string.data == record->as.scalar.value.string.data,
                   "scalar accessor borrows document string");
            }
        } else {
            OK(runner, scalar.kind == MARKDOWN_CORE_METADATA_BOOL && !scalar.value.boolean,
               "wrong scalar branch leaves output unchanged");
        }
        INT_EQ(runner, markdown_core_metadata_value_item_count(record), i == 5 ? 2 : 0,
               "only list branch exposes item count");
        INT_EQ(runner, markdown_core_metadata_value_item_at(record, 0, &item), i == 5,
               "list accessor checks value branch");
        if (i == 5) {
            OK(runner,
               item.kind == MARKDOWN_CORE_METADATA_ITEM_NUMBER && item.value.length == 4 &&
                   memcmp(item.value.data, "1.25", 4) == 0,
               "list number payload retained");
            OK(runner, item.value.data == list->as.list.items[0].value.data, "list accessor borrows document string");
            OK(runner, markdown_core_metadata_value_item_at(record, 1, &item), "second list item accessible");
            OK(runner, item.kind == MARKDOWN_CORE_METADATA_ITEM_TEXT && item.value.length == 0,
               "empty text list item retained");
        } else {
            OK(runner, item.kind == MARKDOWN_CORE_METADATA_ITEM_TEXT && !item.value.data && !item.value.length,
               "absent list item leaves output unchanged");
        }
        markdown_core_metadata_list_item before = item;
        OK(runner, !markdown_core_metadata_value_item_at(record, 2, &item), "metadata item bounds checked");
        OK(runner,
           item.kind == before.kind && item.value.data == before.value.data && item.value.length == before.value.length,
           "out-of-bounds item leaves output unchanged");
        OK(runner, !markdown_core_metadata_value_scalar(record, NULL), "null scalar output rejected");
        OK(runner, !markdown_core_metadata_value_item_at(record, 0, NULL), "null list item output rejected");
    }
    OK(runner, !markdown_core_metadata_subtitle(metadata), "absent field differs from explicit null");
    OK(runner, markdown_core_metadata_name(metadata) != NULL, "explicit null field is present");
    markdown_core_node *image = root->first_child->first_child;
    OK(runner, markdown_core_node_dimensions(image) == NULL, "unsized image has no dimensions");
    image->as.link->dimensions.has_value = true;
    image->as.link->dimensions.value = (markdown_core_dimensions){640, {true, 480}};
    const markdown_core_dimensions *dimensions = markdown_core_node_dimensions(image);
    OK(runner,
       dimensions && dimensions->width == 640 && dimensions->height.has_value && dimensions->height.value == 480,
       "dimensions preserve their owned value");
    OK(runner, markdown_core_node_dimensions(root) == NULL && markdown_core_node_dimensions(NULL) == NULL,
       "only a sized image has dimensions");
    uint8_t *dump = NULL;
    size_t length = 0;
    OK(runner, markdown_core_document_dump(document, &dump, &length, NULL), "metadata and dimensions dump");
    OK(runner, strstr((const char *)dump, "date=scalar(number(\"9007199254740993\"))") != NULL,
       "decimal text never rounded");
    OK(runner, strstr((const char *)dump, "authors=list([])") != NULL, "empty list distinct from null");
    OK(runner, strstr((const char *)dump, "dimensions=(width=640,height=480)") != NULL, "typed dimensions dump");
    markdown_core_dump_free(dump);
    markdown_core_document_free(document); /* Sanitizers verify complete recursive ownership. */
}

/* Count visited source positions as well as verifying values. Repeated failed
 * candidates share one extent, so they cannot rescan each other's suffixes. */
typedef struct {
    size_t cross_link, opaque, delimiters, comment, lookahead, footnote_body, block_identifier, callout, dimensions;
    size_t registered_footnotes;
    bool footnote_collection_allocated, footnotes_owned;
} inline_work;
static markdown_core_node *record_inline_work(const markdown_core_extension *extension, markdown_core_parser *parser,
                                              markdown_core_node *root) {
    (void)extension;
    inline_work *work = root->user_data;
    if (!work) {
        return root;
    }
    work->cross_link = parser->cross_link_scan_work;
    work->opaque = parser->opaque_scan_work;
    work->delimiters = parser->delimiter_work;
    work->comment = parser->comment_scan_work;
    work->lookahead = parser->block_lookahead_work;
    work->block_identifier = parser->block_identifier_work;
    work->callout = parser->callout_scan_work;
    work->dimensions = parser->dimension_work;
    work->footnote_body = parser->footnote_body_work;
    work->registered_footnotes = parser->footnote_registration_work;
    work->footnote_collection_allocated = parser->footnotes.values != NULL;
    work->footnotes_owned = true;
    for (markdown_core_node *note = root->as.document->footnotes; note; note = note->next) {
        work->footnotes_owned &=
            note->kind == MARKDOWN_CORE_NODE_FOOTNOTE && note->parent == NULL && note->as.footnote->id.data != NULL;
    }
    root->user_data = NULL;
    return root;
}
static const markdown_core_extension WORK_RECORDER = {.name = "work-recorder", .postprocess_func = record_inline_work};
static bool measure_inline_work(markdown_core_parser *parser, void *context) {
    parser->root->user_data = context;
    return markdown_core_parser_attach_extension(parser, &WORK_RECORDER);
}

static void cross_link_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    static const struct {
        const char *prefix, *unit, *suffix;
    } cases[] = {
        {"", "!", "[[a]]"},
        {"", "[", "a]]"},
        {"[[a]]", "]", ""},
        {"[[a", "#h", "]]"},
        {"[[", "^", "]]"},
        {"[[a|", "|", "]]"},
        {"![[a|", "9", "]]"},
        {"![[a|1x", "9", "]]"},
        {"![[a|", "|", "2147483647x1]]"},
        {"![[a|", "\\|", "2147483647x1]]"},
        {"", "![[a|caption|1x2]] ", ""},
        {"", "![[a|caption|1x2[", "]]"},
        {"![[", "a", ""},
        {"![[", "a", "]"},
        {"", "![[a[", "]]"},
        {"", "[[a#|x]]", ""},
        {"", "[[a\\|b]] ![[#^id|]] ", ""},
        {"$", "![[a|", "$]]"},
        {"", "$x ", ""},
        {"\\\\(", "[[a|", "\\\\)]]"},
        {"", "\\\\(a ", ""},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_mem((char *)source.ptr, source.size, mem, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial cross links parse successfully");
            OK(runner, work.cross_link <= 3 * (size_t)source.size,
               "cross-link scanner inspects disjoint bodies: case=%zu size=%d work=%zu", c, source.size,
               work.cross_link);
            OK(runner, work.opaque <= 4 * (size_t)source.size,
               "opaque delimiter searches are linear: case=%zu size=%d work=%zu", c, source.size, work.opaque);
            OK(runner, work.dimensions <= 3 * (size_t)source.size,
               "dimension recognition is bounded: case=%zu size=%d work=%zu", c, source.size, work.dimensions);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* Nested bodies are never rescanned for emptiness and close in one bracket
 * operation. The document order follows opening positions, not close order. */
static void inline_footnote_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    static const struct {
        const char *left, *middle, *right;
        int notes_per_unit;
    } cases[] = {
        {"^", "", "", 0},        {"[", "x", "]", 0},      {"^[", "x", "]", 1},  {"^[", "", "", 0},
        {"]", "", "", 0},        {"^[ \t ] ", "", "", 0}, {"^[a] ", "", "", 1}, {"^[a [x](u) ", "z", " b]", 1},
        {"^[a ", "^[]", "]", 1},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].left);
            }
            markdown_core_strbuf_puts(&source, cases[c].middle);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].right);
            }
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_mem(
                (const char *)source.ptr, (size_t)source.size, mem, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial inline footnotes parse");
            OK(runner, work.footnote_body <= (size_t)source.size,
               "nonblank evidence inspects disjoint source ranges: case=%zu size=%d work=%zu", c, source.size,
               work.footnote_body);
            INT_EQ(runner, work.registered_footnotes, count * cases[c].notes_per_unit,
                   "all committed notes are registered before finalization");
            OK(runner, work.footnotes_owned, "postprocessors receive resolved document-owned footnotes");
            OK(runner, !work.footnote_collection_allocated,
               "parse-time footnote edges are discarded before postprocessing");
            if (root) {
                size_t actual = 0;
                int previous_column = 0;
                for (markdown_core_node *note = root->as.document->footnotes; note; note = note->next) {
                    char expected[40];
                    snprintf(expected, sizeof(expected), "inline-%zu", ++actual);
                    STR_EQ(runner, (const char *)note->as.footnote->id.data, expected, "ids follow source order");
                    OK(runner, note->start_column > previous_column && note->parent == NULL,
                       "document owns each value once in increasing source order");
                    previous_column = note->start_column;
                }
                INT_EQ(runner, actual, count * cases[c].notes_per_unit, "all and only completed notes are retained");
                markdown_core_node_free(root);
            }
            markdown_core_strbuf_free(&source);
        }
    }
    for (size_t count = 128; count <= 4096; count *= 2) {
        markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
        markdown_core_strbuf_puts(&source, "^[x]\n\n[^inline-1]: reserved\n");
        for (size_t i = 1; i <= count; i++) {
            char definition[64];
            snprintf(definition, sizeof(definition), "[^inline-1-%zu]: reserved\n", i);
            markdown_core_strbuf_puts(&source, definition);
        }
        markdown_core_node *root =
            markdown_core_parse_document_with_mem((const char *)source.ptr, (size_t)source.size, mem, NULL, NULL);
        OK(runner, root != NULL, "long authored suffix sets parse");
        if (root) {
            char expected[40];
            snprintf(expected, sizeof(expected), "inline-1-%zu", count + 1);
            STR_EQ(runner, (const char *)root->as.document->footnotes->as.footnote->id.data, expected,
                   "generated ids skip the complete authored suffix set");
            markdown_core_node_free(root);
        }
        markdown_core_strbuf_free(&source);
    }
}

/* Definitions, closing brackets, and deferred fields commit in different
 * orders. All must already be registered before finalization, including
 * notes whose enclosing candidate fails or becomes a link or image. */
static void footnote_registration(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    static const struct {
        const char *source;
        size_t count;
        const char *ids[6];
    } cases[] = {
        {"^[outer ^[inner]] :d[^[label]] ^[last]\n\n[^inline-1]: ^[body]\n",
         6,
         {"inline-1-1", "inline-2", "inline-3", "inline-4", "inline-1", "inline-5"}},
        {"^[unclosed ^[inner]", 1, {"inline-1"}},
        {"[^a]: ^[body]\n\n[^a ^[x]] [^[link]](u) ![^[image]](v)\n",
         5,
         {"a", "inline-1", "inline-2", "inline-3", "inline-4"}},
        {"`^[code]` $^[formula]$ %%^[comment]%% <!-- ^[html] --> <i data-x=\"^[token]\">\n", 0, {NULL}},
        {"[[^[target]]]\n", 1, {"inline-1"}},
        {"[^a]: first\n[^a]: duplicate\n", 2, {"a", "a"}},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        inline_work work = {0};
        markdown_core_node *root = markdown_core_parse_document_with_mem(cases[c].source, strlen(cases[c].source), mem,
                                                                         measure_inline_work, &work);
        OK(runner, root != NULL, "footnote registration boundaries parse: case=%zu", c);
        INT_EQ(runner, work.registered_footnotes, cases[c].count, "only committed notes enter the parser collection");
        OK(runner, work.footnotes_owned, "postprocessors receive resolved document-owned footnotes");
        if (root) {
            size_t actual = 0;
            for (markdown_core_node *note = root->as.document->footnotes; note; note = note->next) {
                if (actual < cases[c].count) {
                    STR_EQ(runner, (const char *)note->as.footnote->id.data, cases[c].ids[actual],
                           "finalization orders committed notes by source start");
                }
                actual++;
            }
            INT_EQ(runner, actual, cases[c].count, "each registered note becomes one document value");
            markdown_core_node_free(root);
        }
    }
}

typedef struct {
    size_t removed;
    bool resolved, index_released;
} footnote_postprocess_probe;

static markdown_core_node *remove_footnotes(const markdown_core_extension *extension, markdown_core_parser *parser,
                                            markdown_core_node *root) {
    (void)extension;
    if (root->kind != MARKDOWN_CORE_NODE_DOCUMENT) {
        return root;
    }
    footnote_postprocess_probe *probe = root->user_data;
    probe->index_released =
        parser->footnotes.values == NULL && parser->footnotes.count == 0 && parser->footnotes.last_inline == NULL;
    probe->resolved = true;
    while (root->as.document->footnotes) {
        markdown_core_node *note = root->as.document->footnotes;
        probe->resolved &= note->parent == NULL && note->as.footnote->id.data != NULL;
        root->as.document->footnotes = note->next;
        markdown_core_node_free(note);
        probe->removed++;
    }
    root->user_data = NULL;
    return root;
}

static bool observe_footnote_removal(markdown_core_parser *parser, void *context) {
    static const markdown_core_extension probe = {.name = "remove-footnotes", .postprocess_func = remove_footnotes};
    parser->root->user_data = context;
    return markdown_core_parser_attach_extension(parser, &probe);
}

static void footnote_postprocessing(test_batch_runner *runner) {
    static const char source[] = "^[outer ^[inner]] :d[^[label]]\n\n[^n]: ^[body]\n";
    footnote_postprocess_probe probe = {0};
    markdown_core_node *root = markdown_core_parse_document_with_mem(
        source, sizeof(source) - 1, markdown_core_get_default_mem_allocator(), observe_footnote_removal, &probe);
    OK(runner, root != NULL, "postprocessing may remove document footnotes without stale parser pointers");
    INT_EQ(runner, probe.removed, 5, "postprocessor receives all authored and inline values");
    OK(runner, probe.resolved && probe.index_released, "ids and ownership are final before mutable callbacks run");
    if (root) {
        markdown_core_node *cite = root->first_child->first_child;
        OK(runner, !root->as.document->footnotes && !cite->first_child,
           "Cite has no transient structural body before or after postprocessing");
        STR_EQ(runner, (const char *)cite->as.cite->citations->as.citation->value.data, "inline-1",
               "citation ids survive deletion of their document value");
        markdown_core_node *note = markdown_core_node_new(MARKDOWN_CORE_NODE_FOOTNOTE);
        OK(runner, !markdown_core_node_append_child(cite, note), "Cite containment rejects a Footnote child");
        markdown_core_node_free(note);
        markdown_core_node_free(root);
    }
}

static size_t text_allocation_calls;
static void *count_text_calloc(size_t count, size_t size) {
    text_allocation_calls++;
    return calloc(count, size);
}
static void *count_text_realloc(void *pointer, size_t size) {
    text_allocation_calls++;
    return realloc(pointer, size);
}

/* A literal caret is ordinary text. Allocation work must equal an ordinary
 * text span of the same length, even when every byte is a caret. */
static void literal_caret_allocations(test_batch_runner *runner) {
    markdown_core_mem mem = {count_text_calloc, count_text_realloc, free};
    for (size_t count = 1024; count <= 1048576; count *= 2) {
        char *source = malloc(count);
        size_t ordinary_allocations = 0;
        for (size_t shape = 0; shape < 3; shape++) {
            for (size_t i = 0; i < count; i++) {
                source[i] = shape == 0 || (shape == 2 && i % 2) ? 'a' : '^';
            }
            text_allocation_calls = 0;
            markdown_core_node *root = markdown_core_parse_document_with_mem(source, count, &mem, NULL, NULL);
            OK(runner, root != NULL, "ordinary text and caret spans parse at %zu bytes", count);
            if (shape == 0) {
                ordinary_allocations = text_allocation_calls;
            } else {
                INT_EQ(runner, text_allocation_calls, ordinary_allocations,
                       "literal carets allocate only for their text span");
            }
            if (root) {
                markdown_core_node *text = root->first_child->first_child;
                OK(runner,
                   !text->next && text->as.literal->len == (bufsize_t)count &&
                       memcmp(text->as.literal->data, source, count) == 0,
                   "one Text retains every literal byte");
                markdown_core_node_free(root);
            }
        }
        free(source);
    }
}

typedef struct {
    const char *left, *middle, *right;
    size_t nodes_per_unit;
} paired_delimiter_case;

static void paired_delimiter_linear_work(test_batch_runner *runner, markdown_core_node_type kind,
                                         const paired_delimiter_case *cases, size_t case_count) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    for (size_t c = 0; c < case_count; c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].left);
            }
            markdown_core_strbuf_puts(&source, cases[c].middle);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].right);
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_mem((char *)source.ptr, source.size, mem, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial paired-delimiter runs parse successfully");
            OK(runner, work.delimiters > 0 && work.delimiters <= 8 * (size_t)source.size,
               "shared delimiter work is linear: case=%zu size=%d work=%zu", c, source.size, work.delimiters);
            size_t nodes = 0;
            markdown_core_iter *iter = markdown_core_iter_new(root);
            markdown_core_event_type event;
            while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
                if (event == MARKDOWN_CORE_EVENT_ENTER && markdown_core_iter_get_node(iter)->kind == kind) {
                    nodes++;
                }
            }
            markdown_core_iter_free(iter);
            size_t expected = count * cases[c].nodes_per_unit;
            INT_EQ(runner, nodes, expected, "pairwise matching preserves the semantic paired-delimiter count");
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void mark_linear_work(test_batch_runner *runner) {
    static const paired_delimiter_case cases[] = {
        {"=", "", "", 0},                                       // one maximal run: no empty mark
        {"==", "x", "==", 1},                                   // one nested pair per repeat
        {"==a ", "", "", 0},                                    // unmatched openers
        {" a==", "", "", 0},                                    // unmatched closers
        {"==a* ", "", "", 0},                                   // failed searches across another rule
        {"==a* ", "", " b==", 1},                               // mixed nested runs
        {"==a===b== ", "", "", 1},                              // odd leftovers must never match
        {"==*a*== ", "", "", 1},                                // parsed child ownership
        {"==a====b== ", "", "", 2},                             // adjacent marks
        {"[==a==](/u) ", "", "", 1},                            // separate inline containers
        {"<i title=\"==hidden==\">==*body*==</i> ", "", "", 1}, // token opacity, live body
        {"==<i title=\"==hidden==\">body</i>== ", "", "", 1},   // tag delimiters stay owned
        {"==a %%==b%% c== ", "", "", 1},                        // comment cannot close the mark
    };
    paired_delimiter_linear_work(runner, MARKDOWN_CORE_NODE_MARK, cases, sizeof(cases) / sizeof(*cases));
}

static void insertion_linear_work(test_batch_runner *runner) {
    static const paired_delimiter_case cases[] = {
        {"+", "", "", 0},                                       // one maximal run: no empty insertion
        {"++", "x", "++", 1},                                   // one nested pair per repeat
        {"++a ", "", "", 0},                                    // unmatched openers
        {" a++", "", "", 0},                                    // unmatched closers
        {"++a* ", "", "", 0},                                   // failed searches across another rule
        {"++a* ", "", " b++", 1},                               // mixed nested runs
        {"++a+++b++ ", "", "", 1},                              // odd leftovers must never match
        {"++*a*++ ", "", "", 1},                                // parsed child ownership
        {"++a++++b++ ", "", "", 2},                             // adjacent insertions
        {"[++a++](/u) ", "", "", 1},                            // separate inline containers
        {"<i title=\"++hidden++\">++*body*++</i> ", "", "", 1}, // token opacity, live body
        {"++<i title=\"++hidden++\">body</i>++ ", "", "", 1},   // tag delimiters stay owned
        {"++a %%++b%% c++ ", "", "", 1},                        // comment cannot close the insertion
    };
    paired_delimiter_linear_work(runner, MARKDOWN_CORE_NODE_INSERTION, cases, sizeof(cases) / sizeof(*cases));
}

static size_t count_kind(markdown_core_node *root, markdown_core_node_type kind) {
    size_t found = 0;
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type event;
    while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (event == MARKDOWN_CORE_EVENT_ENTER && markdown_core_iter_get_node(iter)->kind == kind) {
            found++;
        }
    }
    markdown_core_iter_free(iter);
    return found;
}

/* O3: the inline `%%` scanner. Every failed closer search is cached under the
 * comment's rule, so a run of signs that never closes, a body that crosses
 * another construct's bytes, and a comment beside every earlier opaque
 * construct all cost work proportional to the input. */
static void comment_inline_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    static const struct {
        const char *prefix, *unit, *suffix;
        size_t comments_per_unit; /* comments per unit, or 0 when the count is asserted separately */
    } cases[] = {
        {"", "%%a%% ", "", 1},                // closed comments
        {"", "%%%%", "", 1},                  // adjacent empty comments
        {"", "%", "", 0},                     // one maximal run: pairs close, at most one sign is text
        {"", "%%%a", "", 0},                  // the leftover of each run opens the next candidate
        {"%%", "a", "", 0},                   // one opener, one long failed search
        {"", "%% a", "", 0},                  // openers whose closers are the next openers
        {"", "\\%%a", "", 0},                 // escaped signs never open
        {"", "$%%$ ", "", 0},                 // formula bodies own their signs
        {"", "`%%` ", "", 0},                 // code spans own their signs
        {"", "[[%%]] ", "", 0},               // cross links own their signs
        {"", "%%[[a", "", 0},                 // comment bodies own brackets
        {"", "==%%a%%== ", "", 1},            // marks around comments
        {"", "%%a%%%%b%% <!-- c -->", "", 3}, // both grammars side by side, three Comment nodes
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_mem((char *)source.ptr, source.size, mem, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial percent runs parse successfully");
            OK(runner, work.comment + work.opaque <= 4 * (size_t)source.size,
               "comment scanner work is linear: case=%zu size=%d comment=%zu opaque=%zu", c, source.size, work.comment,
               work.opaque);
            if (cases[c].comments_per_unit) {
                INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_COMMENT), count * cases[c].comments_per_unit,
                       "every closed comment is one Comment node");
            }
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* O3: the block form's lookahead. A candidate scans forward under the open
 * containers' prefixes; nested containers each carrying a candidate that never
 * closes are the shape that would rescan every line once per container, and
 * blank runs inside nested list items are the shape that would visit every
 * blank line once per candidate. The recorded work counts lines visited and
 * prefix bytes matched, and stays within a constant of the input size. */
static void comment_block_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    for (int shape = 0; shape < 5; shape++) {
        for (size_t depth = 16; depth <= 128; depth *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            size_t expected_comments = 0;
            size_t expected_paragraphs = 0;
            size_t i;
            switch (shape) {
            case 0: /* nested block quotes, each opening a candidate, no closer anywhere */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 0; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "> ");
                    }
                    markdown_core_strbuf_puts(&source, "%%\n");
                }
                expected_paragraphs = depth;
                break;
            case 1: /* nested block quotes whose deepest candidate closes; the outer ones cannot */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 0; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "> ");
                    }
                    markdown_core_strbuf_puts(&source, "%%\n");
                }
                for (size_t j = 0; j < depth; j++) {
                    markdown_core_strbuf_puts(&source, "> ");
                }
                markdown_core_strbuf_puts(&source, "x\n");
                for (size_t j = 0; j < depth; j++) {
                    markdown_core_strbuf_puts(&source, "> ");
                }
                markdown_core_strbuf_puts(&source, "%%\n");
                expected_comments = 1;
                expected_paragraphs = depth - 1;
                break;
            case 2: /* nested list items, each opening a candidate, then a long blank run */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 1; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "  ");
                    }
                    markdown_core_strbuf_puts(&source, "- %%\n");
                }
                for (i = 0; i < depth * depth; i++) {
                    markdown_core_strbuf_puts(&source, "\n");
                }
                expected_paragraphs = depth;
                break;
            case 3: /* nested list items with blank runs between non-blank continuation lines */
                for (i = 1; i <= depth; i++) {
                    for (size_t j = 1; j < i; j++) {
                        markdown_core_strbuf_puts(&source, "  ");
                    }
                    markdown_core_strbuf_puts(&source, "- %%\n");
                }
                for (i = 0; i < depth; i++) {
                    for (size_t j = 0; j < depth; j++) {
                        markdown_core_strbuf_puts(&source, "\n");
                    }
                    for (size_t j = 0; j < depth; j++) {
                        markdown_core_strbuf_puts(&source, "  ");
                    }
                    markdown_core_strbuf_puts(&source, "x\n");
                }
                expected_paragraphs = depth + depth;
                break;
            default: /* many candidates in sibling items: each scan ends at its item's end */
                for (i = 0; i < depth * depth; i++) {
                    markdown_core_strbuf_puts(&source, "- %%\n  a\n");
                }
                expected_paragraphs = depth * depth;
                break;
            }
            inline_work work = {0};
            markdown_core_node *root =
                markdown_core_parse_document_with_mem((char *)source.ptr, source.size, mem, measure_inline_work, &work);
            OK(runner, root != NULL, "nested block comment candidates parse successfully");
            OK(runner, work.lookahead <= 4 * (size_t)source.size,
               "block lookahead work is linear: shape=%d size=%d work=%zu", shape, source.size, work.lookahead);
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_COMMENT_BLOCK), expected_comments,
                   "only a closed candidate is a block comment: shape=%d depth=%zu", shape, depth);
            INT_EQ(runner, count_kind(root, MARKDOWN_CORE_NODE_PARAGRAPH), expected_paragraphs,
                   "a failed candidate's line is paragraph text: shape=%d depth=%zu", shape, depth);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* O3: the `%%` forms produce the `Comment` kind M0 added, on the engine's own
 * accessors, with the type strings and positions the HTML forms have. */
static void percent_comment_nodes(test_batch_runner *runner) {
    static const char markdown[] = "a %%b%% c %%%% %%x\r\ny%% d\r\n"
                                   "\r\n"
                                   "%%\r\n"
                                   "  block\r\n"
                                   "\r\n"
                                   "%%\t\r\n"
                                   "\r\n"
                                   "> %%\r\n"
                                   "> q\r\n"
                                   "> %%\r\n"
                                   "\r\n"
                                   "%%\r\n"
                                   "open\r\n";
    /* The engine entry with the dialect attached: `%%` is an extension's
     * syntax, unlike the HTML comment of `comment_nodes` above. */
    markdown_core_node *doc = markdown_core_parse_document_with_mem(
        markdown, sizeof(markdown) - 1, markdown_core_get_default_mem_allocator(), NULL, NULL);
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *text = markdown_core_node_first_child(paragraph);
    markdown_core_node *comment = markdown_core_node_next(text);
    markdown_core_node *empty = markdown_core_node_next(markdown_core_node_next(comment));
    markdown_core_node *spanning = markdown_core_node_next(markdown_core_node_next(empty));
    markdown_core_node *block = markdown_core_node_next(paragraph);
    markdown_core_node *quote = markdown_core_node_next(block);
    markdown_core_node *quoted = markdown_core_node_first_child(quote);
    markdown_core_node *open = markdown_core_node_next(quote);

    INT_EQ(runner, markdown_core_node_get_type(comment), MARKDOWN_CORE_NODE_COMMENT, "inline `%%` comment type");
    STR_EQ(runner, markdown_core_node_get_type_string(comment), "comment", "inline `%%` comment type string");
    STR_EQ(runner, markdown_core_node_get_literal(comment), "b", "inline literal excludes the delimiters");
    INT_EQ(runner, markdown_core_node_get_start_column(comment), 3, "inline scope starts at the opener");
    INT_EQ(runner, markdown_core_node_get_end_column(comment), 7, "inline scope ends at the closer");
    INT_EQ(runner, markdown_core_node_get_type(empty), MARKDOWN_CORE_NODE_COMMENT, "`%%%%` is a comment");
    STR_EQ(runner, markdown_core_node_get_literal(empty), "", "`%%%%` has an empty literal");
    STR_EQ(runner, markdown_core_node_get_literal(spanning), "x\ny",
           "a body spanning lines keeps one LF per line ending");
    INT_EQ(runner, markdown_core_node_get_start_line(spanning), 1, "a spanning body starts on its opener line");
    INT_EQ(runner, markdown_core_node_get_end_line(spanning), 2, "a spanning body ends on its closer line");
    INT_EQ(runner, markdown_core_node_get_end_column(spanning), 3, "a spanning body ends at its closer");

    INT_EQ(runner, markdown_core_node_get_type(block), MARKDOWN_CORE_NODE_COMMENT_BLOCK, "block `%%` comment type");
    STR_EQ(runner, markdown_core_node_get_type_string(block), "comment_block", "block `%%` comment type string");
    STR_EQ(runner, markdown_core_node_get_literal(block), "  block\n\n",
           "block literal keeps indentation, blank lines, and LF line endings, and excludes both fences");
    INT_EQ(runner, markdown_core_node_get_start_line(block), 4, "block starts on its opener line");
    INT_EQ(runner, markdown_core_node_get_end_line(block), 7, "block ends on its closer line");
    INT_EQ(runner, markdown_core_node_get_end_column(block), 3, "block ends at its closer line's last byte");
    OK(runner, markdown_core_node_first_child(block) == NULL, "a block comment is a leaf");

    INT_EQ(runner, markdown_core_node_get_type(quote), MARKDOWN_CORE_NODE_CALLOUT,
           "the quoted form is inside its container");
    INT_EQ(runner, markdown_core_node_get_type(quoted), MARKDOWN_CORE_NODE_COMMENT_BLOCK, "a quoted block comment");
    STR_EQ(runner, markdown_core_node_get_literal(quoted), "q\n", "a quoted literal has its prefix removed");
    INT_EQ(runner, markdown_core_node_get_start_column(quoted), 3, "a quoted block starts after the prefix");

    INT_EQ(runner, markdown_core_node_get_type(open), MARKDOWN_CORE_NODE_PARAGRAPH,
           "an unclosed candidate is a paragraph");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(open)), "%%",
           "an unmatched opener is text");
    OK(runner, markdown_core_node_next(open) == NULL, "nothing follows the unclosed candidate");

    markdown_core_node_free(doc);
}

static void cross_link_fields(test_batch_runner *runner) {
    const char *source = "[[ Note ]] [[Note|]] ![[#^block|raw *label*]]";
    markdown_core_document *doc = markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL);
    OK(runner, doc != NULL, "cross links parse through facade");
    const markdown_core_node *node = markdown_core_node_get_first_child(markdown_core_document_root(doc));
    node = markdown_core_node_get_first_child(node);
    markdown_core_destination dest;
    markdown_core_optional_string label;
    label = markdown_core_node_cross_label(node);
    OK(runner, markdown_core_node_destination(node, &dest) && dest.kind == MARKDOWN_CORE_DESTINATION_CROSS,
       "cross links produce the cross destination branch");
    OK(runner, dest.path.length == 6 && memcmp(dest.path.data, " Note ", 6) == 0 && !dest.anchor.has_value,
       "path bytes are preserved and anchor is absent");
    OK(runner,
       markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_CROSS_LINK && !label.has_value &&
           markdown_core_node_dimensions(node) == NULL,
       "no separator means absent label");
    OK(runner, markdown_core_node_resource(node) == NULL && markdown_core_node_get_first_child(node) == NULL,
       "a cross link is an occurrence-owned leaf");
    OK(runner, !markdown_core_node_title(node, &label), "cross links have labels, not link titles");
    node = markdown_core_node_get_next_sibling(markdown_core_node_get_next_sibling(node));
    label = markdown_core_node_cross_label(node);
    OK(runner, label.has_value && label.value.length == 0, "an authored empty label remains present");
    node = markdown_core_node_get_next_sibling(markdown_core_node_get_next_sibling(node));
    OK(runner, markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_CROSS_EMBEDDED, "transclusion has its own kind");
    OK(runner,
       strcmp(markdown_core_node_kind_name(MARKDOWN_CORE_KIND_CROSS_EMBEDDED), "CrossEmbedded") == 0 &&
           markdown_core_node_dimensions(node) == NULL,
       "an unsized CrossEmbedded retains its kind");
    markdown_core_node_destination(node, &dest);
    OK(runner,
       dest.path.length == 0 && dest.anchor.has_value && dest.anchor.value.length == 5 &&
           memcmp(dest.anchor.value.data, "block", 5) == 0,
       "block punctuation is removed, current-document path is empty");
    OK(runner,
       !markdown_core_node_cross_label(NULL).has_value &&
           !markdown_core_node_cross_label(markdown_core_document_root(doc)).has_value,
       "cross label is absent for null and unrelated nodes");
    markdown_core_document_free(doc);
}

static void attribute_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    static const struct {
        const char *prefix, *unit, *suffix;
        bool valid;
    } cases[] = {
        {"{", ".a k=1 ", "}", true},   {"{", "k=1 k=2 class='a a' ", "}", true},
        {"", "{#valid ", "?}", false}, {"", "{k=bad ", "", false},
        {"", "{k=' ", "", false},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            markdown_core_attribute_parser parser = {.mem = mem, .data = source.ptr, .length = source.size};
            size_t attempts = 0;
            for (bufsize_t at = 0; at < source.size; at++) {
                if (source.ptr[at] != '{') {
                    continue;
                }
                markdown_core_attributes value = {0};
                bufsize_t end = -1;
                int valid = markdown_core_attributes_parse(&parser, at, &value, &end);
                attempts++;
                INT_EQ(runner, valid, cases[c].valid, "attribute recognition is atomic");
                if (valid) {
                    INT_EQ(runner, end, source.size, "complete container consumed");
                    INT_EQ(runner, value.class_count, count * (c == 1 ? 2 : 1), "class occurrences retained");
                    INT_EQ(runner, value.record_count, count * (c == 1 ? 2 : 1), "duplicate records retained");
                    OK(runner,
                       value.class_capacity <= 2 * value.class_count && value.record_capacity <= 2 * value.record_count,
                       "attribute vector storage is linear in retained values");
                } else {
                    INT_EQ(runner, end, -1, "failed candidate never advances caller");
                    OK(runner, !value.classes && !value.records && !value.anchor.data,
                       "no partial attribute value escapes");
                }
                markdown_core_attributes_free(mem, &value);
            }
            OK(runner, parser.work <= 12 * (size_t)source.size + attempts,
               "attribute work is linear: case=%zu size=%d work=%zu", c, source.size, parser.work);
            markdown_core_attribute_parser_free(&parser);
            markdown_core_strbuf_free(&source);
        }
    }
}

static size_t count_anchors(markdown_core_node *root) {
    size_t count = 0;
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type event;
    while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (event == MARKDOWN_CORE_EVENT_ENTER && markdown_core_iter_get_node(iter)->attributes.anchor.len) {
            count++;
        }
    }
    markdown_core_iter_free(iter);
    return count;
}

/* Long digit/pipe runs and nested successful images exercise the same bound:
 * no image closer rescans a nested label, including labels without pipes. */
static void image_dimension_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    for (size_t count = 128; count <= 8192; count *= 2) {
        for (int shape = 0; shape < 5; shape++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            if (shape < 2) {
                markdown_core_strbuf_puts(&source, "![alt|");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, shape == 0 ? '9' : '|');
                }
                markdown_core_strbuf_puts(&source, "10](/i)\n");
            } else {
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_puts(&source, shape == 4 ? "![" : "![a|");
                }
                markdown_core_strbuf_puts(&source, "1");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_puts(&source, shape == 3 ? "](/i)|2" : "](/i)");
                }
                markdown_core_strbuf_putc(&source, '\n');
            }
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_mem(
                (const char *)source.ptr, (size_t)source.size, mem, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial image labels parse");
            OK(runner, work.dimensions > 0 && work.dimensions <= 3 * (size_t)source.size,
               "image dimension work is linear: shape=%d bytes=%d work=%zu", shape, source.size, work.dimensions);
            if (root) {
                markdown_core_node *node = root->first_child->first_child;
                size_t images = 0, sized = 0;
                while (node) {
                    if (node->kind == MARKDOWN_CORE_NODE_MEDIA) {
                        images++;
                        sized += node->as.link->dimensions.has_value;
                    }
                    node = node->first_child ? node->first_child : node->next;
                }
                INT_EQ(runner, images, shape < 2 ? 1 : count, "nested image depth is preserved");
                INT_EQ(runner, sized,
                       shape == 0   ? 0
                       : shape == 3 ? count
                                    : 1,
                       "only each image's own complete suffix sets dimensions");
            }
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

/* Each newly opened quote inspects one bounded prefix or its own type/title
 * separators. Deep quote chains and long failed type candidates must never
 * rescan the remainder of the document. */
static void callout_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    for (size_t count = 128; count <= 8192; count *= 2) {
        for (int shape = 0; shape < 4; shape++) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            if (shape == 0) {
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, '>');
                }
                markdown_core_strbuf_puts(&source, " [!deep]- ==title==\n");
            } else if (shape == 1) {
                markdown_core_strbuf_puts(&source, "> [!");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, 'a');
                }
                markdown_core_strbuf_puts(&source, ".] invalid\n");
            } else if (shape == 2) {
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_puts(&source, "> [!note]+ T\n\n");
                }
            } else {
                markdown_core_strbuf_puts(&source, "> [!note]-");
                for (size_t i = 0; i < count; i++) {
                    markdown_core_strbuf_putc(&source, ' ');
                }
                markdown_core_strbuf_puts(&source, "\n");
            }
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_mem(
                (const char *)source.ptr, (size_t)source.size, mem, measure_inline_work, &work);
            OK(runner, root != NULL, "adversarial callouts parse");
            OK(runner, work.callout > 0 && work.callout <= 2 * (size_t)source.size,
               "callout recognition is linear: shape=%d bytes=%d work=%zu", shape, source.size, work.callout);
            if (root && shape == 0) {
                markdown_core_node *node = root->first_child;
                size_t depth = 1;
                while (node->first_child) {
                    node = node->first_child;
                    depth++;
                }
                INT_EQ(runner, depth, count, "no quote depth truncation");
                STR_EQ(runner, (const char *)node->as.callout->variant.value.data, "deep", "deepest metadata retained");
                OK(runner,
                   node->as.callout->title && node->as.callout->title->first_child->kind == MARKDOWN_CORE_NODE_MARK,
                   "deep title parsed through shared inlines");
            }
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static void block_identifier_linear_work(test_batch_runner *runner) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    static const struct {
        const char *prefix, *unit, *suffix;
        size_t anchors_per_unit, anchors_at_end;
    } cases[] = {
        {"text #", "a", "#", 0, 1},
        {"text #", "a", "_#", 0, 0},
        {"text ", "#", "", 0, 0},
        {"text ", "#a# ", "#last#", 0, 1},
        {"text ", "#a# ", "invalid", 0, 0},
        {"", "text #id#\n\n", "", 1, 0},
        {"text\n", "#id#\n", "", 0, 1},
        {"text\n", "    #id#\n", "", 0, 0},
        {"", "> quote\n\n#id#\n\n", "", 1, 0},
        {"", "- item\n\n#id#\nnext\n\n", "", 0, 0},
        {"- item\n\n#id#\n", "\n", "next", 0, 1},
        {"", "lead #id#\n| h |\n| - |\n\n", "", 1, 0},
        {"", "- item\n\n[ref]: /x\n\n#id#\n\n", "", 0, 0},
        {"", "> - item\n>\n> [ref]: /x\n>\n> #id#\n>\n", "", 0, 0},
        {"- item\n\n[ref]: /x\n", "\n", "#id#", 0, 0},
        {"- item\n\n", "[ref]: /x\n\n", "#id#", 0, 0},
        {"- item\n\n", "\n", "#id#", 0, 1},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        for (size_t count = 128; count <= 8192; count *= 2) {
            markdown_core_strbuf source = MARKDOWN_CORE_BUF_INIT(mem);
            markdown_core_strbuf_puts(&source, cases[c].prefix);
            for (size_t i = 0; i < count; i++) {
                markdown_core_strbuf_puts(&source, cases[c].unit);
            }
            markdown_core_strbuf_puts(&source, cases[c].suffix);
            inline_work work = {0};
            markdown_core_node *root = markdown_core_parse_document_with_mem((const char *)source.ptr, source.size, mem,
                                                                             measure_inline_work, &work);
            OK(runner, root != NULL, "block identifier adversary parses: case=%zu count=%zu", c, count);
            INT_EQ(runner, count_anchors(root), count * cases[c].anchors_per_unit + cases[c].anchors_at_end,
                   "every placement and fallback retains its semantics: case=%zu count=%zu", c, count);
            OK(runner, work.block_identifier + work.lookahead <= 4 * (size_t)source.size,
               "identifier and boundary work is linear: case=%zu bytes=%d scanner=%zu lookahead=%zu", c, source.size,
               work.block_identifier, work.lookahead);
            markdown_core_node_free(root);
            markdown_core_strbuf_free(&source);
        }
    }
}

static markdown_core_node *seed_anchor(const markdown_core_extension *extension, int indented,
                                       markdown_core_parser *parser, markdown_core_node *parent, unsigned char *input,
                                       int length) {
    (void)extension;
    (void)indented;
    (void)parent;
    (void)input;
    (void)length;
    markdown_core_node *owner = parser->current;
    if (parser->blank && owner->kind == MARKDOWN_CORE_NODE_PARAGRAPH) {
        if (owner->parent->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
            owner = owner->parent;
        }
        if (!markdown_core_chunk_set_cstr(parser->mem, &owner->attributes.anchor, "existing")) {
            parser->oom = true;
        }
    }
    return NULL;
}

static markdown_core_node *observe_definition_before_anchor(const markdown_core_extension *extension, int indented,
                                                            markdown_core_parser *parser, markdown_core_node *parent,
                                                            unsigned char *input, int length) {
    (void)extension;
    (void)indented;
    if (length - parser->first_nonspace >= 6 && memcmp(input + parser->first_nonspace, "#list#", 6) == 0) {
        markdown_core_node *previous = parent->last_child;
        *(bool *)parser->root->user_data = previous && previous->kind == MARKDOWN_CORE_NODE_PARAGRAPH &&
                                           !(previous->flags & MARKDOWN_CORE_NODE__OPEN) && previous->content.size == 0;
    }
    return NULL;
}

static bool observe_reference_definition_lifetime(markdown_core_parser *parser, void *context) {
    static const markdown_core_extension observer = {.name = "reference-definition-lifetime",
                                                     .try_opening_block = observe_definition_before_anchor};
    parser->root->user_data = context;
    return markdown_core_parser_attach_extension(parser, &observer);
}

static void reference_definition_lifetime(test_batch_runner *runner) {
    const char source[] = "- a\n\n[ref]: /x\n\n#list#\n\n[ref]\n";
    bool retained_at_anchor = false;
    markdown_core_node *root =
        markdown_core_parse_document_with_mem(source, sizeof(source) - 1, markdown_core_get_default_mem_allocator(),
                                              observe_reference_definition_lifetime, &retained_at_anchor);
    OK(runner, root != NULL, "deferred reference definition parses");
    OK(runner, retained_at_anchor, "the finalized definition remains a sibling when anchor syntax is reached");
    if (root) {
        markdown_core_node *marker = root->first_child ? root->first_child->next : NULL;
        OK(runner,
           marker && marker->kind == MARKDOWN_CORE_NODE_PARAGRAPH && marker->first_child &&
               marker->first_child->kind == MARKDOWN_CORE_NODE_TEXT &&
               strcmp(markdown_core_node_get_literal(marker->first_child), "#list#") == 0,
           "definition cleanup leaves the marker's paragraph as the next semantic sibling");
        OK(runner,
           marker && marker->next && marker->next->first_child &&
               marker->next->first_child->kind == MARKDOWN_CORE_NODE_LINK && !marker->next->next,
           "the definition resolves links without leaking a paragraph into the completed tree");
        root->user_data = NULL;
        markdown_core_node_free(root);
    }
}

static void block_identifier_ownership(test_batch_runner *runner) {
    static const markdown_core_extension seed = {.name = "preexisting-anchor", .try_opening_block = seed_anchor};
    const markdown_core_extension *probes[] = {&seed};
    const char *sources[] = {"text #candidate#\n\n", "- text #candidate#\n\n"};
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); i++) {
        markdown_core_node *root = parse_with_probes(sources[i], strlen(sources[i]), probes, 1);
        markdown_core_node *owner = root->first_child;
        if (owner->kind == MARKDOWN_CORE_NODE_LIST) {
            owner = owner->first_child;
        }
        STR_EQ(runner, (const char *)owner->attributes.anchor.data, "existing", "an existing anchor is never replaced");
        markdown_core_node *paragraph = owner->kind == MARKDOWN_CORE_NODE_PARAGRAPH ? owner : owner->first_child;
        STR_EQ(runner, markdown_core_node_get_literal(paragraph->first_child), "text #candidate#",
               "a refused attachment keeps the complete marker visible");
        INT_EQ(runner, count_anchors(root), 1, "a refused item anchor is not transferred to its paragraph");
        markdown_core_node_free(root);
    }
    const char *endings[] = {"\n", "\r\n", "\r"};
    for (size_t i = 0; i < sizeof(endings) / sizeof(*endings); i++) {
        for (int final_newline = 0; final_newline <= 1; final_newline++) {
            char source[80];
            snprintf(source, sizeof(source), "- text #item#%s%s#list#%s", endings[i], endings[i],
                     final_newline ? endings[i] : "");
            markdown_core_node *root = markdown_core_parse_document(source, strlen(source));
            markdown_core_node *list = root->first_child;
            markdown_core_node *item = list->first_child;
            memset(source, 'x', strlen(source));
            STR_EQ(runner, (const char *)list->attributes.anchor.data, "list",
                   "detached EOF identifier owns its bytes");
            STR_EQ(runner, (const char *)item->attributes.anchor.data, "item", "item identifier owns its bytes");
            OK(runner, list->attributes.anchor.alloc && item->attributes.anchor.alloc, "anchors are owned chunks");
            INT_EQ(runner, list->end_line, 3, "detached line belongs to the list scope for every line ending");
            INT_EQ(runner, list->end_column, 6, "list scope includes the closing hash at EOF");
            INT_EQ(runner, item->first_child->first_child->end_column, 6, "text scope ends before the separator");
            markdown_core_node_free(root);
        }
    }
}

int main(void) {
    int retval;
    test_batch_runner *runner = test_batch_runner_new();

    universal_values(runner);
    properties_values(runner);
    properties_source_boundaries(runner);
    properties_member_work(runner);
    properties_text_memory(runner);
    block_identifier_linear_work(runner);
    callout_linear_work(runner);
    image_dimension_linear_work(runner);
    block_identifier_ownership(runner);
    reference_definition_lifetime(runner);
    attribute_linear_work(runner);
    cross_link_linear_work(runner);
    inline_footnote_linear_work(runner);
    footnote_registration(runner);
    footnote_postprocessing(runner);
    literal_caret_allocations(runner);
    mark_linear_work(runner);
    insertion_linear_work(runner);
    comment_inline_linear_work(runner);
    comment_block_linear_work(runner);
    percent_comment_nodes(runner);
    cross_link_fields(runner);
    node_payload_lifecycle(runner);
    kind_conversion_containment(runner);
    version(runner);
    node_type_values(runner);
    constructor(runner);
    accessors(runner);
    formula_extension_accessors(runner);
    directive_extension_accessors(runner);
    node_check(runner);
    iterator(runner);
    iterator_delete(runner);
    create_tree(runner);
    hierarchy(runner);
    parser(runner);
    utf8(runner);
    line_endings(runner);
    numeric_entities(runner);
    test_cplusplus(runner);
    comment_nodes(runner);
    test_crlf_line_ending(runner);
    test_pathological_regressions(runner);
    extension_decline_yields_turn(runner);
    source_pos(runner);
    source_pos_inlines(runner);
    ref_source_pos(runner);
    link_resource_lifecycle(runner);
    block_cursor_coordinates(runner);
    task_marker_tab_structure(runner);
    task_marker_ownership(runner);
    specimen_values(runner);
    set_kind_keeps_extension_data_beside_the_arm(runner);
    citation_and_footnote_values(runner);
    autolink_source_pos(runner);
    table_source_map_growth(runner);
    table_values(runner);
    strbuf_overflow(runner);
    strbuf_failure_is_a_transaction(runner);
    stray_delimiter(runner);
    no_node_is_its_own_ancestor(runner);
    iterator_contract_is_total(runner);

    test_print_summary(runner);
    retval = test_ok(runner) ? 0 : 1;
    free(runner);

    return retval;
}
