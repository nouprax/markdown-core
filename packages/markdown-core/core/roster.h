#ifndef MARKDOWN_CORE_ROSTER_H
#define MARKDOWN_CORE_ROSTER_H

#include <stdbool.h>
#include <stdint.h>
#include "map.h"
#include "array.h"
#include "pieces.h"
#include "node.h"
#include "tree.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE ROSTER: every label the parse's definitions declare, each once, with
 * its declarations; and each shelf's declarations in the order of where
 * they start, about an edit.
 *
 * A LABEL is a kind and a label in that kind's normal form, its key the
 * kind's byte, the label and NUL, held once, in the label. The kinds are
 * lookup spaces: the engine's own,
 *
 *   ANCHOR     an explicit anchor, reserved where completion meets it, as
 *              its bytes as written;
 *
 * and the spaces the dialect's elements declare (element.h,
 * markdown_core_space), numbered after it as the dialect is sealed: a link
 * reference definition's labels, say, which a heading's text declares too.
 * A space that `folds` holds its labels in the reference-label normal form
 * (markdown_core_roster_normal_label), any other its bytes as written.
 *
 * A DECLARATION is a node's (`node`, or none for an anchor) and starts in
 * the text where the node does. It belongs to its node's text: an edit
 * that replaces that text lets it go (THE SPLICE), and the reading that
 * reads the text again declares it anew. A label lists its declarations
 * that stand in the order of where they start, the earliest first, which a
 * lookup answers with: so a lookup answers whether the label is declared at
 * all and by which declaration first, and a declaration leaves its label's
 * list without a look at any other label. The index maps a key to its
 * label, from the declaration that first names it on.
 *
 * THE PLACES. Each shelf's declarations are in a tree (tree.h), in order
 * of where they start, placed as they are declared. A shelf is a kind and
 * the kind of node that declares in it (`node`), or, for a declaration of
 * no node, the kind alone: so a kind that nodes of two kinds declare, a
 * link reference definition's and a heading's, holds each kind's
 * declarations in a tree of their own, which a document lists (A
 * DOCUMENT'S TABLES, elements/ast.c). A declaration of a node may have no
 * label, which places it and declares nothing.
 * Each tree's focus is an edit's: the declarations before where
 * the reading starts in the front, and the rest in the back, which the
 * reading does not see, until it takes the old text back from a place (THE
 * SPLICE, blocks.c), the back's declarations before that place going with
 * the text they were in. Every shelf is cut at the same place, so two
 * declarations of shelves apart are in order as their parts and starts
 * are. A declaration's number says where its entry is (`where`), which
 * holds where it starts: in the text, or, in the back, in the text before
 * the edit until the splice.
 *
 * THE USES. A lookup is a use of the label it asks for: a record of the
 * inline root whose completion made it, at the byte of the text the lookup
 * reads (blocks.c, WHERE A USE IS), in a shelf of its own, the uses' (kind
 * none, node none), which the focus cuts and the splice takes back as it
 * does every shelf. A label lists its uses, declared or not, so a label
 * whose being declared an edit flipped names the roots whose lookups of it
 * may now answer otherwise (markdown_core_parser_altered_uses). The text a
 * use was in goes as a declaration's does, and the reading of an edit lets
 * go of those of the content it parses again (THE SEAMS, blocks.c).
 *
 * THE JOURNAL. A transaction (a parse, or an edit's readings, up to its
 * flush) lists each label whose declarations it changed, once, with
 * whether the label was declared as the transaction began. What a
 * transaction changed, and so what a lookup made before it may now answer
 * otherwise, is read from it: the names (names.c) and a document's tables
 * read the labels it touched, and an edit the uses of the labels whose
 * being declared it flipped. The next transaction's first change starts it
 * empty. */
#define MARKDOWN_CORE_ROSTER_UNPLACED UINT32_MAX

/* A kind: the engine's ANCHOR, or a number the dialect gave a space its
 * elements declare (dialect.h, `spaces`); a key's first byte. */
typedef unsigned markdown_core_roster_kind;
#define MARKDOWN_CORE_ROSTER_ANCHOR 1u

struct markdown_core_roster_record;

/* A LABEL (THE ROSTER). */
typedef struct markdown_core_roster_label {
    /* Its earliest declaration that stands, which holds the last in
     * `before`, or NULL while none does; and how many stand. */
    struct markdown_core_roster_record *first;
    uint32_t count;
    /* Its uses, the lookups that asked for it, in no order, linked through
     * their `after`, or NULL while none does (THE USES). */
    struct markdown_core_roster_record *uses;
    /* The transaction that last listed it in the journal (`epoch`), and
     * whether it was declared as that one began. */
    uint32_t listed;
    bool was;
    /* Its copy, in a compaction. */
    struct markdown_core_roster_label *copy;
    /* The key: the kind's byte, then the label, then NUL. */
    bufsize_t key_len;
    unsigned char key[];
} markdown_core_roster_label;

/* A RECORD: a declaration, or a use (THE USES). */
typedef struct markdown_core_roster_record {
    /* A declaration its label lists: the label's declarations before and
     * after it, the first holding the last in `before`; and, in the first,
     * `hint`, the declaration the list last took or one beside the last it
     * let go, where a search of the list begins (S_link). A use's are the
     * uses of its label before and after it. In a compaction, an old
     * record's `hint` is its copy. */
    struct markdown_core_roster_record *before, *after, *hint;
    /* The label it declares or uses, or NULL for a declaration of no
     * label. */
    markdown_core_roster_label *label;
    /* The node that declared it, or NULL; of a use, the inline root whose
     * completion looked it up. */
    const markdown_core_node *node;
    /* Its number in `where`, or MARKDOWN_CORE_ROSTER_UNPLACED. */
    uint32_t id;
    /* Whether its label lists it, and whether the text it was in is gone,
     * which an edit replaced. */
    bool linked, gone;
    /* The shelf that places it. */
    uint8_t shelf;
} markdown_core_roster_record;

/* Where a declaration of a parse an edit reads starts, and the record. */
typedef struct {
    uint32_t start;
    markdown_core_roster_record *record;
} markdown_core_roster_place;

/* Storage labels and records are carved from, newest first (roster.c). */
typedef struct markdown_core_roster_block markdown_core_roster_block;

/* One shelf's declarations about an edit (THE PLACES). */
typedef struct markdown_core_roster_shelf {
    markdown_core_tree places;
    /* The roster that holds the shelf, which a moved place's entry is
     * written in. */
    struct markdown_core_roster *roster;
    markdown_core_roster_kind kind;
    /* The kind of node that declares in it, or MARKDOWN_CORE_NODE_NONE. */
    uint16_t node;
} markdown_core_roster_shelf;

#define MARKDOWN_CORE_ROSTER_SHELVES 255

typedef struct markdown_core_roster {
    /* How many declarations of each kind the parse ever made, by kind;
     * `kinds` of them, the first no kind's: a kind none declares answers
     * every lookup without its key being assembled, and a reading that
     * made one is told by the count. */
    size_t *made;
    size_t kinds;
    /* The shelves, each in storage of its own, as they are first declared
     * in. */
    markdown_core_roster_shelf **shelves;
    size_t shelf_count;
    /* The uses' shelf's number, or -1 before the first use (THE USES). */
    int use_shelf;
    /* Each key's label, in `value`; the index borrows the key bytes from
     * it. */
    markdown_core_key_index index;
    markdown_core_roster_block *blocks;
    size_t block_used, block_size;
    /* Where each placed declaration of a number has its entry (tree.h, A
     * HANDLE); a number none holds has no leaf, and its slot is the next
     * free number plus one, as `free` is the first's. */
    MARKDOWN_CORE_ARRAY(markdown_core_tree_handle) where;
    uint32_t free;
    /* Where a lookup assembles its key. */
    markdown_core_strbuf key;
    /* Whether the back is hidden from lookups, from the reopen that makes
     * it to the splice that takes it back. */
    bool hidden;
    /* THE JOURNAL: the labels the transaction `epoch` changed, in the order
     * it first changed them; `closed` once it flushed, so that the next
     * change starts the next. */
    MARKDOWN_CORE_ARRAY(markdown_core_roster_label *) journal;
    uint32_t epoch;
    bool closed;
    /* How many bytes the labels and records take, and how many of them are
     * of those gone: declarations an edit replaced, and labels none
     * declares. */
    size_t bytes, dead;
} markdown_core_roster;

/* A roster of `kinds` kinds, the first no kind's (dialect.h,
 * `space_count`).
 * False when storage could not be had, and the parse that holds it fails;
 * it is freed as any other. */
bool markdown_core_roster_init(markdown_core_roster *roster, size_t kinds);
void markdown_core_roster_free(markdown_core_roster *roster);

/* `node`, or no node when it is NULL, declares `label` of `kind`, already in
 * the kind's normal form, and starts at `start`. An empty label declares
 * nothing: of a node, the declaration is placed all the same. False when
 * storage could not be had. */
bool markdown_core_roster_declare(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                  const unsigned char *label, bufsize_t length, uint32_t start,
                                  const markdown_core_node *node);

/* Where a use went: nowhere, as a lookup of a label that names nothing
 * makes none; after every use of the front; or among them. */
typedef enum {
    MARKDOWN_CORE_ROSTER_UNUSED,
    MARKDOWN_CORE_ROSTER_APPENDED,
    MARKDOWN_CORE_ROSTER_AMONG
} markdown_core_roster_placing;

/* THE USES: the node `node` looks up `label` of `kind`, as written, at
 * `start`; its use is placed, after every use that starts there or before,
 * and listed by its label. The answer is the label's earliest declaration
 * that stands and a lookup sees, or NULL; and `*placed` where the use
 * went. A label of a kind that `folds` is
 * looked up in its reference-label normal form, and one that normalizes
 * to nothing, or longer than a link label may be, is declared by nothing
 * and makes no use, as an empty label of any kind. `*failed` is set when
 * storage could not be had, and the answer is then NULL. */
const markdown_core_roster_record *markdown_core_roster_use(markdown_core_roster *roster,
                                                            markdown_core_roster_kind kind,
                                                            const markdown_core_chunk *label, bool folds,
                                                            uint32_t start, const markdown_core_node *node,
                                                            markdown_core_roster_placing *placed, bool *failed);

/* The first use that starts at or past `place`, as a rank of the uses'
 * shelf, in the terms of the part that holds it: every use of the front
 * starts before every one of the back. */
size_t markdown_core_roster_uses_from(const markdown_core_roster *roster, size_t place);

/* How many uses the front holds, and the back. */
size_t markdown_core_roster_uses_front(const markdown_core_roster *roster);
size_t markdown_core_roster_uses_back(const markdown_core_roster *roster);

/* The `count` uses from rank `at` go. */
void markdown_core_roster_unuse(markdown_core_roster *roster, size_t at, size_t count);

/* The earliest declaration of `kind` for `label`, in the kind's normal
 * form, that stands and a lookup sees, or NULL when none does. `*failed` is
 * set when the key could not be assembled, and the answer is then NULL. */
const markdown_core_roster_record *markdown_core_roster_find(markdown_core_roster *roster,
                                                             markdown_core_roster_kind kind, const unsigned char *label,
                                                             bufsize_t length, bool *failed);

/* The same of an ANCHOR, `name` kept in pieces and measuring `measure`. */
const markdown_core_roster_record *markdown_core_roster_find_name(markdown_core_roster *roster,
                                                                  const markdown_core_pieces *name,
                                                                  markdown_core_measure measure, bool *failed);

/* The earliest declaration of `kind` for `label`, in the kind's normal
 * form, that stands, or NULL, of a roster whose back is not hidden: the
 * label's next is its `after`. It reads and changes nothing else. */
const markdown_core_roster_record *markdown_core_roster_seek(const markdown_core_roster *roster,
                                                             markdown_core_roster_kind kind, const unsigned char *label,
                                                             bufsize_t length);

/* The shelf of `kind` that nodes of kind `node` declare in, or NULL when
 * none has. */
const markdown_core_roster_shelf *markdown_core_roster_shelf_of(const markdown_core_roster *roster,
                                                                markdown_core_roster_kind kind, uint16_t node);

/* The record at rank `index` of the shelf's places. */
const markdown_core_roster_record *markdown_core_roster_shelf_at(const markdown_core_roster_shelf *shelf, size_t index);

/* A roster moved to `roster` from where it was, which holds none. */
void markdown_core_roster_moved(markdown_core_roster *roster);

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

/* A label's text, borrowed from the roster. */
static inline markdown_core_chunk markdown_core_roster_text(const markdown_core_roster_label *label) {
    markdown_core_chunk text = {(unsigned char *)label->key + 1, label->key_len - 1, 0};
    return text;
}

/* The label a record declares, borrowed from the roster: empty for one of
 * no label. */
static inline markdown_core_chunk markdown_core_roster_label_of(const markdown_core_roster_record *record) {
    return record->label ? markdown_core_roster_text(record->label) : markdown_core_chunk_literal("");
}

/* Where a placed declaration starts, in the terms of the part that holds
 * it, and whether that is the back. */
uint32_t markdown_core_roster_placed(const markdown_core_roster *roster, const markdown_core_roster_record *record,
                                     bool *back);

/* Where the record starts in the text: the text before the edit, for one
 * of the back before the splice. */
static inline uint32_t markdown_core_roster_start(const markdown_core_roster *roster,
                                                  const markdown_core_roster_record *record) {
    bool back;
    return markdown_core_roster_placed(roster, record, &back);
}

/* Whether the declaration is in the back. */
static inline bool markdown_core_roster_back(const markdown_core_roster *roster,
                                             const markdown_core_roster_record *record) {
    bool back;
    (void)markdown_core_roster_placed(roster, record, &back);
    return back;
}

/* Whether the label is declared and a lookup sees it: its first
 * declaration, in the back, is hidden from the reopen to the splice. */
static inline bool markdown_core_roster_declared(const markdown_core_roster *roster,
                                                 const markdown_core_roster_label *label) {
    return label->first && !(roster->hidden && markdown_core_roster_back(roster, label->first));
}

/* THE JOURNAL: its labels, `*count` of them. */
static inline markdown_core_roster_label *const *markdown_core_roster_journal(const markdown_core_roster *roster,
                                                                              size_t *count) {
    *count = roster->journal.count;
    return roster->journal.values;
}

/* The transaction flushed: the next change starts the next. */
static inline void markdown_core_roster_close(markdown_core_roster *roster) { roster->closed = true; }

/* AN EDIT READS FROM `start` (blocks.c, THE REOPEN): each kind's
 * declarations that start there or past it go to the back, which lookups
 * do not see until the splice, and the uses that start at `uses` or past
 * it; a roster that holds more dead bytes than live compacts first. False,
 * with the roster as it was, when storage could not be had. */
bool markdown_core_roster_reopen(markdown_core_roster *roster, uint32_t start, uint32_t uses);

/* THE SPLICE (blocks.c): the back's records that start before `place` of
 * the text before the edit go with the text the edit replaced, but the
 * first `held` uses of the back, which the reading kept (THE OLD TAIL OF A
 * PARAGRAPH, blocks.c); the end moves by `delta`, and lookups see the back
 * again. A `place` past every record lets the whole back go, as a reading
 * that took nothing back does. False when the journal could not take a
 * label. */
bool markdown_core_roster_splice(markdown_core_roster *roster, size_t place, int64_t delta, size_t held);

/* The roster keeps only the declarations that stand and the labels they
 * declare, in storage of their size, and each shelf's places shrink to
 * what they hold; the journal starts empty. False, with the roster as it
 * was, when storage could not be had. */
bool markdown_core_roster_compact(markdown_core_roster *roster);

#ifdef __cplusplus
}
#endif

#endif
