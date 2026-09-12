#include "mark.h"
#include "inline_internal.h"
static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    if (character != '=') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_parser);
}
const markdown_core_extension MARKDOWN_CORE_EXTENSION_MARK = {
    .name = "mark",
    .delimiter_rule = MARKDOWN_CORE_DELIM_RULE_MARK,
    .delimiter_character = '=',
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_MARK},
    .match_inline = match,
    .terminates_text = "=",
    .dispatch = "=",
};
