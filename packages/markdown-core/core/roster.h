#ifndef MARKDOWN_CORE_ROSTER_H
#define MARKDOWN_CORE_ROSTER_H

#include <stdbool.h>
#include <stdint.h>
#include "map.h"
#include "node.h"
#include "zipper.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE ROSTER: everything a parse declares that a later lookup asks for, as
 * one log in the order the parse made it, an index of each key's
 * declarations in the order of where they start, and, once an edit reads a
 * parse again, each kind's declarations in that order about the edit.
 *
 * A record states a kind, a label in that kind's normal form, and the source
 * offset where the declaring definition starts. The kinds are lookup spaces:
 *
 *   REFERENCE  the label of a link reference definition, and the label a
 *              heading's text declares -- one space, which a reference
 *              occurrence resolves against;
 *   FOOTNOTE   the label of a footnote definition;
 *   SPECIMEN   the label of a specimen definition;
 *   ANCHOR     an explicit anchor, reserved where completion meets it, and
 *              withdrawn where completion releases the node that holds it.
 *
 * Reference and footnote labels are in the reference-label normal form
 * (markdown_core_roster_normal_label); a specimen label and an anchor are
 * their bytes as written.
 *
 * A record declares its key, or withdraws one earlier declaration of it, the
 * one that starts where the withdrawal does: that declaration is withdrawn,
 * and a key is declared while one of its declarations stands.
 *
 * THE LOG is what a rollback pops back to a mark and what the headings read
 * (heading.c, THE COMMITTED NAMES); a record lives in it, where its label is
 * the roster's own copy, until a rollback pops it or the roster is
 * compacted. The log holds no node.
 *
 * THE INDEX maps a kind and a label to the key's declarations that stand, in
 * a list through the records in the order of where they start, the first
 * the earliest. So a lookup answers whether the key is declared at all and
 * whether some declaration of it starts before a position, and a
 * declaration the list holds leaves it, which a withdrawal, a rollback or an
 * edit that replaced its text does, without looking at any other key. A
 * declaration only appends to the log: a lookup of a kind has the index
 * take that kind's records beyond it, in log order, room for all of them
 * made at once, and each joins its key's list from the list's end, where a
 * declaration in log order mostly goes. So a kind that no lookup asks for
 * and nothing withdraws is never indexed, and a record rolled back before a
 * lookup of its kind never is. A withdrawal has the index take its kind's
 * records up to it as it is appended.
 *
 * THE PLACES. A parse an edit reads again holds each kind's declarations in
 * a zipper (zipper.h) about the edit: the declarations before where the
 * reading starts in the front, and the rest in the back, which the reading
 * does not see, until it takes the old text back from a place (THE SPLICE,
 * blocks.c), the back's declarations before that place going with the text
 * they were in. A record holds where it starts as its entry does: in the
 * text, or, in the back, measured back from the end. A parse no edit read
 * has no places, and its records stand in the text. */
typedef enum {
    MARKDOWN_CORE_ROSTER_REFERENCE = 1,
    MARKDOWN_CORE_ROSTER_FOOTNOTE,
    MARKDOWN_CORE_ROSTER_SPECIMEN,
    MARKDOWN_CORE_ROSTER_ANCHOR
} markdown_core_roster_kind;

typedef struct markdown_core_roster_record {
    /* A declaration the index holds: the key's declarations before and
     * after it in its list, the list's first holding its last in `before`;
     * and, in the first, `hint`, the declaration the list last took or one
     * beside the last it let go, where a search of the list begins
     * (S_link). A withdrawal: the declaration it withdrew, in `before`. */
    struct markdown_core_roster_record *before, *after, *hint;
    /* Where the declaring or withdrawing definition starts: in the text, or
     * measured back from the end when the record is in the back. */
    uint32_t start;
    /* A withdrawal: how many of the key's declarations stand after it. */
    uint32_t live;
    /* The key: the kind's byte, then the label, then NUL. */
    bufsize_t key_len;
    /* Whether the record withdraws a declaration of its key; whether, a
     * declaration, the index holds it in its key's list; whether a
     * withdrawal withdrew it; whether it is in the back; and whether the
     * text it was in is gone, which an edit replaced. */
    bool withdraws, linked, withdrawn, back, gone;
    unsigned char key[];
} markdown_core_roster_record;

/* Where a declaration of a parse an edit reads starts, and the record. */
typedef struct {
    uint32_t start;
    markdown_core_roster_record *record;
} markdown_core_roster_place;

typedef MARKDOWN_CORE_ZIPPER(markdown_core_roster_place) markdown_core_roster_places;

/* Storage records are carved from, newest first (roster.c). */
typedef struct markdown_core_roster_block markdown_core_roster_block;

/* One kind's records. */
typedef struct {
    /* How many records of the log are of the kind: a kind none declares
     * answers every lookup without its key being assembled. */
    size_t records;
    /* How many of them the index has taken: every one before `cursor` in
     * the log, and none after it. */
    size_t indexed, cursor;
    /* The kind's declarations about an edit, once one reads the parse. */
    markdown_core_roster_places places;
} markdown_core_roster_space;

typedef struct {
    markdown_core_roster_record **records;
    size_t count, capacity;
    markdown_core_roster_space spaces[MARKDOWN_CORE_ROSTER_ANCHOR + 1];
    /* Each key's first declaration in its list, as the record. The index
     * borrows the key bytes from that record, and holds in `extra` how
     * many declarations the list holds. */
    markdown_core_key_index index;
    markdown_core_roster_block *blocks;
    size_t block_used, block_size;
    /* Where a lookup assembles its key. */
    markdown_core_strbuf key;
    /* Whether the kinds hold their declarations in places, which an edit
     * makes; and whether the back is hidden from lookups, from the reopen
     * that makes it to the splice that takes it back. */
    bool placed, hidden;
    /* How many bytes the log's records take, and how many of them are of
     * records gone, which an edit replaced or a withdrawal withdrew. */
    size_t bytes, dead;
    /* The keys of the declarations the last splice let go, each with its
     * NUL. */
    markdown_core_strbuf lost;
} markdown_core_roster;

/* A point the log can roll back to. */
typedef struct {
    size_t count;
    markdown_core_roster_block *blocks;
    size_t block_used, block_size, bytes, dead;
} markdown_core_roster_mark;

void markdown_core_roster_init(markdown_core_roster *roster);
void markdown_core_roster_free(markdown_core_roster *roster);

/* Append a record of `kind` for `label`, already in the kind's normal form,
 * declared by a definition starting at `start`. An empty label declares
 * nothing. False when storage could not be had. */
bool markdown_core_roster_declare(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                  const unsigned char *label, bufsize_t length, uint32_t start);

/* Append a record of `kind` that withdraws the declaration of `label` that
 * starts at `start`, which stands. False when storage could not be had. */
bool markdown_core_roster_withdraw(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                   const unsigned char *label, bufsize_t length, uint32_t start);

/* The earliest declaration of `kind` for `label`, in the kind's normal
 * form, that stands and a lookup sees, or NULL when none does. `*failed` is
 * set when the key could not be assembled or the index could not take the
 * log, and the answer is then NULL. */
const markdown_core_roster_record *markdown_core_roster_find(markdown_core_roster *roster,
                                                             markdown_core_roster_kind kind, const unsigned char *label,
                                                             bufsize_t length, bool *failed);

/* The earliest declaration of `kind` for `label`, in the kind's normal
 * form, that stands, one of a back that lookups do not see as well: the
 * key's next is its `after`, in order of where they start, the front's
 * first. `*failed` as above. */
markdown_core_roster_record *markdown_core_roster_first(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                                        const unsigned char *label, bufsize_t length, bool *failed);

/* The declaration `record` of the index's list that starts where `record`
 * does, of its key, in the terms of the back when `back` or of the front:
 * the one of `kind` for `label`, as markdown_core_roster_first takes it,
 * that the index holds and starts at `start` there, or NULL, as it is when
 * the kinds hold no places. */
const markdown_core_roster_record *markdown_core_roster_held_at(const markdown_core_roster *roster,
                                                                markdown_core_roster_kind kind,
                                                                const unsigned char *label, bufsize_t length, bool back,
                                                                uint32_t start);
/* The first declaration of the back in the list `first` begins, or NULL. */
const markdown_core_roster_record *markdown_core_roster_back_first(markdown_core_roster_record *first);

/* The earliest declaration of `kind` for the reference-label normal form of
 * `label`, as written, as above; a label that normalizes to nothing, or is
 * longer than a link label may be, is declared by nothing. `*failed` as
 * above. */
const markdown_core_roster_record *markdown_core_roster_find_label(markdown_core_roster *roster,
                                                                   markdown_core_roster_kind kind,
                                                                   const markdown_core_chunk *label, bool *failed);

/* `label`'s reference-label normal form, in storage from `pool` the node
 * that holds it owns (markdown_core_node_pool_bytes), NUL-terminated; empty
 * when it normalizes to nothing. False when storage could not be had. */
bool markdown_core_roster_normal_label(markdown_core_roster *roster, markdown_core_node_pool *pool,
                                       const markdown_core_chunk *label, markdown_core_chunk *normal);

/* The record's label, borrowed from the roster. */
static inline markdown_core_chunk markdown_core_roster_label(const markdown_core_roster_record *record) {
    markdown_core_chunk label = {(unsigned char *)record->key + 1, record->key_len - 1, 0};
    return label;
}

/* Where the record starts in the text. */
static inline uint32_t markdown_core_roster_start(const markdown_core_roster *roster,
                                                  const markdown_core_roster_record *record) {
    return record->back ? (uint32_t)roster->spaces[record->key[0]].places.end - record->start : record->start;
}

/* Whether the declaration `record`, the earliest of its key a lookup found,
 * starts before `position`. */
static inline bool markdown_core_roster_starts_before(const markdown_core_roster *roster,
                                                      const markdown_core_roster_record *record, uint32_t position) {
    return markdown_core_roster_start(roster, record) < position;
}

/* A reading of one kind's records in log order: where the next one is
 * looked for, and how many are left. */
typedef struct {
    markdown_core_roster_kind kind;
    size_t at, left;
} markdown_core_roster_cursor;

/* A reading of `kind`'s records from the start of the log. */
static inline markdown_core_roster_cursor markdown_core_roster_records(const markdown_core_roster *roster,
                                                                       markdown_core_roster_kind kind) {
    markdown_core_roster_cursor cursor = {kind, 0, roster->spaces[kind].records};
    return cursor;
}

/* The cursor's next record, or NULL when none of its kind is left. */
static inline const markdown_core_roster_record *markdown_core_roster_next(const markdown_core_roster *roster,
                                                                           markdown_core_roster_cursor *cursor) {
    if (!cursor->left) {
        return NULL;
    }
    while (roster->records[cursor->at]->key[0] != (unsigned char)cursor->kind) {
        cursor->at++;
    }
    cursor->left--;
    return roster->records[cursor->at++];
}

/* The cursor's next record before position `end` of the log, or NULL, with
 * the cursor at `end`, when none of its kind is left before it. */
static inline const markdown_core_roster_record *
markdown_core_roster_next_before(const markdown_core_roster *roster, markdown_core_roster_cursor *cursor, size_t end) {
    while (cursor->left && cursor->at < end) {
        const markdown_core_roster_record *record = roster->records[cursor->at++];
        if (record->key[0] == (unsigned char)cursor->kind) {
            cursor->left--;
            return record;
        }
    }
    if (cursor->at < end) {
        cursor->at = end;
    }
    return NULL;
}

/* Whether the declaration stands: no withdrawal withdrew it and no edit
 * replaced its text. */
static inline bool markdown_core_roster_stands(const markdown_core_roster_record *record) {
    return !record->withdraws && !record->withdrawn && !record->gone;
}

/* Whether the declaration stands and a lookup sees it: one in the back is
 * hidden from the reopen to the splice. */
static inline bool markdown_core_roster_seen(const markdown_core_roster *roster,
                                             const markdown_core_roster_record *record) {
    return markdown_core_roster_stands(record) && !(record->back && roster->hidden);
}

/* The point the log is at now. */
markdown_core_roster_mark markdown_core_roster_mark_now(const markdown_core_roster *roster);
/* Pop every record appended after `mark`, last first, each undone, and
 * release the storage they were carved from. */
void markdown_core_roster_rollback(markdown_core_roster *roster, markdown_core_roster_mark mark);

/* AN EDIT READS FROM `start` (blocks.c, THE REOPEN) of a text of `size`
 * bytes: each kind's declarations that start there or past it go to the
 * back, which lookups do not see until the splice; the first edit places
 * them, and one whose log holds more dead bytes than live compacts it
 * first. False, with the roster as it was, when storage could not be
 * had. */
bool markdown_core_roster_reopen(markdown_core_roster *roster, uint32_t start, size_t size);
/* THE SPLICE (blocks.c): the back's declarations that start before `place`
 * of the text before the edit go with the text the edit replaced, their
 * keys kept for markdown_core_roster_lost, the end moves by `delta`, and
 * lookups see the back again. A `place` past every declaration lets the
 * whole back go, as a reading that took nothing back does. */
void markdown_core_roster_splice(markdown_core_roster *roster, size_t place, int64_t delta);
/* The keys of the declarations of the front, but anchors, that start from
 * `start` on, each once and in byte order, each with its NUL, into
 * `labels`. False when storage could not be had. */
bool markdown_core_roster_made(const markdown_core_roster *roster, uint32_t start, markdown_core_strbuf *labels);
/* The keys of the declarations, but anchors, the last splice let go, as
 * markdown_core_roster_made writes them. False when storage could not be
 * had. */
bool markdown_core_roster_lost(const markdown_core_roster *roster, markdown_core_strbuf *labels);
/* The labels of the anchors an edit's reading may have made or let go,
 * each with its NUL, some more than once: the front's from `start` on, the
 * ones the last splice let go, and, while the back is hidden -- a reading
 * that took nothing back, at its finish -- all of the back's. False when
 * storage could not be had. */
bool markdown_core_roster_anchors(const markdown_core_roster *roster, uint32_t start, markdown_core_strbuf *labels);
/* Whether a declaration of `kind` for `label` that stands starts before
 * `from` or at `to` or past it. `*failed` as for markdown_core_roster_find,
 * and the answer is then false. */
bool markdown_core_roster_declared_outside(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                           const unsigned char *label, bufsize_t length, uint32_t from, uint32_t to,
                                           bool *failed);
/* The log holds only the declarations that stand, in storage of their
 * size, and each kind's places shrink to what they hold. A parse that keeps
 * snapshots, whose headings read the log by position (heading.c), never
 * compacts. False, with the roster as it was, when storage could not be
 * had. */
bool markdown_core_roster_compact(markdown_core_roster *roster);

#ifdef __cplusplus
}
#endif

#endif
