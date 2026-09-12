#include "emphasis.h"
#include "inline_internal.h"
static markdown_core_node *match(const markdown_core_element *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '*' && character != '_') {
        return NULL;
    }
    if (character != self->delimiter_character) {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS = {
    .inline_precedence = MARKDOWN_CORE_INLINE_TOKEN,

    .name = "emphasis",
    .delimiter_rule = MARKDOWN_CORE_DELIM_RULE_EMPHASIS,
    .delimiter_character = '*',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 2,
                  .rule_of_three = true,
                  .punctuation_bound = false,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match_inline = match,
    .terminates_text = "*",
    .dispatch = "*",
};
const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS_UNDERSCORE = {
    .inline_precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .name = "emphasis_underscore",
    .delimiter_rule = MARKDOWN_CORE_DELIM_RULE_UNDERSCORE,
    .delimiter_character = '_',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 2,
                  .rule_of_three = true,
                  .punctuation_bound = true,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match_inline = match,
    .terminates_text = "_",
    .dispatch = "_",
};
