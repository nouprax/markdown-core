#include "document.h"
#include "block_internal.h"
#include "properties.h"
#include "heading.h"
#include "footnote.h"
#include "specimen.h"
#include "paragraph.h"
#include "block_identifier.h"

static void init_document(markdown_core_parser *parser) {
    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs = markdown_core_footnote_definition_map_new(parser->mem);
    if (!parser->refmap || !parser->footnote_defs) {
        parser->oom = true;
    }
}
static void dispose_document(markdown_core_parser *parser) {
    markdown_core_block_dispose_headings(parser, &parser->headings);
    markdown_core_key_index_free(&parser->anchors.index);
    markdown_core_key_index_free(&parser->anchors.resources);
    parser->mem->free(parser->footnotes.values);
    parser->footnotes.values = NULL;
    parser->mem->free(parser->specimens.values);
    parser->specimens.values = NULL;
    markdown_core_key_index_free(&parser->specimen_ids);
    if (parser->refmap) {
        markdown_core_map_free(parser->refmap);
        parser->refmap = NULL;
    }
    if (parser->footnote_defs) {
        markdown_core_map_free(parser->footnote_defs);
        parser->footnote_defs = NULL;
    }
}
static void prepare_document(markdown_core_parser *parser) {
    markdown_core_block_prepare_specimens(parser);
    if (parser->oom) {
        return;
    }
    markdown_core_block_prepare_headings(parser, &parser->headings);
    if (parser->oom) {
        return;
    }
    if (!markdown_core_key_index_init(&parser->anchors.index, parser->mem, parser->headings.count) ||
        !markdown_core_key_index_init(&parser->anchors.resources, parser->mem, 0)) {
        parser->oom = true;
    }
}
static void observe_inline(markdown_core_parser *parser, markdown_core_node *node) {
    if (parser->headings.count) {
        markdown_core_block_reserve_node_anchor(parser, &parser->anchors, node);
    }
}
static void finish_document(markdown_core_parser *parser) {
    if ((parser->refmap && parser->refmap->oom) || (parser->footnote_defs && parser->footnote_defs->oom)) {
        parser->oom = true;
    }
    if (!parser->oom) {
        markdown_core_block_finalize_footnotes(parser);
    }
    if (!parser->oom) {
        markdown_core_specimen_finish(parser);
    }
    if (!parser->oom) {
        markdown_core_block_finalize_heading_anchors(parser, &parser->headings, &parser->anchors);
    }
    markdown_core_key_index_free(&parser->anchors.index);
    markdown_core_key_index_free(&parser->anchors.resources);
    markdown_core_block_dispose_headings(parser, &parser->headings);
}
const markdown_core_extension MARKDOWN_CORE_EXTENSION_DOCUMENT = {
    .name = "document",
    .init_document = init_document,
    .dispose_document = dispose_document,
    .read_document_prefix = markdown_core_properties_parse,
    .prepare_document = prepare_document,
    .finish_document = finish_document,
    .observe_inline = observe_inline,
    .open_text_block = markdown_core_paragraph_open_text,
};
