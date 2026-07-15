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

    for (i = 0; i < MARKDOWN_CORE_NUM_ENTITIES; i++) {
        const char *entity = (const char *)markdown_core_entities[i].entity;
        const char *expansion = (const char *)markdown_core_entities[i].bytes;
        char input[MARKDOWN_CORE_ENTITY_MAX_LENGTH + 4];
        markdown_core_document *document;
        char *text;

        snprintf(input, sizeof(input), "&%s;", entity);
        document = ts_ast_parse((const uint8_t *)input, strlen(input), &options);
        if (!document) {
            fprintf(stderr, "%s [ERRORED]\n", entity);
            errored++;
            continue;
        }
        text = ts_ast_concat_text(markdown_core_document_root(document), NULL);
        if (!text) {
            fprintf(stderr, "%s [ERRORED (traversal)]\n", entity);
            errored++;
        } else if (strstr(text, expansion)) {
            passed++;
        } else {
            fprintf(stderr, "%s [FAILED]\n  input: %s\n  text:  %s\n", entity, input, text);
            failed++;
        }
        free(text);
        markdown_core_document_free(document);
    }

    printf("%zu passed, %zu failed, %zu errored\n", passed, failed, errored);
    return (failed || errored) ? 1 : 0;
}
