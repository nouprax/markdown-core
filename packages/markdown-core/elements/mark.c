#include "mark.h"
#include "inline_internal.h"
#include "services.h"

/* The services this element asks, as `self->needs` holds them: the flank
 * rule of a paired delimiter run (services.h). */
enum { MARK_FLANK };
static const char *const MARK_NEEDS[] = {[MARK_FLANK] = MARKDOWN_CORE_SERVICE_FLANK, NULL};
static unsigned S_flank(const markdown_core_element_instance *self, markdown_core_flank flank) {
    return markdown_core_flank_sides(self->needs[MARK_FLANK], flank);
}
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
                  .flank = S_flank,
                  .body = DELIMITER_INLINE_BODY,
                  .double_kind = MARKDOWN_CORE_NODE_MARK},
    .match = match,
    .terminates = "=",
    .dispatch = "=",
};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_MARK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_MARK, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

/* A run of `==` right after the slot keeps its flank (markdown_core_write_runs). */
static markdown_core_status S_keeps(const markdown_core_site *site, const uint8_t *value, size_t size,
                                    const markdown_core_spelling *spelled, bool *kept) {
    (void)value;
    (void)size;
    *kept = markdown_core_write_runs(site, spelled, "==");
    return MARKDOWN_CORE_OK;
}

static const markdown_core_write_ops MARK_WRITES = {
    .keeps = S_keeps,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_MARK = {
    .writes = &MARK_WRITES,
    .facets = MARKDOWN_CORE_MARK_FACETS,
    .needs = MARK_NEEDS,
    .name = "mark",
    .inlines = &MARK_INLINES,
};
