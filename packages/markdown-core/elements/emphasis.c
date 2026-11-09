#include "emphasis.h"
#include "inline_internal.h"
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '*' && character != '_') {
        return NULL;
    }
    if (character != self->element->inlines->character) {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}

static const markdown_core_inline_ops EMPHASIS_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .rule = MARKDOWN_CORE_DELIM_RULE_EMPHASIS,
    .character = '*',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 2,
                  .rule_of_three = true,
                  .punctuation_bound = false,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match = match,
    .terminates = "*",
    .dispatch = "*",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS = {
    .name = "emphasis",
    .inlines = &EMPHASIS_INLINES,
};
static const markdown_core_inline_ops EMPHASIS_UNDERSCORE_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .rule = MARKDOWN_CORE_DELIM_RULE_UNDERSCORE,
    .character = '_',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 2,
                  .rule_of_three = true,
                  .punctuation_bound = true,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match = match,
    .terminates = "_",
    .dispatch = "_",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS_UNDERSCORE = {
    .name = "emphasis_underscore",
    .inlines = &EMPHASIS_UNDERSCORE_INLINES,
};
