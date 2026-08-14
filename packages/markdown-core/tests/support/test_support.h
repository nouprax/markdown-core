#ifndef MARKDOWN_CORE_TEST_SUPPORT_H
#define MARKDOWN_CORE_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared native test support for the CTest suites.  Every runner links this
 * library instead of re-implementing fixture, comparison, or diagnostic
 * glue.  All verification goes through the read-only markdown_core facade
 * (parse, accessors, canonical AST dump); no renderer is ever invoked.  All
 * comparisons are UTF-8 byte comparisons; all diffs are line-oriented and
 * deterministic. */

/* A string view, spelled once. Every test owns its own bytes and hands the
 * whole of them over, which is what the engine's operation takes. */
static inline markdown_core_string mc_sv(const void *data, size_t length) {
    markdown_core_string view;
    view.data = (const uint8_t *)data;
    view.length = length;
    return view;
}

/* The caller's text. Tests that used to splice the document's buffer splice
 * this instead and hand the whole of it over. */
typedef struct mc_text {
    char *bytes;
    size_t length;
    size_t capacity;
} mc_text;

static inline bool mc_text_splice(mc_text *t, size_t start, size_t end, const void *bytes, size_t length) {
    size_t tail;
    if (start > end || end > t->length) {
        return false;
    }
    if (t->length - (end - start) + length + 1 > t->capacity) {
        size_t want = (t->length - (end - start) + length + 1) * 2;
        char *grown = (char *)realloc(t->bytes, want);
        if (!grown) {
            return false;
        }
        t->bytes = grown;
        t->capacity = want;
    }
    tail = t->length - end;
    memmove(t->bytes + start + length, t->bytes + end, tail);
    if (length) {
        memcpy(t->bytes + start, bytes, length);
    }
    t->length = start + length + tail;
    t->bytes[t->length] = 0;
    return true;
}

static inline void mc_text_free(mc_text *t) {
    free(t->bytes);
    t->bytes = NULL;
    t->length = 0;
    t->capacity = 0;
}

/* Hands `markdown` to the document and swaps the caller's handle. */
static inline bool mc_edit(
    markdown_core_document **document,
    markdown_core_string markdown,
    markdown_core_error **error
) {
    markdown_core_document *previous = *document;
    // The mutation supersedes the receiver, whose one remaining legal call
    // is free — which this helper performs, keeping the replace-in-place
    // shape every test was written against. On failure the receiver was
    // still the live head; releasing it ends the chain, which is the
    // fail-stop shape these tests want.
    markdown_core_document *successor = markdown_core_document_edit(previous, markdown, error);
    markdown_core_document_free(previous);
    *document = successor;
    return successor != NULL;
}

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

/* Facade parsing --------------------------------------------------------- */

/* Initializes every parse option to false (pure CommonMark). */
void ts_ast_options_none(markdown_core_parse_options *options);

/* Enables the option named by a fixture/suite tag ("table", "smart",
 * "formula", ...).  Returns 0 on success, -1 for unknown names. */
int ts_ast_enable(markdown_core_parse_options *options, const char *name);

/* Parses through the facade; prints the facade diagnostic to stderr and
 * returns NULL on failure. */
markdown_core_document *ts_ast_parse(const uint8_t *bytes, size_t length, const markdown_core_parse_options *options);

/* Traversal -------------------------------------------------------------- */

/* Pre-order callback; return non-zero to abort the walk. */
typedef int (*ts_ast_visit_fn)(const markdown_core_node *node, void *context);

/* Iterative pre-order walk over the subtree rooted at `root` (call it on the
 * document root; following siblings of `root` are walked too).  Never
 * recurses, so pathologically deep trees are safe.  Returns the first
 * non-zero visitor result, 0 on completion, or -1 on allocation failure. */
int ts_ast_walk(const markdown_core_node *root, ts_ast_visit_fn visit, void *context);

/* Counts every node kind in the subtree. `counts` must hold
 * MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE + 1 entries. Returns 0 on success. */
int ts_ast_count_kinds(const markdown_core_node *root, size_t *counts);
/* One past the last node kind. It must track the end of
 * markdown_core_node_kind: a stale value silently drops the kinds past it
 * from every count, and lets a caller asking about one read off the end of
 * the array. It named FOOTNOTE_REFERENCE until 2026-08-02, so CrossLink and
 * Embed had been uncountable since they were added. */
#define TS_KIND_COUNT (MARKDOWN_CORE_KIND_IMAGE_REFERENCE + 1)

/* Concatenates the literals of every Text node in pre-order into a malloc'd
 * NUL-terminated buffer (embedded NULs impossible: parser replaces them). */
char *ts_ast_concat_text(const markdown_core_node *root, size_t *length);

/* Comparison and diagnostics -------------------------------------------- */

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

/* Process user+kernel CPU time in nanoseconds for complexity measurements.
 * Unlike the monotonic clock, this excludes time while the test process is
 * descheduled by a shared runner. */
uint64_t ts_process_cpu_ns(void);

#ifdef __cplusplus
}
#endif

#endif
