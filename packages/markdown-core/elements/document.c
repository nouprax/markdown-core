#include "alloc.h"
#include "document.h"
#include "block_internal.h"
#include "properties.h"
#include "heading.h"
#include "registry.h"
#include "ast_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { DOCUMENT_HEADING };
static const markdown_core_element *const DOCUMENT_PEERS[] = {[DOCUMENT_HEADING] = &MARKDOWN_CORE_ELEMENT_HEADING,
                                                              NULL};

/* The document element's parse record: the properties grammar's work. */
typedef struct {
    markdown_core_properties_work properties;
} document_state;

/* THE DOCUMENT LIFECYCLE. The document element drives it and owns the
 * parse's round of the registries (registry.h) and the publishing of nodes;
 * the headings it finalizes are the state of their own element, its peer,
 * which it asks through their lifecycle calls. A dialect without headings
 * has none to finalize. */
static void init_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    markdown_core_registries_begin(parser);
}
static void dispose_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_dispose(headings);
    }
    markdown_core_registries_end(parser);
}
/* After the block parse: the facts of what the parse read are dropped, and
 * the headings declare theirs and parse their inlines. */
static void prepare_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    markdown_core_registries_seal(parser);
    if (!parser->error && headings) {
        markdown_core_headings_prepare(headings, parser);
    }
}
static bool complete_node(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, const markdown_core_node *owner, uint32_t start) {
    (void)self;
    return markdown_core_publish_node(parser, node, owner, start);
}
static void publish_relation(const markdown_core_element_instance *self, markdown_core_parser *parser,
                             const markdown_core_node *owner, uint32_t start, const markdown_core_node *part) {
    (void)self;
    markdown_core_publish_relation(parser, owner, start, part);
}
/* Once the tree is complete: the headings take their anchors, and the round
 * resolves its lookups. */
static void finish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_finish(headings, parser);
        markdown_core_headings_dispose(headings);
    } else {
        markdown_core_registries_assign_anchors(parser);
        markdown_core_registries_resolve(parser);
    }
}
static void publish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    if (!markdown_core_publish_tree(parser)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
static size_t read_document_prefix(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    return markdown_core_properties_parse(&((document_state *)self->state)->properties, parser);
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_DOCUMENT = {
    .peers = DOCUMENT_PEERS,
    .state_size = sizeof(document_state),
    .name = "document",
    .init_document = init_document,
    .dispose_document = dispose_document,
    .read_document_prefix = read_document_prefix,
    .prepare_document = prepare_document,
    .finish_document = finish_document,
    .publish_document = publish_document,
    .complete_node = complete_node,
    .publish_relation = publish_relation,
};
