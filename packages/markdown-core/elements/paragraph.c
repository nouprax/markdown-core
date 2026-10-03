#include "paragraph.h"
#include "block_internal.h"

#include "link.h"
#include "block_identifier.h"
void markdown_core_paragraph_finalize(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                      markdown_core_node *parent, markdown_core_node *paragraph) {
    if (!markdown_core_block_resolve_reference_link_definitions(parser, paragraph)) {
        paragraph->flags |= MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY;
        return;
    }
    markdown_core_block_attach_paragraph_identifier(self->state, parser, parent, paragraph);
}

/* The paragraph's own `finalize_block`: it is the open spine's last block. */
static void finalize_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *paragraph) {
    markdown_core_paragraph_finalize(self, parser, markdown_core_parser_open_parent(parser, paragraph), paragraph);
}

static int continue_paragraph(const markdown_core_element_instance *self, markdown_core_parser *parser,
                              unsigned char *data, int length, markdown_core_node *container) {
    return !parser->blank;
}
/* A PARAGRAPH THAT HELD ONLY REFERENCE DEFINITIONS IS NOT A PARAGRAPH. Its
 * finalization consumed the definitions and left nothing, so it has no
 * inline content to parse; its parent drops it (blocks.c,
 * S_drop_definition_paragraph). */
static int contains_inlines(const markdown_core_element *element, markdown_core_node *node) {
    (void)element;
    return !(node->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY);
}
static bool accepts_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node) {
    (void)self;
    (void)parser;
    (void)node;
    return true;
}
static markdown_core_node *open_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *node, markdown_core_chunk *input) {
    (void)self;
    return node;
}

/* A text line no block claims opens a paragraph: the dialect's text block. */
static markdown_core_node *open_text(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *container, markdown_core_chunk *input) {
    container = markdown_core_block_parent_for(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!container) {
        return NULL;
    }
    if (markdown_core_block_attach_identifier_line(self->state, parser, container, input) || parser->error) {
        return NULL;
    }
    container = markdown_core_parser_add_child_validated(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH,
                                                         parser->first_nonspace + 1);
    if (container) {
        markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
    }
    return container;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH = {
    .state_size = sizeof(markdown_core_block_identifier_work),
    .accepts_lazy = accepts_lazy,
    .open_lazy = open_lazy,

    .name = "paragraph",
    .last_block_matches = continue_paragraph,
    .content_mode = MARKDOWN_CORE_CONTENT_PROSE,
    /* Inline content, unless the paragraph was only definitions. */
    .contains_inlines_func = contains_inlines,
    .paragraph = true,
    .finalize_block = finalize_block,
    .open_text_block = open_text,
};
