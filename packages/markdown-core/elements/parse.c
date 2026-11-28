#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "array.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "markdown-core-elements.h"

#include <element.h>
#include <parser.h>
#include <utf8.h>

/* A PARSE reads the whole source once and publishes the document it ends
 * with. The revision lends the parse its pool and counts the ids it issues;
 * the document leaves with its nodes, which hold their slabs. Offsets are
 * int32 and every buffer derived from the source stays under half of that,
 * so the capacity is checked here, where text enters the library. */
markdown_core_status markdown_core_document_parse_in(const uint8_t *source, size_t length, markdown_core_text_unit unit,
                                                     markdown_core_document **document) {
    static const uint8_t empty[1] = {0};
    if (length > MARKDOWN_CORE_SOURCE_CAPACITY) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_document *parsed = markdown_core_alloc(1, sizeof(*parsed));
    if (!parsed) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_node_pool pool = {0};
    markdown_core_revision revision = {0};
    revision.pool = &pool;
    markdown_core_parser *parser = markdown_core_core_parser(NULL, NULL);
    markdown_core_node *root = NULL;
    if (parser) {
        const char *text = (const char *)(length ? source : empty);
        markdown_core_parser_start(parser, &revision);
        markdown_core_parser_commit(parser, text, length, true);
        root = markdown_core_parser_flush(parser);
        markdown_core_parser_discard(parser);
        markdown_core_parser_destroy(parser);
    }
    if (!root) {
        markdown_core_node_pool_dispose(&pool);
        markdown_core_free(parsed);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_node_pool_dispose(&pool);
    parsed->unit = unit;
    parsed->root = root;
    *document = parsed;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_document_parse(const uint8_t *source, size_t length,
                                                  markdown_core_document **document) {
    return markdown_core_document_parse_in(source, length, MARKDOWN_CORE_TEXT_UNIT_UTF8, document);
}
