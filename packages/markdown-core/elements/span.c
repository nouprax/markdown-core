#include "link.h"
#include "inline_internal.h"
#include "attributes.h"
#include "citation.h"
#include "span.h"
#include "parts.h"
markdown_core_bracket_match markdown_core_span_close(const markdown_core_element_instance *link,
                                                     const markdown_core_element_instance *citation,
                                                     markdown_core_parser *parser,
                                                     markdown_core_inline_state *inline_state, bracket *opener) {
    bufsize_t initial_pos = inline_state->pos;
    {
        markdown_core_node *inl;
        markdown_core_attributes attributes = {0};
        bufsize_t end;
        if (markdown_core_inline_state_attributes(inline_state, initial_pos, MARKDOWN_CORE_KIND_SPAN, &attributes,
                                                  &end)) {
            if (!markdown_core_node_can_contain_type(markdown_core_scaffold_parent(opener->inl_text),
                                                     MARKDOWN_CORE_NODE_SPAN)) {
                markdown_core_attributes_free(&attributes);
                return BRACKET_REJECTED;
            }
            inl = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_SPAN);
            if (!inl) {
                markdown_core_attributes_free(&attributes);
                markdown_core_inline_pop_bracket(link, inline_state);
                return BRACKET_MATCHED;
            }
            inl->attributes = attributes;
            inline_state->pos = end;
            markdown_core_inline_state_place(inline_state, inl, opener->position - 1, end - 1);
            markdown_core_inline_finish_citation_tokens(citation, inline_state, &opener->citations);
            markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
            markdown_core_inline_take_bracket_content(link, parser, opener, inl);
            markdown_core_inline_replace_bracket_opener(inline_state, opener, inl);
            markdown_core_inline_pop_bracket(link, inline_state);
            return BRACKET_MATCHED;
        }
    }
    return BRACKET_UNMATCHED;
}

/* THE PARTS OF A BRACKETED SPAN (parts.h): the attribute container after
 * the `]` that closes its text. */
void markdown_core_span_parts(markdown_core_parts *parts) {
    const unsigned char *s = parts->text;
    const bufsize_t n = parts->length;
    bufsize_t close = markdown_core_parts_last(parts, 1);
    while (close < n && s[close] != ']') {
        close += s[close] == '\\' && close + 1 < n ? 2 : 1;
    }
    if (close + 1 < n && s[close + 1] == '{') {
        markdown_core_attributes_parts(parts, close + 1);
    }
}
