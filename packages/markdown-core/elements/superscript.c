#include "superscript.h"
#include "attention.h"
#include "inline_internal.h"
static const markdown_core_text_construct SUPERSCRIPT_CONSTRUCTS[] = {{"^", MARKDOWN_CORE_ATTENTION_CONSTRUCT}};
static const markdown_core_inline_ops SUPERSCRIPT_INLINES = {
    .constructs = SUPERSCRIPT_CONSTRUCTS,
    .construct_count = 1,
    .tokens = MARKDOWN_CORE_ATTENTION_TOKENS,
    .token_count = MARKDOWN_CORE_ATTENTION_KINDS,
    .rule = MARKDOWN_CORE_DELIM_RULE_SUPERSCRIPT,
    .character = '^',
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 1,
                  .run_limit = 1,
                  .body = DELIMITER_WORD_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_SUPERSCRIPT},
};

/* What its nodes read as in a name: their children (element.h,
 * markdown_core_facet). */
const markdown_core_facet MARKDOWN_CORE_SUPERSCRIPT_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_SUPERSCRIPT, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_SUPERSCRIPT = {
    .facets = MARKDOWN_CORE_SUPERSCRIPT_FACETS,
    .name = "superscript",
    .inlines = &SUPERSCRIPT_INLINES,
};
