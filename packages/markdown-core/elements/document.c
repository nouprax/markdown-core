#include "alloc.h"
#include "document.h"
#include "block_internal.h"
#include "properties.h"
#include "heading.h"
#include "footnote.h"
#include "specimen.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { DOCUMENT_HEADING, DOCUMENT_FOOTNOTE, DOCUMENT_SPECIMEN };
static const markdown_core_element *const DOCUMENT_PEERS[] = {[DOCUMENT_HEADING] = &MARKDOWN_CORE_ELEMENT_HEADING,
                                                              [DOCUMENT_FOOTNOTE] = &MARKDOWN_CORE_ELEMENT_FOOTNOTE,
                                                              [DOCUMENT_SPECIMEN] = &MARKDOWN_CORE_ELEMENT_SPECIMEN,
                                                              NULL};

/* THE DOCUMENT LIFECYCLE. The document element drives it and owns only the
 * reference map and the properties grammar's work, its parse record; the
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
    if (parser->refmap) {
        markdown_core_map_free(parser->refmap);
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
static void observe_inline(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *node) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_observe(headings, parser, node);
    }
}
static void finish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    const markdown_core_element_instance *footnotes = self->peers[DOCUMENT_FOOTNOTE];
    const markdown_core_element_instance *specimens = self->peers[DOCUMENT_SPECIMEN];
    if ((parser->refmap && parser->refmap->oom) || (footnotes && markdown_core_footnotes_lost(footnotes))) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (!parser->error && footnotes) {
        markdown_core_block_finalize_footnotes(footnotes, parser);
    }
    if (!parser->error && specimens) {
        markdown_core_specimen_finish(specimens, parser);
    }
    if (!parser->error && headings) {
        markdown_core_headings_finish(headings, parser);
    }
    if (headings) {
        markdown_core_headings_dispose(headings);
    }
}
static size_t read_document_prefix(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   const unsigned char *source, size_t length) {
    return markdown_core_properties_parse(self->state, parser, source, length);
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_DOCUMENT = {
    .peers = DOCUMENT_PEERS,
    .state_size = sizeof(markdown_core_properties_work),
    .name = "document",
    .init_document = init_document,
    .dispose_document = dispose_document,
    .read_document_prefix = read_document_prefix,
    .prepare_document = prepare_document,
    .finish_document = finish_document,
    .observe_inline = observe_inline,
};
