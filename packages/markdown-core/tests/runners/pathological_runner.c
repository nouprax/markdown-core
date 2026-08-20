/* Pathological and adversarial input suite.
 *
 * Each case is registered as its own CTest test so failures and timeouts
 * stay addressable:
 *
 *   pathological_runner --list
 *   pathological_runner --case NAME
 *
 * Wall-clock limits are enforced by the CTest TIMEOUT property.  All
 * verification is structural, through the read-only facade: node-kind
 * counts, concatenated Text literals, and typed property probes — the AST
 * equivalents of the retired HTML-output assertions.  Traversal is
 * iterative, so 50000-deep trees cannot overflow the stack.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

typedef struct pc_context {
    char *input;
    size_t input_length;
    markdown_core_document *document;
    size_t counts[TS_KIND_COUNT];
} pc_context;

static const char *const PC_TABLE_ONLY[] = {"table", NULL};
static const char *const PC_DIRECTIVE_ONLY[] = {"directive", NULL};
static const char *const PC_FORMULA[] = {"formula", "dollar-formula-delimiters", "latex-formula-delimiters", NULL};

/* Builds prefix + unit*count + suffix into context->input. */
static int pc_build(pc_context *context, const char *prefix, const char *unit, size_t count, const char *suffix) {
    size_t prefix_length = prefix ? strlen(prefix) : 0;
    size_t suffix_length = suffix ? strlen(suffix) : 0;
    size_t unit_length = strlen(unit);
    size_t total = prefix_length + unit_length * count + suffix_length;
    char *buffer = (char *)malloc(total + 1);
    size_t i;
    if (!buffer)
        return -1;
    if (prefix_length)
        memcpy(buffer, prefix, prefix_length);
    for (i = 0; i < count; i++)
        memcpy(buffer + prefix_length + i * unit_length, unit, unit_length);
    if (suffix_length)
        memcpy(buffer + prefix_length + unit_length * count, suffix, suffix_length);
    buffer[total] = 0;
    context->input = buffer;
    context->input_length = total;
    return 0;
}

static int pc_parse(pc_context *context, const char *const *option_names) {
    markdown_core_parse_options options;
    size_t i;
    ts_ast_options_none(&options);
    for (i = 0; option_names && option_names[i]; i++) {
        if (ts_ast_enable(&options, option_names[i]) != 0)
            return -1;
    }
    context->document = ts_ast_parse((const uint8_t *)context->input, context->input_length, &options);
    if (!context->document)
        return -1;
    return ts_ast_count_kinds(markdown_core_document_root(context->document), context->counts);
}

static int pc_expect_count(const pc_context *context, markdown_core_node_kind kind, size_t expected, const char *what) {
    if (context->counts[kind] != expected) {
        fprintf(stderr, "expected %zu %s node(s), found %zu\n", expected, what, context->counts[kind]);
        return -1;
    }
    return 0;
}

/* Asserts the concatenated Text literals equal `expected` (defaults to the
 * raw input, the AST equivalent of "everything stayed literal text"). */
static int pc_expect_text(const pc_context *context, const char *expected, size_t expected_length) {
    size_t actual_length = 0;
    char *actual = ts_ast_concat_text(markdown_core_document_root(context->document), &actual_length);
    int result = 0;
    if (!actual)
        return -1;
    if (actual_length != expected_length || memcmp(actual, expected, expected_length) != 0) {
        fprintf(stderr, "concatenated text differs from expected (%zu vs %zu bytes, first bytes: %.40s)\n",
                actual_length, expected_length, actual);
        result = -1;
    }
    free(actual);
    return result;
}

/* Trailing blanks on the final line are trimmed by the block parser, so the
 * literal-text expectation drops them from the input too. */
static int pc_expect_text_is_input(const pc_context *context) {
    size_t expected_length = context->input_length;
    while (expected_length > 0 &&
           (context->input[expected_length - 1] == ' ' || context->input[expected_length - 1] == '\t'))
        expected_length--;
    return pc_expect_text(context, context->input, expected_length);
}

/* Case implementations ---------------------------------------------------- */

static int case_nested_strong_emph(pc_context *context) {
    char *left = ts_repeat("*a **a ", 65000, NULL);
    char *right = ts_repeat(" a** a*", 65000, NULL);
    int result = -1;
    if (left && right) {
        size_t left_length = strlen(left);
        size_t right_length = strlen(right);
        context->input = (char *)malloc(left_length + 1 + right_length + 1);
        if (context->input) {
            memcpy(context->input, left, left_length);
            context->input[left_length] = 'b';
            memcpy(context->input + left_length + 1, right, right_length + 1);
            context->input_length = left_length + 1 + right_length;
            result = 0;
        }
    }
    free(left);
    free(right);
    if (result != 0 || pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 65000, "Emphasis") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 65000, "Strong") != 0)
        return -1;
    return 0;
}

static int pc_literal_case(pc_context *context, const char *unit, size_t count, markdown_core_node_kind forbidden_kind,
                           const char *forbidden_name) {
    if (pc_build(context, NULL, unit, count, NULL) != 0)
        return -1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, forbidden_kind, 0, forbidden_name) != 0)
        return -1;
    return pc_expect_text_is_input(context);
}

static int case_emph_closers(pc_context *context) {
    return pc_literal_case(context, "a_ ", 65000, MARKDOWN_CORE_KIND_EMPHASIS, "Emphasis");
}

static int case_emph_openers(pc_context *context) {
    return pc_literal_case(context, "_a ", 65000, MARKDOWN_CORE_KIND_EMPHASIS, "Emphasis");
}

static int case_link_closers(pc_context *context) {
    return pc_literal_case(context, "a]", 65000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_link_openers(pc_context *context) {
    return pc_literal_case(context, "[a", 65000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_mismatched_openers_closers(pc_context *context) {
    if (pc_literal_case(context, "*a_ ", 50000, MARKDOWN_CORE_KIND_EMPHASIS, "Emphasis") != 0)
        return -1;
    return pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 0, "Strong");
}

static int case_openers_closers_multiple_of_3(pc_context *context) {
    if (pc_build(context, "a**b", "c* ", 50000, NULL) != 0)
        return -1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 0, "Emphasis") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 0, "Strong") != 0)
        return -1;
    return pc_expect_text_is_input(context);
}

static int case_link_openers_emph_closers(pc_context *context) {
    if (pc_literal_case(context, "[ a_", 50000, MARKDOWN_CORE_KIND_LINK, "Link") != 0)
        return -1;
    return pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 0, "Emphasis");
}

static int case_pattern_bracket_paren(pc_context *context) {
    return pc_literal_case(context, "[ (](", 80000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_pattern_image_link(pc_context *context) {
    char *expected;
    size_t expected_length = 0;
    int result;
    if (pc_build(context, NULL, "![[]()", 160000, NULL) != 0)
        return -1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 160000, "Link") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_IMAGE, 0, "Image") != 0)
        return -1;
    expected = ts_repeat("![", 160000, &expected_length);
    if (!expected)
        return -1;
    result = pc_expect_text(context, expected, expected_length);
    free(expected);
    return result;
}

static int case_hard_link_emph(pc_context *context) {
    const markdown_core_node *root;
    const markdown_core_node *paragraph;
    const markdown_core_node *text;
    const markdown_core_node *link;
    const markdown_core_node *emphasis;
    markdown_core_string_view view;
    markdown_core_string_view title;

    if (pc_build(context, "**x [a*b**c*](d)", "", 0, NULL) != 0)
        return -1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 1, "Link") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 1, "Emphasis") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 0, "Strong") != 0)
        return -1;

    root = markdown_core_document_root(context->document);
    paragraph = markdown_core_node_get_first_child(root);
    text = markdown_core_node_get_first_child(paragraph);
    if (!markdown_core_node_literal(text, &view) || view.length != 4 || memcmp(view.data, "**x ", 4) != 0) {
        fprintf(stderr, "leading text is not the literal '**x '\n");
        return -1;
    }
    link = markdown_core_node_get_next_sibling(text);
    if (markdown_core_node_get_kind(link) != MARKDOWN_CORE_KIND_LINK ||
        !markdown_core_node_link_properties(link, &view, &title) || view.length != 1 || view.data[0] != 'd') {
        fprintf(stderr, "link destination is not 'd'\n");
        return -1;
    }
    emphasis = markdown_core_node_get_next_sibling(markdown_core_node_get_first_child(link));
    if (markdown_core_node_get_kind(emphasis) != MARKDOWN_CORE_KIND_EMPHASIS) {
        fprintf(stderr, "emphasis is not inside the link\n");
        return -1;
    }
    return 0;
}

static int case_nested_brackets(pc_context *context) {
    char *closers;
    if (pc_build(context, NULL, "[", 50000, "a") != 0)
        return -1;
    closers = ts_repeat("]", 50000, NULL);
    if (!closers)
        return -1;
    {
        size_t old_length = context->input_length;
        char *grown = (char *)realloc(context->input, old_length + 50000 + 1);
        if (!grown) {
            free(closers);
            return -1;
        }
        memcpy(grown + old_length, closers, 50000 + 1);
        context->input = grown;
        context->input_length = old_length + 50000;
    }
    free(closers);
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 0, "Link") != 0)
        return -1;
    return pc_expect_text_is_input(context);
}

static int case_nested_block_quotes(pc_context *context) {
    if (pc_build(context, NULL, "> ", 50000, "a") != 0)
        return -1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_BLOCK_QUOTE, 50000, "BlockQuote") != 0)
        return -1;
    return pc_expect_text(context, "a", 1);
}

static int case_deeply_nested_lists(pc_context *context) {
    char *expected;
    size_t expected_length = 0;
    int result;
    {
        size_t depth;
        size_t total = 0;
        char *cursor;
        for (depth = 0; depth < 1000; depth++)
            total += depth * 2 + 4; /* "  "*depth + "* a\n" */
        context->input = (char *)malloc(total + 1);
        if (!context->input)
            return -1;
        cursor = context->input;
        for (depth = 0; depth < 1000; depth++) {
            memset(cursor, ' ', depth * 2);
            cursor += depth * 2;
            memcpy(cursor, "* a\n", 4);
            cursor += 4;
        }
        *cursor = 0;
        context->input_length = total;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LIST, 1000, "List") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_LIST_ITEM, 1000, "ListItem") != 0)
        return -1;
    expected = ts_repeat("a", 1000, &expected_length);
    if (!expected)
        return -1;
    result = pc_expect_text(context, expected, expected_length);
    free(expected);
    return result;
}

static int case_nul_in_input(pc_context *context) {
    static const char raw[] = "abc\0de\0";
    static const char expected[] = "abc\xEF\xBF\xBD"
                                   "de\xEF\xBF\xBD";
    context->input = (char *)malloc(sizeof(raw));
    if (!context->input)
        return -1;
    memcpy(context->input, raw, sizeof(raw));
    context->input_length = sizeof(raw) - 1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    return pc_expect_text(context, expected, sizeof(expected) - 1);
}

static int case_backticks(pc_context *context) {
    size_t run, total = 0;
    char *cursor;
    for (run = 1; run < 5000; run++)
        total += 1 + run;
    context->input = (char *)malloc(total + 1);
    if (!context->input)
        return -1;
    cursor = context->input;
    for (run = 1; run < 5000; run++) {
        *cursor++ = 'e';
        memset(cursor, '`', run);
        cursor += run;
    }
    *cursor = 0;
    context->input_length = total;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_CODE, 0, "Code") != 0)
        return -1;
    return pc_expect_text_is_input(context);
}

static int case_unclosed_links_a(pc_context *context) {
    return pc_literal_case(context, "[a](<b", 30000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_unclosed_links_b(pc_context *context) {
    return pc_literal_case(context, "[a](b", 30000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_unclosed_comment(pc_context *context) {
    if (pc_build(context, "</", "<!--", 300000, NULL) != 0)
        return -1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    return pc_expect_text_is_input(context);
}

static int case_tables(pc_context *context) {
    const markdown_core_node *root;
    const markdown_core_node *paragraph;
    markdown_core_string_view view;
    if (pc_build(context, NULL, "aaa\rbbb\n-\x0b\n", 30000, NULL) != 0)
        return -1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_TABLE, 1, "Table") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_TABLE_ROW, 89998, "TableRow") != 0)
        return -1;
    root = markdown_core_document_root(context->document);
    paragraph = markdown_core_node_get_first_child(root);
    if (markdown_core_node_get_kind(paragraph) != MARKDOWN_CORE_KIND_PARAGRAPH ||
        !markdown_core_node_literal(markdown_core_node_get_first_child(paragraph), &view) || view.length != 3 ||
        memcmp(view.data, "aaa", 3) != 0) {
        fprintf(stderr, "leading paragraph is not the literal 'aaa'\n");
        return -1;
    }
    return 0;
}

/* Port of the reference-map hash collision generator. */
static int pc_badhash(const char *key) {
    uint32_t h = 0;
    const char *cursor;
    for (cursor = key; *cursor; cursor++) {
        uint32_t a = h << 6;
        uint32_t b = h << 16;
        h = (uint32_t)*cursor + a + b - h;
    }
    return (h % 16) == 0;
}

typedef struct pc_uniform_text {
    const char *expected;
    size_t expected_length;
    size_t seen;
    int mismatch;
} pc_uniform_text;

static int pc_uniform_text_visit(const markdown_core_node *node, void *context) {
    pc_uniform_text *check = (pc_uniform_text *)context;
    if (markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_TEXT) {
        markdown_core_string_view view;
        check->seen++;
        if (!markdown_core_node_literal(node, &view) || view.length != check->expected_length ||
            memcmp(view.data, check->expected, view.length) != 0) {
            check->mismatch = 1;
            return 1;
        }
    }
    return 0;
}

static int case_reference_collisions(pc_context *context) {
    enum { COLLISIONS = 50000 };
    char bad_key[32] = "";
    char key[32];
    size_t found = 0;
    unsigned long candidate = 0;
    char expected_text[64];
    pc_uniform_text check;

    {
        size_t capacity = 1 << 20;
        size_t length = 0;
        char *buffer = (char *)malloc(capacity);
        if (!buffer)
            return -1;
        while (found < COLLISIONS) {
            snprintf(key, sizeof(key), "x%lu", candidate++);
            if (!pc_badhash(key))
                continue;
            found++;
            if (found == 1) {
                snprintf(bad_key, sizeof(bad_key), "%s", key);
                continue;
            }
            {
                char entry[96];
                int entry_length = snprintf(entry, sizeof(entry), "[%s]: /url\n\n[%s]\n\n", key, bad_key);
                if (length + (size_t)entry_length + 1 > capacity) {
                    char *grown;
                    capacity *= 2;
                    grown = (char *)realloc(buffer, capacity);
                    if (!grown) {
                        free(buffer);
                        return -1;
                    }
                    buffer = grown;
                }
                memcpy(buffer + length, entry, (size_t)entry_length);
                length += (size_t)entry_length;
            }
        }
        buffer[length] = 0;
        context->input = buffer;
        context->input_length = length;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_PARAGRAPH, COLLISIONS - 1, "Paragraph") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 0, "Link") != 0)
        return -1;
    snprintf(expected_text, sizeof(expected_text), "[%s]", bad_key);
    check.expected = expected_text;
    check.expected_length = strlen(expected_text);
    check.seen = 0;
    check.mismatch = 0;
    if (ts_ast_walk(markdown_core_document_root(context->document), pc_uniform_text_visit, &check) < 0 ||
        check.mismatch || check.seen != COLLISIONS - 1) {
        fprintf(stderr, "unresolved references are not uniform literal text\n");
        return -1;
    }
    return 0;
}

/* Directive pathological cases -------------------------------------------- */

static int pc_directive_literal_case(pc_context *context, const char *unit, size_t count) {
    if (pc_build(context, NULL, unit, count, NULL) != 0)
        return -1;
    if (pc_parse(context, PC_DIRECTIVE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 0, "Directive") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, 0, "DirectiveBlock") != 0)
        return -1;
    return pc_expect_text_is_input(context);
}

static int case_directive_unclosed_labels(pc_context *context) {
    return pc_directive_literal_case(context, ":x[", 20000);
}

static int case_directive_unclosed_attributes(pc_context *context) {
    return pc_directive_literal_case(context, ":x{", 20000);
}

static int case_directive_colon_pairs(pc_context *context) { return pc_directive_literal_case(context, "::", 40000); }

static const markdown_core_node *pc_first_directive(const pc_context *context) {
    const markdown_core_node *root = markdown_core_document_root(context->document);
    const markdown_core_node *paragraph = markdown_core_node_get_first_child(root);
    return markdown_core_node_get_first_child(paragraph);
}

static int case_directive_long_label(pc_context *context) {
    const markdown_core_node *directive;
    const markdown_core_node *label;
    markdown_core_placement_mode mode;
    markdown_core_string_view name;
    markdown_core_string_view attributes;
    markdown_core_string_view literal;
    bool has_label = false;
    size_t label_count = 0;
    char *expected = NULL;

    if (pc_build(context, ":long[", "a", 1500, "]") != 0)
        return -1;
    if (pc_parse(context, PC_DIRECTIVE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 1, "Directive") != 0)
        return -1;
    directive = pc_first_directive(context);
    if (markdown_core_node_get_kind(directive) != MARKDOWN_CORE_KIND_DIRECTIVE ||
        !markdown_core_node_directive_properties(directive, &mode, &name, &attributes, &has_label, &label_count) ||
        name.length != 4 || memcmp(name.data, "long", 4) != 0 || !has_label || label_count != 1 ||
        mode != MARKDOWN_CORE_PLACEMENT_EMBEDDED) {
        fprintf(stderr, "directive name/label/mode properties are wrong\n");
        return -1;
    }
    label = markdown_core_node_directive_first_label_child(directive);
    expected = ts_repeat("a", 1500, NULL);
    if (!expected)
        return -1;
    if (markdown_core_node_get_kind(label) != MARKDOWN_CORE_KIND_TEXT || !markdown_core_node_literal(label, &literal) ||
        literal.length != 1500 || memcmp(literal.data, expected, 1500) != 0) {
        fprintf(stderr, "directive label text is wrong\n");
        free(expected);
        return -1;
    }
    free(expected);
    return 0;
}

static int case_directive_long_attributes(pc_context *context) {
    const markdown_core_node *directive;
    markdown_core_placement_mode mode;
    markdown_core_string_view name;
    markdown_core_string_view attributes;
    bool has_label = false;
    size_t label_count = 0;
    char *value;
    char *expected;
    size_t expected_length;
    int result = -1;

    if (pc_build(context, ":long{data-x=\"", "a", 5000, "\"}") != 0)
        return -1;
    if (pc_parse(context, PC_DIRECTIVE_ONLY) != 0)
        return -1;
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 1, "Directive") != 0)
        return -1;
    directive = pc_first_directive(context);
    if (!markdown_core_node_directive_properties(directive, &mode, &name, &attributes, &has_label, &label_count) ||
        name.length != 4 || memcmp(name.data, "long", 4) != 0) {
        fprintf(stderr, "directive name properties are wrong\n");
        return -1;
    }
    value = ts_repeat("a", 5000, NULL);
    if (!value)
        return -1;
    expected_length = strlen("{\"data-x\":\"\"}") + 5000;
    expected = (char *)malloc(expected_length + 1);
    if (expected) {
        snprintf(expected, expected_length + 1, "{\"data-x\":\"%s\"}", value);
        if (attributes.data && attributes.length == expected_length &&
            memcmp(attributes.data, expected, expected_length) == 0) {
            result = 0;
        } else {
            fprintf(stderr, "directive attributes JSON is wrong\n");
        }
    }
    free(value);
    free(expected);
    return result;
}

/* Inline delimiter stack cases --------------------------------------------- */

static int pc_formula_case(pc_context *context, const char *prefix, const char *unit, size_t count, const char *suffix,
                           size_t expected_formulas, const char *expected_literal) {
    if (pc_build(context, prefix, unit, count, suffix) != 0)
        return -1;
    if (pc_parse(context, PC_FORMULA) != 0)
        return -1;
    if (expected_formulas != (size_t)-1 &&
        pc_expect_count(context, MARKDOWN_CORE_KIND_FORMULA, expected_formulas, "Formula") != 0)
        return -1;
    if (expected_literal) {
        const markdown_core_node *root = markdown_core_document_root(context->document);
        const markdown_core_node *paragraph = markdown_core_node_get_first_child(root);
        const markdown_core_node *formula = markdown_core_node_get_first_child(paragraph);
        markdown_core_placement_mode mode;
        markdown_core_string_view literal;
        size_t expected_length = strlen(expected_literal);
        if (markdown_core_node_get_kind(formula) != MARKDOWN_CORE_KIND_FORMULA ||
            !markdown_core_node_formula_properties(formula, &mode, &literal) ||
            mode != MARKDOWN_CORE_PLACEMENT_EMBEDDED || literal.length != expected_length ||
            memcmp(literal.data, expected_literal, expected_length) != 0) {
            fprintf(stderr, "formula literal/mode properties are wrong\n");
            return -1;
        }
    }
    return 0;
}

static int case_formula_long_dollar(pc_context *context) {
    char *body = ts_repeat("x", 5000, NULL);
    int result;
    if (!body)
        return -1;
    result = pc_formula_case(context, "$", "x", 5000, "$", 1, body);
    free(body);
    return result;
}

static int case_formula_long_backslash(pc_context *context) {
    char *body = ts_repeat("x", 5000, NULL);
    int result;
    if (!body)
        return -1;
    result = pc_formula_case(context, "\\\\(", "x", 5000, "\\\\)", 1, body);
    free(body);
    return result;
}

static int case_formula_dollar_backtick(pc_context *context) {
    return pc_formula_case(context, "$`x", " $`x", 19999, NULL, (size_t)-1, NULL);
}

static int case_formula_backslash_openers(pc_context *context) {
    return pc_formula_case(context, "\\\\(x", " \\\\(x", 19999, NULL, (size_t)-1, NULL);
}

/* Registry ------------------------------------------------------------------ */

typedef struct pc_case_entry {
    const char *name;
    int (*run)(pc_context *context);
} pc_case_entry;

static const pc_case_entry PC_CASES[] = {
    {"nested_strong_emph", case_nested_strong_emph},
    {"many_emph_closers", case_emph_closers},
    {"many_emph_openers", case_emph_openers},
    {"many_link_closers", case_link_closers},
    {"many_link_openers", case_link_openers},
    {"mismatched_openers_closers", case_mismatched_openers_closers},
    {"openers_closers_multiple_of_3", case_openers_closers_multiple_of_3},
    {"link_openers_emph_closers", case_link_openers_emph_closers},
    {"pattern_bracket_paren", case_pattern_bracket_paren},
    {"pattern_image_link", case_pattern_image_link},
    {"hard_link_emph", case_hard_link_emph},
    {"nested_brackets", case_nested_brackets},
    {"nested_block_quotes", case_nested_block_quotes},
    {"deeply_nested_lists", case_deeply_nested_lists},
    {"nul_in_input", case_nul_in_input},
    {"backticks", case_backticks},
    {"unclosed_links_a", case_unclosed_links_a},
    {"unclosed_links_b", case_unclosed_links_b},
    {"unclosed_comment", case_unclosed_comment},
    {"tables", case_tables},
    {"reference_collisions", case_reference_collisions},
    {"directive_unclosed_labels", case_directive_unclosed_labels},
    {"directive_unclosed_attributes", case_directive_unclosed_attributes},
    {"directive_colon_pairs", case_directive_colon_pairs},
    {"directive_long_label", case_directive_long_label},
    {"directive_long_attributes", case_directive_long_attributes},
    {"formula_long_dollar", case_formula_long_dollar},
    {"formula_long_backslash", case_formula_long_backslash},
    {"formula_dollar_backtick_openers", case_formula_dollar_backtick},
    {"formula_backslash_openers", case_formula_backslash_openers},
};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    size_t i;
    int list_only = 0;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0)
            list_only = 1;
        else if (strcmp(argv[i], "--case") == 0 && i + 1 < (size_t)argc)
            case_name = argv[++i];
        else {
            fputs("usage: pathological_runner [--list] [--case NAME]\n", stderr);
            return 2;
        }
    }

    if (list_only) {
        for (i = 0; i < sizeof(PC_CASES) / sizeof(PC_CASES[0]); i++)
            puts(PC_CASES[i].name);
        return 0;
    }
    if (!case_name) {
        fputs("usage: pathological_runner [--list] [--case NAME]\n", stderr);
        return 2;
    }

    for (i = 0; i < sizeof(PC_CASES) / sizeof(PC_CASES[0]); i++) {
        if (strcmp(PC_CASES[i].name, case_name) == 0) {
            pc_context context;
            int result;
            memset(&context, 0, sizeof(context));
            result = PC_CASES[i].run(&context);
            free(context.input);
            markdown_core_document_free(context.document);
            if (result == 0) {
                printf("%s [PASSED]\n", case_name);
                return 0;
            }
            printf("%s [FAILED]\n", case_name);
            return 1;
        }
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
