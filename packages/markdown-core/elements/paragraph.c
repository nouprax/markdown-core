#include "paragraph.h"
#include "block_internal.h"

#include "link.h"
#include "block_identifier.h"
void markdown_core_paragraph_finalize(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                      markdown_core_member *paragraph) {
    if (!markdown_core_block_resolve_reference_link_definitions(parser, paragraph)) {
        paragraph->node->flags |= MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY;
        /* The last definition ends the paragraph: it holds the next as the
         * paragraph did (5.3). */
        if (paragraph->prev && paragraph->prev->node->kind == MARKDOWN_CORE_NODE_REFERENCE &&
            !(paragraph->node->flags & MARKDOWN_CORE_NODE__HOLDS_NEXT)) {
            paragraph->prev->node->flags &= ~MARKDOWN_CORE_NODE__HOLDS_NEXT;
        }
        return;
    }
    markdown_core_block_attach_paragraph_identifier(self->state, parser, paragraph);
}

static int continue_paragraph(const markdown_core_element_instance *self, markdown_core_parser *parser,
                              unsigned char *data, int length, markdown_core_member *container) {
    return !parser->blank;
}
static bool accepts_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_member *node) {
    (void)self;
    (void)parser;
    (void)node;
    return true;
}
static markdown_core_member *open_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_member *node, markdown_core_chunk *input) {
    (void)self;
    return node;
}

/* A text line no block claims opens a paragraph: the dialect's text block. */
static markdown_core_member *open_text(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_member *container, markdown_core_chunk *input) {
    container = markdown_core_block_parent_for(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!container) {
        return NULL;
    }
    parser->current = container;
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
    .inline_content = true,
    .paragraph = true,
    .finalize_block = markdown_core_paragraph_finalize,
    .open_text_block = open_text,
};
