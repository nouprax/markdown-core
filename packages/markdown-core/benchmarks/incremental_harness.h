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
 * steps and one `flush`, then `edit` steps (plan 12.3), each of which
 * returns the new document, and closes. A document a subject returns is the
 * subject's, and is valid until its next step or its close. This file holds
 * the two subjects: `session`, the engine's session, and `reparse`, which
 * keeps the text and parses the whole of it on every step.
 */
#ifndef MARKDOWN_CORE_INCREMENTAL_HARNESS_H
#define MARKDOWN_CORE_INCREMENTAL_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <markdown_core.h>

/* THE BASE'S NAMES. The stream benchmark rebuilds this harness against its
 * base revision (scripts/benchmark/run.mjs, buildBaseline), whose session may
 * still name its flush `finalize` and its close `free`; CMakeLists.txt finds
 * out and defines MARKDOWN_CORE_BENCH_SESSION_FINALIZE. Gone once no base
 * predates the rename. */
#ifdef MARKDOWN_CORE_BENCH_SESSION_FINALIZE
#define markdown_core_session_flush markdown_core_session_finalize
#define markdown_core_session_close markdown_core_session_free
#define markdown_core_wire_session_flush markdown_core_wire_session_finalize
#define MARKDOWN_CORE_SESSION_FLUSHED MARKDOWN_CORE_SESSION_FINALIZED
#endif

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
/* Replaces [start, end) with `length` bytes. The range must lie in the text. */
bool ih_text_replace(ih_text *text, size_t start, size_t end, const uint8_t *bytes, size_t length);
void ih_text_free(ih_text *text);

/* The coordinate unit a subject counts the columns of its scope queries in. */
typedef enum ih_unit { IH_UTF8 = 0, IH_UTF16 = 1 } ih_unit;

const char *ih_unit_name(ih_unit unit);

/* The number of UTF-16 code units in the first `offset` bytes. A byte counts
 * the units of the scalar it begins: a continuation byte none, a byte that
 * begins a four-byte scalar two, any other byte one, whatever the bytes are,
 * as scope queries count columns. */
size_t ih_utf16_units(const uint8_t *bytes, size_t offset);

/* The byte offset of `units` UTF-16 code units: the first byte that begins a
 * scalar with `units` before it, or the end. False when `units` falls
 * between the two units of one scalar or past the end. */
bool ih_utf8_offset(const uint8_t *bytes, size_t length, size_t units, size_t *offset);

/* Whether `offset` is a scalar boundary of `bytes`: the end, or a byte that
 * begins a scalar. */
bool ih_utf8_boundary(const uint8_t *bytes, size_t length, size_t offset);

/* The status an edit fails with while its subject still streams (plan 12.3):
 * MARKDOWN_CORE_SESSION_STREAMING, which the public header names from step
 * E1 on. */
#define IH_STREAMING ((markdown_core_status)5)

/* ---------------------------------------------------------------- scripts */

/* A scripted identity expectation (gates 4.5), kept as written for the
 * oracles that read ids. */
typedef struct ih_expectation {
    char *verb;  /* kept, new, retired, changed, only */
    char *kind;  /* a canonical AST kind name, or NULL for `only` */
    size_t at;   /* retired: the old position; otherwise the new one */
    size_t from; /* kept: the old position */
} ih_expectation;

/* What a step does: append `text`, flush, edit, or make an edit the
 * subject must refuse (gates 4.13). */
typedef enum ih_step_kind { IH_APPEND = 0, IH_FLUSH, IH_EDIT, IH_REJECT } ih_step_kind;

/* One step. `length` bytes at `text`, which the step's owner holds, are an
 * append's text or the text an edit puts in place of [start, end). An edit's
 * offsets are UTF-8 bytes of the text before it, and a runner in UTF-16
 * converts them through its text model; a refused edit's are in `unit`, and
 * only a subject opened in `unit` runs it. With `bounded`, the bytes of the
 * new text the edit reads again lie in [from, to), and with `exact` they are
 * those bytes (gates 4.12). */
typedef struct ih_step {
    ih_step_kind kind;
    const uint8_t *text;
    size_t length;
    size_t start, end;
    ih_unit unit;
    markdown_core_status refusal;
    bool bounded, exact;
    size_t from, to;
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
     * text, and `flush` ends it; after `flush` both answer
     * MARKDOWN_CORE_SESSION_FLUSHED. */
    markdown_core_status (*append)(void *subject, const uint8_t *text, size_t length,
                                   const markdown_core_document **document);
    markdown_core_status (*flush)(void *subject, const markdown_core_document **document);
    /* The subject's text, as UTF-8. */
    const uint8_t *(*text)(const void *subject, size_t *length);
    void (*close)(void *subject);
    /* Replaces [start, end) of the flushed text, in the subject's unit,
     * with `length` bytes: IH_STREAMING before `flush`, and
     * MARKDOWN_CORE_OUT_OF_BOUNDS for a range that is reversed, ends past the
     * text or has an offset that is no scalar boundary. NULL for a subject
     * that takes no edits. */
    markdown_core_status (*edit)(void *subject, size_t start, size_t end, const uint8_t *text, size_t length,
                                 const markdown_core_document **document);
    /* The bytes [*from, *to) of its new text the last edit read again: the
     * damage bound of gates 4.12. NULL for a subject that states none. */
    void (*reread)(const void *subject, size_t *from, size_t *to);
    /* Whether the subject's ids follow the lineage (4.2 to 4.5); `reparse`
     * numbers every parse from 1. */
    bool keeps;
} ih_subject_class;

/* The `session` subject (gates section 2): the engine's session, opened in
 * the subject's unit. */
extern const ih_subject_class ih_session;

/* The engine's session a `session` subject holds, for the harness that
 * steps it through another entry. */
markdown_core_session *ih_session_of(void *subject);

/* The `reparse` subject: it keeps the text and calls the one parse entry on
 * the whole of it on every step, an edit too. */
extern const ih_subject_class ih_reparse;

/* The reparse subject's two halves, for the harness that measures it: extend
 * its text, and parse the whole text, releasing the previous document.
 * `append` is exactly the one followed by the other, and `flush` the parse
 * alone. */
bool ih_reparse_extend(void *subject, const uint8_t *text, size_t length);
bool ih_reparse_parse(void *subject, const markdown_core_document **document);
/* The edit's half that changes the text: the edit is exactly it followed by
 * the parse. */
markdown_core_status ih_reparse_replace(void *subject, size_t start, size_t end, const uint8_t *text, size_t length);

#ifdef __cplusplus
}
#endif

#endif
