#include "strikethrough.h"
#include <parser.h>

markdown_core_node_type MARKDOWN_CORE_NODE_STRIKETHROUGH;

static markdown_core_node *match(markdown_core_syntax_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_node *res = NULL;
    int left_flanking, right_flanking, punct_before, punct_after, delims;
    char buffer[101];

    if (character != '~')
        return NULL;

    delims = markdown_core_inline_parser_scan_delimiters(inline_parser, sizeof(buffer) - 1, '~', &left_flanking,
                                                         &right_flanking, &punct_before, &punct_after);

    memset(buffer, '~', delims);
    buffer[delims] = 0;

    res = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);
    markdown_core_node_set_literal(res, buffer);
    res->start_line = res->end_line = markdown_core_inline_parser_get_line(inline_parser);
    res->start_column = markdown_core_inline_parser_get_column(inline_parser) - delims;

    if ((left_flanking || right_flanking) &&
        (delims == 2 || (!(parser->options & MARKDOWN_CORE_OPT_STRIKETHROUGH_DOUBLE_TILDE) && delims == 1))) {
        markdown_core_inline_parser_push_delimiter(inline_parser, character, left_flanking, right_flanking, res);
    }

    return res;
}

static delimiter *insert(markdown_core_syntax_extension *self, markdown_core_parser *parser,
                         markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    markdown_core_node *strikethrough;
    markdown_core_node *tmp, *next;
    delimiter *delim, *tmp_delim;
    delimiter *res = closer->next;

    strikethrough = opener->inl_text;

    if (opener->inl_text->as.literal.len != closer->inl_text->as.literal.len)
        goto done;

    if (!markdown_core_node_set_type(strikethrough, MARKDOWN_CORE_NODE_STRIKETHROUGH))
        goto done;

    markdown_core_node_set_syntax_extension(strikethrough, self);

    tmp = markdown_core_node_next(opener->inl_text);

    while (tmp) {
        if (tmp == closer->inl_text)
            break;
        next = markdown_core_node_next(tmp);
        markdown_core_node_append_child(strikethrough, tmp);
        tmp = next;
    }

    strikethrough->end_column = closer->inl_text->start_column + closer->inl_text->as.literal.len - 1;
    markdown_core_node_free(closer->inl_text);

done:
    delim = closer;
    while (delim != NULL && delim != opener) {
        tmp_delim = delim->previous;
        markdown_core_inline_parser_remove_delimiter(inline_parser, delim);
        delim = tmp_delim;
    }

    markdown_core_inline_parser_remove_delimiter(inline_parser, opener);

    return res;
}

static const char *get_type_string(markdown_core_syntax_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_STRIKETHROUGH ? "strikethrough" : "<unknown>";
}

static int can_contain(markdown_core_syntax_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->type != MARKDOWN_CORE_NODE_STRIKETHROUGH)
        return false;

    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
}

markdown_core_syntax_extension *create_strikethrough_extension(void) {
    markdown_core_syntax_extension *ext = markdown_core_syntax_extension_new("strikethrough");
    markdown_core_llist *special_chars = NULL;

    markdown_core_syntax_extension_set_get_type_string_func(ext, get_type_string);
    markdown_core_syntax_extension_set_can_contain_func(ext, can_contain);
    MARKDOWN_CORE_NODE_STRIKETHROUGH = markdown_core_syntax_extension_add_node(1);

    markdown_core_syntax_extension_set_match_inline_func(ext, match);
    markdown_core_syntax_extension_set_inline_from_delim_func(ext, insert);

    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    special_chars = markdown_core_llist_append(mem, special_chars, (void *)'~');
    markdown_core_syntax_extension_set_special_inline_chars(ext, special_chars);

    markdown_core_syntax_extension_set_emphasis(ext, 1);

    return ext;
}
