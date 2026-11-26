#include "mark.h"
#include "attention.h"
#include "inline_internal.h"
#include "services.h"

/* The services this element asks, as `self->needs` holds them: the flank
 * rule of a paired delimiter run (services.h). */
enum { MARK_FLANK };
static const char *const MARK_NEEDS[] = {[MARK_FLANK] = MARKDOWN_CORE_SERVICE_FLANK, NULL};
static unsigned S_flank(const markdown_core_element_instance *self, markdown_core_flank flank) {
    return markdown_core_flank_sides(self->needs[MARK_FLANK], flank);
}
static const markdown_core_text_construct MARK_CONSTRUCTS[] = {{"=", MARKDOWN_CORE_ATTENTION_CONSTRUCT}};
static const markdown_core_inline_ops MARK_INLINES = {
    .constructs = MARK_CONSTRUCTS,
    .construct_count = 1,
    .tokens = MARKDOWN_CORE_ATTENTION_TOKENS,
    .token_count = MARKDOWN_CORE_ATTENTION_KINDS,
    .rule = MARKDOWN_CORE_DELIM_RULE_MARK,
    .character = '=',
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .flank = S_flank,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_MARK},
};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_MARK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_MARK, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_MARK = {
    .facets = MARKDOWN_CORE_MARK_FACETS,
    .needs = MARK_NEEDS,
    .name = "mark",
    .inlines = &MARK_INLINES,
};
