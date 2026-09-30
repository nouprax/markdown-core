/* THE INCREMENTAL HARNESS: one subject interface, one text model, one reader
 * for the workloads of docs/plans/2026-09-29-incremental-gates.md.
 *
 * Two programs drive subjects through it: the correctness runner
 * (tests/runners/incremental_runner.c), which checks the oracles after every
 * step, and the edit benchmark runner (edit_runner.c), which measures each
 * step under callgrind. Both read the same workload files, written by
 * scripts/benchmark/workloads.mjs, so the two never disagree about what a
 * script means.
 *
 * A subject opens in a coordinate unit with an initial text, applies `edit`
 * (a batch of non-overlapping edits against the text before the batch) and
 * `append`, each of which returns the new document, and closes. A document a
 * subject returns is the subject's, and is valid until its next step or its
 * close. This file holds the two subjects: `session`, the engine's session,
 * and `reparse`, which keeps the text and parses the whole of it on every
 * step.
 */
#ifndef MARKDOWN_CORE_EDIT_HARNESS_H
#define MARKDOWN_CORE_EDIT_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <markdown_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- text model */

/* A plain byte buffer, the harness's own copy of the text every subject must
 * hold. */
typedef struct eh_text {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} eh_text;

bool eh_text_assign(eh_text *text, const uint8_t *bytes, size_t length);
/* Replace [start, end) with `length` bytes. The range must lie in the text. */
bool eh_text_replace(eh_text *text, size_t start, size_t end, const uint8_t *bytes, size_t length);
void eh_text_free(eh_text *text);

/* The coordinate unit a subject takes its offsets in. */
typedef enum eh_unit { EH_UTF8 = 0, EH_UTF16 = 1 } eh_unit;

const char *eh_unit_name(eh_unit unit);
bool eh_unit_parse(const char *name, eh_unit *unit);

/* The number of UTF-16 code units in the first `offset` bytes, or the byte
 * offset of `units` UTF-16 code units. A byte counts the units of the scalar
 * it begins: a continuation byte none, a byte that begins a four-byte scalar
 * two, any other byte one, whatever the bytes are (plan 4.4). The second
 * answers the first byte that begins a scalar with `units` before it, or the
 * end, and fails when `units` falls between the two units of one scalar or
 * past the end. */
size_t eh_utf16_units(const uint8_t *bytes, size_t offset);
bool eh_utf8_offset(const uint8_t *bytes, size_t length, size_t units, size_t *offset);

/* Whether `offset` is a scalar boundary of `bytes`: the end, or a byte that
 * begins a scalar. These are the offsets both units name; before a leading
 * continuation byte there is none. */
bool eh_utf8_boundary(const uint8_t *bytes, size_t length, size_t offset);

/* ---------------------------------------------------------------- scripts */

typedef struct eh_edit {
    size_t start;
    size_t end;
    uint8_t *text;
    size_t length;
} eh_edit;

typedef enum eh_step_kind { EH_STEP_EDIT, EH_STEP_APPEND, EH_STEP_REJECT } eh_step_kind;

/* A scripted identity expectation (gates 4.5), kept as written for the
 * oracles that read ids. */
typedef struct eh_expectation {
    char *verb;  /* kept, new, retired, changed, only */
    char *kind;  /* a canonical AST kind name, or NULL for `only` */
    size_t at;   /* retired: the old position; otherwise the new one */
    size_t from; /* kept: the old position */
} eh_expectation;

typedef struct eh_step {
    eh_step_kind kind;
    eh_unit unit; /* the unit a rejected step's offsets are written in */
    eh_edit *edits;
    size_t count;
    eh_expectation *expectations;
    size_t expectation_count;
} eh_step;

typedef struct eh_script {
    char *name;
    char *family; /* the category it belongs to: an edit family, `identity` or `rejections` */
    eh_step *steps;
    size_t count;
} eh_script;

/* The scripts of one file, in the text format of workloads.mjs. */
typedef struct eh_scripts {
    eh_script *scripts;
    size_t count;
} eh_scripts;

/* On failure, writes a message naming the file and line to stderr and
 * returns false. */
bool eh_scripts_load(const char *path, eh_scripts *scripts);
void eh_scripts_free(eh_scripts *scripts);

/* A stream script is a rule over its document (gates 3.3): the chunk ends of
 * `tokens`, `scalars` or `rows`, taken from `document`. `splits` is a family
 * of two-chunk scripts and is enumerated by its caller, one per interior
 * scalar boundary. */
typedef struct eh_sizes {
    size_t *values;
    size_t count;
} eh_sizes;

bool eh_sizes_load(const char *path, eh_sizes *sizes);
void eh_sizes_free(eh_sizes *sizes);
bool eh_stream_ends(const uint8_t *document, size_t length, const char *family, const eh_sizes *tokens, size_t **ends,
                    size_t *count);

/* --------------------------------------------------------------- manifest */

typedef enum eh_case_kind { EH_CASE_DOCUMENT, EH_CASE_EDITS, EH_CASE_STREAM } eh_case_kind;

typedef struct eh_case {
    eh_case_kind kind;
    char *document; /* path, relative to the manifest's directory */
    char *script;   /* edits: the script's path; stream: the family */
    size_t *parts;  /* document: the byte length of each part it joins */
    size_t part_count;
} eh_case;

/* The set's categories, declared by `family` lines, and its cases. Every case
 * belongs to one category: a document to `documents`, a stream to its family,
 * a script to the family its script file names. */
typedef struct eh_manifest {
    char **families;
    size_t family_count;
    eh_case *cases;
    size_t count;
} eh_manifest;

/* Also refuses a document or stream whose family is not declared. */
bool eh_manifest_load(const char *path, eh_manifest *manifest);
/* The family of a document or stream case; NULL for an edits case, whose
 * scripts each name their own. */
const char *eh_case_family(const eh_case *entry);
/* The declared family's index, or `family_count` when it is not declared. */
size_t eh_family_index(const eh_manifest *manifest, const char *family);
void eh_manifest_free(eh_manifest *manifest);

/* Read a whole file. The buffer is NUL-terminated one byte past `*length`. */
uint8_t *eh_read_file(const char *path, size_t *length);

/* ---------------------------------------------------------------- subject */

typedef enum eh_status {
    EH_OK = 0,
    /* The arguments name no range of the text (gates 4.8). */
    EH_INVALID = 1,
    /* The step could not be carried out: out of memory or a parse failure. */
    EH_FAILED = 2
} eh_status;

/* A subject is an opaque handle its class opens and closes, so a harness can
 * wrap one subject in another. */
typedef struct eh_subject_class {
    const char *name;
    /* Returns NULL, and no document, when the subject cannot be opened. */
    void *(*open)(eh_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document);
    eh_status (*edit)(void *subject, const eh_edit *edits, size_t count, const markdown_core_document **document);
    eh_status (*append)(void *subject, const uint8_t *text, size_t length, const markdown_core_document **document);
    /* The subject's text, as UTF-8. */
    const uint8_t *(*text)(const void *subject, size_t *length);
    void (*close)(void *subject);
} eh_subject_class;

/* The `session` subject (gates section 2): the engine's session, opened in
 * the subject's unit. */
extern const eh_subject_class eh_session;

/* The `reparse` subject: it keeps the text and calls the one parse entry on
 * the whole of it on every step. It takes the arguments the session takes:
 * ranges that lie in the text, in order and apart, and in UTF-16 no offset
 * between the two units of one scalar. */
extern const eh_subject_class eh_reparse;

/* The reparse subject's two halves, for the harness that wraps or measures
 * it: apply a step's arguments to its text, checking them in its unit, and
 * parse the whole text, releasing the previous document. `edit` and `append`
 * are exactly the one followed by the other. */
eh_status eh_reparse_apply_edit(void *subject, const eh_edit *edits, size_t count);
eh_status eh_reparse_apply_append(void *subject, const uint8_t *text, size_t length);
eh_status eh_reparse_parse(void *subject, const markdown_core_document **document);

#ifdef __cplusplus
}
#endif

#endif
