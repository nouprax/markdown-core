#include "tasklist.h"
#include <parser.h>
#include "ext_scanners.h"
#include "extension.h"

typedef enum {
    MARKDOWN_CORE_TASKLIST_NOCHECKED,
    MARKDOWN_CORE_TASKLIST_CHECKED,
} markdown_core_tasklist_type;

// Local constants
static const char *TYPE_STRING = "tasklist";

static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node) { return TYPE_STRING; }

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

static int matches(
    markdown_core_extension *self,
    markdown_core_parser *parser,
    unsigned char *input,
    int len,
    markdown_core_node *parent_container
) {
    markdown_core_chunk input_chunk = {input, len, 0};
    return markdown_core_parser_match_list_item_prefix(parser, &input_chunk, parent_container);
}

static int can_contain(
    markdown_core_extension *extension,
    markdown_core_node *node,
    markdown_core_node_type child_type
) {
    return (node->type == MARKDOWN_CORE_NODE_LIST_ITEM) ? 1 : 0;
}

static markdown_core_node *open_tasklist_item(
    markdown_core_extension *self,
    int indented,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
) {
    markdown_core_node_type node_type = markdown_core_node_get_type(parent_container);
    if (node_type != MARKDOWN_CORE_NODE_LIST_ITEM) {
        return NULL;
    }

    // The list marker has already been consumed by list parsing, so
    // parser->first_nonspace points at the character after it. Scanning from
    // there (rather than offset 0) lets us match task markers nested inside
    // container blocks like block quotes, where the input still starts with
    // the container's prefix.
    markdown_core_bufsize matched = scan_tasklist(input, len, parser->first_nonspace);
    if (!matched) {
        return NULL;
    }

    markdown_core_node_set_extension(parent_container, self);
    /* The checkbox trio is the ListItem's own marker material (11.1) —
     * consumed here, before any paragraph exists, exactly like the bullet
     * the list parser captured when the item opened. Every firing records:
     * the scanner fires on any item line spelling one, and the last firing
     * is the state the item keeps. The trailing spacechar run the scan
     * matched is trivia, like the spacing after a footnote opener. */
    markdown_core_parser_capture_marker(
        parser,
        parent_container,
        MARKDOWN_CORE_CONCRETE_TASK_MARKER,
        parser->first_nonspace,
        3
    );
    markdown_core_parser_advance_offset(parser, (char *)input, 3, false);

    // Either an upper or lower case X means the task is completed. Read the
    // marker character scan_tasklist just matched at first_nonspace; the item
    // text after it must not influence the checked state.
    parent_container->as.list.checked =
        (input[parser->first_nonspace + 1] == 'x' || input[parser->first_nonspace + 1] == 'X');

    return NULL;
}

static const markdown_core_extension tasklist_extension = {
    .name = "tasklist",
    .last_block_matches = matches,
    .get_type_string = get_type_string,
    .try_opening_block = open_tasklist_item,
    .can_contain = can_contain,
};

markdown_core_extension *markdown_core_tasklist_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&tasklist_extension;
}
