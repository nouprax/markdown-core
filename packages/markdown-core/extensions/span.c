#include "link.h"
#include "inline_internal.h"
#include "attributes.h"
#include "citation.h"
#include "span.h"
markdown_core_bracket_match markdown_core_span_close(markdown_core_parser *parser,
                                                     markdown_core_inline_state *inline_state, bracket *opener) {
    bufsize_t initial_pos = inline_state->pos;
    {
        markdown_core_node *inl;
        markdown_core_attributes attributes = {0};
        bufsize_t end;
        if (markdown_core_inline_state_attributes(inline_state, initial_pos, &attributes, &end)) {
            if (!markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_SPAN)) {
                markdown_core_attributes_free(inline_state->mem, &attributes);
                return BRACKET_REJECTED;
            }
            inl = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_SPAN);
            if (!inl) {
                markdown_core_attributes_free(inline_state->mem, &attributes);
                markdown_core_inline_pop_bracket(inline_state);
                return BRACKET_MATCHED;
            }
            inl->attributes = attributes;
            inline_state->pos = end;
            markdown_core_inline_state_place(inline_state, inl, opener->position - 1, end - 1);
            markdown_core_inline_finish_citation_tokens(inline_state, &opener->citations);
            markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
            markdown_core_inline_take_bracket_content(parser, opener, inl);
            markdown_core_inline_replace_bracket_opener(inline_state, opener, inl);
            markdown_core_inline_pop_bracket(inline_state);
            return BRACKET_MATCHED;
        }
    }
    return BRACKET_UNMATCHED;
}
