#include "insertion.h"
#include "inline_internal.h"
#include "services.h"

/* The services this element asks, as `self->needs` holds them: the flank
 * rule of a paired delimiter run (services.h). */
enum { INSERTION_FLANK };
static const char *const INSERTION_NEEDS[] = {[INSERTION_FLANK] = MARKDOWN_CORE_SERVICE_FLANK, NULL};
static unsigned S_flank(const markdown_core_element_instance *self, markdown_core_flank flank) {
    return markdown_core_flank_sides(self->needs[INSERTION_FLANK], flank);
}
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '+') {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}
static const markdown_core_inline_ops INSERTION_INLINES = {
    .rule = MARKDOWN_CORE_DELIM_RULE_INSERTION,
    .character = '+',
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .flank = S_flank,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_INSERTION},
    .match = match,
    .terminates = "+",
    .dispatch = "+",
};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_INSERTION_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_INSERTION, .words = markdown_core_words_children},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_INSERTION = {
    .facets = MARKDOWN_CORE_INSERTION_FACETS,
    .needs = INSERTION_NEEDS,
    .name = "insertion",
    .inlines = &INSERTION_INLINES,
};
