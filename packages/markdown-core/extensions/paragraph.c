#include "paragraph.h"
#include "block_internal.h"

#include "link.h"
#include "block_identifier.h"
void markdown_core_parser_finalize_paragraph(markdown_core_parser *parser, markdown_core_node *paragraph) {
    if (!markdown_core_block_resolve_reference_link_definitions(parser, paragraph)) {
        paragraph->flags |= MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY;
        return;
    }
    markdown_core_block_attach_paragraph_identifier(parser, paragraph);
}

static int continue_paragraph(const markdown_core_extension *self, markdown_core_parser *parser, unsigned char *data,
                              int length, markdown_core_node *container) {
    return !parser->blank;
}
static void complete_block(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY) {
        markdown_core_node_free(node);
    }
}
static bool accepts_lazy(markdown_core_parser *parser, markdown_core_node *node) { return true; }
static markdown_core_node *open_lazy(markdown_core_parser *parser, markdown_core_node *node) { return node; }

const markdown_core_extension MARKDOWN_CORE_EXTENSION_PARAGRAPH = {
    .complete_block = complete_block,
    .accepts_lazy = accepts_lazy,
    .open_lazy = open_lazy,

    .name = "paragraph",
    .last_block_matches = continue_paragraph,
    .content_mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inline_content = true,
    .paragraph = true,
    .finalize_block = markdown_core_parser_finalize_paragraph,
};

markdown_core_node *markdown_core_paragraph_open_text(markdown_core_parser *parser, markdown_core_node *container,
                                                      markdown_core_chunk *input) {
    container = markdown_core_block_parent_for(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH);
    parser->current = container;
    if (markdown_core_block_attach_identifier_line(parser, container, input) || parser->oom) {
        return NULL;
    }
    container =
        markdown_core_parser_add_child(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, parser->first_nonspace + 1);
    if (container) {
        markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
    }
    return container;
}
