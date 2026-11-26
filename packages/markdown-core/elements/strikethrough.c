#include "attention.h"
#include "inline_internal.h"
#include "services.h"

/* The services this element asks, as `self->needs` holds them: the flank
 * rule of a paired delimiter run (services.h). */
enum { STRIKETHROUGH_FLANK };
static const char *const STRIKETHROUGH_NEEDS[] = {[STRIKETHROUGH_FLANK] = MARKDOWN_CORE_SERVICE_FLANK, NULL};
static unsigned S_flank(const markdown_core_element_instance *self, markdown_core_flank flank) {
    return markdown_core_flank_sides(self->needs[STRIKETHROUGH_FLANK], flank);
}
#include "strikethrough.h"
#include "element.h"
#include <parser.h>
#include <limits.h>

static const markdown_core_node_type containment_kinds[] = {MARKDOWN_CORE_NODE_STRIKETHROUGH, MARKDOWN_CORE_NODE_NONE};

/* `~` is the ONE byte in this repository that is genuinely
 * flanking-transparent, and it must stay so: it is inherited from cmark-gfm,
 * it behaves identically there, and upstream parity breaks without it. */
static const markdown_core_contain_ops STRIKETHROUGH_CONTAIN = {
    .kinds = containment_kinds,
};

static const markdown_core_text_construct STRIKETHROUGH_CONSTRUCTS[] = {{"~", MARKDOWN_CORE_ATTENTION_CONSTRUCT}};
static const markdown_core_inline_ops STRIKETHROUGH_INLINES = {
    .constructs = STRIKETHROUGH_CONSTRUCTS,
    .construct_count = 1,
    .tokens = MARKDOWN_CORE_ATTENTION_TOKENS,
    .token_count = MARKDOWN_CORE_ATTENTION_KINDS,
    .rule = MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH,
    .delimiter = {.minimum_width = 2,
                  .maximum_width = 2,
                  .flank = S_flank,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_STRIKETHROUGH,
                  .exact_run = true},
    .transparent = "~",
};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_STRIKETHROUGH_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_STRIKETHROUGH, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_STRIKETHROUGH = {
    .facets = MARKDOWN_CORE_STRIKETHROUGH_FACETS,
    .needs = STRIKETHROUGH_NEEDS,
    .name = "strikethrough",
    .contain = &STRIKETHROUGH_CONTAIN,
    .inlines = &STRIKETHROUGH_INLINES,
};
