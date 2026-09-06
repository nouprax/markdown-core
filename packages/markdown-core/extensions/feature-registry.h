#ifndef MARKDOWN_CORE_FEATURE_REGISTRY_H
#define MARKDOWN_CORE_FEATURE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE FEATURE REGISTRY: every feature the dialect recognizes beyond the
 * CommonMark base, by name, with the engine bit that turns it on and the
 * layer whose oracle judges it.
 *
 * The dialect has no switches. The product parse turns on every row, so a
 * feature is public from the commit that adds its row and nothing else is
 * needed to publish it. The names exist for the conformance harness alone:
 * the cmark gate has to run the base layer by itself and the cmark-gfm gate
 * the GFM layer by itself, so `spec_runner`'s fixture tags, the harness
 * executable's `--profile` and `-e` shorthands, and the oracle gates select a
 * subset of these rows by name. No facade, binding, wire format, or installed
 * executable can reach a row: this header is not installed and none of these
 * symbols is exported from the shared library.
 *
 * One list, in one place. Before this table the same set was spelled four
 * times -- the facade defaults, the CLI's profile arithmetic, the test
 * support's tag table, and the manifest's option object -- and each could
 * drift from the others. */

typedef enum markdown_core_feature_layer {
    /** Judged by cmark-gfm 0.29.0.gfm.13: the GFM extension layer. */
    MARKDOWN_CORE_FEATURE_LAYER_GFM = 1,
    /** Judged by remark: the repository's own syntax. */
    MARKDOWN_CORE_FEATURE_LAYER_EXTENDED = 2,
    /** Judged by no oracle: product behavior the fixtures alone pin. */
    MARKDOWN_CORE_FEATURE_LAYER_PRODUCT = 3
} markdown_core_feature_layer;

typedef struct markdown_core_feature {
    /** The registered name: a fixture tag, a `-e` argument, a `--feature`. */
    const char *name;
    markdown_core_feature_layer layer;
    /** A `markdown_core_core_extension_bit`, or 0 for an engine scanner. */
    unsigned extension_bit;
    /** A `MARKDOWN_CORE_OPT_*` bit, or 0 for a parser extension. */
    int option_bit;
} markdown_core_feature;

/** A set of registry rows: bit `i` is the row `markdown_core_feature_at(i)`.
 * Wide enough for every row the dialect specification lists, and the table
 * is checked against this width where it is defined. */
typedef uint64_t markdown_core_feature_set;

size_t markdown_core_feature_count(void);

/** The row at `index`, or NULL past the end. */
const markdown_core_feature *markdown_core_feature_at(size_t index);

/** The set holding exactly the row named `name`, or the empty set when no row
 * carries that name. An unregistered name is not a feature. */
markdown_core_feature_set markdown_core_feature_named(const char *name);

/** Every row: the language the product parses. */
markdown_core_feature_set markdown_core_features_all(void);

/** Every row whose layer is at most `layer`, so the GFM layer alone is
 * `markdown_core_features_through(MARKDOWN_CORE_FEATURE_LAYER_GFM)`. */
markdown_core_feature_set markdown_core_features_through(markdown_core_feature_layer layer);

/** Translates a set into the engine's terms: the option word handed to the
 * parser and the extension mask handed to `markdown_core_core_extensions_attach`.
 * Bits beyond the registry are ignored. */
void markdown_core_features_resolve(markdown_core_feature_set features, int *options, unsigned *extensions);

#ifdef __cplusplus
}
#endif

#endif
