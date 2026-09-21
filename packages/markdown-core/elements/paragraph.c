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

static int continue_paragraph(const markdown_core_element *self, markdown_core_parser *parser, unsigned char *data,
                              int length, markdown_core_node *container) {
    return !parser->blank;
}
/* A PARAGRAPH THAT HELD ONLY REFERENCE DEFINITIONS IS NOT A PARAGRAPH. Its
 * finalization consumed the definitions and left nothing, so it has no
 * inline content to parse and no place in the tree: it is released at its
 * EXIT, from inside the one finish walk, which is postorder -- the list it
 * sits in lays itself out at its own EXIT, after this, and sees the cleaned
 * children. A root is never released: it belongs to whoever holds it, and a
 * definition's term that was only definitions stays the empty term it is. */
static int contains_inlines(const markdown_core_element *element, markdown_core_node *node) {
    (void)element;
    return !(node->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY);
}
static markdown_core_finish_result finish_step(const markdown_core_element *element, markdown_core_parser *parser,
                                               markdown_core_node *node, markdown_core_event_type event, int is_root,
                                               void **state) {
    (void)element;
    (void)event;
    (void)state;
    assert(event == MARKDOWN_CORE_EVENT_EXIT);
    if (is_root || !(node->flags & MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY)) {
        return MARKDOWN_CORE_FINISH_CONTINUE;
    }
    markdown_core_parser_release_node(parser, node);
    return MARKDOWN_CORE_FINISH_CONSUMED;
}
static const markdown_core_node_type PARAGRAPH_EXIT_KINDS[] = {MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_NONE};
static bool accepts_lazy(markdown_core_parser *parser, markdown_core_node *node) { return true; }
static markdown_core_node *open_lazy(markdown_core_parser *parser, markdown_core_node *node) { return node; }

const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH = {
    .accepts_lazy = accepts_lazy,
    .open_lazy = open_lazy,

    .name = "paragraph",
    .last_block_matches = continue_paragraph,
    .content_mode = MARKDOWN_CORE_CONTENT_PROSE,
    /* Inline content, unless the paragraph was only definitions. */
    .contains_inlines_func = contains_inlines,
    .paragraph = true,
    .finalize_block = markdown_core_parser_finalize_paragraph,
    .finish_step = finish_step,
    .finish_exit_kinds = PARAGRAPH_EXIT_KINDS,
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
