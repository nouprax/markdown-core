#include "tasklist.h"
#include "extension.h"
#include <assert.h>
#include <parser.h>
#include "ext_scanners.h"

// Local constants
static const char TYPE_STRING[] = "tasklist";

static const char *get_type_string(const markdown_core_extension *extension, markdown_core_node *node) {
    return TYPE_STRING;
}

static bool parse_node_item_prefix(markdown_core_parser *parser, const char *input, markdown_core_node *container) {
    bool res = false;

    if (parser->indent >= container->as.list->marker_offset + container->as.list->padding) {
        markdown_core_parser_advance_offset(parser, input,
                                            container->as.list->marker_offset + container->as.list->padding, true);
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

    // The scanner currently accepts one ASCII byte between the brackets. Keep
    // the marker as an owned UTF-8 string so custom scalars need no storage or
    // facade change when their grammar lands. Copy before reusing parser input.
    assert(matched >= 4 && parser->first_nonspace + 1 < len);
    markdown_core_chunk marker = {input + parser->first_nonspace + 1, 1, 0};
    if (!markdown_core_chunk_to_cstr(parser->mem, &marker)) {
        parser->oom = true;
        return NULL;
    }
    markdown_core_optional_chunk_free(parser->mem, &parent_container->as.list->task_marker);
    parent_container->as.list->task_marker = markdown_core_optional_chunk_present(marker);
    markdown_core_node_set_extension(parent_container, self);
    markdown_core_parser_advance_offset(parser, (char *)input, 3, false);

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
