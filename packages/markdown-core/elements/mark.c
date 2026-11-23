#include "mark.h"
#include "inline_internal.h"
#include "emphasis.h"

/* The elements whose grammar this element takes: emphasis's flanking of a
 * delimiter run (emphasis.h). */
static const markdown_core_element *const MARK_PEERS[] = {&MARKDOWN_CORE_ELEMENT_EMPHASIS, NULL};
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
                  .flank = markdown_core_emphasis_free,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_MARK},
    .match = match,
    .terminates = "=",
    .dispatch = "=",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_MARK = {
    .peers = MARK_PEERS,
    .name = "mark",
    .inlines = &MARK_INLINES,
};
