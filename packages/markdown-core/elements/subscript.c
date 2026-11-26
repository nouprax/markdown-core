#include "subscript.h"
#include "attention.h"
#include "inline_internal.h"
static const markdown_core_text_construct SUBSCRIPT_CONSTRUCTS[] = {{"~", MARKDOWN_CORE_ATTENTION_CONSTRUCT}};
static const markdown_core_inline_ops SUBSCRIPT_INLINES = {
    .constructs = SUBSCRIPT_CONSTRUCTS,
    .construct_count = 1,
    .tokens = MARKDOWN_CORE_ATTENTION_TOKENS,
    .token_count = MARKDOWN_CORE_ATTENTION_KINDS,
    .rule = MARKDOWN_CORE_DELIM_RULE_SUBSCRIPT,
    .delimiter = {.minimum_width = 1,
                  .maximum_width = 1,
                  .run_limit = 1,
                  .body = DELIMITER_WORD_BODY,
                  .single_kind = MARKDOWN_CORE_NODE_SUBSCRIPT},
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
