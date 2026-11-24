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
                  .flank = S_flank,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_STRIKETHROUGH,
                  .exact_run = true},
    .match = match,
    .terminates = "~",
    .dispatch = "~",
    .transparent = "~",
};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_STRIKETHROUGH_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_STRIKETHROUGH, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

/* A run of `~` right after the slot keeps its flank (markdown_core_write_runs). */
static markdown_core_status S_keeps(const markdown_core_site *site, const uint8_t *value, size_t size,
                                    const markdown_core_spelling *spelled, bool *kept) {
    (void)value;
    (void)size;
    *kept = markdown_core_write_runs(site, spelled, "~");
    return MARKDOWN_CORE_OK;
}

static const markdown_core_write_ops STRIKETHROUGH_WRITES = {
    .keeps = S_keeps,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_STRIKETHROUGH = {
    .writes = &STRIKETHROUGH_WRITES,
    .facets = MARKDOWN_CORE_STRIKETHROUGH_FACETS,
    .needs = STRIKETHROUGH_NEEDS,
    .name = "strikethrough",
    .contain = &STRIKETHROUGH_CONTAIN,
    .inlines = &STRIKETHROUGH_INLINES,
};
