#include "subscript.h"
#include "inline_internal.h"
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '~') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}
static const markdown_core_inline_ops SUBSCRIPT_INLINES = {
    .rule = MARKDOWN_CORE_DELIM_RULE_SUBSCRIPT,
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 1,
                  .run_limit = 1,
                  .body = DELIMITER_WORD_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_SUBSCRIPT},
    .match = match,
    .terminates = "~",
    .dispatch = "~",
};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_SUBSCRIPT_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_SUBSCRIPT, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_SUBSCRIPT = {
    .facets = MARKDOWN_CORE_SUBSCRIPT_FACETS,
    .name = "subscript",
    .inlines = &SUBSCRIPT_INLINES,
};
