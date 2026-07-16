#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "markdown-core.h"
#include "node.h"
#include "markdown-core-extensions.h"

#include <markdown_core.h>

#include "harness.h"
#include "cplusplus.h"

#define UTF8_REPL "\xEF\xBF\xBD"

static const markdown_core_node_type node_types[] = {
    MARKDOWN_CORE_NODE_DOCUMENT,  MARKDOWN_CORE_NODE_BLOCK_QUOTE, MARKDOWN_CORE_NODE_LIST,
    MARKDOWN_CORE_NODE_LIST_ITEM, MARKDOWN_CORE_NODE_CODE_BLOCK,  MARKDOWN_CORE_NODE_HTML_BLOCK,
    MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_HEADING,     MARKDOWN_CORE_NODE_THEMATIC_BREAK,
    MARKDOWN_CORE_NODE_TEXT,      MARKDOWN_CORE_NODE_SOFT_BREAK,  MARKDOWN_CORE_NODE_LINE_BREAK,
    MARKDOWN_CORE_NODE_CODE,      MARKDOWN_CORE_NODE_HTML,        MARKDOWN_CORE_NODE_EMPHASIS,
    MARKDOWN_CORE_NODE_STRONG,    MARKDOWN_CORE_NODE_LINK,        MARKDOWN_CORE_NODE_IMAGE};
static const char *const node_type_names[] = {"document",   "block_quote", "list",    "list_item",      "code_block",
                                              "html_block", "paragraph",   "heading", "thematic_break", "text",
                                              "soft_break", "line_break",  "code",    "html",           "emphasis",
                                              "strong",     "link",        "image"};
static const int num_node_types = sizeof(node_types) / sizeof(*node_types);

static void test_md_paragraph_text(test_batch_runner *runner, const char *markdown, const char *expected_text,
                                   const char *msg);

static void test_md_paragraph_text_options(test_batch_runner *runner, const char *markdown, size_t markdown_length,
                                           int options, const char *expected_text, const char *msg);

static markdown_core_node *parse_with_formula_extension(const char *markdown);
static markdown_core_node *parse_with_directive_extension(const char *markdown);

static void test_content(test_batch_runner *runner, markdown_core_node_type type, unsigned int *allowed_content);

static void test_char(test_batch_runner *runner, int valid, const char *utf8, const char *msg);

static void test_incomplete_char(test_batch_runner *runner, const char *utf8, const char *msg);

static void test_continuation_byte(test_batch_runner *runner, const char *utf8);

static void version(test_batch_runner *runner) {
    INT_EQ(runner, markdown_core_version(), MARKDOWN_CORE_VERSION, "markdown_core_version");
    STR_EQ(runner, markdown_core_version_string(), MARKDOWN_CORE_VERSION_STRING, "markdown_core_version_string");
}

static void node_type_values(test_batch_runner *runner) {
    static const markdown_core_node_type block_types[] = {
        MARKDOWN_CORE_NODE_DOCUMENT,           MARKDOWN_CORE_NODE_BLOCK_QUOTE, MARKDOWN_CORE_NODE_LIST,
        MARKDOWN_CORE_NODE_LIST_ITEM,          MARKDOWN_CORE_NODE_CODE_BLOCK,  MARKDOWN_CORE_NODE_HTML_BLOCK,
        MARKDOWN_CORE_NODE_PARAGRAPH,          MARKDOWN_CORE_NODE_HEADING,     MARKDOWN_CORE_NODE_THEMATIC_BREAK,
        MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION};
    static const markdown_core_node_type inline_types[] = {
        MARKDOWN_CORE_NODE_TEXT,       MARKDOWN_CORE_NODE_SOFT_BREAK,
        MARKDOWN_CORE_NODE_LINE_BREAK, MARKDOWN_CORE_NODE_CODE,
        MARKDOWN_CORE_NODE_HTML,       MARKDOWN_CORE_NODE_EMPHASIS,
        MARKDOWN_CORE_NODE_STRONG,     MARKDOWN_CORE_NODE_LINK,
        MARKDOWN_CORE_NODE_IMAGE,      MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE};

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

        switch (node->type) {
        case MARKDOWN_CORE_NODE_HEADING:
            INT_EQ(runner, markdown_core_node_get_heading_level(node), 1, "default heading level is 1");
            node->as.heading.level = 1;
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

    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1, MARKDOWN_CORE_OPT_DEFAULT);

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
    markdown_core_node *unclosed_doc =
        markdown_core_parse_document(unclosed_markdown, sizeof(unclosed_markdown) - 1, MARKDOWN_CORE_OPT_DEFAULT);
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
    STR_EQ(runner, markdown_core_node_get_url(link), "url", "get_url");
    STR_EQ(runner, markdown_core_node_get_title(link), "title", "get_title");

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

    OK(runner, markdown_core_node_set_url(link, "URL"), "set_url");
    OK(runner, markdown_core_node_set_title(link, "TITLE"), "set_title");

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
    STR_EQ(runner, markdown_core_node_get_url(link), "URL", "set_url applied");
    STR_EQ(runner, markdown_core_node_get_title(link), "TITLE", "set_title applied");
    STR_EQ(runner, markdown_core_node_get_literal(string), "LINK", "set_literal suffix applied");

    // Getter errors

    INT_EQ(runner, markdown_core_node_get_heading_level(bullet_list), 0, "get_heading_level error");
    INT_EQ(runner, markdown_core_node_get_list_type(heading), MARKDOWN_CORE_NO_LIST, "get_list_type error");
    INT_EQ(runner, markdown_core_node_get_list_start(code), 0, "get_list_start error");
    INT_EQ(runner, markdown_core_node_get_list_tight(fenced), 0, "get_list_tight error");
    OK(runner, markdown_core_node_get_literal(ordered_list) == NULL, "get_literal error");
    OK(runner, markdown_core_node_get_fence_info(paragraph) == NULL, "get_fence_info error");
    INT_EQ(runner, markdown_core_node_get_fence_closed(paragraph), 0, "get_fence_closed error");
    OK(runner, markdown_core_node_get_url(html) == NULL, "get_url error");
    OK(runner, markdown_core_node_get_title(heading) == NULL, "get_title error");

    // Setter errors

    OK(runner, !markdown_core_node_set_heading_level(bullet_list, 3), "set_heading_level error");
    OK(runner, !markdown_core_node_set_list_type(heading, MARKDOWN_CORE_ORDERED_LIST), "set_list_type error");
    OK(runner, !markdown_core_node_set_list_start(code, 3), "set_list_start error");
    OK(runner, !markdown_core_node_set_list_tight(fenced, 0), "set_list_tight error");
    OK(runner, !markdown_core_node_set_literal(ordered_list, "content\n"), "set_literal error");
    OK(runner, !markdown_core_node_set_fence_info(paragraph, "lang"), "set_fence_info error");
    OK(runner, !markdown_core_node_set_url(html, "url"), "set_url error");
    OK(runner, !markdown_core_node_set_title(heading, "title"), "set_title error");

    OK(runner, !markdown_core_node_set_heading_level(heading, 0), "set_heading_level too small");
    OK(runner, !markdown_core_node_set_heading_level(heading, 7), "set_heading_level too large");
    OK(runner, !markdown_core_node_set_list_type(bullet_list, MARKDOWN_CORE_NO_LIST), "set_list_type invalid");
    OK(runner, !markdown_core_node_set_list_start(bullet_list, -1), "set_list_start negative");

    markdown_core_node_free(doc);
}

static markdown_core_node *parse_with_formula_extension_options(const char *markdown, int options) {

    markdown_core_parser *parser = markdown_core_parser_new(options);
    markdown_core_extension *formula = markdown_core_find_extension("formula");

    if (formula) {
        markdown_core_parser_attach_extension(parser, formula);
    }

    markdown_core_parser_feed(parser, markdown, strlen(markdown));
    markdown_core_node *doc = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);

    return doc;
}

static markdown_core_node *parse_with_formula_extension(const char *markdown) {
    return parse_with_formula_extension_options(markdown, MARKDOWN_CORE_OPT_DEFAULT);
}

static markdown_core_node *parse_with_dollar_formula_extension(const char *markdown) {
    return parse_with_formula_extension_options(markdown, MARKDOWN_CORE_OPT_DEFAULT |
                                                              MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS);
}

static markdown_core_node *parse_with_directive_extension(const char *markdown) {

    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_DIRECTIVE);
    markdown_core_extension *directive = markdown_core_find_extension("directive");

    if (directive) {
        markdown_core_parser_attach_extension(parser, directive);
    }

    markdown_core_parser_feed(parser, markdown, strlen(markdown));
    markdown_core_node *doc = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);

    return doc;
}

static void formula_extension_accessors(test_batch_runner *runner) {
    markdown_core_node *doc = parse_with_formula_extension("Inline $x+y$ end.\n");
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *text = markdown_core_node_first_child(paragraph);

    INT_EQ(runner, markdown_core_node_get_type(text), MARKDOWN_CORE_NODE_TEXT,
           "dollar formula delimiters require opt-in");
    STR_EQ(runner, markdown_core_node_get_literal(text), "Inline $x+y$ end.",
           "dollar formula delimiter text remains literal without opt-in");
    markdown_core_node_free(doc);

    doc = parse_with_dollar_formula_extension("Inline $x+y$ end.\n");
    paragraph = markdown_core_node_first_child(doc);
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

    doc = parse_with_dollar_formula_extension("$$x+y$$\n");
    formula = markdown_core_node_first_child(doc);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "standalone formula block type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "standalone formula block literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula block mode is standalone");
    markdown_core_node_free(doc);

    doc = parse_with_dollar_formula_extension("Display $$a+b$$ end.\n");
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "standalone formula inline type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "a+b", "standalone formula inline literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "formula inline mode is standalone");
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension_options("Inline \\\\(x+y\\\\) end.\n",
                                               MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS);
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "LaTeX embedded formula inline type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y",
           "LaTeX embedded formula inline literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
           "LaTeX formula inline mode is embedded");
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension_options("Display \\\\[x+y\\\\] end.\n",
                                               MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS);
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula",
           "LaTeX standalone formula inline type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y",
           "LaTeX standalone formula inline literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "LaTeX formula inline mode is standalone");
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension_options("\\\\[x+y\\\\]\n",
                                               MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS);
    formula = markdown_core_node_first_child(doc);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "LaTeX standalone formula block type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y",
           "LaTeX standalone formula block literal");
    INT_EQ(runner, markdown_core_extensions_get_formula_mode(formula), MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
           "LaTeX formula block mode is standalone");
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension("```formula\nx+y\n```\n");
    formula = markdown_core_node_first_child(doc);
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula_block",
           "formula fence becomes standalone block");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "formula fence literal is trimmed");
    markdown_core_node_free(doc);
}

static void directive_extension_accessors(test_batch_runner *runner) {
    const char *source_attributes = "{\"id\":\"123\",\"muted\":\"true\",\"title\":\"My Video\","
                                    "\"bare\":\"\",\"dup\":\"last\",\"class\":\"blue\"}";
    markdown_core_node *doc =
        parse_with_directive_extension(":-a[]{id=first muted=true title=\"My Video\" bare dup=first dup=last "
                                       "class=red class=green class=blue id=123}\n");
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *directive = markdown_core_node_first_child(paragraph);
    const char *invalid_attributes[] = {
        "data-x=\"1\"",   "{\"x\":1}",           "{\"x\":true}",       "{\"x\":null}",
        "{\"x\":{}}",     "{\"x\":[]}",          "{\"x\":\"bad\\q\"}", "{\"x\":\"open}",
        "{\"x\":\"y\",}", "{\"x\":\"\\uD800\"}", "{\"x\"\f:\"y\"}",    "{\"x\":\"y\"}tail",
    };
    size_t i;

    STR_EQ(runner, markdown_core_node_get_type_string(directive), "directive", "directive inline type string");
    STR_EQ(runner, markdown_core_extensions_get_directive_name(directive), "-a", "directive name getter");
    STR_EQ(runner, markdown_core_extensions_get_directive_attributes(directive), source_attributes,
           "directive attribute list normalizes to string-map JSON");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "next_name-2"), 1,
           "set directive name succeeds");
    STR_EQ(runner, markdown_core_extensions_get_directive_name(directive), "next_name-2",
           "directive name setter updates payload");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "bad-"), 0,
           "set directive name rejects trailing hyphen");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, "bad_"), 0,
           "set directive name rejects trailing underscore");
    INT_EQ(runner, markdown_core_extensions_set_directive_name(directive, ""), 0,
           "set directive name rejects empty name");
    STR_EQ(runner, markdown_core_extensions_get_directive_name(directive), "next_name-2",
           "rejected directive name leaves payload unchanged");

    INT_EQ(runner,
           markdown_core_extensions_set_directive_attributes(
               directive, "{ \"class\" : \"ordinary\", \"empty\" : \"\", \"nul\" : \"\\u0000\", "
                          "\"dup\":\"first\", \"dup\":\"last\" }"),
           1, "set directive attributes from string-map JSON succeeds");
    STR_EQ(runner, markdown_core_extensions_get_directive_attributes(directive),
           "{\"class\":\"ordinary\",\"empty\":\"\",\"nul\":\"\\u0000\",\"dup\":\"last\"}",
           "directive attributes setter normalizes JSON and applies last duplicate");

    for (i = 0; i < sizeof(invalid_attributes) / sizeof(invalid_attributes[0]); i++) {
        INT_EQ(runner, markdown_core_extensions_set_directive_attributes(directive, invalid_attributes[i]), 0,
               "directive attributes setter rejects invalid string-map JSON");
        STR_EQ(runner, markdown_core_extensions_get_directive_attributes(directive),
               "{\"class\":\"ordinary\",\"empty\":\"\",\"nul\":\"\\u0000\",\"dup\":\"last\"}",
               "failed directive attributes setter is transactional");
    }

    INT_EQ(runner, markdown_core_extensions_set_directive_name(paragraph, "ok"), 0,
           "set directive name rejects non-directive nodes");
    INT_EQ(runner, markdown_core_extensions_set_directive_attributes(paragraph, "{\"data-x\":\"1\"}"), 0,
           "set directive attributes rejects non-directive nodes");
    OK(runner, markdown_core_extensions_get_directive_name(paragraph) == NULL,
       "get directive name rejects non-directive nodes");
    OK(runner, markdown_core_extensions_get_directive_attributes(paragraph) == NULL,
       "get directive attributes rejects non-directive nodes");
    markdown_core_node_free(doc);

    doc = parse_with_directive_extension(":plain[] :empty{}\n");
    paragraph = markdown_core_node_first_child(doc);
    directive = markdown_core_node_first_child(paragraph);
    OK(runner, markdown_core_extensions_get_directive_attributes(directive) == NULL,
       "missing directive attributes return NULL");
    directive = markdown_core_node_next(markdown_core_node_next(directive));
    STR_EQ(runner, markdown_core_extensions_get_directive_attributes(directive), "{}",
           "explicit empty directive attributes are preserved");
    markdown_core_node_free(doc);

    doc = parse_with_directive_extension(":a{data-x=1 class=ordinary}\n");
    paragraph = markdown_core_node_first_child(doc);
    directive = markdown_core_node_first_child(paragraph);
    STR_EQ(runner, markdown_core_extensions_get_directive_attributes(directive),
           "{\"data-x\":\"1\",\"class\":\"ordinary\"}", "directive attributes retain first-key source order");
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
    markdown_core_node *doc = markdown_core_parse_document("> a *b*\n\nc", 10, MARKDOWN_CORE_OPT_DEFAULT);
    int parnodes = 0;
    markdown_core_event_type ev_type;
    markdown_core_iter *iter = markdown_core_iter_new(doc);
    markdown_core_node *cur;

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (cur->type == MARKDOWN_CORE_NODE_PARAGRAPH && ev_type == MARKDOWN_CORE_EVENT_ENTER) {
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
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_iter *iter = markdown_core_iter_new(doc);
    markdown_core_event_type ev_type;

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        // Delete list, emph, and code nodes.
        if ((ev_type == MARKDOWN_CORE_EVENT_EXIT && node->type == MARKDOWN_CORE_NODE_LIST) ||
            (ev_type == MARKDOWN_CORE_EVENT_EXIT && node->type == MARKDOWN_CORE_NODE_EMPHASIS) ||
            (ev_type == MARKDOWN_CORE_EVENT_ENTER && node->type == MARKDOWN_CORE_NODE_CODE)) {
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
    markdown_core_node *bquote1 = markdown_core_node_new(MARKDOWN_CORE_NODE_BLOCK_QUOTE);
    markdown_core_node *bquote2 = markdown_core_node_new(MARKDOWN_CORE_NODE_BLOCK_QUOTE);
    markdown_core_node *bquote3 = markdown_core_node_new(MARKDOWN_CORE_NODE_BLOCK_QUOTE);

    OK(runner, markdown_core_node_append_child(bquote1, bquote2), "append bquote2");
    OK(runner, markdown_core_node_append_child(bquote2, bquote3), "append bquote3");
    OK(runner, !markdown_core_node_append_child(bquote3, bquote3), "adding a node as child of itself fails");
    OK(runner, !markdown_core_node_append_child(bquote3, bquote1), "adding a parent as child fails");

    markdown_core_node_free(bquote1);

    unsigned int list_item_flag[] = {MARKDOWN_CORE_NODE_LIST_ITEM, 0};
    unsigned int top_level_blocks[] = {MARKDOWN_CORE_NODE_BLOCK_QUOTE,    MARKDOWN_CORE_NODE_LIST,
                                       MARKDOWN_CORE_NODE_CODE_BLOCK,     MARKDOWN_CORE_NODE_HTML_BLOCK,
                                       MARKDOWN_CORE_NODE_PARAGRAPH,      MARKDOWN_CORE_NODE_HEADING,
                                       MARKDOWN_CORE_NODE_THEMATIC_BREAK, 0};
    unsigned int all_inlines[] = {MARKDOWN_CORE_NODE_TEXT,       MARKDOWN_CORE_NODE_SOFT_BREAK,
                                  MARKDOWN_CORE_NODE_LINE_BREAK, MARKDOWN_CORE_NODE_CODE,
                                  MARKDOWN_CORE_NODE_HTML,       MARKDOWN_CORE_NODE_EMPHASIS,
                                  MARKDOWN_CORE_NODE_STRONG,     MARKDOWN_CORE_NODE_LINK,
                                  MARKDOWN_CORE_NODE_IMAGE,      0};

    test_content(runner, MARKDOWN_CORE_NODE_DOCUMENT, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_BLOCK_QUOTE, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_LIST, list_item_flag);
    test_content(runner, MARKDOWN_CORE_NODE_LIST_ITEM, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_CODE_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_PARAGRAPH, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_HEADING, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_THEMATIC_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_TEXT, 0);
    test_content(runner, MARKDOWN_CORE_NODE_SOFT_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_LINE_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_CODE, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML, 0);
    test_content(runner, MARKDOWN_CORE_NODE_EMPHASIS, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_STRONG, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_LINK, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_IMAGE, all_inlines);
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
    // Ranges
    test_char(runner, 1, "\x01", "valid utf8 01");
    test_char(runner, 1, "\x7F", "valid utf8 7F");
    test_char(runner, 0, "\x80", "invalid utf8 80");
    test_char(runner, 0, "\xBF", "invalid utf8 BF");
    test_char(runner, 0, "\xC0\x80", "invalid utf8 C080");
    test_char(runner, 0, "\xC1\xBF", "invalid utf8 C1BF");
    test_char(runner, 1, "\xC2\x80", "valid utf8 C280");
    test_char(runner, 1, "\xDF\xBF", "valid utf8 DFBF");
    test_char(runner, 0, "\xE0\x80\x80", "invalid utf8 E08080");
    test_char(runner, 0, "\xE0\x9F\xBF", "invalid utf8 E09FBF");
    test_char(runner, 1, "\xE0\xA0\x80", "valid utf8 E0A080");
    test_char(runner, 1, "\xED\x9F\xBF", "valid utf8 ED9FBF");
    test_char(runner, 0, "\xED\xA0\x80", "invalid utf8 EDA080");
    test_char(runner, 0, "\xED\xBF\xBF", "invalid utf8 EDBFBF");
    test_char(runner, 0, "\xF0\x80\x80\x80", "invalid utf8 F0808080");
    test_char(runner, 0, "\xF0\x8F\xBF\xBF", "invalid utf8 F08FBFBF");
    test_char(runner, 1, "\xF0\x90\x80\x80", "valid utf8 F0908080");
    test_char(runner, 1, "\xF4\x8F\xBF\xBF", "valid utf8 F48FBFBF");
    test_char(runner, 0, "\xF4\x90\x80\x80", "invalid utf8 F4908080");
    test_char(runner, 0, "\xF7\xBF\xBF\xBF", "invalid utf8 F7BFBFBF");
    test_char(runner, 0, "\xF8", "invalid utf8 F8");
    test_char(runner, 0, "\xFF", "invalid utf8 FF");

    // Incomplete byte sequences at end of input
    test_incomplete_char(runner, "\xE0\xA0", "invalid utf8 E0A0");
    test_incomplete_char(runner, "\xF0\x90\x80", "invalid utf8 F09080");

    // Invalid continuation bytes
    test_continuation_byte(runner, "\xC2\x80");
    test_continuation_byte(runner, "\xE0\xA0\x80");
    test_continuation_byte(runner, "\xF0\x90\x80\x80");

    // Test string containing null character
    static const char string_with_null[] = "((((\0))))";
    test_md_paragraph_text_options(runner, string_with_null, sizeof(string_with_null) - 1, MARKDOWN_CORE_OPT_DEFAULT,
                                   "((((" UTF8_REPL "))))", "utf8 with U+0000");

    // Test NUL followed by newline
    static const char string_with_nul_lf[] = "```\n\0\n```\n";
    markdown_core_node *doc =
        markdown_core_parse_document(string_with_nul_lf, sizeof(string_with_nul_lf) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *code_block = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_type(code_block), MARKDOWN_CORE_NODE_CODE_BLOCK,
           "utf8 with \\0\\n parses a code block");
    STR_EQ(runner, markdown_core_node_get_literal(code_block), UTF8_REPL "\n", "utf8 with \\0\\n");
    markdown_core_node_free(doc);

    // Test byte-order marker
    static const char string_with_bom[] = "\xef\xbb\xbf# Hello\n";
    doc = markdown_core_parse_document(string_with_bom, sizeof(string_with_bom) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *heading = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_type(heading), MARKDOWN_CORE_NODE_HEADING, "utf8 with BOM parses a heading");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(heading)), "Hello", "utf8 with BOM");
    markdown_core_node_free(doc);
}

static void test_char(test_batch_runner *runner, int valid, const char *utf8, const char *msg) {
    char buf[20];
    snprintf(buf, sizeof(buf), "((((%s))))", utf8);

    if (valid) {
        char expected[30];
        snprintf(expected, sizeof(expected), "((((%s))))", utf8);
        test_md_paragraph_text(runner, buf, expected, msg);
    } else {
        test_md_paragraph_text(runner, buf, "((((" UTF8_REPL "))))", msg);
    }
}

static void test_incomplete_char(test_batch_runner *runner, const char *utf8, const char *msg) {
    char buf[20];
    snprintf(buf, sizeof(buf), "----%s", utf8);
    test_md_paragraph_text(runner, buf, "----" UTF8_REPL, msg);
}

static void test_continuation_byte(test_batch_runner *runner, const char *utf8) {
    size_t len = strlen(utf8);

    for (size_t pos = 1; pos < len; ++pos) {
        char buf[20];
        snprintf(buf, sizeof(buf), "((((%s))))", utf8);
        buf[4 + pos] = '\x20';

        char expected[50];
        strcpy(expected, "((((" UTF8_REPL "\x20");
        for (size_t i = pos + 1; i < len; ++i) {
            strcat(expected, UTF8_REPL);
        }
        strcat(expected, "))))");

        char msg[80];
        snprintf(msg, sizeof(msg), "invalid utf8 continuation byte %zu/%zu", pos, len);
        test_md_paragraph_text(runner, buf, expected, msg);
    }
}

static void line_endings(test_batch_runner *runner) {
    // Test list with different line endings
    static const char list_with_endings[] = "- a\n- b\r\n- c\r- d";
    static const char *const expected_items[] = {"a", "b", "c", "d"};
    markdown_core_node *doc =
        markdown_core_parse_document(list_with_endings, sizeof(list_with_endings) - 1, MARKDOWN_CORE_OPT_DEFAULT);
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

    // OPT_HARDBREAKS/OPT_NOBREAKS only changed the retired renderers; in the
    // AST a CRLF line ending is always a SoftBreak between the two texts.
    static const char crlf_lines[] = "line\r\nline\r\n";
    doc = markdown_core_parse_document(crlf_lines, sizeof(crlf_lines) - 1, MARKDOWN_CORE_OPT_DEFAULT);
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
    doc = markdown_core_parse_document(no_line_ending, sizeof(no_line_ending) - 1, MARKDOWN_CORE_OPT_DEFAULT);
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
    test_md_paragraph_text(runner, "&#x80000000;", UTF8_REPL, "Invalid numeric entity 0x80000000");
    test_md_paragraph_text(runner, "&#xFFFFFFFF;", UTF8_REPL, "Invalid numeric entity 0xFFFFFFFF");
    test_md_paragraph_text(runner, "&#99999999;", UTF8_REPL, "Invalid numeric entity 99999999");

    test_md_paragraph_text(runner, "&#;", "&#;", "Min decimal entity length");
    test_md_paragraph_text(runner, "&#x;", "&#x;", "Min hexadecimal entity length");
    test_md_paragraph_text(runner, "&#999999999;", "&#999999999;", "Max decimal entity length");
    test_md_paragraph_text(runner, "&#x000000041;", "&#x000000041;", "Max hexadecimal entity length");
}

static int count_html_comment_nodes(markdown_core_node *root) {
    int count = 0;
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER &&
            (node->type == MARKDOWN_CORE_NODE_HTML_BLOCK || node->type == MARKDOWN_CORE_NODE_HTML)) {
            const char *literal = markdown_core_node_get_literal(node);
            if (literal && strncmp(literal, "<!--", 4) == 0) {
                count++;
            }
        }
    }

    markdown_core_iter_free(iter);
    return count;
}

static void strip_html_comments(test_batch_runner *runner) {
    static const char markdown[] = "before <!-- hidden --> after <br>\n"
                                   "\n"
                                   "<!-- block\n"
                                   "hidden -->\n"
                                   "\n"
                                   "<div>raw</div>\n";

    markdown_core_node *doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    INT_EQ(runner, count_html_comment_nodes(doc), 2, "default parse preserves HTML comment nodes");
    markdown_core_node_free(doc);

    doc = markdown_core_parse_document(markdown, sizeof(markdown) - 1, MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS);
    INT_EQ(runner, count_html_comment_nodes(doc), 0, "strip-html-comments option removes HTML comment nodes");

    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *text = markdown_core_node_first_child(paragraph);
    STR_EQ(runner, markdown_core_node_get_literal(text), "before  after ",
           "strip-html-comments preserves surrounding text");

    markdown_core_node *inline_html = markdown_core_node_next(text);
    INT_EQ(runner, markdown_core_node_get_type(inline_html), MARKDOWN_CORE_NODE_HTML,
           "strip-html-comments preserves non-comment inline HTML");
    STR_EQ(runner, markdown_core_node_get_literal(inline_html), "<br>",
           "strip-html-comments keeps inline HTML literal");

    markdown_core_node *block_html = markdown_core_node_next(paragraph);
    INT_EQ(runner, markdown_core_node_get_type(block_html), MARKDOWN_CORE_NODE_HTML_BLOCK,
           "strip-html-comments preserves non-comment HTML blocks");
    STR_EQ(runner, markdown_core_node_get_literal(block_html), "<div>raw</div>\n",
           "strip-html-comments keeps block HTML literal");

    markdown_core_node_free(doc);
}

/* Parses and asserts the document is a single paragraph whose concatenated
 * Text literals equal `expected_text`.  This replaces the retired
 * markdown_to_html comparisons: AST literals carry raw bytes, without HTML
 * escaping. */
static void test_md_paragraph_text_options(test_batch_runner *runner, const char *markdown, size_t markdown_length,
                                           int options, const char *expected_text, const char *msg) {
    markdown_core_node *doc = markdown_core_parse_document(markdown, markdown_length, options);
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
    test_md_paragraph_text_options(runner, markdown, strlen(markdown), MARKDOWN_CORE_OPT_VALIDATE_UTF8, expected_text,
                                   msg);
}

static void test_feed_across_line_ending(test_batch_runner *runner) {
    // See #117
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_parser_feed(parser, "line1\r", 6);
    markdown_core_parser_feed(parser, "\nline2\r\n", 8);
    markdown_core_node *document = markdown_core_parser_finish(parser);
    OK(runner, document->first_child->next == NULL, "document has one paragraph");
    markdown_core_parser_free(parser);
    markdown_core_node_free(document);
}

#if !defined(_WIN32) || defined(__CYGWIN__)
#include <sys/time.h>
static struct timeval _before, _after;
static int _timing;
#define START_TIMING() gettimeofday(&_before, NULL)

#define END_TIMING()                                                                                                   \
    do {                                                                                                               \
        gettimeofday(&_after, NULL);                                                                                   \
        _timing = (_after.tv_sec - _before.tv_sec) * 1000 + (_after.tv_usec - _before.tv_usec) / 1000;                 \
    } while (0)

#define TIMING _timing
#else
#define START_TIMING()
#define END_TIMING()
#define TIMING 0
#endif

static void test_pathological_regressions(test_batch_runner *runner) {
    {
        // I don't care what the output is, so long as it doesn't take too long.
        char path[] = "[a](b";
        char *input = (char *)calloc(1, (sizeof(path) - 1) * 50000);
        for (int i = 0; i < 50000; ++i) {
            memcpy(input + i * (sizeof(path) - 1), path, sizeof(path) - 1);
        }

        START_TIMING();
        markdown_core_node *doc =
            markdown_core_parse_document(input, (sizeof(path) - 1) * 50000, MARKDOWN_CORE_OPT_VALIDATE_UTF8);
        END_TIMING();
        markdown_core_node_free(doc);
        free(input);

        OK(runner, TIMING < 1000, "takes less than 1000ms to run");
    }

    {
        char path[] = "[a](<b";
        char *input = (char *)calloc(1, (sizeof(path) - 1) * 50000);
        for (int i = 0; i < 50000; ++i) {
            memcpy(input + i * (sizeof(path) - 1), path, sizeof(path) - 1);
        }

        START_TIMING();
        markdown_core_node *doc =
            markdown_core_parse_document(input, (sizeof(path) - 1) * 50000, MARKDOWN_CORE_OPT_VALIDATE_UTF8);
        END_TIMING();
        markdown_core_node_free(doc);
        free(input);

        OK(runner, TIMING < 1000, "takes less than 1000ms to run");
    }
}

/* Parses through the read-only facade and compares the canonical AST dump,
 * which carries every node's scope, byte-for-byte.  This replaces the
 * retired sourcepos XML renderer assertions. */
static void test_facade_dump(test_batch_runner *runner, const char *markdown, int autolinks, const char *expected_dump,
                             const char *msg) {
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    uint8_t *dump = NULL;
    size_t dump_length = 0;

    memset(&options, 0, sizeof(options)); /* pure CommonMark; no smart punctuation */
    options.autolinks = autolinks != 0;
    document = markdown_core_document_parse((const uint8_t *)markdown, strlen(markdown), &options, &error);
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

    test_facade_dump(runner, markdown, 0,
                     "Document scope=1:1..10:20 children=3\n"
                     "├── Heading scope=1:1..1:13 level=1 children=3\n"
                     "│   ├── Text scope=1:3..1:5 literal=\"Hi \" children=0\n"
                     "│   ├── Emphasis scope=1:6..1:12 children=1\n"
                     "│   │   └── Text scope=1:7..1:11 literal=\"there\" children=0\n"
                     "│   └── Text scope=1:13..1:13 literal=\".\" children=0\n"
                     "├── Paragraph scope=3:1..4:42 children=8\n"
                     "│   ├── Text scope=3:1..3:14 literal=\"Hello “ \" children=0\n"
                     "│   ├── Link scope=3:15..3:37 destination=\"http://www.google.com\" "
                     "title=\"\" children=1\n"
                     "│   │   └── Text scope=3:16..3:36 literal=\"http://www.google.com\" "
                     "children=0\n"
                     "│   ├── SoftBreak scope=0:0..0:0 children=0\n"
                     "│   ├── Text scope=4:1..4:6 literal=\"there \" children=0\n"
                     "│   ├── Code scope=4:8..4:9 mode=embedded literal=\"hi\" children=0\n"
                     "│   ├── Text scope=4:11..4:14 literal=\" -- \" children=0\n"
                     "│   ├── Link scope=4:15..4:41 destination=\"www.google.com\" title=\"ok\" "
                     "children=1\n"
                     "│   │   └── Text scope=4:16..4:19 literal=\"okay\" children=0\n"
                     "│   └── Text scope=4:42..4:42 literal=\".\" children=0\n"
                     "└── BlockQuote scope=6:1..10:20 children=1\n"
                     "    └── List scope=6:3..10:20 flavor=ordered start=1 tight=false children=2\n"
                     "        ├── ListItem scope=6:3..8:1 checked=null children=1\n"
                     "        │   └── Paragraph scope=6:6..7:10 children=3\n"
                     "        │       ├── Text scope=6:6..6:10 literal=\"Okay.\" children=0\n"
                     "        │       ├── SoftBreak scope=0:0..0:0 children=0\n"
                     "        │       └── Text scope=7:6..7:10 literal=\"Sure.\" children=0\n"
                     "        └── ListItem scope=9:3..10:20 checked=null children=1\n"
                     "            └── Paragraph scope=9:6..10:20 children=3\n"
                     "                ├── Text scope=9:6..9:15 literal=\"Yes, okay.\" children=0\n"
                     "                ├── SoftBreak scope=0:0..0:0 children=0\n"
                     "                └── Image scope=10:6..10:20 source=\"hi\" title=\"yes\" "
                     "children=1\n"
                     "                    └── Text scope=10:8..10:9 literal=\"ok\" children=0\n",
                     "scopes are as expected");
}

static void source_pos_inlines(test_batch_runner *runner) {
    test_facade_dump(runner,
                     "*first*\n"
                     "second\n",
                     0,
                     "Document scope=1:1..2:6 children=1\n"
                     "└── Paragraph scope=1:1..2:6 children=3\n"
                     "    ├── Emphasis scope=1:1..1:7 children=1\n"
                     "    │   └── Text scope=1:2..1:6 literal=\"first\" children=0\n"
                     "    ├── SoftBreak scope=0:0..0:0 children=0\n"
                     "    └── Text scope=2:1..2:6 literal=\"second\" children=0\n",
                     "closed emphasis scopes are as expected");
    test_facade_dump(runner,
                     "*first\n"
                     "second*\n",
                     0,
                     "Document scope=1:1..2:7 children=1\n"
                     "└── Paragraph scope=1:1..2:7 children=1\n"
                     "    └── Emphasis scope=1:1..2:7 children=3\n"
                     "        ├── Text scope=1:2..1:6 literal=\"first\" children=0\n"
                     "        ├── SoftBreak scope=0:0..0:0 children=0\n"
                     "        └── Text scope=2:1..2:6 literal=\"second\" children=0\n",
                     "multiline emphasis scopes are as expected");
}

static void ref_source_pos(test_batch_runner *runner) {
    static const char markdown[] = "Let's try [reference] links.\n"
                                   "\n"
                                   "[reference]: https://github.com (GitHub)\n";

    test_facade_dump(runner, markdown, 0,
                     "Document scope=1:1..3:40 children=1\n"
                     "└── Paragraph scope=1:1..1:28 children=3\n"
                     "    ├── Text scope=1:1..1:10 literal=\"Let's try \" children=0\n"
                     "    ├── Link scope=1:11..1:21 destination=\"https://github.com\" "
                     "title=\"GitHub\" children=1\n"
                     "    │   └── Text scope=1:12..1:20 literal=\"reference\" children=0\n"
                     "    └── Text scope=1:22..1:28 literal=\" links.\" children=0\n",
                     "reference link scopes are as expected");
}

static void autolink_source_pos(test_batch_runner *runner) {
    test_facade_dump(runner, "See www.example.com.\n", 1,
                     "Document scope=1:1..1:20 children=1\n"
                     "└── Paragraph scope=1:1..1:20 children=3\n"
                     "    ├── Text scope=1:1..1:4 literal=\"See \" children=0\n"
                     "    ├── Link scope=1:5..1:19 destination=\"http://www.example.com\" "
                     "title=null children=1\n"
                     "    │   └── Text scope=1:5..1:19 literal=\"www.example.com\" children=0\n"
                     "    └── Text scope=1:20..1:20 literal=\".\" children=0\n",
                     "www autolink scopes are as expected");
    test_facade_dump(runner, "See http://example.com.\n", 1,
                     "Document scope=1:1..1:23 children=1\n"
                     "└── Paragraph scope=1:1..1:23 children=3\n"
                     "    ├── Text scope=1:1..1:4 literal=\"See \" children=0\n"
                     "    ├── Link scope=1:5..1:22 destination=\"http://example.com\" title=null "
                     "children=1\n"
                     "    │   └── Text scope=1:5..1:22 literal=\"http://example.com\" children=0\n"
                     "    └── Text scope=1:23..1:23 literal=\".\" children=0\n",
                     "scheme autolink scopes are as expected");
    test_facade_dump(runner, "http://example.com\n", 1,
                     "Document scope=1:1..1:18 children=1\n"
                     "└── Paragraph scope=1:1..1:18 children=2\n"
                     "    ├── Text scope=0:0..0:0 literal=\"\" children=0\n"
                     "    └── Link scope=1:1..1:18 destination=\"http://example.com\" title=null "
                     "children=1\n"
                     "        └── Text scope=1:1..1:18 literal=\"http://example.com\" children=0\n",
                     "scheme autolink at column one scopes are as expected");
    test_facade_dump(runner, "Mail user@example.com now.\n", 1,
                     "Document scope=1:1..1:26 children=1\n"
                     "└── Paragraph scope=1:1..1:26 children=3\n"
                     "    ├── Text scope=1:1..1:5 literal=\"Mail \" children=0\n"
                     "    ├── Link scope=1:6..1:21 destination=\"mailto:user@example.com\" "
                     "title=null children=1\n"
                     "    │   └── Text scope=1:6..1:21 literal=\"user@example.com\" children=0\n"
                     "    └── Text scope=1:22..1:26 literal=\" now.\" children=0\n",
                     "email autolink scopes are as expected");
}

/* ---------------- incremental sessions ---------------- */

static char *dump_document_cstr(const markdown_core_document *document) {
    uint8_t *dump = NULL;
    size_t length = 0;
    markdown_core_error *error = NULL;
    char *copy;
    if (!markdown_core_document_dump(document, &dump, &length, &error)) {
        markdown_core_error_free(error);
        return NULL;
    }
    copy = (char *)malloc(length + 1);
    if (copy) {
        memcpy(copy, dump, length);
        copy[length] = 0;
    }
    markdown_core_dump_free(dump);
    return copy;
}

static int changeset_contains(const markdown_core_node_id *ids, size_t count, markdown_core_node_id id) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (ids[i] == id) {
            return 1;
        }
    }
    return 0;
}

static const char SESSION_RICH_SOURCE[] = "# Title\n"
                                          "\n"
                                          "Intro with *emphasis*, **strong**, ~~gone~~, `code`, a [ref][label],\n"
                                          "https://example.com autolink, $x^2$ formula, and :note[hi]{k=v}.\n"
                                          "\n"
                                          "- [ ] task one\n"
                                          "- [x] task two\n"
                                          "\n"
                                          "| a | b |\n"
                                          "| --- | ---: |\n"
                                          "| 1 | 2 |\n"
                                          "\n"
                                          "```c\n"
                                          "int main(void);\n"
                                          "```\n"
                                          "\n"
                                          "> quoted with a footnote[^fn]\n"
                                          "\n"
                                          "$$\n"
                                          "\\sum_i i\n"
                                          "$$\n"
                                          "\n"
                                          "[label]: https://example.org \"t\"\n"
                                          "\n"
                                          "[^fn]: footnote body\n";

static void session_streaming_equivalence(test_batch_runner *runner) {
    markdown_core_error *error = NULL;
    markdown_core_document *reference =
        markdown_core_document_parse((const uint8_t *)SESSION_RICH_SOURCE, strlen(SESSION_RICH_SOURCE), NULL, &error);
    char *expected = reference ? dump_document_cstr(reference) : NULL;
    markdown_core_session *session = markdown_core_session_open(NULL, &error);
    size_t length = strlen(SESSION_RICH_SOURCE);
    size_t offset;
    int all_edits_ok = 1, all_commits_ok = 1;

    OK(runner, reference != NULL && expected != NULL, "session equivalence reference parse");
    OK(runner, session != NULL, "session_open succeeds");
    if (!reference || !expected || !session) {
        goto cleanup;
    }

    /* Byte-at-a-time token stream with a commit after every byte. */
    for (offset = 0; offset < length; offset++) {
        if (!markdown_core_session_edit(session, offset, offset, (const uint8_t *)SESSION_RICH_SOURCE + offset, 1,
                                        &error)) {
            all_edits_ok = 0;
        }
        if (!markdown_core_session_commit(session, NULL, &error)) {
            all_commits_ok = 0;
        }
    }
    OK(runner, all_edits_ok, "per-byte appends all succeed");
    OK(runner, all_commits_ok, "per-byte commits all succeed");
    INT_EQ(runner, (int)markdown_core_session_length(session), (int)length, "session_length tracks the text");

    {
        char *streamed = dump_document_cstr(markdown_core_session_document(session));
        OK(runner, streamed != NULL, "session dump succeeds");
        if (streamed) {
            STR_EQ(runner, streamed, expected, "byte-streamed session dump equals one-shot dump");
        }
        free(streamed);
    }

cleanup:
    free(expected);
    markdown_core_document_free(reference);
    markdown_core_session_free(session);
    markdown_core_error_free(error);
}

static void session_append_id_stability(test_batch_runner *runner) {
    markdown_core_error *error = NULL;
    markdown_core_session *session = markdown_core_session_open(NULL, &error);
    const char *part_one = "# Title\n\nHello ";
    const char *part_two = "world **bold**";
    markdown_core_node_id heading_id, paragraph_id, text_id, root_id;
    uint64_t heading_rev, root_rev_before;
    markdown_core_changeset *changes = NULL;

    OK(runner, session != NULL, "id-stability session opens");
    if (!session) {
        return;
    }

    markdown_core_session_edit(session, 0, 0, (const uint8_t *)part_one, strlen(part_one), &error);
    OK(runner, markdown_core_session_commit(session, NULL, &error), "first commit succeeds");

    {
        const markdown_core_node *root = markdown_core_document_root(markdown_core_session_document(session));
        const markdown_core_node *heading = markdown_core_node_get_first_child(root);
        const markdown_core_node *paragraph = markdown_core_node_get_next_sibling(heading);
        const markdown_core_node *text = markdown_core_node_get_first_child(paragraph);
        root_id = markdown_core_node_get_id(root);
        heading_id = markdown_core_node_get_id(heading);
        heading_rev = markdown_core_node_get_revision(heading);
        paragraph_id = markdown_core_node_get_id(paragraph);
        text_id = markdown_core_node_get_id(text);
        root_rev_before = markdown_core_node_get_revision(root);
        OK(runner, heading_id != 0 && paragraph_id != 0 && text_id != 0, "committed nodes carry nonzero ids");
        OK(runner, markdown_core_node_get_parent(heading) == root, "node_get_parent reaches the root");
    }

    markdown_core_session_edit(session, markdown_core_session_length(session), markdown_core_session_length(session),
                               (const uint8_t *)part_two, strlen(part_two), &error);
    OK(runner, markdown_core_session_commit(session, &changes, &error), "second commit succeeds");
    OK(runner, changes != NULL, "changeset is produced on request");

    {
        const markdown_core_node *root = markdown_core_document_root(markdown_core_session_document(session));
        const markdown_core_node *heading = markdown_core_node_get_first_child(root);
        const markdown_core_node *paragraph = markdown_core_node_get_next_sibling(heading);
        const markdown_core_node *text = markdown_core_node_get_first_child(paragraph);
        const markdown_core_node *strong = markdown_core_node_get_next_sibling(text);
        uint64_t before = 0, after = 0;
        const markdown_core_node_id *ids;
        size_t count;

        OK(runner, markdown_core_node_get_id(heading) == heading_id, "frontier append keeps the heading id");
        OK(runner, markdown_core_node_get_revision(heading) == heading_rev, "untouched heading keeps its revision");
        OK(runner, markdown_core_node_get_id(paragraph) == paragraph_id, "open paragraph keeps its id");
        OK(runner, markdown_core_node_get_id(text) == text_id, "trailing text keeps its id");
        OK(runner, strong != NULL && markdown_core_node_get_kind(strong) == MARKDOWN_CORE_KIND_STRONG,
           "appended strong exists");

        markdown_core_changeset_revisions(changes, &before, &after);
        OK(runner, before + 1 == after, "changeset revisions are consecutive");
        OK(runner, markdown_core_session_revision(session) == after, "session revision matches the changeset");

        count = markdown_core_changeset_changed(changes, &ids);
        OK(runner, changeset_contains(ids, count, paragraph_id), "paragraph is reported changed");
        OK(runner, changeset_contains(ids, count, text_id), "grown text is reported changed");
        OK(runner, !changeset_contains(ids, count, heading_id), "heading is not reported changed");
        count = markdown_core_changeset_added(changes, &ids);
        OK(runner, changeset_contains(ids, count, markdown_core_node_get_id(strong)), "strong is reported added");
        count = markdown_core_changeset_bubbled(changes, &ids);
        OK(runner, changeset_contains(ids, count, root_id), "root revision bubbles");
        count = markdown_core_changeset_removed(changes, &ids);
        INT_EQ(runner, (int)count, 0, "append removes nothing");

        OK(runner, markdown_core_node_get_revision(root) == after && root_rev_before < after,
           "root revision advances by bubbling");
        OK(runner, markdown_core_session_node_by_id(session, paragraph_id) == paragraph,
           "node_by_id resolves the paragraph");
        OK(runner, markdown_core_session_node_by_id(session, 0) == NULL, "node_by_id rejects id 0");
        OK(runner, markdown_core_session_lineage(session) != 0, "session lineage is nonzero");
    }

    markdown_core_changeset_free(changes);
    markdown_core_session_free(session);
    markdown_core_error_free(error);
}

static void session_suffix_id_stability(test_batch_runner *runner) {
    markdown_core_error *error = NULL;
    markdown_core_session *session = markdown_core_session_open(NULL, &error);
    const char *source = "para one\n\npara two\n\npara three\n";
    markdown_core_node_id ids[3];
    uint64_t revs[3];

    OK(runner, session != NULL, "suffix-stability session opens");
    if (!session) {
        return;
    }
    markdown_core_session_edit(session, 0, 0, (const uint8_t *)source, strlen(source), &error);
    OK(runner, markdown_core_session_commit(session, NULL, &error), "suffix baseline commit succeeds");

    {
        const markdown_core_node *root = markdown_core_document_root(markdown_core_session_document(session));
        const markdown_core_node *child = markdown_core_node_get_first_child(root);
        int i;
        for (i = 0; i < 3 && child; i++) {
            ids[i] = markdown_core_node_get_id(child);
            revs[i] = markdown_core_node_get_revision(child);
            child = markdown_core_node_get_next_sibling(child);
        }
    }

    /* Replace "one" (bytes 5..8) with "1!" — only the first paragraph. */
    markdown_core_session_edit(session, 5, 8, (const uint8_t *)"1!", 2, &error);
    OK(runner, markdown_core_session_commit(session, NULL, &error), "mid-document edit commit succeeds");

    {
        const markdown_core_node *root = markdown_core_document_root(markdown_core_session_document(session));
        const markdown_core_node *first = markdown_core_node_get_first_child(root);
        const markdown_core_node *second = markdown_core_node_get_next_sibling(first);
        const markdown_core_node *third = markdown_core_node_get_next_sibling(second);
        OK(runner, markdown_core_node_get_id(first) == ids[0], "edited paragraph keeps its id");
        OK(runner, markdown_core_node_get_revision(first) > revs[0], "edited paragraph revision advances");
        OK(runner, markdown_core_node_get_id(second) == ids[1] && markdown_core_node_get_revision(second) == revs[1],
           "second paragraph is untouched");
        OK(runner, markdown_core_node_get_id(third) == ids[2] && markdown_core_node_get_revision(third) == revs[2],
           "third paragraph is untouched");
    }

    markdown_core_session_free(session);
    markdown_core_error_free(error);
}

static void session_utf8_split_append(test_batch_runner *runner) {
    /* A streamed token may split a multi-byte character; the completing
     * append must yield the same tree as a one-shot parse. */
    static const uint8_t euro_doc[] = {'p', ' ', 0xE2, 0x82, 0xAC, '\n'};
    markdown_core_error *error = NULL;
    markdown_core_document *reference = markdown_core_document_parse(euro_doc, sizeof(euro_doc), NULL, &error);
    char *expected = reference ? dump_document_cstr(reference) : NULL;
    markdown_core_session *session = markdown_core_session_open(NULL, &error);

    OK(runner, session != NULL && expected != NULL, "utf8-split session and reference exist");
    if (session && expected) {
        char *streamed;
        markdown_core_session_edit(session, 0, 0, euro_doc, 3, &error); /* 'p', ' ', 0xE2 */
        OK(runner, markdown_core_session_commit(session, NULL, &error), "commit with a dangling lead byte succeeds");
        markdown_core_session_edit(session, 3, 3, euro_doc + 3, 3, &error);
        OK(runner, markdown_core_session_commit(session, NULL, &error), "completing commit succeeds");
        streamed = dump_document_cstr(markdown_core_session_document(session));
        if (streamed) {
            STR_EQ(runner, streamed, expected, "split multi-byte character parses whole");
        }
        free(streamed);
    }
    free(expected);
    markdown_core_document_free(reference);
    markdown_core_session_free(session);
    markdown_core_error_free(error);
}

static void session_edit_errors(test_batch_runner *runner) {
    markdown_core_error *error = NULL;
    markdown_core_session *session = markdown_core_session_open(NULL, &error);

    OK(runner, session != NULL, "error-case session opens");
    if (!session) {
        return;
    }

    OK(runner, markdown_core_session_revision(session) == 0, "fresh session is at revision 0");
    {
        const markdown_core_document *view = markdown_core_session_document(session);
        const markdown_core_node *root = markdown_core_document_root(view);
        OK(runner, root != NULL && markdown_core_node_child_count(root) == 0, "fresh session document is empty");
        OK(runner, markdown_core_node_get_id(root) != 0, "fresh root already has an id");
    }

    OK(runner, !markdown_core_session_edit(session, 5, 2, NULL, 0, &error), "inverted range is rejected");
    INT_EQ(runner, (int)markdown_core_error_get_code(error), (int)MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
           "inverted range reports invalid argument");
    markdown_core_error_free(error);
    error = NULL;

    OK(runner, !markdown_core_session_edit(session, 0, 1, NULL, 0, &error), "out-of-range end is rejected");
    markdown_core_error_free(error);
    error = NULL;

    OK(runner, !markdown_core_session_edit(session, 0, 0, NULL, 3, &error), "null bytes with length are rejected");
    markdown_core_error_free(error);
    error = NULL;

    /* An edit whose total length would overflow size_t must fail cleanly
     * before any byte of the (impossible) source buffer is read. */
    markdown_core_session_edit(session, 0, 0, (const uint8_t *)"x", 1, &error);
    OK(runner, !markdown_core_session_edit(session, 1, 1, (const uint8_t *)"y", (size_t)-1, &error),
       "overflowing edit length is rejected");
    markdown_core_error_free(error);
    error = NULL;
    OK(runner, markdown_core_session_length(session) == 1, "rejected edit leaves the text unchanged");

    markdown_core_session_free(session);
}

static void session_directive_label_parent(test_batch_runner *runner) {
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)":video[watch me]{k=v}\n",
                                                                    strlen(":video[watch me]{k=v}\n"), NULL, &error);
    OK(runner, document != NULL, "directive document parses");
    if (document) {
        const markdown_core_node *root = markdown_core_document_root(document);
        const markdown_core_node *paragraph = markdown_core_node_get_first_child(root);
        const markdown_core_node *directive = markdown_core_node_get_first_child(paragraph);
        const markdown_core_node *label = markdown_core_node_directive_first_label_child(directive);
        OK(runner, directive != NULL && markdown_core_node_get_kind(directive) == MARKDOWN_CORE_KIND_DIRECTIVE,
           "directive node found");
        OK(runner, label != NULL, "directive label child exists");
        OK(runner, markdown_core_node_get_parent(label) == directive,
           "label child's canonical parent is the directive");
        OK(runner, markdown_core_node_get_parent(root) == NULL, "root has no parent");
        OK(runner, markdown_core_node_get_id(directive) != 0, "one-shot documents carry ids");
    }
    markdown_core_document_free(document);
    markdown_core_error_free(error);
}

int main(void) {
    int retval;
    test_batch_runner *runner = test_batch_runner_new();

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
    strip_html_comments(runner);
    test_feed_across_line_ending(runner);
    test_pathological_regressions(runner);
    source_pos(runner);
    source_pos_inlines(runner);
    ref_source_pos(runner);
    autolink_source_pos(runner);
    session_streaming_equivalence(runner);
    session_append_id_stability(runner);
    session_suffix_id_stability(runner);
    session_utf8_split_append(runner);
    session_edit_errors(runner);
    session_directive_label_parent(runner);

    test_print_summary(runner);
    retval = test_ok(runner) ? 0 : 1;
    free(runner);

    return retval;
}
