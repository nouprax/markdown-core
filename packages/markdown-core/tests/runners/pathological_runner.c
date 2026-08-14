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
 *
 * The append_* cases replay adversarial inputs through successive appends
 * via the shared replay harness: every append checks the document dump
 * against a one-shot parse of the same text and double-walks the
 * predecessor and successor trees against the id ledger.  The canonical dump is
 * iterative like every other traversal; append-case depths are bounded
 * only by the dump volume the per-append verification materializes (dump
 * bytes grow quadratically with depth), never by a stack budget.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "append_replay.h"
#include "test_support.h"

typedef struct pc_context {
    char *input;
    size_t input_length;
    markdown_core_document *document;
    size_t counts[TS_KIND_COUNT];
} pc_context;

static const char *const PC_TABLE_ONLY[] = {"table", NULL};
static const char *const PC_AUTOLINK_ONLY[] = {"autolink", NULL};
static const char *const PC_DIRECTIVE_ONLY[] = {"directive", NULL};
static const char *const PC_FORMULA[] = {"formula", NULL};

/* Builds prefix + unit*count + suffix into context->input. */
static int pc_build(pc_context *context, const char *prefix, const char *unit, size_t count, const char *suffix) {
    size_t prefix_length = prefix ? strlen(prefix) : 0;
    size_t suffix_length = suffix ? strlen(suffix) : 0;
    size_t unit_length = strlen(unit);
    size_t total = prefix_length + unit_length * count + suffix_length;
    char *buffer = (char *)malloc(total + 1);
    size_t i;
    if (!buffer) {
        return -1;
    }
    if (prefix_length) {
        memcpy(buffer, prefix, prefix_length);
    }
    for (i = 0; i < count; i++) {
        memcpy(buffer + prefix_length + i * unit_length, unit, unit_length);
    }
    if (suffix_length) {
        memcpy(buffer + prefix_length + unit_length * count, suffix, suffix_length);
    }
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
        if (ts_ast_enable(&options, option_names[i]) != 0) {
            return -1;
        }
    }
    context->document = ts_ast_parse((const uint8_t *)context->input, context->input_length, &options);
    if (!context->document) {
        return -1;
    }
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
    if (!actual) {
        return -1;
    }
    if (actual_length != expected_length || memcmp(actual, expected, expected_length) != 0) {
        fprintf(
            stderr,
            "concatenated text differs from expected (%zu vs %zu bytes, first bytes: %.40s)\n",
            actual_length,
            expected_length,
            actual
        );
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
           (context->input[expected_length - 1] == ' ' || context->input[expected_length - 1] == '\t')) {
        expected_length--;
    }
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
    if (result != 0 || pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 65000, "Emphasis") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 65000, "Strong") != 0) {
        return -1;
    }
    return 0;
}

static int pc_literal_case(
    pc_context *context,
    const char *unit,
    size_t count,
    markdown_core_node_kind forbidden_kind,
    const char *forbidden_name
) {
    if (pc_build(context, NULL, unit, count, NULL) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, forbidden_kind, 0, forbidden_name) != 0) {
        return -1;
    }
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
    if (pc_literal_case(context, "*a_ ", 50000, MARKDOWN_CORE_KIND_EMPHASIS, "Emphasis") != 0) {
        return -1;
    }
    return pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 0, "Strong");
}

static int case_openers_closers_multiple_of_3(pc_context *context) {
    if (pc_build(context, "a**b", "c* ", 50000, NULL) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 0, "Emphasis") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 0, "Strong") != 0) {
        return -1;
    }
    return pc_expect_text_is_input(context);
}

static int case_link_openers_emph_closers(pc_context *context) {
    if (pc_literal_case(context, "[ a_", 50000, MARKDOWN_CORE_KIND_LINK, "Link") != 0) {
        return -1;
    }
    return pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 0, "Emphasis");
}

static int case_pattern_bracket_paren(pc_context *context) {
    return pc_literal_case(context, "[ (](", 80000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_pattern_image_link(pc_context *context) {
    char *expected;
    size_t expected_length = 0;
    int result;
    if (pc_build(context, NULL, "![[]()", 160000, NULL) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 160000, "Link") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_IMAGE, 0, "Image") != 0) {
        return -1;
    }
    expected = ts_repeat("![", 160000, &expected_length);
    if (!expected) {
        return -1;
    }
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
    markdown_core_string view;
    markdown_core_string title;

    if (pc_build(context, "**x [a*b**c*](d)", "", 0, NULL) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 1, "Link") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_EMPHASIS, 1, "Emphasis") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_STRONG, 0, "Strong") != 0) {
        return -1;
    }

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
    if (pc_build(context, NULL, "[", 50000, "a") != 0) {
        return -1;
    }
    closers = ts_repeat("]", 50000, NULL);
    if (!closers) {
        return -1;
    }
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
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 0, "Link") != 0) {
        return -1;
    }
    return pc_expect_text_is_input(context);
}

static int case_nested_block_quotes(pc_context *context) {
    if (pc_build(context, NULL, "> ", 50000, "a") != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_BLOCK_QUOTE, 50000, "BlockQuote") != 0) {
        return -1;
    }
    return pc_expect_text(context, "a", 1);
}

/* Builds one "* a" item per level, indented two spaces per depth. */
static int pc_build_nested_list(pc_context *context, size_t levels) {
    size_t depth;
    size_t total = 0;
    char *cursor;
    for (depth = 0; depth < levels; depth++) {
        total += depth * 2 + 4; /* "  "*depth + "* a\n" */
    }
    context->input = (char *)malloc(total + 1);
    if (!context->input) {
        return -1;
    }
    cursor = context->input;
    for (depth = 0; depth < levels; depth++) {
        memset(cursor, ' ', depth * 2);
        cursor += depth * 2;
        memcpy(cursor, "* a\n", 4);
        cursor += 4;
    }
    *cursor = 0;
    context->input_length = total;
    return 0;
}

static int case_deeply_nested_lists(pc_context *context) {
    char *expected;
    size_t expected_length = 0;
    int result;
    if (pc_build_nested_list(context, 1000) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LIST, 1000, "List") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_LIST_ITEM, 1000, "ListItem") != 0) {
        return -1;
    }
    expected = ts_repeat("a", 1000, &expected_length);
    if (!expected) {
        return -1;
    }
    result = pc_expect_text(context, expected, expected_length);
    free(expected);
    return result;
}

static int case_nul_in_input(pc_context *context) {
    static const char raw[] = "abc\0de\0";
    static const char expected[] = "abc\xEF\xBF\xBD"
                                   "de\xEF\xBF\xBD";
    context->input = (char *)malloc(sizeof(raw));
    if (!context->input) {
        return -1;
    }
    memcpy(context->input, raw, sizeof(raw));
    context->input_length = sizeof(raw) - 1;
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    return pc_expect_text(context, expected, sizeof(expected) - 1);
}

/* Builds "e" + backtick runs of every length in [1, max_run). */
static int pc_build_backtick_runs(pc_context *context, size_t max_run) {
    size_t run, total = 0;
    char *cursor;
    for (run = 1; run < max_run; run++) {
        total += 1 + run;
    }
    context->input = (char *)malloc(total + 1);
    if (!context->input) {
        return -1;
    }
    cursor = context->input;
    for (run = 1; run < max_run; run++) {
        *cursor++ = 'e';
        memset(cursor, '`', run);
        cursor += run;
    }
    *cursor = 0;
    context->input_length = total;
    return 0;
}

static int case_backticks(pc_context *context) {
    if (pc_build_backtick_runs(context, 5000) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_CODE, 0, "Code") != 0) {
        return -1;
    }
    return pc_expect_text_is_input(context);
}

static int case_unclosed_links_a(pc_context *context) {
    return pc_literal_case(context, "[a](<b", 30000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_unclosed_links_b(pc_context *context) {
    return pc_literal_case(context, "[a](b", 30000, MARKDOWN_CORE_KIND_LINK, "Link");
}

static int case_unclosed_comment(pc_context *context) {
    if (pc_build(context, "</", "<!--", 300000, NULL) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    return pc_expect_text_is_input(context);
}

static int case_tables(pc_context *context) {
    const markdown_core_node *root;
    const markdown_core_node *paragraph;
    markdown_core_string view;
    if (pc_build(context, NULL, "aaa\rbbb\n-\x0b\n", 30000, NULL) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_TABLE, 1, "Table") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_TABLE_ROW, 89998, "TableRow") != 0) {
        return -1;
    }
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
        markdown_core_string view;
        check->seen++;
        if (!markdown_core_node_literal(node, &view) || view.length != check->expected_length ||
            memcmp(view.data, check->expected, view.length) != 0) {
            check->mismatch = 1;
            return 1;
        }
    }
    return 0;
}

/* Builds `collisions` colliding-label entries; every entry defines its own
 * label and references the first colliding key, which stays undefined and
 * is written to `bad_key`. */
static int pc_build_reference_collisions(pc_context *context, size_t collisions, char bad_key[32]) {
    char key[32];
    size_t found = 0;
    unsigned long candidate = 0;
    size_t capacity = 1 << 20;
    size_t length = 0;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return -1;
    }
    bad_key[0] = 0;
    while (found < collisions) {
        snprintf(key, sizeof(key), "x%lu", candidate++);
        if (!pc_badhash(key)) {
            continue;
        }
        found++;
        if (found == 1) {
            snprintf(bad_key, 32, "%s", key);
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
    return 0;
}

static int case_reference_collisions(pc_context *context) {
    enum { COLLISIONS = 50000 };
    char bad_key[32] = "";
    char expected_text[64];
    pc_uniform_text check;

    if (pc_build_reference_collisions(context, COLLISIONS, bad_key) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_TABLE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_PARAGRAPH, COLLISIONS - 1, "Paragraph") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, 0, "Link") != 0) {
        return -1;
    }
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

/* One text node with 50000 email links.  Source-position bookkeeping must
 * advance with the links instead of rescanning the whole prefix and suffix
 * for every split; the sibling complexity case pins doubling behavior. */
static int case_many_email_autolinks(pc_context *context) {
    enum { EMAILS = 50000 };

    if (pc_build(context, NULL, "a@b.c ", EMAILS, "tail") != 0) {
        return -1;
    }
    if (pc_parse(context, PC_AUTOLINK_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_LINK, EMAILS, "Link") != 0) {
        return -1;
    }
    return pc_expect_text_is_input(context);
}

/* Directive pathological cases -------------------------------------------- */

static int pc_directive_literal_case(pc_context *context, const char *unit, size_t count) {
    if (pc_build(context, NULL, unit, count, NULL) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_DIRECTIVE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 0, "Directive") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, 0, "DirectiveBlock") != 0) {
        return -1;
    }
    return pc_expect_text_is_input(context);
}

/* `:x[` is a directive named `x` followed by a literal `[`: the label opener is
 * emitted after the directive, so an opener the delimiter engine never pairs
 * leaves only the bracket behind (see extensions/directive.c). The adversarial
 * property is unchanged — the parse stays linear and every input byte survives
 * — but the node counts follow the corrected semantics. */
static int case_directive_unclosed_labels(pc_context *context) {
    char *expected;
    size_t expected_length = 0;
    int result;

    if (pc_build(context, NULL, ":x[", 20000, NULL) != 0 || pc_parse(context, PC_DIRECTIVE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 20000, "Directive") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, 0, "DirectiveBlock") != 0) {
        return -1;
    }
    expected = ts_repeat("[", 20000, &expected_length);
    if (!expected) {
        return -1;
    }
    result = pc_expect_text(context, expected, expected_length);
    free(expected);
    return result;
}

/* `:x{` is a directive named `x` followed by a literal `{`: an attribute block
 * that does not parse leaves the name standing, as it does in
 * micromark-extension-directive (see extensions/directive.c). The adversarial
 * property this case guards is unchanged — the parse stays linear and every
 * input byte survives — but the node counts follow the corrected semantics
 * rather than the old all-text one. */
static int case_directive_unclosed_attributes(pc_context *context) {
    char *expected;
    size_t expected_length = 0;
    int result;

    if (pc_build(context, NULL, ":x{", 20000, NULL) != 0 || pc_parse(context, PC_DIRECTIVE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 20000, "Directive") != 0 ||
        pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, 0, "DirectiveBlock") != 0) {
        return -1;
    }
    expected = ts_repeat("{", 20000, &expected_length);
    if (!expected) {
        return -1;
    }
    result = pc_expect_text(context, expected, expected_length);
    free(expected);
    return result;
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
    const markdown_core_node *label_text;
    markdown_core_placement_mode mode;
    markdown_core_string name;
    bool has_attributes = false;
    markdown_core_string literal;
    char *expected = NULL;

    if (pc_build(context, ":long[", "a", 1500, "]") != 0) {
        return -1;
    }
    if (pc_parse(context, PC_DIRECTIVE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 1, "Directive") != 0) {
        return -1;
    }
    directive = pc_first_directive(context);
    if (markdown_core_node_get_kind(directive) != MARKDOWN_CORE_KIND_DIRECTIVE ||
        !markdown_core_node_directive_properties(directive, &mode, &name, &has_attributes) || name.length != 4 ||
        memcmp(name.data, "long", 4) != 0 || mode != MARKDOWN_CORE_PLACEMENT_EMBEDDED) {
        fprintf(stderr, "directive name/label/mode properties are wrong\n");
        return -1;
    }
    label = markdown_core_node_directive_label(directive);
    label_text = markdown_core_node_get_first_child(label);
    expected = ts_repeat("a", 1500, NULL);
    if (!expected) {
        return -1;
    }
    if (markdown_core_node_get_kind(label) != MARKDOWN_CORE_KIND_DIRECTIVE_LABEL ||
        markdown_core_node_get_parent(label) != directive || markdown_core_node_child_count(label) != 1 ||
        markdown_core_node_get_kind(label_text) != MARKDOWN_CORE_KIND_TEXT ||
        markdown_core_node_get_parent(label_text) != label || !markdown_core_node_literal(label_text, &literal) ||
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
    markdown_core_string name;
    markdown_core_string key;
    markdown_core_string attribute;
    bool has_attributes = false;
    size_t count = 0;
    char *value;
    int result = -1;

    if (pc_build(context, ":long{data-x=\"", "a", 5000, "\"}") != 0) {
        return -1;
    }
    if (pc_parse(context, PC_DIRECTIVE_ONLY) != 0) {
        return -1;
    }
    if (pc_expect_count(context, MARKDOWN_CORE_KIND_DIRECTIVE, 1, "Directive") != 0) {
        return -1;
    }
    directive = pc_first_directive(context);
    if (!markdown_core_node_directive_properties(directive, &mode, &name, &has_attributes) || name.length != 4 ||
        memcmp(name.data, "long", 4) != 0 || !has_attributes) {
        fprintf(stderr, "directive name properties are wrong\n");
        return -1;
    }
    value = ts_repeat("a", 5000, NULL);
    if (!value) {
        return -1;
    }
    markdown_core_node_directive_attribute_count(directive, &count);
    if (count == 1 && markdown_core_node_directive_attribute_at(directive, 0, &key, &attribute) &&
        key.length == strlen("data-x") && memcmp(key.data, "data-x", key.length) == 0 && attribute.length == 5000 &&
        memcmp(attribute.data, value, 5000) == 0) {
        result = 0;
    } else {
        fprintf(stderr, "directive attributes are wrong\n");
    }
    free(value);
    return result;
}

/* Inline delimiter stack cases --------------------------------------------- */

static int pc_formula_case(
    pc_context *context,
    const char *prefix,
    const char *unit,
    size_t count,
    const char *suffix,
    size_t expected_formulas,
    const char *expected_literal
) {
    if (pc_build(context, prefix, unit, count, suffix) != 0) {
        return -1;
    }
    if (pc_parse(context, PC_FORMULA) != 0) {
        return -1;
    }
    if (expected_formulas != (size_t)-1 &&
        pc_expect_count(context, MARKDOWN_CORE_KIND_FORMULA, expected_formulas, "Formula") != 0) {
        return -1;
    }
    if (expected_literal) {
        const markdown_core_node *root = markdown_core_document_root(context->document);
        const markdown_core_node *paragraph = markdown_core_node_get_first_child(root);
        const markdown_core_node *formula = markdown_core_node_get_first_child(paragraph);
        markdown_core_placement_mode mode;
        markdown_core_string literal;
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
    if (!body) {
        return -1;
    }
    result = pc_formula_case(context, "$", "x", 5000, "$", 1, body);
    free(body);
    return result;
}

static int case_formula_long_backslash(pc_context *context) {
    char *body = ts_repeat("x", 5000, NULL);
    int result;
    if (!body) {
        return -1;
    }
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

/* Document pathological cases ------------------------------------------------
 *
 * Adversarial structures replayed through successive appends.  The shared
 * harness verifies every append in full (dump equality against a one-shot
 * parse, identity double-walk, footnote-query equivalence), so these cases
 * only add the structural probes that document each attack; the CTest
 * TIMEOUT bounds an append whose cost degenerates against the structure even
 * when it stays correct.  Every append re-parses all bytes so far, so a tail
 * append IS the attack: the adversarial prefix is re-run in whole per tick.
 */

static int pe_failures;

static void pe_report(void *user, const char *context, const char *message) {
    (void)user;
    fprintf(stderr, "FAILED: %s: %s\n", context, message);
    pe_failures++;
}

static int pe_open(er_replay *replay, const char *context, const markdown_core_parse_options *options) {
    return er_replay_open(replay, context, options, pe_report, NULL);
}

/* One verified append of a NUL-terminated tail. */
static int pe_append(er_replay *replay, const char *text) {
    return er_replay_append(replay, (const uint8_t *)text, strlen(text));
}

/* The adversarial prefix arrives as one verified append of its own. */
static int pe_append_input(er_replay *replay, const pc_context *context) {
    return er_replay_append(replay, (const uint8_t *)context->input, context->input_length);
}

static int pe_expect_kind(const er_replay *replay, markdown_core_node_kind kind, size_t expected, const char *what) {
    size_t counts[TS_KIND_COUNT];
    const markdown_core_document *document = replay->document;
    if (!document || ts_ast_count_kinds(markdown_core_document_root(document), counts) != 0) {
        fprintf(stderr, "cannot count node kinds in the appended document\n");
        return -1;
    }
    if ((size_t)kind >= TS_KIND_COUNT) {
        fprintf(stderr, "node kind %d is outside TS_KIND_COUNT; the table is stale\n", (int)kind);
        return -1;
    }
    if (counts[kind] != expected) {
        fprintf(stderr, "expected %zu %s node(s) in the appended document, found %zu\n", expected, what, counts[kind]);
        return -1;
    }
    return 0;
}

/* A 48 KiB single paragraph of emphasis openers: every append reparses the
 * whole inline run, and the delimiter stack must not leak state between
 * appends.  Each appended closer right-flanks against 16384 candidate
 * openers. */
static int case_append_emph_openers(pc_context *context) {
    markdown_core_parse_options options;
    er_replay replay;
    int result = -1;

    if (pc_build(context, NULL, "_a ", 16384, NULL) != 0) {
        return -1;
    }
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_emph_openers", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_EMPHASIS, 0, "Emphasis") != 0) {
        goto done;
    }
    /* "b_" right-flanks: one pair matches against the opener wall. */
    if (pe_append(&replay, "b_") != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_EMPHASIS, 1, "Emphasis") != 0) {
        goto done;
    }
    if (pe_append(&replay, " c_") != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_EMPHASIS, 2, "Emphasis") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* Backtick runs of every length below 1200 (~700 KiB) never close a span;
 * an appended lone backtick closes exactly the length-1 run and reflows the
 * candidate pairing across every run behind it. */
static int case_append_backtick_runs(pc_context *context) {
    markdown_core_parse_options options;
    er_replay replay;
    int result = -1;

    if (pc_build_backtick_runs(context, 1200) != 0) {
        return -1;
    }
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_backtick_runs", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_CODE, 0, "Code") != 0) {
        goto done;
    }
    /* The space keeps the new backtick a run of its own; it closes the
     * length-1 run and the span swallows every run between them. */
    if (pe_append(&replay, " `") != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_CODE, 1, "Code") != 0) {
        goto done;
    }
    /* A self-contained pair appended after the giant span still reflows the
     * whole candidate pairing behind it. */
    if (pe_append(&replay, " ``x``") != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_CODE, 2, "Code") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* 4096-deep block quotes.  Depth here is bounded by dump volume, not by
 * any stack: the replay harness verifies every append with two canonical
 * dumps whose per-line prefixes grow with depth, so dump bytes are
 * quadratic in depth (50000 deep would be ~5 GiB per dump).  Stack safety
 * at adversarial depth is pinned separately by the small-stack dump case
 * in the concurrency runner and the 50000-deep structural one-shot cases.
 * The open chain spans the whole document, so every appended byte re-runs
 * container matching over all 4096 levels. */
static int case_append_quotes_deep(pc_context *context) {
    enum { QUOTE_DEPTH = 4096 };
    markdown_core_parse_options options;
    er_replay replay;
    int result = -1;

    if (pc_build(context, NULL, "> ", QUOTE_DEPTH, "a") != 0) {
        return -1;
    }
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_quotes_deep", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_BLOCK_QUOTE, QUOTE_DEPTH, "BlockQuote") != 0) {
        goto done;
    }
    /* Extends the innermost line, then continues it lazily: both rides
     * re-match the whole 4096-marker chain. */
    if (pe_append(&replay, "b") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_BLOCK_QUOTE, QUOTE_DEPTH, "BlockQuote") != 0) {
        goto done;
    }
    if (pe_append(&replay, "\nlazy") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_BLOCK_QUOTE, QUOTE_DEPTH, "BlockQuote") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 1, "Paragraph") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* 512-deep list nesting (~260 KiB of indentation): each appended lazy
 * continuation re-runs container matching down the whole spine. */
static int case_append_list_spine(pc_context *context) {
    markdown_core_parse_options options;
    er_replay replay;
    int result = -1;

    if (pc_build_nested_list(context, 512) != 0) {
        return -1;
    }
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_list_spine", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_LIST, 512, "List") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_LIST_ITEM, 512, "ListItem") != 0) {
        goto done;
    }
    if (pe_append(&replay, "lazy one\n") != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_LIST, 512, "List") != 0) {
        goto done;
    }
    if (pe_append(&replay, "lazy two\n") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_LIST_ITEM, 512, "ListItem") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* 2048 colliding reference definitions, every entry referencing one shared
 * undefined label: appending that label's definition turns 2047 of them
 * into reference nodes across the whole document in one append — maximal
 * cross-document re-resolution against a collision-saturated reference map.
 * A duplicate definition appended after it must change nothing: the first
 * definition keeps winning while the duplicate's paragraph vanishes. */
static int case_append_reference_collisions(pc_context *context) {
    enum { COLLISIONS = 2048 };
    markdown_core_parse_options options;
    er_replay replay;
    char bad_key[32] = "";
    char definition[64];
    int result = -1;

    if (pc_build_reference_collisions(context, COLLISIONS, bad_key) != 0) {
        return -1;
    }
    snprintf(definition, sizeof(definition), "[%s]: /t\n", bad_key);
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_reference_collisions", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_LINK_REFERENCE, 0, "LinkReference") != 0) {
        goto done;
    }
    if (pe_append(&replay, definition) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_LINK_REFERENCE, COLLISIONS - 1, "LinkReference") != 0) {
        goto done;
    }
    if (pe_append(&replay, definition) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_LINK_REFERENCE, COLLISIONS - 1, "LinkReference") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* An unclosed fence ahead of 4096 one-line paragraphs swallows all of them;
 * the appended closing fence releases the tail of the stream again — the
 * streaming shape of the reparse-to-EOF degradation. */
static int case_append_fence_gate(pc_context *context) {
    markdown_core_parse_options options;
    er_replay replay;
    int result = -1;

    if (pc_build(context, NULL, "a\n\n", 4096, NULL) != 0) {
        return -1;
    }
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_fence_gate", &options) != 0) {
        return -1;
    }
    if (pe_append(&replay, "````\n") != 0 || pe_append_input(&replay, context) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_CODE_BLOCK, 1, "CodeBlock") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 0, "Paragraph") != 0) {
        goto done;
    }
    if (pe_append(&replay, "````\n") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_CODE_BLOCK, 1, "CodeBlock") != 0) {
        goto done;
    }
    if (pe_append(&replay, "after\n") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 1, "Paragraph") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* One quoted paragraph continued by 4096 lazy lines: every appended tail
 * line rides the whole lazy wall again, and the appended blank finally
 * evicts the continuation from the quote. */
static int case_append_lazy_wall(pc_context *context) {
    markdown_core_parse_options options;
    er_replay replay;
    int result = -1;

    if (pc_build(context, "> a\n", "b\n", 4096, NULL) != 0) {
        return -1;
    }
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_lazy_wall", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_BLOCK_QUOTE, 1, "BlockQuote") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 1, "Paragraph") != 0) {
        goto done;
    }
    if (pe_append(&replay, "c\n") != 0 || pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 1, "Paragraph") != 0) {
        goto done;
    }
    if (pe_append(&replay, "\n") != 0 || pe_append(&replay, "d\n") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 2, "Paragraph") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_BLOCK_QUOTE, 1, "BlockQuote") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* 4096 CRLF-separated paragraphs, then line endings split across appends:
 * a chunk ending in "\r" leaves half a pair on the seam and the next
 * append completes it — the ending must fuse across the mutation exactly
 * as a one-shot parse of the joined bytes fuses it. */
static int case_append_crlf_seam(pc_context *context) {
    markdown_core_parse_options options;
    er_replay replay;
    int result = -1;

    if (pc_build(context, NULL, "aa\r\n\r\n", 4096, NULL) != 0) {
        return -1;
    }
    markdown_core_parse_options_init(&options);
    if (pe_open(&replay, "append_crlf_seam", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 4096, "Paragraph") != 0) {
        goto done;
    }
    /* A lone trailing "\r" is an ending at EOF; the "\n" arriving next
     * must fuse into one CRLF rather than mint a blank line. */
    if (pe_append(&replay, "xx\r") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 4097, "Paragraph") != 0) {
        goto done;
    }
    /* A NULL chunk of no bytes, landing between the two halves of the pair.
     * It is a legal mutation — the contract forbids NULL only with a nonzero
     * length — and this is where it is sharpest: the stored bytes end with
     * the CR, so the parser meets the arriving chunk holding a pending seam,
     * and a feed that inspects a byte before checking that it has one reads
     * through NULL right here. */
    if (er_replay_append(&replay, NULL, 0) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 4097, "Paragraph") != 0) {
        goto done;
    }
    if (pe_append(&replay, "\n") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 4097, "Paragraph") != 0) {
        goto done;
    }
    if (pe_append(&replay, "\r\n") != 0 || pe_append(&replay, "yy\r\n") != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_PARAGRAPH, 4098, "Paragraph") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
}

/* 1024 footnote definitions whose labels need case folding, 256 references
 * from one head paragraph: every append re-interns and re-folds against
 * the saturated table, an appended tail reference resolves against it, and
 * a fresh definition grows it — with footnote-query equivalence run on
 * every append by the harness. */
static int case_append_footnote_labels(pc_context *context) {
    enum { DEFINITIONS = 1024, REFERENCES = 256 };
    markdown_core_parse_options options;
    er_replay replay;
    size_t i;
    int result = -1;

    {
        size_t capacity = 64 * DEFINITIONS + 16 * REFERENCES + 64;
        size_t length = 0;
        char *buffer = (char *)malloc(capacity);
        if (!buffer) {
            return -1;
        }
        length += (size_t)snprintf(buffer + length, capacity - length, "refs:");
        for (i = 0; i < REFERENCES && length + 64 <= capacity; i++) {
            length += (size_t)snprintf(buffer + length, capacity - length, " [^\xC3\x80\xD0\x91%04zu]", i * 4);
        }
        length += (size_t)snprintf(buffer + length, capacity - length, "\n\nplain\n\n");
        for (i = 0; i < DEFINITIONS && length + 64 <= capacity; i++) {
            length += (size_t)snprintf(buffer + length, capacity - length, "[^\xC3\x80\xD0\x91%04zu]: d%zu\n\n", i, i);
        }
        if (length + 64 > capacity) {
            free(buffer);
            return -1;
        }
        context->input = buffer;
        context->input_length = length;
    }
    markdown_core_parse_options_init(&options);
    options.footnotes = true;
    if (pe_open(&replay, "append_footnote_labels", &options) != 0) {
        return -1;
    }
    if (pe_append_input(&replay, context) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE, REFERENCES, "FootnoteReference") != 0) {
        goto done;
    }
    /* A tail reference to a defined label folds and resolves. */
    if (pe_append(
            &replay,
            "tail [^\xC3\x80\xD0\x91"
            "0008]\n\n"
        ) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE, REFERENCES + 1, "FootnoteReference") != 0) {
        goto done;
    }
    /* A fresh definition grows the interning table; nothing references it. */
    if (pe_append(
            &replay,
            "[^\xC3\x80\xD0\x91"
            "9999]: d\n"
        ) != 0 ||
        pe_expect_kind(&replay, MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE, REFERENCES + 1, "FootnoteReference") != 0) {
        goto done;
    }
    result = pe_failures ? -1 : 0;
done:
    er_replay_close(&replay);
    return result;
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
    {"many_email_autolinks", case_many_email_autolinks},
    {"directive_unclosed_labels", case_directive_unclosed_labels},
    {"directive_unclosed_attributes", case_directive_unclosed_attributes},
    {"directive_colon_pairs", case_directive_colon_pairs},
    {"directive_long_label", case_directive_long_label},
    {"directive_long_attributes", case_directive_long_attributes},
    {"formula_long_dollar", case_formula_long_dollar},
    {"formula_long_backslash", case_formula_long_backslash},
    {"formula_dollar_backtick_openers", case_formula_dollar_backtick},
    {"formula_backslash_openers", case_formula_backslash_openers},
    {"append_emph_openers", case_append_emph_openers},
    {"append_backtick_runs", case_append_backtick_runs},
    {"append_quotes_deep", case_append_quotes_deep},
    {"append_list_spine", case_append_list_spine},
    {"append_reference_collisions", case_append_reference_collisions},
    {"append_fence_gate", case_append_fence_gate},
    {"append_lazy_wall", case_append_lazy_wall},
    {"append_crlf_seam", case_append_crlf_seam},
    {"append_footnote_labels", case_append_footnote_labels},
};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    size_t i;
    int list_only = 0;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < (size_t)argc) {
            case_name = argv[++i];
        } else {
            fputs("usage: pathological_runner [--list] [--case NAME]\n", stderr);
            return 2;
        }
    }

    if (list_only) {
        for (i = 0; i < sizeof(PC_CASES) / sizeof(PC_CASES[0]); i++) {
            puts(PC_CASES[i].name);
        }
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
