#include "feature-registry.h"

#include <string.h>

#include "markdown-core-extensions.h"

#include <markdown-core.h>

/* The order is the bit order of `markdown_core_feature_set` and nothing else:
 * attach order belongs to `core-extensions.c`, and the engine reads its option
 * bits by value. A row is added when its feature lands and deleted when its
 * bit goes; `strip-html-comments` is the row `M0` deletes, because an HTML
 * comment becomes a `Comment` node then and nothing strips anything. */
static const markdown_core_feature FEATURES[] = {
    {"table", MARKDOWN_CORE_FEATURE_LAYER_GFM, MARKDOWN_CORE_CORE_EXTENSION_TABLE, 0},
    {"strikethrough", MARKDOWN_CORE_FEATURE_LAYER_GFM, MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH, 0},
    {"autolink", MARKDOWN_CORE_FEATURE_LAYER_GFM, MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK, 0},
    {"tasklist", MARKDOWN_CORE_FEATURE_LAYER_GFM, MARKDOWN_CORE_CORE_EXTENSION_TASKLIST, 0},
    {"footnotes", MARKDOWN_CORE_FEATURE_LAYER_GFM, 0, MARKDOWN_CORE_OPT_FOOTNOTES},
    {"formula", MARKDOWN_CORE_FEATURE_LAYER_EXTENDED, MARKDOWN_CORE_CORE_EXTENSION_FORMULA, 0},
    {"directive", MARKDOWN_CORE_FEATURE_LAYER_EXTENDED, MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE, 0},
    {"strip-html-comments", MARKDOWN_CORE_FEATURE_LAYER_PRODUCT, 0, MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS},
};

#define FEATURE_COUNT (sizeof(FEATURES) / sizeof(FEATURES[0]))

size_t markdown_core_feature_count(void) { return FEATURE_COUNT; }

const markdown_core_feature *markdown_core_feature_at(size_t index) {
    return index < FEATURE_COUNT ? &FEATURES[index] : NULL;
}

markdown_core_feature_set markdown_core_feature_named(const char *name) {
    size_t i;
    if (!name) {
        return 0;
    }
    for (i = 0; i < FEATURE_COUNT; i++) {
        if (strcmp(name, FEATURES[i].name) == 0) {
            return (markdown_core_feature_set)1u << i;
        }
    }
    return 0;
}

markdown_core_feature_set markdown_core_features_all(void) {
    return markdown_core_features_through(MARKDOWN_CORE_FEATURE_LAYER_PRODUCT);
}

markdown_core_feature_set markdown_core_features_through(markdown_core_feature_layer layer) {
    markdown_core_feature_set features = 0;
    size_t i;
    for (i = 0; i < FEATURE_COUNT; i++) {
        if (FEATURES[i].layer <= layer) {
            features |= (markdown_core_feature_set)1u << i;
        }
    }
    return features;
}

void markdown_core_features_resolve(markdown_core_feature_set features, int *options, unsigned *extensions) {
    size_t i;
    *options = 0;
    *extensions = 0;
    for (i = 0; i < FEATURE_COUNT; i++) {
        if (features & ((markdown_core_feature_set)1u << i)) {
            *options |= FEATURES[i].option_bit;
            *extensions |= FEATURES[i].extension_bit;
        }
    }
}
