#include "strikethrough.h"
#include "extension.h"
#include <parser.h>
#include <limits.h>

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_node *res = NULL;
    int left_flanking, right_flanking, punct_before, punct_after, delims;
    /* One maximal run is a token. Only width two has delimiter semantics;
     * a long literal run must never be split into a trailing valid pair. */

    if (character != '~') {
        return NULL;
    }

    delims = markdown_core_inline_parser_scan_delimiters(inline_parser, INT_MAX, '~', &left_flanking, &right_flanking,
                                                         &punct_before, &punct_after);

    // The cursor is one past the run here, so the run is the `delims` bytes
    // behind it. The shared constructor owns the extent: left to this file it
    // was an end column computed by addition, and before 0a.12 not computed at
    // all -- `a~~` gave Text 1:1..1:0, an end before its own start, which
    // consolidation then carried onto the whole merged run.
    {
        int end = markdown_core_inline_parser_get_offset(inline_parser) - 1;
        res = markdown_core_inline_parser_make_delimiter_text(inline_parser, end - delims + 1, end);
    }
    if (!res) {
        parser->oom = true;
        return NULL;
    }

    if ((left_flanking || right_flanking) && delims == 2) {
        markdown_core_inline_parser_push_delimiter(inline_parser, self, MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH,
                                                   left_flanking, right_flanking, res);
    }

    return res;
}

static const char *get_type_string(const markdown_core_extension *extension, markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_STRIKETHROUGH ? "strikethrough" : "<unknown>";
}

static int can_contain(const markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->kind != MARKDOWN_CORE_NODE_STRIKETHROUGH) {
        return false;
    }

    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
}

/* `~` is the ONE byte in this repository that is genuinely
 * flanking-transparent, and it must stay so: it is inherited from cmark-gfm,
 * it behaves identically there, and upstream parity breaks without it. */
const markdown_core_extension MARKDOWN_CORE_EXTENSION_STRIKETHROUGH = {
    .name = "strikethrough",
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .match_inline = match,
    .terminates_text = "~",
    .dispatch = "~",
    .flanking_transparent = "~",
};
