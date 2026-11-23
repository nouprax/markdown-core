#include "emphasis.h"
#include "inline_internal.h"

static inline bool S_left(markdown_core_flank flank) {
    return !flank.space_after && (!flank.punct_after || flank.space_before || flank.punct_before);
}

static inline bool S_right(markdown_core_flank flank) {
    return !flank.space_before && (!flank.punct_before || flank.space_after || flank.punct_after);
}

unsigned markdown_core_emphasis_free(markdown_core_flank flank) {
    return (S_left(flank) ? MARKDOWN_CORE_FLANK_OPENS : 0u) | (S_right(flank) ? MARKDOWN_CORE_FLANK_CLOSES : 0u);
}

unsigned markdown_core_emphasis_bound(markdown_core_flank flank) {
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
                  .flank = markdown_core_emphasis_free,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match = match,
    .terminates = "*",
    .dispatch = "*",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS = {
    .name = "emphasis",
    .inlines = &EMPHASIS_INLINES,
};
static const markdown_core_inline_ops EMPHASIS_UNDERSCORE_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .rule = MARKDOWN_CORE_DELIM_RULE_UNDERSCORE,
    .character = '_',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 2,
                  .rule_of_three = true,
                  .flank = markdown_core_emphasis_bound,
                  .body = DELIMITER_INLINE_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                  .double_kind = MARKDOWN_CORE_NODE_STRONG},
    .match = match,
    .terminates = "_",
    .dispatch = "_",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS_UNDERSCORE = {
    .name = "emphasis_underscore",
    .inlines = &EMPHASIS_UNDERSCORE_INLINES,
};
