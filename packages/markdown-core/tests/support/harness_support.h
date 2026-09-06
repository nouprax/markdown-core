#ifndef MARKDOWN_CORE_HARNESS_SUPPORT_H
#define MARKDOWN_CORE_HARNESS_SUPPORT_H

#include "test_support.h"

#include "ast_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Harness layer selection -------------------------------------------------- */

/* The harness's one lever. The dialect has no switches: the product parse
 * recognizes every registered feature, and these functions exist so a
 * conformance suite can run the base layer or the GFM layer ALONE against
 * the oracle that judges it. A `markdown_core_feature_set` names rows of the
 * feature registry (`extensions/feature-registry.h`), which the installed
 * shared library does not export -- so a runner that links this support
 * library links the static engine, and nothing here reaches a shipping
 * surface. */

/* The base layer alone: no registered feature. */
void ts_ast_features_none(markdown_core_feature_set *features);

/* Adds the registered feature a fixture tag or `--feature` names ("table",
 * "footnotes", "directive", ...). Returns 0 on success, -1 for a name the
 * registry does not carry: an unknown tag fails the suite instead of
 * silently parsing another language. */
int ts_ast_feature_enable(markdown_core_feature_set *features, const char *name);

/* The harness executable's `--profile` shorthands, each a layer of the
 * registry: `commonmark` is the base alone, `gfm` the GFM layer, `gfm-extended`
 * the GFM layer plus the repository's own syntax, and `default` every row.
 * Replaces `*features`. Returns 0 on success, -1 for an unknown name. */
int ts_ast_profile(markdown_core_feature_set *features, const char *name);

/* Parses the named features through the facade's one transaction; prints the
 * facade error message to stderr and returns NULL on failure. */
markdown_core_document *ts_ast_parse(const uint8_t *bytes, size_t length, markdown_core_feature_set features);

#ifdef __cplusplus
}
#endif

#endif
