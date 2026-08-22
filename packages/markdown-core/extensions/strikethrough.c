#include "strikethrough.h"
#include "syntax_extension.h"
#include <parser.h>

static markdown_core_node *match(const markdown_core_syntax_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_node *res = NULL;
    int left_flanking, right_flanking, punct_before, punct_after, delims;
    char buffer[101];

    if (character != '~') {
        return NULL;
    }

    delims = markdown_core_inline_parser_scan_delimiters(inline_parser, sizeof(buffer) - 1, '~', &left_flanking,
                                                         &right_flanking, &punct_before, &punct_after);

    memset(buffer, '~', delims);
    buffer[delims] = 0;

    res = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);
    if (!res) {
        parser->oom = true;
        return NULL;
    }
    if (!markdown_core_node_set_literal(res, buffer)) {
        parser->oom = true;
    }
    res->start_line = res->end_line = markdown_core_inline_parser_get_line(inline_parser);
    res->start_column = markdown_core_inline_parser_get_column(inline_parser) - delims;
    // The run owns `delims` bytes and must say so. Left unset it stayed 0 from
    // the calloc, so an UNMATCHED run reported an end before its own start --
    // `a~~` gave Text 1:1..1:0 -- and consolidation then carried that 0 onto the
    // whole merged run, because it takes the merged end column from the last
    // operand.
    res->end_column = res->start_column + delims - 1;

    if ((left_flanking || right_flanking) &&
        (delims == 2 || (!(parser->options & MARKDOWN_CORE_OPT_STRIKETHROUGH_DOUBLE_TILDE) && delims == 1))) {
        markdown_core_inline_parser_push_delimiter(inline_parser, self, MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH,
                                                   left_flanking, right_flanking, res);
    }

    return res;
}

static delimiter *insert(const markdown_core_syntax_extension *self, markdown_core_parser *parser,
                         markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    markdown_core_node *strikethrough;
    markdown_core_node *tmp, *next;
    delimiter *delim, *tmp_delim;
    delimiter *res = markdown_core_delimiter_next(closer);

    strikethrough = markdown_core_delimiter_node(opener);

    if (markdown_core_delimiter_node(opener)->as.literal.len != markdown_core_delimiter_node(closer)->as.literal.len) {
        goto done;
    }

    if (!markdown_core_node_set_type(strikethrough, MARKDOWN_CORE_NODE_STRIKETHROUGH)) {
        goto done;
    }

    markdown_core_node_set_syntax_extension(strikethrough, self);

    tmp = markdown_core_node_next(markdown_core_delimiter_node(opener));

    while (tmp) {
        if (tmp == markdown_core_delimiter_node(closer)) {
            break;
        }
        next = markdown_core_node_next(tmp);
        markdown_core_node_append_child(strikethrough, tmp);
        tmp = next;
    }

    strikethrough->end_column =
        markdown_core_delimiter_node(closer)->start_column + markdown_core_delimiter_node(closer)->as.literal.len - 1;
    markdown_core_node_free(markdown_core_delimiter_node(closer));

done:
    delim = closer;
    while (delim != NULL && delim != opener) {
        tmp_delim = markdown_core_delimiter_previous(delim);
        markdown_core_inline_parser_remove_delimiter(inline_parser, delim);
        delim = tmp_delim;
    }

    markdown_core_inline_parser_remove_delimiter(inline_parser, opener);

    return res;
}

static const char *get_type_string(const markdown_core_syntax_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_STRIKETHROUGH ? "strikethrough" : "<unknown>";
}

static int can_contain(const markdown_core_syntax_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->type != MARKDOWN_CORE_NODE_STRIKETHROUGH) {
        return false;
    }

    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
}

/* `~` is the ONE byte in this repository that is genuinely
 * flanking-transparent, and it must stay so: it is inherited from cmark-gfm,
 * it behaves identically there, and upstream parity breaks without it. */
const markdown_core_syntax_extension MARKDOWN_CORE_EXTENSION_STRIKETHROUGH = {
    .name = "strikethrough",
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .match_inline = match,
    .insert_inline_from_delim = insert,
    .terminates_text = "~",
    .dispatch = "~",
    .flanking_transparent = "~",
};
