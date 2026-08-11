#ifndef MARKDOWN_CORE_EDIT_REPLAY_H
#define MARKDOWN_CORE_EDIT_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include <markdown_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared edit replay harness: drives a document through successive edits
 * next to a shadow copy of the text and verifies, on every edit, that the
 * edited document dumps byte-identically to a one-shot parse of the shadow
 * bytes, and that the delta accounts for every observable node change (an
 * id->revision mirror maintained purely from deltas is compared against a
 * fresh walk).
 *
 * The equivalence runner and the fuzzing entry points share this harness so
 * every driver checks the same invariants: failures are routed through the
 * report callback, and each er_* call returns 0 on success or -1 after
 * reporting. */

typedef void (*er_report_fn)(void *user, const char *context, const char *message);

typedef struct er_text {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} er_text;

typedef struct er_mirror_entry {
    markdown_core_node_id id;
    uint64_t revision;
} er_mirror_entry;

typedef struct er_mirror {
    er_mirror_entry *entries;
    size_t count;
    size_t capacity;
} er_mirror;

typedef struct er_replay {
    const char *context;
    markdown_core_document *document;
    er_text shadow;
    er_mirror mirror;
    const markdown_core_parse_options *options;
    er_report_fn report;
    void *user;
} er_replay;

/* Parses the empty document and seeds the mirror with its revision-0 root.
 * `options` must stay valid until er_replay_close. */
int er_replay_open(
    er_replay *replay,
    const char *context,
    const markdown_core_parse_options *options,
    er_report_fn report,
    void *user
);

void er_replay_close(er_replay *replay);

/* Applies one splice to the shadow text and hands the whole of it over.  The shadow buffer
 * is always allocated (even while empty) and NUL-terminated one byte past
 * `length`, so scripted drivers can locate edit positions with strstr. */
int er_replay_edit(er_replay *replay, size_t start, size_t end, const uint8_t *bytes, size_t length);

/* Commits the document, folds the delta into the mirror, verifies the mirror
 * against a fresh walk, and compares the document dump with a one-shot parse
 * of the shadow text. */
int er_replay_commit(er_replay *replay);

/* Deterministic edit-script interpreter: replays `script` as a document edit
 * sequence with full per-commit verification, then commits once more at the
 * end.  Every byte is meaningful, so a coverage-guided fuzzer can drive it
 * directly.  The format is:
 *
 *   options: two little-endian bytes; bit i enables parse option i in
 *     markdown_core_parse_options field order (9 bits used)
 *   then operations until the script runs out, selected by op & 3:
 *     0 insert   pos16 len8 bytes...
 *     1 delete   pos16 span16
 *     2 replace  pos16 span16 len8 bytes...
 *     3 commit
 *   pos16/span16 are little-endian and taken modulo the shadow length so
 *   every splice is in range; len8 counts literal bytes taken from the
 *   script (clamped to what remains).  Operand reads past the end supply
 *   zeroes.
 *
 * Returns 0 when every commit verified, -1 after reporting a failure. */
int er_script_replay(const uint8_t *script, size_t length, const char *context, er_report_fn report, void *user);

#ifdef __cplusplus
}
#endif

#endif

/* --- edit-shaped test harness over a whole-text engine -----------------
 *
 * The engine has one operation: `Document(markdown, options)` and
 * `commit(markdown)`, both taking whole text. A great many tests are written
 * as a sequence of byte-range edits, which is what an editor produces and
 * what they are meant to exercise. `mc_doc` keeps that shape where it belongs
 * — in the test — by holding the text itself and handing the WHOLE of it to
 * the engine on every commit. */
typedef struct mc_doc {
    markdown_core_document *document;
    char *text;
    size_t length;
    size_t capacity;
} mc_doc;

bool mc_doc_open(mc_doc *doc, const markdown_core_parse_options *options, markdown_core_error **error);
bool mc_doc_edit(mc_doc *doc, size_t start, size_t end, const void *bytes, size_t length);
bool mc_doc_commit(mc_doc *doc, markdown_core_delta **delta, markdown_core_error **error);
void mc_doc_close(mc_doc *doc);
