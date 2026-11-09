#include "inline_internal.h"
#include "strikethrough.h"
#include "element.h"
#include <parser.h>
#include <limits.h>

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '~' || markdown_core_inline_state_peek_at(
                                inline_state, markdown_core_inline_state_get_offset(inline_state) + 1) != '~') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}

static const markdown_core_node_type containment_kinds[] = {MARKDOWN_CORE_NODE_STRIKETHROUGH, MARKDOWN_CORE_NODE_NONE};

/* `~` is the ONE byte in this repository that is genuinely
 * flanking-transparent, and it must stay so: it is inherited from cmark-gfm,
 * it behaves identically there, and upstream parity breaks without it. */
static const markdown_core_contain_ops STRIKETHROUGH_CONTAIN = {
    .kinds = containment_kinds,
};

static const markdown_core_inline_ops STRIKETHROUGH_INLINES = {
    .rule = MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH,
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_STRIKETHROUGH,
                  .exact_run = true},
    .match = match,
    .terminates = "~",
    .dispatch = "~",
    .transparent = "~",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_STRIKETHROUGH = {
    .name = "strikethrough",
    .contain = &STRIKETHROUGH_CONTAIN,
    .inlines = &STRIKETHROUGH_INLINES,
};
