#include <stdio.h>
#include "table.h"
#include "formula.h"
#include "directive.h"
#include <stdlib.h>
#include <string.h>

#include "markdown-core.h"
#include "node.h"
#include "buffer.h"
#include "syntax_extension.h"
#include "markdown-core-extensions.h"

#include <markdown_core.h>

#include "harness.h"
#include "cplusplus.h"

#define UTF8_REPL "\xEF\xBF\xBD"

static const markdown_core_node_type node_types[] = {
    MARKDOWN_CORE_NODE_DOCUMENT,
    MARKDOWN_CORE_NODE_BLOCK_QUOTE,
    MARKDOWN_CORE_NODE_LIST,
    MARKDOWN_CORE_NODE_LIST_ITEM,
    MARKDOWN_CORE_NODE_CODE_BLOCK,
    MARKDOWN_CORE_NODE_HTML_BLOCK,
    MARKDOWN_CORE_NODE_PARAGRAPH,
    MARKDOWN_CORE_NODE_HEADING,
    MARKDOWN_CORE_NODE_THEMATIC_BREAK,
    MARKDOWN_CORE_NODE_TEXT,
    MARKDOWN_CORE_NODE_SOFT_BREAK,
    MARKDOWN_CORE_NODE_LINE_BREAK,
    MARKDOWN_CORE_NODE_CODE,
    MARKDOWN_CORE_NODE_HTML,
    MARKDOWN_CORE_NODE_EMPHASIS,
    MARKDOWN_CORE_NODE_STRONG,
    MARKDOWN_CORE_NODE_LINK,
    MARKDOWN_CORE_NODE_IMAGE,
    MARKDOWN_CORE_NODE_REFERENCE_DEFINITION,
    MARKDOWN_CORE_NODE_LINK_REFERENCE,
    MARKDOWN_CORE_NODE_IMAGE_REFERENCE
};
static const char *const node_type_names[] = {
    "document",
    "block_quote",
    "list",
    "list_item",
    "code_block",
    "html_block",
    "paragraph",
    "heading",
    "thematic_break",
    "text",
    "soft_break",
    "line_break",
    "code",
    "html",
    "emphasis",
    "strong",
    "link",
    "image",
    "reference_definition",
    "link_reference",
    "image_reference"
};
static const int num_node_types = sizeof(node_types) / sizeof(*node_types);

static void test_md_paragraph_text(
    test_batch_runner *runner,
    const char *markdown,
    const char *expected_text,
    const char *msg
);

static void test_md_paragraph_text_options(
    test_batch_runner *runner,
    const char *markdown,
    size_t markdown_length,
    int options,
    const char *expected_text,
    const char *msg
);

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

/* The extension types continue these two sequences, so listing them here means
 * the existing contiguity assertions pin every one of the nine values AND make
 * a collision or a gap impossible. Until Step 3.1 they were globals filled in
 * by `markdown_core_syntax_extension_add_node` in whatever order
 * `core_extensions_registration` called the `create_*` functions, and nothing
 * in the repository asserted a single one of them. */
static void node_type_values(test_batch_runner *runner) {
    static const markdown_core_node_type block_types[] = {
        MARKDOWN_CORE_NODE_DOCUMENT,
        MARKDOWN_CORE_NODE_BLOCK_QUOTE,
        MARKDOWN_CORE_NODE_LIST,
        MARKDOWN_CORE_NODE_LIST_ITEM,
        MARKDOWN_CORE_NODE_CODE_BLOCK,
        MARKDOWN_CORE_NODE_HTML_BLOCK,
        MARKDOWN_CORE_NODE_PARAGRAPH,
        MARKDOWN_CORE_NODE_HEADING,
        MARKDOWN_CORE_NODE_THEMATIC_BREAK,
        MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION,
        MARKDOWN_CORE_NODE_TABLE,
        MARKDOWN_CORE_NODE_TABLE_ROW,
        MARKDOWN_CORE_NODE_TABLE_CELL,
        MARKDOWN_CORE_NODE_FORMULA_BLOCK,
        MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
        MARKDOWN_CORE_NODE_REFERENCE_DEFINITION
    };
    static const markdown_core_node_type inline_types[] = {
        MARKDOWN_CORE_NODE_TEXT,
        MARKDOWN_CORE_NODE_SOFT_BREAK,
        MARKDOWN_CORE_NODE_LINE_BREAK,
        MARKDOWN_CORE_NODE_CODE,
        MARKDOWN_CORE_NODE_HTML,
        MARKDOWN_CORE_NODE_EMPHASIS,
        MARKDOWN_CORE_NODE_STRONG,
        MARKDOWN_CORE_NODE_LINK,
        MARKDOWN_CORE_NODE_IMAGE,
        MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE,
        MARKDOWN_CORE_NODE_STRIKETHROUGH,
        MARKDOWN_CORE_NODE_FORMULA,
        MARKDOWN_CORE_NODE_DIRECTIVE,
        MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
        MARKDOWN_CORE_NODE_LINK_REFERENCE,
        MARKDOWN_CORE_NODE_IMAGE_REFERENCE
    };

    for (size_t i = 0; i < sizeof(block_types) / sizeof(*block_types); ++i) {
        INT_EQ(
            runner,
            block_types[i] & MARKDOWN_CORE_NODE_TYPE_MASK,
            MARKDOWN_CORE_NODE_TYPE_BLOCK,
            "block node type class %zu",
            i
        );
        INT_EQ(runner, block_types[i] & MARKDOWN_CORE_NODE_VALUE_MASK, i + 1, "block node type value %zu", i);
    }

    for (size_t i = 0; i < sizeof(inline_types) / sizeof(*inline_types); ++i) {
        INT_EQ(
            runner,
            inline_types[i] & MARKDOWN_CORE_NODE_TYPE_MASK,
            MARKDOWN_CORE_NODE_TYPE_INLINE,
            "inline node type class %zu",
            i
        );
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
            INT_EQ(
                runner,
                markdown_core_node_get_list_type(node),
                MARKDOWN_CORE_BULLET_LIST,
                "default is list type is bullet"
            );
            INT_EQ(
                runner,
                markdown_core_node_get_list_delim(node),
                MARKDOWN_CORE_NO_DELIM,
                "default is list delim is NO_DELIM"
            );
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
    INT_EQ(
        runner,
        markdown_core_node_get_list_delim(ordered_list),
        MARKDOWN_CORE_PERIOD_DELIM,
        "get_list_delim ordered"
    );
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
    INT_EQ(
        runner,
        markdown_core_node_get_list_type(ordered_list),
        MARKDOWN_CORE_BULLET_LIST,
        "set_list_type bullet applied"
    );
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
    const markdown_core_syntax_extension *formula = &MARKDOWN_CORE_EXTENSION_FORMULA;

    if (formula) {
        markdown_core_parser_attach_syntax_extension(parser, formula);
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
    return parse_with_formula_extension_options(markdown, MARKDOWN_CORE_OPT_DEFAULT);
}

static markdown_core_node *parse_with_directive_extension(const char *markdown) {

    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    const markdown_core_syntax_extension *directive = &MARKDOWN_CORE_EXTENSION_DIRECTIVE;

    if (directive) {
        markdown_core_parser_attach_syntax_extension(parser, directive);
    }

    markdown_core_parser_feed(parser, markdown, strlen(markdown));
    markdown_core_node *doc = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);

    return doc;
}

/* ATTACHING THE EXTENSION IS THE ONLY GATE (Q14, Step 6). These two assertions
 * used to say the opposite -- "dollar formula delimiters require opt-in" -- and
 * they passed because `MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS` existed and
 * this parser did not set it. There is no such option now, so a parser with the
 * extension attached parses the syntax and a parser without it does not, and
 * nothing in between is expressible. */
static void formula_extension_accessors(test_batch_runner *runner) {
    markdown_core_node *doc = markdown_core_parse_document("Inline $x+y$ end.\n", 18, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *text = markdown_core_node_first_child(paragraph);

    INT_EQ(
        runner,
        markdown_core_node_get_type(text),
        MARKDOWN_CORE_NODE_TEXT,
        "without the extension attached, dollar syntax is ordinary text"
    );
    STR_EQ(
        runner,
        markdown_core_node_get_literal(text),
        "Inline $x+y$ end.",
        "and the text keeps every byte the author wrote"
    );
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension("Inline $x+y$ end.\n");
    paragraph = markdown_core_node_first_child(doc);
    OK(runner,
        markdown_core_node_get_type(markdown_core_node_next(markdown_core_node_first_child(paragraph))) ==
            MARKDOWN_CORE_NODE_FORMULA,
        "attaching the extension is the whole gate");
    markdown_core_node_free(doc);

    doc = parse_with_dollar_formula_extension("Inline $x+y$ end.\n");
    paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));

    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "formula type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "formula inline literal");
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(formula),
        MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
        "formula inline mode is embedded"
    );
    INT_EQ(runner, markdown_core_extensions_set_formula_literal(formula, "z"), 1, "set formula literal succeeds");
    STR_EQ(
        runner,
        markdown_core_extensions_get_formula_literal(formula),
        "z",
        "formula literal setter updates payload"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_formula_mode(formula, MARKDOWN_CORE_FORMULA_MODE_STANDALONE),
        1,
        "set formula mode succeeds"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(formula),
        MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
        "formula mode setter updates mode"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_formula_literal(paragraph, "nope"),
        0,
        "set formula literal rejects non-formula nodes"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_formula_mode(paragraph, MARKDOWN_CORE_FORMULA_MODE_EMBEDDED),
        0,
        "set formula mode rejects non-formula nodes"
    );
    OK(runner,
        markdown_core_extensions_get_formula_literal(paragraph) == NULL,
        "get formula literal rejects non-formula nodes");
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(paragraph),
        MARKDOWN_CORE_FORMULA_MODE_NONE,
        "get formula mode rejects non-formula nodes"
    );
    markdown_core_node_free(doc);

    doc = parse_with_dollar_formula_extension("$$x+y$$\n");
    formula = markdown_core_node_first_child(doc);
    STR_EQ(
        runner,
        markdown_core_node_get_type_string(formula),
        "formula_block",
        "standalone formula block type string"
    );
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "standalone formula block literal");
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(formula),
        MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
        "formula block mode is standalone"
    );
    markdown_core_node_free(doc);

    doc = parse_with_dollar_formula_extension("Display $$a+b$$ end.\n");
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "standalone formula inline type string");
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "a+b", "standalone formula inline literal");
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(formula),
        MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
        "formula inline mode is standalone"
    );
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension_options("Inline \\\\(x+y\\\\) end.\n", MARKDOWN_CORE_OPT_DEFAULT);
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(runner, markdown_core_node_get_type_string(formula), "formula", "LaTeX embedded formula inline type string");
    STR_EQ(
        runner,
        markdown_core_extensions_get_formula_literal(formula),
        "x+y",
        "LaTeX embedded formula inline literal"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(formula),
        MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
        "LaTeX formula inline mode is embedded"
    );
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension_options("Display \\\\[x+y\\\\] end.\n", MARKDOWN_CORE_OPT_DEFAULT);
    paragraph = markdown_core_node_first_child(doc);
    formula = markdown_core_node_next(markdown_core_node_first_child(paragraph));
    STR_EQ(
        runner,
        markdown_core_node_get_type_string(formula),
        "formula",
        "LaTeX standalone formula inline type string"
    );
    STR_EQ(
        runner,
        markdown_core_extensions_get_formula_literal(formula),
        "x+y",
        "LaTeX standalone formula inline literal"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(formula),
        MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
        "LaTeX formula inline mode is standalone"
    );
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension_options("\\\\[x+y\\\\]\n", MARKDOWN_CORE_OPT_DEFAULT);
    formula = markdown_core_node_first_child(doc);
    STR_EQ(
        runner,
        markdown_core_node_get_type_string(formula),
        "formula_block",
        "LaTeX standalone formula block type string"
    );
    STR_EQ(
        runner,
        markdown_core_extensions_get_formula_literal(formula),
        "x+y",
        "LaTeX standalone formula block literal"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_get_formula_mode(formula),
        MARKDOWN_CORE_FORMULA_MODE_STANDALONE,
        "LaTeX formula block mode is standalone"
    );
    markdown_core_node_free(doc);

    doc = parse_with_formula_extension("```formula\nx+y\n```\n");
    formula = markdown_core_node_first_child(doc);
    STR_EQ(
        runner,
        markdown_core_node_get_type_string(formula),
        "formula_block",
        "formula fence becomes standalone block"
    );
    STR_EQ(runner, markdown_core_extensions_get_formula_literal(formula), "x+y", "formula fence literal is trimmed");
    markdown_core_node_free(doc);
}

/* Reads one attribute and compares it, so a sequence can be asserted without a
 * serialization to compare against -- which is what the deleted JSON was
 * doing for these tests. */
static void attribute_eq(
    test_batch_runner *runner,
    markdown_core_node *node,
    size_t index,
    const char *name,
    const char *value,
    const char *message
) {
    const char *actual_name = NULL;
    const char *actual_value = NULL;
    size_t name_length = 0;
    size_t value_length = 0;
    int ok = markdown_core_extensions_directive_attribute_at(
        node,
        index,
        &actual_name,
        &name_length,
        &actual_value,
        &value_length
    );
    OK(runner,
        ok && name_length == strlen(name) && memcmp(actual_name, name, name_length) == 0 &&
            value_length == strlen(value) && memcmp(actual_value, value, value_length) == 0,
        message);
}

static void directive_extension_accessors(test_batch_runner *runner) {
    /* `:-a[]` was the input here until Step 7, and it is not a directive: a
     * name may not BEGIN with a hyphen or underscore any more than it may end
     * with one. `class` is also the one name whose repeats accumulate now, so
     * the three of them are one value rather than the last one, and Step 7.2
     * made the sequence SORTED BY NAME rather than first-key source order. */
    markdown_core_node *doc = parse_with_directive_extension(
        ":a[]{id=first muted=true title=\"My Video\" bare dup=first dup=last "
        "class=red class=green class=blue id=123}\n"
    );
    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *directive = markdown_core_node_first_child(paragraph);

    STR_EQ(runner, markdown_core_node_get_type_string(directive), "directive", "directive inline type string");
    STR_EQ(runner, markdown_core_extensions_get_directive_name(directive), "a", "directive name getter");
    INT_EQ(
        runner,
        markdown_core_extensions_directive_has_attributes(directive),
        1,
        "directive reports its attribute container"
    );
    INT_EQ(runner, (int)markdown_core_extensions_directive_attribute_count(directive), 6, "directive attribute count");
    attribute_eq(runner, directive, 0, "bare", "", "attribute 0 sorts first and keeps its empty value");
    attribute_eq(runner, directive, 1, "class", "red green blue", "class accumulates in source order");
    attribute_eq(runner, directive, 2, "dup", "last", "a repeated name keeps its last value");
    attribute_eq(runner, directive, 3, "id", "123", "id is an ordinary name and keeps its last value");
    attribute_eq(runner, directive, 4, "muted", "true", "a bare attribute has an empty value");
    attribute_eq(runner, directive, 5, "title", "My Video", "a quoted value keeps its spaces");
    OK(runner,
        !markdown_core_extensions_directive_attribute_at(directive, 6, NULL, NULL, NULL, NULL),
        "an out-of-range attribute index is refused");

    INT_EQ(
        runner,
        markdown_core_extensions_set_directive_name(directive, "next_name-2"),
        1,
        "set directive name succeeds"
    );
    STR_EQ(
        runner,
        markdown_core_extensions_get_directive_name(directive),
        "next_name-2",
        "directive name setter updates payload"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_directive_name(directive, "bad-"),
        0,
        "set directive name rejects trailing hyphen"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_directive_name(directive, "-bad"),
        0,
        "set directive name rejects leading hyphen"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_directive_name(directive, "_bad"),
        0,
        "set directive name rejects leading underscore"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_directive_name(directive, "bad_"),
        0,
        "set directive name rejects trailing underscore"
    );
    INT_EQ(
        runner,
        markdown_core_extensions_set_directive_name(directive, ""),
        0,
        "set directive name rejects empty name"
    );
    STR_EQ(
        runner,
        markdown_core_extensions_get_directive_name(directive),
        "next_name-2",
        "rejected directive name leaves payload unchanged"
    );

    INT_EQ(
        runner,
        markdown_core_extensions_set_directive_name(paragraph, "ok"),
        0,
        "set directive name rejects non-directive nodes"
    );
    OK(runner,
        markdown_core_extensions_get_directive_name(paragraph) == NULL,
        "get directive name rejects non-directive nodes");
    INT_EQ(
        runner,
        markdown_core_extensions_directive_has_attributes(paragraph),
        0,
        "a non-directive node has no attribute container"
    );
    INT_EQ(
        runner,
        (int)markdown_core_extensions_directive_attribute_count(paragraph),
        0,
        "a non-directive node has no attributes to count"
    );
    markdown_core_node_free(doc);

    /* ABSENT is not EMPTY. `:plain[]` wrote no container and `:empty{}` wrote
     * one with nothing in it; a count of zero cannot tell them apart, which is
     * why has_attributes exists at all. */
    doc = parse_with_directive_extension(":plain[] :empty{}\n");
    paragraph = markdown_core_node_first_child(doc);
    directive = markdown_core_node_first_child(paragraph);
    INT_EQ(
        runner,
        markdown_core_extensions_directive_has_attributes(directive),
        0,
        "a directive with no attribute container reports none"
    );
    directive = markdown_core_node_next(markdown_core_node_next(directive));
    INT_EQ(
        runner,
        markdown_core_extensions_directive_has_attributes(directive),
        1,
        "an explicit empty attribute container is preserved"
    );
    INT_EQ(
        runner,
        (int)markdown_core_extensions_directive_attribute_count(directive),
        0,
        "an explicit empty attribute container holds nothing"
    );
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
        // Delete list, emph, and code nodes -- all at EXIT, which is Step 5's
        // mutation rule. `CODE` was deleted at ENTER here until the event
        // contract became total, and that was legal only because `S_is_leaf`
        // suppressed `CODE`'s EXIT. A test that frees at ENTER is a test
        // asserting the suppression list.
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT &&
            (node->type == MARKDOWN_CORE_NODE_LIST || node->type == MARKDOWN_CORE_NODE_EMPHASIS ||
                node->type == MARKDOWN_CORE_NODE_CODE)) {
            markdown_core_node_free(node);
        }
    }

    // Both lists are gone and each paragraph keeps only its text pieces.
    markdown_core_node *first = markdown_core_node_first_child(doc);
    markdown_core_node *second = markdown_core_node_next(first);
    INT_EQ(
        runner,
        markdown_core_node_get_type(first),
        MARKDOWN_CORE_NODE_PARAGRAPH,
        "first surviving node is a paragraph"
    );
    INT_EQ(
        runner,
        markdown_core_node_get_type(second),
        MARKDOWN_CORE_NODE_PARAGRAPH,
        "second surviving node is a paragraph"
    );
    OK(runner, markdown_core_node_next(second) == NULL, "deleted lists are unlinked");
    STR_EQ(
        runner,
        markdown_core_node_get_literal(markdown_core_node_first_child(first)),
        "a ",
        "first paragraph keeps leading text"
    );
    STR_EQ(
        runner,
        markdown_core_node_get_literal(markdown_core_node_next(markdown_core_node_first_child(first))),
        " c",
        "first paragraph keeps trailing text after deleted emph"
    );
    STR_EQ(
        runner,
        markdown_core_node_get_literal(markdown_core_node_next(markdown_core_node_first_child(second))),
        " c",
        "second paragraph keeps trailing text after deleted code"
    );

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
    unsigned int top_level_blocks[] = {
        MARKDOWN_CORE_NODE_BLOCK_QUOTE,
        MARKDOWN_CORE_NODE_LIST,
        MARKDOWN_CORE_NODE_CODE_BLOCK,
        MARKDOWN_CORE_NODE_HTML_BLOCK,
        MARKDOWN_CORE_NODE_PARAGRAPH,
        MARKDOWN_CORE_NODE_HEADING,
        MARKDOWN_CORE_NODE_THEMATIC_BREAK,
        MARKDOWN_CORE_NODE_REFERENCE_DEFINITION,
        0
    };
    unsigned int all_inlines[] = {
        MARKDOWN_CORE_NODE_TEXT,
        MARKDOWN_CORE_NODE_SOFT_BREAK,
        MARKDOWN_CORE_NODE_LINE_BREAK,
        MARKDOWN_CORE_NODE_CODE,
        MARKDOWN_CORE_NODE_HTML,
        MARKDOWN_CORE_NODE_EMPHASIS,
        MARKDOWN_CORE_NODE_STRONG,
        MARKDOWN_CORE_NODE_LINK,
        MARKDOWN_CORE_NODE_IMAGE,
        MARKDOWN_CORE_NODE_LINK_REFERENCE,
        MARKDOWN_CORE_NODE_IMAGE_REFERENCE,
        0
    };

    test_content(runner, MARKDOWN_CORE_NODE_DOCUMENT, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_BLOCK_QUOTE, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_LIST, list_item_flag);
    test_content(runner, MARKDOWN_CORE_NODE_LIST_ITEM, top_level_blocks);
    test_content(runner, MARKDOWN_CORE_NODE_CODE_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML_BLOCK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_PARAGRAPH, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_HEADING, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_THEMATIC_BREAK, 0);
    /* A link reference definition's body is a resource, not children. */
    test_content(runner, MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, 0);
    test_content(runner, MARKDOWN_CORE_NODE_TEXT, 0);
    test_content(runner, MARKDOWN_CORE_NODE_SOFT_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_LINE_BREAK, 0);
    test_content(runner, MARKDOWN_CORE_NODE_CODE, 0);
    test_content(runner, MARKDOWN_CORE_NODE_HTML, 0);
    test_content(runner, MARKDOWN_CORE_NODE_EMPHASIS, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_STRONG, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_LINK, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_IMAGE, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_LINK_REFERENCE, all_inlines);
    test_content(runner, MARKDOWN_CORE_NODE_IMAGE_REFERENCE, all_inlines);
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
    test_md_paragraph_text_options(
        runner,
        string_with_null,
        sizeof(string_with_null) - 1,
        MARKDOWN_CORE_OPT_DEFAULT,
        "((((" UTF8_REPL "))))",
        "utf8 with U+0000"
    );

    // Test NUL followed by newline
    static const char string_with_nul_lf[] = "```\n\0\n```\n";
    markdown_core_node *doc =
        markdown_core_parse_document(string_with_nul_lf, sizeof(string_with_nul_lf) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *code_block = markdown_core_node_first_child(doc);
    INT_EQ(
        runner,
        markdown_core_node_get_type(code_block),
        MARKDOWN_CORE_NODE_CODE_BLOCK,
        "utf8 with \\0\\n parses a code block"
    );
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
    INT_EQ(
        runner,
        markdown_core_node_get_type(list),
        MARKDOWN_CORE_NODE_LIST,
        "list with different line endings parses one list"
    );
    for (size_t i = 0; i < 4; i++) {
        OK(runner, item != NULL, "list item %zu exists", i);
        if (item) {
            markdown_core_node *paragraph = markdown_core_node_first_child(item);
            STR_EQ(
                runner,
                markdown_core_node_get_literal(markdown_core_node_first_child(paragraph)),
                expected_items[i],
                "list item %zu text",
                i
            );
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
    STR_EQ(
        runner,
        markdown_core_node_get_literal(markdown_core_node_first_child(paragraph)),
        "line",
        "crlf line splits into text"
    );
    INT_EQ(
        runner,
        markdown_core_node_get_type(middle),
        MARKDOWN_CORE_NODE_SOFT_BREAK,
        "crlf endings produce a softbreak"
    );
    STR_EQ(
        runner,
        markdown_core_node_get_literal(markdown_core_node_next(middle)),
        "line",
        "crlf trailing text follows the softbreak"
    );
    markdown_core_node_free(doc);

    static const char no_line_ending[] = "```\nline\n```";
    doc = markdown_core_parse_document(no_line_ending, sizeof(no_line_ending) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *code_block = markdown_core_node_first_child(doc);
    INT_EQ(
        runner,
        markdown_core_node_get_type(code_block),
        MARKDOWN_CORE_NODE_CODE_BLOCK,
        "fenced code block with no final newline parses"
    );
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
    STR_EQ(
        runner,
        markdown_core_node_get_literal(text),
        "before  after ",
        "strip-html-comments preserves surrounding text"
    );

    markdown_core_node *inline_html = markdown_core_node_next(text);
    INT_EQ(
        runner,
        markdown_core_node_get_type(inline_html),
        MARKDOWN_CORE_NODE_HTML,
        "strip-html-comments preserves non-comment inline HTML"
    );
    STR_EQ(
        runner,
        markdown_core_node_get_literal(inline_html),
        "<br>",
        "strip-html-comments keeps inline HTML literal"
    );

    markdown_core_node *block_html = markdown_core_node_next(paragraph);
    INT_EQ(
        runner,
        markdown_core_node_get_type(block_html),
        MARKDOWN_CORE_NODE_HTML_BLOCK,
        "strip-html-comments preserves non-comment HTML blocks"
    );
    STR_EQ(
        runner,
        markdown_core_node_get_literal(block_html),
        "<div>raw</div>\n",
        "strip-html-comments keeps block HTML literal"
    );

    markdown_core_node_free(doc);
}

/* Parses and asserts the document is a single paragraph whose concatenated
 * Text literals equal `expected_text`.  This replaces the retired
 * markdown_to_html comparisons: AST literals carry raw bytes, without HTML
 * escaping. */
static void test_md_paragraph_text_options(
    test_batch_runner *runner,
    const char *markdown,
    size_t markdown_length,
    int options,
    const char *expected_text,
    const char *msg
) {
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

static void test_md_paragraph_text(
    test_batch_runner *runner,
    const char *markdown,
    const char *expected_text,
    const char *msg
) {
    test_md_paragraph_text_options(
        runner,
        markdown,
        strlen(markdown),
        MARKDOWN_CORE_OPT_VALIDATE_UTF8,
        expected_text,
        msg
    );
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
static void test_facade_dump(
    test_batch_runner *runner,
    const char *markdown,
    int autolinks,
    const char *expected_dump,
    const char *msg
) {
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

// An extension that declines to open a block must answer NULL. The parser
// offers each attached extension a turn in attach order and stops at the first
// non-NULL answer, so an extension that returns the parent container on a
// decline takes away the turn of every extension attached after it -- and
// `table` used to do exactly that, on every path including "there is no table
// here". Enabling tables then stopped a directive block from interrupting a
// paragraph.
//
// THIS TEST SETS THE ATTACH ORDER ITSELF, and that is the point.
// packages/markdown-core/tests/fixtures/extensions-conflicts.txt covers the
// same property end to end, but only while the product's own attach order still
// puts `table` first; the moment that order changes the fixture passes whether
// or not the defect is present. This one keeps failing.
static void extension_decline_yields_turn(test_batch_runner *runner) {
    static const char *const markdown = "text\n:::note\nbody\n:::\n";

    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    const markdown_core_syntax_extension *table = &MARKDOWN_CORE_EXTENSION_TABLE;
    const markdown_core_syntax_extension *directive = &MARKDOWN_CORE_EXTENSION_DIRECTIVE;

    OK(runner, parser && table && directive, "table and directive extensions are available");
    if (!parser || !table || !directive) {
        if (parser) {
            markdown_core_parser_free(parser);
        }
        return;
    }
    OK(runner, markdown_core_parser_attach_syntax_extension(parser, table) != 0, "table attaches first");
    OK(runner, markdown_core_parser_attach_syntax_extension(parser, directive) != 0, "directive attaches second");

    markdown_core_parser_feed(parser, markdown, strlen(markdown));
    markdown_core_node *doc = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);

    markdown_core_node *paragraph = markdown_core_node_first_child(doc);
    markdown_core_node *block = paragraph ? markdown_core_node_next(paragraph) : NULL;
    INT_EQ(
        runner,
        markdown_core_node_get_type(paragraph),
        MARKDOWN_CORE_NODE_PARAGRAPH,
        "the lead paragraph survives table declining"
    );
    OK(runner, block != NULL, "an extension attached after table still gets its turn");
    STR_EQ(
        runner,
        block ? markdown_core_node_get_type_string(block) : "",
        "directive_block",
        "a declining table does not swallow the directive block"
    );
    markdown_core_node_free(doc);
}

/* Step 5. The event contract is TOTAL: every node yields exactly one ENTER and
 * exactly one EXIT, in that order, with its descendants' events between them.
 *
 * Until Step 5 an internal `S_is_leaf` list of eight node types suppressed the
 * EXIT of a node that "cannot have children" -- a list, not a property, so a
 * `FOOTNOTE_REFERENCE` with no children got an EXIT and a `TEXT` with no
 * children did not, and every walk in the engine had to know which. Three did:
 * `consolidate_text_nodes`, `S_strip_html_comments` and `autolink`'s
 * `postprocess`, and all three freed or spliced at ENTER because the
 * suppression made it safe. The input below contains one of every suppressed
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
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1, MARKDOWN_CORE_OPT_DEFAULT);
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
    markdown_core_node *q = markdown_core_node_new(MARKDOWN_CORE_NODE_BLOCK_QUOTE);
    markdown_core_node *r = markdown_core_node_new(MARKDOWN_CORE_NODE_BLOCK_QUOTE);
    markdown_core_node *a = markdown_core_node_new(MARKDOWN_CORE_NODE_BLOCK_QUOTE);
    markdown_core_node *b = markdown_core_node_new(MARKDOWN_CORE_NODE_BLOCK_QUOTE);

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
static markdown_core_node *stray_delimiter_push(
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    unsigned char character,
    markdown_core_delimiter_rule rule
) {
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
    markdown_core_inline_parser_push_delimiter(inline_parser, NULL, rule, 0, 1, node);
    return node;
}

static markdown_core_node *stray_unowned_match(
    const markdown_core_syntax_extension *self,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
) {
    (void)self;
    (void)parent;
    return stray_delimiter_push(parser, inline_parser, character, MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH);
}

static markdown_core_node *stray_unnamed_match(
    const markdown_core_syntax_extension *self,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
) {
    (void)self;
    (void)parent;
    return stray_delimiter_push(parser, inline_parser, character, (markdown_core_delimiter_rule)200);
}

static const markdown_core_syntax_extension STRAY_UNOWNED =
    {.name = "stray-unowned", .match_inline = stray_unowned_match, .terminates_text = "@", .dispatch = "@"};
static const markdown_core_syntax_extension STRAY_UNNAMED =
    {.name = "stray-unnamed", .match_inline = stray_unnamed_match, .terminates_text = "@", .dispatch = "@"};

static void stray_delimiter_parse(
    test_batch_runner *runner,
    const markdown_core_syntax_extension *extension,
    const char *what
) {
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *document;
    const char *input = "a @ b @ c\n";

    markdown_core_parser_attach_syntax_extension(parser, extension);
    markdown_core_parser_feed(parser, input, strlen(input));
    document = markdown_core_parser_finish(parser);

    OK(runner, document != NULL, "a delimiter with %s still finishes the parse", what);
    markdown_core_node_free(document);
    markdown_core_parser_free(parser);
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
 * cleared and reused -- `parser->curline` and `parser->linebuf` -- now both
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
 * cap is INT32_MAX/2 -- so this is the state a single 1.07 GiB line reaches
 * through `markdown_core_parser_feed`'s `linebuf`, and the put is the next
 * chunk. It is forged rather than fed because feeding it costs 2 GiB. */
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
        runner,
        markdown,
        0,
        "Document id=1:0 scope=1:1..10:20 children=3\n"
        "├── Heading id=2:0 scope=1:1..1:13 level=1 children=3\n"
        "│   ├── Text id=2:1 scope=1:3..1:5 literal=\"Hi \" children=0\n"
        "│   ├── Emphasis id=2:2 scope=1:6..1:12 children=1\n"
        "│   │   └── Text id=2:3 scope=1:7..1:11 literal=\"there\" children=0\n"
        "│   └── Text id=2:4 scope=1:13..1:13 literal=\".\" children=0\n"
        "├── Paragraph id=3:0 scope=3:1..4:42 children=8\n"
        "│   ├── Text id=3:1 scope=3:1..3:14 literal=\"Hello “ \" children=0\n"
        "│   ├── Link id=3:2 scope=3:15..3:37 destination=\"http://www.google.com\" "
        "title=null children=1\n"
        "│   │   └── Text id=3:3 scope=3:16..3:36 literal=\"http://www.google.com\" "
        "children=0\n"
        "│   ├── SoftBreak id=3:4 scope=3:38..3:38 children=0\n"
        "│   ├── Text id=3:5 scope=4:1..4:6 literal=\"there \" children=0\n"
        "│   ├── Code id=3:6 scope=4:7..4:10 literal=\"hi\" children=0\n"
        "│   ├── Text id=3:7 scope=4:11..4:14 literal=\" -- \" children=0\n"
        "│   ├── Link id=3:8 scope=4:15..4:41 destination=\"www.google.com\" title=\"ok\" "
        "children=1\n"
        "│   │   └── Text id=3:9 scope=4:16..4:19 literal=\"okay\" children=0\n"
        "│   └── Text id=3:10 scope=4:42..4:42 literal=\".\" children=0\n"
        "└── BlockQuote id=4:0 scope=6:1..10:20 children=1\n"
        "    └── List id=5:0 scope=6:3..10:20 flavor=ordered start=1 tight=false children=2\n"
        "        ├── ListItem id=6:0 scope=6:3..8:1 checked=null children=1\n"
        "        │   └── Paragraph id=7:0 scope=6:6..7:10 children=3\n"
        "        │       ├── Text id=7:1 scope=6:6..6:10 literal=\"Okay.\" children=0\n"
        "        │       ├── SoftBreak id=7:2 scope=6:11..6:11 children=0\n"
        "        │       └── Text id=7:3 scope=7:6..7:10 literal=\"Sure.\" children=0\n"
        "        └── ListItem id=8:0 scope=9:3..10:20 checked=null children=1\n"
        "            └── Paragraph id=9:0 scope=9:6..10:20 children=3\n"
        "                ├── Text id=9:1 scope=9:6..9:15 literal=\"Yes, okay.\" children=0\n"
        "                ├── SoftBreak id=9:2 scope=9:16..9:16 children=0\n"
        "                └── Image id=9:3 scope=10:6..10:20 source=\"hi\" title=\"yes\" "
        "children=1\n"
        "                    └── Text id=9:4 scope=10:8..10:9 literal=\"ok\" children=0\n",
        "scopes are as expected"
    );
}

static void source_pos_inlines(test_batch_runner *runner) {
    test_facade_dump(
        runner,
        "*first*\n"
        "second\n",
        0,
        "Document id=1:0 scope=1:1..2:6 children=1\n"
        "└── Paragraph id=2:0 scope=1:1..2:6 children=3\n"
        "    ├── Emphasis id=2:1 scope=1:1..1:7 children=1\n"
        "    │   └── Text id=2:2 scope=1:2..1:6 literal=\"first\" children=0\n"
        "    ├── SoftBreak id=2:3 scope=1:8..1:8 children=0\n"
        "    └── Text id=2:4 scope=2:1..2:6 literal=\"second\" children=0\n",
        "closed emphasis scopes are as expected"
    );
    test_facade_dump(
        runner,
        "*first\n"
        "second*\n",
        0,
        "Document id=1:0 scope=1:1..2:7 children=1\n"
        "└── Paragraph id=2:0 scope=1:1..2:7 children=1\n"
        "    └── Emphasis id=2:1 scope=1:1..2:7 children=3\n"
        "        ├── Text id=2:2 scope=1:2..1:6 literal=\"first\" children=0\n"
        "        ├── SoftBreak id=2:3 scope=1:7..1:7 children=0\n"
        "        └── Text id=2:4 scope=2:1..2:6 literal=\"second\" children=0\n",
        "multiline emphasis scopes are as expected"
    );
}

/* §5.6's G7: ONE accessor answers for all five reference kinds and refuses
 * every other node.
 *
 * The five differ in where the association lives -- a definition's is boxed
 * behind a pointer, a footnote's is inline in the union, a link reference's is
 * inside a wider struct -- which is exactly why this is a switch on the type
 * and not a common-initial-sequence read. A sixth kind that answered here
 * would be reading some other union arm as two chunks. */
static void association_accessor(test_batch_runner *runner) {
    /* An inline Link and an inline Image are in the corpus DELIBERATELY: they
     * are the two kinds nearest to answering by accident, because their union
     * arm is a pair of chunks too. Without them a sixth arm added to the
     * switch kills nothing -- measured. */
    static const char markdown[] = "[a][ref] ![b][ref] [^n] [c](/inline) ![d](/i.png)\n"
                                   "\n"
                                   "[ref]: /r\n"
                                   "\n"
                                   "[^n]: note\n";
    markdown_core_parse_options options;
    markdown_core_document *document;
    const markdown_core_node *root;
    const markdown_core_node *node;
    markdown_core_string label = {NULL, 0};
    markdown_core_string identifier = {NULL, 0};
    int answered = 0;
    int refused = 0;
    size_t seen = 0;
    /* Five kinds answer. Everything else -- including the Paragraph, the Text
     * children and the Document -- refuses. */
    const markdown_core_node_kind carriers[] = {
        MARKDOWN_CORE_KIND_REFERENCE_DEFINITION,
        MARKDOWN_CORE_KIND_LINK_REFERENCE,
        MARKDOWN_CORE_KIND_IMAGE_REFERENCE,
        MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION,
        MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE
    };
    unsigned int found = 0;

    memset(&options, 0, sizeof(options));
    options.footnotes = true;
    document = markdown_core_document_parse((const uint8_t *)markdown, strlen(markdown), &options, NULL);
    if (!document) {
        OK(runner, 0, "association corpus parses");
        return;
    }
    root = markdown_core_document_semantic(document);

    {
        /* An explicit stack, because the walk must reach every node and the
         * facade's traversal is the only one the accessor is public through. */
        const markdown_core_node *stack[64];
        size_t depth = 0;
        stack[depth++] = root;
        while (depth > 0) {
            const markdown_core_node *current = stack[--depth];
            markdown_core_node_kind kind = markdown_core_node_get_kind(current);
            size_t index;
            int carries = 0;
            seen++;
            for (index = 0; index < sizeof(carriers) / sizeof(carriers[0]); index++) {
                if (kind == carriers[index]) {
                    carries = 1;
                    found |= 1u << index;
                }
            }
            if (markdown_core_node_association(current, &label, &identifier)) {
                answered++;
                INT_EQ(runner, carries, 1, "kind %d answers the association accessor", (int)kind);
                OK(runner,
                    label.data != NULL && identifier.data != NULL && identifier.length > 0,
                    "kind %d carries both halves",
                    (int)kind);
            } else {
                refused++;
                INT_EQ(runner, carries, 0, "kind %d refuses the association accessor", (int)kind);
            }
            markdown_core_children walk_cursor = markdown_core_node_children(current);
            for (; (node = walk_cursor.child) != NULL; walk_cursor = markdown_core_children_next(walk_cursor)) {
                if (depth < sizeof(stack) / sizeof(stack[0])) {
                    stack[depth++] = node;
                }
            }
        }
    }
    INT_EQ(runner, (int)found, 31, "all five reference kinds appear in the corpus");
    INT_EQ(runner, answered, 5, "exactly five nodes answer");
    OK(runner, refused > 0 && seen == (size_t)(answered + refused), "every other node refuses");
    markdown_core_document_free(document);
}

/* D4 through the facade: every node answers the identity accessor with the
 * whole pair the dump prints as `id=`, and every reference names the FIRST
 * definition of its label while later definitions stay in the tree. */
static void identity_accessors(test_batch_runner *runner) {
    /* Two definitions of one label, in document order, referenced once --
     * plus a footnote pair, so all three reference kinds carry the edge. */
    static const char markdown[] = "See [a] and ![a] and [^n].\n"
                                   "\n"
                                   "[a]: /first\n"
                                   "\n"
                                   "[a]: /second\n"
                                   "\n"
                                   "[^n]: note\n";
    markdown_core_parse_options options;
    markdown_core_document *document;
    const markdown_core_node *root;
    const markdown_core_node *paragraph;
    const markdown_core_node *node;
    markdown_core_identity identity;
    markdown_core_identity winner = {0, 0};
    markdown_core_identity loser = {0, 0};
    markdown_core_identity definition = {0, 0};
    int references = 0;

    memset(&options, 0, sizeof(options));
    options.footnotes = true;
    document = markdown_core_document_parse((const uint8_t *)markdown, strlen(markdown), &options, NULL);
    if (!document) {
        OK(runner, 0, "identity corpus parses");
        return;
    }
    root = markdown_core_document_semantic(document);

    identity = markdown_core_node_identifier(root);
    OK(runner, identity.block != 0 && identity.ordinal == 0, "the root is a block with a nonzero mint");
    identity = markdown_core_node_identifier(NULL);
    OK(runner, identity.block == 0 && identity.ordinal == 0, "a null node has no identity");
    OK(runner, !markdown_core_node_reference_definition(root, &definition), "a document carries no definition edge");

    /* The two ReferenceDefinition siblings, in document order. Mints are
     * monotone in document order (D4), which is what lets the map's smallest
     * identity BE the first definition. */
    markdown_core_children root_cursor = markdown_core_node_children(root);
    for (; (node = root_cursor.child) != NULL; root_cursor = markdown_core_children_next(root_cursor)) {
        if (markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_REFERENCE_DEFINITION) {
            markdown_core_identity mint = markdown_core_node_identifier(node);
            OK(runner, mint.block != 0 && mint.ordinal == 0, "a definition is a block with a nonzero mint");
            if (!winner.block) {
                winner = mint;
            } else {
                loser = mint;
            }
        }
    }
    OK(runner,
        winner.block != 0 && loser.block != 0 && winner.block < loser.block,
        "both definitions are in the tree and mints follow document order");

    paragraph = markdown_core_node_children(root).child;
    {
        markdown_core_children para_cursor = markdown_core_node_children(paragraph);
        for (; (node = para_cursor.child) != NULL; para_cursor = markdown_core_children_next(para_cursor)) {
            markdown_core_node_kind kind = markdown_core_node_get_kind(node);
            if (kind == MARKDOWN_CORE_KIND_LINK_REFERENCE || kind == MARKDOWN_CORE_KIND_IMAGE_REFERENCE) {
                markdown_core_identity paragraph_identity = markdown_core_node_identifier(paragraph);
                references++;
                identity = markdown_core_node_identifier(node);
                OK(runner,
                    identity.block == paragraph_identity.block && identity.ordinal != 0,
                    "kind %d is an inline owned by its paragraph, with a nonzero ordinal",
                    (int)kind);
                OK(runner,
                    markdown_core_node_reference_definition(node, &definition) && definition.block == winner.block &&
                        definition.ordinal == 0,
                    "kind %d names the first definition of its label",
                    (int)kind);
            }
            if (kind == MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE) {
                references++;
                OK(runner,
                    markdown_core_node_reference_definition(node, &definition) && definition.block != 0 &&
                        definition.block != winner.block && definition.block != loser.block,
                    "a footnote call names its own definition");
            }
        }
    }
    INT_EQ(runner, references, 3, "all three reference kinds resolved");
    markdown_core_document_free(document);
}

/* THE WIRE beside the dump: deterministic, partition-invariant, and refused
 * without a document to serialize. The per-field truth of the layout is
 * pinned by every binding's decoder against its own suites; what the C side
 * owns is that the bytes are a function of the parse alone. */
static void wire_serialization(test_batch_runner *runner) {
    static const char markdown[] = "# Hi\n\nSee [a] and [^n].\n\n[a]: /u \"t\"\n\n[^n]: note\n";
    markdown_core_parse_options options;
    markdown_core_document *document;
    markdown_core_session *session;
    markdown_core_error *error = NULL;
    uint8_t *whole = NULL;
    uint8_t *again = NULL;
    uint8_t *streamed = NULL;
    size_t whole_length = 0;
    size_t again_length = 0;
    size_t streamed_length = 0;
    size_t index;

    markdown_core_parse_options_init(&options);
    document = markdown_core_document_parse((const uint8_t *)markdown, strlen(markdown), &options, NULL);
    if (!document) {
        OK(runner, 0, "wire corpus parses");
        return;
    }
    OK(runner,
        markdown_core_document_wire(document, 0, &whole, &whole_length, &error) && whole != NULL && whole_length > 0,
        "the wire serializes a parsed document");
    OK(runner,
        markdown_core_document_wire(document, 0, &again, &again_length, &error) && again_length == whole_length &&
            memcmp(whole, again, whole_length) == 0,
        "two serializations of one document are byte-identical");
    markdown_core_wire_free(again);

    /* THE ENVELOPE ROOM: a transport's prefix rides in the one allocation,
     * zeroed, ahead of the same payload bytes. */
    OK(runner,
        markdown_core_document_wire(document, 5, &again, &again_length, &error) && again_length == whole_length + 5 &&
            again[0] == 0 && again[4] == 0 && memcmp(again + 5, whole, whole_length) == 0,
        "a prefixed serialization reserves zeroed envelope room ahead of the payload");
    markdown_core_wire_free(again);
    again = NULL;

    OK(runner,
        !markdown_core_document_wire(NULL, 0, &again, &again_length, &error),
        "the wire refuses a null document");
    OK(runner,
        error != NULL && markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
        "the refusal names its reason");
    markdown_core_error_free(error);
    error = NULL;

    /* Partition invariance rides through: the sealed stream's wire equals the
     * whole-text parse's, byte for byte, however the bytes were fed. */
    /* Partition invariance rides through `advance` exactly as through `feed`:
     * an advance takes the bytes and answers nothing -- no projection, no
     * document -- which is the discarded-read lifecycle the bindings'
     * `Document(markdown)` constructor documents. */
    session = markdown_core_session_new(&options, NULL);
    if (session) {
        markdown_core_document *sealed;
        for (index = 0; index < strlen(markdown); index += 3) {
            size_t length = strlen(markdown) - index < 3 ? strlen(markdown) - index : 3;
            const uint8_t *chunk = (const uint8_t *)markdown + index;
            OK(runner, markdown_core_session_advance(session, chunk, length, NULL), "an advance takes its bytes");
        }
        sealed = markdown_core_session_finish(session, NULL);
        OK(runner,
            sealed != NULL && markdown_core_document_wire(sealed, 0, &streamed, &streamed_length, NULL) &&
                streamed_length == whole_length && memcmp(streamed, whole, whole_length) == 0,
            "the stream sealed after advances equals the whole-text parse");
        markdown_core_wire_free(streamed);
        markdown_core_document_free(sealed);
        OK(runner,
            !markdown_core_session_advance(session, (const uint8_t *)"x", 1, &error) && error != NULL &&
                markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "an advance after finish is refused with the finished-session error");
        markdown_core_error_free(error);
        error = NULL;
        markdown_core_session_free(session);
    } else {
        OK(runner, 0, "wire session opens");
    }

    /* THE MANAGED FEED's equivalence (#146): `session_feed_wire` answers the
     * same bytes as feed + wire + free -- prefix room included -- at every
     * boundary of a chunked stream, and again for a zero-length feed. Two
     * sessions run the same chunks side by side, one through each path. */
    {
        markdown_core_session *composed = markdown_core_session_new(&options, NULL);
        markdown_core_session *direct = markdown_core_session_new(&options, NULL);
        int equal = composed != NULL && direct != NULL;
        size_t offset = 0;
        size_t rounds = 0;

        while (equal && offset <= strlen(markdown)) {
            size_t chunk_length = strlen(markdown) - offset < 7 ? strlen(markdown) - offset : 7;
            const uint8_t *chunk = chunk_length ? (const uint8_t *)markdown + offset : NULL;
            markdown_core_document *stepped;
            uint8_t *composed_wire = NULL;
            uint8_t *direct_wire = NULL;
            size_t composed_length = 0;
            size_t direct_length = 0;

            stepped = markdown_core_session_feed(composed, chunk, chunk_length, NULL);
            equal = stepped != NULL && markdown_core_document_wire(stepped, 3, &composed_wire, &composed_length, NULL);
            markdown_core_document_free(stepped);
            equal = equal &&
                    markdown_core_session_feed_wire(direct, chunk, chunk_length, 3, &direct_wire, &direct_length, NULL);
            equal = equal && composed_length == direct_length &&
                    memcmp(composed_wire, direct_wire, composed_length) == 0 && direct_wire[0] == 0 &&
                    direct_wire[2] == 0;
            markdown_core_wire_free(composed_wire);
            markdown_core_wire_free(direct_wire);
            rounds++;
            if (chunk_length == 0) {
                break;
            }
            offset += chunk_length;
        }
        OK(runner,
            equal && rounds > 2,
            "the managed feed's wire equals feed + wire + free at every boundary, zero-length feed included");
        markdown_core_session_free(composed);
        markdown_core_session_free(direct);
    }
    {
        uint8_t *refused = NULL;
        size_t refused_length = 0;
        OK(runner,
            !markdown_core_session_feed_wire(NULL, NULL, 0, 0, &refused, &refused_length, &error) && error != NULL &&
                markdown_core_error_get_code(error) == MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "the managed feed wire refuses a null session with the finished-session error");
        markdown_core_error_free(error);
        error = NULL;
    }
    markdown_core_wire_free(whole);
    markdown_core_document_free(document);
}

static void ref_source_pos(test_batch_runner *runner) {
    static const char markdown[] = "Let's try [reference] links.\n"
                                   "\n"
                                   "[reference]: https://github.com (GitHub)\n";

    test_facade_dump(
        runner,
        markdown,
        0,
        "Document id=1:0 scope=1:1..3:40 children=2\n"
        "├── Paragraph id=2:0 scope=1:1..1:28 children=3\n"
        "│   ├── Text id=2:1 scope=1:1..1:10 literal=\"Let's try \" children=0\n"
        "│   ├── LinkReference id=2:2 scope=1:11..1:21 label=\"reference\" "
        "form=shortcut definition=3:0 children=1\n"
        "│   │   └── Text id=2:3 scope=1:12..1:20 literal=\"reference\" children=0\n"
        "│   └── Text id=2:4 scope=1:22..1:28 literal=\" links.\" children=0\n"
        "└── ReferenceDefinition id=3:0 scope=3:1..3:40 label=\"reference\" norm=\"reference\" "
        "destination=\"https://github.com\" title=\"GitHub\" children=0\n",
        "reference link scopes are as expected"
    );
}

static void autolink_source_pos(test_batch_runner *runner) {
    test_facade_dump(
        runner,
        "See www.example.com.\n",
        1,
        "Document id=1:0 scope=1:1..1:20 children=1\n"
        "└── Paragraph id=2:0 scope=1:1..1:20 children=3\n"
        "    ├── Text id=2:1 scope=1:1..1:4 literal=\"See \" children=0\n"
        "    ├── Link id=2:2 scope=1:5..1:19 destination=\"http://www.example.com\" "
        "title=null children=1\n"
        "    │   └── Text id=2:3 scope=1:5..1:19 literal=\"www.example.com\" children=0\n"
        "    └── Text id=2:4 scope=1:20..1:20 literal=\".\" children=0\n",
        "www autolink scopes are as expected"
    );
    test_facade_dump(
        runner,
        "See http://example.com.\n",
        1,
        "Document id=1:0 scope=1:1..1:23 children=1\n"
        "└── Paragraph id=2:0 scope=1:1..1:23 children=3\n"
        "    ├── Text id=2:1 scope=1:1..1:4 literal=\"See \" children=0\n"
        "    ├── Link id=2:2 scope=1:5..1:22 destination=\"http://example.com\" title=null "
        "children=1\n"
        "    │   └── Text id=2:3 scope=1:5..1:22 literal=\"http://example.com\" children=0\n"
        "    └── Text id=2:4 scope=1:23..1:23 literal=\".\" children=0\n",
        "scheme autolink scopes are as expected"
    );
    /* An autolink at column one leaves NO prefix. This assertion used to pin the
     * defect -- it asserted a `Text scope=0:0..0:0 literal=""` as expected
     * output, a child with no bytes and no position, and a paragraph that said
     * it had two children when it had one thing in it. 0a.14 removes the node;
     * unpinning the assertion is the fix, the same shape as D10's
     * `regression.txt` example 24 at 0a.2. */
    test_facade_dump(
        runner,
        "http://example.com\n",
        1,
        "Document id=1:0 scope=1:1..1:18 children=1\n"
        "└── Paragraph id=2:0 scope=1:1..1:18 children=1\n"
        "    └── Link id=2:1 scope=1:1..1:18 destination=\"http://example.com\" title=null "
        "children=1\n"
        "        └── Text id=2:2 scope=1:1..1:18 literal=\"http://example.com\" children=0\n",
        "scheme autolink at column one scopes are as expected"
    );
    test_facade_dump(
        runner,
        "Mail user@example.com now.\n",
        1,
        "Document id=1:0 scope=1:1..1:26 children=1\n"
        "└── Paragraph id=2:0 scope=1:1..1:26 children=3\n"
        "    ├── Text id=2:1 scope=1:1..1:5 literal=\"Mail \" children=0\n"
        "    ├── Link id=2:2 scope=1:6..1:21 destination=\"mailto:user@example.com\" "
        "title=null children=1\n"
        "    │   └── Text id=2:3 scope=1:6..1:21 literal=\"user@example.com\" children=0\n"
        "    └── Text id=2:4 scope=1:22..1:26 literal=\" now.\" children=0\n",
        "email autolink scopes are as expected"
    );
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
    extension_decline_yields_turn(runner);
    source_pos(runner);
    source_pos_inlines(runner);
    ref_source_pos(runner);
    association_accessor(runner);
    identity_accessors(runner);
    wire_serialization(runner);
    autolink_source_pos(runner);
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
