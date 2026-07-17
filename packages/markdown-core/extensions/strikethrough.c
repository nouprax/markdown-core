#include "strikethrough.h"
#include <parser.h>
#include "extension.h"

static markdown_core_node *match(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
) {
    markdown_core_node *res = NULL;
    int left_flanking, right_flanking, punct_before, punct_after, delims;
    char buffer[101];

    if (character != '~') {
        return NULL;
    }

    delims = markdown_core_inline_parser_scan_delimiters(
        inline_parser,
        sizeof(buffer) - 1,
        '~',
        &left_flanking,
        &right_flanking,
        &punct_before,
        &punct_after
    );

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

    if ((left_flanking || right_flanking) &&
        (delims == 2 || (!(parser->options & MARKDOWN_CORE_OPT_STRIKETHROUGH_DOUBLE_TILDE) && delims == 1))) {
        markdown_core_inline_parser_push_delimiter(inline_parser, character, left_flanking, right_flanking, res);
    }

    return res;
}

static delimiter *insert(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    delimiter *opener,
    delimiter *closer
) {
    markdown_core_node *strikethrough;
    markdown_core_node *tmp, *next;
    delimiter *delim, *tmp_delim;
    delimiter *res = closer->next;

    strikethrough = opener->inl_text;

    if (opener->inl_text->as.literal.len != closer->inl_text->as.literal.len) {
        goto done;
    }

    if (!markdown_core_node_set_type(strikethrough, MARKDOWN_CORE_NODE_STRIKETHROUGH)) {
        goto done;
    }

    markdown_core_node_set_extension(strikethrough, self);

    tmp = markdown_core_node_next(opener->inl_text);

    while (tmp) {
        if (tmp == closer->inl_text) {
            break;
        }
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

static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_STRIKETHROUGH ? "strikethrough" : "<unknown>";
}

static int
can_contain(markdown_core_extension *extension, markdown_core_node *node, markdown_core_node_type child_type) {
    if (node->type != MARKDOWN_CORE_NODE_STRIKETHROUGH) {
        return false;
    }

    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
}

static const unsigned char strikethrough_special_chars[] = {'~'};

static const markdown_core_extension strikethrough_extension = {
    .name = "strikethrough",
    .get_type_string = get_type_string,
    .can_contain = can_contain,
    .match_inline = match,
    .insert_inline_from_delim = insert,
    .special_inline_chars = strikethrough_special_chars,
    .special_inline_char_count = sizeof(strikethrough_special_chars),
    .emphasis = true,
};

markdown_core_extension *markdown_core_strikethrough_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&strikethrough_extension;
}
