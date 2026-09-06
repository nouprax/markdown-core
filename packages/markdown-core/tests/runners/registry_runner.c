/* The feature registry's contract, which the oracle gates stand on.
 *
 * Three invariants, none of which a fixture can state:
 *
 *   names    the registry carries exactly the reviewed names, in the reviewed
 *            layers, so a feature that lands without a row -- or a row whose
 *            layer moved -- is a reviewed change here and not a silent one in
 *            what the cmark-gfm gate compares;
 *   layers   the harness shorthands select exactly their layer: `commonmark`
 *            nothing, `gfm` the GFM layer, `gfm-extended` that plus the
 *            repository's own syntax, `default` every row;
 *   levers   every row can be excluded on its own and attached on its own,
 *            witnessed by a construct that appears exactly when the row is
 *            selected -- the property the `*-layer-gates` fixtures prove for
 *            two rows and this runner proves for all of them;
 *
 * and one bridge: the public entry parses the whole registry, so the product
 * and the harness's `default` are the same language.
 *
 *   registry_runner
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static int failures = 0;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        failures++;
    }
}

typedef struct expected_row {
    const char *name;
    markdown_core_feature_layer layer;
    const char *source;
    const char *witness;
} expected_row;

/* THE REVIEWED REGISTRY. Adding a feature means adding a row here in the same
 * change, with the input and the dump fragment that show it acting. */
static const expected_row ROWS[] = {
    {"table", MARKDOWN_CORE_FEATURE_LAYER_GFM, "| a |\n| --- |\n| b |\n", "Table scope="},
    {"strikethrough", MARKDOWN_CORE_FEATURE_LAYER_GFM, "~~x~~\n", "Strikethrough scope="},
    {"autolink", MARKDOWN_CORE_FEATURE_LAYER_GFM, "www.example.com\n", "Link scope="},
    {"tasklist", MARKDOWN_CORE_FEATURE_LAYER_GFM, "- [x] task\n", "checked=true"},
    {"footnotes", MARKDOWN_CORE_FEATURE_LAYER_GFM, "ref[^a]\n\n[^a]: note\n", "FootnoteReference scope="},
    {"formula", MARKDOWN_CORE_FEATURE_LAYER_EXTENDED, "$x$\n", "Formula scope="},
    {"directive", MARKDOWN_CORE_FEATURE_LAYER_EXTENDED, ":badge[label]\n", "Directive scope="},
    {"strip-html-comments", MARKDOWN_CORE_FEATURE_LAYER_PRODUCT, "before <!-- kept --> after\n",
     "literal=\"before  after\""},
};
#define ROW_COUNT (sizeof(ROWS) / sizeof(ROWS[0]))

static char *dump_with(const char *source, markdown_core_feature_set features) {
    markdown_core_document *document = ts_ast_parse((const uint8_t *)source, strlen(source), features);
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    if (!document) {
        return NULL;
    }
    if (!markdown_core_document_dump(document, &dump, &length, &error)) {
        markdown_core_error_free(error);
        markdown_core_document_free(document);
        return NULL;
    }
    markdown_core_document_free(document);
    return (char *)dump;
}

static void check_names(void) {
    size_t index;
    check(markdown_core_feature_count() == ROW_COUNT, "the registry carries exactly the reviewed rows");
    for (index = 0; index < ROW_COUNT; index++) {
        const markdown_core_feature *feature = markdown_core_feature_at(index);
        check(feature != NULL && strcmp(feature->name, ROWS[index].name) == 0,
              "registry row names are the reviewed ones, in order");
        check(feature != NULL && feature->layer == ROWS[index].layer, "registry row layers are the reviewed ones");
        check(feature != NULL && ((feature->extension_bit != 0) != (feature->option_bit != 0)),
              "a row is exactly one of a parser extension and an engine scanner");
        check(markdown_core_feature_named(ROWS[index].name) == ((markdown_core_feature_set)1u << index),
              "a name resolves to its own row's bit");
    }
    check(markdown_core_feature_at(ROW_COUNT) == NULL, "the registry ends where the reviewed rows end");
    check(markdown_core_feature_named("smart") == 0, "smart punctuation is not a feature");
    check(markdown_core_feature_named("tables") == 0, "a registered name has no alias");
    check(markdown_core_feature_named(NULL) == 0, "a null name is not a feature");
}

static markdown_core_feature_set rows_through(markdown_core_feature_layer layer) {
    markdown_core_feature_set features = 0;
    size_t index;
    for (index = 0; index < ROW_COUNT; index++) {
        if (ROWS[index].layer <= layer) {
            features |= (markdown_core_feature_set)1u << index;
        }
    }
    return features;
}

static void check_layers(void) {
    markdown_core_feature_set features = 0;
    check(ts_ast_profile(&features, "commonmark") == 0 && features == 0, "`commonmark` is the base layer alone");
    check(ts_ast_profile(&features, "gfm") == 0 && features == rows_through(MARKDOWN_CORE_FEATURE_LAYER_GFM),
          "`gfm` is exactly the GFM layer");
    check(ts_ast_profile(&features, "gfm-extended") == 0 &&
              features == rows_through(MARKDOWN_CORE_FEATURE_LAYER_EXTENDED),
          "`gfm-extended` is the GFM layer plus the repository's own syntax");
    check(ts_ast_profile(&features, "default") == 0 && features == rows_through(MARKDOWN_CORE_FEATURE_LAYER_PRODUCT),
          "`default` is every row");
    check(features == markdown_core_features_all(), "every row is what the product parses");
    check(ts_ast_profile(&features, "obsidian") != 0 && ts_ast_profile(&features, "commonmark-smart") != 0,
          "no source-named or smart shorthand exists");
}

static void check_levers(void) {
    markdown_core_feature_set all = markdown_core_features_all();
    size_t index;
    for (index = 0; index < ROW_COUNT; index++) {
        markdown_core_feature_set row = (markdown_core_feature_set)1u << index;
        char *with = dump_with(ROWS[index].source, all);
        char *without = dump_with(ROWS[index].source, all & ~row);
        char *alone = dump_with(ROWS[index].source, row);
        check(with && without && alone, "witness parses under every selection");
        if (with && without && alone) {
            check(strstr(with, ROWS[index].witness) != NULL, "a selected row acts");
            check(strstr(without, ROWS[index].witness) == NULL, "an excluded row is excluded on its own");
            check(strstr(alone, ROWS[index].witness) != NULL, "a row attaches on its own over the base layer");
        }
        markdown_core_dump_free((uint8_t *)with);
        markdown_core_dump_free((uint8_t *)without);
        markdown_core_dump_free((uint8_t *)alone);
    }
}

static void check_product_is_default(void) {
    static const char source[] = "# Heading\n\n| a |\n| --- |\n| ~~b~~ www.x.com $y$ :d[l] |\n\n- [x] t[^n]\n\n"
                                 "[^n]: note <!-- gone -->\n\n\"quotes\" -- ... 'it'\n";
    markdown_core_error *error = NULL;
    markdown_core_document *document =
        markdown_core_document_parse((const uint8_t *)source, sizeof(source) - 1, &error);
    uint8_t *product = NULL;
    size_t product_length = 0;
    char *harness = dump_with(source, markdown_core_features_all());
    check(document != NULL && error == NULL, "the public entry parses");
    if (document) {
        check(markdown_core_document_dump(document, &product, &product_length, &error), "the public entry dumps");
        markdown_core_document_free(document);
    }
    check(product && harness && strcmp((const char *)product, harness) == 0,
          "the public entry and the harness's `default` are one language");
    markdown_core_dump_free(product);
    markdown_core_dump_free((uint8_t *)harness);
    markdown_core_error_free(error);
}

int main(void) {
    check_names();
    check_layers();
    check_levers();
    check_product_is_default();
    if (failures) {
        fprintf(stderr, "%d registry check(s) failed\n", failures);
        return 1;
    }
    printf("feature registry: %zu rows, four shorthands, every row a lever\n", ROW_COUNT);
    return 0;
}
