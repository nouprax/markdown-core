#include "inline_internal.h"
#include "strikethrough.h"
#include "extension.h"
#include <parser.h>
#include <limits.h>

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    if (character != '~' || markdown_core_inline_parser_peek_at(
                                inline_parser, markdown_core_inline_parser_get_offset(inline_parser) + 1) != '~') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_parser);
}

static const char *get_type_string(const markdown_core_extension *extension, markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_STRIKETHROUGH ? "strikethrough" : "<unknown>";
}

static int can_contain(const markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->kind != MARKDOWN_CORE_NODE_STRIKETHROUGH) {
        return false;
    }

    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
}

/* `~` is the ONE byte in this repository that is genuinely
 * flanking-transparent, and it must stay so: it is inherited from cmark-gfm,
 * it behaves identically there, and upstream parity breaks without it. */
const markdown_core_extension MARKDOWN_CORE_EXTENSION_STRIKETHROUGH = {
    .name = "strikethrough",
    .delimiter_rule = MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH,
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_STRIKETHROUGH,
                  .exact_run = true},
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .match_inline = match,
    .terminates_text = "~",
    .dispatch = "~",
    .flanking_transparent = "~",
};
