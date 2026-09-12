#include "link.h"
#include "inline_internal.h"
#include "attributes.h"
#include "citation.h"
#include "span.h"
markdown_core_bracket_match markdown_core_span_close(markdown_core_parser *parser, subject *subj, bracket *opener) {
    bufsize_t initial_pos = subj->pos;
    {
        markdown_core_node *inl;
        markdown_core_attributes attributes = {0};
        bufsize_t end;
        if (markdown_core_inline_parser_attributes(subj, initial_pos, &attributes, &end)) {
            if (!markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_SPAN)) {
                markdown_core_attributes_free(subj->mem, &attributes);
                return BRACKET_REJECTED;
            }
            inl = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_SPAN);
            if (!inl) {
                markdown_core_attributes_free(subj->mem, &attributes);
                markdown_core_inline_pop_bracket(subj);
                return BRACKET_MATCHED;
            }
            inl->attributes = attributes;
            subj->pos = end;
            markdown_core_inline_parser_place(subj, inl, opener->position - 1, end - 1);
            markdown_core_inline_finish_citation_tokens(subj, &opener->citations);
            markdown_core_inline_process_delimiters(parser, subj, opener->position, opener->delim_end);
            markdown_core_inline_take_bracket_content(parser, opener, inl);
            markdown_core_inline_replace_bracket_opener(subj, opener, inl);
            markdown_core_inline_pop_bracket(subj);
            return BRACKET_MATCHED;
        }
    }
    return BRACKET_UNMATCHED;
}
