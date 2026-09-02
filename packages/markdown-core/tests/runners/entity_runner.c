/* Entity reference suite.
 *
 * Parses "&name;" for every entity in the engine's entity table (included
 * directly from core/entities.inc) through the read-only facade and checks
 * that the concatenated Text literals of the resulting AST contain the
 * expected UTF-8 expansion.  No renderer and no HTML escaping are involved:
 * the AST carries the raw expansion bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

#include "entities.inc"

int main(void) {
    size_t i;
    size_t passed = 0, failed = 0, errored = 0;
    markdown_core_parse_options options;

    ts_ast_options_none(&options);

    for (i = 0; i < ENT_TABLE_SIZE; i++) {
        uint32_t packed = markdown_core_entities[i];
        size_t entity_length = ENT_NAME_SIZE(packed);
        size_t expansion_length = ENT_REPL_SIZE(packed);
        const char *entity = (const char *)markdown_core_entity_text + ENT_TEXT_IDX(packed);
        const char *expansion = entity + entity_length;
        char input[ENT_MAX_LENGTH + 3];
        markdown_core_document *document;
        char *text;
        size_t text_length = 0;

        input[0] = '&';
        memcpy(input + 1, entity, entity_length);
        input[entity_length + 1] = ';';
        input[entity_length + 2] = 0;
        document = ts_ast_parse((const uint8_t *)input, entity_length + 2, &options);
        if (!document) {
            fprintf(stderr, "%.*s [ERRORED]\n", (int)entity_length, entity);
            errored++;
            continue;
        }
        text = ts_ast_concat_text(markdown_core_document_semantic(document), &text_length);
        if (!text) {
            fprintf(stderr, "%.*s [ERRORED (traversal)]\n", (int)entity_length, entity);
            errored++;
        } else if (text_length == expansion_length && memcmp(text, expansion, expansion_length) == 0) {
            passed++;
        } else {
            fprintf(stderr, "%.*s [FAILED]\n  input: %s\n  text:  %s\n", (int)entity_length, entity, input, text);
            failed++;
        }
        free(text);
        markdown_core_document_free(document);
    }

    printf("%zu passed, %zu failed, %zu errored\n", passed, failed, errored);
    return (failed || errored) ? 1 : 0;
}
