#include "mark.h"
#include "inline_internal.h"
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '=') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}
static const markdown_core_inline_ops MARK_INLINES = {
    .rule = MARKDOWN_CORE_DELIM_RULE_MARK,
    .character = '=',
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_MARK},
    .match = match,
    .terminates = "=",
    .dispatch = "=",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_MARK = {
    .name = "mark",
    .inlines = &MARK_INLINES,
};
