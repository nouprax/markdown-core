/* The oracles of the incremental gates (docs/plans/2026-09-29-incremental-gates.md,
 * section 4), over one subject (incremental_harness.h): the incremental runner
 * checks the tracked correctness set with them, and the session fuzz target
 * checks the scripts it decodes.
 *
 *   3.1  a composite document's fresh parse has as many root children as its
 *        parts' fresh parses together
 *   4.1  the subject's text equals the harness's text model; the canonical
 *        dump of its flushed document equals the dump of a fresh parse of
 *        that text in the same unit, and so do its extents in walk order, its
 *        scope and hit-test answers and its definition tables; a snapshot
 *        equals the fresh parse in all of these but where plan 5.5 lets it
 *        differ (4.6's snapshot meaning)
 *   4.2  ids are unique; over the lineage an id keeps its kind and a retired
 *        id never returns; a fresh parse numbers its nodes from 1 in
 *        canonical walk order
 *   4.3  a node deep equal to the node of the previous document with its id
 *        is that node's object, and every other node is a new object
 *   4.4  each node has the id of the old node the matching rule says it
 *        continues, or an id the lineage has never seen
 *   4.5  every scripted identity expectation holds
 *   4.6  the snapshot the open and every append return equals the snapshot
 *        a new subject returns for one append of the text, ids aside, in
 *        all of the above
 *   4.7  after the script's `flush`, an append or a flush fails with
 *        the flushed status
 *   4.11 after every edit, 4.1 to 4.5 hold against the document before it,
 *        with each old node's anchor at its image under the edit
 *   4.12 the bytes an edit reads again hold the bytes it wrote, and are the
 *        script's bound where it states one
 *   4.13 an edit while the subject streams, or one that names no range of
 *        the text, fails with its status and changes nothing
 *
 * Scope and hit-test queries each walk the document, so 4.1 asks them for a
 * sample of the nodes spread evenly over the walk rather than for every node.
 */
#ifndef MARKDOWN_CORE_INCREMENTAL_ORACLES_H
#define MARKDOWN_CORE_INCREMENTAL_ORACLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <markdown_core.h>

#include "incremental_harness.h"

/* The ids a lineage has issued: the kind each first named, and whether it
 * is live or retired. */
typedef struct history {
    unsigned char *state; /* 0 never seen, 1 live, 2 retired */
    markdown_core_node_kind *kinds;
    size_t capacity;
} history;

typedef struct run {
    const ih_subject_class *subject;
    /* What failed, by oracle, and the first failure's description. */
    size_t failures;
    const char *first_oracle;
    char first[512];
    /* A self-test run stops once every oracle it must see fail has failed:
     * `required` names them, and `caught` holds the ones that have. */
    bool stop_at_failure;
    const char *required;
    char caught[64];
    /* The family this run checks, or NULL for every family, and how many of
     * its cases ran. */
    const char *family;
    size_t checked;
    /* With `shards` set, the run checks only the cases of its family whose
     * place among them, counted from 0 in manifest order, leaves `shard` when
     * divided by `shards`; `cases` counts the family's cases met so far. */
    size_t shard, shards, cases;
    const char *label;
} run;

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void fail(run *state, const char *oracle, const char *format, ...);
/* Whether every oracle the run must see fail has failed. */
bool failed_all(const run *state);
/* Whether a self-test run has seen every failure it needs. */
bool stopped(const run *state);
markdown_core_text_unit unit_of(ih_unit unit);

/* The run's subject, opened in `unit` on `opened`, given `steps`, and
 * flushed at the end unless a step did: 4.6 and 4.2 to 4.5 hold after the
 * open and after every append, 4.1 to 4.5 after the flush, 4.7 after
 * that, 4.11 and 4.12 after every edit, and 4.13 for every edit it must
 * refuse. 4.2 to 4.5 hold only for a subject that keeps ids. */
void run_steps(run *state, const char *where, ih_unit unit, const uint8_t *opened, size_t opened_length,
               const ih_step *steps, size_t count);
/* 3.1: parts that keep their meaning when joined. */
void check_parts(run *state, const char *where, const uint8_t *document, size_t length, const ih_case *entry);
/* 4.10: `size` bytes of `data` decoded as a document and the appends that
 * follow it (incremental_fuzz.c), checked in both units. */
void check_script_bytes(run *state, const uint8_t *data, size_t size);

#endif
