#include "subscript.h"
#include "inline_internal.h"
static markdown_core_node *match(const markdown_core_element *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '~') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_SUBSCRIPT = {
    .name = "subscript",
    .delimiter_rule = MARKDOWN_CORE_DELIM_RULE_SUBSCRIPT,
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 1,
                  .run_limit = 1,
                  .body = DELIMITER_WORD_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_SUBSCRIPT},
    .match_inline = match,
    .terminates_text = "~",
    .dispatch = "~",
};
