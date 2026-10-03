#include "alloc.h"
#include "document.h"
#include "block_internal.h"
#include "properties.h"
#include "heading.h"
#include "footnote.h"
#include "specimen.h"
#include "ast_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { DOCUMENT_HEADING, DOCUMENT_FOOTNOTE, DOCUMENT_SPECIMEN };
static const markdown_core_element *const DOCUMENT_PEERS[] = {[DOCUMENT_HEADING] = &MARKDOWN_CORE_ELEMENT_HEADING,
                                                              [DOCUMENT_FOOTNOTE] = &MARKDOWN_CORE_ELEMENT_FOOTNOTE,
                                                              [DOCUMENT_SPECIMEN] = &MARKDOWN_CORE_ELEMENT_SPECIMEN,
                                                              NULL};

/* The document element's parse record: the properties grammar's work, first,
 * and the footnotes and specimens as they complete, for the lookup tables. */
typedef struct {
    markdown_core_properties_work properties;
    markdown_core_lookup_registry lookups;
} document_state;

/* THE DOCUMENT LIFECYCLE. The document element drives it and owns only the
 * reference map, its parse record and the publishing of nodes; the
 * headings, footnotes and specimens it finalizes are the state of their own
 * elements, its peers, which it asks through their lifecycle calls. An
 * element the dialect does not hold has nothing to finalize. */
static void init_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *footnotes = self->peers[DOCUMENT_FOOTNOTE];
    parser->refmap = markdown_core_reference_map_new();
    if (!parser->refmap) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (footnotes) {
        markdown_core_footnotes_begin(footnotes, parser);
    }
}
static void dispose_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    const markdown_core_element_instance *footnotes = self->peers[DOCUMENT_FOOTNOTE];
    const markdown_core_element_instance *specimens = self->peers[DOCUMENT_SPECIMEN];
    if (headings) {
        markdown_core_headings_dispose(headings);
    }
    if (footnotes) {
        markdown_core_footnotes_dispose(footnotes);
    }
    if (specimens) {
        markdown_core_specimen_dispose(specimens);
    }
    markdown_core_lookup_registry_dispose(&((document_state *)self->state)->lookups);
    if (parser->refmap) {
        markdown_core_map_free(&parser->pool->resources, parser->refmap);
        parser->refmap = NULL;
    }
}
static void prepare_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    const markdown_core_element_instance *specimens = self->peers[DOCUMENT_SPECIMEN];
    if (specimens) {
        markdown_core_block_prepare_specimens(specimens, parser);
    }
    if (parser->error) {
        return;
    }
    if (headings) {
        markdown_core_headings_prepare(headings, parser);
    }
}
static bool complete_node(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, const markdown_core_node *owner, uint32_t start) {
    return markdown_core_publish_node(parser, &((document_state *)self->state)->lookups, node, owner, start);
}
static void publish_relation(const markdown_core_element_instance *self, markdown_core_parser *parser,
                             const markdown_core_node *owner, uint32_t start, const markdown_core_node *part) {
    markdown_core_publish_relation(parser, &((document_state *)self->state)->lookups, owner, start, part);
}
static void finish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    const markdown_core_element_instance *footnotes = self->peers[DOCUMENT_FOOTNOTE];
    const markdown_core_element_instance *specimens = self->peers[DOCUMENT_SPECIMEN];
    if ((parser->refmap && parser->refmap->oom) || (footnotes && markdown_core_footnotes_lost(footnotes))) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (specimens) {
        markdown_core_specimen_dispose(specimens);
    }
    if (!parser->error && headings) {
        const markdown_core_lookup_registry *lookups = &((document_state *)self->state)->lookups;
        markdown_core_headings_finish(headings, parser, lookups->anchors.values, lookups->anchors.count);
    }
    if (headings) {
        markdown_core_headings_dispose(headings);
    }
}
static void publish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    if (!markdown_core_publish_tree(parser, &((document_state *)self->state)->lookups)) {
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
