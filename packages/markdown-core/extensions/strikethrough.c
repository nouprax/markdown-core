#include "strikethrough.h"
#include <assert.h>
#include <limits.h>
#include <parser.h>
#include "extension.h"

enum { STRIKETHROUGH_RULE = 0 };

static markdown_core_node *match(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
) {
    markdown_core_bufsize source_start;
    markdown_core_bufsize source_end;
    int left_flanking, right_flanking, punct_before, punct_after, delims;

    if (character != '~') {
        return NULL;
    }

    source_start = markdown_core_inline_parser_get_offset(inline_parser);
    delims = markdown_core_inline_parser_scan_delimiters(
        inline_parser,
        INT_MAX,
        '~',
        &left_flanking,
        &right_flanking,
        &punct_before,
        &punct_after
    );
    if (delims <= 0) {
        return NULL;
    }
    source_end = source_start + (markdown_core_bufsize)delims;

    if ((left_flanking || right_flanking) && (delims == 1 || delims == 2)) {
        return markdown_core_inline_parser_consume_delimiter(
            inline_parser,
            STRIKETHROUGH_RULE,
            left_flanking,
            right_flanking,
            source_end
        );
    }

    return markdown_core_inline_parser_consume_text(inline_parser, source_end);
}

static markdown_core_delimiter_result insert(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    markdown_core_node *strikethrough;
    markdown_core_node *tmp, *next;

    if (match->kind != STRIKETHROUGH_RULE) {
        assert(0 && "strikethrough reducer received an unsupported delimiter rule");
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    strikethrough = match->opener_node;

    if (match->opener_length != match->closer_length) {
        return MARKDOWN_CORE_DELIMITER_OK;
    }

    markdown_core_node_set_type_unchecked(strikethrough, MARKDOWN_CORE_NODE_STRIKETHROUGH);
    markdown_core_node_set_extension(strikethrough, self);

    tmp = markdown_core_node_next(match->opener_node);

    while (tmp) {
        if (tmp == match->closer_node) {
            break;
        }
        next = markdown_core_node_next(tmp);
        markdown_core_node_append_child_unchecked(strikethrough, tmp);
        tmp = next;
    }

    strikethrough->end_line = match->closer_node->end_line;
    strikethrough->end_column = match->closer_node->end_column;
    markdown_core_node_free(match->closer_node);
    /* The length-mismatch OK above consumes nothing; only this path does,
     * and for a RANGE reduce the reducer must say so itself. */
    markdown_core_inline_parser_concrete_use_endpoints(inline_parser, match);
    return MARKDOWN_CORE_DELIMITER_OK;
}

static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node) {
    return node->type == MARKDOWN_CORE_NODE_STRIKETHROUGH ? "strikethrough" : "<unknown>";
}

static int can_contain(
    markdown_core_extension *extension,
    markdown_core_node *node,
    markdown_core_node_type child_type
) {
    if (node->type != MARKDOWN_CORE_NODE_STRIKETHROUGH) {
        return false;
    }

    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
}

static const unsigned char strikethrough_special_chars[] = {'~'};

static const markdown_core_delimiter_rule strikethrough_delimiter_rules[] = {
    {
        .pairing = MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK,
        .reduction = MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        .close_trigger = 0,
        .close_probe = NULL,
    },
};

static const markdown_core_extension strikethrough_extension = {
    .name = "strikethrough",
    .get_type_string = get_type_string,
    .can_contain = can_contain,
    .match_inline = match,
    .insert_inline_from_delim = insert,
    .delimiter_rules = strikethrough_delimiter_rules,
    .delimiter_rule_count = sizeof(strikethrough_delimiter_rules) / sizeof(strikethrough_delimiter_rules[0]),
    .special_inline_chars = strikethrough_special_chars,
    .special_inline_char_count = sizeof(strikethrough_special_chars),
    // '~' stays transparent to emphasis flanking (inherited gfm semantics:
    // tilde runs pair through the same delimiter machinery as emphasis).
    .flanking_skip_chars = strikethrough_special_chars,
    .flanking_skip_char_count = sizeof(strikethrough_special_chars),
};

markdown_core_extension *markdown_core_strikethrough_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&strikethrough_extension;
}
