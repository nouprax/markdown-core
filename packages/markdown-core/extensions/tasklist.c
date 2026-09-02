#include "tasklist.h"
#include "extension.h"
#include <assert.h>
#include <parser.h>
#include "ext_scanners.h"

typedef enum {
    MARKDOWN_CORE_TASKLIST_NOCHECKED,
    MARKDOWN_CORE_TASKLIST_CHECKED,
} markdown_core_tasklist_type;

// Local constants
static const char TYPE_STRING[] = "tasklist";

static const char *get_type_string(const markdown_core_extension *extension, markdown_core_node *node) {
    return TYPE_STRING;
}

// Return 1 if state was set, 0 otherwise
int markdown_core_extensions_set_tasklist_item_checked(markdown_core_node *node, bool is_checked) {
    // The node has to exist, and be an extension, and actually be the right type in order to get
    // the value.
    if (!node || !node->extension || strcmp(markdown_core_node_get_type_string(node), TYPE_STRING)) {
        return 0;
    }

    node->as.list.checked = is_checked;
    return 1;
}

bool markdown_core_extensions_get_tasklist_item_checked(markdown_core_node *node) {
    if (!node || !node->extension || strcmp(markdown_core_node_get_type_string(node), TYPE_STRING)) {
        return false;
    }

    if (node->as.list.checked) {
        return true;
    } else {
        return false;
    }
}

static bool parse_node_item_prefix(markdown_core_parser *parser, const char *input, markdown_core_node *container) {
    bool res = false;

    if (parser->indent >= container->as.list.marker_offset + container->as.list.padding) {
        markdown_core_parser_advance_offset(parser, input,
                                            container->as.list.marker_offset + container->as.list.padding, true);
        res = true;
    } else if (parser->blank && container->first_child != NULL) {
        // if container->first_child is NULL, then the opening line
        // of the list item was blank after the list marker; in this
        // case, we are done with the list item.
        markdown_core_parser_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
        res = true;
    }
    return res;
}

static int matches(const markdown_core_extension *self, markdown_core_parser *parser, unsigned char *input, int len,
                   markdown_core_node *parent_container) {
    return parse_node_item_prefix(parser, (const char *)input, parent_container);
}

static int can_contain(const markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    return (node->type == MARKDOWN_CORE_NODE_LIST_ITEM) ? 1 : 0;
}

static markdown_core_node *open_tasklist_item(const markdown_core_extension *self, int indented,
                                              markdown_core_parser *parser, markdown_core_node *parent_container,
                                              unsigned char *input, int len) {
    markdown_core_node_type node_type = markdown_core_node_get_type(parent_container);
    if (node_type != MARKDOWN_CORE_NODE_LIST_ITEM) {
        return NULL;
    }

    // The list marker has already been consumed by list parsing, so
    // parser->first_nonspace points at the character after it. Scanning from
    // there (rather than offset 0) lets us match task markers nested inside
    // container blocks like block quotes, where the input still starts with
    // the container's prefix.
    bufsize_t matched = scan_tasklist(input, len, parser->first_nonspace);
    if (!matched) {
        return NULL;
    }

    markdown_core_node_set_extension(parent_container, self);
    markdown_core_parser_advance_offset(parser, (char *)input, 3, false);

    // Either an upper or lower case X means the task is completed -- read from
    // the marker the scanner just matched, not from anywhere else on the line.
    // A substring search over the whole line answers for the wrong bytes:
    // `- [ ] call me [x] later` is an OPEN task whose text happens to mention a
    // closed one, and searching says it is closed. (Upstream cmark-gfm still
    // does; the difference is registered as `tasklist-checked-marker`.)
    //
    // The read is in range because the scanner says so, not because the line is
    // long: `scan_tasklist`'s rule is ("[ ]"|"[x]"|"[X]")spacechar+, so a
    // non-zero `matched` means at least four bytes were matched from
    // `first_nonspace` and the marker byte is the second of them.
    assert(matched >= 4 && parser->first_nonspace + 1 < len);
    parent_container->as.list.checked =
        (input[parser->first_nonspace + 1] == 'x' || input[parser->first_nonspace + 1] == 'X');

    return NULL;
}

/* A block-only extension; see the note in extensions/table.c. */
const markdown_core_extension MARKDOWN_CORE_EXTENSION_TASKLIST = {
    .name = "tasklist",
    .last_block_matches = matches,
    .get_type_string_func = get_type_string,
    .try_opening_block = open_tasklist_item,
    .can_contain_func = can_contain,
};
