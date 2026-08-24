#include "strikethrough.h"
#include "syntax_extension.h"
#include <parser.h>

static markdown_core_node *match(const markdown_core_syntax_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_node *res = NULL;
    int left_flanking, right_flanking, punct_before, punct_after, delims;
    /* The longest run this matcher will consider. It used to be `sizeof` a
     * 101-byte stack buffer the run was then written into, character by
     * character, only to be copied back out as the node's literal -- the
     * literal is a slice of the block's own content and needs no copy at all. */
    enum { MAX_DELIMITERS = 100 };

    if (character != '~') {
        return NULL;
    }

    delims = markdown_core_inline_parser_scan_delimiters(inline_parser, MAX_DELIMITERS, '~', &left_flanking,
                                                         &right_flanking, &punct_before, &punct_after);

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
    /* REQUIREMENT 11b: both tilde runs are the strikethrough's markers. The
     * opener's node IS the strikethrough -- it was retyped in place -- so its
     * own claim would otherwise read CONTENT, and the closer's node is freed on
     * the next line, so its claim would name nothing. */
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
