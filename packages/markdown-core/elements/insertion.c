#include "insertion.h"
#include "inline_internal.h"
static markdown_core_node *match(const markdown_core_element *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '+') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_INSERTION = {
    .name = "insertion",
    .delimiter_rule = MARKDOWN_CORE_DELIM_RULE_INSERTION,
    .delimiter_character = '+',
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_INSERTION},
    .match_inline = match,
    .terminates_text = "+",
    .dispatch = "+",
};
