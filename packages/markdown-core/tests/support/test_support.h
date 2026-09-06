#ifndef MARKDOWN_CORE_TEST_SUPPORT_H
#define MARKDOWN_CORE_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <markdown_core.h>

#include "ast_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared native test support for the CTest suites.  Every runner links this
 * library instead of re-implementing fixture, comparison, or failure-report
 * glue.  All verification goes through the read-only markdown_core facade
 * (parse, accessors, canonical AST dump); no renderer is ever invoked.  All
 * comparisons are UTF-8 byte comparisons; all diffs are line-oriented and
 * deterministic. */

#define TS_MAX_EXTENSIONS 16

typedef struct ts_spec_case {
    char *markdown;
    size_t markdown_length; /* bytes; the markdown may contain NULs */
    char *expected;
    char *section;
    char *extensions[TS_MAX_EXTENSIONS];
    size_t extension_count;
    int example;
    int start_line;
    int end_line;
} ts_spec_case;

typedef struct ts_spec_file {
    ts_spec_case *cases;
    size_t count;
} ts_spec_file;

/* File IO -------------------------------------------------------------- */

/* Reads a whole file.  Returns NULL on failure.  The buffer is always
 * NUL-terminated one byte past *length. */
uint8_t *ts_read_file(const char *path, size_t *length);

/* Spec fixtures --------------------------------------------------------- */

/* Parses a CommonMark-style spec fixture (32-backtick example fences with a
 * `.` separator).  Examples flagged `disabled` are skipped.  Returns 0 on
 * success. */
int ts_spec_load(const char *path, ts_spec_file *out);
void ts_spec_free(ts_spec_file *file);

/* Harness layer selection -------------------------------------------------- */

/* The harness's one lever. The dialect has no switches: the product parse
 * recognizes every registered feature, and these functions exist so a
 * conformance suite can run the base layer or the GFM layer ALONE against
 * the oracle that judges it. A `markdown_core_feature_set` names rows of the
 * feature registry (`extensions/feature-registry.h`); nothing here reaches a
 * shipping surface. */

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

/* Traversal -------------------------------------------------------------- */

/* Pre-order callback; return non-zero to abort the walk. */
typedef int (*ts_ast_visit_fn)(const markdown_core_node *node, void *context);

/* Iterative pre-order walk over the subtree rooted at `root` (call it on the
 * document root; following siblings of `root` are walked too).  Never
 * recurses, so pathologically deep trees are safe.  Returns the first
 * non-zero visitor result, 0 on completion, or -1 on allocation failure. */
int ts_ast_walk(const markdown_core_node *root, ts_ast_visit_fn visit, void *context);

/* Counts every node kind in the subtree.  `counts` must hold
 * MARKDOWN_CORE_KIND_TABLE_CELL + 1 entries.  Returns 0 on success. */
int ts_ast_count_kinds(const markdown_core_node *root, size_t *counts);
#define TS_KIND_COUNT (MARKDOWN_CORE_KIND_TABLE_CELL + 1)

/* Concatenates the literals of every Text node in pre-order into a malloc'd
 * NUL-terminated buffer (embedded NULs impossible: parser replaces them). */
char *ts_ast_concat_text(const markdown_core_node *root, size_t *length);

/* Comparison and failure reporting -------------------------------------- */

/* Prints a deterministic line diff between expected and actual to stream. */
void ts_print_line_diff(FILE *stream, const char *expected, const char *actual);

/* Deterministic data ----------------------------------------------------- */

/* xorshift64* PRNG for reproducible fuzz-smoke inputs. */
typedef struct ts_prng {
    uint64_t state;
} ts_prng;

void ts_prng_seed(ts_prng *prng, uint64_t seed);
uint64_t ts_prng_next(ts_prng *prng);

/* Appends `unit` to a growable buffer `count` times.  The buffer is
 * NUL-terminated.  Returns the malloc'd buffer; *length receives the byte
 * length. */
char *ts_repeat(const char *unit, size_t count, size_t *length);

/* Monotonic clock in nanoseconds for relative benchmark measurements. */
uint64_t ts_monotonic_ns(void);

#ifdef __cplusplus
}
#endif

#endif
