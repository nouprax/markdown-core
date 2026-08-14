#ifndef MARKDOWN_CORE_APPEND_REPLAY_H
#define MARKDOWN_CORE_APPEND_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include <markdown_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared append replay harness: drives a document through successive
 * appends next to a shadow copy of the text and verifies, on every append,
 * that the document dumps byte-identically to a one-shot parse of the
 * shadow bytes, and that identity behaves. The identity check is a double
 * walk: the predecessor and successor trees are walked against a cumulative
 * id ledger, which pins that ids never resurrect, revisions never regress,
 * and a node whose (id, revision) did not move has an unchanged own
 * projection and child list.
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

/* One id the series has ever shown, with the revision it carried when last
 * seen. Entries are never removed: a retired entry is what makes
 * resurrection detectable at any later append, not just the next one. */
typedef struct er_ledger_entry {
    markdown_core_node_id id;
    uint64_t revision; /* at the last sighting */
    uint32_t seen;     /* serial of the walk that last saw it */
    bool alive;        /* present in the current head's tree */
} er_ledger_entry;

typedef struct er_ledger {
    er_ledger_entry *entries; /* sorted by id */
    size_t count;
    size_t capacity;
    uint32_t walk; /* serial of the latest verification walk */
} er_ledger;

typedef struct er_replay {
    const char *context;
    markdown_core_document *document;
    er_text shadow;
    er_ledger ledger;
    const markdown_core_parse_options *options;
    er_report_fn report;
    void *user;
} er_replay;

/* Parses the empty document and seeds the ledger from its tree.
 * `options` must stay valid until er_replay_close. */
int er_replay_open(
    er_replay *replay,
    const char *context,
    const markdown_core_parse_options *options,
    er_report_fn report,
    void *user
);

void er_replay_close(er_replay *replay);

/* Appends bytes through the real append mutation — any split is legal,
 * mid-UTF-8 and mid-line included — appends the same bytes to the shadow
 * (always allocated, NUL-terminated one byte past `length`), double-walks
 * the predecessor and successor trees against the ledger, and compares the
 * document dump with a one-shot parse of the shadow text.  A zero `length`
 * is the empty append: still a mutation, verified the same way. */
int er_replay_append(er_replay *replay, const uint8_t *bytes, size_t length);

/* Deterministic append-script interpreter: replays `script` as a document
 * append sequence with full per-append verification, then appends the empty
 * chunk once more at the end.  Every byte is meaningful, so a
 * coverage-guided fuzzer can drive it directly.  The format is:
 *
 *   options: two little-endian bytes; bit i enables parse option i in
 *     markdown_core_parse_options field order
 *   then chunks until the script runs out:
 *     len8 bytes...
 *   len8 counts literal bytes taken from the script (clamped to what
 *   remains; zero is the empty append), so the chunk boundaries themselves
 *   are fuzzer-chosen: every mid-UTF-8, mid-CRLF, mid-line split is
 *   reachable by construction.
 *
 * Returns 0 when every append verified, -1 after reporting a failure. */
int er_script_replay(const uint8_t *script, size_t length, const char *context, er_report_fn report, void *user);

#ifdef __cplusplus
}
#endif

#endif
