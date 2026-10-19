/* THE INCREMENTAL HARNESS: one subject interface, one text model, one reader
 * for the workloads of docs/plans/2026-09-29-incremental-gates.md.
 *
 * Two programs drive subjects through it: the correctness runner
 * (tests/runners/incremental_runner.c), which checks the oracles after every
 * step, and the stream benchmark runner (stream_runner.c), which measures each
 * step under callgrind. Both read the same workload files, written by
 * scripts/benchmark/workloads.mjs, so the two never disagree about what a
 * script means.
 *
 * A subject opens in a coordinate unit with an initial text, takes `append`
 * steps and one `finalize`, each of which returns the new document, and
 * closes. A document a subject returns is the subject's, and is valid until
 * its next step or its close. This file holds the two subjects: `session`, the engine's session,
 * and `reparse`, which keeps the text and parses the whole of it on every
 * step.
 */
#ifndef MARKDOWN_CORE_INCREMENTAL_HARNESS_H
#define MARKDOWN_CORE_INCREMENTAL_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <markdown_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- text model */

/* A plain byte buffer, the harness's own copy of the text every subject must
 * hold. It is NUL-terminated one byte past `length`. */
typedef struct ih_text {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} ih_text;

bool ih_text_assign(ih_text *text, const uint8_t *bytes, size_t length);
bool ih_text_append(ih_text *text, const uint8_t *bytes, size_t length);
void ih_text_free(ih_text *text);

/* The coordinate unit a subject counts the columns of its scope queries in. */
typedef enum ih_unit { IH_UTF8 = 0, IH_UTF16 = 1 } ih_unit;

const char *ih_unit_name(ih_unit unit);

/* The number of UTF-16 code units in the first `offset` bytes. A byte counts
 * the units of the scalar it begins: a continuation byte none, a byte that
 * begins a four-byte scalar two, any other byte one, whatever the bytes are,
 * as scope queries count columns. */
size_t ih_utf16_units(const uint8_t *bytes, size_t offset);

/* Whether `offset` is a scalar boundary of `bytes`: the end, or a byte that
 * begins a scalar. */
bool ih_utf8_boundary(const uint8_t *bytes, size_t length, size_t offset);

/* ---------------------------------------------------------------- scripts */

/* A scripted identity expectation (gates 4.5), kept as written for the
 * oracles that read ids. */
typedef struct ih_expectation {
    char *verb;  /* kept, new, retired, changed, only */
    char *kind;  /* a canonical AST kind name, or NULL for `only` */
    size_t at;   /* retired: the old position; otherwise the new one */
    size_t from; /* kept: the old position */
} ih_expectation;

/* One append: `length` bytes at `text`, which the step's owner holds. */
typedef struct ih_step {
    const uint8_t *text;
    size_t length;
    ih_expectation *expectations;
    size_t expectation_count;
} ih_step;

/* A script holds its steps and everything they name: their texts are views
 * of `bytes`. */
typedef struct ih_script {
    char *name;
    char *family; /* the category it belongs to */
    ih_step *steps;
    size_t count;
    ih_text bytes;
} ih_script;

/* The scripts of one file, in the text format of workloads.mjs. */
typedef struct ih_scripts {
    ih_script *scripts;
    size_t count;
} ih_scripts;

/* On failure, writes a message naming the file and line to stderr and
 * returns false. */
bool ih_scripts_load(const char *path, ih_scripts *scripts);
void ih_scripts_free(ih_scripts *scripts);

/* A stream is a rule over its document (gates 3.2): the chunk ends of
 * `tokens`, `scalars` or `rows`, taken from `document`. `splits` is a family
 * of two-chunk streams and is enumerated by its caller, one per interior
 * scalar boundary. */
typedef struct ih_sizes {
    size_t *values;
    size_t count;
} ih_sizes;

bool ih_sizes_load(const char *path, ih_sizes *sizes);
void ih_sizes_free(ih_sizes *sizes);
bool ih_stream_ends(const uint8_t *document, size_t length, const char *family, const ih_sizes *tokens, size_t **ends,
                    size_t *count);
/* The append steps of chunks ending at `ends`: views of `document`, which
 * outlives them. The caller frees the array. */
ih_step *ih_chunks(const uint8_t *document, const size_t *ends, size_t count);

/* --------------------------------------------------------------- manifest */

typedef enum ih_case_kind { IH_CASE_DOCUMENT, IH_CASE_SCRIPT, IH_CASE_STREAM } ih_case_kind;

typedef struct ih_case {
    ih_case_kind kind;
    char *document; /* path, relative to the manifest's directory */
    char *script;   /* script: the script file's path; stream: the family */
    size_t *parts;  /* document: the byte length of each part it joins */
    size_t part_count;
} ih_case;

/* The set's categories, declared by `family` lines, and its cases. Every case
 * belongs to one category: a document to `documents`, a stream to its family,
 * a script to the family its script file names. */
typedef struct ih_manifest {
    char **families;
    size_t family_count;
    ih_case *cases;
    size_t count;
} ih_manifest;

/* Also refuses a document or stream whose family is not declared. */
bool ih_manifest_load(const char *path, ih_manifest *manifest);
/* The family of a document or stream case; NULL for a script case, whose
 * scripts each name their own. */
const char *ih_case_family(const ih_case *entry);
/* The declared family's index, or `family_count` when it is not declared. */
size_t ih_family_index(const ih_manifest *manifest, const char *family);
void ih_manifest_free(ih_manifest *manifest);

/* Read a whole file. The buffer is NUL-terminated one byte past `*length`. */
uint8_t *ih_read_file(const char *path, size_t *length);

/* ---------------------------------------------------------------- subject */

/* A subject is an opaque handle its class opens and closes, so a harness can
 * wrap one subject in another. */
typedef struct ih_subject_class {
    const char *name;
    /* Returns NULL, and no document, when the subject cannot be opened. */
    void *(*open)(ih_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document);
    /* Each step answers MARKDOWN_CORE_OK with the new document, or the
     * status of its failure and no document: `append` adds to the end of the
     * text, and `finalize` ends it; after `finalize` both answer
     * MARKDOWN_CORE_SESSION_FINALIZED. */
    markdown_core_status (*append)(void *subject, const uint8_t *text, size_t length,
                                   const markdown_core_document **document);
    markdown_core_status (*finalize)(void *subject, const markdown_core_document **document);
    /* The subject's text, as UTF-8. */
    const uint8_t *(*text)(const void *subject, size_t *length);
    void (*close)(void *subject);
} ih_subject_class;

/* The `session` subject (gates section 2): the engine's session, opened in
 * the subject's unit. */
extern const ih_subject_class ih_session;

/* The engine's session a `session` subject holds, for the harness that
 * steps it through another entry. */
markdown_core_session *ih_session_of(void *subject);

/* The `reparse` subject: it keeps the text and calls the one parse entry on
 * the whole of it on every step. */
extern const ih_subject_class ih_reparse;

/* The reparse subject's two halves, for the harness that measures it: extend
 * its text, and parse the whole text, releasing the previous document.
 * `append` is exactly the one followed by the other, and `finalize` the parse
 * alone. */
bool ih_reparse_extend(void *subject, const uint8_t *text, size_t length);
bool ih_reparse_parse(void *subject, const markdown_core_document **document);

#ifdef __cplusplus
}
#endif

#endif
