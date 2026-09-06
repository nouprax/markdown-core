#include "harness_support.h"

#include <stdio.h>
#include <string.h>

void ts_ast_features_none(markdown_core_feature_set *features) { *features = 0; }

int ts_ast_feature_enable(markdown_core_feature_set *features, const char *name) {
    markdown_core_feature_set feature = markdown_core_feature_named(name);
    if (!feature) {
        return -1;
    }
    *features |= feature;
    return 0;
}

int ts_ast_profile(markdown_core_feature_set *features, const char *name) {
    if (strcmp(name, "commonmark") == 0) {
        *features = 0;
    } else if (strcmp(name, "gfm") == 0) {
        *features = markdown_core_features_through(MARKDOWN_CORE_FEATURE_LAYER_GFM);
    } else if (strcmp(name, "gfm-extended") == 0) {
        *features = markdown_core_features_through(MARKDOWN_CORE_FEATURE_LAYER_EXTENDED);
    } else if (strcmp(name, "default") == 0) {
        *features = markdown_core_features_all();
    } else {
        return -1;
    }
    return 0;
}

markdown_core_document *ts_ast_parse(const uint8_t *bytes, size_t length, markdown_core_feature_set features) {
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_parse_features(
        bytes, length, features, markdown_core_get_default_mem_allocator(), &error);
    if (!document) {
        markdown_core_string message = error ? markdown_core_error_get_message(error) : (markdown_core_string){NULL, 0};
        fprintf(stderr, "facade parse failed: ");
        if (message.data) {
            fwrite(message.data, 1, message.length, stderr);
        }
        fputc('\n', stderr);
        markdown_core_error_free(error);
        return NULL;
    }
    return document;
}
