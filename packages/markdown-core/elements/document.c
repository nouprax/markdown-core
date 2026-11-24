#include "alloc.h"
#include "document.h"
#include "block_internal.h"
#include "ast_internal.h"

/* THE DOCUMENT LIFECYCLE. The document element owns the properties
 * grammar's work, its parse record, and publishes the tree; the
 * definitions the parse declares are the parse's roster (roster.h), and
 * the headings name themselves in the parse's stages (element.h,
 * markdown_core_stage_ops). */
static void init_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    (void)parser;
}
/* AN EDIT REOPENS THE DOCUMENT (blocks.c, THE REOPEN): the published tree
 * keeps the children before `child`; what goes is matched against what the edit reads (ast.c,
 * A DOCUMENT REOPENED FOR AN EDIT). */
static bool reopen_document(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *root, uint32_t child, uint32_t start) {
    (void)self;
    return markdown_core_publish_reopen(parser, root, child, start);
}
/* THE EDIT TAKES THE OLD CHILDREN BACK (blocks.c, THE SPLICE): the
 * publish's records let the children go last, once nothing else can
 * fail. */
static bool splice_document(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *root, uint32_t child, uint32_t point) {
    (void)self;
    (void)root;
    return markdown_core_publish_splice(parser, child, point);
}
static void publish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    if (!markdown_core_publish_tree(parser)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* THE PREFIX, read before line 1: an envelope that closes is read as the
 * document's properties. */
static size_t read_document_prefix(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   markdown_core_reader *reader) {
    size_t fence = 0;
    if (!markdown_core_properties_closed(self->state, parser, reader, self, &fence) || parser->error) {
        return 0;
    }
    return markdown_core_properties_parse(self->state, parser, reader, fence);
}

static const markdown_core_edit_ops DOCUMENT_EDIT = {
    .stops = markdown_core_properties_stops,
};

static const markdown_core_document_ops DOCUMENT_OPS = {
    .kind = MARKDOWN_CORE_NODE_DOCUMENT,
    .init = init_document,
    .read = read_document_prefix,
    .reopen = reopen_document,
    .splice = splice_document,
    .publish = publish_document,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_DOCUMENT = {
    .state_size = sizeof(markdown_core_properties_work),
    .name = "document",
    .facets = MARKDOWN_CORE_PROPERTIES_FACETS,
    .edit = &DOCUMENT_EDIT,
    .document = &DOCUMENT_OPS,
};

/* A document's definition tables, but a session's, which it borrows (ast.c,
 * A SESSION'S DOCUMENT). Out of line, so that the free of every other kind
 * keeps its own registers. */
MARKDOWN_CORE_ATTRIBUTE((noinline)) void markdown_core_node_free_tables(markdown_core_document_value *document) {
    if (document->committed) {
        return;
    }
    markdown_core_definitions *const tables[] = {&document->footnotes, &document->specimens, &document->references};
    for (size_t i = 0; i < sizeof(tables) / sizeof(*tables); i++) {
        markdown_core_free((void *)tables[i]->all.nodes);
        markdown_core_free((void *)tables[i]->labeled);
    }
    markdown_core_free((void *)document->reference_targets.nodes);
}

/* The document's record (markdown_core_record_ops): its definition tables,
 * which its publish makes -- no block the block phase writes, so no view,
 * has them --, and its metadata, a node-valued field the document holds
 * once its prefix is read. */
static void free_document(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_node_free_tables(node->as.document);
}

static int visit_document(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    return !node->as.document->metadata || visitor(&node->as.document->metadata, context);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_DOCUMENT = {
    .size = sizeof(markdown_core_document_value),
    .free = free_document,
    .visit = visit_document,
};

/* The metadata's record, which the document's prefix reader fills
 * (properties.c). */
static void free_metadata(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_metadata_fields_free(node->as.metadata);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_METADATA = {
    .size = sizeof(markdown_core_metadata_fields),
    .free = free_metadata,
};
