#ifndef MARKDOWN_CORE_SESSION_REPLAY_H
#define MARKDOWN_CORE_SESSION_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include <markdown_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared session replay harness: drives an incremental session next to a
 * shadow copy of the text and verifies, on every commit, that the session
 * dump is byte-identical to a one-shot parse of the shadow bytes, that the
 * delta stream accounts for every observable node change (an id->revision
 * mirror maintained purely from deltas is compared against a fresh walk),
 * and — with footnotes enabled — that numbering, resolution, and
 * back-reference queries equal a fresh session's on the same text.
 *
 * The equivalence runner and the fuzzing entry points share this harness so
 * every driver checks the same invariants: failures are routed through the
 * report callback, and each sr_* call returns 0 on success or -1 after
 * reporting. */

typedef void (*sr_report_fn)(void *user, const char *context, const char *message);

typedef struct sr_text {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} sr_text;

typedef struct sr_mirror_entry {
    markdown_core_node_id id;
    uint64_t revision;
} sr_mirror_entry;

typedef struct sr_mirror {
    sr_mirror_entry *entries;
    size_t count;
    size_t capacity;
} sr_mirror;

typedef struct sr_replay {
    const char *context;
    markdown_core_session *session;
    sr_text shadow;
    sr_mirror mirror;
    const markdown_core_parse_options *options;
    sr_report_fn report;
    void *user;
} sr_replay;

/* Opens a session and seeds the mirror with the revision-0 empty root.
 * `options` must stay valid until sr_replay_close. */
int sr_replay_open(
    sr_replay *replay,
    const char *context,
    const markdown_core_parse_options *options,
    sr_report_fn report,
    void *user
);

void sr_replay_close(sr_replay *replay);

/* Applies one splice to the session and the shadow text.  The shadow buffer
 * is always allocated (even while empty) and NUL-terminated one byte past
 * `length`, so scripted drivers can locate edit positions with strstr. */
int sr_replay_edit(sr_replay *replay, size_t start, size_t end, const uint8_t *bytes, size_t length);

/* Commits the session, folds the delta into the mirror, verifies the mirror
 * against a fresh walk, and compares the session dump with a one-shot parse
 * of the shadow text. */
int sr_replay_commit(sr_replay *replay);

/* Deterministic edit-script interpreter: replays `script` as a session edit
 * sequence with full per-commit verification, then commits once more at the
 * end.  Every byte is meaningful, so a coverage-guided fuzzer can drive it
 * directly.  The format is:
 *
 *   options: two little-endian bytes; bit i enables parse option i in
 *     markdown_core_parse_options field order (11 bits used)
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
int sr_script_replay(const uint8_t *script, size_t length, const char *context, sr_report_fn report, void *user);

#ifdef __cplusplus
}
#endif

#endif
