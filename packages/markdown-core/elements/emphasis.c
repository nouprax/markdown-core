#include "emphasis.h"
#include "inline_internal.h"
#include "services.h"

static inline bool S_left(markdown_core_flank flank) {
    return !flank.space_after && (!flank.punct_after || flank.space_before || flank.punct_before);
}

static inline bool S_right(markdown_core_flank flank) {
    return !flank.space_before && (!flank.punct_before || flank.space_after || flank.punct_after);
}

/* `*`: a left-flanking run opens and a right-flanking one closes. */
static unsigned S_free(const markdown_core_element_instance *self, markdown_core_flank flank) {
    (void)self;
    return (S_left(flank) ? MARKDOWN_CORE_FLANK_OPENS : 0u) | (S_right(flank) ? MARKDOWN_CORE_FLANK_CLOSES : 0u);
}

/* `_`: a run flanking on both sides opens only after punctuation and closes
 * only before it. */
static unsigned S_bound(const markdown_core_element_instance *self, markdown_core_flank flank) {
    (void)self;
    const bool left = S_left(flank), right = S_right(flank);
    return (left && (!right || flank.punct_before) ? MARKDOWN_CORE_FLANK_OPENS : 0u) |
           (right && (!left || flank.punct_after) ? MARKDOWN_CORE_FLANK_CLOSES : 0u);
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '*' && character != '_') {
        return NULL;
    }
    if (character != self->element->inlines->character) {
        return NULL;
    }
    return markdown_core_inline_match_delimiter(self, inline_state);
}

static const markdown_core_inline_ops EMPHASIS_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .rule = MARKDOWN_CORE_DELIM_RULE_EMPHASIS,
    .character = '*',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 2,
                  .rule_of_three = true,
                  .flank = S_free,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match = match,
    .terminates = "*",
    .dispatch = "*",
};

/* The `*` rule, which the other paired-run elements take as theirs. */
static const markdown_core_flank_rule EMPHASIS_FLANK = {.sides = S_free};
static const markdown_core_service EMPHASIS_SERVICES[] = {{MARKDOWN_CORE_SERVICE_FLANK, &EMPHASIS_FLANK}, {0}};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_EMPHASIS_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_EMPHASIS, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_STRONG, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

/* A run of `*` right after the slot keeps its flank (markdown_core_write_runs). */
static markdown_core_status S_keeps(const markdown_core_site *site, const uint8_t *value, size_t size,
                                    const markdown_core_spelling *spelled, bool *kept) {
    (void)value;
    (void)size;
    *kept = markdown_core_write_runs(site, spelled, "*");
    return MARKDOWN_CORE_OK;
}

static const markdown_core_write_ops EMPHASIS_WRITES = {
    .keeps = S_keeps,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS = {
    .writes = &EMPHASIS_WRITES,
    .facets = MARKDOWN_CORE_EMPHASIS_FACETS,
    .name = "emphasis",
    .provides = EMPHASIS_SERVICES,
    .inlines = &EMPHASIS_INLINES,
};
static const markdown_core_inline_ops EMPHASIS_UNDERSCORE_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .rule = MARKDOWN_CORE_DELIM_RULE_UNDERSCORE,
    .character = '_',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 2,
                  .rule_of_three = true,
                  .flank = S_bound,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match = match,
    .terminates = "_",
    .dispatch = "_",
};

/* A run of `_` right after the slot keeps its flank (markdown_core_write_runs). */
static markdown_core_status S_keeps_underscore(const markdown_core_site *site, const uint8_t *value, size_t size,
                                               const markdown_core_spelling *spelled, bool *kept) {
    (void)value;
    (void)size;
    *kept = markdown_core_write_runs(site, spelled, "_");
    return MARKDOWN_CORE_OK;
}

static const markdown_core_write_ops EMPHASIS_UNDERSCORE_WRITES = {
    .keeps = S_keeps_underscore,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS_UNDERSCORE = {
    .writes = &EMPHASIS_UNDERSCORE_WRITES,
    .name = "emphasis_underscore",
    .inlines = &EMPHASIS_UNDERSCORE_INLINES,
};
